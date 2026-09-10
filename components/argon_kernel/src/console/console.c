/*
 * ArgonOS - console subsystem.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/console.h>
#include <argon/input.h>

#include <stdio.h>
#include <string.h>

#include <argon/codepage.h>
#include <argon/display.h>
#include <argon/log.h>
#include <argon/session.h>
#include <argon/textpanel.h>

#include <argon/port/mem.h>
#include <argon/port/time.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>

/*
 * A lone ESC only becomes the Escape key once nothing follows it.  30 ms is
 * long enough that a real escape sequence has arrived even over a slow link,
 * and short enough that pressing Escape feels immediate.
 */
#define AG_CON_ESC_IDLE_MS 30

#define AG_CON_TICK_MS 10

/*
 * Deep enough for a pasted command line to arrive in one burst.  It is not the
 * real defence against overflow - that is the backpressure in pump_endpoint -
 * but it keeps the common case from ever touching it.
 */
#define AG_CON_EVENT_QUEUE 64
#define AG_CON_READ_CHUNK 64

/*
 * How many times a tick will go round reading before it goes back to rendering.
 * 16 chunks is a kilobyte a tick, comfortably above the line rate; the loop
 * stops early the moment the ports are empty, which is nearly always.
 */
#define AG_CON_DRAIN_ROUNDS 16

typedef struct {
    const ag_con_transport_t *transport;
    void                     *ctx;
    ag_vtout_t                out;
    ag_vtin_t                 in;
    uint32_t                  last_byte_ms;
    bool                      used;
    bool                      paused; /* XOFF has been sent to this endpoint */
} ag_con_endpoint_t;

/*
 * Flow control on the input side.
 *
 * The queue and the backpressure in pump_endpoint stop the console from losing
 * events, but they cannot stop a sender: bytes keep arriving, the port's own
 * buffer fills, and the driver drops what does not fit.  For someone typing that
 * is invisible.  For a file arriving through the recv command it is a file that
 * is quietly wrong, which is the worst of the possible outcomes - measured, at
 * 11 KB into a 12 KB transfer.
 *
 * XOFF and XON are what a serial line has for exactly this, they cost two bytes,
 * and every terminal understands them.  The port's buffer absorbs whatever was
 * already in flight when the sender is told to stop.
 */
#define AG_CON_XOFF 0x13
#define AG_CON_XON 0x11

/*
 * One screen per slot, and why the console owns them rather than the slots.
 *
 * A slot switch used to clear the screen, because there was one screen: the
 * only way to stop slot 2 reading like the tail of slot 1 was to wipe it.
 * So everything a program said was gone the moment you looked at something
 * else - and on this machine "something else" is usually the graphical shell,
 * which is where you go to run the next thing.
 *
 * Everything above reaches the screen through ag_console_screen(), all
 * thirty-odd call sites, so making that answer with the focused slot's screen
 * is the whole of the change.  They live here and not in the session module
 * for two reasons: a resize has to reshape every one of them, and the console
 * is what a screen is FOR - a slot is a stack of processes and does not
 * otherwise care what a cell is.
 *
 * Lazily: index 0 is the system's and exists from boot; a user slot gets its
 * own the first time it is looked at, so a machine that never leaves slot 1
 * pays for one.
 */
#define CON_SCREENS (AG_SESSION_SLOTS + 1)

static ag_screen_t        s_screens[CON_SCREENS];
static bool               s_screen_up[CON_SCREENS];
static int                s_active; /* index into s_screens */

#define s_screen (s_screens[s_active])
static ag_con_endpoint_t  s_endpoints[AG_CON_MAX_ENDPOINTS];
static ag_port_mutex_t         s_lock;
static ag_port_queue_t s_events;
static ag_port_task_t       s_task;
static bool               s_ready;
static ag_con_sink_fn     s_redirect;
static void              *s_redirect_ctx;
static ag_con_live_fn     s_live;
static void              *s_live_ctx;
static uint32_t           s_dropped_events;
static uint16_t           s_mods;
static bool (*s_hotkeys)(ag_event_t *ev);

/* HID usage page 0x07 keycodes fit in a byte; sticky TTL covers auto-repeat. */
#define AG_KEY_STICKY_MS 150u
static uint8_t  s_key_down[32];
static uint32_t s_key_stamp[256];

/* ---------------------------------------------------------------------- */

static uint32_t now_ms(void) { return (uint32_t)(ag_port_us() / 1000); }

void ag_console_lock(void)
{
    if (s_lock != NULL) {
        ag_port_mutex_take_recursive(s_lock, AG_PORT_FOREVER);
    }
}

void ag_console_unlock(void)
{
    if (s_lock != NULL) {
        ag_port_mutex_give_recursive(s_lock);
    }
}

void ag_console_set_hotkeys(bool (*fn)(ag_event_t *ev)) { s_hotkeys = fn; }

void *ag_console_lock_holder(void)
{
    if (s_lock == NULL) {
        return NULL;
    }
    return (void *)ag_port_mutex_holder(s_lock);
}

ag_screen_t *ag_console_screen(void) { return &s_screens[s_active]; }

/* AG_SESSION_SYSTEM is index 0; user slots 0..3 are 1..4. */
static int screen_index(int slot)
{
    if (slot == AG_SESSION_SYSTEM) {
        return 0;
    }
    if (slot < 0 || slot >= AG_SESSION_SLOTS) {
        return -1;
    }
    return slot + 1;
}

ag_err_t ag_console_adopt_slot(int slot)
{
    const int want = screen_index(slot);
    if (!s_ready || want < 0) {
        return -AG_EINVAL;
    }
    ag_console_lock();
    if (want != s_active) {
        /*
         * The screen that exists changes hands; nothing is allocated and
         * nothing is drawn.
         *
         * The console comes up long before there are slots and its screen
         * is the one the boot report is written on.  Whoever is focused
         * when the slots appear has been writing to that screen all along,
         * so it is theirs - and saying otherwise is not a bookkeeping
         * detail: going back to that slot would then make it a screen it
         * had never used, which is to say a blank one, with the boot report
         * filed under a slot nobody had been looking at.
         */
        s_screens[want] = s_screens[s_active];
        s_screen_up[want] = true;
        s_screen_up[s_active] = false;
        memset(&s_screens[s_active], 0, sizeof(s_screens[s_active]));
        s_active = want;
    }
    ag_console_unlock();
    return AG_OK;
}

ag_err_t ag_console_use_slot(int slot)
{
    const int want = screen_index(slot);
    if (!s_ready || want < 0) {
        return -AG_EINVAL;
    }

    ag_console_lock();
    if (want == s_active) {
        ag_console_unlock();
        return AG_OK;
    }

    if (!s_screen_up[want]) {
        /*
         * A slot that has never been looked at gets its screen now, at the
         * size the console is now.  Out of memory is not a failure worth
         * stopping a slot switch for: the slots share the one screen again,
         * which is what they all did until today.
         */
        const size_t need = ag_screen_memsize(s_screens[s_active].cols,
                                              s_screens[s_active].rows);
        void        *mem = ag_port_alloc(need, AG_MEM_FAST | AG_MEM_BYTE);
        if (mem == NULL) {
            ag_console_unlock();
            ag_log(AG_LOG_WARN, "console",
                   "no memory for slot %d's own screen; sharing", slot);
            return -AG_ENOMEM;
        }
        if (ag_screen_init(&s_screens[want], mem, need,
                           s_screens[s_active].cols,
                           s_screens[s_active].rows) != AG_OK) {
            ag_port_free(mem);
            ag_console_unlock();
            return -AG_ENOMEM;
        }
        s_screen_up[want] = true;
    }

    s_active = want;

    /*
     * Everything that draws the console holds a picture of what is on it and
     * sends only what changed, so a switch has to say that all of it did -
     * to the panel, to the framebuffer, and to every terminal on the far end
     * of a wire.
     */
    ag_screen_mark_all_dirty(&s_screens[s_active]);
    for (int i = 0; i < AG_CON_MAX_ENDPOINTS; i++) {
        ag_con_endpoint_t *ep = &s_endpoints[i];
        if (ep->used) {
            ag_vtout_mark_all(&ep->out);
        }
    }
    ag_display_console_dirty();
    ag_textpanel_owe_full();
    ag_console_unlock();
    return AG_OK;
}

bool ag_console_ready(void) { return s_ready; }

/* ---------------------------------------------------------------------- */
/* Output                                                                 */
/* ---------------------------------------------------------------------- */

static void sink_to_transport(void *ctx, const char *data, size_t len)
{
    ag_con_endpoint_t *ep = (ag_con_endpoint_t *)ctx;
    if (ep->transport->write != NULL) {
        ep->transport->write(ep->ctx, data, len);
    }
}

/* Caller holds the lock. */
static void render_all(void)
{
    for (int i = 0; i < AG_CON_MAX_ENDPOINTS; i++) {
        ag_con_endpoint_t *ep = &s_endpoints[i];
        if (!ep->used) {
            continue;
        }
        ag_vtout_take_dirty(&ep->out, &s_screen);
        ag_vtout_flush(&ep->out, &s_screen, sink_to_transport, ep);
    }
    /* Soft (or panel) local fb, while graphics mode has not taken it over. */
    ag_display_render_console(&s_screen);
    /* A panel with no framebuffer takes the same screen as characters. */
    ag_textpanel_render(&s_screen);
    ag_inputpoll_tick();
    ag_screen_clear_dirty(&s_screen);
}

void ag_console_redirect(ag_con_sink_fn sink, void *ctx)
{
    ag_console_lock();
    s_redirect = sink;
    s_redirect_ctx = ctx;
    ag_console_unlock();
}

void ag_console_set_live(ag_con_live_fn fn, void *ctx)
{
    ag_console_lock();
    s_live = fn;
    s_live_ctx = ctx;
    ag_console_unlock();
}

void ag_console_write(const char *buf, size_t len)
{
    if (!s_ready || buf == NULL) {
        return;
    }

    ag_console_lock();
    if (s_redirect != NULL) {
        s_redirect(s_redirect_ctx, buf, len);
    } else {
        ag_screen_write(&s_screen, buf, len);
    }
    ag_console_unlock();
}

void ag_console_write_log(const char *buf, size_t len)
{
    if (!s_ready || buf == NULL || len == 0) {
        return;
    }

    ag_console_lock();
    if (s_redirect != NULL) {
        s_redirect(s_redirect_ctx, buf, len);
    } else if (s_live != NULL) {
        /*
         * The prompt leaves the cursor mid-line.  Break away, print the
         * message, then hand the row back to whoever is editing.
         */
        if (s_screen.cur_x != 0) {
            ag_screen_puts(&s_screen, "\n");
        }
        ag_screen_write(&s_screen, buf, len);
        if (buf[len - 1] != '\n') {
            ag_screen_puts(&s_screen, "\n");
        }
        s_live(s_live_ctx);
    } else {
        ag_screen_write(&s_screen, buf, len);
    }
    ag_console_unlock();
}

void ag_console_puts(const char *s)
{
    if (s != NULL) {
        ag_console_write(s, strlen(s));
    }
}

int ag_console_vprintf(const char *fmt, va_list ap)
{
    char      buf[256];
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);

    if (n <= 0) {
        return n;
    }
    /* vsnprintf reports what it would have written; only what fit is real. */
    const size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
    ag_console_write(buf, len);
    return n;
}

int ag_console_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = ag_console_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

uint32_t ag_console_dropped_events(void) { return s_dropped_events; }

void ag_console_sync(void)
{
    if (!s_ready) {
        return;
    }
    ag_console_lock();
    render_all();
    ag_console_unlock();
}

void ag_console_restore_tty(void)
{
    if (!s_ready) {
        return;
    }
    ag_console_lock();
    ag_screen_set_attr(&s_screen, AG_ATTR_DEFAULT);
    ag_screen_set_cursor(&s_screen, true);
    for (int i = 0; i < AG_CON_MAX_ENDPOINTS; i++) {
        if (s_endpoints[i].used) {
            /* Next flush emits ?25h even if we already thought it was shown. */
            s_endpoints[i].out.cursor_visible = false;
        }
    }
    ag_console_unlock();
}

/* ---------------------------------------------------------------------- */
/* Input                                                                  */
/* ---------------------------------------------------------------------- */

/* When something last arrived from a person.  Zero at boot means "at boot",
 * which is the right answer: the machine has been idle since it started. */
static uint32_t s_last_input_ms;

static void publish(const ag_event_t *ev)
{
    ag_event_t stamped = *ev;
    stamped.ts = (ag_time_t)ag_port_us();

    /*
     * Somebody is there.  Recorded before the hotkey filter below, because
     * Ctrl+C and Ctrl+\ are somebody being there as much as any other key -
     * and this is the one clock the idle timer in src/core/powerctl.c has.
     *
     * Every kind of input arrives through here, including what a touchscreen
     * driver and a pad injects, which is why it is one line rather than one per
     * source.
     */
    s_last_input_ms = now_ms();

    /*
     * The supervisor gets first look.  It must be quick - this runs on the
     * console task, and a console that is busy is a console that stops reading
     * input - so it notes what to do and does it on its own task.
     */
    if (s_hotkeys != NULL && s_hotkeys(&stamped)) {
        return;
    }

    if (stamped.type == AG_EV_KEY_DOWN) {
        s_mods = stamped.key.mods;
        if (stamped.key.keycode < 256u) {
            const uint16_t k = stamped.key.keycode;
            s_key_down[k >> 3] |= (uint8_t)(1u << (k & 7u));
            s_key_stamp[k] = now_ms();
        }
    } else if (stamped.type == AG_EV_KEY_UP) {
        if (stamped.key.keycode < 256u) {
            const uint16_t k = stamped.key.keycode;
            s_key_down[k >> 3] &= (uint8_t)~(1u << (k & 7u));
        }
    }

    /*
     * Dropping the newest, not the oldest.  Losing the head of a burst
     * silently rewrites what the user typed - "delete foo" arriving as
     * "lete foo" is a different command, and could have been a worse one.
     * Losing the tail leaves a visibly truncated line that can be fixed.
     *
     * This should not happen: pump_endpoint only reads what the queue can
     * hold.  The counter exists so that if it ever does, it is visible.
     */
    if (!ag_port_queue_send(s_events, &stamped, 0)) {
        s_dropped_events++;
    }
}

/* Tells the sender to stop while the queue is filling, and to go on once it has
 * drained.  Two thresholds rather than one, so a queue hovering at the limit does
 * not turn into a stream of XOFF and XON. */
static void update_flow(ag_con_endpoint_t *ep, uint32_t space)
{
    if (ep->transport->write == NULL) {
        return;
    }

    if (!ep->paused && space <= AG_CON_EVENT_QUEUE / 2) {
        const char stop = AG_CON_XOFF;
        ep->transport->write(ep->ctx, &stop, 1);
        ep->paused = true;
    } else if (ep->paused && space >= (AG_CON_EVENT_QUEUE * 3) / 4) {
        const char go = AG_CON_XON;
        ep->transport->write(ep->ctx, &go, 1);
        ep->paused = false;
    }
}

/*
 * Release an endpoint: call its transport `close` (to shut a socket) and free
 * the slot.  Under the console lock so it does not race attach or the render.
 * The close hook is called after the slot is marked free, so a transport that
 * frees its own ctx in close cannot have the console touch it again.
 */
static void detach_endpoint(ag_con_endpoint_t *ep)
{
    ag_console_lock();
    if (!ep->used) {
        ag_console_unlock();
        return;
    }
    const ag_con_transport_t *t = ep->transport;
    void                     *ctx = ep->ctx;
    memset(ep, 0, sizeof(*ep)); /* clears `used` */
    ag_console_unlock();

    if (t->close != NULL) {
        t->close(ctx);
    }
}

/* Returns how many bytes were taken from the port, so the caller knows whether
 * there is more to do before it goes back to sleep. */
static int32_t pump_endpoint(ag_con_endpoint_t *ep)
{
    uint8_t chunk[AG_CON_READ_CHUNK];

    if (ep->transport->read == NULL) {
        return 0;
    }

    /*
     * Backpressure: never read more bytes than the event queue can accept,
     * since one byte can produce one event.  What is not read stays in the
     * transport's own buffer, which is where it is safe - and the sender is told
     * to stop before that buffer is the only thing holding the line.
     */
    const uint32_t space = ag_port_queue_space(s_events);
    update_flow(ep, space);
    if (space == 0) {
        return 0;
    }
    size_t want = sizeof(chunk);
    if ((size_t)space < want) {
        want = (size_t)space;
    }

    const int32_t n = ep->transport->read(ep->ctx, chunk, want);
    if (n < 0) {
        /* The endpoint has gone (a telnet client hung up).  Detach it here, on
         * the console task, so no other task races the endpoint array. */
        detach_endpoint(ep);
        return 0;
    }
    for (int32_t i = 0; i < n; i++) {
        ag_event_t ev;
        if (ag_vtin_feed(&ep->in, chunk[i], &ev)) {
            /* The decoder counts in cells because the terminal does; above
             * here a pointer is measured in pixels (ABI 0.42). */
            ag_input_to_pixels(&ev);
            publish(&ev);
        }
    }

    if (n > 0) {
        ep->last_byte_ms = now_ms();
    } else if (ag_vtin_busy(&ep->in) &&
               (now_ms() - ep->last_byte_ms) >= AG_CON_ESC_IDLE_MS) {
        ag_event_t ev;
        if (ag_vtin_idle(&ep->in, &ev)) {
            publish(&ev);
        }
    }
    return n;
}

void ag_console_flush_input(void)
{
    if (!s_ready) {
        return;
    }
    ag_event_t drop;
    while (ag_port_queue_recv(s_events, &drop, 0)) {
    }
    memset(s_key_down, 0, sizeof(s_key_down));
}

uint16_t ag_console_mods(void) { return s_mods; }

bool ag_console_inject_event(const ag_event_t *ev)
{
    if (!s_ready || ev == NULL || s_events == NULL) {
        return false;
    }
    if (ev->type == AG_EV_NONE) {
        return false;
    }
    publish(ev);
    return true;
}

uint32_t ag_console_idle_ms(void)
{
    return (uint32_t)(now_ms() - s_last_input_ms);
}

bool ag_console_read_event(ag_event_t *ev, uint32_t timeout_ms)
{
    if (!s_ready || ev == NULL) {
        return false;
    }
    const ag_port_ticks_t ticks = (timeout_ms == UINT32_MAX)
                                 ? AG_PORT_FOREVER
                                 : ag_port_ms_to_ticks(timeout_ms);
    return ag_port_queue_recv(s_events, ev, ticks);
}

bool ag_console_key_pressed(uint16_t keycode)
{
    if (!s_ready || keycode >= 256u) {
        return false;
    }
    /* Drain so a game poll loop does not fill the queue; sticky already set
     * in publish() for each KEY_DOWN. */
    ag_event_t drop;
    while (ag_console_read_event(&drop, 0)) {
    }
    const uint32_t now = now_ms();
    if ((s_key_down[keycode >> 3] & (uint8_t)(1u << (keycode & 7u))) == 0) {
        return false;
    }
    if ((now - s_key_stamp[keycode]) > AG_KEY_STICKY_MS) {
        s_key_down[keycode >> 3] &= (uint8_t)~(1u << (keycode & 7u));
        return false;
    }
    return true;
}

bool ag_console_peek_event(ag_event_t *ev)
{
    if (!s_ready) {
        return false;
    }

    /* A caller that only wants the answer still needs somewhere for the copy. */
    ag_event_t scratch;
    return ag_port_queue_peek(s_events, (ev != NULL) ? ev : &scratch, 0);
}

int32_t ag_console_getch(uint32_t timeout_ms)
{
    const uint32_t deadline = now_ms() + timeout_ms;

    for (;;) {
        ag_event_t ev;
        const uint32_t remaining =
            (timeout_ms == UINT32_MAX)
                ? UINT32_MAX
                : ((now_ms() < deadline) ? (deadline - now_ms()) : 0);

        if (!ag_console_read_event(&ev, remaining)) {
            return -1;
        }
        /*
         * Being asked to stop is an answer to "give me a key": a read that went
         * on waiting would be exactly the hang the request is trying to end.
         */
        if (ev.type == AG_EV_QUIT) {
            return -AG_EKILLED;
        }
        /*
         * A byte in the active code page, not a code point: this is what an
         * application puts on the screen or into a file, and both are bytes.
         * A character the page cannot represent is not delivered at all - it
         * would have to become either a wrong byte or two bytes behaving as two
         * characters, and both are worse than nothing arriving.
         */
        if (ev.type == AG_EV_KEY_DOWN && ev.key.unicode != 0) {
            const int32_t byte =
                ag_cp_from_unicode(ag_cp_active(), ev.key.unicode);
            if (byte >= 0) {
                return byte;
            }
        }
        if (timeout_ms != UINT32_MAX && now_ms() >= deadline) {
            return -1;
        }
    }
}

int32_t ag_console_readline(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }

    size_t n = 0;
    buf[0] = '\0';

    for (;;) {
        ag_event_t ev;
        if (!ag_console_read_event(&ev, UINT32_MAX)) {
            continue;
        }
        if (ev.type == AG_EV_QUIT) {
            return -AG_EKILLED;
        }
        if (ev.type != AG_EV_KEY_DOWN) {
            continue;
        }

        if (ev.key.mods & AG_MOD_CTRL) {
            if (ev.key.keycode == AG_KEY_C) {
                return -AG_EKILLED;
            }
            continue;
        }

        if (ev.key.keycode == AG_KEY_ENTER) {
            break;
        }
        if (ev.key.keycode == AG_KEY_BACKSPACE) {
            if (n > 0) {
                n--;
                ag_console_puts("\b \b");
            }
            continue;
        }

        /* Plain ASCII only; a confirmation prompt has no use for more. */
        if (ev.key.unicode >= 0x20 && ev.key.unicode < 0x7f && n + 1 < len) {
            buf[n++] = (char)ev.key.unicode;
            ag_console_write(&buf[n - 1], 1);
        }
    }

    buf[n] = '\0';
    return (int32_t)n;
}

/* ---------------------------------------------------------------------- */

static void console_task(void *arg)
{
    (void)arg;

    for (;;) {
        /*
         * Read until the ports are empty rather than one chunk a tick.  One chunk
         * a tick is 64 bytes every 10 ms - 6.4 KB a second, which is *below* the
         * line rate at 115200 baud: a burst then overran the port's buffer and
         * lost bytes, which showed up as a file arriving through recv with a hole
         * in it.  Bounded so that a stuck endpoint cannot keep this task from
         * rendering, and it stops as soon as there is nothing left, so an idle
         * console costs exactly what it did before.
         */
        for (int round = 0; round < AG_CON_DRAIN_ROUNDS; round++) {
            int32_t taken = 0;
            for (int i = 0; i < AG_CON_MAX_ENDPOINTS; i++) {
                if (s_endpoints[i].used) {
                    taken += pump_endpoint(&s_endpoints[i]);
                }
            }
            if (taken <= 0) {
                break;
            }
        }

        /*
         * Unconditional: a flush with nothing pending emits nothing, and the
         * cursor can move without any cell changing.
         */
        ag_console_lock();
        render_all();
        ag_console_unlock();

        ag_port_task_delay(ag_port_ms_to_ticks(AG_CON_TICK_MS));
    }
}

ag_err_t ag_console_init(uint16_t cols, uint16_t rows)
{
    if (s_ready) {
        return AG_OK;
    }

    const size_t need = ag_screen_memsize(cols, rows);
    void *mem = ag_port_alloc(need, AG_MEM_FAST | AG_MEM_BYTE);
    if (mem == NULL) {
        return -AG_ENOMEM;
    }

    s_active = 0;
    const ag_err_t err = ag_screen_init(&s_screens[0], mem, need, cols, rows);
    s_screen_up[0] = (err == AG_OK);
    if (err != AG_OK) {
        ag_port_free(mem);
        return err;
    }

    s_lock = ag_port_mutex_new_recursive();
    s_events = ag_port_queue_new(AG_CON_EVENT_QUEUE, sizeof(ag_event_t));
    if (s_lock == NULL || s_events == NULL) {
        ag_port_free(mem);
        return -AG_ENOMEM;
    }

    s_ready = true;

    /*
     * The console task lives on the system core: the application core is
     * meant to stay free for the application.
     */
    if (!ag_port_task_create(console_task, "ag_con", 3072, NULL, 10, 0, 0,
                             &s_task)) {
        s_ready = false;
        ag_port_free(mem);
        return -AG_ENOMEM;
    }

    return AG_OK;
}

/*
 * A new grid, with the old picture carried over.
 *
 * The size the console starts at is a build option because the console exists
 * three stages before anything has read a file off the flash.  This is how it
 * changes afterwards, once SYSTEM.CFG can be read and the panel has said how
 * much glass it has - so a board that gets a different display plugged into it
 * is a line in a file, not a rebuild.
 *
 * The overlap is copied rather than cleared, because the boot report is on
 * that screen and it is the one screenful most worth keeping.  When the new
 * grid is shorter, the rows copied are the ones ending at the cursor: that is
 * what a terminal does when its window is dragged smaller, and it keeps the
 * newest output rather than the oldest.
 */
ag_err_t ag_console_resize(uint16_t cols, uint16_t rows)
{
    if (!s_ready) {
        return -AG_ENODEV;
    }
    if (cols == 0 || rows == 0 || cols > AG_SCREEN_MAX_COLS ||
        rows > AG_SCREEN_MAX_ROWS) {
        return -AG_EINVAL;
    }
    if (cols == s_screen.cols && rows == s_screen.rows) {
        return AG_OK;
    }

    ag_console_lock();

    /*
     * Every screen that exists, not only the one being looked at: a slot
     * left at eighty columns while the console moved to forty would come
     * back the next time it was focused and hand the panel a picture of the
     * wrong shape.  The one on view keeps its content (that is what
     * recreate is for); the others are of no interest to anybody yet, so
     * they are simply remade at the new size.
     */
    void *old = NULL;
    for (int i = 0; i < CON_SCREENS; i++) {
        if (!s_screen_up[i] && i != s_active) {
            continue;
        }
        const size_t need = ag_screen_memsize(cols, rows);
        void *mem = ag_port_alloc(need, AG_MEM_FAST | AG_MEM_BYTE);
        if (mem == NULL) {
            ag_console_unlock();
            return -AG_ENOMEM;
        }
        ag_screen_t    next;
        const ag_err_t err = ag_screen_recreate(&next, mem, need, cols, rows,
                                                &s_screens[i]);
        if (err != AG_OK) {
            ag_console_unlock();
            ag_port_free(mem);
            return err;
        }
        void *was = s_screens[i].dirty;
        s_screens[i] = next;
        s_screen_up[i] = true;
        if (i == s_active) {
            old = was;
        } else {
            ag_port_free(was);
        }
    }

    /*
     * Every endpoint is told the whole terminal again, not just the rows: a
     * VT100 showing eighty columns has to be cleared, or the right-hand half
     * of the old picture stays there for ever with nothing ever writing to it.
     */
    for (int i = 0; i < AG_CON_MAX_ENDPOINTS; i++) {
        ag_con_endpoint_t *ep = &s_endpoints[i];
        if (!ep->used) {
            continue;
        }
        ag_vtout_init(&ep->out);
        ag_vtout_hello(&ep->out, sink_to_transport, ep);
        ag_vtout_mark_all(&ep->out);
    }

    ag_console_unlock();
    ag_port_free(old);
    return AG_OK;
}

ag_err_t ag_console_attach(const ag_con_transport_t *transport, void *ctx)
{
    if (transport == NULL) {
        return -AG_EINVAL;
    }

    ag_console_lock();
    for (int i = 0; i < AG_CON_MAX_ENDPOINTS; i++) {
        ag_con_endpoint_t *ep = &s_endpoints[i];
        if (ep->used) {
            continue;
        }

        memset(ep, 0, sizeof(*ep));
        ep->transport = transport;
        ep->ctx = ctx;
        ag_vtin_init(&ep->in);
        ag_vtout_init(&ep->out);
        ag_vtout_hello(&ep->out, sink_to_transport, ep);
        ep->last_byte_ms = now_ms();
        ep->used = true;

        ag_console_unlock();
        return AG_OK;
    }
    ag_console_unlock();

    return -AG_ENFILE;
}

void ag_console_detach(void *ctx)
{
    ag_con_endpoint_t *found = NULL;

    ag_console_lock();
    for (int i = 0; i < AG_CON_MAX_ENDPOINTS; i++) {
        if (s_endpoints[i].used && s_endpoints[i].ctx == ctx) {
            found = &s_endpoints[i];
            break;
        }
    }
    ag_console_unlock();

    /* detach_endpoint re-locks and re-checks `used`, so a concurrent detach of
     * the same endpoint is harmless. */
    if (found != NULL) {
        detach_endpoint(found);
    }
}
