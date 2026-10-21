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
 *   kvmagent                       find the board's WinUSB interface and start
 *   kvmagent --list                what WinUSB devices look like the board
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
#include <winsock2.h>
#include <ws2tcpip.h>
#include <setupapi.h>
/* winusb.h pulls in winusbio.h (the pipe-policy constants and PIPE_INFORMATION)
 * and needs windows.h ahead of it; include it last so those are in scope. */
#include <winusb.h>

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
#define AG_VID_TYPE 0x12
/* First-run setup, the one thing the agent and board must agree on by hand: the
 * board has no keyboard to be told a Wi-Fi name on, so the agent collects it at
 * a console and ships it, and the board reports back where it ended up. */
#define AG_VID_CONFIG 0x20    /* agent -> board: ssid '\0' pass '\0' webpass '\0' */
#define AG_VID_CFG_STATE 0x21 /* board -> agent: [state:1][IP ascii, NUL-term]   */

/* The board's VID/PID.  Matches ARGON_USB_VID/PID in usbdev_hw.c; a product
 * with its own pair changes both places. */
#define ARGON_VID 0x303A
#define ARGON_PID 0x4020

/* The DeviceInterfaceGUID the board's WinUSB vendor interface advertises - it
 * must match the GUID in the device's registry (set by the WinUSB/WCID
 * descriptors on the board).  This is how we find the device without caring
 * what COM-less path Windows gave it. */
static const GUID ARGON_KVM_GUID = {
    0xA7B3F1C2, 0x9D4E, 0x4F8A,
    {0xB6, 0xC1, 0x2E, 0x3D, 0x4A, 0x5B, 0x6C, 0x7D}};

/* ------------------------------------------------------------------------ */
/* The link: WinUSB to a board, or a socket when there is no board           */
/* ------------------------------------------------------------------------ */

typedef struct {
    HANDLE                 dev;    /* device file handle (overlapped), or
                                      INVALID_HANDLE_VALUE                     */
    WINUSB_INTERFACE_HANDLE winusb; /* the WinUSB handle, or NULL              */
    UCHAR                  pipe_in;  /* bulk IN  endpoint (device->host)       */
    UCHAR                  pipe_out; /* bulk OUT endpoint (host->device)       */
    HANDLE                 h;    /* legacy/unused; kept INVALID_HANDLE_VALUE   */
    SOCKET                 s;    /* the socket, or INVALID_SOCKET              */
} link_t;

static int link_write(link_t *l, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t      off = 0;

    while (off < len) {
        if (l->winusb != NULL) {
            /* Synchronous bulk OUT: WinUsb_WritePipe without an OVERLAPPED
             * blocks until the transfer completes, which is what we want - a
             * stalled write is the board telling us the picture is arriving
             * faster than it can forward it, the same back-pressure the serial
             * path relied on. */
            ULONG wrote = 0;
            if (!WinUsb_WritePipe(l->winusb, l->pipe_out,
                                  (PUCHAR)(p + off), (ULONG)(len - off),
                                  &wrote, NULL)) {
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
    if (l->winusb != NULL) {
        /* The IN pipe carries a PIPE_TRANSFER_TIMEOUT, so this returns soon
         * even when the board has nothing to say.  A timeout is not a dead
         * link - it is "nothing arrived" - so it maps to 0 bytes, the same
         * answer the serial path gave from an empty queue.  Only a genuine
         * failure (the device unplugged, the pipe wedged) is -1. */
        ULONG got = 0;
        if (WinUsb_ReadPipe(l->winusb, l->pipe_in, (PUCHAR)buf,
                            (ULONG)len, &got, NULL)) {
            return (int)got;
        }
        /* ERROR_SEM_TIMEOUT (121) is the PIPE_TRANSFER_TIMEOUT firing; the
         * WinUSB docs spell it ERROR_SEMAPHORE_TIMEOUT but the winerror.h
         * macro (MinGW and MSVC alike) is ERROR_SEM_TIMEOUT. */
        if (GetLastError() == ERROR_SEM_TIMEOUT) {
            return 0;
        }
        return -1;
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
 * Ask the WinUSB handle which pipes its one interface has, and keep the bulk
 * IN and bulk OUT endpoint addresses.  The pipe id is the USB endpoint address:
 * its high bit set means IN (device->host).  We do not assume the contract's
 * 0x83/0x03 - we read whatever the device enumerated - but we do insist the two
 * we need are bulk, because that is the transport the framing rides on.
 */
static int winusb_find_pipes(link_t *l)
{
    USB_INTERFACE_DESCRIPTOR ifd;
    memset(&ifd, 0, sizeof(ifd));
    if (!WinUsb_QueryInterfaceSettings(l->winusb, 0, &ifd)) {
        return 0;
    }

    int have_in = 0, have_out = 0;
    for (UCHAR i = 0; i < ifd.bNumEndpoints; i++) {
        WINUSB_PIPE_INFORMATION pi;
        memset(&pi, 0, sizeof(pi));
        if (!WinUsb_QueryPipe(l->winusb, 0, i, &pi)) {
            continue;
        }
        if (pi.PipeType != UsbdPipeTypeBulk) {
            continue;
        }
        if (pi.PipeId & 0x80) {
            if (!have_in) {
                l->pipe_in = pi.PipeId;
                have_in = 1;
            }
        } else {
            if (!have_out) {
                l->pipe_out = pi.PipeId;
                have_out = 1;
            }
        }
    }
    return have_in && have_out;
}

/*
 * Settle the IN pipe's policy.  A read with no data must come back quickly and
 * say "nothing", not block the capture loop forever, so PIPE_TRANSFER_TIMEOUT
 * gives it a 50 ms deadline (link_read turns that timeout into zero bytes).
 * RAW_IO off lets WinUSB buffer short reads for us; AUTO_CLEAR_STALL on means a
 * stalled endpoint recovers without us issuing the clear by hand.
 */
static void winusb_set_policy(link_t *l)
{
    ULONG   timeout = 50; /* ms */
    UCHAR   off = FALSE, on = TRUE;
    WinUsb_SetPipePolicy(l->winusb, l->pipe_in, PIPE_TRANSFER_TIMEOUT,
                         sizeof(timeout), &timeout);
    WinUsb_SetPipePolicy(l->winusb, l->pipe_in, RAW_IO,
                         sizeof(off), &off);
    WinUsb_SetPipePolicy(l->winusb, l->pipe_in, AUTO_CLEAR_STALL,
                         sizeof(on), &on);
}

/*
 * Find the board by its vendor interface's DeviceInterfaceGUID and open it.
 * Replaces the COM-port walk: a WinUSB device has no COM name to match on, it
 * has an interface path, so we enumerate the present interfaces that carry our
 * GUID, take the first, and bring up WinUSB on it.
 *
 * With list_only, print the paths instead of opening - the WinUSB equivalent of
 * "--list".  Returns 1 when a device was opened (or, for list_only, when at
 * least one was found).
 */
static int find_winusb_device(link_t *l, int list_only)
{
    HDEVINFO info = SetupDiGetClassDevsA(&ARGON_KVM_GUID, NULL, NULL,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) {
        return 0;
    }

    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(iface);
    int found = 0;

    for (DWORD i = 0;
         SetupDiEnumDeviceInterfaces(info, NULL, &ARGON_KVM_GUID, i, &iface);
         i++) {
        /* Two-call pattern: the first asks how many bytes the detail needs. */
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(info, &iface, NULL, 0, &need, NULL);
        if (need == 0) {
            continue;
        }
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(need);
        if (detail == NULL) {
            continue;
        }
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(info, &iface, detail, need, NULL,
                                              NULL)) {
            free(detail);
            continue;
        }

        if (list_only) {
            printf("%s%s\n", detail->DevicePath,
                   found ? "" : "   <- the board");
            found = 1;
            free(detail);
            continue;
        }

        HANDLE dev = CreateFileA(detail->DevicePath,
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        free(detail);
        if (dev == INVALID_HANDLE_VALUE) {
            continue;
        }

        WINUSB_INTERFACE_HANDLE wu = NULL;
        if (!WinUsb_Initialize(dev, &wu)) {
            CloseHandle(dev);
            continue;
        }

        l->dev = dev;
        l->winusb = wu;
        if (!winusb_find_pipes(l)) {
            WinUsb_Free(wu);
            CloseHandle(dev);
            l->dev = INVALID_HANDLE_VALUE;
            l->winusb = NULL;
            continue;
        }
        winusb_set_policy(l);
        found = 1;
        break;
    }

    SetupDiDestroyDeviceInfoList(info);
    return found;
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

/*
 * Type text the way the phone meant it, not the way the PC's keyboard layout
 * would read a scan code.  A USB keyboard sends key positions and the host
 * decides the letter, so on a Russian layout the position of "r" becomes a
 * Cyrillic letter.  The agent runs on the PC, so it can inject the actual
 * characters with KEYEVENTF_UNICODE, which carries no layout at all - what was
 * typed on the phone is what the focused window receives.
 */
static void type_unicode(const uint8_t *utf8, uint32_t len)
{
    const int wide = MultiByteToWideChar(CP_UTF8, 0, (const char *)utf8,
                                         (int)len, NULL, 0);
    if (wide <= 0) {
        return;
    }
    wchar_t *w = (wchar_t *)malloc((size_t)wide * sizeof(wchar_t));
    if (w == NULL) {
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, (const char *)utf8, (int)len, w, wide);

    for (int i = 0; i < wide; i++) {
        INPUT in[2];
        memset(in, 0, sizeof(in));
        in[0].type       = INPUT_KEYBOARD;
        in[0].ki.wScan   = w[i];
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1]            = in[0];
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
    }
    free(w);
}

/* ------------------------------------------------------------------------ */
/* First-run configuration                                                   */
/* ------------------------------------------------------------------------ */

/* The board tells us, every few moments until we act, whether it has a network
 * yet.  These remember what we have done about it so the once-only things stay
 * once-only: g_asked guards the wizard (we do not re-prompt while it joins),
 * g_configured guards the "Ready" line, and g_force_setup is --setup asking to
 * run the wizard even on a board that is already configured. */
static int g_configured = 0;
static int g_asked = 0;
static int g_force_setup = 0;

/* Read one line from the console into buf, without its trailing CR/LF.  An EOF
 * (the user closed the input) leaves an empty string, which the board reads as
 * "unchanged"; there is nothing better to do without a console. */
static void read_line(char *buf, size_t cap)
{
    buf[0] = '\0';
    if (fgets(buf, (int)cap, stdin) == NULL) {
        return;
    }
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
}

/*
 * The wizard.  The board has just told us it has no Wi-Fi, so there is no phone
 * to serve and nothing is lost by blocking on the console: ask for the network,
 * its password, and the password that will guard the web page, and send all
 * three as one NUL-separated payload (each field keeps its terminating NUL, so
 * the board can read them straight out of the buffer).  The board replies with
 * CFG_STATE 1 while it joins and 2 once it has an address.
 */
static void run_setup(link_t *l)
{
    char ssid[128], pass[128], webpass[128];

    printf("\n-- First-run setup --\n");
    printf("The dongle needs a Wi-Fi network to put the phone page on.\n");

    printf("Wi-Fi network (SSID): ");
    fflush(stdout);
    read_line(ssid, sizeof(ssid));

    printf("Wi-Fi password: ");
    fflush(stdout);
    read_line(pass, sizeof(pass));

    printf("Password to open the web page: ");
    fflush(stdout);
    read_line(webpass, sizeof(webpass));

    /* ssid '\0' pass '\0' webpass '\0' - the trailing NULs are part of the
     * length so the board gets three C strings back, empty ones included. */
    const size_t ls = strlen(ssid), lp = strlen(pass), lw = strlen(webpass);
    const size_t total = ls + 1 + lp + 1 + lw + 1;
    uint8_t *payload = (uint8_t *)malloc(total);
    if (payload == NULL) {
        return;
    }
    size_t o = 0;
    memcpy(payload + o, ssid, ls + 1);
    o += ls + 1;
    memcpy(payload + o, pass, lp + 1);
    o += lp + 1;
    memcpy(payload + o, webpass, lw + 1);
    o += lw + 1;

    send_msg(l, AG_VID_CONFIG, payload, (uint32_t)total);
    free(payload);

    printf("sent to the dongle; joining the network...\n");
    fflush(stdout);
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
    /* Without this the process is DPI-virtualised: on a display scaled past
     * 100% (125% here) GetSystemMetrics and BitBlt report and capture the
     * smaller logical screen, so the picture loses its bottom-right and the
     * composited cursor lands short of where Windows actually drew it.  Make
     * the agent see physical pixels, so one pixel is one pixel end to end. */
    SetProcessDPIAware();

    const char *tcp = NULL;
    int         fps = 15;
    int         tile = 64;
    int         list_only = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[i], "--setup") == 0) {
            /* Force the wizard the next time the board reports its config
             * state, even if it is already on a network - for changing the
             * Wi-Fi or the web password after first run. */
            g_force_setup = 1;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            /* Accepted and ignored: the board is no longer a COM port, it is a
             * WinUSB device found by its interface GUID.  Kept so old scripts
             * and muscle memory do not error out. */
            ++i;
        } else if (strcmp(argv[i], "--tcp") == 0 && i + 1 < argc) {
            tcp = argv[++i];
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--tile") == 0 && i + 1 < argc) {
            tile = atoi(argv[++i]);
        } else {
            printf("usage: kvmagent [--tcp host:port] "
                   "[--fps N] [--tile N] [--list] [--setup]\n");
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

    link_t link;
    link.dev = INVALID_HANDLE_VALUE;
    link.winusb = NULL;
    link.pipe_in = 0;
    link.pipe_out = 0;
    link.h = INVALID_HANDLE_VALUE;
    link.s = INVALID_SOCKET;

    if (list_only) {
        find_winusb_device(&link, 1);
        return 0;
    }

    if (tcp != NULL) {
        link.s = open_tcp(tcp);
        if (link.s == INVALID_SOCKET) {
            fprintf(stderr, "cannot connect to %s\n", tcp);
            return 2;
        }
        printf("connected to %s\n", tcp);
    } else {
        if (!find_winusb_device(&link, 0)) {
            fprintf(stderr,
                    "no ArgonOS KVM device found (is it plugged in and "
                    "running KVM.AXE?)\n");
            return 2;
        }
        printf("connected over WinUSB (IN 0x%02X, OUT 0x%02X)\n",
               link.pipe_in, link.pipe_out);
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
                } else if (type == AG_VID_TYPE && len > 0) {
                    type_unicode(msg + off + AG_VID_HDR, len);
                } else if (type == AG_VID_CFG_STATE && len >= 1) {
                    /*
                     * Where the board is on getting onto a network.  The first
                     * byte is the state; the rest is the IP as ASCII, which the
                     * board NUL-terminates - but we never trust a wire length,
                     * so bound the string to the frame and plant our own NUL.
                     */
                    const uint8_t *p = msg + off + AG_VID_HDR;
                    const uint8_t  state = p[0];
                    char ip[64];
                    uint32_t iplen = len - 1; /* bytes after the state byte */
                    if (iplen > sizeof(ip) - 1) {
                        iplen = sizeof(ip) - 1;
                    }
                    memcpy(ip, p + 1, iplen);
                    ip[iplen] = '\0'; /* terminate even if the board did not */

                    if (g_force_setup && !g_asked) {
                        /* --setup: reconfigure regardless of the reported
                         * state (the board may already be on a network). */
                        run_setup(&link);
                        g_asked = 1;
                    } else if (state == 0 && !g_asked) {
                        run_setup(&link);
                        g_asked = 1;
                    } else if (state == 1) {
                        if (!g_asked) {
                            printf("joining the network...\n");
                            fflush(stdout);
                            g_asked = 1;
                        }
                    } else if (state == 2 && !g_configured) {
                        printf("\nReady. Open  http://%s/  on the phone\n", ip);
                        fflush(stdout);
                        g_configured = 1;
                    }
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
    if (link.winusb != NULL) {
        WinUsb_Free(link.winusb);
    }
    if (link.dev != INVALID_HANDLE_VALUE) {
        CloseHandle(link.dev);
    }
    if (link.s != INVALID_SOCKET) {
        closesocket(link.s);
        WSACleanup();
    }
    printf("stopped: %u frames, %u tiles\n", frames, sent);
    return 0;
}
