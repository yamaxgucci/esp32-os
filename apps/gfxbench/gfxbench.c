/*
 * GFXBENCH — native ag_gfx_* vs LVGL, same player-like scene.
 *
 *   run h:\gfxbench.axe [full|dirty|idle] [frames]
 *   run h:\lvglbench.axe ...
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gfxbench.h"

#include <argon/keys.h>
#include <string.h>

#ifdef GFXBENCH_LVGL
AG_APP_SIZED("LVGLBENCH", "0.1", "argon", AG_AXE_NEEDS_GFX, 24 * 1024,
             512 * 1024);
#else
AG_APP_SIZED("GFXBENCH", "0.1", "argon", AG_AXE_NEEDS_GFX, 16 * 1024,
             128 * 1024);
#endif

static const char *const k_tracks[GFXBENCH_PL_N] = {
    "01. Neon Skyline", "02. Night Drive", "03. Glass Harbor",
    "04. Quiet Station", "05. Redline",    "06. After Hours",
    "07. Low Battery",  "08. Soft Reset",  "09. Copper Wire",
    "10. Last Train",   "11. Static",      "12. Demo Loop",
};

static ag_time_t s_flush_t0;

void gfxbench_flush_begin(void) { s_flush_t0 = ag_micros(); }

uint32_t gfxbench_flush_end(void)
{
    return (uint32_t)(ag_micros() - s_flush_t0);
}

const char *gfxbench_track_name(int i)
{
    if (i < 0 || i >= GFXBENCH_PL_N) {
        return "";
    }
    return k_tracks[i];
}

void gfxbench_layout_init(gfxbench_layout_t *L, uint16_t w, uint16_t h)
{
    int m = (w >= 400) ? 8 : 4;
    int gap = (w >= 400) ? 8 : 4;
    int main_w, main_h, pl_w, i, bw, bh, sx, sy, sw, sh, eh;

    memset(L, 0, sizeof(*L));
    L->screen.x = 0;
    L->screen.y = 0;
    L->screen.w = w;
    L->screen.h = h;

    pl_w = (w * 38) / 100;
    if (pl_w < 100) {
        pl_w = 100;
    }
    main_w = (int)w - pl_w - 3 * m;
    if (main_w < 120) {
        main_w = (int)w - 2 * m;
        pl_w = 0;
    }
    main_h = ((int)h * 38) / 100;
    if (main_h < 90) {
        main_h = 90;
    }

    L->mainp.x = (int16_t)m;
    L->mainp.y = (int16_t)m;
    L->mainp.w = (uint16_t)main_w;
    L->mainp.h = (uint16_t)main_h;

    L->pl.x = (int16_t)(m + main_w + gap);
    L->pl.y = (int16_t)m;
    L->pl.w = (uint16_t)((int)w - L->pl.x - m);
    L->pl.h = (uint16_t)((int)h - 2 * m);

    L->eq.x = L->mainp.x;
    L->eq.y = (int16_t)(L->mainp.y + L->mainp.h + gap);
    L->eq.w = L->mainp.w;
    L->eq.h = (uint16_t)((int)h - L->eq.y - m);

    L->title.x = (int16_t)(L->mainp.x + 8);
    L->title.y = (int16_t)(L->mainp.y + 6);
    L->title.w = (uint16_t)(L->mainp.w - 96);
    L->title.h = 16;

    L->time.x = (int16_t)(L->mainp.x + L->mainp.w - 80);
    L->time.y = L->title.y;
    L->time.w = 72;
    L->time.h = 16;

    sw = (int)L->mainp.w - 16;
    sh = (main_h >= 130) ? 36 : 24;
    L->spec[0].x = (int16_t)(L->mainp.x + 8);
    L->spec[0].y = (int16_t)(L->title.y + 18);
    sx = L->spec[0].x;
    sy = L->spec[0].y;
    for (i = 0; i < GFXBENCH_SPEC_N; i++) {
        int bw_i = sw / GFXBENCH_SPEC_N;
        L->spec[i].x = (int16_t)(sx + i * bw_i);
        L->spec[i].y = (int16_t)sy;
        L->spec[i].w = (uint16_t)(bw_i - 1);
        L->spec[i].h = (uint16_t)sh;
    }

    L->seek.x = (int16_t)(L->mainp.x + 8);
    L->seek.y = (int16_t)(sy + sh + 6);
    L->seek.w = (uint16_t)sw;
    L->seek.h = 12;

    bh = 24;
    bw = ((int)L->mainp.w - 16 - 4 * 6) / GFXBENCH_BTN_N;
    if (bw < 28) {
        bw = 28;
        bh = 18;
    }
    for (i = 0; i < GFXBENCH_BTN_N; i++) {
        L->btn[i].x = (int16_t)(L->mainp.x + 8 + i * (bw + 6));
        L->btn[i].y = (int16_t)(L->seek.y + L->seek.h + 6);
        L->btn[i].w = (uint16_t)bw;
        L->btn[i].h = (uint16_t)bh;
    }

    L->vol.x = L->seek.x;
    L->vol.y = (int16_t)(L->btn[0].y + bh + 6);
    L->vol.w = L->seek.w;
    L->vol.h = 10;

    eh = (int)L->eq.h - 20;
    if (eh < 40) {
        eh = 40;
    }
    for (i = 0; i < GFXBENCH_EQ_N; i++) {
        int col = ((int)L->eq.w - 16) / GFXBENCH_EQ_N;
        L->eq_band[i].x = (int16_t)(L->eq.x + 8 + i * col + 4);
        L->eq_band[i].y = (int16_t)(L->eq.y + 12);
        L->eq_band[i].w = (uint16_t)(col - 8);
        L->eq_band[i].h = (uint16_t)eh;
    }

    for (i = 0; i < GFXBENCH_PL_N; i++) {
        int rh = ((int)L->pl.h - 8) / GFXBENCH_PL_N;
        if (rh < 12) {
            rh = 12;
        }
        L->pl_row[i].x = (int16_t)(L->pl.x + 6);
        L->pl_row[i].y = (int16_t)(L->pl.y + 6 + i * rh);
        L->pl_row[i].w = (uint16_t)(L->pl.w - 12);
        L->pl_row[i].h = (uint16_t)(rh - 1);
    }
}

void gfxbench_state_reset(gfxbench_state_t *st)
{
    int i;
    memset(st, 0, sizeof(*st));
    st->vol = 70;
    st->playing = 1;
    st->sel = 2;
    for (i = 0; i < GFXBENCH_EQ_N; i++) {
        st->eq[i] = 40 + (i * 7) % 50;
    }
    for (i = 0; i < GFXBENCH_SPEC_N; i++) {
        st->spec[i] = 20 + (i * 13) % 70;
    }
}

void gfxbench_state_step(gfxbench_state_t *st, gfxbench_mode_t mode)
{
    int i;
    st->frame++;
    if (mode == GFXBENCH_IDLE) {
        return;
    }
    st->seek = (int)((st->frame * 3u) % 1001u);
    for (i = 0; i < GFXBENCH_SPEC_N; i++) {
        unsigned v = (st->frame * 17u + (unsigned)i * 29u) % 101u;
        st->spec[i] = (int)v;
    }
    if (mode == GFXBENCH_FULL) {
        st->vol = 40 + (int)((st->frame * 5u) % 61u);
        st->sel = (int)((st->frame / 8u) % (unsigned)GFXBENCH_PL_N);
        for (i = 0; i < GFXBENCH_EQ_N; i++) {
            st->eq[i] = 20 + (int)((st->frame * 3u + (unsigned)i * 11u) % 71u);
        }
    }
}

static gfxbench_mode_t parse_mode(const char *s)
{
    if (s == NULL) {
        return GFXBENCH_FULL;
    }
    if (s[0] == 'd' || s[0] == 'D') {
        return GFXBENCH_DIRTY;
    }
    if (s[0] == 'i' || s[0] == 'I') {
        return GFXBENCH_IDLE;
    }
    return GFXBENCH_FULL;
}

static int parse_frames(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        const char *s = argv[i];
        int n = 0;
        if (s == NULL || s[0] < '0' || s[0] > '9') {
            continue;
        }
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s - '0');
            s++;
        }
        if (*s == '\0') {
            return n;
        }
    }
    return -1;
}

static void print_mem(const char *tag)
{
    ag_meminfo_t mi;
    ag_meminfo(&mi);
    ag_printf("%s mem: arena %u/%u free, largest %u, fast %u, sys %u\n", tag,
              (unsigned)mi.arena_free, (unsigned)mi.arena_total,
              (unsigned)mi.arena_largest, (unsigned)mi.fast_free,
              (unsigned)mi.system_free);
}

static void report(const char *tag, uint32_t frames, uint64_t span_us,
                   uint64_t draw_sum, uint64_t flush_sum, uint64_t work_max)
{
    unsigned fps, draw, flush, work;
    if (frames == 0u || span_us == 0u) {
        return;
    }
    fps = (unsigned)((uint64_t)frames * 1000000u / span_us);
    draw = (unsigned)(draw_sum / frames);
    flush = (unsigned)(flush_sum / frames);
    work = draw + flush;
    ag_printf("%s: %u fps, work %u us (draw %u, flush %u), max %u us\n", tag,
              fps, work, draw, flush, (unsigned)work_max);
}

int ag_main(int argc, char **argv)
{
    ag_gfxinfo_t gi;
    gfxbench_layout_t lay;
    gfxbench_state_t st;
    gfxbench_mode_t mode = GFXBENCH_FULL;
    int max_frames = -1;
    int i;
    uint32_t window = 0;
    uint64_t draw_sum = 0, flush_sum = 0, work_max = 0;
    ag_time_t window_t0;
    const char *tag;
    const char *mode_s = "full";

    for (i = 1; i < argc; i++) {
        if (argv[i][0] >= '0' && argv[i][0] <= '9') {
            continue;
        }
        mode = parse_mode(argv[i]);
        mode_s = argv[i];
    }
    max_frames = parse_frames(argc, argv);

    if (ag_api()->gfx == NULL) {
        ag_printf("no gfx\n");
        return 1;
    }
    if (ag_gfx_acquire(&gi) != AG_OK) {
        ag_printf("acquire failed\n");
        return 1;
    }

    gfxbench_layout_init(&lay, gi.width, gi.height);
    gfxbench_state_reset(&st);
    if (gfxbench_backend_init(&gi, &lay) != 0) {
        ag_printf("backend init failed\n");
        ag_gfx_release();
        return 1;
    }

    tag = gfxbench_backend_name();
    ag_printf("%s %s %ux%u mode=%s frames=%d\n", tag, gi.double_buf ? "db" : "fb",
              (unsigned)gi.width, (unsigned)gi.height, mode_s, max_frames);
    print_mem(tag);

    window_t0 = ag_micros();
    for (i = 0; max_frames < 0 || i < max_frames; i++) {
        gfxbench_timing_t tm;
        ag_event_t ev;
        uint64_t work;

        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_QUIT) {
                max_frames = i;
                break;
            }
            if (ev.type == AG_EV_KEY_DOWN &&
                (ev.key.keycode == AG_KEY_ESC || ev.key.keycode == AG_KEY_Q)) {
                max_frames = i;
                break;
            }
        }
        if (ag_interrupted()) {
            break;
        }
        if (!ag_focused()) {
            ag_heartbeat();
            ag_delay(20);
            continue;
        }

        gfxbench_state_step(&st, mode);
        gfxbench_backend_frame(&st, &lay, mode, &tm);

        work = (uint64_t)tm.draw_us + (uint64_t)tm.flush_us;
        draw_sum += tm.draw_us;
        flush_sum += tm.flush_us;
        if (work > work_max) {
            work_max = work;
        }
        window++;

        {
            ag_time_t now = ag_micros();
            if ((uint64_t)(now - window_t0) >= 2000000u) {
                report(tag, window, (uint64_t)(now - window_t0), draw_sum,
                       flush_sum, work_max);
                print_mem(tag);
                draw_sum = flush_sum = work_max = 0;
                window = 0;
                window_t0 = now;
            }
        }
        ag_heartbeat();
        ag_yield();
    }

    if (window > 0) {
        report(tag, window, (uint64_t)(ag_micros() - window_t0), draw_sum,
               flush_sum, work_max);
    }
    print_mem(tag);
    gfxbench_backend_shutdown();
    ag_gfx_release();
    ag_printf("%s: done, %d frames\n", tag, i);
    return 0;
}
