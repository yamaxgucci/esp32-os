/*
 * kvmagent for Linux (X11) and macOS - the same job as kvmagent.c on Windows:
 * capture the screen, send the tiles that changed as PNG to the KVM dongle, and
 * take what the board sends back.  The wire format is the one kvmagent.c
 * speaks; the transport is the board's HID pipe (report 2 = [len][62 bytes],
 * report 3 = credit [taken u32][received u32]), which both systems drive with
 * no driver: hidraw on Linux, IOHIDManager on macOS.
 *
 *   kvmagent [--fps N] [--tile N] [--setup] [--list]
 *
 * Linux: X11 or XWayland (pure Wayland has no screen to read without a portal).
 * The hidraw node is root's unless 99-argon-kvm.rules is installed (see the
 * README next to this file).  macOS: grant Screen Recording to the terminal the
 * first time, or the picture is the wallpaper only.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "miniz.h"

#define AG_VID_HDR 8
#define AG_VID_MAX_PAYLOAD 8192
#define AG_VID_HELLO 0x01
#define AG_VID_TILE 0x02
#define AG_VID_CLIP 0x10
#define AG_VID_WANT 0x11
#define AG_VID_CONFIG 0x20
#define AG_VID_CFG_STATE 0x21
#define ARGON_VID 0x303A
#define ARGON_PID 0x4021

#define HID_REP 64
#define HID_WINDOW 3072u

/* ---- the transport: a byte ring filled by the platform's report reader --- */

static uint8_t           s_ring[1u << 16];
static volatile uint32_t s_rhead, s_rtail;
static volatile uint32_t s_taken, s_recv;
static volatile int      s_credits, s_alive;
static uint32_t          s_sent;
static volatile int      s_stop;

/* A report from the board, whatever the platform: ID first. */
static void on_report(const uint8_t *rep, size_t n)
{
    if (n >= 2 && rep[0] == 2) {
        uint32_t len = rep[1] > 62 ? 62 : rep[1];
        for (uint32_t i = 0; i < len && 2 + i < n; i++) {
            if (s_rhead - s_rtail >= sizeof(s_ring)) {
                break;
            }
            s_ring[s_rhead % sizeof(s_ring)] = rep[2 + i];
            s_rhead++;
        }
    } else if (n >= 9 && rep[0] == 3) {
        s_taken = (uint32_t)rep[1] | ((uint32_t)rep[2] << 8) |
                  ((uint32_t)rep[3] << 16) | ((uint32_t)rep[4] << 24);
        s_recv = (uint32_t)rep[5] | ((uint32_t)rep[6] << 8) |
                 ((uint32_t)rep[7] << 16) | ((uint32_t)rep[8] << 24);
        __sync_fetch_and_add(&s_credits, 1);
    }
}

static int  plat_hid_open(int list_only); /* 1 when open */
static int  plat_hid_write(const uint8_t rep[HID_REP]);
static void plat_hid_close(void);

static void msleep(int ms)
{
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

static int hid_send(const uint8_t *p, uint32_t n)
{
    uint8_t rep[HID_REP];
    memset(rep, 0, sizeof(rep));
    rep[0] = 2;
    rep[1] = (uint8_t)n;
    if (n) {
        memcpy(rep + 2, p, n);
    }
    return plat_hid_write(rep);
}

static int link_open(void)
{
    s_rhead = s_rtail = 0;
    s_credits = 0;
    if (!plat_hid_open(0)) {
        return 0;
    }
    (void)hid_send(NULL, 0); /* where are your counts? */
    for (int w = 0; w < 150 && s_credits == 0 && s_alive; w++) {
        msleep(10);
    }
    if (s_credits == 0) {
        plat_hid_close();
        return 0;
    }
    s_sent = s_recv;
    return 1;
}

static int link_write(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t         off = 0;
    while (off < len) {
        uint32_t n = (uint32_t)(len - off);
        if (n > 62) {
            n = 62;
        }
        while ((uint32_t)(s_sent + n - s_taken) > HID_WINDOW) {
            if (!s_alive) {
                return 0;
            }
            msleep(1);
        }
        if (!hid_send(p + off, n)) {
            return 0;
        }
        s_sent += n;
        off += n;
    }
    return 1;
}

static int link_read(void *buf, size_t len)
{
    if (!s_alive) {
        return -1;
    }
    uint32_t have = s_rhead - s_rtail;
    if (have > len) {
        have = (uint32_t)len;
    }
    for (uint32_t i = 0; i < have; i++) {
        ((uint8_t *)buf)[i] = s_ring[(s_rtail + i) % sizeof(s_ring)];
    }
    s_rtail += have;
    return (int)have;
}

static int send_msg(uint8_t type, const void *payload, uint32_t len)
{
    uint8_t hdr[AG_VID_HDR] = {'A', 'G', type, 0, (uint8_t)len,
                               (uint8_t)(len >> 8), (uint8_t)(len >> 16),
                               (uint8_t)(len >> 24)};
    if (!link_write(hdr, sizeof(hdr))) {
        return 0;
    }
    return len == 0 ? 1 : link_write(payload, len);
}

/* ---- the screen: the platform fills a 32-bit B,G,R,x buffer -------------- */

typedef struct {
    int      w, h, stride; /* stride in bytes */
    uint8_t *px;
} grab_t;

static int  plat_grab_open(grab_t *g);
static int  plat_grab(grab_t *g); /* 1 when px holds a fresh frame */
static void plat_grab_close(grab_t *g);

static void tile_rgb(const grab_t *g, int x, int y, int w, int h, uint8_t *out)
{
    for (int r = 0; r < h; r++) {
        const uint8_t *s = g->px + (size_t)(y + r) * g->stride + (size_t)x * 4;
        for (int c = 0; c < w; c++, s += 4) {
            *out++ = s[2];
            *out++ = s[1];
            *out++ = s[0];
        }
    }
}

static uint32_t tile_hash(const grab_t *g, int x, int y, int w, int h)
{
    uint32_t hv = 2166136261u;
    for (int r = 0; r < h; r++) {
        const uint8_t *s = g->px + (size_t)(y + r) * g->stride + (size_t)x * 4;
        for (int c = 0; c < w * 4; c++) {
            hv = (hv ^ s[c]) * 16777619u;
        }
    }
    return hv;
}

static int send_rect(grab_t *g, int x, int y, int w, int h, uint8_t *scratch,
                     int depth)
{
    tile_rgb(g, x, y, w, h, scratch);
    size_t png_len = 0;
    void  *png = tdefl_write_image_to_png_file_in_memory_ex(scratch, w, h, 3,
                                                           &png_len, 6, MZ_FALSE);
    if (png == NULL) {
        return 0;
    }
    if (png_len + 8u <= AG_VID_MAX_PAYLOAD) {
        uint8_t *msg = (uint8_t *)malloc(8 + png_len);
        int      ok = 0;
        if (msg != NULL) {
            const int v[4] = {x, y, w, h};
            for (int i = 0; i < 4; i++) {
                msg[i * 2] = (uint8_t)(v[i] & 0xFF);
                msg[i * 2 + 1] = (uint8_t)(v[i] >> 8);
            }
            memcpy(msg + 8, png, png_len);
            ok = send_msg(AG_VID_TILE, msg, (uint32_t)(8 + png_len));
            free(msg);
        }
        mz_free(png);
        return ok;
    }
    mz_free(png);
    if (depth > 4 || w <= 8 || h <= 8) {
        return 1;
    }
    const int hw = w / 2, hh = h / 2;
    return send_rect(g, x, y, hw, hh, scratch, depth + 1) &&
           send_rect(g, x + hw, y, w - hw, hh, scratch, depth + 1) &&
           send_rect(g, x, y + hh, hw, h - hh, scratch, depth + 1) &&
           send_rect(g, x + hw, y + hh, w - hw, h - hh, scratch, depth + 1);
}

/* ---- first-run setup and what the board reports -------------------------- */

static int  g_force_setup, g_asked, g_configured, g_busy_shown;
static char g_link_id[48], g_relay_host[128];

static void read_line(char *buf, size_t cap)
{
    buf[0] = 0;
    if (fgets(buf, (int)cap, stdin) == NULL) {
        return;
    }
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = 0;
    }
}

static void run_setup(void)
{
    char ssid[128], pass[128], webpass[128], rid[48];
    printf("\n-- First-run setup --\nWi-Fi network (SSID): ");
    fflush(stdout);
    read_line(ssid, sizeof(ssid));
    printf("Wi-Fi password: ");
    fflush(stdout);
    read_line(pass, sizeof(pass));
    printf("Password to open the web page: ");
    fflush(stdout);
    read_line(webpass, sizeof(webpass));
    printf("Id for the web link (letters/digits, Enter = automatic): ");
    fflush(stdout);
    read_line(rid, sizeof(rid));
    for (char *c = rid; *c; c++) {
        const char k = *c;
        if (!((k >= '0' && k <= '9') || (k >= 'a' && k <= 'z') ||
              (k >= 'A' && k <= 'Z') || k == '-' || k == '_')) {
            *c = '-';
        }
    }
    const size_t ls = strlen(ssid), lp = strlen(pass), lw = strlen(webpass),
                 li = strlen(rid);
    const size_t total = ls + lp + lw + li + 4;
    uint8_t     *pl = (uint8_t *)malloc(total);
    if (pl == NULL) {
        return;
    }
    size_t o = 0;
    memcpy(pl + o, ssid, ls + 1), o += ls + 1;
    memcpy(pl + o, pass, lp + 1), o += lp + 1;
    memcpy(pl + o, webpass, lw + 1), o += lw + 1;
    memcpy(pl + o, rid, li + 1), o += li + 1;
    send_msg(AG_VID_CONFIG, pl, (uint32_t)total);
    free(pl);
    printf("sent to the dongle; joining the network...\n");
}

/* RELAY.TXT next to this program: the relay's host, for the outside link. */
static void load_relay_host(const char *argv0)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s", argv0);
    char *slash = strrchr(path, '/');
    snprintf(slash ? slash + 1 : path,
             sizeof(path) - (size_t)(slash ? slash + 1 - path : 0),
             "%s", "RELAY.TXT");
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        snprintf(slash ? slash + 1 : path,
                 sizeof(path) - (size_t)(slash ? slash + 1 - path : 0), "%s",
                 "relay.txt");
        f = fopen(path, "r");
    }
    if (f != NULL) {
        char line[128];
        if (fgets(line, sizeof(line), f) != NULL) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                         line[n - 1] == ' ')) {
                line[--n] = 0;
            }
            snprintf(g_relay_host, sizeof(g_relay_host), "%s", line);
        }
        fclose(f);
    }
}

static void on_cfg_state(const uint8_t *p, uint32_t len)
{
    const uint8_t state = p[0];
    const char   *q = (const char *)(p + 1);
    const uint32_t tot = len - 1;
    char           ip[64] = "", bid[48] = "";
    int            busy = 0;
    uint32_t       i = 0;
    while (i < tot && q[i]) {
        i++;
    }
    snprintf(ip, sizeof(ip), "%.*s", (int)i, q);
    if (i + 1 < tot) {
        const char    *r = q + i + 1;
        const uint32_t rl = tot - i - 1;
        uint32_t       j = 0;
        while (j < rl && r[j]) {
            j++;
        }
        snprintf(bid, sizeof(bid), "%.*s", (int)j, r);
        busy = (j + 1 < rl) && r[j + 1] == '1';
    }
    if (busy && !g_busy_shown) {
        printf("\n!! The id '%s' is already in use on the relay by another KVM.\n"
               "   From outside, that link reaches the other dongle, not this "
               "one.\n   Choose another id: run this program with --setup.\n",
               bid);
        g_busy_shown = 1;
        g_link_id[0] = 0;
    } else if (!busy && g_busy_shown && state == 2) {
        printf("relay accepted the id '%s'\n", bid);
        g_busy_shown = 0;
    }
    if ((g_force_setup && !g_asked) || (state == 0 && !g_asked)) {
        run_setup();
        g_asked = 1;
    } else if (state == 1 && !g_asked) {
        printf("joining the network...\n");
        g_asked = 1;
    } else if (state == 2 && !g_configured) {
        printf("\nReady. On the same network:  http://%s/\n", ip);
        g_configured = 1;
    }
    if (state == 2 && !busy && g_relay_host[0] && bid[0] &&
        strcmp(bid, g_link_id) != 0) {
        printf("From anywhere (any network):  https://%s/?id=%s\n",
               g_relay_host, bid);
        snprintf(g_link_id, sizeof(g_link_id), "%s", bid);
    }
    fflush(stdout);
}

static void on_stop(int sig)
{
    (void)sig;
    s_stop = 1;
}

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

int main(int argc, char **argv)
{
    int fps = 15, tile = 64, list_only = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--tile") == 0 && i + 1 < argc) {
            tile = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--setup") == 0) {
            g_force_setup = 1;
        } else if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else {
            printf("usage: kvmagent [--fps N] [--tile N] [--setup] [--list]\n");
            return 1;
        }
    }
    if (fps < 1 || fps > 60) {
        fps = 15;
    }
    if (tile < 16 || tile > 256) {
        tile = 64;
    }
    if (list_only) {
        return plat_hid_open(1) ? 0 : 1;
    }
    load_relay_host(argv[0]);
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);

    grab_t g;
    if (!plat_grab_open(&g)) {
        fprintf(stderr, "cannot read the screen\n");
        return 3;
    }
    printf("screen %dx%d, %d px tiles, up to %d frames a second\nCtrl+C to stop\n",
           g.w, g.h, tile, fps);

    const int cols = (g.w + tile - 1) / tile, rows = (g.h + tile - 1) / tile;
    uint32_t *hashes = (uint32_t *)calloc((size_t)cols * rows, sizeof(uint32_t));
    uint8_t  *scratch = (uint8_t *)malloc((size_t)tile * tile * 3);
    uint8_t  *msg = (uint8_t *)malloc(AG_VID_MAX_PAYLOAD + AG_VID_HDR);
    const uint8_t hello[8] = {1, 0, (uint8_t)g.w, (uint8_t)(g.w >> 8),
                              (uint8_t)g.h, (uint8_t)(g.h >> 8),
                              (uint8_t)tile, (uint8_t)(tile >> 8)};
    int printed_wait = 0;

    while (!s_stop) {
        if (!link_open()) {
            if (!printed_wait) {
                printf("waiting for the dongle...\n");
                fflush(stdout);
                printed_wait = 1;
            }
            msleep(1000);
            continue;
        }
        printed_wait = 0;
        printf("dongle connected (HID)\n");
        fflush(stdout);
        send_msg(AG_VID_HELLO, hello, sizeof(hello));
        memset(hashes, 0, (size_t)cols * rows * sizeof(uint32_t));
        int      want_all = 1;
        uint32_t have = 0;
        uint64_t next = now_ms();
        const uint64_t period = 1000u / (unsigned)fps;

        while (!s_stop) {
            const int n = link_read(msg + have, AG_VID_MAX_PAYLOAD + AG_VID_HDR - have);
            if (n < 0) {
                break;
            }
            have += (uint32_t)n;
            uint32_t off = 0;
            while (have - off >= AG_VID_HDR) {
                const uint8_t *h = msg + off;
                if (h[0] != 'A' || h[1] != 'G') {
                    off++;
                    continue;
                }
                const uint32_t len = (uint32_t)h[4] | ((uint32_t)h[5] << 8) |
                                     ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
                if (len > AG_VID_MAX_PAYLOAD) {
                    off++;
                    continue;
                }
                if (have - off < AG_VID_HDR + len) {
                    break;
                }
                if (h[2] == AG_VID_WANT) {
                    want_all = 1;
                    send_msg(AG_VID_HELLO, hello, sizeof(hello));
                } else if (h[2] == AG_VID_CFG_STATE && len >= 1) {
                    on_cfg_state(h + AG_VID_HDR, len);
                }
                off += AG_VID_HDR + len;
            }
            if (off) {
                memmove(msg, msg + off, have - off);
                have -= off;
            }

            const uint64_t t = now_ms();
            if (t < next) {
                msleep(2);
                continue;
            }
            next = t + period;
            if (!plat_grab(&g)) {
                continue;
            }
            int ok = 1;
            for (int ty = 0; ok && ty < rows; ty++) {
                for (int tx = 0; ok && tx < cols; tx++) {
                    const int x = tx * tile, y = ty * tile;
                    const int w = (x + tile <= g.w) ? tile : g.w - x;
                    const int hh = (y + tile <= g.h) ? tile : g.h - y;
                    const uint32_t hv = tile_hash(&g, x, y, w, hh);
                    uint32_t *slot = &hashes[ty * cols + tx];
                    if (want_all || hv != *slot) {
                        ok = send_rect(&g, x, y, w, hh, scratch, 0);
                        *slot = hv;
                    }
                }
            }
            want_all = 0;
            if (!ok) {
                break;
            }
        }
        plat_hid_close();
        if (!s_stop) {
            printf("dongle gone; waiting for it to come back\n");
        }
    }
    plat_grab_close(&g);
    return 0;
}

/* ======================================================================== */
#if defined(__linux__)
/* Linux: hidraw for the pipe, X11 (+XFixes for the pointer) for the screen. */

#include <dirent.h>
#include <fcntl.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>

static int       s_fd = -1;
static pthread_t s_reader;

static void *reader(void *arg)
{
    (void)arg;
    uint8_t rep[HID_REP];
    while (s_alive) {
        const ssize_t n = read(s_fd, rep, sizeof(rep));
        if (n <= 0) {
            s_alive = 0;
            break;
        }
        on_report(rep, (size_t)n);
    }
    return NULL;
}

/* The node whose device is ours and whose report descriptor has the vendor
 * page: the mouse interface carries the pipe, the keyboard's does not. */
static int plat_hid_open(int list_only)
{
    DIR *d = opendir("/sys/class/hidraw");
    if (d == NULL) {
        return 0;
    }
    struct dirent *e;
    int            found = 0, denied = 0;
    while (!found && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "hidraw", 6) != 0) {
            continue;
        }
        char path[300], buf[4096];
        snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", e->d_name);
        FILE *f = fopen(path, "r");
        if (f == NULL) {
            continue;
        }
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        if (strstr(buf, "HID_ID=0003:0000303A:00004021") == NULL) {
            continue;
        }
        snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/report_descriptor",
                 e->d_name);
        f = fopen(path, "rb");
        if (f == NULL) {
            continue;
        }
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        int vendor = 0;
        for (size_t i = 0; i + 2 < n; i++) {
            if ((uint8_t)buf[i] == 0x06 && (uint8_t)buf[i + 1] == 0x00 &&
                (uint8_t)buf[i + 2] == 0xFF) {
                vendor = 1;
            }
        }
        if (!vendor) {
            continue;
        }
        snprintf(path, sizeof(path), "/dev/%s", e->d_name);
        if (list_only) {
            printf("%s   <- the board (HID)\n", path);
            found = 1;
            break;
        }
        const int fd = open(path, O_RDWR);
        if (fd < 0) {
            denied = 1;
            continue;
        }
        s_fd = fd;
        s_alive = 1;
        if (pthread_create(&s_reader, NULL, reader, NULL) != 0) {
            close(fd);
            s_fd = -1;
            s_alive = 0;
            continue;
        }
        pthread_detach(s_reader);
        found = 1;
    }
    closedir(d);
    if (!found && denied) {
        static int told;
        if (!told) {
            fprintf(stderr, "the dongle is there but its hidraw node is not "
                            "readable: install 99-argon-kvm.rules (see "
                            "README-LINUX.txt) or run with sudo\n");
            told = 1;
        }
    }
    return found;
}

static int plat_hid_write(const uint8_t rep[HID_REP])
{
    return write(s_fd, rep, HID_REP) == HID_REP;
}

static void plat_hid_close(void)
{
    s_alive = 0;
    if (s_fd >= 0) {
        close(s_fd); /* the reader's read() returns and it exits */
        s_fd = -1;
    }
}

static Display *s_dpy;
static Window   s_root;
static int      s_has_fixes;

static int plat_grab_open(grab_t *g)
{
    s_dpy = XOpenDisplay(NULL);
    if (s_dpy == NULL) {
        return 0;
    }
    s_root = DefaultRootWindow(s_dpy);
    XWindowAttributes a;
    XGetWindowAttributes(s_dpy, s_root, &a);
    g->w = a.width;
    g->h = a.height;
    g->stride = g->w * 4;
    g->px = (uint8_t *)calloc((size_t)g->stride, (size_t)g->h);
    int ev, er;
    s_has_fixes = XFixesQueryExtension(s_dpy, &ev, &er);
    return g->px != NULL;
}

static int plat_grab(grab_t *g)
{
    XImage *im = XGetImage(s_dpy, s_root, 0, 0, (unsigned)g->w, (unsigned)g->h,
                           AllPlanes, ZPixmap);
    if (im == NULL) {
        return 0;
    }
    for (int y = 0; y < g->h; y++) {
        uint8_t *dst = g->px + (size_t)y * g->stride;
        if (im->bits_per_pixel == 32) {
            memcpy(dst, im->data + (size_t)y * im->bytes_per_line, (size_t)g->w * 4);
        } else {
            for (int x = 0; x < g->w; x++) {
                const unsigned long p = XGetPixel(im, x, y);
                dst[x * 4 + 0] = (uint8_t)(p & 0xFF);
                dst[x * 4 + 1] = (uint8_t)((p >> 8) & 0xFF);
                dst[x * 4 + 2] = (uint8_t)((p >> 16) & 0xFF);
            }
        }
    }
    XDestroyImage(im);
    /* The pointer is not in XGetImage; draw it in, as the Windows agent does. */
    if (s_has_fixes) {
        XFixesCursorImage *c = XFixesGetCursorImage(s_dpy);
        if (c != NULL) {
            for (int cy = 0; cy < c->height; cy++) {
                for (int cx = 0; cx < c->width; cx++) {
                    const int x = c->x - c->xhot + cx, y = c->y - c->yhot + cy;
                    if (x < 0 || y < 0 || x >= g->w || y >= g->h) {
                        continue;
                    }
                    const unsigned long p = c->pixels[cy * c->width + cx];
                    const unsigned al = (unsigned)(p >> 24) & 0xFF;
                    if (al == 0) {
                        continue;
                    }
                    uint8_t *d = g->px + (size_t)y * g->stride + (size_t)x * 4;
                    for (int k = 0; k < 3; k++) {
                        const unsigned s = (unsigned)(p >> (8 * k)) & 0xFF;
                        d[k] = (uint8_t)((s * al + d[k] * (255 - al)) / 255);
                    }
                }
            }
            XFree(c);
        }
    }
    return 1;
}

static void plat_grab_close(grab_t *g)
{
    free(g->px);
    if (s_dpy) {
        XCloseDisplay(s_dpy);
    }
}

/* ======================================================================== */
#elif defined(__APPLE__)
/* macOS: IOHIDManager for the pipe, CoreGraphics for the screen. */

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>

static IOHIDManagerRef s_mgr;
static IOHIDDeviceRef  s_dev;
static CFRunLoopRef    s_loop;
static pthread_t       s_thread;
static uint8_t         s_inbuf[HID_REP];
static volatile int    s_ready;

static void input_cb(void *ctx, IOReturn res, void *sender, IOHIDReportType type,
                     uint32_t id, uint8_t *report, CFIndex len)
{
    (void)ctx;
    (void)sender;
    (void)type;
    if (res != kIOReturnSuccess) {
        return;
    }
    uint8_t rep[HID_REP + 1];
    /* Some versions hand the ID inside the buffer, some only in `id`. */
    if (len > 0 && report[0] == (uint8_t)id && len == HID_REP) {
        on_report(report, (size_t)len);
    } else if (len < HID_REP + 1) {
        rep[0] = (uint8_t)id;
        memcpy(rep + 1, report, (size_t)len);
        on_report(rep, (size_t)len + 1);
    }
}

static void removed_cb(void *ctx, IOReturn res, void *sender)
{
    (void)ctx;
    (void)res;
    (void)sender;
    s_alive = 0;
}

static CFDictionaryRef match_dict(void)
{
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    const int vals[4] = {ARGON_VID, ARGON_PID, 0xFF00, 1};
    const CFStringRef keys[4] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey),
                                 CFSTR(kIOHIDPrimaryUsagePageKey),
                                 CFSTR(kIOHIDPrimaryUsageKey)};
    for (int i = 0; i < 4; i++) {
        CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vals[i]);
        CFDictionarySetValue(d, keys[i], n);
        CFRelease(n);
    }
    return d;
}

static void *loop_thread(void *arg)
{
    (void)arg;
    s_loop = CFRunLoopGetCurrent();
    IOHIDDeviceScheduleWithRunLoop(s_dev, s_loop, kCFRunLoopDefaultMode);
    IOHIDDeviceRegisterInputReportCallback(s_dev, s_inbuf, sizeof(s_inbuf),
                                           input_cb, NULL);
    IOHIDDeviceRegisterRemovalCallback(s_dev, removed_cb, NULL);
    s_ready = 1;
    while (s_alive) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
    }
    IOHIDDeviceUnscheduleFromRunLoop(s_dev, s_loop, kCFRunLoopDefaultMode);
    return NULL;
}

static int plat_hid_open(int list_only)
{
    if (s_mgr == NULL) {
        s_mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        CFDictionaryRef m = match_dict();
        IOHIDManagerSetDeviceMatching(s_mgr, m);
        CFRelease(m);
        IOHIDManagerOpen(s_mgr, kIOHIDOptionsTypeNone);
    }
    CFSetRef set = IOHIDManagerCopyDevices(s_mgr);
    if (set == NULL) {
        return 0;
    }
    const CFIndex n = CFSetGetCount(set);
    if (n == 0) {
        CFRelease(set);
        return 0;
    }
    IOHIDDeviceRef devs[8];
    CFSetGetValues(set, (const void **)devs);
    if (list_only) {
        printf("IOHID %04x:%04x usage page 0xFF00   <- the board (HID)\n",
               ARGON_VID, ARGON_PID);
        CFRelease(set);
        return 1;
    }
    s_dev = devs[0];
    CFRetain(s_dev);
    CFRelease(set);
    if (IOHIDDeviceOpen(s_dev, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        CFRelease(s_dev);
        s_dev = NULL;
        return 0;
    }
    s_alive = 1;
    s_ready = 0;
    pthread_create(&s_thread, NULL, loop_thread, NULL);
    for (int w = 0; w < 100 && !s_ready; w++) {
        msleep(5);
    }
    return 1;
}

static int plat_hid_write(const uint8_t rep[HID_REP])
{
    return IOHIDDeviceSetReport(s_dev, kIOHIDReportTypeOutput, rep[0], rep,
                                HID_REP) == kIOReturnSuccess;
}

static void plat_hid_close(void)
{
    s_alive = 0;
    if (s_dev != NULL) {
        pthread_join(s_thread, NULL);
        IOHIDDeviceClose(s_dev, kIOHIDOptionsTypeNone);
        CFRelease(s_dev);
        s_dev = NULL;
    }
}

static int plat_grab_open(grab_t *g)
{
    const CGDirectDisplayID d = CGMainDisplayID();
    g->w = (int)CGDisplayPixelsWide(d);
    g->h = (int)CGDisplayPixelsHigh(d);
    g->stride = g->w * 4;
    g->px = (uint8_t *)calloc((size_t)g->stride, (size_t)g->h);
    return g->px != NULL;
}

static int plat_grab(grab_t *g)
{
    CGImageRef img = CGDisplayCreateImage(CGMainDisplayID());
    if (img == NULL) {
        return 0;
    }
    /* Draw into our buffer at the logical size: a Retina screen comes back at
     * twice the points, and the phone wants the size the HELLO announced. */
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef    cx = CGBitmapContextCreate(
        g->px, (size_t)g->w, (size_t)g->h, 8, (size_t)g->stride, cs,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    if (cx != NULL) {
        CGContextDrawImage(cx, CGRectMake(0, 0, g->w, g->h), img);
        CGContextRelease(cx);
    }
    CGColorSpaceRelease(cs);
    CGImageRelease(img);
    return cx != NULL;
}

static void plat_grab_close(grab_t *g) { free(g->px); }

#else
#error "kvmagent_posix.c: Linux or macOS only (Windows has kvmagent.c)"
#endif
