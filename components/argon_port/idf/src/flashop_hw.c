/*
 * ArgonOS port: ESP-IDF - flash for a task whose stack is in PSRAM.
 *
 * A TASK WITH ITS STACK IN PSRAM MAY NOT TOUCH FLASH, and IDF says so the hard
 * way: every read, write and erase starts by switching the flash cache off,
 * and spi_flash_disable_interrupts_caches_and_other_cpu() opens with
 * assert(esp_task_stack_is_sane_cache_disabled()).  With the cache off, a
 * stack in PSRAM is a stack that cannot be reached.
 *
 * The kernel gives a task a PSRAM stack when internal SRAM is short
 * (ag_proc_task_create): applications, the link's reader, the remote run and
 * device workers.  Every one of them can reach flash - the loader putting an
 * image's code there for XIP, a program opening a file on C:, the reader
 * serving the neighbour's file - and the first time one did, the Zero stopped
 * dead: TONE started with 12 KB left in one piece, its stack went to PSRAM,
 * the loader wrote its code to flash, and the assertion fired inside a kernel
 * the fault handler then tried to "recover" (26 Sep 2026).
 *
 * So the flash calls every IDF component goes through - littlefs, FAT's wear
 * levelling, NVS, the loader's own - are wrapped (-Wl,--wrap, see
 * CMakeLists.txt).  A caller whose stack is internal calls straight through.
 * One whose stack is not hands the call to a task here whose stack is, and
 * waits for it: the same work, done where the cache can go off.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

esp_err_t __real_esp_partition_read(const esp_partition_t *partition,
                                    size_t src_offset, void *dst, size_t size);
esp_err_t __real_esp_partition_write(const esp_partition_t *partition,
                                     size_t dst_offset, const void *src,
                                     size_t size);
esp_err_t __real_esp_partition_read_raw(const esp_partition_t *partition,
                                        size_t src_offset, void *dst,
                                        size_t size);
esp_err_t __real_esp_partition_write_raw(const esp_partition_t *partition,
                                         size_t dst_offset, const void *src,
                                         size_t size);
esp_err_t __real_esp_partition_erase_range(const esp_partition_t *partition,
                                           size_t offset, size_t size);
esp_err_t __real_esp_partition_mmap(const esp_partition_t *partition,
                                    size_t offset, size_t size,
                                    esp_partition_mmap_memory_t memory,
                                    const void **out_ptr,
                                    esp_partition_mmap_handle_t *out_handle);

typedef enum {
    OP_READ,
    OP_WRITE,
    OP_READ_RAW,
    OP_WRITE_RAW,
    OP_ERASE,
    OP_MMAP,
} op_t;

typedef struct {
    op_t                          op;
    const esp_partition_t        *part;
    size_t                        off;
    void                         *dst;
    const void                   *src;
    size_t                        size;
    esp_partition_mmap_memory_t   memory;
    const void                  **out_ptr;
    esp_partition_mmap_handle_t  *out_handle;
    esp_err_t                     result;
} req_t;

static StaticSemaphore_t s_lock_mem;
static SemaphoreHandle_t s_lock; /* one request at a time */
static SemaphoreHandle_t s_go;
static SemaphoreHandle_t s_done;
static TaskHandle_t      s_task;
static req_t             s_req;
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool              s_failed;

/* The test IDF itself makes before it lets the cache go off. */
static bool stack_is_internal(void)
{
    if (xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return true; /* nothing to hand it to; IDF's own rules apply */
    }
    return esp_ptr_in_dram((const void *)esp_cpu_get_sp());
}

static esp_err_t run(req_t *r)
{
    switch (r->op) {
    case OP_READ:
        return __real_esp_partition_read(r->part, r->off, r->dst, r->size);
    case OP_WRITE:
        return __real_esp_partition_write(r->part, r->off, r->src, r->size);
    case OP_READ_RAW:
        return __real_esp_partition_read_raw(r->part, r->off, r->dst, r->size);
    case OP_WRITE_RAW:
        return __real_esp_partition_write_raw(r->part, r->off, r->src, r->size);
    case OP_ERASE:
        return __real_esp_partition_erase_range(r->part, r->off, r->size);
    case OP_MMAP:
        return __real_esp_partition_mmap(r->part, r->off, r->size, r->memory,
                                         r->out_ptr, r->out_handle);
    }
    return ESP_ERR_INVALID_ARG;
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        (void)xSemaphoreTake(s_go, portMAX_DELAY);
        s_req.result = run(&s_req);
        (void)xSemaphoreGive(s_done);
    }
}

/* The helper, made the first time a PSRAM stack needs it.  Its own stack is
 * internal because that is where FreeRTOS puts a stack unless told not to. */
static bool helper(void)
{
    if (s_task != NULL) {
        return true;
    }
    taskENTER_CRITICAL(&s_mux);
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_mem);
    }
    taskEXIT_CRITICAL(&s_mux);
    (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_task == NULL && !s_failed) {
        s_go = xSemaphoreCreateBinary();
        s_done = xSemaphoreCreateBinary();
        if (s_go == NULL || s_done == NULL ||
            xTaskCreatePinnedToCore(worker, "flashop", 3072, NULL,
                                    configMAX_PRIORITIES - 3, &s_task,
                                    tskNO_AFFINITY) != pdPASS) {
            s_failed = true;
            s_task = NULL;
        }
    }
    (void)xSemaphoreGive(s_lock);
    return s_task != NULL;
}

static esp_err_t handed_over(req_t *r)
{
    if (!helper()) {
        /* no internal room even for the helper: IDF will say so itself */
        return run(r);
    }
    (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    s_req = *r;
    (void)xSemaphoreGive(s_go);
    (void)xSemaphoreTake(s_done, portMAX_DELAY);
    const esp_err_t result = s_req.result;
    (void)xSemaphoreGive(s_lock);
    return result;
}

esp_err_t __wrap_esp_partition_read(const esp_partition_t *partition,
                                    size_t src_offset, void *dst, size_t size)
{
    if (stack_is_internal()) {
        return __real_esp_partition_read(partition, src_offset, dst, size);
    }
    req_t r = {.op = OP_READ, .part = partition, .off = src_offset, .dst = dst,
               .size = size};
    return handed_over(&r);
}

esp_err_t __wrap_esp_partition_write(const esp_partition_t *partition,
                                     size_t dst_offset, const void *src,
                                     size_t size)
{
    if (stack_is_internal()) {
        return __real_esp_partition_write(partition, dst_offset, src, size);
    }
    req_t r = {.op = OP_WRITE, .part = partition, .off = dst_offset, .src = src,
               .size = size};
    return handed_over(&r);
}

esp_err_t __wrap_esp_partition_read_raw(const esp_partition_t *partition,
                                        size_t src_offset, void *dst,
                                        size_t size)
{
    if (stack_is_internal()) {
        return __real_esp_partition_read_raw(partition, src_offset, dst, size);
    }
    req_t r = {.op = OP_READ_RAW, .part = partition, .off = src_offset,
               .dst = dst, .size = size};
    return handed_over(&r);
}

esp_err_t __wrap_esp_partition_write_raw(const esp_partition_t *partition,
                                         size_t dst_offset, const void *src,
                                         size_t size)
{
    if (stack_is_internal()) {
        return __real_esp_partition_write_raw(partition, dst_offset, src, size);
    }
    req_t r = {.op = OP_WRITE_RAW, .part = partition, .off = dst_offset,
               .src = src, .size = size};
    return handed_over(&r);
}

esp_err_t __wrap_esp_partition_erase_range(const esp_partition_t *partition,
                                           size_t offset, size_t size)
{
    if (stack_is_internal()) {
        return __real_esp_partition_erase_range(partition, offset, size);
    }
    req_t r = {.op = OP_ERASE, .part = partition, .off = offset, .size = size};
    return handed_over(&r);
}

esp_err_t __wrap_esp_partition_mmap(const esp_partition_t *partition,
                                    size_t offset, size_t size,
                                    esp_partition_mmap_memory_t memory,
                                    const void **out_ptr,
                                    esp_partition_mmap_handle_t *out_handle)
{
    if (stack_is_internal()) {
        return __real_esp_partition_mmap(partition, offset, size, memory,
                                         out_ptr, out_handle);
    }
    req_t r = {.op = OP_MMAP, .part = partition, .off = offset, .size = size,
               .memory = memory, .out_ptr = out_ptr, .out_handle = out_handle};
    return handed_over(&r);
}
