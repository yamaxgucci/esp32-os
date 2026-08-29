/*
 * STOMP - the valve amplifier with its knobs on a screen.
 *
 *   run a:\stomp.axe [model]
 *
 * The knobs are not a list kept here.  A model says how many controls its
 * amplifier has and what they are called - see ag_amp_pot_count - and this lays
 * out however many that is: five for the 2203, six for the Shiva and the SLO
 * because those have a gain pot between stages, three for the pedal, which has
 * no tone stack at all.  A screen that carried its own list would be wrong the
 * first time a model changed, and the models are still changing.
 *
 * HOW A KNOB IS TURNED
 *
 * Tap one, or walk to it with the arrows and press Enter, and it opens as a
 * slider from 0 to 10.  Ten positions is what a real pedal is marked with, and
 * the conversion into whatever the circuit actually wants - a resistance in the
 * tone network, a level, a wiper position - belongs to the model rather than to
 * this file.  Enter or Escape closes it.
 *
 * **Noon is 5, and 5 is exactly what the voicing fit left.**  A preset played
 * with nothing touched is the preset that was measured: the knobs are the
 * player's, and the matching walk never turns them.
 *
 * THE THREE BUTTONS
 *
 * Along the bottom, and they are buttons rather than labels because they are
 * pressed: the cabinet, the switch that takes the cabinet out of the path, and
 * the preset.  The two outer ones open a chooser over the picture - a real
 * directory listing, walked with the arrows, entered with Enter, left with
 * Escape.  The cabinet's list begins with "(no cabinet)", and the preset's with
 * the models built into this image, because those are presets too.
 *
 * WHAT IS HERE AND WHAT IS NOT, YET
 *
 * What is not here is the audio: this edits a configuration and draws it.
 * Wiring it to the input and the output is the next piece, and it is
 * deliberately separate, because a screen that redraws while the card is waiting
 * is how a knob becomes a crackle.  So a chosen *file* - a .wav cabinet, a
 * .preset off the card - is recorded and shown, and read when there is something
 * to play it through; a chosen built-in model takes effect at once, which is why
 * the knobs under it change as you pick.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <argon/argon.h>
#include <argon/keys.h>
#include <stdio.h>
#include <string.h>

#include "ag_amp.h"

AG_APP("STOMP", "1.0", "argon", AG_AXE_NEEDS_GFX);

#define ROW_KNOBS 3  /* three across; six knobs make two rows, three make one */
#define TOP_H     26 /* the model's name */
#define BAR_H     36 /* the row of buttons */
#define LIST_MAX  64 /* entries a chooser holds; more are counted, not shown */
#define LIST_ROW  18

/* What a line in the chooser is.  A header is drawn and skipped over, so that
 * "built in" and "on the card" can separate two kinds of preset in one list. */
enum {
    ENT_HEAD,
    ENT_UP,
    ENT_DIR,
    ENT_FILE,
    ENT_MODEL,
    ENT_NONE
};

enum { BROWSE_OFF, BROWSE_CAB, BROWSE_PRESET };

static ag_gfxinfo_t s_info; /* the display, held for as long as we run */
static ag_amp_cfg_t s_cfg;
static int          s_model = AG_AMP_MODEL_JCM800;
static int          s_sel;     /* knobs, then cabinet, switch, preset */
static int          s_editing; /* the slider is open on s_sel */
static int          s_cab_on = 1;
static char         s_cab[AG_PATH_MAX] = "";
static char         s_preset[AG_PATH_MAX] = "jcm800";

static int  s_browse;
static char s_dir[AG_PATH_MAX];
static char s_names[LIST_MAX][AG_NAME_MAX];
static unsigned char s_kind[LIST_MAX];
static short         s_aux[LIST_MAX]; /* the model number, for ENT_MODEL */
static int           s_n, s_bsel, s_btop, s_more;

/* The soft display takes 0x00RRGGBB and narrows it itself; packing 565 here
 * would put the red where the panel expects the blue. */
static uint32_t rgb(int r, int g, int b)
{
    return (uint32_t)(((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
}

/* ---------------------------------------------------------------------- */
/* Paths                                                                  */
/* ---------------------------------------------------------------------- */

/* Whichever separator the path already uses.  getcwd answers in one form and
 * the shell types the other, and joining with the wrong one makes a name that
 * opendir cannot find. */
static char sep_of(const char *path)
{
    size_t i;
    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == '\\') {
            return '\\';
        }
    }
    return '/';
}

static void join(char *out, size_t len, const char *dir, const char *name)
{
    const size_t n = strlen(dir);
    const char   sep = sep_of(dir);
    if (n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) {
        (void)snprintf(out, len, "%s%s", dir, name);
    } else {
        (void)snprintf(out, len, "%s%c%s", dir, sep, name);
    }
}

/* True when there is nowhere above this: "a:\", "/", "A:/". */
static int is_root(const char *path)
{
    const size_t n = strlen(path);
    if (n == 0) {
        return 1;
    }
    if (n == 1 && (path[0] == '/' || path[0] == '\\')) {
        return 1;
    }
    return n == 3 && path[1] == ':';
}

static void go_up(char *path)
{
    size_t at = strlen(path);
    while (at > 0 && (path[at - 1] == '/' || path[at - 1] == '\\')) {
        at--;
    }
    while (at > 0 && path[at - 1] != '/' && path[at - 1] != '\\') {
        at--;
    }
    if (at > 1 && path[at - 2] != ':') {
        at--; /* drop the separator, unless it is the one after "a:" */
    }
    path[at] = '\0';
}

/* Case-insensitive tail match, because a card writes NAME.WAV and a person
 * types name.wav. */
static int ends_with(const char *s, const char *ext)
{
    const size_t ns = strlen(s), ne = strlen(ext);
    size_t       i;
    if (ne > ns) {
        return 0;
    }
    for (i = 0; i < ne; i++) {
        char a = s[ns - ne + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + 32);
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b + 32);
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

/* The last component, for showing a chosen file on a button. */
static const char *leaf(const char *path)
{
    size_t at = strlen(path);
    while (at > 0 && path[at - 1] != '/' && path[at - 1] != '\\') {
        at--;
    }
    return path + at;
}

/* ---------------------------------------------------------------------- */
/* The chooser's list                                                     */
/* ---------------------------------------------------------------------- */

static void add(const char *name, int kind, int aux)
{
    if (s_n >= LIST_MAX) {
        s_more++;
        return;
    }
    (void)snprintf(s_names[s_n], AG_NAME_MAX, "%s", name);
    s_kind[s_n] = (unsigned char)kind;
    s_aux[s_n] = (short)aux;
    s_n++;
}

static void fill_list(void)
{
    const char *ext = (s_browse == BROWSE_CAB) ? ".wav" : ".preset";
    ag_dirent_t e;
    ag_handle_t h;

    s_n = 0;
    s_more = 0;
    if (s_browse == BROWSE_CAB) {
        /* Off is a position of this control and not a separate switch, so it
         * belongs in the list as well as on the button beside it. */
        add("(no cabinet)", ENT_NONE, 0);
    } else {
        int m;
        add("built in", ENT_HEAD, 0);
        for (m = 0; m < AG_AMP_MODEL_N; m++) {
            add(ag_amp_model_name(m), ENT_MODEL, m);
        }
        add("on the card", ENT_HEAD, 0);
    }
    if (!is_root(s_dir)) {
        add("..", ENT_UP, 0);
    }
    h = ag_opendir(s_dir);
    if (h < 0) {
        add("(cannot read this directory)", ENT_HEAD, 0);
        return;
    }
    while (ag_readdir(h, &e) == AG_OK) {
        if (e.name[0] == '.' && e.name[1] == '\0') {
            continue;
        }
        if ((e.st.attr & AG_A_DIR) != 0) {
            add(e.name, ENT_DIR, 0);
        } else if (ends_with(e.name, ext)) {
            add(e.name, ENT_FILE, 0);
        }
    }
    (void)ag_closedir(h);

    s_bsel = 0;
    s_btop = 0;
    while (s_bsel < s_n && s_kind[s_bsel] == ENT_HEAD) {
        s_bsel++;
    }
}

static void open_browser(int what)
{
    s_browse = what;
    if (s_dir[0] == '\0' && ag_getcwd(s_dir, sizeof(s_dir)) != AG_OK) {
        (void)snprintf(s_dir, sizeof(s_dir), "a:\\");
    }
    fill_list();
}

/* Headers are drawn but not landed on. */
static void browse_move(int d)
{
    int at = s_bsel;
    for (;;) {
        at += d;
        if (at < 0 || at >= s_n) {
            return;
        }
        if (s_kind[at] != ENT_HEAD) {
            break;
        }
    }
    s_bsel = at;
}

/* ---------------------------------------------------------------------- */
/* Geometry                                                               */
/* ---------------------------------------------------------------------- */

static int knob_n(void) { return ag_amp_pot_count(s_model); }
static int item_count(void) { return knob_n() + 3; }

static void knob_box(int i, int *x, int *y, int *w, int *h, int sw, int sh)
{
    const int n = knob_n();
    const int rows = (n + ROW_KNOBS - 1) / ROW_KNOBS;
    const int per = (n + rows - 1) / rows;
    const int row = i / per;
    const int col = i % per;
    const int top = TOP_H;
    const int bot = sh - BAR_H - 16;
    *h = (bot - top) / (rows > 0 ? rows : 1);
    *w = sw / (per > 0 ? per : 1);
    *x = col * *w;
    *y = top + row * *h;
}

/* which: 0 the cabinet, 1 the switch, 2 the preset. */
static void button_box(int which, int *x, int *y, int *w, int *h, int sw, int sh)
{
    const int pad = 10;
    const int sww = 76; /* the switch; the other two share what is left */
    const int side = (sw - 4 * pad - sww) / 2;
    *y = sh - BAR_H - 8;
    *h = BAR_H;
    switch (which) {
    case 0: *x = pad; *w = side; break;
    case 1: *x = 2 * pad + side; *w = sww; break;
    default: *x = 3 * pad + side + sww; *w = side; break;
    }
}

/* 0..1 into the ten marks a pedal carries. */
static int marks(float pos)
{
    int v = (int)(pos * 10.0f + 0.5f);
    return v < 0 ? 0 : (v > 10 ? 10 : v);
}

/* ---------------------------------------------------------------------- */
/* Drawing                                                                */
/* ---------------------------------------------------------------------- */

static void draw_knob(int i, int sw, int sh)
{
    int         x, y, w, h;
    const int   id = ag_amp_pot_id(s_model, i);
    const float pos = s_cfg.pot[id];
    char        buf[24];
    int         cx, cy, r, avail, group, top;

    knob_box(i, &x, &y, &w, &h, sw, sh);
    if (s_sel == i) {
        ag_gfx_fill_round_rect((int16_t)(x + 2), (int16_t)(y + 2),
                               (uint16_t)(w - 4), (uint16_t)(h - 4), 4,
                               rgb(40, 44, 60));
    }
    /* The dial is sized from what is left after the label rather than from the
     * whole cell: a pointer touching its own name reads as one smudge.  Then
     * the dial and its label are centred together, so that one row of three
     * knobs does not leave its names stranded at the bottom of a tall cell. */
    avail = h - 28;
    r = avail / 2 - 10;
    if (r > w / 2 - 18) {
        r = w / 2 - 18;
    }
    if (r > 72) {
        r = 72; /* past this a one-pixel pointer looks lost in the face */
    }
    if (r < 6) {
        r = 6;
    }
    cx = x + w / 2;
    group = 2 * r + 8 + 16;
    top = y + (h - group) / 2;
    cy = top + r;
    ag_gfx_fill_circle((int16_t)cx, (int16_t)cy, (uint16_t)r, rgb(28, 30, 36));
    ag_gfx_circle((int16_t)cx, (int16_t)cy, (uint16_t)r, rgb(150, 155, 170));
    {
        /* ag_sinf/ag_cosf are not in the app ABI, and a pointer does not need
         * them: eleven marks, one per position, as hundredths of the radius.
         * Twenty-seven degrees apart, seven o'clock round to five, so that 5
         * is exactly noon - which is where a preset was measured. */
        static const signed char sx[11] = { -71, -95, -99, -81, -45,   0,
                                             45,  81,  99,  95,  71 };
        static const signed char sy[11] = { -71, -31,  16,  59,  89, 100,
                                             89,  59,  16, -31, -71 };
        const int k = marks(pos);
        ag_gfx_line((int16_t)cx, (int16_t)cy,
                    (int16_t)(cx + sx[k] * r / 100),
                    (int16_t)(cy - sy[k] * r / 100), rgb(230, 200, 120));
    }
    /* The built-in font is eight pixels wide, so half the string is half of
     * eight times its length: the label sits under the middle of its dial. */
    (void)snprintf(buf, sizeof(buf), "%s %d", ag_amp_pot_name(s_model, i),
                   marks(pos));
    ag_gfx_text((int16_t)(cx - (int)strlen(buf) * 4),
                (int16_t)(top + 2 * r + 8), buf,
                s_sel == i ? rgb(240, 210, 130) : rgb(200, 205, 220),
                AG_GFX_TRANS);
}

static void draw_button(int x, int y, int w, int h, const char *label, int sel,
                        uint32_t face, uint32_t ink)
{
    /* Two rounded rectangles, the outer one two pixels proud: that is the
     * border, and it is what makes a pressable thing look pressable. */
    ag_gfx_fill_round_rect((int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 6,
                           sel ? rgb(235, 205, 125) : rgb(76, 82, 100));
    ag_gfx_fill_round_rect((int16_t)(x + 2), (int16_t)(y + 2),
                           (uint16_t)(w - 4), (uint16_t)(h - 4), 5, face);
    (void)ag_gfx_text_fit((int16_t)(x + 10), (int16_t)(y + (h - 16) / 2),
                          (uint16_t)(w - 20), label, ink, AG_GFX_TRANS);
}

static void draw_bar(int sw, int sh)
{
    const int n = knob_n();
    int       x, y, w, h;
    char      line[AG_PATH_MAX + 8];

    button_box(0, &x, &y, &w, &h, sw, sh);
    (void)snprintf(line, sizeof(line), "CAB  %s",
                   s_cab[0] != '\0' ? leaf(s_cab) : "from the preset");
    draw_button(x, y, w, h, line, s_sel == n, rgb(34, 37, 46),
                s_cab_on ? rgb(215, 220, 235) : rgb(120, 126, 142));

    button_box(1, &x, &y, &w, &h, sw, sh);
    draw_button(x, y, w, h, s_cab_on ? "  ON" : " OFF", s_sel == n + 1,
                s_cab_on ? rgb(46, 78, 54) : rgb(46, 34, 36),
                s_cab_on ? rgb(170, 235, 175) : rgb(230, 150, 150));

    button_box(2, &x, &y, &w, &h, sw, sh);
    (void)snprintf(line, sizeof(line), "PRESET  %s", leaf(s_preset));
    draw_button(x, y, w, h, line, s_sel == n + 2, rgb(34, 37, 46),
                rgb(215, 220, 235));
}

/* The two overlays' rectangles, in one place: the painter, the hit test and
 * the dirty marking all have to agree about where they are. */
static void slider_box(int *x, int *y, int *w, int *h, int sw, int sh)
{
    *x = 18;
    *y = sh / 2 - 28;
    *w = sw - 36;
    *h = 62;
}

static void browser_box(int *x, int *y, int *w, int *h, int sw, int sh)
{
    *x = 30;
    *y = 24;
    *w = sw - 60;
    *h = sh - 48;
}

static void draw_slider(int sw, int sh)
{
    /* Over the picture rather than instead of it: the other knobs stay
     * readable, which is what a hand reaching for one wants. */
    const int id = ag_amp_pot_id(s_model, s_sel);
    const int y = sh / 2 - 18;
    const int w = sw - 48;
    const int v = marks(s_cfg.pot[id]);
    char      cap[32];
    int       i;

    ag_gfx_fill_round_rect(18, (int16_t)(y - 10), (uint16_t)(w + 12), 62, 6,
                           rgb(30, 32, 42));
    (void)snprintf(cap, sizeof(cap), "%s  %d", ag_amp_pot_name(s_model, s_sel),
                   v);
    ag_gfx_text(28, (int16_t)(y - 2), cap, rgb(235, 235, 240), AG_GFX_TRANS);
    ag_gfx_fill_rect(28, (int16_t)(y + 22), (uint16_t)w, 6, rgb(60, 64, 80));
    ag_gfx_fill_rect(28, (int16_t)(y + 22), (uint16_t)(w * v / 10), 6,
                     rgb(230, 200, 120));
    ag_gfx_fill_circle((int16_t)(28 + w * v / 10), (int16_t)(y + 25), 7,
                       rgb(240, 220, 160));
    /* Eleven ticks, so a glance says which of the ten it is sitting on. */
    for (i = 0; i <= 10; i++) {
        ag_gfx_fill_rect((int16_t)(28 + w * i / 10), (int16_t)(y + 34), 1, 4,
                         rgb(90, 96, 116));
    }
}

static void draw_browser(int sw, int sh)
{
    int       x, y, w, h;
    int       rows;
    browser_box(&x, &y, &w, &h, sw, sh);
    rows = (h - 54) / LIST_ROW;
    {
    int       i;
    char      line[AG_PATH_MAX + 16];

    ag_gfx_fill_round_rect((int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 8,
                           rgb(70, 76, 94));
    ag_gfx_fill_round_rect((int16_t)(x + 2), (int16_t)(y + 2),
                           (uint16_t)(w - 4), (uint16_t)(h - 4), 7,
                           rgb(24, 26, 33));
    ag_gfx_text((int16_t)(x + 12), (int16_t)(y + 8),
                s_browse == BROWSE_CAB ? "CABINET" : "PRESET",
                rgb(240, 210, 130), AG_GFX_TRANS);
    (void)ag_gfx_text_fit((int16_t)(x + 12), (int16_t)(y + 26),
                          (uint16_t)(w - 24), s_dir, rgb(140, 146, 164),
                          AG_GFX_TRANS);

    if (s_bsel < s_btop) {
        s_btop = s_bsel;
    }
    if (s_bsel >= s_btop + rows) {
        s_btop = s_bsel - rows + 1;
    }
    for (i = 0; i < rows && s_btop + i < s_n; i++) {
        const int at = s_btop + i;
        const int ly = y + 50 + i * LIST_ROW;
        uint32_t  ink = rgb(210, 215, 230);
        if (s_kind[at] == ENT_HEAD) {
            ink = rgb(120, 126, 148);
        } else if (s_kind[at] == ENT_DIR || s_kind[at] == ENT_UP) {
            ink = rgb(150, 200, 235);
        }
        if (at == s_bsel) {
            ag_gfx_fill_round_rect((int16_t)(x + 8), (int16_t)(ly - 2),
                                   (uint16_t)(w - 16), LIST_ROW, 3,
                                   rgb(48, 54, 72));
            ink = rgb(245, 220, 150);
        }
        if (s_kind[at] == ENT_DIR) {
            (void)snprintf(line, sizeof(line), "[%s]", s_names[at]);
        } else {
            (void)snprintf(line, sizeof(line), "%s", s_names[at]);
        }
        (void)ag_gfx_text_fit((int16_t)(x + 14), (int16_t)ly,
                              (uint16_t)(w - 28), line, ink, AG_GFX_TRANS);
    }
    if (s_more > 0) {
        (void)snprintf(line, sizeof(line), "... %d more not shown", s_more);
        ag_gfx_text((int16_t)(x + 14), (int16_t)(y + h - 20), line,
                    rgb(150, 120, 120), AG_GFX_TRANS);
    }
    }
}

/* ---------------------------------------------------------------------- */
/* Redrawing only what changed                                            */
/* ---------------------------------------------------------------------- */

/*
 * A full repaint of this screen is a million instructions - measured, see
 * `bench` below - and turning one knob changes about a fiftieth of the picture.
 * So what changed is marked, and only that is painted and sent.
 *
 * There is no swap here.  ag_gfx_flush already copies its rectangle to the front
 * buffer and puts that rectangle on the panel; calling swap as well copied the
 * whole frame and presented it a second time, which on a panel hanging off SPI
 * is the entire cost of the update paid twice.
 */
#define DIRTY_MAX 4

static struct {
    short x, y, w, h;
} s_dirty[DIRTY_MAX];
static int s_ndirty;
static int s_dirty_all;

static void dirty_all(void)
{
    s_dirty_all = 1;
    s_ndirty = 0;
}

static void dirty(int x, int y, int w, int h)
{
    if (s_dirty_all) {
        return;
    }
    if (s_ndirty >= DIRTY_MAX) {
        /* More pieces than it is worth tracking: paint the lot.  Four is
         * enough for the two things a keystroke can touch. */
        dirty_all();
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > (int)s_info.width) {
        w = (int)s_info.width - x;
    }
    if (y + h > (int)s_info.height) {
        h = (int)s_info.height - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    s_dirty[s_ndirty].x = (short)x;
    s_dirty[s_ndirty].y = (short)y;
    s_dirty[s_ndirty].w = (short)w;
    s_dirty[s_ndirty].h = (short)h;
    s_ndirty++;
}

/* A knob's cell or a button, whichever this index is. */
static void dirty_item(int i)
{
    const int sw = (int)s_info.width, sh = (int)s_info.height;
    int       x, y, w, h;
    if (i < 0 || i >= item_count()) {
        return;
    }
    if (i < knob_n()) {
        knob_box(i, &x, &y, &w, &h, sw, sh);
    } else {
        button_box(i - knob_n(), &x, &y, &w, &h, sw, sh);
    }
    dirty(x, y, w, h);
}

static void dirty_slider(void)
{
    int x, y, w, h;
    slider_box(&x, &y, &w, &h, (int)s_info.width, (int)s_info.height);
    dirty(x, y, w, h);
}

static int hits(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

/* Everything that shows through the given rectangle, in the order it stacks. */
static void paint_region(int rx, int ry, int rw, int rh)
{
    const int sw = (int)s_info.width;
    const int sh = (int)s_info.height;
    int       i, x, y, w, h;

    ag_gfx_fill_rect((int16_t)rx, (int16_t)ry, (uint16_t)rw, (uint16_t)rh,
                     rgb(16, 17, 22));
    if (hits(rx, ry, rw, rh, 0, 0, sw, TOP_H)) {
        ag_gfx_text(6, 6, ag_amp_model_name(s_model), rgb(235, 235, 240),
                    AG_GFX_TRANS);
    }
    for (i = 0; i < knob_n(); i++) {
        knob_box(i, &x, &y, &w, &h, sw, sh);
        if (hits(rx, ry, rw, rh, x, y, w, h)) {
            draw_knob(i, sw, sh);
        }
    }
    button_box(0, &x, &y, &w, &h, sw, sh);
    if (hits(rx, ry, rw, rh, x, y, sw - 2 * x, h)) {
        draw_bar(sw, sh); /* the three of them are one strip */
    }
    if (s_editing && s_sel < knob_n()) {
        slider_box(&x, &y, &w, &h, sw, sh);
        if (hits(rx, ry, rw, rh, x, y, w, h)) {
            draw_slider(sw, sh);
        }
    }
    if (s_browse != BROWSE_OFF) {
        browser_box(&x, &y, &w, &h, sw, sh);
        if (hits(rx, ry, rw, rh, x, y, w, h)) {
            draw_browser(sw, sh);
        }
    }
}

/* Painting the whole thing, which is what the first frame and the bench want. */
static void paint(void)
{
    paint_region(0, 0, (int)s_info.width, (int)s_info.height);
}

static void present(void)
{
    int i;
    if (s_dirty_all) {
        paint();
        ag_gfx_flush(0, 0, s_info.width, s_info.height);
    } else {
        for (i = 0; i < s_ndirty; i++) {
            paint_region(s_dirty[i].x, s_dirty[i].y, s_dirty[i].w,
                         s_dirty[i].h);
            ag_gfx_flush((uint16_t)s_dirty[i].x, (uint16_t)s_dirty[i].y,
                         (uint16_t)s_dirty[i].w, (uint16_t)s_dirty[i].h);
        }
    }
    s_dirty_all = 0;
    s_ndirty = 0;
}

static void draw(void)
{
    dirty_all();
    present();
}

/* ---------------------------------------------------------------------- */
/* What the screen costs                                                  */
/* ---------------------------------------------------------------------- */

/*
 * The question this answers is not "how fast is the drawing" but "can the
 * amplifier still make its deadline while it happens".  So the units are
 * tubebench's: instructions under QEMU with -icount shift=0, which on the chip
 * can only be larger - QEMU models neither the FPU's latency nor a cache miss.
 *
 *   argon test -Icount -Sd "run a:\STOMP.AXE bench"
 *
 * The last column is the one that matters: how many audio samples' worth of a
 * core one redraw takes at 22.05 kHz.  If that is larger than the frame the
 * audio callback works in, the drawing cannot share the callback's thread, and
 * no amount of tidying the drawing changes that.
 */
#define BENCH_HZ    240000000u /* the core the numbers are converted against */
#define BENCH_RATE  22050u
#define BENCH_FPS   30u

static const char *pad18(const char *s)
{
    static char p[20];
    int         i = 0;
    while (s[i] != '\0' && i < 18) {
        p[i] = s[i];
        i++;
    }
    while (i < 18) {
        p[i++] = ' ';
    }
    p[18] = '\0';
    return p;
}

static void bench_line(const char *label, uint64_t us, uint32_t n)
{
    const uint64_t instr = (us * 1000ull) / (n > 0 ? n : 1u);
    const uint32_t us240 = (uint32_t)(instr / 240ull);
    /* Percent of one core, in basis points, if this were done FPS times a
     * second - which is the pessimistic reading of "redraw on every event". */
    const uint32_t bp =
        (uint32_t)((instr * BENCH_FPS * 10000ull) / (uint64_t)BENCH_HZ);
    const uint32_t samples =
        (uint32_t)((instr * BENCH_RATE) / (uint64_t)BENCH_HZ);
    ag_printf("  %s %9u instr %6u.%03u ms  %3u.%02u%% at %u fps  %5u samples\n",
              pad18(label), (unsigned)instr, us240 / 1000u, us240 % 1000u,
              bp / 100u, bp % 100u, (unsigned)BENCH_FPS, samples);
}

static void bench(void)
{
    const int sw = (int)s_info.width;
    const int sh = (int)s_info.height;
    const uint32_t n = 20;
    uint32_t       i;
    uint64_t       t0, t1;
    uint64_t       us_paint, us_present, us_knob, us_move, us_browse, us_clear;
    int            x, y, w, h;

    /* Painting the whole screen, with nothing put on the panel. */
    t0 = ag_micros();
    for (i = 0; i < n; i++) {
        paint();
    }
    t1 = ag_micros();
    us_paint = t1 - t0;

    /* The clear on its own, because a full-screen fill is most of the cost and
     * is the first thing a partial redraw stops doing. */
    t0 = ag_micros();
    for (i = 0; i < n; i++) {
        ag_gfx_clear(rgb(16, 17, 22));
    }
    t1 = ag_micros();
    us_clear = t1 - t0;

    /* Handing the whole frame to the panel.  On the soft display in QEMU the
     * drawing target *is* the panel's memory, so this is nearly free and says
     * nothing about a screen on the end of an SPI bus; there the same call is
     * width*height*2 bytes down the wire. */
    t0 = ag_micros();
    for (i = 0; i < n; i++) {
        ag_gfx_flush(0, 0, s_info.width, s_info.height);
    }
    t1 = ag_micros();
    us_present = t1 - t0;

    /* What turning a knob actually costs now: one cell marked, painted and
     * flushed, through the same path the event loop uses. */
    t0 = ag_micros();
    for (i = 0; i < n; i++) {
        dirty_item(0);
        present();
    }
    t1 = ag_micros();
    us_knob = t1 - t0;

    /* And what walking the selection costs: two cells, the one left and the
     * one arrived at. */
    t0 = ag_micros();
    for (i = 0; i < n; i++) {
        dirty_item(0);
        dirty_item(1);
        present();
    }
    t1 = ag_micros();
    us_move = t1 - t0;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)sw;
    (void)sh;

    /* The worst frame there is: the chooser over a full screen. */
    s_browse = BROWSE_CAB;
    fill_list();
    t0 = ag_micros();
    for (i = 0; i < n; i++) {
        paint();
    }
    t1 = ag_micros();
    us_browse = t1 - t0;
    s_browse = BROWSE_OFF;

    ag_gfx_release();

    ag_printf("stomp bench: %s, %d knobs, %ux%u, %u passes\n",
              ag_amp_model_name(s_model), ag_amp_pot_count(s_model),
              (unsigned)s_info.width, (unsigned)s_info.height, (unsigned)n);
    ag_printf("  instructions under -icount; on the chip they can only be "
              "larger\n");
    bench_line("full paint", us_paint, n);
    bench_line("  of which clear", us_clear, n);
    bench_line("flush, whole", us_present, n);
    bench_line("full redraw", us_paint + us_present, n);
    bench_line("turn a knob", us_knob, n);
    bench_line("move the cursor", us_move, n);
    bench_line("chooser open", us_browse + us_present, n);
    ag_printf("  a redraw sharing the audio thread has to fit in one frame:\n"
              "  32 samples at %u Hz\n",
              (unsigned)BENCH_RATE);
}

/* ---------------------------------------------------------------------- */
/* Doing something about it                                               */
/* ---------------------------------------------------------------------- */

static void set_model(int m)
{
    dirty_all(); /* the knob count changes, so nothing is where it was */
    s_model = m;
    ag_amp_model(&s_cfg, s_model, 22050.0f);
    ag_amp_pot_apply(&s_cfg, s_model);
    (void)snprintf(s_preset, sizeof(s_preset), "%s", ag_amp_model_name(m));
    if (s_sel >= item_count()) {
        s_sel = item_count() - 1;
    }
}

static void nudge(int d)
{
    const int id = ag_amp_pot_id(s_model, s_sel);
    int       v;
    if (s_sel >= knob_n()) {
        return;
    }
    v = marks(s_cfg.pot[id]) + d;
    if (v < 0) {
        v = 0;
    }
    if (v > 10) {
        v = 10;
    }
    s_cfg.pot[id] = (float)v / 10.0f;
    ag_amp_pot_apply(&s_cfg, s_model);
    dirty_item(s_sel);
    if (s_editing) {
        dirty_slider();
    }
}

static void activate(void)
{
    const int n = knob_n();
    if (s_sel < n) {
        s_editing = !s_editing;
        dirty_all(); /* the slider lies over other knobs, both ways */
    } else if (s_sel == n) {
        open_browser(BROWSE_CAB);
        dirty_all();
    } else if (s_sel == n + 1) {
        s_cab_on = !s_cab_on;
        dirty_item(n);     /* the name goes dim with the cabinet out */
        dirty_item(n + 1);
    } else {
        open_browser(BROWSE_PRESET);
        dirty_all();
    }
}

/* Enter in the chooser: descend, climb, or take what is under the cursor. */
static void dirty_browser(void)
{
    int x, y, w, h;
    browser_box(&x, &y, &w, &h, (int)s_info.width, (int)s_info.height);
    dirty(x, y, w, h);
}

static void browse_enter(void)
{
    char next[AG_PATH_MAX];
    if (s_bsel < 0 || s_bsel >= s_n) {
        return;
    }
    switch (s_kind[s_bsel]) {
    case ENT_HEAD:
        return;
    case ENT_UP:
        go_up(s_dir);
        fill_list();
        dirty_browser();
        return;
    case ENT_DIR:
        join(next, sizeof(next), s_dir, s_names[s_bsel]);
        (void)snprintf(s_dir, sizeof(s_dir), "%s", next);
        fill_list();
        dirty_browser();
        return;
    case ENT_MODEL:
        set_model(s_aux[s_bsel]);
        break;
    case ENT_NONE:
        s_cab[0] = '\0';
        s_cab_on = 0;
        break;
    default:
        join(next, sizeof(next), s_dir, s_names[s_bsel]);
        if (s_browse == BROWSE_CAB) {
            (void)snprintf(s_cab, sizeof(s_cab), "%s", next);
            s_cab_on = 1;
        } else {
            (void)snprintf(s_preset, sizeof(s_preset), "%s", next);
        }
        break;
    }
    s_browse = BROWSE_OFF;
    dirty_all();
}

static void nav_vert(int d)
{
    const int n = knob_n();
    const int rows = (n + ROW_KNOBS - 1) / ROW_KNOBS;
    const int per = (n + rows - 1) / rows;
    if (s_sel >= n) {
        /* Out of the button row and back into the last row of knobs. */
        if (d < 0 && n > 0) {
            s_sel = n - 1;
        }
        return;
    }
    if (s_sel + per * d >= 0 && s_sel + per * d < n) {
        s_sel += per * d;
    } else if (d > 0) {
        s_sel = n; /* off the bottom knob is the cabinet button */
    }
}

/* Every path through here says what it changed, and nothing says "the lot"
 * unless the lot really did change. */
static void on_key(uint16_t k, int *quit)
{
    const int was_sel = s_sel;
    const int was_browse = s_browse;

    if (s_browse != BROWSE_OFF) {
        const int was_bsel = s_bsel;
        switch (k) {
        case AG_KEY_ESC:
        case AG_KEY_Q:
            s_browse = BROWSE_OFF;
            break;
        case AG_KEY_ENTER:
            browse_enter();
            break;
        case AG_KEY_UP:
        case AG_KEY_LEFT:
            browse_move(-1);
            break;
        case AG_KEY_DOWN:
        case AG_KEY_RIGHT:
            browse_move(1);
            break;
        default:
            break;
        }
        if (s_browse != was_browse) {
            dirty_all(); /* it covered most of the screen; all of it comes back */
        } else if (s_bsel != was_bsel) {
            dirty_browser();
        }
        return;
    }
    switch (k) {
    case AG_KEY_ESC:
    case AG_KEY_Q:
        if (s_editing) {
            s_editing = 0;
        } else {
            *quit = 1;
        }
        break;
    case AG_KEY_ENTER:
        activate();
        break;
    case AG_KEY_LEFT:
        if (s_editing) {
            nudge(-1);
        } else if (s_sel > 0) {
            s_sel--;
        }
        break;
    case AG_KEY_RIGHT:
        if (s_editing) {
            nudge(1);
        } else if (s_sel + 1 < item_count()) {
            s_sel++;
        }
        break;
    case AG_KEY_UP:
        if (s_editing) {
            nudge(1);
        } else {
            nav_vert(-1);
        }
        break;
    case AG_KEY_DOWN:
        if (s_editing) {
            nudge(-1);
        } else {
            nav_vert(1);
        }
        break;
    default:
        break;
    }
    if (s_sel != was_sel) {
        /* Two small rectangles rather than the screen: the one that stopped
         * being selected and the one that started. */
        dirty_item(was_sel);
        dirty_item(s_sel);
    }
}

static void on_tap(int px, int py)
{
    const int sw = (int)s_info.width, sh = (int)s_info.height;
    const int n = knob_n();
    int       i, x, y, w, h;

    if (s_browse != BROWSE_OFF) {
        int bx, by, bw, bh;
        browser_box(&bx, &by, &bw, &bh, sw, sh);
        if (px < bx || px >= bx + bw || py < by || py >= by + bh) {
            s_browse = BROWSE_OFF; /* a tap outside is the way out */
            dirty_all();
            return;
        }
        i = s_btop + (py - (by + 48)) / LIST_ROW;
        if (i >= 0 && i < s_n && s_kind[i] != ENT_HEAD) {
            if (i == s_bsel) {
                browse_enter(); /* the second tap is the Enter */
            } else {
                s_bsel = i;
                dirty_browser();
            }
        }
        return;
    }
    for (i = 0; i < n; i++) {
        knob_box(i, &x, &y, &w, &h, sw, sh);
        if (px >= x && px < x + w && py >= y && py < y + h) {
            /* A tap picks a knob and opens it in one go - a finger should not
             * have to select and then confirm. */
            dirty_item(s_sel);
            s_sel = i;
            s_editing = 1;
            dirty_all(); /* the slider goes over its neighbours */
            return;
        }
    }
    for (i = 0; i < 3; i++) {
        button_box(i, &x, &y, &w, &h, sw, sh);
        if (px >= x && px < x + w && py >= y && py < y + h) {
            dirty_item(s_sel);
            s_sel = n + i;
            dirty_item(s_sel);
            if (s_editing) {
                s_editing = 0;
                dirty_all();
            }
            activate();
            return;
        }
    }
}

int ag_main(int argc, char **argv)
{
    int quit = 0;
    int i, want_bench = 0;

    for (i = 1; i < argc; i++) {
        const int m = ag_amp_model_by_name(argv[i]);
        if (m >= 0) {
            s_model = m;
        } else if (strcmp(argv[i], "bench") == 0) {
            want_bench = 1;
        }
    }
    set_model(s_model);

    /* Acquired once and kept: releasing between frames hands the panel back to
     * the console, which then writes its own lines over the amplifier. */
    if (ag_gfx_acquire(&s_info) != AG_OK) {
        ag_printf("stomp: the display belongs to somebody else\n");
        return 1;
    }
    if (want_bench) {
        bench(); /* releases the display itself, so the report is readable */
        return 0;
    }
    ag_printf("stomp: %s, %d knobs, %ux%u\n", ag_amp_model_name(s_model),
              ag_amp_pot_count(s_model), (unsigned)s_info.width,
              (unsigned)s_info.height);
    draw();

    while (!quit) {
        ag_event_t ev;
        if (!ag_poll_event(&ev, 100)) {
            if (ag_interrupted()) {
                quit = 1;
            }
            continue;
        }
        switch (ev.type) {
        case AG_EV_QUIT:
            quit = 1;
            break;
        case AG_EV_FOCUS_GAINED:
            draw(); /* somebody else had the panel: none of it is ours */
            break;
        case AG_EV_KEY_DOWN:
            on_key(ev.key.keycode, &quit);
            break;
        case AG_EV_POINTER_DOWN:
            on_tap(ev.ptr.x, ev.ptr.y);
            break;
        default:
            break;
        }
        /* Nothing marked, nothing painted: an event that changed no pixel
         * costs no drawing at all. */
        present();
        if (ag_interrupted()) {
            quit = 1;
        }
    }
    ag_gfx_release();
    return 0;
}
