/*
 * ArgonOS - the CYD as a monitor, and nothing else.
 *
 * The S3 next to this board has no screen; this one is little else but a
 * screen.  A wire joins them, the S3 sends whole frames down it, and this
 * firmware puts them on the glass.  That is the entire program.
 *
 * Why it is not an application on the operating system, which is where it
 * lived until now: a 320x240 frame is 150 KB, and under ArgonOS this board can
 * give one application about 123 KB in the shapes it can use.  Not for want of
 * memory - 259 KB is free - but because a process heap is a single allocation
 * and cannot exceed the largest free block (108 KB here), and because much of
 * what is left is instruction RAM that cannot be addressed a byte at a time.
 * That was measured, at length, and it is the whole reason this file exists.
 *
 * A frame that cannot be assembled whole reaches the glass as stripes, and the
 * stripes were the complaint that started all of this.
 *
 * Nothing is lost with the operating system any more.  The sound came back
 * first - one stream of samples into the converter on GPIO 26, where this
 * board's amplifier is - and then the console, because a monitor that shows
 * only pictures is not a monitor: when the application on the far machine
 * stops, what should appear here is its shell, and for a long while what
 * appeared instead was the last frame the dead game drew.
 *
 * The wire protocol is REMDISP own:
 *
 *   A5 5A F len16   frame:2 slice:2 slices:2 width:2 y:2 rows:2  pixels...
 *   A5 5A X len16   the same twelve, and one byte a pixel instead of two
 *   A5 5A L len16   gen:2 count:2 pad:8  sum:1 pad...   256 x RGB565
 *   A5 5A A len16   rate:4 samples:2 seq:2 pad:4 sum:1 pad...     samples...
 *   A5 5A R len16   row:1 count:1  (ch,attr) x count      + sum   console row
 *   A5 5A C len16   col:1 row:1 ch:1 attr:1 visible:1     + sum   the caret
 *   A5 5A R ...     going the other way: send slice N of frame M again
 *
 * Picture and sound are the same packet with a different body - thirty-two
 * bytes of header carrying their own sum, then the payload - so one piece of
 * framing reads both and they interleave on the wire in whatever order the
 * sender queued them.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "font8x8.h"

#include "driver/dac_continuous.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "monitor";

/* ---- the board ---------------------------------------------------------- */
/*
 * Found by looking rather than reading, and the same numbers ILI9341.SYS uses.
 * A wrong guess here drives a pin belonging to something else.
 */
#define PIN_SCK   14
#define PIN_MOSI  13
#define PIN_MISO  12
#define PIN_CS    15
#define PIN_DC     2
#define PIN_BL    27
#define LCD_HZ    (80 * 1000 * 1000)

#define LCD_W    320
#define LCD_H    240
#define MADCTL   0x40 /* landscape, RGB order - this panel has no BGR bit */

/* The wire.  RX is the picture, TX is what this end asks for. */
#define LINK_UART UART_NUM_1
#define PIN_RX    35
#define PIN_TX    21
#define LINK_BAUD 4000000
#define RX_RING   (32 * 1024)

#define FR_MAGIC0 0xA5u
#define FR_MAGIC1 0x5Au
#define FR_RAW    0x46u /* F */

/*
 * A raw slice is one packet now, and the packet is what the sender's memory
 * already looks like: thirty-two bytes of header and then the pixels, laid out
 * so the whole thing can leave by DMA in a single transfer with no processor
 * touching it.  That is why the sum is *in* the header instead of after the
 * payload - a trailing byte would have to be written by somebody, and the only
 * somebody available after the pixels is the processor this arrangement exists
 * to keep out of the way.
 *
 *   0   two magic bytes, the op, and a sixteen-bit length  (five bytes)
 *   5   frame, slice, slices, width, y, rows               (twelve)
 *   17  the sum, over the twelve above and the pixels
 *   18  padding to the alignment the sender's DMA wants
 *   32  the pixels
 *
 * The length counts everything after the five, padding included, so a reader
 * that does not care about any of this can still skip the frame.
 */
#define PKT_HDR   32u
#define PKT_GEOM  12u  /* bytes of geometry, from payload offset 0 */
#define PKT_SUM   12u  /* where the sum sits, in payload offsets   */
#define PKT_PIX   (PKT_HDR - 5u) /* payload offset of the body      */
#define FR_AUD    0x41u /* A */
#define FR_IDX8   0x58u /* X - a slice as one byte a pixel */
#define FR_PAL    0x4cu /* L - the palette those bytes index */
#define FR_ROW    0x52u /* R - one row of console cells */
#define FR_CUR    0x43u /* C - the caret, and the cell under it */
#define FR_RESEND 0x52u /* R - the same letter, going the other way */
#define RAW_ROWS  8u
#define SLICES    (LCD_H / RAW_ROWS) /* 30 */

/*
 * The colours a slice of indices names.
 *
 * Kept byte-swapped, the way the frame is: the controller wants the high byte
 * first and the far end sends little-endian, so the swap happens once here
 * rather than 76 800 times a frame.
 *
 * A frame of this game has about 150 different colours, so one byte a pixel
 * carries it with nothing lost - which is the whole reason this exists.  The
 * far end sends the table whenever it changes and again every sixty-four
 * frames, so a receiver plugged in late is right within half a minute.
 */
static uint16_t s_pal[256];
static uint32_t s_pal_gen;
static uint32_t s_stat_pal;
static uint32_t s_slice_bpp = 2; /* set by the reader from the opcode */

static uint16_t *s_frame;      /* 150 KB, one piece, because it can be */
static uint32_t  s_have;       /* bitmap of slices in hand             */
static uint32_t  s_want = (1u << SLICES) - 1u;
static uint32_t  s_asked;
static int32_t   s_frame_no = -1;

static uint32_t s_stat_frames, s_stat_slices, s_stat_partial, s_stat_asked;
static uint32_t s_stat_bad; /* slices whose sum did not agree */
/*
 * Bytes off the wire, and how many of them were not part of anything.
 *
 * Without these two a silent screen has three explanations that look identical
 * from here - nothing is being sent, something is being sent and the line is
 * mangling it, or it arrives and this program does not understand it - and a
 * morning went into telling them apart by other means.  `rx` says whether the
 * wire carries anything; `stray` says whether what it carries makes sense.
 */
static uint32_t s_stat_rx, s_stat_stray;
static uint64_t s_stat_paint_us;

static spi_device_handle_t s_spi;

/* ---- the sound ----------------------------------------------------------
 *
 * A sound packet is the picture packet with samples where the pixels are:
 * twelve bytes of what-this-is at offset five - a rate, how many samples are
 * real, and a sequence number - the sum at seventeen, and unsigned eight-bit
 * mono from thirty-two on.  The sender rounds the count on the wire up to a
 * cache line because its DMA reads whole ones, so the last few bytes of a
 * packet may be padding; the header says how many to keep.
 *
 * The reader drops them in a ring and goes straight back to the wire.  Nothing
 * in the reading path touches the converter: a write to it waits for a
 * descriptor to be played, which is a tenth of a second, and a tenth of a
 * second not reading the UART is thirty kilobytes of picture on the floor.
 * That was measured, under the operating system, and it is why the player is a
 * task of its own.
 *
 * Eight thousand samples is 372 ms of cushion.  It is there for the gap
 * between two frames of picture, not for the wire: the sender interleaves a
 * block of sound between its slices now, so sound arrives about every 23 ms
 * whatever the picture is doing.
 */
#define AUD_RING   8192u    /* a power of two: the indices are masked, not wrapped */
#define AUD_PRIME  2048u    /* 93 ms before the first sample is played */
#define AUD_CHUNK  1024u

/*
 * On the heap, and taken after the frame - not because nine kilobytes are
 * precious but because of where they would otherwise sit.  A static lands in
 * DRAM at link time, inside the one 160 KB region the 150 KB frame has to come
 * out of in a single piece; nine kilobytes of .bss there took the largest free
 * block to 147456 and the frame could not be allocated at all.  The board came
 * up, said "frame buffer 153600 bytes: NO" and returned from app_main, which
 * from the outside looks exactly like a board that is not running.
 */
static uint8_t *s_aring;
static uint8_t *s_aout;
/*
 * One producer, one consumer, no lock.  The reader task owns s_aw and the
 * player owns s_ar; each of them only ever reads the other's, and both run
 * free rather than wrapping, so a torn read is impossible on a 32-bit load and
 * the difference between them is the fill however many times they have wrapped.
 */
static volatile uint32_t s_aw, s_ar;
static volatile uint32_t s_arate;
static uint32_t s_stat_asamp, s_stat_adrop, s_stat_abad, s_stat_astarve;

static uint32_t aud_used(void)
{
    return s_aw - s_ar;
}

/* One sound packet into the ring.  Called from the reader and nowhere else. */
static void aud_in(const uint8_t *p, uint32_t len)
{
    if (s_aring == NULL || len < PKT_PIX) {
        return;
    }
    const uint32_t rate = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    const uint32_t want = (uint32_t)p[4] | ((uint32_t)p[5] << 8);
    const uint32_t body = len - PKT_PIX;
    if (want == 0u || want > body || rate < 8000u || rate > 48000u) {
        return;
    }

    /* The sum, over the twelve bytes of header and the body as it arrived,
     * padding included - the sender computes it over exactly the same run. */
    uint8_t got = 0;
    for (uint32_t i = 0; i < PKT_GEOM; i++) {
        got = (uint8_t)(got ^ p[i]);
    }
    const uint8_t *src = p + PKT_PIX;
    for (uint32_t i = 0; i < body; i++) {
        got = (uint8_t)(got ^ src[i]);
    }
    if (got != p[PKT_SUM]) {
        s_stat_abad++;
        return;
    }

    s_arate = rate;

    /* One slot is left empty so that full and empty are different states. */
    const uint32_t room = AUD_RING - 1u - aud_used();
    uint32_t       take = want;
    if (take > room) {
        s_stat_adrop += take - room;
        take = room;
    }
    for (uint32_t i = 0; i < take; i++) {
        s_aring[(s_aw + i) & (AUD_RING - 1u)] = src[i];
    }
    s_aw += take;
    s_stat_asamp += take;
}

/*
 * The converter, fed from the ring and paced by it.
 *
 * dac_continuous_write blocks until a descriptor has been played, which is
 * exactly the clock this wants: the task spends its life asleep in there and
 * the sound comes out at the rate the peripheral runs, not at the rate the
 * wire happens to deliver.
 *
 * Silence is 128, not 0 - the converter swings from ground to the supply and
 * the middle is what the amplifier is coupled around.  It matters more than it
 * looks: a starved DMA chain replays whatever was left in it, which is not a
 * gap in the sound but a buzz at that buffer's own frequency, and writing
 * silence is what keeps the chain honest while nothing is arriving.
 */
static void player(void *arg)
{
    uint8_t *const          out = s_aout;
    dac_continuous_handle_t dac = NULL;
    uint32_t                open_at = 0;
    bool                    primed = false;

    (void)arg;
    for (;;) {
        const uint32_t rate = s_arate;
        if (rate == 0u) {
            vTaskDelay(pdMS_TO_TICKS(50)); /* nothing has ever arrived */
            continue;
        }
        if (dac != NULL && rate != open_at) {
            (void)dac_continuous_disable(dac);
            (void)dac_continuous_del_channels(dac);
            dac = NULL;
        }
        if (dac == NULL) {
            /* Eight descriptors of a quarter kilobyte: 93 ms of chain, and one
             * descriptor - 11.6 ms - is the shortest a write can wait. */
            const dac_continuous_config_t cfg = {
                .chan_mask = DAC_CHANNEL_MASK_CH1, /* GPIO 26, the amplifier */
                .desc_num = 8,
                .buf_size = 256,
                .freq_hz = rate,
                .offset = 0,
                .clk_src = DAC_DIGI_CLK_SRC_DEFAULT,
                .chan_mode = DAC_CHANNEL_MODE_SIMUL,
            };
            if (dac_continuous_new_channels(&cfg, &dac) != ESP_OK ||
                dac_continuous_enable(dac) != ESP_OK) {
                ESP_LOGW(TAG, "no converter at %u Hz; the sound stays off",
                         (unsigned)rate);
                if (dac != NULL) {
                    (void)dac_continuous_del_channels(dac);
                    dac = NULL;
                }
                s_arate = 0;
                continue;
            }
            open_at = rate;
            primed = false;
            ESP_LOGI(TAG, "sound out on gpio26 at %u Hz", (unsigned)rate);
        }

        const uint32_t used = aud_used();
        if (!primed) {
            primed = used >= AUD_PRIME;
        } else if (used == 0u) {
            primed = false;
            s_stat_astarve++;
        }

        uint32_t n;
        if (primed) {
            n = (used > AUD_CHUNK) ? AUD_CHUNK : used;
            for (uint32_t i = 0; i < n; i++) {
                out[i] = s_aring[(s_ar + i) & (AUD_RING - 1u)];
            }
        } else {
            n = 256u;
            memset(out, 128, n);
        }

        size_t loaded = 0;
        if (dac_continuous_write(dac, out, n, &loaded, 200) == ESP_OK &&
            primed) {
            s_ar += (uint32_t)loaded;
        }
    }
}

/* ---- the panel ---------------------------------------------------------- */

static void lcd_cmd(uint8_t c)
{
    spi_transaction_t t = {.length = 8, .tx_buffer = &c};
    gpio_set_level(PIN_DC, 0);
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void lcd_data(const void *p, size_t n)
{
    if (n == 0) {
        return;
    }
    gpio_set_level(PIN_DC, 1);
    /*
     * In chunks: one transaction is bounded by the DMA descriptor list the bus
     * was set up with, and a whole frame is 150 KB.  Thirty-two at a time keeps
     * the bus busy without asking for a descriptor table nobody has.
     */
    const uint8_t *b = (const uint8_t *)p;
    while (n > 0) {
        const size_t chunk = (n > 32768u) ? 32768u : n;
        spi_transaction_t t = {.length = chunk * 8, .tx_buffer = b};
        ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
        b += chunk;
        n -= chunk;
    }
}

static void lcd_cmd_data(uint8_t c, const void *p, size_t n)
{
    lcd_cmd(c);
    lcd_data(p, n);
}

static void lcd_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    const uint8_t ca[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0,
                           (uint8_t)(x1 >> 8), (uint8_t)x1};
    const uint8_t pa[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0,
                           (uint8_t)(y1 >> 8), (uint8_t)y1};
    lcd_cmd_data(0x2a, ca, sizeof(ca));
    lcd_cmd_data(0x2b, pa, sizeof(pa));
    lcd_cmd(0x2c);
}

static void panel_init(void)
{
    static const uint8_t pwctr1[] = {0x23};
    static const uint8_t pwctr2[] = {0x10};
    static const uint8_t vmctr1[] = {0x3e, 0x28};
    static const uint8_t vmctr2[] = {0x86};
    static const uint8_t pixfmt[] = {0x55};
    static const uint8_t frmctr[] = {0x00, 0x18};
    static const uint8_t dfunctr[] = {0x08, 0x82, 0x27};
    static const uint8_t madctl[] = {MADCTL};
    static const uint8_t vscrdef[] = {0x00, 0x00, 0x01, 0x40, 0x00, 0x00};
    static const uint8_t vscrsadd[] = {0x00, 0x00};
    static uint16_t blank[LCD_W];

    gpio_config_t o = {
        .pin_bit_mask = (1ull << PIN_DC) | (1ull << PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&o));
    gpio_set_level(PIN_BL, 0);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32768,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_HZ,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 4,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));

    lcd_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd_data(0xc0, pwctr1, sizeof(pwctr1));
    lcd_cmd_data(0xc1, pwctr2, sizeof(pwctr2));
    lcd_cmd_data(0xc5, vmctr1, sizeof(vmctr1));
    lcd_cmd_data(0xc7, vmctr2, sizeof(vmctr2));
    lcd_cmd_data(0x36, madctl, sizeof(madctl));
    lcd_cmd_data(0x3a, pixfmt, sizeof(pixfmt));
    lcd_cmd_data(0xb1, frmctr, sizeof(frmctr));
    lcd_cmd_data(0xb6, dfunctr, sizeof(dfunctr));
    lcd_cmd(0x13);
    lcd_cmd(0x20);
    lcd_cmd_data(0x33, vscrdef, sizeof(vscrdef));
    lcd_cmd_data(0x37, vscrsadd, sizeof(vscrsadd));
    lcd_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Black before the backlight: whatever the panel held is not ours. */
    memset(blank, 0, sizeof(blank));
    lcd_window(0, 0, LCD_W - 1, LCD_H - 1);
    for (int y = 0; y < LCD_H; y++) {
        lcd_data(blank, sizeof(blank));
    }
    gpio_set_level(PIN_BL, 1);
}

/*
 * The whole frame, in one window and one run of pixels.
 *
 * This is the point of the firmware.  The bytes are already here and already
 * complete, so what an eye sees is the SPI transfer and nothing else: 150 KB
 * at 80 MHz is about fifteen milliseconds, which is a frame changing rather
 * than a frame being drawn.
 */
static void paint(void)
{
    const int64_t t0 = esp_timer_get_time();
    lcd_window(0, 0, LCD_W - 1, LCD_H - 1);
    lcd_data(s_frame, (size_t)LCD_W * LCD_H * sizeof(uint16_t));
    s_stat_paint_us += (uint64_t)(esp_timer_get_time() - t0);
    s_stat_frames++;
}

/* ---- the wire ----------------------------------------------------------- */

static void ask_again(uint32_t frame, uint32_t slice)
{
    const uint8_t req[5] = {
        (uint8_t)FR_RESEND, (uint8_t)frame, (uint8_t)(frame >> 8),
        (uint8_t)slice,     (uint8_t)(slice >> 8),
    };
    uart_write_bytes(LINK_UART, (const char *)req, sizeof(req));
    s_stat_asked++;
}

/* ---- the console --------------------------------------------------------
 *
 * Forty by twenty-five cells of eight by eight pixels, which is what REMDISP
 * sends and what fits: 40 x 8 = 320 exactly, and 25 rows of 8 is 200 of the
 * 240 available.  The attribute byte is a background nibble and a foreground
 * nibble and has been since 1981; the sixteen colours are the ones every other
 * panel driver in this project uses.
 *
 * The page is kept here, not just painted, for one reason: the far end sends a
 * row only when that row changes, so when the glass has been showing a picture
 * and text starts arriving there is nothing to draw except what is remembered.
 * Coming back the other way needs nothing - a picture repaints the whole glass
 * in one go.
 *
 * The scratch a row is built in is the frame buffer itself.  It is 150 KB
 * standing idle whenever text is on the screen, and the alternative - five
 * kilobytes of .bss - is the thing that broke the frame allocation once
 * already: static memory sits inside the one region the frame has to come out
 * of in a single piece.  A picture cannot be half-shown either way, because a
 * frame reaches the glass only when all thirty slices have arrived.
 */
#define CELL_W 8u
#define CELL_H 8u
#define TCOLS  (LCD_W / CELL_W) /* 40 */
#define TROWS  25u              /* what REMDISP sends */

static uint8_t  s_cell_ch[TROWS][TCOLS];
static uint8_t  s_cell_at[TROWS][TCOLS];
static uint8_t  s_cur_col, s_cur_row;
static bool     s_cur_on;
static bool     s_text;          /* the glass is showing the console */
static uint32_t s_stat_rows;

/* Written high byte first by the panel, so they are kept swapped, exactly as
 * slice_in keeps the pixels. */
static const uint16_t k_cga[16] = {
    0x0000, 0x1500, 0x4005, 0x5505, 0x00a8, 0x15a8, 0xa0aa, 0x55ad,
    0x9552, 0x9f52, 0xea57, 0xff57, 0x95fa, 0x9ffa, 0xeaff, 0xffff,
};

static void text_paint_row(uint32_t row)
{
    if (row >= TROWS || s_frame == NULL) {
        return;
    }
    uint16_t *const px = s_frame;
    for (uint32_t c = 0; c < TCOLS; c++) {
        uint8_t fg = (uint8_t)(s_cell_at[row][c] & 0x0fu);
        uint8_t bg = (uint8_t)((s_cell_at[row][c] >> 4) & 0x0fu);
        if (s_cur_on && row == s_cur_row && c == s_cur_col) {
            const uint8_t t = fg;
            fg = bg;
            bg = t;
        }
        const uint16_t f = k_cga[fg];
        const uint16_t b = k_cga[bg];
        const uint8_t *const g = k_font8x8[s_cell_ch[row][c]];
        for (uint32_t y = 0; y < CELL_H; y++) {
            uint16_t *const o = px + y * LCD_W + c * CELL_W;
            const uint8_t bits = g[y];
            /* Bit 0 is the leftmost pixel, as in the font this came from. */
            for (uint32_t x = 0; x < CELL_W; x++) {
                o[x] = (bits & (1u << x)) ? f : b;
            }
        }
    }
    lcd_window(0, (uint16_t)(row * CELL_H), LCD_W - 1,
               (uint16_t)(row * CELL_H + CELL_H - 1));
    lcd_data(px, (size_t)LCD_W * CELL_H * sizeof(uint16_t));
}

/* The console is taking the glass from a picture: black out what the text does
 * not cover, then lay the whole remembered page down. */
static void text_take_glass(void)
{
    if (s_text) {
        return;
    }
    s_text = true;
    if (s_frame != NULL) {
        memset(s_frame, 0, (size_t)LCD_W * CELL_H * sizeof(uint16_t));
        lcd_window(0, (uint16_t)(TROWS * CELL_H), LCD_W - 1, LCD_H - 1);
        for (uint32_t y = TROWS * CELL_H; y < LCD_H; y += CELL_H) {
            lcd_data(s_frame, (size_t)LCD_W * CELL_H * sizeof(uint16_t));
        }
    }
    for (uint32_t r = 0; r < TROWS; r++) {
        text_paint_row(r);
    }
}

static void text_row_in(const uint8_t *p, uint32_t len)
{
    if (len < 2u) {
        return;
    }
    const uint32_t row = p[0];
    const uint32_t count = p[1];
    if (row >= TROWS || count > TCOLS || len < 2u + count * 2u) {
        return;
    }
    for (uint32_t c = 0; c < count; c++) {
        s_cell_ch[row][c] = p[2u + c * 2u];
        s_cell_at[row][c] = p[3u + c * 2u];
    }
    for (uint32_t c = count; c < TCOLS; c++) {
        s_cell_ch[row][c] = ' ';
        s_cell_at[row][c] = 0x07u;
    }
    s_stat_rows++;
    text_take_glass();
    text_paint_row(row);
}

static void text_cursor_in(const uint8_t *p, uint32_t len)
{
    if (len < 5u || p[0] >= TCOLS || p[1] >= TROWS) {
        return;
    }
    const uint32_t was_row = s_cur_row;
    const bool     was_on = s_cur_on;
    s_cur_col = p[0];
    s_cur_row = p[1];
    s_cell_ch[s_cur_row][s_cur_col] = p[2];
    s_cell_at[s_cur_row][s_cur_col] = p[3];
    s_cur_on = p[4] != 0u;
    text_take_glass();
    if (was_on && was_row != s_cur_row) {
        text_paint_row(was_row); /* un-draw the caret where it used to be */
    }
    text_paint_row(s_cur_row);
}

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/*
 * One slice into the frame.
 *
 * The pixels arrive little-endian - the S3 keeps them the way its own
 * framebuffer does - and the controller wants them the other way round, so
 * they are swapped on the way in.  Here rather than at paint time on purpose:
 * this happens while the wire is the slow part, and the painting has to be one
 * uninterrupted run.
 */
static void pal_in(const uint8_t *p, uint32_t len)
{
    if (len < PKT_PIX + 512u) {
        return;
    }
    uint8_t got = 0;
    for (uint32_t i = 0; i < PKT_GEOM; i++) {
        got = (uint8_t)(got ^ p[i]);
    }
    const uint8_t *const b = p + PKT_PIX;
    for (uint32_t i = 0; i < 512u; i++) {
        got = (uint8_t)(got ^ b[i]);
    }
    if (got != p[PKT_SUM]) {
        s_stat_bad++;
        return;
    }
    for (uint32_t i = 0; i < 256u; i++) {
        /* Little-endian on the wire, high byte first to the panel. */
        s_pal[i] = (uint16_t)(b[i * 2u + 1u] | ((uint16_t)b[i * 2u] << 8));
    }
    s_pal_gen = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    s_stat_pal++;
}

static void slice_in(const uint8_t *p, uint32_t len)
{
    const uint32_t frame = rd16(p + 0);
    const uint32_t slice = rd16(p + 2);
    const uint32_t slices = rd16(p + 4);
    const uint32_t w = rd16(p + 6);
    const uint32_t y0 = rd16(p + 8);
    const uint32_t rows = rd16(p + 10);

    /* One byte a pixel or two, said by the opcode and passed in here as the
     * width of a pixel on the wire. */
    const uint32_t bpp = s_slice_bpp;
    if (w != LCD_W || slices != SLICES || slice >= SLICES || rows == 0 ||
        rows > RAW_ROWS || y0 + rows > LCD_H ||
        len < PKT_PIX + rows * w * bpp) {
        return;
    }

    /*
     * The sum, over the geometry and the pixels - not over the padding, which
     * carries nothing and would only make the two ends agree about zeroes.
     * A slice that does not add up is dropped and asked for again by the gap
     * logic below, exactly as a missing one is.
     */
    {
        uint8_t got = 0;
        for (uint32_t i = 0; i < PKT_GEOM; i++) {
            got = (uint8_t)(got ^ p[i]);
        }
        const uint8_t *pix = p + PKT_PIX;
        for (uint32_t i = 0; i < rows * w * bpp; i++) {
            got = (uint8_t)(got ^ pix[i]);
        }
        if (got != p[PKT_SUM]) {
            s_stat_bad++;
            return;
        }
    }
    s_stat_slices++;

    if ((int32_t)frame != s_frame_no) {
        if (s_frame_no >= 0 && s_have != s_want) {
            s_stat_partial++;
        }
        s_frame_no = (int32_t)frame;
        s_have = 0;
        s_asked = 0;
    }

    /* A gap means one went missing; ask once, while the far end still has it. */
    for (uint32_t i = 0; i < slice; i++) {
        const uint32_t bit = 1u << i;
        if ((s_have & bit) == 0u && (s_asked & bit) == 0u) {
            s_asked |= bit;
            ask_again(frame, i);
        }
    }

    const uint8_t *src = p + PKT_PIX;
    uint16_t *dst = s_frame + (size_t)y0 * LCD_W;
    if (bpp == 1u) {
        for (uint32_t i = 0; i < rows * w; i++) {
            dst[i] = s_pal[src[i]];
        }
    } else {
        for (uint32_t i = 0; i < rows * w; i++) {
            dst[i] = (uint16_t)((uint16_t)src[i * 2u + 1u] |
                                ((uint16_t)src[i * 2u] << 8));
        }
    }
    s_have |= (1u << slice);

    if (s_have == s_want) {
        /* A whole frame covers every pixel, so the console needs no clearing
         * to be taken off the glass - it is simply painted over. */
        s_text = false;
        paint();
        s_frame_no = -1;
    }
}

/*
 * Framing, a byte at a time, because a slice straddles any read and the only
 * thing that has to be found is the two magic bytes.
 */
static void reader(void *arg)
{
    static uint8_t io[4096];
    static uint8_t payload[PKT_PIX + LCD_W * RAW_ROWS * 2];
    enum { HUNT0, HUNT1, OP, LEN0, LEN1, BODY, SUM } st = HUNT0;
    uint32_t op = 0, len = 0, at = 0;
    uint8_t sum = 0;

    (void)arg;
    for (;;) {
        const int n = uart_read_bytes(LINK_UART, io, sizeof(io),
                                      pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }
        s_stat_rx += (uint32_t)n;
        for (int i = 0; i < n; i++) {
            const uint8_t b = io[i];
            switch (st) {
            case HUNT0:
                if (b != FR_MAGIC0) {
                    s_stat_stray++;
                }
                st = (b == FR_MAGIC0) ? HUNT1 : HUNT0;
                break;
            case HUNT1:
                st = (b == FR_MAGIC1) ? OP : ((b == FR_MAGIC0) ? HUNT1 : HUNT0);
                break;
            case OP:
                op = b;
                st = LEN0;
                break;
            case LEN0:
                len = b;
                st = LEN1;
                break;
            case LEN1:
                len |= (uint32_t)b << 8;
                if (len > sizeof(payload)) {
                    st = HUNT0;
                    break;
                }
                at = 0;
                sum = 0;
                st = (len == 0) ? SUM : BODY;
                break;
            case BODY:
                payload[at++] = b;
                sum = (uint8_t)(sum ^ b);
                if (at == len) {
                    /*
                     * Picture and sound both carry their sum inside the header
                     * and have nothing after the payload.  Console rows still
                     * end with one, and are still read the old way.
                     */
                    if (op == FR_RAW || op == FR_IDX8) {
                        s_slice_bpp = (op == FR_IDX8) ? 1u : 2u;
                        if (len >= PKT_PIX) {
                            slice_in(payload, len);
                        }
                        st = HUNT0;
                    } else if (op == FR_PAL) {
                        pal_in(payload, len);
                        st = HUNT0;
                    } else if (op == FR_AUD) {
                        aud_in(payload, len);
                        st = HUNT0;
                    } else {
                        /* Console rows and the caret still carry a byte after
                         * the payload; they are read in the SUM state below,
                         * which is where they are acted on. */
                        st = SUM;
                    }
                }
                break;
            case SUM:
                /* Console rows and the caret only; picture and sound never
                 * reach here.  The sum is checked, because a mangled row would
                 * otherwise be painted and stay on the glass until that row
                 * changes again - which on a quiet prompt is for ever. */
                if (b == sum) {
                    if (op == FR_ROW) {
                        text_row_in(payload, len);
                    } else if (op == FR_CUR) {
                        text_cursor_in(payload, len);
                    }
                } else {
                    s_stat_bad++;
                }
                st = HUNT0;
                break;
            }
        }
    }
}

void app_main(void)
{
    /*
     * The frame, in one piece, first - before anything else can put something
     * in the middle of the largest block.  With no operating system in the way
     * that block is around 300 KB here, so 150 KB is asked for plainly.
     */
    s_frame = heap_caps_malloc((size_t)LCD_W * LCD_H * sizeof(uint16_t),
                               MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "frame buffer %u bytes: %s",
             (unsigned)((size_t)LCD_W * LCD_H * 2u),
             (s_frame != NULL) ? "yes" : "NO");
    if (s_frame == NULL) {
        ESP_LOGE(TAG, "largest free block %u",
                 (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        return;
    }
    memset(s_frame, 0, (size_t)LCD_W * LCD_H * 2u);

    panel_init();
    ESP_LOGI(TAG, "panel up, %u free", (unsigned)esp_get_free_heap_size());

    const uart_config_t uc = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, RX_RING, 256, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, PIN_TX, PIN_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "wire up: uart1 %d baud, rx %d tx %d", LINK_BAUD, PIN_RX,
             PIN_TX);

    xTaskCreate(reader, "reader", 4096, NULL, 10, NULL);

    /*
     * The sound, if there is room for it once the picture has what it needs.
     * Nine kilobytes: a 372 ms ring and the block the converter is fed from.
     */
    s_aring = heap_caps_malloc(AUD_RING, MALLOC_CAP_8BIT);
    s_aout = heap_caps_malloc(AUD_CHUNK, MALLOC_CAP_8BIT);
    if (s_aring == NULL || s_aout == NULL) {
        free(s_aring);
        free(s_aout);
        s_aring = NULL;
        s_aout = NULL;
        ESP_LOGW(TAG, "no room for the sound; the picture goes on alone");
    } else {
        /*
         * Below the reader on purpose.  The player spends its life blocked in
         * the converter, so it costs nothing to prefer the wire over it - and
         * if the board is ever short of time, a sound that stutters is better
         * than a picture with holes in it.
         */
        xTaskCreate(player, "player", 3072, NULL, 6, NULL);
    }

    /*
     * The numbers, every five seconds, into the console port - which nothing
     * else here is using.  Under the operating system this was the hard part:
     * the receiver held the display, so its console had nowhere to print, and
     * taking the console back took the display away for good.
     */
    uint32_t last_frames = 0, last_slices = 0, last_asamp = 0;
    uint32_t last_rx = 0, last_stray = 0;
    uint64_t last_paint = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        const uint32_t f = s_stat_frames - last_frames;
        const uint32_t sl = s_stat_slices - last_slices;
        const uint64_t pu = s_stat_paint_us - last_paint;
        ESP_LOGI(TAG,
                 "%u.%u frames/s, %u slices/s, paint %u us, %u partial, "
                 "%u asked, %u bad, %u rows, pal %u/%u, %u B/s in (%u stray), "
                 "%u free",
                 (unsigned)(f / 5u), (unsigned)((f * 2u) % 10u),
                 (unsigned)(sl / 5u), (unsigned)(f ? (pu / f) : 0),
                 (unsigned)s_stat_partial, (unsigned)s_stat_asked,
                 (unsigned)s_stat_bad, (unsigned)s_stat_rows,
                 (unsigned)s_stat_pal, (unsigned)s_pal_gen,
                 (unsigned)((s_stat_rx - last_rx) / 5u),
                 (unsigned)(s_stat_stray - last_stray),
                 (unsigned)esp_get_free_heap_size());
        last_rx = s_stat_rx;
        last_stray = s_stat_stray;
        if (s_stat_asamp != 0u || s_stat_adrop != 0u) {
            ESP_LOGI(TAG, "sound %u B/s, %u buffered, %u dropped, %u bad, "
                          "%u starved",
                     (unsigned)((s_stat_asamp - last_asamp) / 5u),
                     (unsigned)aud_used(), (unsigned)s_stat_adrop,
                     (unsigned)s_stat_abad, (unsigned)s_stat_astarve);
        }
        last_asamp = s_stat_asamp;
        last_frames = s_stat_frames;
        last_slices = s_stat_slices;
        last_paint = s_stat_paint_us;
    }
}
