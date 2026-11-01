/*
 * ArgonOS port: ESP-IDF - who holds the heap, and who held its holes
 * (CONFIG_ARGON_HEAP_WHO).
 *
 * The heap walk shows where the free pieces are; it cannot show how they came
 * to be there.  A hole is made in time: something takes a block, something
 * else takes the one after it and keeps it, the first is given back.  So, in
 * a diagnostic build, ESP-IDF's allocation hooks keep two records:
 *
 *   - every block still held: its size, the task and the calls that took it,
 *     and its number in the order of allocation - its age;
 *   - the last blocks given back: the same, plus when they went - who lived
 *     in each hole, and for how long.
 *
 * A full journal of every event was tried first and boot alone wrote six
 * thousand, most of them the radio's 8-byte timers; this keeps what the
 * question needs and does not grow.  `mem who` prints both, for
 * tools/heapwho.py to lay over the map from `mem /i`.
 *
 * The records live in IRAM: on the ESP32 that memory serves only code and
 * word-sized data, so taking it leaves the byte heap - what is being studied
 * - as an ordinary build has it.  Every access to it is a whole word.
 *
 * The hooks run inside every malloc and free, in a task or an interrupt, with
 * the cache possibly off: no lock but a spinlock, no allocation, nothing in
 * flash.
 */
#include "sdkconfig.h"

#if CONFIG_ARGON_HEAP_WHO

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "argon/port/mem.h"

#define LIVE_N  560
#define GONE_N  192
#define MARK_N  32

static volatile ag_port_heap_rec_t *s_live;   /* ptr 0: a free slot */
static volatile ag_port_heap_rec_t *s_gone;   /* a ring, newest at s_gone_at */
static volatile uint32_t s_gone_at;
static volatile uint32_t s_seq;               /* allocations so far */
static volatile uint32_t s_hint;
static volatile uint32_t s_lost;              /* held, but no slot to say so */
static volatile uint32_t s_mark_seq[MARK_N];
static volatile uint32_t s_mark_name[MARK_N];
static volatile uint32_t s_marks;
static portMUX_TYPE      s_lock = portMUX_INITIALIZER_UNLOCKED;

/* As early as there is a heap: what boot takes is most of what stays. */
__attribute__((constructor)) static void who_start(void)
{
    const size_t caps = MALLOC_CAP_EXEC | MALLOC_CAP_32BIT;
    volatile ag_port_heap_rec_t *g = (volatile ag_port_heap_rec_t *)
        heap_caps_malloc(GONE_N * sizeof(ag_port_heap_rec_t), caps);
    volatile ag_port_heap_rec_t *l = (volatile ag_port_heap_rec_t *)
        heap_caps_malloc(LIVE_N * sizeof(ag_port_heap_rec_t), caps);
    if (g == NULL || l == NULL) {
        return;
    }
    for (unsigned i = 0; i < LIVE_N; i++) {
        l[i].ptr = 0;
    }
    for (unsigned i = 0; i < GONE_N; i++) {
        g[i].ptr = 0;
    }
    s_gone = g;
    s_live = l;
}

/* Four letters of the task's name in one word; "ISR " in an interrupt. */
static uint32_t IRAM_ATTR who_task(void)
{
    if (xPortInIsrContext()) {
        return 0x20525349u;   /* "ISR " */
    }
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        return 0x746f6f62u;   /* "boot" */
    }
    const char *n = pcTaskGetName(NULL);
    uint32_t w = 0;
    for (int i = 0; i < 4 && n != NULL && n[i] != '\0'; i++) {
        w |= (uint32_t)(uint8_t)n[i] << (8 * i);
    }
    return w;
}

static void IRAM_ATTR rec_copy(volatile ag_port_heap_rec_t *d,
                               const volatile ag_port_heap_rec_t *s)
{
    d->ptr = s->ptr;
    d->size = s->size;
    d->seq = s->seq;
    d->gone = s->gone;
    d->task = s->task;
    for (int k = 0; k < AG_PORT_HEAP_PCS; k++) {
        d->pc[k] = s->pc[k];
    }
}

void IRAM_ATTR esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps)
{
    (void)caps;
    volatile ag_port_heap_rec_t *live = s_live;
    if (live == NULL || ptr == NULL) {
        return;
    }
    uint32_t pc[AG_PORT_HEAP_PCS] = {0};
    esp_backtrace_frame_t f;
    esp_backtrace_get_start(&f.pc, &f.sp, &f.next_pc);
    /* the first two frames are always the heap's own (the *_base, then
     * heap_caps_malloc_base); the owner is after them */
    for (int skip = 0; skip < 2 && f.next_pc != 0; skip++) {
        if (!esp_backtrace_get_next_frame(&f)) {
            f.next_pc = 0;
        }
    }
    for (int i = 0; i < AG_PORT_HEAP_PCS && f.next_pc != 0; i++) {
        if (!esp_backtrace_get_next_frame(&f)) {
            break;
        }
        pc[i] = esp_cpu_process_stack_pc(f.pc);
    }
    const uint32_t task = who_task();

    portENTER_CRITICAL_SAFE(&s_lock);
    const uint32_t seq = ++s_seq;
    uint32_t i = s_hint;
    for (uint32_t n = 0; n < LIVE_N; n++, i = (i + 1u == LIVE_N) ? 0u : i + 1u) {
        if (live[i].ptr == 0) {
            live[i].ptr = (uint32_t)ptr;
            live[i].size = (uint32_t)size;
            live[i].seq = seq;
            live[i].gone = 0;
            live[i].task = task;
            for (int k = 0; k < AG_PORT_HEAP_PCS; k++) {
                live[i].pc[k] = pc[k];
            }
            s_hint = (i + 1u == LIVE_N) ? 0u : i + 1u;
            portEXIT_CRITICAL_SAFE(&s_lock);
            return;
        }
    }
    s_lost++;
    portEXIT_CRITICAL_SAFE(&s_lock);
}

void IRAM_ATTR esp_heap_trace_free_hook(void *ptr)
{
    volatile ag_port_heap_rec_t *live = s_live;
    if (live == NULL || ptr == NULL) {
        return;
    }
    portENTER_CRITICAL_SAFE(&s_lock);
    for (uint32_t i = 0; i < LIVE_N; i++) {
        if (live[i].ptr == (uint32_t)ptr) {
            /* the hole's last tenant, unless it lived and died in a moment:
             * a block given back before anything else was taken changed
             * nothing, and those are thousands of the radio's timers */
            if (live[i].seq != s_seq) {
                volatile ag_port_heap_rec_t *g = &s_gone[s_gone_at];
                rec_copy(g, &live[i]);
                g->gone = s_seq;
                s_gone_at = (s_gone_at + 1u == GONE_N) ? 0u : s_gone_at + 1u;
            }
            live[i].ptr = 0;
            s_hint = i;
            break;
        }
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
}

void ag_port_heap_who_mark(const char *text)
{
    uint32_t w = 0;
    for (int i = 0; i < 4 && text != NULL && text[i] != '\0'; i++) {
        w |= (uint32_t)(uint8_t)text[i] << (8 * i);
    }
    portENTER_CRITICAL(&s_lock);
    if (s_marks < MARK_N) {
        s_mark_seq[s_marks] = s_seq;
        s_mark_name[s_marks] = w;
        s_marks++;
    }
    portEXIT_CRITICAL(&s_lock);
}

/*
 * One record at a time, by index: `which` 0 the held blocks, 1 the given
 * back (oldest first), 2 the marks (in `seq` and `task`).  Returns false past
 * the end; a held slot that is empty comes back with ptr 0.
 */
bool ag_port_heap_who_get(int which, uint32_t i, ag_port_heap_rec_t *out)
{
    if (s_live == NULL) {
        return false;
    }
    bool ok = true;
    portENTER_CRITICAL(&s_lock);
    if (which == 0 && i < LIVE_N) {
        rec_copy((volatile ag_port_heap_rec_t *)out, &s_live[i]);
    } else if (which == 1 && i < GONE_N) {
        rec_copy((volatile ag_port_heap_rec_t *)out,
                 &s_gone[(s_gone_at + i) % GONE_N]);
    } else if (which == 2 && i < s_marks) {
        out->ptr = 0;
        out->seq = s_mark_seq[i];
        out->task = s_mark_name[i];
    } else {
        ok = false;
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

uint32_t ag_port_heap_who_seq(uint32_t *lost)
{
    *lost = s_lost;
    return s_seq;
}

#endif /* CONFIG_ARGON_HEAP_WHO */
