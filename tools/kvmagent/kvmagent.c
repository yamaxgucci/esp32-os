/*
 * ArgonOS - the agent: the program on the far machine that sends its screen.
 *
 * This is the one piece of the remote desktop that runs on somebody else's
 * computer, and the whole design is arranged so that it is only needed for the
 * picture.  Keyboard and mouse go the other way entirely - out of the board as
 * USB HID, into a machine that has never heard of us - so they keep working in
 * a BIOS, at a login screen, and after this program has crashed.  Nothing here
 * is in the path of a keystroke.
 *
 * What it does, once a second thirty times over:
 *
 *   grab the desktop, draw the mouse cursor into the grab, hash it in tiles,
 *   and send the tiles that changed as PNGs down a serial port.
 *
 * The board at the other end of that serial port does not decode any of it: it
 * finds the message boundaries and forwards the bytes to a phone, where a
 * browser decodes PNG for free.  That is why the compression lives here, where
 * there is a desktop processor with nothing better to do, and why the format is
 * PNG rather than something cleverer - a browser already has a decoder for it.
 *
 * Lossless matters more than it looks like it should.  This is a remote desktop
 * for reading text on: a JPEG of a terminal is unreadable at the compression
 * ratios that would make it worth using, and a screen of text is exactly what
 * PNG is best at.
 *
 *   kvmagent                       find the board and start
 *   kvmagent --list                what serial ports look like the board
 *   kvmagent --port COM7           a particular one
 *   kvmagent --tcp 127.0.0.1:5563  the emulator instead of a board
 *   kvmagent --fps 20 --tile 64
 *
 * Build (mingw-w64, nothing else needed):
 *
 *   tools\kvmagent\build.cmd
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"

/* ------------------------------------------------------------------------ */
/* The wire format, which is apps/common/kvm/ag_vid.h seen from this side.   */
/* ------------------------------------------------------------------------ */

#define AG_VID_HDR 8
#define AG_VID_MAX_PAYLOAD 8192

#define AG_VID_HELLO 0x01
#define AG_VID_TILE 0x02
#define AG_VID_BYE 0x03
#define AG_VID_CLIP 0x10
#define AG_VID_WANT 0x11

/* The board's VID/PID.  Matches ARGON_USB_VID/PID in usbdev_hw.c; a product
 * with its own pair changes both places. */
#define ARGON_VID 0x303A
#define ARGON_PID 0x4020

/* ------------------------------------------------------------------------ */
/* The link: a COM port, or a socket when there is no board                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    HANDLE h;    /* the serial port, or INVALID_HANDLE_VALUE */
    SOCKET s;    /* the socket, or INVALID_SOCKET            */
} link_t;

static int link_write(link_t *l, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t      off = 0;

    while (off < len) {
        if (l->h != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            if (!WriteFile(l->h, p + off, (DWORD)(len - off), &wrote, NULL)) {
                return 0;
            }
            if (wrote == 0) {
                return 0;
            }
            off += wrote;
        } else {
            const int n = send(l->s, p + off, (int)(len - off), 0);
            if (n <= 0) {
                return 0;
            }
            off += (size_t)n;
        }
    }
    return 1;
}

/* Whatever has arrived, without waiting.  Returns bytes, or -1 when the link
 * has gone. */
static int link_read(link_t *l, void *buf, size_t len)
{
    if (l->h != INVALID_HANDLE_VALUE) {
        COMSTAT st;
        DWORD   errs = 0;
        if (!ClearCommError(l->h, &errs, &st)) {
            return -1;
        }
        if (st.cbInQue == 0) {
            return 0;
        }
        DWORD want = st.cbInQue < len ? st.cbInQue : (DWORD)len;
        DWORD got = 0;
        if (!ReadFile(l->h, buf, want, &got, NULL)) {
            return -1;
        }
        return (int)got;
    }

    fd_set rd;
    struct timeval tv = {0, 0};
    FD_ZERO(&rd);
    FD_SET(l->s, &rd);
    if (select(0, &rd, NULL, NULL, &tv) <= 0) {
        return 0;
    }
    const int n = recv(l->s, (char *)buf, (int)len, 0);
    if (n <= 0) {
        return -1;
    }
    return n;
}

static int send_msg(link_t *l, uint8_t type, const void *payload, uint32_t len)
{
    uint8_t hdr[AG_VID_HDR];

    if (len > AG_VID_MAX_PAYLOAD) {
        return 0;
    }
    hdr[0] = 'A';
    hdr[1] = 'G';
    hdr[2] = type;
    hdr[3] = 0;
    hdr[4] = (uint8_t)(len & 0xFF);
    hdr[5] = (uint8_t)((len >> 8) & 0xFF);
    hdr[6] = (uint8_t)((len >> 16) & 0xFF);
    hdr[7] = (uint8_t)((len >> 24) & 0xFF);

    if (!link_write(l, hdr, sizeof(hdr))) {
        return 0;
    }
    return len == 0 ? 1 : link_write(l, payload, len);
}

/* ------------------------------------------------------------------------ */
/* Finding the board                                                        */
/* ------------------------------------------------------------------------ */

/*
 * Walks the ports Windows knows about and picks the one whose hardware id
 * carries our VID and PID.  Guessing "the highest COM number" is what most
 * tools do and it is wrong the moment anything else is plugged in.
 */
static int find_port(char *out, size_t cap, int list_only)
{
    static const GUID guid_ports = {
        0x4D36E978, 0xE325, 0x11CE,
        {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}};

    HDEVINFO info = SetupDiGetClassDevsA(&guid_ports, NULL, NULL,
                                         DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) {
        return 0;
    }

    char want[32];
    snprintf(want, sizeof(want), "VID_%04X&PID_%04X", ARGON_VID, ARGON_PID);

    SP_DEVINFO_DATA dev;
    dev.cbSize = sizeof(dev);
    int found = 0;

    for (DWORD i = 0; SetupDiEnumDeviceInfo(info, i, &dev); i++) {
        char id[512] = {0};
        if (!SetupDiGetDeviceRegistryPropertyA(info, &dev, SPDRP_HARDWAREID,
                                               NULL, (PBYTE)id, sizeof(id),
                                               NULL)) {
            continue;
        }

        char name[64] = {0};
        HKEY key = SetupDiOpenDevRegKey(info, &dev, DICS_FLAG_GLOBAL, 0,
                                        DIREG_DEV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            DWORD type = 0, n = sizeof(name);
            RegQueryValueExA(key, "PortName", NULL, &type, (LPBYTE)name, &n);
            RegCloseKey(key);
        }
        if (name[0] == '\0') {
            continue;
        }

        const int mine = (strstr(id, want) != NULL);
        if (list_only) {
            char desc[256] = {0};
            SetupDiGetDeviceRegistryPropertyA(info, &dev, SPDRP_DEVICEDESC,
                                              NULL, (PBYTE)desc, sizeof(desc),
                                              NULL);
            printf("%-6s %s%s\n", name, desc, mine ? "   <- the board" : "");
        } else if (mine && !found) {
            snprintf(out, cap, "%s", name);
            found = 1;
        }
    }
    SetupDiDestroyDeviceInfoList(info);
    return found;
}

static HANDLE open_port(const char *name)
{
    char path[64];
    /* The \\.\ prefix is needed for COM10 and above and harmless below it. */
    snprintf(path, sizeof(path), "\\\\.\\%s", name);

    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return h;
    }

    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (GetCommState(h, &dcb)) {
        /*
         * The baud rate is a fiction on a USB serial port - the bytes go at
         * bulk-endpoint speed whatever is asked for - but DTR is not: the
         * board watches it to know whether anything over here has the port
         * open, and that is what its status line reports.
         */
        dcb.BaudRate = CBR_115200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        SetCommState(h, &dcb);
    }

    COMMTIMEOUTS to;
    memset(&to, 0, sizeof(to));
    /* Reads never block: link_read asks how much is queued first. Writes get
     * a long deadline, because a write stalling is the board telling us the
     * picture is arriving faster than it can forward it - which is exactly the
     * back-pressure this design wants. */
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutConstant = 0;
    to.ReadTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 5000;
    to.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(h, &to);

    SetupComm(h, 4096, 65536);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return h;
}

static SOCKET open_tcp(const char *hostport)
{
    char host[128];
    snprintf(host, sizeof(host), "%s", hostport);
    char *colon = strrchr(host, ':');
    if (colon == NULL) {
        fprintf(stderr, "--tcp wants host:port\n");
        return INVALID_SOCKET;
    }
    *colon = '\0';
    const char *port = colon + 1;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return INVALID_SOCKET;
    }

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL) {
        fprintf(stderr, "cannot resolve %s:%s\n", host, port);
        return INVALID_SOCKET;
    }

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s != INVALID_SOCKET &&
        connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

/* ------------------------------------------------------------------------ */
/* The screen                                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
    HDC        screen;
    HDC        mem;
    HBITMAP    bmp;
    uint8_t   *pix; /* BGRA, top-down                                       */
    int        w, h;
    int        tile;
    int        cols, rows;
    uint32_t  *hash;
} grab_t;

static int grab_open(grab_t *g, int tile)
{
    memset(g, 0, sizeof(*g));
    g->tile = tile;
    g->screen = GetDC(NULL);
    if (g->screen == NULL) {
        return 0;
    }
    g->w = GetSystemMetrics(SM_CXSCREEN);
    g->h = GetSystemMetrics(SM_CYSCREEN);
    g->mem = CreateCompatibleDC(g->screen);

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = g->w;
    bi.bmiHeader.biHeight = -g->h; /* negative: top-down, like everything else */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    g->bmp = CreateDIBSection(g->screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (g->bmp == NULL || bits == NULL) {
        return 0;
    }
    g->pix = (uint8_t *)bits;
    SelectObject(g->mem, g->bmp);

    g->cols = (g->w + tile - 1) / tile;
    g->rows = (g->h + tile - 1) / tile;
    g->hash = (uint32_t *)calloc((size_t)g->cols * g->rows, sizeof(uint32_t));
    return g->hash != NULL;
}

static void grab_close(grab_t *g)
{
    if (g->bmp) {
        DeleteObject(g->bmp);
    }
    if (g->mem) {
        DeleteDC(g->mem);
    }
    if (g->screen) {
        ReleaseDC(NULL, g->screen);
    }
    free(g->hash);
}

/*
 * One frame, with the cursor drawn into it.  BitBlt does not include the
 * cursor - it is not part of the desktop, it is composited on top - and a
 * remote desktop with no pointer is unusable, so it is painted in here.  It
 * needs no special handling for the tiles: a cursor that moved changes the
 * pixels under it, and the hash notices.
 */
static void grab_frame(grab_t *g)
{
    BitBlt(g->mem, 0, 0, g->w, g->h, g->screen, 0, 0, SRCCOPY | CAPTUREBLT);

    CURSORINFO ci;
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor) {
        ICONINFO ii;
        if (GetIconInfo(ci.hCursor, &ii)) {
            DrawIconEx(g->mem, ci.ptScreenPos.x - (int)ii.xHotspot,
                       ci.ptScreenPos.y - (int)ii.yHotspot, ci.hCursor, 0, 0,
                       0, NULL, DI_NORMAL);
            if (ii.hbmMask) {
                DeleteObject(ii.hbmMask);
            }
            if (ii.hbmColor) {
                DeleteObject(ii.hbmColor);
            }
        }
    }
}

static uint32_t tile_hash(const grab_t *g, int x, int y, int w, int h)
{
    uint32_t v = 2166136261u; /* FNV-1a */
    for (int r = 0; r < h; r++) {
        const uint32_t *row =
            (const uint32_t *)(g->pix + (size_t)(y + r) * g->w * 4) + x;
        for (int c = 0; c < w; c++) {
            v ^= row[c];
            v *= 16777619u;
        }
    }
    return v;
}

/* BGRA out of the grab, RGB into a tight buffer, which is what the PNG writer
 * wants and what halves the bytes it has to chew through. */
static void tile_rgb(const grab_t *g, int x, int y, int w, int h, uint8_t *out)
{
    for (int r = 0; r < h; r++) {
        const uint8_t *src = g->pix + ((size_t)(y + r) * g->w + x) * 4;
        uint8_t       *dst = out + (size_t)r * w * 3;
        for (int c = 0; c < w; c++) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst += 3;
            src += 4;
        }
    }
}

/*
 * One rectangle, as a message - splitting it when the compressed form will not
 * fit in what the board can hold at once.  Splitting rather than growing the
 * board's buffer is the right way round: the board is the machine with the
 * least memory in the chain, and a screen of photographs must not be able to
 * make it ask for more.
 */
static int send_rect(link_t *l, grab_t *g, int x, int y, int w, int h,
                     uint8_t *scratch, int depth)
{
    tile_rgb(g, x, y, w, h, scratch);

    size_t png_len = 0;
    void  *png = tdefl_write_image_to_png_file_in_memory_ex(
         scratch, w, h, 3, &png_len, 6, MZ_FALSE);
    if (png == NULL) {
        return 0;
    }

    if (png_len + 8u <= AG_VID_MAX_PAYLOAD) {
        uint8_t *msg = (uint8_t *)malloc(8 + png_len);
        int      ok = 0;
        if (msg != NULL) {
            msg[0] = (uint8_t)(x & 0xFF);
            msg[1] = (uint8_t)(x >> 8);
            msg[2] = (uint8_t)(y & 0xFF);
            msg[3] = (uint8_t)(y >> 8);
            msg[4] = (uint8_t)(w & 0xFF);
            msg[5] = (uint8_t)(w >> 8);
            msg[6] = (uint8_t)(h & 0xFF);
            msg[7] = (uint8_t)(h >> 8);
            memcpy(msg + 8, png, png_len);
            ok = send_msg(l, AG_VID_TILE, msg, (uint32_t)(8 + png_len));
            free(msg);
        }
        mz_free(png);
        return ok;
    }
    mz_free(png);

    /* Too big.  Quarter it; at 8x8 give up rather than recurse forever, which
     * cannot happen with any real content but is not worth trusting. */
    if (depth > 4 || w <= 8 || h <= 8) {
        return 1;
    }
    const int hw = w / 2, hh = h / 2;
    return send_rect(l, g, x, y, hw, hh, scratch, depth + 1) &&
           send_rect(l, g, x + hw, y, w - hw, hh, scratch, depth + 1) &&
           send_rect(l, g, x, y + hh, hw, h - hh, scratch, depth + 1) &&
           send_rect(l, g, x + hw, y + hh, w - hw, h - hh, scratch, depth + 1);
}

/* ------------------------------------------------------------------------ */
/* What comes back up the wire                                              */
/* ------------------------------------------------------------------------ */

/* The phone's text onto this machine's clipboard, so that a paste can put
 * letters no keyboard layout of ours could have typed. */
static void set_clipboard(const uint8_t *utf8, uint32_t len)
{
    const int wide = MultiByteToWideChar(CP_UTF8, 0, (const char *)utf8,
                                         (int)len, NULL, 0);
    if (wide <= 0 || !OpenClipboard(NULL)) {
        return;
    }
    EmptyClipboard();

    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (size_t)(wide + 1) * 2);
    if (mem != NULL) {
        wchar_t *p = (wchar_t *)GlobalLock(mem);
        MultiByteToWideChar(CP_UTF8, 0, (const char *)utf8, (int)len, p, wide);
        p[wide] = L'\0';
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
}

/* ------------------------------------------------------------------------ */

static volatile int s_stop;

static BOOL WINAPI on_break(DWORD type)
{
    (void)type;
    s_stop = 1;
    return TRUE;
}

int main(int argc, char **argv)
{
    const char *port = NULL;
    const char *tcp = NULL;
    int         fps = 15;
    int         tile = 64;
    int         list_only = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "--tcp") == 0 && i + 1 < argc) {
            tcp = argv[++i];
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--tile") == 0 && i + 1 < argc) {
            tile = atoi(argv[++i]);
        } else {
            printf("usage: kvmagent [--port COMn | --tcp host:port] "
                   "[--fps N] [--tile N] [--list]\n");
            return 1;
        }
    }
    if (fps < 1) {
        fps = 1;
    }
    if (fps > 60) {
        fps = 60;
    }
    if (tile < 16 || tile > 256) {
        tile = 64;
    }

    if (list_only) {
        char dummy[32];
        find_port(dummy, sizeof(dummy), 1);
        return 0;
    }

    link_t link;
    link.h = INVALID_HANDLE_VALUE;
    link.s = INVALID_SOCKET;

    if (tcp != NULL) {
        link.s = open_tcp(tcp);
        if (link.s == INVALID_SOCKET) {
            fprintf(stderr, "cannot connect to %s\n", tcp);
            return 2;
        }
        printf("connected to %s\n", tcp);
    } else {
        char found[32] = {0};
        if (port == NULL) {
            if (!find_port(found, sizeof(found), 0)) {
                fprintf(stderr,
                        "no ArgonOS board found (%04X:%04X).  Is it plugged "
                        "in, and running an image with the serial pipe?\n"
                        "Try --list, or --port COMn.\n",
                        ARGON_VID, ARGON_PID);
                return 2;
            }
            port = found;
        }
        link.h = open_port(port);
        if (link.h == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "cannot open %s (%lu)\n", port,
                    (unsigned long)GetLastError());
            return 2;
        }
        printf("connected on %s\n", port);
    }

    grab_t g;
    if (!grab_open(&g, tile)) {
        fprintf(stderr, "cannot capture the screen\n");
        return 3;
    }
    printf("screen %dx%d, %d px tiles, up to %d frames a second\n", g.w, g.h,
           tile, fps);
    printf("Ctrl+C to stop\n");

    SetConsoleCtrlHandler(on_break, TRUE);

    uint8_t *scratch = (uint8_t *)malloc((size_t)tile * tile * 3);
    uint8_t  in[1024];
    uint8_t  msg[AG_VID_HDR + AG_VID_MAX_PAYLOAD];
    size_t   have = 0;
    if (scratch == NULL) {
        return 3;
    }

    /* HELLO first: the phone cannot size its canvas until it knows. */
    uint8_t hello[8];
    hello[0] = 1;
    hello[1] = 0;
    hello[2] = (uint8_t)(g.w & 0xFF);
    hello[3] = (uint8_t)(g.w >> 8);
    hello[4] = (uint8_t)(g.h & 0xFF);
    hello[5] = (uint8_t)(g.h >> 8);
    hello[6] = (uint8_t)(tile & 0xFF);
    hello[7] = (uint8_t)(tile >> 8);
    send_msg(&link, AG_VID_HELLO, hello, sizeof(hello));

    int      want_all = 1;
    int      want_hello = 0;
    uint32_t frames = 0, sent = 0;
    DWORD    next = GetTickCount();
    const DWORD period = (DWORD)(1000 / fps);
    DWORD    report = GetTickCount() + 5000;

    while (!s_stop) {
        /* Anything from the phone, by way of the board. */
        const int n = link_read(&link, in, sizeof(in));
        if (n < 0) {
            printf("the link went away\n");
            break;
        }
        if (n > 0) {
            if (have + (size_t)n > sizeof(msg)) {
                have = 0; /* nothing we send is this big; resynchronise */
            }
            memcpy(msg + have, in, (size_t)n);
            have += (size_t)n;

            size_t off = 0;
            while (have - off >= AG_VID_HDR) {
                if (msg[off] != 'A' || msg[off + 1] != 'G') {
                    off++;
                    continue;
                }
                const uint32_t len = (uint32_t)msg[off + 4] |
                                     ((uint32_t)msg[off + 5] << 8) |
                                     ((uint32_t)msg[off + 6] << 16) |
                                     ((uint32_t)msg[off + 7] << 24);
                if (len > AG_VID_MAX_PAYLOAD) {
                    off++;
                    continue;
                }
                if (have - off < AG_VID_HDR + len) {
                    break;
                }
                const uint8_t type = msg[off + 2];
                if (type == AG_VID_WANT) {
                    /*
                     * A phone that has just connected asks for this, and it
                     * needs the screen's size as much as it needs the pixels:
                     * it never saw the HELLO sent when this program started,
                     * and without one there is nothing to size a canvas from.
                     */
                    want_all = 1;
                    want_hello = 1;
                } else if (type == AG_VID_CLIP && len > 0) {
                    set_clipboard(msg + off + AG_VID_HDR, len);
                }
                off += AG_VID_HDR + len;
            }
            if (off > 0) {
                memmove(msg, msg + off, have - off);
                have -= off;
            }
        }

        const DWORD now = GetTickCount();
        if ((int)(now - next) < 0) {
            Sleep(2);
            continue;
        }
        next = now + period;

        if (want_hello) {
            want_hello = 0;
            send_msg(&link, AG_VID_HELLO, hello, sizeof(hello));
        }

        grab_frame(&g);
        frames++;

        for (int ty = 0; ty < g.rows; ty++) {
            for (int tx = 0; tx < g.cols; tx++) {
                const int x = tx * tile;
                const int y = ty * tile;
                const int w = (x + tile <= g.w) ? tile : g.w - x;
                const int h = (y + tile <= g.h) ? tile : g.h - y;
                const uint32_t hv = tile_hash(&g, x, y, w, h);
                const int      idx = ty * g.cols + tx;

                if (!want_all && g.hash[idx] == hv) {
                    continue;
                }
                g.hash[idx] = hv;
                if (!send_rect(&link, &g, x, y, w, h, scratch, 0)) {
                    printf("the link went away\n");
                    s_stop = 1;
                    break;
                }
                sent++;
            }
            if (s_stop) {
                break;
            }
        }
        want_all = 0;

        if ((int)(GetTickCount() - report) >= 0) {
            report = GetTickCount() + 5000;
            printf("%u frames, %u tiles sent\n", frames, sent);
            fflush(stdout);
        }
    }

    send_msg(&link, AG_VID_BYE, NULL, 0);
    free(scratch);
    grab_close(&g);
    if (link.h != INVALID_HANDLE_VALUE) {
        CloseHandle(link.h);
    }
    if (link.s != INVALID_SOCKET) {
        closesocket(link.s);
        WSACleanup();
    }
    printf("stopped: %u frames, %u tiles\n", frames, sent);
    return 0;
}
