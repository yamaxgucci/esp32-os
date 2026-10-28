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
#include <stdlib.h>
#include <string.h>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_partition.h"
#include "esp_timer.h"
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

/*
 * EVERY READ OF FLASH IS COUNTED, BECAUSE NOBODY SHOULD BE DOING IT.
 *
 * Maxim's rule, and it holds for reading as much as for writing: write a file
 * rarely, then work from memory.  The reason is measured - a read from SPI
 * flash turns the cache off for the whole chip, so a core fetching its code
 * through that cache stops for as long as the read lasts, and the amplifier
 * lost two blocks in five while the phone's page was being served.
 *
 * A quiet machine prints nothing here.  One that does not says how many reads
 * and how many bytes, once a second, so the next one can be found instead of
 * guessed at.
 */
static uint32_t volatile s_reads;
static uint32_t volatile s_read_bytes;
static int64_t           s_said_us;

static const esp_partition_t *s_last_part;

static void counted(size_t size, const esp_partition_t *part)
{
    s_reads++;
    s_read_bytes += (uint32_t)size;
    s_last_part = part;
    const int64_t now = esp_timer_get_time();
    if (s_said_us == 0 || now - s_said_us >= 1000000) {
        const uint32_t n = s_reads;
        const uint32_t b = s_read_bytes;
        s_said_us = now;
        s_reads = 0;
        s_read_bytes = 0;
        ESP_LOGI("flash", "%u reads, %u bytes in the last second (%s)", n, b,
                 (s_last_part != NULL && s_last_part->label[0] != 0)
                     ? s_last_part->label
                     : "?");
    }
}

/*
 * Declared before the mirror because the mirror uses them: the guard says
 * whether sound is playing, and the sector map is filled in beside the
 * mirror it belongs to.
 */
static volatile int s_guard;  /* > 0 while a converter is clocking frames */
static uint8_t     *s_dirty;  /* one bit a sector, for writes held back   */
static uint32_t     s_dirty_sectors;
static uint32_t     s_sector;
static bool         s_hold;   /* off until [flash] hold says otherwise */
static uint32_t     s_held_now;

/*
 * THE MIRROR.  See the port's flash.h for what it is for.
 */
static uint8_t              *s_mirror;
static const esp_partition_t *s_mirror_part;
static size_t                s_mirror_size;
static uint32_t volatile     s_mirror_reads;
static uint32_t volatile     s_mirror_bytes;

bool ag_port_flash_mirror(const char *label)
{
    if (s_mirror != NULL) {
        return true;
    }
    if (label == NULL) {
        return false;
    }
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (part == NULL) {
        return false;
    }
    /*
     * External memory only, and never the internal kind: a megabyte of
     * internal SRAM does not exist on this machine, and a mirror that took
     * what the audio curves need would cure one stall by causing another.
     */
    uint8_t *buf = (uint8_t *)heap_caps_malloc(part->size, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGW("flash", "no room to mirror %s (%u bytes); reads stay on the "
                 "flash", label, (unsigned)part->size);
        return false;
    }
    /*
     * Filled in chunks, because this runs at boot where the cache going off
     * costs nothing and a single read of a megabyte would hold it off for the
     * whole of it.
     */
    size_t off = 0;
    while (off < part->size) {
        const size_t n = ((part->size - off) > 65536u) ? 65536u
                                                       : (part->size - off);
        if (__real_esp_partition_read(part, off, buf + off, n) != ESP_OK) {
            heap_caps_free(buf);
            ESP_LOGW("flash", "could not read %s to mirror it", label);
            return false;
        }
        off += n;
    }
    s_mirror_size = part->size;
    s_mirror_part = part;
    s_mirror = buf;
    /* One bit a sector, for writes held while sound plays. 1 MB of 4 KB
       sectors is 32 bytes, so this is never the allocation that fails. */
    s_sector = part->erase_size;
    if (s_sector > 0u) {
        s_dirty_sectors = (uint32_t)(part->size / s_sector);
        s_dirty = (uint8_t *)calloc((s_dirty_sectors + 7u) / 8u, 1u);
    }
    ESP_LOGI("flash", "%s mirrored in external RAM, %u KB: reads no longer "
             "stop the cores", label, (unsigned)(part->size / 1024u));
    return true;
}

bool ag_port_flash_mirrored(void) { return s_mirror != NULL; }


void ag_port_flash_hold(bool on) { s_hold = on; }

uint32_t ag_port_flash_dirty(void) { return s_held_now; }

static void dirty_mark(size_t off, size_t size)
{
    if (s_dirty == NULL || s_sector == 0u) {
        return;
    }
    const uint32_t first = (uint32_t)(off / s_sector);
    const uint32_t last = (uint32_t)((off + size - 1u) / s_sector);
    for (uint32_t i = first; i <= last && i < s_dirty_sectors; i++) {
        const uint32_t byte = i >> 3;
        const uint8_t  bit = (uint8_t)(1u << (i & 7u));
        if ((s_dirty[byte] & bit) == 0u) {
            s_dirty[byte] |= bit;
            s_held_now++;
        }
    }
}

/*
 * Write every held sector out of the mirror.  The mirror is the truth here:
 * it has had every write applied to it in order, so a sector erased and
 * rewritten from it is exactly what the chip would have held.
 */
uint32_t ag_port_flash_flush(void)
{
    if (s_mirror == NULL || s_dirty == NULL || s_held_now == 0u) {
        return 0;
    }
    uint32_t done = 0;
    for (uint32_t i = 0; i < s_dirty_sectors; i++) {
        const uint32_t byte = i >> 3;
        const uint8_t  bit = (uint8_t)(1u << (i & 7u));
        if ((s_dirty[byte] & bit) == 0u) {
            continue;
        }
        const size_t off = (size_t)i * s_sector;
        if (__real_esp_partition_erase_range(s_mirror_part, off, s_sector) ==
                ESP_OK &&
            __real_esp_partition_write(s_mirror_part, off, s_mirror + off,
                                       s_sector) == ESP_OK) {
            s_dirty[byte] &= (uint8_t)~bit;
            if (s_held_now > 0u) {
                s_held_now--;
            }
            done++;
        }
    }
    if (done > 0u) {
        ESP_LOGI("flash", "%u held sectors written out", (unsigned)done);
    }
    return done;
}

/*
 * True when the write must not reach the chip now.  The mirror has it
 * already; the sector is remembered and goes out when the sound stops.
 */
static bool held(size_t off, size_t size, const esp_partition_t *part)
{
    if (!s_hold || s_mirror == NULL || part != s_mirror_part) {
        return false;
    }
    if (s_guard <= 0) {
        return false;
    }
    dirty_mark(off, size);
    return true;
}

uint32_t ag_port_flash_mirror_reads(uint32_t *bytes)
{
    if (bytes != NULL) {
        *bytes = s_mirror_bytes;
    }
    return s_mirror_reads;
}

/* True when the read was answered from RAM and the flash was not touched. */
static bool mirror_read(const esp_partition_t *part, size_t off, void *dst,
                        size_t size)
{
    if (s_mirror == NULL || part != s_mirror_part) {
        return false;
    }
    if (off > s_mirror_size || size > s_mirror_size - off) {
        return false; /* out of the mirror: let the real one answer and fail */
    }
    memcpy(dst, s_mirror + off, size);
    s_mirror_reads++;
    s_mirror_bytes += (uint32_t)size;
    return true;
}

/* A write goes to both, so that they cannot drift apart. */
static void mirror_wrote(const esp_partition_t *part, size_t off,
                         const void *src, size_t size)
{
    if (s_mirror == NULL || part != s_mirror_part) {
        return;
    }
    if (off > s_mirror_size || size > s_mirror_size - off) {
        return;
    }
    memcpy(s_mirror + off, src, size);
}

static void mirror_erased(const esp_partition_t *part, size_t off, size_t size)
{
    if (s_mirror == NULL || part != s_mirror_part) {
        return;
    }
    if (off > s_mirror_size || size > s_mirror_size - off) {
        return;
    }
    memset(s_mirror + off, 0xFF, size);
}

/*
 * THE GUARD.  See the port's flash.h for why this exists and what it costs.
 */
static volatile bool     s_strict;
static volatile uint32_t s_violations;
static char              s_violator[20];
static int64_t           s_violation_said_us;

void ag_port_flash_guard(int delta)
{
    const int n = s_guard + delta;
    s_guard = (n > 0) ? n : 0;
}

bool ag_port_flash_guarded(void) { return s_guard > 0; }

void ag_port_flash_strict(bool on) { s_strict = on; }

bool ag_port_flash_is_strict(void) { return s_strict; }

uint32_t ag_port_flash_violations(const char **who)
{
    if (who != NULL) {
        *who = (s_violator[0] != 0) ? s_violator : "?";
    }
    return s_violations;
}

/*
 * Returns true when the caller must be refused.  It names the task rather
 * than the partition: a partition says what was touched, a task says who to
 * go and fix, and only one of those can be acted on.
 */
static bool guard_hit(const char *what, size_t size)
{
    if (s_guard <= 0) {
        return false;
    }
    s_violations++;
    {
        const char *name = pcTaskGetName(NULL);
        if (name != NULL) {
            size_t i = 0;
            for (; i + 1u < sizeof(s_violator) && name[i] != 0; i++) {
                s_violator[i] = name[i];
            }
            s_violator[i] = 0;
        }
    }
    /* Once a second at the most.  A caller in a loop would otherwise deny the
       journal to everything else, which is the fault this is here to stop. */
    {
        const int64_t now = esp_timer_get_time();
        if (s_violation_said_us == 0 || now - s_violation_said_us >= 1000000) {
            s_violation_said_us = now;
            ESP_LOGW("flash", "%s of %u bytes from '%s' while sound plays%s",
                     what, (unsigned)size,
                     (s_violator[0] != 0) ? s_violator : "?",
                     s_strict ? " - refused" : "");
        }
    }
    return s_strict;
}

esp_err_t __wrap_esp_partition_read(const esp_partition_t *partition,
                                    size_t src_offset, void *dst, size_t size)
{
    if (mirror_read(partition, src_offset, dst, size)) {
        return ESP_OK;
    }
    if (guard_hit("read", size)) {
        return ESP_ERR_INVALID_STATE;
    }
    counted(size, partition);
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
    if (guard_hit("write", size)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (held(dst_offset, size, partition)) {
        mirror_wrote(partition, dst_offset, src, size);
        return ESP_OK;
    }
    const esp_err_t err =
        stack_is_internal()
            ? __real_esp_partition_write(partition, dst_offset, src, size)
            : handed_over(&(req_t){.op = OP_WRITE, .part = partition,
                                   .off = dst_offset, .src = src,
                                   .size = size});
    if (err == ESP_OK) {
        mirror_wrote(partition, dst_offset, src, size);
    }
    return err;
}

esp_err_t __wrap_esp_partition_read_raw(const esp_partition_t *partition,
                                        size_t src_offset, void *dst,
                                        size_t size)
{
    if (mirror_read(partition, src_offset, dst, size)) {
        return ESP_OK;
    }
    if (guard_hit("read", size)) {
        return ESP_ERR_INVALID_STATE;
    }
    counted(size, partition);
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
    if (guard_hit("write", size)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (held(dst_offset, size, partition)) {
        mirror_wrote(partition, dst_offset, src, size);
        return ESP_OK;
    }
    const esp_err_t err =
        stack_is_internal()
            ? __real_esp_partition_write_raw(partition, dst_offset, src, size)
            : handed_over(&(req_t){.op = OP_WRITE_RAW, .part = partition,
                                   .off = dst_offset, .src = src,
                                   .size = size});
    if (err == ESP_OK) {
        mirror_wrote(partition, dst_offset, src, size);
    }
    return err;
}

esp_err_t __wrap_esp_partition_erase_range(const esp_partition_t *partition,
                                           size_t offset, size_t size)
{
    if (guard_hit("erase", size)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (held(offset, size, partition)) {
        mirror_erased(partition, offset, size);
        return ESP_OK;
    }
    const esp_err_t err =
        stack_is_internal()
            ? __real_esp_partition_erase_range(partition, offset, size)
            : handed_over(&(req_t){.op = OP_ERASE, .part = partition,
                                   .off = offset, .size = size});
    if (err == ESP_OK) {
        mirror_erased(partition, offset, size);
    }
    return err;
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
