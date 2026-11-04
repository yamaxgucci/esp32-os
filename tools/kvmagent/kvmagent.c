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
#include <shellapi.h> /* Shell_NotifyIcon and NOTIFYICONDATA for the tray icon */
/* winusb.h pulls in winusbio.h (the pipe-policy constants and PIPE_INFORMATION)
 * and needs windows.h ahead of it; include it last so those are in scope. */
#include <winusb.h>
#include <hidsdi.h>
#include <hidpi.h>

/* No C runtime: the few library calls come from compat.c, on Win32 alone, so
 * one 32-bit executable runs on every Windows from 98 to 11 (see build.cmd). */
#include "compat.h"

#include "miniz.h"

/*
 * WinUSB is looked up at run time, not linked: winusb.dll is in Windows only
 * from Vista on, and binds to the board without a driver only from 8.1 on.  An
 * import of it would stop the program from starting at all on 98 and XP; a
 * lookup lets it start and use HID there instead.  The same for the one call
 * that does not exist before Vista, SetProcessDPIAware.
 */
typedef BOOL(WINAPI *pfn_WinUsb_Initialize)(HANDLE, PWINUSB_INTERFACE_HANDLE);
typedef BOOL(WINAPI *pfn_WinUsb_Free)(WINUSB_INTERFACE_HANDLE);
typedef BOOL(WINAPI *pfn_WinUsb_QueryInterfaceSettings)(
    WINUSB_INTERFACE_HANDLE, UCHAR, PUSB_INTERFACE_DESCRIPTOR);
typedef BOOL(WINAPI *pfn_WinUsb_QueryPipe)(WINUSB_INTERFACE_HANDLE, UCHAR,
                                           UCHAR, PWINUSB_PIPE_INFORMATION);
typedef BOOL(WINAPI *pfn_WinUsb_SetPipePolicy)(WINUSB_INTERFACE_HANDLE, UCHAR,
                                               ULONG, ULONG, PVOID);
typedef BOOL(WINAPI *pfn_WinUsb_ReadPipe)(WINUSB_INTERFACE_HANDLE, UCHAR, PUCHAR,
                                          ULONG, PULONG, LPOVERLAPPED);
typedef BOOL(WINAPI *pfn_WinUsb_WritePipe)(WINUSB_INTERFACE_HANDLE, UCHAR,
                                           PUCHAR, ULONG, PULONG, LPOVERLAPPED);
static pfn_WinUsb_Initialize             p_WinUsb_Initialize;
static pfn_WinUsb_Free                   p_WinUsb_Free;
static pfn_WinUsb_QueryInterfaceSettings p_WinUsb_QueryInterfaceSettings;
static pfn_WinUsb_QueryPipe              p_WinUsb_QueryPipe;
static pfn_WinUsb_SetPipePolicy          p_WinUsb_SetPipePolicy;
static pfn_WinUsb_ReadPipe               p_WinUsb_ReadPipe;
static pfn_WinUsb_WritePipe              p_WinUsb_WritePipe;
#define WinUsb_Initialize p_WinUsb_Initialize
#define WinUsb_Free p_WinUsb_Free
#define WinUsb_QueryInterfaceSettings p_WinUsb_QueryInterfaceSettings
#define WinUsb_QueryPipe p_WinUsb_QueryPipe
#define WinUsb_SetPipePolicy p_WinUsb_SetPipePolicy
#define WinUsb_ReadPipe p_WinUsb_ReadPipe
#define WinUsb_WritePipe p_WinUsb_WritePipe

static int winusb_load(void)
{
    static int tried, ok;
    if (tried) {
        return ok;
    }
    tried = 1;
    HMODULE m = LoadLibraryA("winusb.dll");
    if (m == NULL) {
        return 0;
    }
#define WU(name) (p_##name = (pfn_##name)(void *)GetProcAddress(m, #name))
    ok = WU(WinUsb_Initialize) && WU(WinUsb_Free) &&
         WU(WinUsb_QueryInterfaceSettings) && WU(WinUsb_QueryPipe) &&
         WU(WinUsb_SetPipePolicy) && WU(WinUsb_ReadPipe) &&
         WU(WinUsb_WritePipe);
#undef WU
    return ok;
}

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
#define ARGON_PID 0x4021

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
    HANDLE                 hid;  /* the HID pipe collection, or INVALID_...    */
} link_t;

/*
 * The pipe over HID, for every Windows that has no driverless WinUSB (98, ME,
 * 2000, XP, 7).  The board's mouse interface carries a vendor collection:
 * report 2 is [len][up to 62 bytes] each way, report 3 (board to us) is
 * [taken u32][received u32] - credit, because an interrupt OUT endpoint has no
 * flow control of its own and we must not send more than the board can hold.
 * A reader thread takes reports as they come (ReadFile on HID blocks, and there
 * is no overlapped I/O on 98 worth relying on) into a ring link_read drains.
 */
static int g_trace; /* --trace, defined with the other options below */
static HANDLE s_hread = INVALID_HANDLE_VALUE; /* the reader thread's handle */

#define HID_REP 64        /* report ID + 63 */
#define HID_WINDOW 3072u  /* the board's ring is 4096 */
static uint8_t           s_hring[1u << 16];
static volatile uint32_t s_hhead, s_htail;
static volatile uint32_t s_htaken, s_hrecv; /* the board's counts */
static volatile LONG     s_hcredits;        /* credit reports seen */
static volatile LONG     s_hid_alive;
static uint32_t          s_hsent;           /* in the board's numbering */

static DWORD WINAPI hid_reader(LPVOID arg)
{
    HANDLE  h = (HANDLE)arg;
    uint8_t rep[HID_REP];
    while (s_hid_alive) {
        DWORD got = 0;
        if (!ReadFile(h, rep, HID_REP, &got, NULL) || got < 2) {
            s_hid_alive = 0;
            break;
        }
        if (rep[0] == 2) {
            uint32_t n = rep[1] > 62 ? 62 : rep[1];
            for (uint32_t i = 0; i < n; i++) {
                if (s_hhead - s_htail >= sizeof(s_hring)) {
                    break; /* we are not reading: drop, the stream is lost */
                }
                s_hring[s_hhead % sizeof(s_hring)] = rep[2 + i];
                s_hhead++;
            }
        } else if (rep[0] == 3 && got >= 9) {
            s_htaken = (uint32_t)rep[1] | ((uint32_t)rep[2] << 8) |
                       ((uint32_t)rep[3] << 16) | ((uint32_t)rep[4] << 24);
            s_hrecv = (uint32_t)rep[5] | ((uint32_t)rep[6] << 8) |
                      ((uint32_t)rep[7] << 16) | ((uint32_t)rep[8] << 24);
            InterlockedIncrement(&s_hcredits);
        }
    }
    return 0;
}

static int hid_send(HANDLE h, const uint8_t *p, uint32_t n)
{
    uint8_t rep[HID_REP];
    memset(rep, 0, sizeof(rep));
    rep[0] = 2;
    rep[1] = (uint8_t)n;
    if (n > 0) {
        memcpy(rep + 2, p, n);
    }
    DWORD w = 0;
    return WriteFile(h, rep, HID_REP, &w, NULL) && w == HID_REP;
}

/*
 * Find the board's pipe collection among the HID devices: our VID/PID, usage
 * page 0xFF00.  Windows makes each top-level collection its own device path, so
 * this is the vendor one and not the mouse beside it.  Then ask the board for
 * its counts (an empty report) - an older board without the pipe never answers,
 * and the link is not taken.
 */
static int find_hid_device(link_t *l, int list_only)
{
    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO info = SetupDiGetClassDevsA(&g, NULL, NULL,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) {
        return 0;
    }
    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(iface);
    int found = 0;
    for (DWORD i = 0; !found &&
         SetupDiEnumDeviceInterfaces(info, NULL, &g, i, &iface); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(info, &iface, NULL, 0, &need, NULL);
        if (need == 0) {
            continue;
        }
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *d =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(need);
        if (d == NULL) {
            continue;
        }
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(info, &iface, d, need, NULL,
                                              NULL)) {
            free(d);
            continue;
        }
        if (g_trace) {
            printf("hid: try %s\n", d->DevicePath);
        }
        HANDLE h = CreateFileA(d->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            free(d);
            continue; /* a keyboard or mouse the system holds, most likely */
        }
        HIDD_ATTRIBUTES a;
        a.Size = sizeof(a);
        PHIDP_PREPARSED_DATA pp = NULL;
        HIDP_CAPS            caps;
        int                  ours = 0;
        if (HidD_GetAttributes(h, &a) && a.VendorID == ARGON_VID &&
            a.ProductID == ARGON_PID && HidD_GetPreparsedData(h, &pp)) {
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS &&
                caps.UsagePage == 0xFF00 && caps.Usage == 1 &&
                caps.InputReportByteLength == HID_REP) {
                ours = 1;
            }
            HidD_FreePreparsedData(pp);
        }
        if (!ours) {
            CloseHandle(h);
            free(d);
            continue;
        }
        if (list_only) {
            printf("%s   <- the board (HID)\n", d->DevicePath);
            CloseHandle(h);
            free(d);
            found = 1;
            break;
        }
        /*
         * A second handle for writing.  I/O on a synchronous handle is done one
         * call at a time, and the reader thread sits in ReadFile on the first
         * for as long as the board is quiet - a WriteFile on the same handle
         * would wait behind it for ever.
         */
        HANDLE hw = CreateFileA(d->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, 0, NULL);
        free(d);
        if (hw == INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            continue;
        }
        s_hhead = s_htail = 0;
        s_hcredits = 0;
        s_hid_alive = 1;
        HANDLE t = CreateThread(NULL, 0, hid_reader, (LPVOID)h, 0, NULL);
        if (t == NULL) {
            s_hid_alive = 0;
            CloseHandle(h);
            CloseHandle(hw);
            continue;
        }
        CloseHandle(t);
        s_hread = h;
        h = hw;
        if (g_trace) {
            printf("hid: ours, asking for credit\n");
        }
        const int sent0 = hid_send(h, NULL, 0); /* where are your counts? */
        if (g_trace) {
            printf("hid: empty report %s (err %lu)\n", sent0 ? "sent" : "FAILED",
                   (unsigned long)GetLastError());
        }
        for (int w = 0; w < 150 && s_hcredits == 0 && s_hid_alive; w++) {
            Sleep(10);
        }
        if (g_trace) {
            printf("hid: credits %ld alive %ld\n", (long)s_hcredits, (long)s_hid_alive);
        }
        if (s_hcredits == 0) {
            s_hid_alive = 0;
            CloseHandle(h);
            CloseHandle(s_hread); /* unblocks the reader on NT; on 9x it exits with us */
            s_hread = INVALID_HANDLE_VALUE;
            continue;
        }
        s_hsent = s_hrecv;
        l->hid = h;
        found = 1;
    }
    SetupDiDestroyDeviceInfoList(info);
    return found;
}

static int link_write(link_t *l, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t      off = 0;

    while (off < len) {
        if (l->hid != INVALID_HANDLE_VALUE) {
            /* Within the board's window: what it has received but not yet
             * taken, plus what we sent that has not arrived, stays under
             * HID_WINDOW.  Waiting here is the same back-pressure a stalled
             * bulk write gives on WinUSB. */
            uint32_t n = (uint32_t)(len - off);
            if (n > 62) {
                n = 62;
            }
            while ((uint32_t)(s_hsent + n - s_htaken) > HID_WINDOW) {
                if (!s_hid_alive) {
                    return 0;
                }
                Sleep(1);
            }
            if (!hid_send(l->hid, (const uint8_t *)(p + off), n)) {
                return 0;
            }
            s_hsent += n;
            off += n;
            continue;
        }
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
    if (l->hid != INVALID_HANDLE_VALUE) {
        if (!s_hid_alive) {
            return -1;
        }
        uint32_t have = s_hhead - s_htail;
        if (have > len) {
            have = (uint32_t)len;
        }
        for (uint32_t i = 0; i < have; i++) {
            ((uint8_t *)buf)[i] = s_hring[(s_htail + i) % sizeof(s_hring)];
        }
        s_htail += have;
        if (have == 0) {
            Sleep(1); /* what the WinUSB read's short timeout gave */
        }
        return (int)have;
    }
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

    /* gethostbyname, not getaddrinfo: the latter is not in the Winsock that
     * Windows 98 and 2000 have, and importing it would stop the program
     * starting there at all. */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)atoi(port));
    sa.sin_addr.s_addr = inet_addr(host);
    if (sa.sin_addr.s_addr == INADDR_NONE) {
        struct hostent *he = gethostbyname(host);
        if (he == NULL || he->h_addr_list[0] == NULL) {
            fprintf(stderr, "cannot resolve %s:%s\n", host, port);
            return INVALID_SOCKET;
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET &&
        connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        closesocket(s);
        s = INVALID_SOCKET;
    }
    return s;
}

/* Where to find the link, remembered from argv so the reconnect loop can go
 * looking again: the --tcp target, or NULL for the WinUSB board. */
static const char *g_tcp = NULL;
static int         g_force_hid = 0;
static int         g_trace = 0; /* --trace: say what the link setup is doing */ /* --hid: skip WinUSB even where it works */

/*
 * Bring the link up, from nothing to ready-to-stream: for a board, find its
 * WinUSB interface and its bulk pipes; for --tcp, connect the socket.  Returns
 * 1 connected, 0 not (yet).  The whole reason it is a function is that the
 * board reboots - the USB re-enumerates and the old handle dies - so we come
 * back here to open it afresh.  It resets the handles up front and leaves
 * nothing half-open on failure (find_winusb_device and open_tcp each close
 * their own partial state), so it is safe to call again and again.
 */
static int link_open(link_t *l)
{
    l->dev     = INVALID_HANDLE_VALUE;
    l->winusb  = NULL;
    l->pipe_in = 0;
    l->pipe_out = 0;
    l->h       = INVALID_HANDLE_VALUE;
    l->s       = INVALID_SOCKET;
    l->hid     = INVALID_HANDLE_VALUE;

    if (g_tcp != NULL) {
        l->s = open_tcp(g_tcp);
        return l->s != INVALID_SOCKET;
    }
    /* WinUSB where Windows binds it by itself (8.1 on) - it is ~15x faster -
     * and HID everywhere else. */
    if (!g_force_hid && winusb_load() && find_winusb_device(l, 0)) {
        return 1;
    }
    return find_hid_device(l, 0);
}

/* Drop the link and return l to the "nothing open" state, so link_open can run
 * again.  WinUsb_Free before CloseHandle for the board; closesocket for tcp.
 * (WSACleanup is left for the final teardown - WSAStartup is reference-counted
 * and a reconnect simply opens another socket.) */
static void link_close(link_t *l)
{
    if (l->hid != INVALID_HANDLE_VALUE) {
        s_hid_alive = 0;
        CloseHandle(l->hid);
        l->hid = INVALID_HANDLE_VALUE;
        if (s_hread != INVALID_HANDLE_VALUE) {
            CloseHandle(s_hread);
            s_hread = INVALID_HANDLE_VALUE;
        }
    }
    if (l->winusb != NULL) {
        WinUsb_Free(l->winusb);
        l->winusb = NULL;
    }
    if (l->dev != INVALID_HANDLE_VALUE) {
        CloseHandle(l->dev);
        l->dev = INVALID_HANDLE_VALUE;
    }
    if (l->s != INVALID_SOCKET) {
        closesocket(l->s);
        l->s = INVALID_SOCKET;
    }
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

/* The public address of the relay the dongle dials, if there is one.  The board
 * keeps the relay's host, port and secret in C:\KVMRLY.TXT; the agent needs only
 * the host, and only to print a URL, so it reads it from a plain RELAY.TXT laid
 * next to this exe on the flash drive - no secret on the agent's side, and the
 * board's firmware is untouched.  Empty means "LAN only", the old behaviour. */
static char g_relay_host[128] = "";
static char g_link_id[48] = "";  /* the id the printed link carries          */
static int  g_busy_shown = 0;    /* the "id taken" warning is on the console  */

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

/* Fill g_relay_host from RELAY.TXT sitting next to the exe (the flash drive the
 * agent runs from).  First non-blank line is the host; leading/trailing spaces
 * and any CR/LF are trimmed.  No file, or no readable line, leaves it empty and
 * the agent stays LAN-only. */
static void load_relay_host(void)
{
    char path[MAX_PATH];
    DWORD got = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    if (got == 0 || got >= sizeof(path)) {
        return;
    }
    char *slash = strrchr(path, '\\');
    if (slash == NULL) {
        return;
    }
    slash[1] = '\0';
    if (strlen(path) + strlen("RELAY.TXT") >= sizeof(path)) {
        return;
    }
    strcat(path, "RELAY.TXT");

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    char line[128];
    while (fgets(line, (int)sizeof(line), f) != NULL) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                         line[n - 1] == ' ' || line[n - 1] == '\t')) {
            line[--n] = '\0';
        }
        char *s = line;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (*s != '\0') {
            snprintf(g_relay_host, sizeof(g_relay_host), "%s", s);
            break;
        }
    }
    fclose(f);
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
    char ssid[128], pass[128], webpass[128], rid[48];

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

    /* The name in the link (https://<relay>/?id=<this>).  Nothing is asked of
     * the server here - setup is local; if somebody else's dongle is already
     * using the same id there, the board says so once it dials in. */
    printf("Id for the web link (letters/digits, Enter = automatic): ");
    fflush(stdout);
    read_line(rid, sizeof(rid));
    for (char *c = rid; *c; c++) {
        if (!(isalnum((unsigned char)*c) || *c == '-' || *c == '_')) {
            *c = '-'; /* keep it typeable and URL-safe */
        }
    }

    /* ssid '\0' pass '\0' webpass '\0' id '\0' - the trailing NULs are part of
     * the length so the board gets four C strings back, empty ones included. */
    const size_t ls = strlen(ssid), lp = strlen(pass), lw = strlen(webpass),
                 li = strlen(rid);
    const size_t total = ls + 1 + lp + 1 + lw + 1 + li + 1;
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
    memcpy(payload + o, rid, li + 1);
    o += li + 1;

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

/* ------------------------------------------------------------------------ */
/* The system tray: once the agent is streaming there is nothing to watch in */
/* the console, so it folds into a tray icon and gets out of the way.  This  */
/* is all local Windows UI - none of it touches the link, the capture, or    */
/* the framing; it only shows and hides the console and re-arms the wizard.  */
/* ------------------------------------------------------------------------ */

#define WM_KVM_TRAY (WM_APP + 1) /* the tray icon's notification callback     */
#define IDM_SHOW    1            /* "Show console"                            */
#define IDM_RECONFIG 2           /* "Reconfigure Wi-Fi / password"           */
#define IDM_QUIT    3            /* "Quit"                                    */

static HWND            g_console    = NULL;  /* this process's console window  */
static HWND            g_tray_wnd   = NULL;  /* the hidden message-only window */
static int             g_tray_added = 0;     /* is the icon in the tray?       */
static int             g_console_up = 1;     /* is the console currently shown?*/
static int             g_keep_console = 0;   /* --console: never auto-hide     */
static NOTIFYICONDATAA g_nid;

/* Show or hide the console window, remembering the state so the double-click
 * toggle and the menu agree on what "showing" means. */
static void console_show(int show)
{
    if (g_console == NULL) {
        return;
    }
    ShowWindow(g_console, show ? SW_SHOW : SW_HIDE);
    if (show) {
        SetForegroundWindow(g_console);
    }
    g_console_up = show ? 1 : 0;
}

/* Put the icon in the tray, once.  Harmless to call again - it is a no-op once
 * added, which is what lets "hide the console" call it unconditionally. */
static void tray_add(void)
{
    if (g_tray_added || g_tray_wnd == NULL) {
        return;
    }
    memset(&g_nid, 0, sizeof(g_nid));
    /* The first, smallest layout: the only one the 98 shell accepts, and
     * every later one still takes it. */
    g_nid.cbSize = NOTIFYICONDATAA_V1_SIZE;
    g_nid.hWnd = g_tray_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_KVM_TRAY;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    snprintf(g_nid.szTip, sizeof(g_nid.szTip), "ArgonOS KVM");
    if (Shell_NotifyIconA(NIM_ADD, &g_nid)) {
        g_tray_added = 1;
    }
}

static void tray_remove(void)
{
    if (g_tray_added) {
        Shell_NotifyIconA(NIM_DELETE, &g_nid);
        g_tray_added = 0;
    }
}

/* Pop the right-click menu at the cursor.  The SetForegroundWindow / WM_NULL
 * dance is the documented way to make the menu dismiss when the user clicks
 * away from it. */
static void tray_menu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    if (menu == NULL) {
        return;
    }
    AppendMenuA(menu, MF_STRING, IDM_SHOW, "Show console");
    AppendMenuA(menu, MF_STRING, IDM_RECONFIG, "Reconfigure Wi-Fi / password");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, IDM_QUIT, "Quit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static LRESULT CALLBACK tray_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KVM_TRAY:
        switch (LOWORD(lp)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            tray_menu(hwnd);
            break;
        case WM_LBUTTONDBLCLK:
            console_show(!g_console_up);
            break;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SHOW:
            console_show(1);
            break;
        case IDM_RECONFIG:
            /* Bring the console back so the typed answers are visible, then
             * re-arm the wizard: g_force_setup makes the next CFG_STATE run it
             * regardless of the board's reported state, and clearing g_asked
             * lets that fire again (it is the once-only guard the first run
             * set).  The board sends CFG_STATE periodically, so the wizard
             * comes up on its own within a few moments and reads from the
             * now-visible console. */
            console_show(1);
            g_force_setup = 1;
            g_asked = 0;
            printf("\n(reconfiguring: answer the prompts below)\n");
            fflush(stdout);
            break;
        case IDM_QUIT:
            s_stop = 1;
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        tray_remove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* Register the window class and create the hidden message-only window that
 * owns the tray icon.  Message-only (HWND_MESSAGE as parent) is enough: it is
 * never shown, it only needs to receive the tray callback and WM_COMMAND. */
static int tray_window_create(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = tray_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "ArgonKvmTray";
    if (RegisterClassA(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 0;
    }
    g_tray_wnd = CreateWindowA("ArgonKvmTray", "ArgonOS KVM", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, NULL, wc.hInstance, NULL);
    return g_tray_wnd != NULL;
}

/* Drain the hidden window's message queue without blocking - called once per
 * capture iteration so the tray stays responsive while the loop keeps
 * streaming. */
static void tray_pump(void)
{
    MSG m;
    while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
}

/* Fold into the tray: make sure the icon exists, drop a hint, and hide the
 * console.  With --console this is a no-op so a debugger keeps the window. */
static void hide_to_tray(void)
{
    if (g_keep_console || !g_console_up || g_tray_wnd == NULL) {
        return;
    }
    tray_add();
    printf("(minimising to the tray; right-click the tray icon for options)\n");
    fflush(stdout);
    console_show(0);
}

int main(int argc, char **argv)
{
    /* Without this the process is DPI-virtualised: on a display scaled past
     * 100% (125% here) GetSystemMetrics and BitBlt report and capture the
     * smaller logical screen, so the picture loses its bottom-right and the
     * composited cursor lands short of where Windows actually drew it.  Make
     * the agent see physical pixels, so one pixel is one pixel end to end. */
    {
        typedef BOOL(WINAPI * pfn_dpi)(void);
        pfn_dpi dpi = (pfn_dpi)(void *)GetProcAddress(
            GetModuleHandleA("user32.dll"), "SetProcessDPIAware");
        if (dpi != NULL) { /* Vista on; before it there is no scaling to undo */
            dpi();
        }
    }

    const char *tcp = NULL;
    int         fps = 15;
    int         tile = 64;
    int         list_only = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--hid") == 0) {
            g_force_hid = 1;
        } else if (strcmp(argv[i], "--trace") == 0) {
            g_trace = 1;
        } else if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[i], "--setup") == 0) {
            /* Force the wizard the next time the board reports its config
             * state, even if it is already on a network - for changing the
             * Wi-Fi or the web password after first run. */
            g_force_setup = 1;
        } else if (strcmp(argv[i], "--console") == 0) {
            /* Keep the console window visible for the whole run instead of
             * folding into the tray once streaming - for debugging. */
            g_keep_console = 1;
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
                   "[--fps N] [--tile N] [--list] [--setup] [--console]\n");
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
    link.hid = INVALID_HANDLE_VALUE;

    if (list_only) {
        if (winusb_load()) {
            find_winusb_device(&link, 1);
        }
        find_hid_device(&link, 1);
        return 0;
    }

    /* Remember where the link lives; the reconnect loop opens it (and re-opens
     * it after every board reboot) through link_open, which reads this. */
    g_tcp = tcp;

    /* If the drive carries a RELAY.TXT, the dongle is reachable from anywhere
     * through that host; remember it so the "Ready" line can point the phone at
     * the public address as well as the LAN one. */
    load_relay_host();

    grab_t g;
    if (!grab_open(&g, tile)) {
        fprintf(stderr, "cannot capture the screen\n");
        return 3;
    }
    printf("screen %dx%d, %d px tiles, up to %d frames a second\n", g.w, g.h,
           tile, fps);
    printf("Ctrl+C to stop\n");

    SetConsoleCtrlHandler(on_break, TRUE);

    /* The tray lives on a hidden message-only window; create it before the
     * loop so the icon and its menu are ready the moment we fold away.  If it
     * cannot be made we simply never hide the console - the agent still runs. */
    {
        /* Windows 2000 on; on 98 there is no console window to fold into the
         * tray, and the console just stays. */
        typedef HWND(WINAPI * pfn_gcw)(void);
        pfn_gcw gcw = (pfn_gcw)(void *)GetProcAddress(
            GetModuleHandleA("kernel32.dll"), "GetConsoleWindow");
        g_console = (gcw != NULL) ? gcw() : NULL;
    }
    tray_window_create();

    uint8_t *scratch = (uint8_t *)malloc((size_t)tile * tile * 3);
    uint8_t  in[1024];
    uint8_t  msg[AG_VID_HDR + AG_VID_MAX_PAYLOAD];
    size_t   have = 0;
    if (scratch == NULL) {
        return 3;
    }

    /* HELLO's payload is the screen's size; it never changes, so build it once
     * here and resend it on every (re)connection - the phone cannot size its
     * canvas until it has one. */
    uint8_t hello[8];
    hello[0] = 1;
    hello[1] = 0;
    hello[2] = (uint8_t)(g.w & 0xFF);
    hello[3] = (uint8_t)(g.w >> 8);
    hello[4] = (uint8_t)(g.h & 0xFF);
    hello[5] = (uint8_t)(g.h >> 8);
    hello[6] = (uint8_t)(tile & 0xFF);
    hello[7] = (uint8_t)(tile >> 8);

    /* Per-connection streaming state, reset each time we (re)connect below. */
    int      want_all = 1;
    int      want_hello = 0;
    uint32_t frames = 0, sent = 0;
    DWORD    next = GetTickCount();
    const DWORD period = (DWORD)(1000 / fps);
    DWORD    report = GetTickCount() + 5000;

    int connected = 0;   /* is the link up right now?                         */
    int waiting_said = 0; /* have we already printed the "waiting" line?      */

    /*
     * The outer loop survives the board rebooting.  When the USB re-enumerates
     * the WinUSB handle dies, link_read returns -1 (or a write fails), and
     * instead of exiting we close the dead handle, come back up here, and
     * link_open the device anew - looping until the user actually quits
     * (Ctrl+C or the tray's Quit, both of which set s_stop).
     *
     * The wizard state (g_configured, g_asked) is deliberately NOT touched
     * across reconnects: a board that was already set up must not re-run the
     * wizard when it reappears.  Only the streaming state above is reset.
     */
    while (!s_stop) {
        if (!connected) {
            if (!link_open(&link)) {
                /* No board yet - booting, unplugged, or not here at all, all
                 * the same to us.  Say it once, keep the tray responsive, and
                 * wait a beat; do not busy-spin on the enumerator. */
                if (!waiting_said) {
                    printf("waiting for the dongle...\n");
                    fflush(stdout);
                    waiting_said = 1;
                }
                tray_pump();
                Sleep(700);
                continue;
            }

            connected = 1;
            waiting_said = 0;
            if (g_tcp != NULL) {
                printf("connected to %s\n", g_tcp);
            } else if (link.hid != INVALID_HANDLE_VALUE) {
                printf("connected over HID (no driver needed; slower than "
                       "WinUSB)\n");
            } else {
                printf("connected over WinUSB (IN 0x%02X, OUT 0x%02X)\n",
                       link.pipe_in, link.pipe_out);
            }
            fflush(stdout);

            /* A fresh link is a fresh stream: forget any half-read frame, hand
             * the next client a full picture, and clear the counters.  The
             * wizard flags survive (see the outer-loop comment). */
            have = 0;
            want_all = 1;
            want_hello = 0;
            frames = 0;
            sent = 0;
            next = GetTickCount();
            report = GetTickCount() + 5000;

            send_msg(&link, AG_VID_HELLO, hello, sizeof(hello));
        }

        /* The inner loop streams until the board goes away or the user quits.
         * On link loss it closes the link, marks us disconnected, and breaks
         * back to the outer loop to reconnect - it never exits the program. */
        while (!s_stop) {
            /* Keep the tray icon and its menu live without blocking the loop. */
            tray_pump();

            /* Anything from the phone, by way of the board. */
            const int n = link_read(&link, in, sizeof(in));
            if (n < 0) {
                /* link_read maps a WinUSB read timeout (nothing arrived) to 0,
                 * so -1 is a genuine failure: the board rebooted and the handle
                 * died.  Drop it and go reconnect rather than exiting. */
                link_close(&link);
                connected = 0;
                printf("the board went away - waiting for it to come back\n");
                fflush(stdout);
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
                         * needs the screen's size as much as it needs the
                         * pixels: it never saw the HELLO sent when this program
                         * started, and without one there is nothing to size a
                         * canvas from.
                         */
                        want_all = 1;
                        want_hello = 1;
                    } else if (type == AG_VID_CLIP && len > 0) {
                        set_clipboard(msg + off + AG_VID_HDR, len);
                    } else if (type == AG_VID_TYPE && len > 0) {
                        type_unicode(msg + off + AG_VID_HDR, len);
                    } else if (type == AG_VID_CFG_STATE && len >= 1) {
                        /*
                         * Where the board is on getting onto a network.  The
                         * first byte is the state; the rest is the IP as ASCII,
                         * which the board NUL-terminates - but we never trust a
                         * wire length, so bound the string to the frame and
                         * plant our own NUL.
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

                        /* Newer boards follow the IP with "id\0" and a '1' when
                         * the relay refused that id as taken by another board. */
                        char bid[48] = "";
                        int  busy = 0;
                        {
                            const char    *q = (const char *)(p + 1);
                            const uint32_t tot = len - 1;
                            uint32_t       i = 0;
                            while (i < tot && q[i] != '\0') {
                                i++;
                            }
                            if (i + 1 < tot) {
                                const char    *r = q + i + 1;
                                const uint32_t rl = tot - i - 1;
                                uint32_t       j = 0;
                                while (j < rl && r[j] != '\0') {
                                    j++;
                                }
                                const uint32_t cp =
                                    (j < sizeof(bid) - 1) ? j : sizeof(bid) - 1;
                                memcpy(bid, r, cp);
                                bid[cp] = '\0';
                                busy = (j + 1 < rl) && r[j + 1] == '1';
                            }
                        }
                        if (busy && !g_busy_shown) {
                            /* The owner must see this even with the console
                             * folded into the tray: bring it back. */
                            console_show(1);
                            printf("\n!! The id '%s' is already in use on the relay"
                                   " by another KVM.\n"
                                   "   From outside, that link reaches the other"
                                   " dongle, not this one.\n"
                                   "   Choose another id: run kvmagent --setup, or"
                                   " tray icon > Reconfigure.\n",
                                   bid);
                            fflush(stdout);
                            g_busy_shown = 1;
                            g_link_id[0] = '\0';
                        } else if (!busy && g_busy_shown && state == 2) {
                            printf("relay accepted the id '%s'\n", bid);
                            g_busy_shown = 0;
                        }
                        /* The link from anywhere, printed again whenever the id
                         * changes (a Reconfigure that picked a new one). */
                        if (g_configured && state == 2 && !busy &&
                            g_relay_host[0] != '\0' && bid[0] != '\0' &&
                            strcmp(bid, g_link_id) != 0) {
                            printf("From anywhere (any network):  "
                                   "https://%s/?id=%s\n", g_relay_host, bid);
                            fflush(stdout);
                            (void)snprintf(g_link_id, sizeof(g_link_id), "%s",
                                           bid);
                        }

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
                            printf("\nReady. On the same network:  http://%s/\n",
                                   ip);
                            if (g_relay_host[0] != '\0' && bid[0] != '\0' &&
                                !busy) {
                                printf("From anywhere (any network):  "
                                       "https://%s/?id=%s\n", g_relay_host, bid);
                                (void)snprintf(g_link_id, sizeof(g_link_id),
                                               "%s", bid);
                            }
                            fflush(stdout);
                            g_configured = 1;
                            /* Streaming from here on; nothing more to read in
                             * the console, so fold it into the tray (unless
                             * --console).  Any later Reconfigure brings it back
                             * by itself. */
                            hide_to_tray();
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
                        /* A write failed: the board vanished mid-frame.  Same
                         * answer as a dead read - drop the link and reconnect,
                         * do not quit. */
                        link_close(&link);
                        connected = 0;
                        printf("the board went away - waiting for it to come "
                               "back\n");
                        fflush(stdout);
                        break;
                    }
                    sent++;
                }
                if (!connected || s_stop) {
                    break;
                }
            }
            if (!connected) {
                break; /* back to the outer loop to reconnect */
            }
            want_all = 0;

            if ((int)(GetTickCount() - report) >= 0) {
                report = GetTickCount() + 5000;
                printf("%u frames, %u tiles sent\n", frames, sent);
                fflush(stdout);
            }
        }
    }

    /* Bring the console back for the parting line and take the icon down.
     * (A Quit from the menu already removed the icon via WM_DESTROY; tray_remove
     * is a no-op the second time.) */
    console_show(1);
    tray_remove();
    if (g_tray_wnd != NULL) {
        DestroyWindow(g_tray_wnd);
        g_tray_wnd = NULL;
    }

    /* Say goodbye only if the link is still up (a reboot may have taken it);
     * then close whatever is open.  WSACleanup balances the WSAStartup that
     * open_tcp did on the tcp path. */
    if (connected) {
        send_msg(&link, AG_VID_BYE, NULL, 0);
    }
    link_close(&link);
    if (g_tcp != NULL) {
        WSACleanup();
    }
    free(scratch);
    grab_close(&g);
    printf("stopped: %u frames, %u tiles\n", frames, sent);
    return 0;
}
