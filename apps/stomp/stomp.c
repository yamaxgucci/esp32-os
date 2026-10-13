/*
 * STOMP - the valve amplifier with its knobs on a screen, or in text.
 *
 *   run a:\stomp.axe [model]
 *
 * TWO FACES, ONE SET OF KNOBS
 *
 * With a panel it paints them.  With no panel it draws the same knobs as text
 * on the console - which on the Audio Kit is a phone, because PHONE.SYS serves
 * the console over HTTP with a keyboard under it, and a pedal with no screen
 * is still a pedal whose gain has to be turned up somehow.  `notext` leaves
 * the console alone for a run that is being measured.
 *
 * Up and down pick a row, left and right turn it, a digit sets it outright and
 * `=` is the tenth mark.  There is no slider to open first: a row of text
 * already shows the number the painted knob needs a slider to show.
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
#include "ag_ckt.h"
#include "ag_ir.h"
#include "ag_pcm.h"
#include "ag_wav.h"

/*
 * Not AG_AXE_NEEDS_GFX.  A pedal is a box with knobs and no screen, and the
 * S3-Zero on the desk is exactly that: no display, sound out of a CS4344 on
 * I2S.  With no panel this runs headless - the amplifier plays and the console
 * is the only thing that says so.
 */
/*
 * And a named arena, because the default one is not enough to hold what this
 * needs at once: 96 KB of curves, the chain, the cabinet's buffers, and a take
 * to play through them.  The default left a largest free block of 192 KB
 * against a seven-second take of 308, so the take was loaded in part and the
 * loop jumped in the middle of the phrase - which is what it sounded like.
 *
 * A size named here is required rather than preferred: a board that cannot give
 * it refuses to start this, which is the right answer.  Half the S3-Zero's
 * PSRAM: 512 KB was not enough, because by the time the take is read the
 * curves and the chain have already taken their share and the largest free
 * block was 264 KB against a 308 KB take.
 */
AG_APP_SIZED("STOMP", "1.0", "argon", 0, 12 * 1024, 1024 * 1024);

#define ROW_KNOBS 3  /* three across; six knobs make two rows, three make one */
#define TOP_H     26 /* the model's name */
#define BAR_H     36 /* the row of buttons */
#define LIST_MAX  64 /* entries a chooser holds; more are counted, not shown */
#define LIST_ROW  18

/* How long an open slider waits for a hand that has gone back to the guitar. */
#define SLIDER_IDLE_MS 5000u

/*
 * How much room before the next block is due is enough to draw in.
 *
 * It was six milliseconds, which is right for a whole painted panel (4.3 ms)
 * against an 11.6 ms block.  On an instrument the block is 64 frames - 2.9 ms
 * - and six milliseconds of slack never exist, so the panel never redrew at
 * all: a phone looking at the board got a picture that was declared and never
 * sent, which reads as a page that opened and stayed empty.
 *
 * It does not need six.  A full repaint is already painted one band of eight
 * at a time while sound is running, and a band is about half a millisecond;
 * the dirty rectangles of a turned knob are smaller again.  So the test is
 * whether there is room for one step, not for the whole picture - and the
 * whole picture arrives over eight blocks, which is twenty-three milliseconds
 * and invisible to anybody watching.
 */
#define REDRAW_SLACK_US 700

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

/*
 * The rate the chain is fitted and measured at, and the block everything moves
 * in - AG_IR_BLOCK, because the cabinet's overlap-add works in exactly that and
 * a second block size would mean a second buffer for no gain.  256 frames at
 * 22.05 kHz is 11.6 ms.
 */
#define RATE  22050u
#define CHUNK AG_IR_BLOCK

/*
 * HOW FAR PAST FULL SCALE THE VALVES ARE ALLOWED TO GO.
 *
 * The cabinet works in int16, so the valves' output has to become int16 to
 * reach it - and a valve amplifier with the drive up is not a unity-gain
 * thing.  Measured on this board with the take, jcm800 at drive 9 and master
 * 8: the chain peaks at 2.3 to 3.7 times full scale, and every sample above
 * 1.0 was flattened against the rail on its way into the loudspeaker.  Twelve
 * thousand samples a second of it - about a third of them - all of it
 * audible, and worse the harder the playing, which is exactly how it was
 * described.
 *
 * That clipping is not a sound anybody chose.  The valve model does its own
 * clipping, on its own curve, which is the amplifier's voice; this one was a
 * word size.  So the valves are given two bits of room before the conversion
 * and the room is taken back after the cabinet, where the trim already
 * saturates and already says CLIP.
 *
 * It costs two bits of the sixteen going into the convolution.  Against the
 * engine's measured error - about a quarter of one least significant bit -
 * that is noise at -63 dB instead of -75 dB, which is still far below a
 * loudspeaker's own floor, and the cabinet now gets the signal the valves
 * actually made rather than a square-topped copy of it.
 */
#define AMP_HEAD 4

/*
 * WHERE THE MASTER STOPS BEING LINEAR.
 *
 * Seven tenths: below it the knee does nothing, so everything played at a
 * sane level is untouched, and the amplifier's own voice is what comes out.
 * Above it the curve bends over and reaches full scale with zero slope, and
 * the knee closes over twice the space that is left - which is what makes
 * the slope continuous where it starts.
 */
#define OUT_KNEE_T 0.70f

/*
 * How many frames the loop actually does at a time, which is latency.
 *
 * CHUNK above is only the size of the buffers - the most this will ever ask
 * for.  What it asks for is s_chunk, and on an instrument that number is the
 * whole argument: 256 frames at 22050 Hz is 11.6 ms between a string being
 * struck and the first sample of it leaving, with a converter buffer on each
 * side of that.  A guitarist feels twenty milliseconds.
 *
 * It can be smaller than a block only because the cabinet no longer needs one:
 * a short impulse is convolved directly (ag_ir_process_n), and it is the
 * transform that forces whole blocks.
 */
static uint32_t     s_chunk = CHUNK;
static int          s_quiet; /* no periodic status; see `quiet` */

static ag_gfxinfo_t s_info;  /* the display, held for as long as we run */
static int          s_headless; /* no panel on this board: play, do not draw */

/*
 * THE SAME AMPLIFIER, DRAWN AS TEXT
 *
 * A pedal has no screen, and the thing looking at it is a phone: PHONE.SYS
 * serves the console as 40x25 cells over HTTP, with a keyboard under it.  So
 * the knobs want a text face as well as a painted one - not a second set of
 * knobs, the same ones.  Everything below asks the model how many controls it
 * has and what they are called, exactly as the painted panel does, so a model
 * with a gain pot between its valves shows six rows and the pedal shows three,
 * and neither list is written down here.
 *
 * It rides on the same dirty marks the painted panel uses.  A knob turned by
 * any route - a key, a tap, `pot drive=8` on the command line - calls
 * dirty_item, and that is what makes the row redraw; there is no second path
 * that could disagree with the first.
 *
 * One row at a time is the whole point.  The console is a serial line as well
 * as a phone, and forty characters is a millisecond of it against 2.9 ms of
 * audio in a block, where the whole screen would be ten blocks and a click in
 * the sound on every keystroke.
 */
static int      s_text = 1;  /* draw the amplifier as text when headless */
static int      s_text_all;  /* the panel, whole */
static uint32_t s_text_rows; /* which item rows changed; bit per item */
static uint16_t s_text_w = 40, s_text_h = 25;

/* The chain, when there is one.  Large enough to be worth the heap. */
static ag_amp_t *s_amp;
static float    *s_tab;
/* One block per stage when internal memory will give them; see tab_reserve. */
static float       *s_tabs[AG_AMP_STAGES];
static int          s_tab_split;
static ag_ir_t   s_ir;
static int       s_ir_ready;

static ag_pcm_t     s_out;
static int          s_sound;   /* the sink is open */
static ag_wav_pcm_t s_di;      /* what is being played through it */
static uint32_t     s_di_at;
static int          s_di_loop = 1; /* a file sink plays the take once */
static int          s_take_done;
/*
 * What comes in, when something does.  Stereo-sized because the converter is
 * opened in stereo: one jack is the guitar and the other is whatever else the
 * board wired, and de-interleaving here is cheaper than a second open.
 */
static int16_t      s_in[CHUNK * 2];
static int          s_in_ch = 2;
static int          s_live;        /* the input is a converter, not a file */
static int          s_in_sum;      /* add both channels in; see `inmix` */
/* What each part of a block costs, summed over blocks and divided when said. */
static uint32_t     s_us_in;
static uint32_t     s_us_valves;
static uint32_t     s_us_cab;
static uint32_t     s_us_out;    /* waiting for the converter to take it */
static uint32_t     s_us_in_blk; /* this block's wait, kept out of the load */
static uint32_t     s_us_fx;   /* what a borrowed effect costs a block */
static uint32_t     s_us_total;
static uint32_t     s_us_blocks;
static uint32_t     s_in_starved;  /* blocks the converter had nothing for */
/* Frames of silence padded into the live input since the last status line,
 * which is the size of the hole a listener hears. */
static uint32_t     s_in_holes;
/* Samples the master's knee touched since the last status line. */
static uint32_t     s_soft;
/*
 * The longest stretch of real work in one block since the run began - the
 * turn's length with the two waits taken out.  Longer than a block and the
 * converter ran dry while this was going on, which is the hole a listener
 * hears; the turn's own length says nothing, because most of a long turn is
 * usually the write being held at a full ring.
 */
static uint32_t     s_worst_busy;
/* The longest single wait for the input, since the run began. */
static uint32_t     s_worst_in;
/*
 * And the same two for the window just ended.
 *
 * A worst-since-start is what somebody who has been playing wants; it is also
 * poisoned by the first block of the run, where the codec is still waking and
 * one wait of 71 ms says nothing about the music.  So both are kept, and the
 * window pair is what a measurement reads.
 */
/* Samples the converter sent at its own full scale, run and window. */
/* `stats`: the counters into the journal every window.  Off, because writing
   them costs the audio thread the thing they are there to measure. */
static int          s_journal;
/* `noadaa`: the antialiasing off, to measure what it costs.  Never to play. */
static int          s_no_adaa;
static uint32_t     s_in_clip;
static uint32_t     s_in_clip_win;
/* The worst single pass through each stage in the window just ended. */
/* And the worst of the whole run, which is what the end reports. */
static uint32_t     s_run_v_max;
static uint32_t     s_run_c_max;
static uint32_t     s_win_v_max;
static uint32_t     s_win_c_max;
static uint32_t     s_win_in;
static uint32_t     s_win_busy;
/* The last window's averages, for the journal: valves, cabinet, whole block. */
static uint32_t     s_av_valves;
static uint32_t     s_av_cab;
static uint32_t     s_av_work;
static uint32_t     s_av_budget;
/*
 * And how many blocks did not finish in their own time at all.
 *
 * The worst is one number and can be a single moment at start-up; this is
 * the count of the holes, which is what somebody playing is actually
 * counting without knowing it.
 */
static uint32_t     s_over;
static int16_t      s_mono[CHUNK];
static int16_t      s_stereo[CHUNK * 2];
static float        s_peak;     /* of the last block at the converter */
static float        s_amp_peak; /* and at the amplifier, before the cabinet */
/* The output trim as a numerator over 64, so a block is one shift and no
 * floating point: 64 is unity, 128 is +6 dB, 255 is +12. */
static int          s_vol_num = 64;
static uint32_t     s_out_clipped;
static uint32_t     s_cab_ms;  /* how much of the impulse to keep, 0 = all */
static uint32_t     s_hdr_ms;  /* when the meter was last redrawn */
static uint32_t     s_clipped;
/*
 * And the same, counted since the last status line rather than since the
 * start.
 *
 * The total was only ever printed when the run ended, and a pedal does not
 * end - so the one number that says "what you are hearing is not the
 * amplifier, it is the rail" was invisible for exactly as long as it
 * mattered.  The valves hand their output to the cabinet as int16, and a
 * valve amplifier with the drive up goes past full scale long before it runs
 * out of anything else: louder in, harder against the rail, and the harshness
 * grows with the playing, which is what a listener describes as artefacts
 * that get worse the louder it gets.
 */
static uint32_t     s_clipped_win;
static uint32_t     s_blocks;  /* blocks written since the sink opened */
static uint32_t     s_short;   /* writes the sink would not take whole */
static uint32_t     s_say_ms;  /* when the console last heard how it goes */
static char         s_hdr_shown[96]; /* what the top strip last said */
static int          s_tab_n;   /* points the curve buffer is sized for */
static int          s_tab_fast; /* and whether it got internal SRAM */
static int          s_tab_inside;
/* Arrays per stage: 3 with the running sums, 2 without. */
static int          s_tab_parts = 3; /* how many stages' curves are in SRAM */
static int          s_tab_stages; /* and for how many stages */
static char         s_status[64]; /* one line about the sound, or empty */
static ag_amp_cfg_t s_cfg;
/* The configuration as the chain arrived with it.  The level knobs scale what
 * the fit calibrated, so the calibration has to live somewhere a knob does not
 * write. */
static ag_amp_cfg_t s_fit;
static int          s_model = AG_AMP_MODEL_JCM800;
static int          s_sel;     /* knobs, then cabinet, switch, preset */
static int          s_editing; /* the slider is open on s_sel */
/*
 * A finger on the slider, and when it was last felt.
 *
 * The slider is drawn *over* the knobs, and until now nothing tested it: a tap
 * on it fell through to the knob underneath, so trying to move it picked a
 * different knob instead.  It has to take the tap itself, follow the finger
 * while it is down, and put itself away - a control that can only be opened is
 * a control in the way.
 */
static int          s_drag;      /* the finger is on the slider now */

/*
 * THE PANEL IS PAINTED BY ITS OWN THREAD
 *
 * It used to be painted between audio blocks, when there was room: the loop
 * asked how long until the converter was next due and drew if the answer was
 * big enough.  On an instrument that answer is almost never big enough.  A
 * 64-frame block is 2.9 ms, and with a phone attached the board runs at 86 to
 * 98 per cent of its core - so the panel was drawn in the crumbs, and a whole
 * screen took the best part of nine seconds to arrive.
 *
 * A picture has no business competing with the block loop.  The painter now
 * has a thread of its own on the *system* core, which is the one the audio is
 * not on (applications run on the application core), so drawing costs the
 * amplifier nothing but the memory bus.
 *
 * What crosses between them is one flag and one semaphore.  Everything that
 * changes the picture marks it dirty and nudges the painter; the painter is
 * the only thing that touches the graphics interface after start-up, so there
 * is no second painter to race with.  The dirty *rectangles* are not shared:
 * with a thread, a mark means the whole panel, which costs eight bands on a
 * core that has nothing else to do and removes the list that two threads would
 * otherwise have to agree about.
 */
/*
 * AN EFFECT SOMEBODY ELSE WROTE
 *
 * Not linked in, not known at build time: a device of class AG_DEV_FX, found
 * by name in the registry at run time and called through its class table.
 * `fx <name>` picks one, `fx` alone takes the first there is.
 *
 * It is called once a block, after the cabinet and before the level, because
 * that is where a pedal's effects loop is - the amplifier makes the sound and
 * what comes after colours it.
 *
 * Holding the handle for the length of the run is what keeps the module from
 * being unloaded out from under the audio thread; the registry counts it.
 */
static ag_handle_t     s_fx = -1;
static const ag_fx_ops_t *s_fx_ops;
static char            s_fx_name[AG_DEV_NAME_MAX];
static int             s_fx_amount = -1; /* -1: leave whatever it came with */

static ag_thread_t  s_painter;
static ag_sem_t     s_paint_wake;
static ag_mutex_t   s_paint_lock; /* the dirty marks, and nothing else */
static volatile int s_paint_stop;
/*
 * Whether anybody is looking.  Cleared when the slot is left and set when it
 * comes back; while it is clear the picture is not drawn and not sent, and
 * the marks that would have been drawn are simply kept until it returns.
 */
static volatile int s_on_screen = 1;

/*
 * THE LOUDSPEAKER ON THE OTHER CORE.
 *
 * It is the second half of the chain and it does not need to be in the same
 * thread as the first: the valves make a block, hand it over, and send out
 * the block the loudspeaker finished while they were working.  One block of
 * latency, which at 64 frames is 2.9 ms, and a fifth to a third of the audio
 * thread's time given back.
 *
 * One buffer each way and two semaphores.  Nothing is shared except those:
 * once the convolution thread is running it is the only one that touches
 * ag_ir, which is why it is stopped around anything that rebuilds the
 * cabinet.
 */
static ag_thread_t  s_cab_thread;
static ag_sem_t     s_cab_go;
static ag_sem_t     s_cab_done;
static volatile int s_cab_quit;
static int          s_cab_primed; /* the first block has nothing to send yet */
/*
 * Asked for by `cab2`, and off otherwise.
 *
 * The other core is for effects - a reverb, a delay, a tremolo, each its own
 * program - and a chain that has taken it for its own loudspeaker has
 * nowhere to put them.  What has to be made to work is one core.
 */
static int          s_cab_two_core;
static int16_t      s_cab_in[CHUNK];
static int16_t      s_cab_out[CHUNK * 2];

/*
 * The marks are shared, so they are locked - and the lock is held for the
 * length of an append or a snapshot, never for the length of a paint.
 *
 * The first attempt avoided the lock by making every mark a whole panel.  That
 * worked and cost too much: the meter marks the top strip four times a second
 * and each one became eight bands, half a megabyte of wire in twenty seconds,
 * and the amplifier went from 55 resyncs to 1791.  Two rectangles for a turned
 * knob is the whole reason the dirty list exists.
 */
static void paint_lock(void)
{
    if (s_paint_lock != NULL) {
        (void)ag_mutex_lock(s_paint_lock, 50u);
    }
}

static void paint_unlock(void)
{
    if (s_paint_lock != NULL) {
        ag_mutex_unlock(s_paint_lock);
    }
}
static uint32_t     s_touch_ms;  /* when it was last touched or typed at */
static int          s_cab_on = 1;
static char         s_cab[AG_PATH_MAX] = "";
static char         s_preset[AG_PATH_MAX] = "jcm800";

static int  s_browse;
/*
 * The settings menu, behind the gear in the top strip.
 *
 * Not a knob and not in the amplifier: these are the machine's settings - what
 * the block is, and therefore what the latency is - and they are reached by
 * touch because that is how this pedal is actually driven, from a phone or
 * from the glass, with no keyboard anywhere near it.
 */
static int  s_menu;      /* 0 closed, 1 open         */
static int  s_msel;      /* which row the finger last landed on */
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

/*
 * The gear, and the square a finger has to hit to reach it.
 *
 * The drawn gear is the height of the strip; the target is wider and taller,
 * because a fingertip is about nine millimetres and the strip is twenty-six
 * pixels.  Being generous here costs nothing - there is nothing else in that
 * corner to hit by mistake.
 */
#define GEAR_W 30

static void gear_box(int *x, int *y, int *w, int *h, int sw)
{
    *x = sw - GEAR_W;
    *y = 0;
    *w = GEAR_W;
    *h = TOP_H;
}

static void gear_target(int *x, int *y, int *w, int *h, int sw)
{
    *x = sw - 44;
    *y = 0;
    *w = 44;
    *h = TOP_H + 10;
}

#define MENU_ROW  26 /* a value, and a finger can hit it */
#define MENU_HEAD 17 /* a heading, which nothing taps */

enum { MROW_HEAD, MROW_BLK, MROW_OS };

static const struct {
    uint8_t     kind;
    uint16_t    val;
    const char *label;
    const char *note;
} k_menu[] = {
    { MROW_HEAD, 0u, "LATENCY", "" },
    { MROW_BLK, 256u, "256", "about 58 ms" },
    { MROW_BLK, 128u, "128", "about 29 ms" },
    { MROW_BLK, 64u, "64", "about 14 ms" },
    { MROW_HEAD, 0u, "VALVES", "" },
    { MROW_OS, 1u, "1x", "cheapest" },
    { MROW_OS, 2u, "2x", "all four models" },
    { MROW_OS, 4u, "4x", "ts9, jcm800" },
};
#define MENU_N ((int)(sizeof(k_menu) / sizeof(k_menu[0])))

/* Where row i starts, measured from the top of the menu's box. */
static int menu_row_y(int i)
{
    int y = 8, k;
    for (k = 0; k < i; k++) {
        y += (k_menu[k].kind == MROW_HEAD) ? MENU_HEAD : MENU_ROW;
    }
    return y;
}

static void menu_box(int *x, int *y, int *w, int *h, int sw, int sh)
{
    const int bw = (sw < 220) ? sw - 24 : 196;
    *w = bw;
    *h = menu_row_y(MENU_N) + 8;
    *x = sw - bw - 6;
    *y = TOP_H + 2;
    if (*y + *h > sh) {
        *y = sh - *h; /* a short screen keeps it on the glass */
    }
    if (*y < 0) {
        *y = 0;
    }
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

/*
 * Where the finger is on the track, as one of the ten marks.
 *
 * The track is the same 28..28+(sw-48) draw_slider paints, and it is written
 * out here rather than shared with a constant because the painter is the
 * definition: a number here that disagreed with the picture would be a slider
 * that moves to somewhere other than where it was touched, which is worse than
 * one that does not move at all.
 *
 * Half a mark is added before the division so that the mark nearest the finger
 * wins rather than the one below it - a tap in the middle of 7 and 8 that
 * always chose 7 reads as a slider that lags behind the hand.
 */
static int slider_mark_at(int px, int sw)
{
    const int w = sw - 48;
    int       v;

    if (w <= 0) {
        return 0;
    }
    v = ((px - 28) * 10 + w / 2) / w;
    if (v < 0) {
        v = 0;
    }
    if (v > 10) {
        v = 10;
    }
    return v;
}

/*
 * A gear out of rectangles, because there are no circles in this interface.
 *
 * Four teeth around a rounded hub with a hole in it reads as a gear at
 * twenty-six pixels; a drawn circle would not have been any clearer and
 * would have needed a primitive that is not there.
 */
static void draw_gear(int sw)
{
    int        x, y, w, h;
    const uint32_t ink = s_menu ? rgb(245, 220, 150) : rgb(150, 156, 178);
    int        cx, cy;

    gear_box(&x, &y, &w, &h, sw);
    ag_gfx_fill_rect((int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h,
                     rgb(16, 17, 22));
    cx = x + w / 2;
    cy = y + h / 2;
    /* the teeth */
    ag_gfx_fill_rect((int16_t)(cx - 2), (int16_t)(cy - 9), 4, 4, ink);
    ag_gfx_fill_rect((int16_t)(cx - 2), (int16_t)(cy + 5), 4, 4, ink);
    ag_gfx_fill_rect((int16_t)(cx - 9), (int16_t)(cy - 2), 4, 4, ink);
    ag_gfx_fill_rect((int16_t)(cx + 5), (int16_t)(cy - 2), 4, 4, ink);
    /* the hub, and the hole through it */
    ag_gfx_fill_round_rect((int16_t)(cx - 6), (int16_t)(cy - 6), 12, 12, 4,
                           ink);
    ag_gfx_fill_round_rect((int16_t)(cx - 2), (int16_t)(cy - 2), 4, 4, 1,
                           rgb(16, 17, 22));
}

/*
 * The settings menu: one setting so far, and it is the one that cannot be
 * judged from a datasheet.
 *
 * Each row says the block, what it costs in delay through the whole chain,
 * and the one before it is marked - so the choice is made by reading what it
 * does rather than by remembering what 128 meant.
 */
static void draw_menu(int sw, int sh)
{
    int  x, y, w, h, i;
    char line[48];

    menu_box(&x, &y, &w, &h, sw, sh);
    ag_gfx_fill_round_rect((int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 8,
                           rgb(70, 76, 94));
    ag_gfx_fill_round_rect((int16_t)(x + 2), (int16_t)(y + 2),
                           (uint16_t)(w - 4), (uint16_t)(h - 4), 7,
                           rgb(24, 26, 33));

    for (i = 0; i < MENU_N; i++) {
        const int ry = y + menu_row_y(i);
        int       here;

        if (k_menu[i].kind == MROW_HEAD) {
            ag_gfx_text((int16_t)(x + 12), (int16_t)(ry + 3), k_menu[i].label,
                        rgb(240, 210, 130), AG_GFX_TRANS);
            continue;
        }
        here = (k_menu[i].kind == MROW_BLK)
                   ? ((uint32_t)k_menu[i].val == s_chunk)
                   : ((int)k_menu[i].val == s_cfg.os);
        if (i == s_msel) {
            ag_gfx_fill_round_rect((int16_t)(x + 8), (int16_t)ry,
                                   (uint16_t)(w - 16), MENU_ROW - 2, 4,
                                   rgb(48, 54, 72));
        }
        (void)snprintf(line, sizeof(line), "%s%-4s %s",
                       here ? "> " : "  ", k_menu[i].label, k_menu[i].note);
        (void)ag_gfx_text_fit((int16_t)(x + 14), (int16_t)(ry + 5),
                              (uint16_t)(w - 28), line,
                              here ? rgb(245, 220, 150) : rgb(210, 215, 230),
                              AG_GFX_TRANS);
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

/* Named, because the painter takes a copy of them before it draws. */
typedef struct {
    short x, y, w, h;
} rect_t;

static rect_t s_dirty[DIRTY_MAX];
static int s_ndirty;
static int s_dirty_all;
static int s_band; /* which band of a full repaint comes next */

static void dirty_all(void)
{
    paint_lock();
    s_text_all = 1;
    s_dirty_all = 1;
    s_band = 0;
    s_ndirty = 0;
    paint_unlock();
    if (s_paint_wake != NULL) {
        ag_sem_give(s_paint_wake);
    }
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
    paint_lock();
    if (s_ndirty < DIRTY_MAX) {
        s_dirty[s_ndirty].x = (short)x;
        s_dirty[s_ndirty].y = (short)y;
        s_dirty[s_ndirty].w = (short)w;
        s_dirty[s_ndirty].h = (short)h;
        s_ndirty++;
    }
    paint_unlock();
    if (s_paint_wake != NULL) {
        ag_sem_give(s_paint_wake);
    }
}

/* A knob's cell or a button, whichever this index is. */
static void dirty_item(int i)
{
    const int sw = (int)s_info.width, sh = (int)s_info.height;
    int       x, y, w, h;
    if (i < 0 || i >= item_count()) {
        return;
    }
    /* The text panel's row for the same item.  Marked here rather than at
     * every call site, so that a knob turned by any route redraws by the one
     * path the painted panel already uses. */
    if (i < 32) {
        s_text_rows |= (uint32_t)1 << i;
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
/*
 * What the top strip reads, in one place.
 *
 * Here rather than inside the painter because the loop has to ask the same
 * question to decide whether the strip is worth redrawing at all - and a
 * second copy of this format would be two answers to "has it changed".
 */
static void head_text(char *out, size_t n)
{
    if (s_status[0] != '\0') {
        (void)snprintf(out, n, "%s", s_status);
        return;
    }
    if (!s_sound) {
        (void)snprintf(out, n, "silent");
        return;
    }
    const int meter = (int)(s_peak * 20.0f);
    char      bar[24];
    int       k;
    for (k = 0; k < 20; k++) {
        bar[k] = (k < meter) ? '=' : '.';
    }
    bar[20] = '\0';
    /* No block here: on a 320-wide panel this line is already cut off before
     * the load is reached, and the gear's menu says it with room to spare. */
    (void)snprintf(out, n, "%s  %s  %u%% cpu", s_out.path, bar,
                   (unsigned)s_out.load_pct);
}

static void paint_region(int rx, int ry, int rw, int rh)
{
    const int sw = (int)s_info.width;
    const int sh = (int)s_info.height;
    int       i, x, y, w, h;

    ag_gfx_fill_rect((int16_t)rx, (int16_t)ry, (uint16_t)rw, (uint16_t)rh,
                     rgb(16, 17, 22));
    if (hits(rx, ry, rw, rh, 0, 0, sw, TOP_H)) {
        char head[96];
        ag_gfx_text(6, 6, ag_amp_model_name(s_model), rgb(235, 235, 240),
                    AG_GFX_TRANS);
        /* What the sound is doing, on the same line as what is playing it:
         * the load is the number that says whether this box can keep up. */
        head_text(head, sizeof(head));
        /* The strip stops short of the gear rather than running under it. */
        (void)ag_gfx_text_fit(120, 6, (uint16_t)(sw - 126 - GEAR_W), head,
                              s_status[0] != '\0' ? rgb(240, 200, 130)
                                                  : rgb(130, 138, 158),
                              AG_GFX_TRANS);
        draw_gear(sw);
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
    /* Last, so it is over everything: it is the thing being touched. */
    if (s_menu) {
        menu_box(&x, &y, &w, &h, sw, sh);
        if (hits(rx, ry, rw, rh, x, y, w, h)) {
            draw_menu(sw, sh);
        }
    }
}

/* Painting the whole thing, which is what the first frame and the bench want. */
static void paint(void)
{
    paint_region(0, 0, (int)s_info.width, (int)s_info.height);
}

/*
 * A whole screen, one band at a time.
 *
 * A full repaint is 15.9 ms on the board - measured, and longer than the 11.6 ms
 * block it would have to fit inside, because the framebuffer is in PSRAM like
 * everything else.  Painting it in one go while the audio is running means a
 * block that arrives late however much slack there was beforehand.
 *
 * So a full repaint is a queue rather than an act: one band per present(), each
 * a fraction of the height, each painted and flushed on its own.  The screen
 * fills in over a few blocks and no single block pays for all of it.  Nothing
 * moves in between, because a band is painted from the same state the last one
 * was - it is one picture arriving in pieces, not several pictures.
 */
/*
 * THE PANEL, AS TEXT
 *
 * Laid out from the model rather than from a list here: however many controls
 * ag_amp_pot_count reports, with the names ag_amp_pot_name gives them, then
 * the same three buttons the painted bar carries.  A model that grows a knob
 * grows a row, and nothing in this file has to be told.
 */
#define TXT_TOP 2 /* the model's name, a rule, then the items */

static int text_row_of(int item) { return TXT_TOP + item; }

/* One row, padded to the width, written where it is rather than printed: a
 * print at the bottom of the screen scrolls the panel off it. */
static void text_line(int row, const char *str)
{
    char     buf[81];
    uint16_t i;
    uint16_t w = s_text_w;

    if (row < 0 || row >= (int)s_text_h) {
        return;
    }
    if (w > (uint16_t)(sizeof(buf) - 1)) {
        w = (uint16_t)(sizeof(buf) - 1);
    }
    for (i = 0; i < w; i++) {
        buf[i] = (str[i] != '\0') ? str[i] : ' ';
        if (str[i] == '\0') {
            break;
        }
    }
    for (; i < w; i++) {
        buf[i] = ' ';
    }
    buf[w] = '\0';
    ag_gotoxy(0, (uint16_t)row);
    (void)ag_print(buf);
}

/*
 * A knob as a row: its name, a bar of ten, and the number under the mark.
 *
 * Ten positions because that is what a pedal is marked with, and the same
 * `marks()` the painted knob uses - two faces of one number, never two
 * numbers.
 */
static void text_item(int i)
{
    const int n = knob_n();
    char      line[81];
    char      bar[12];
    int       j;

    if (i < 0 || i >= item_count()) {
        return;
    }
    if (i < n) {
        const int v = marks(s_cfg.pot[ag_amp_pot_id(s_model, i)]);
        for (j = 0; j < 10; j++) {
            bar[j] = (j < v) ? '#' : '-';
        }
        bar[10] = '\0';
        (void)snprintf(line, sizeof(line), "%c %-6s %s %2d",
                       (i == s_sel) ? '>' : ' ',
                       ag_amp_pot_name(s_model, i), bar, v);
    } else if (i == n) {
        (void)snprintf(line, sizeof(line), "%c cab    %s",
                       (i == s_sel) ? '>' : ' ',
                       s_cab[0] != '\0' ? leaf(s_cab) : "from the preset");
    } else if (i == n + 1) {
        (void)snprintf(line, sizeof(line), "%c cab is %s",
                       (i == s_sel) ? '>' : ' ', s_cab_on ? "ON" : "OFF");
    } else {
        (void)snprintf(line, sizeof(line), "%c preset %s",
                       (i == s_sel) ? '>' : ' ', leaf(s_preset));
    }
    text_line(text_row_of(i), line);
}

/* What the periodic status says, in two rows that stay where they are instead
 * of scrolling the panel away. */
static void text_status(uint32_t budget, uint32_t total, uint32_t cpu)
{
    char      line[81];
    const int row = text_row_of(item_count()) + 1;

    (void)snprintf(line, sizeof(line), "  %u of %u us   %u%% cpu   %u resync",
                   (unsigned)total, (unsigned)budget, (unsigned)cpu,
                   (unsigned)s_out.resync);
    text_line(row, line);
    (void)snprintf(line, sizeof(line), "  in %2u%%  out %2u%%%s",
                   (unsigned)(s_amp_peak * 100.0f),
                   (unsigned)(s_peak * 100.0f),
                   s_out_clipped > 0u ? "   CLIP" : "");
    text_line(row + 1, line);
}

static void text_all(void)
{
    char line[81];
    int  i;

    (void)snprintf(line, sizeof(line), "STOMP  %s  %dx  cab %u ms",
                   ag_amp_model_name(s_model), s_cfg.os,
                   (unsigned)s_cab_ms);
    text_line(0, line);
    for (i = 0; i < (int)s_text_w && i < (int)sizeof(line) - 1; i++) {
        line[i] = '-';
    }
    line[i] = '\0';
    text_line(1, line);
    for (i = 0; i < item_count(); i++) {
        text_item(i);
    }
    text_line(text_row_of(item_count()), "");
    text_line(text_row_of(item_count()) + 3, "");
    text_line(text_row_of(item_count()) + 4,
              "  up/down pick   left/right turn");
    text_line(text_row_of(item_count()) + 5,
              "  0-9 set   enter choose   q quit");
}

static void text_present(void)
{
    if (s_text_all) {
        text_all();
    } else {
        int i;
        for (i = 0; i < item_count() && i < 32; i++) {
            if ((s_text_rows & ((uint32_t)1 << i)) != 0u) {
                text_item(i);
            }
        }
    }
    s_text_all = 0;
    s_text_rows = 0;
}

#define BANDS 8

/* What the panel costs the machine, for the window just ended: how many
   passes and how long they took.  The audio thread does not paint, so this
   is time on the system core - but it is memory traffic the audio thread
   competes with, which is why it is worth a number. */
static uint32_t     s_paints;
static uint32_t     s_paint_us;

static void present_body(void)
{
    int i;
    if (s_text) {
        text_present();
        s_dirty_all = 0;
        s_ndirty = 0;
        return;
    }
    if (s_headless) {
        s_dirty_all = 0;
        s_ndirty = 0;
        return;
    }
    /*
     * Take the marks, then let go, then draw.
     *
     * The painter runs on another core and the marks are made on this one, so
     * what is drawn has to be a snapshot: holding the lock across the paint
     * would stop the amplifier's thread for the length of a band every time it
     * reached for a knob.  Taken and released in microseconds; the painting
     * below touches nothing shared.
     */
    rect_t    mine[DIRTY_MAX];
    int       n_mine = 0;
    int       all;
    int       band_y = 0, band_h = 0;

    paint_lock();
    all = s_dirty_all;
    if (all && s_sound) {
        const int h = (int)s_info.height;
        const int band = (h + BANDS - 1) / BANDS;
        band_y = s_band * band;
        band_h = band;
        if (band_y + band_h > h) {
            band_h = h - band_y;
        }
        if (++s_band >= BANDS) {
            s_band = 0;
            s_dirty_all = 0;
        }
    } else if (!all) {
        n_mine = s_ndirty;
        for (i = 0; i < n_mine; i++) {
            mine[i] = s_dirty[i];
        }
    } else {
        s_dirty_all = 0;
    }
    s_ndirty = 0;
    paint_unlock();

    if (all && s_sound) {
        if (band_h > 0) {
            paint_region(0, band_y, (int)s_info.width, band_h);
            ag_gfx_flush(0, (uint16_t)band_y, s_info.width, (uint16_t)band_h);
        }
        return;
    }
    if (all) {
        paint();
        ag_gfx_flush(0, 0, s_info.width, s_info.height);
        return;
    }
    for (i = 0; i < n_mine; i++) {
        paint_region(mine[i].x, mine[i].y, mine[i].w, mine[i].h);
        ag_gfx_flush((uint16_t)mine[i].x, (uint16_t)mine[i].y,
                     (uint16_t)mine[i].w, (uint16_t)mine[i].h);
    }
}

/*
 * Paint here and now - but only when nothing else is painting.
 *
 * Several places want the screen brought up to date before they go away for a
 * second: baking a model's curves, reading a cabinet off the card.  They did
 * that by calling present() straight out, which was correct while the only
 * painter was this thread.
 *
 * With a painter thread it is two threads inside the same drawing at once, and
 * that is not a race that merely looks wrong: choosing a model from the panel
 * killed the application every time, with a write to address zero inside a ROM
 * memcpy - one thread resetting the band renderer's list while the other was
 * playing it back.  The mark has already woken the painter, so there is
 * nothing to do here but let it.
 */
/* Timed, so that what the panel costs is a number rather than an opinion. */
static void present(void)
{
    const ag_time_t t0 = ag_micros();
    present_body();
    s_paint_us += (uint32_t)(ag_micros() - t0);
    s_paints++;
}

static void present_now(void)
{
    if (s_painter == NULL && s_on_screen) {
        present();
    }
}

static void draw(void)
{
    dirty_all();
    present_now();
}

/*
 * The painter.  It waits to be told the picture changed, then draws it, and
 * the only thing it shares with the amplifier is that flag.
 *
 * present() paints one band of eight a call, so the loop below runs it until
 * nothing is dirty - a whole screen in one pass, on a core the audio is not
 * using.  The wait has a timeout as well as a semaphore so that a mark that
 * arrived while a pass was running is never slept through.
 */
static void paint_thread(void *arg)
{
    (void)arg;
    while (!s_paint_stop) {
        (void)ag_sem_take(s_paint_wake, 100u);
        /* Nobody is looking: the marks keep, and the core goes to the sound. */
        while (!s_paint_stop && s_on_screen &&
               (s_dirty_all || s_ndirty > 0)) {
            present();
        }
    }
    ag_thread_exit();
}

/* Started once the screen is known to exist; before that `draw` paints in
 * line, because there is nothing else running yet to disturb. */
/*
 * Find the effect and hold it open.
 *
 * Nothing here knows what it is or who wrote it: the registry is asked for
 * devices of class AG_DEV_FX, the name is matched if one was given, and what
 * comes back is a table of function pointers.  There is no MMU on this chip,
 * so a call into another module is an ordinary call - what makes it safe is
 * the handle, which the registry counts and which stops the module being
 * unloaded while the audio thread is inside it.
 */
static void fx_start(uint32_t rate, uint32_t max_frames, uint8_t channels)
{
    ag_devinfo_t info;
    uint32_t     i;

    /* Only when asked.  Taking the first effect that happens to be installed
     * would make the pedal sound different because something unrelated was
     * loaded, which is the opposite of what a registry is for. */
    if (s_fx_amount < 0) {
        return;
    }

    for (i = 0; i < 64u; i++) {
        if (ag_dev_enumerate(i, AG_DEV_FX, &info) != AG_OK) {
            break;
        }
        if (s_fx_name[0] != '\0' && strcmp(info.name, s_fx_name) != 0) {
            continue;
        }
        s_fx = ag_dev_open(info.name);
        if (s_fx < 0) {
            ag_printf("stomp: %s will not open (%d)\n", info.name, (int)s_fx);
            s_fx = -1;
            return;
        }
        s_fx_ops = (const ag_fx_ops_t *)ag_dev_ops(s_fx);
        if (!AG_HAS(s_fx_ops, process)) {
            ag_printf("stomp: %s is not an effect this build can call\n",
                      info.name);
            ag_dev_close(s_fx);
            s_fx = -1;
            s_fx_ops = NULL;
            return;
        }
        if (AG_HAS(s_fx_ops, prepare)) {
            (void)s_fx_ops->prepare(NULL, rate, max_frames, channels);
        }
        if (s_fx_amount >= 0 && AG_HAS(s_fx_ops, set_amount)) {
            (void)s_fx_ops->set_amount(NULL, (uint8_t)s_fx_amount);
        }
        (void)snprintf(s_fx_name, sizeof(s_fx_name), "%s", info.name);
        ag_printf("stomp: effect %s by %s%s%s\n", info.name, info.driver,
                  (s_fx_amount >= 0 && AG_HAS(s_fx_ops, amount_name))
                      ? ", " : "",
                  (s_fx_amount >= 0 && AG_HAS(s_fx_ops, amount_name))
                      ? s_fx_ops->amount_name(NULL) : "");
        return;
    }
    if (s_fx_name[0] != '\0') {
        ag_printf("stomp: no effect called %s is loaded\n", s_fx_name);
    } else {
        ag_printf("stomp: no effect is loaded (drv install one)\n");
    }
}

static void fx_stop(void)
{
    if (s_fx >= 0) {
        ag_dev_close(s_fx);
        s_fx = -1;
        s_fx_ops = NULL;
    }
}

static void painter_start(void)
{
    if (s_headless || s_text || s_painter != NULL) {
        return;
    }
    s_paint_wake = ag_sem_create(1, 1);
    if (s_paint_wake == NULL) {
        return; /* no semaphore: keep painting in line, which still works */
    }
    s_painter = ag_thread_create(paint_thread, NULL, "stomp.paint", 4096, 3,
                                 (uint32_t)AG_THREAD_SYS_CORE);
    if (s_painter == NULL) {
        ag_sem_delete(s_paint_wake);
        s_paint_wake = NULL;
        ag_mutex_delete(s_paint_lock);
        s_paint_lock = NULL;
        ag_printf("stomp: no thread for the panel; drawing between blocks\n");
        return;
    }
    ag_printf("stomp: the panel is painted on the system core\n");
}

static void painter_stop(void)
{
    if (s_painter == NULL) {
        return;
    }
    s_paint_stop = 1;
    ag_sem_give(s_paint_wake);
    (void)ag_thread_join(s_painter, 1000u);
    s_painter = NULL;
    ag_sem_delete(s_paint_wake);
    s_paint_wake = NULL;
    ag_mutex_delete(s_paint_lock);
    s_paint_lock = NULL;
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

/*
 * Microseconds, and what they mean against the two clocks that matter: a
 * thirty-a-second redraw, and a sample of audio.
 *
 * It used to print instructions, which was true under QEMU with -icount shift=0
 * and a plain lie on a board, where the same call reads a real clock.  One unit
 * that both machines can honestly produce is better than a number that is only
 * right on one of them and does not say which.
 */
static void bench_line(const char *label, uint64_t us, uint32_t n)
{
    const uint32_t each = (uint32_t)(us / (n > 0 ? n : 1u));
    /* Percent of the processor, in basis points, if this were done FPS times a
     * second - which is the pessimistic reading of "redraw on every event". */
    const uint32_t bp = (uint32_t)(((uint64_t)each * BENCH_FPS * 10000ull) /
                                   1000000ull);
    const uint32_t samples =
        (uint32_t)(((uint64_t)each * BENCH_RATE) / 1000000ull);
    ag_printf("  %s %8u us  %3u.%02u%% at %u fps  %5u samples of audio\n",
              pad18(label), (unsigned)each, bp / 100u, bp % 100u,
              (unsigned)BENCH_FPS, samples);
}

/*
 * What the chain costs in time, on whatever this is running on.
 *
 * The drawing bench above counts instructions because that is all QEMU can
 * honestly give.  This one counts microseconds against the block it has to fit
 * in - 256 frames at 22.05 kHz is 11.61 ms - because on a board the answer is
 * not how many instructions there are but how long they take, and those two
 * turned out to differ by a factor of two: the FPU's latency on a dependent
 * chain and a table lookup that leaves the cache are both invisible to a
 * counter of instructions.
 *
 * Each piece separately, so that "it does not fit" can be pointed at something.
 */
/*
 * A sine out of two parabolas, to within two per cent, because there is no
 * sine in the application ABI and this one is only here to move a curve.
 * Phase in turns; anything is folded back into one.
 */
static float bench_wave(float turns)
{
    float t = turns - (float)(int)turns;
    float v;
    if (t < 0.0f) {
        t += 1.0f;
    }
    if (t < 0.5f) {
        const float u = t * 4.0f - 1.0f; /* -1..1 over the first half */
        v = 1.0f - u * u;
    } else {
        const float u = (t - 0.5f) * 4.0f - 1.0f;
        v = u * u - 1.0f;
    }
    return v;
}

static void bench_audio(void)
{
    static const int oss[] = { 1, 2, 4 };
    const uint32_t   blocks = 40;
    const uint32_t   budget_us = (s_chunk * 1000000u) / RATE;
    uint32_t         i, b, k;
    ag_time_t        t0, t1;

    ag_printf("stomp: what a block costs, against %u us of audio\n",
              (unsigned)budget_us);

    for (k = 0; k < sizeof(oss) / sizeof(oss[0]); k++) {
        uint32_t us;
        s_cfg.os = oss[k];
        s_fit.os = s_cfg.os;
        if (ag_amp_set_voicing(s_amp, &s_cfg) != 0) {
            continue;
        }
        t0 = ag_micros();
        for (b = 0; b < blocks; b++) {
            for (i = 0; i < s_chunk; i++) {
                /* A ramp through the curve rather than a constant, which the
                 * compiler could hoist and the table would answer from one
                 * cache line. */
                const float x = (float)((int)(i * 37u % 2048u) - 1024) / 1024.0f;
                s_mono[i] = (int16_t)(ag_amp_tick(s_amp, x) * 8192.0f);
            }
        }
        t1 = ag_micros();
        us = (uint32_t)((t1 - t0) / blocks);
        ag_printf("  valves %dx%s %6u us  %3u%% of the block\n", oss[k],
                  s_cfg.adaa ? " + adaa" : "        ", (unsigned)us,
                  (unsigned)(us * 100u / budget_us));
    }

    /*
     * And the same chain at five levels, because what a guitarist reports is
     * that it costs more the harder he plays - jcm800 playable, bogner worse,
     * slo worse again, which is the order of their gain.  The probe is a
     * plucked note with harmonics that die faster the higher they are, so the
     * slew between samples is a guitar's and not a ramp's.
     */
    {
        static const uint8_t amp_pc[5] = {2u, 6u, 20u, 60u, 100u};
        uint32_t             k2;
        s_cfg.os = 2;
        s_fit.os = 2;
        (void)ag_amp_set_voicing(s_amp, &s_cfg);
        ag_printf("  the same chain at 2x, by how hard it is played:\n");
        for (k2 = 0; k2 < 5u; k2++) {
            const float amp = (float)amp_pc[k2] / 100.0f;
            uint32_t    us, j;
            float       peak = 0.0f;
            ag_amp_reset(s_amp);
            t0 = ag_micros();
            for (b = 0; b < blocks; b++) {
                for (i = 0; i < s_chunk; i++) {
                    /* Two decaying harmonics and a little of the third: a
                     * note, not a ramp, and the same shape at every level. */
                    const uint32_t t = b * s_chunk + i;
                    const float    ph = (float)(t % 269u) / 269.0f;
                    const float    e = 1.0f - (float)(t % 8192u) / 8192.0f;
                    const float    x =
                        amp * e *
                        (bench_wave(ph) + 0.5f * bench_wave(ph * 2.0f) +
                         0.25f * bench_wave(ph * 4.0f));
                    const float y = ag_amp_tick(s_amp, x);
                    if (y > peak) {
                        peak = y;
                    }
                    s_mono[i] = (int16_t)(y * 8192.0f);
                }
            }
            t1 = ag_micros();
            us = (uint32_t)((t1 - t0) / blocks);
            (void)j;
            ag_printf("    in %3u%%  %6u us  %3u%% of the block  peak %u%%\n",
                      (unsigned)amp_pc[k2], (unsigned)us,
                      (unsigned)(us * 100u / budget_us),
                      (unsigned)(peak * 100.0f));
        }
    }

    if (s_ir_ready) {
        uint32_t us;
        t0 = ag_micros();
        for (b = 0; b < blocks; b++) {
            ag_ir_process_block(&s_ir, s_mono, s_stereo);
        }
        t1 = ag_micros();
        us = (uint32_t)((t1 - t0) / blocks);
        ag_printf("  cabinet %4u ms, %2u parts %6u us  %3u%% of the block\n",
                  (unsigned)(s_ir.ir_frames * 1000u / RATE),
                  (unsigned)s_ir.parts, (unsigned)us,
                  (unsigned)(us * 100u / budget_us));
    }
    ag_printf("  the curves are in %s memory\n",
              s_tab_fast ? "internal" : "extended");
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
        present_now();
    }
    t1 = ag_micros();
    us_knob = t1 - t0;

    /* And what walking the selection costs: two cells, the one left and the
     * one arrived at. */
    t0 = ag_micros();
    for (i = 0; i < n; i++) {
        dirty_item(0);
        dirty_item(1);
        present_now();
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
    ag_printf("  microseconds of this machine; under QEMU with -icount one of\n"
              "  them is a thousand instructions and not a real one\n");
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
/* The chain, and the sound it makes                                      */
/* ---------------------------------------------------------------------- */

/*
 * A preset is the whole amplifier as bytes - component values, voicing, the
 * baked curves and the loudspeaker - and loading one needs no circuit solver
 * and no axis fitting.  That is the path a pedal is meant to take: the host
 * bakes once, the box reads bytes and plays.
 *
 * The built-in models are the other path and are honest about costing more.
 * `ag_amp_model` gives a configuration but not a curve, so playing one means
 * `ag_amp_build`, and that is 212 M instructions - 882 ms at 240 MHz, measured
 * by tubebench.  It happens with the sound stopped and the screen saying so,
 * because a bake under a ringing note is not a knob, it is a gap.
 */

/* One line about the sound, on the screen and in the log both: the screen is
 * where somebody playing looks, and the log is what a test can read. */
static void status(const char *s)
{
    (void)snprintf(s_status, sizeof(s_status), "%s", s);
    if (s[0] != 0) {
        ag_printf("stomp: %s\n", s);
    }
    dirty_all();
}

/* Big enough for `want` points per stage, kept across loads. */
/*
 * The curves, and where they live decides whether this plays at all.
 *
 * Every oversampled sample looks into these tables, four times over for two
 * stages, and a lookup that misses the cache and goes to PSRAM costs hundreds
 * of cycles - the same trap the flash-XIP measurement recorded: read-only
 * tables that the inner loop walks cannot leave internal SRAM.  Measured on the
 * S3-Zero with them in the process arena, which is PSRAM: 110% of the block
 * budget and a late block almost every time.  So ask for internal memory first
 * and only fall back to the arena, where it will run but not in time.
 */
/* Everything tab_reserve may have taken, whichever shape it took it in. */
static void tab_release(void)
{
    int i;
    for (i = 0; i < AG_AMP_STAGES; i++) {
        if (s_tabs[i] != NULL) {
            ag_free(s_tabs[i]);
            s_tabs[i] = NULL;
        }
    }
    if (s_tab != NULL) {
        ag_free(s_tab);
        s_tab = NULL;
    }
    s_tab_split = 0;
}

static int tab_reserve(int want, int stages)
{
    /*
     * Three arrays a stage, or two when three will not fit the fast memory.
     *
     * The third is the running sums, and they buy one thing: a span wider
     * than walk_max cells costs a subtraction rather than a walk.  Measured
     * on the board that is five per cent, and in practice nothing - no span a
     * guitar makes is that wide, and the counters put the shortcut's share at
     * nought at every level.  Internal memory is worth far more: the SLO's
     * four stages are 96 KB against about 80 free, so with the sums its
     * curves go to external RAM and every lookup pays the cache.  Without
     * them it is 64 KB, it fits, and the chain is half the price.
     *
     * Tried in that order, so a board with room keeps the shortcut and only
     * one that is short gives it up - and it says which in the line below.
     */
    const int    parts = s_tab_parts;
    const size_t floats = (size_t)stages * (size_t)parts * (size_t)want;
    const size_t bytes = floats * sizeof(float);
    const size_t per_stage = (size_t)parts * (size_t)want * sizeof(float);
    int          i;

    if (s_tab_n >= want && s_tab_stages >= stages &&
        (s_tab != NULL || s_tabs[0] != NULL)) {
        return 0;
    }
    tab_release();

    /*
     * ONE BLOCK PER STAGE FIRST, and the whole thing only if that fails.
     *
     * Measured on the Audio Kit: the table is 96 KB, internal memory has 133
     * KB free and its largest single block is 76, so asking for it whole sends
     * every curve lookup to external memory - and in the running loop those
     * lookups cost more than the bench ever showed, because the bench keeps
     * the table in cache and a real block does not.  A stage is 24 KB and four
     * of those fit where one 96 KB block does not.  ag_amp_build_split takes
     * them separately; the floats and their order are identical either way.
     */
    s_tab_inside = 0;
    for (i = 0; i < stages; i++) {
        s_tabs[i] = (float *)ag_malloc_caps(per_stage, AG_MEM_FAST);
        if (s_tabs[i] != NULL) {
            s_tab_inside++;
            continue;
        }
        /*
         * Not all of it fits, and that is no reason to put the rest outside.
         *
         * This used to be all-or-nothing: one stage that could not find its
         * 24 KB sent every stage to external memory.  With 117 KB free and a
         * largest block of 76, three of SLO's four fit - and three quarters
         * of the win was thrown away to keep the placement uniform, which
         * nothing needs it to be.  ag_amp_build_split takes a pointer per
         * stage and does not ask where it came from.
         */
        s_tabs[i] = (float *)ag_malloc(per_stage);
        if (s_tabs[i] == NULL) {
            break;
        }
    }
    s_tab_split = (i == stages);
    s_tab_fast = (s_tab_inside == stages);
    if (!s_tab_split) {
        while (i-- > 0) {
            ag_free(s_tabs[i]);
            s_tabs[i] = NULL;
        }
        s_tab_inside = 0;
        s_tab = (float *)ag_malloc_caps(bytes, AG_MEM_FAST);
        s_tab_fast = s_tab != NULL;
        if (s_tab == NULL) {
            s_tab = (float *)ag_malloc(bytes);
        }
    }
    const int have = s_tab_split || (s_tab != NULL);
    s_tab_n = have ? want : 0;
    s_tab_stages = have ? stages : 0;
    if (have) {
        ag_meminfo_t mem;
        size_t       probe = 0;
        ag_meminfo(&mem);
        if (!s_tab_fast) {
            /*
             * The free figure is a sum across regions and says nothing about
             * whether any one of them can hold this.  So ask: the largest block
             * internal memory will actually give, found by halving.  A refusal
             * over 151 KB free is a different problem from a refusal over 20,
             * and only one of them is worth working around.
             */
            size_t try_bytes = bytes;
            while (try_bytes >= 4096u) {
                void *p = ag_malloc_caps(try_bytes, AG_MEM_FAST);
                if (p != NULL) {
                    ag_free(p);
                    probe = try_bytes;
                    break;
                }
                try_bytes -= 4096u;
            }
        }
        ag_printf("stomp: %u KB of curves, %d of %d stages in internal "
                  "memory, %s%s (%u KB fast free, largest %u KB)\n",
                  (unsigned)(bytes / 1024u), s_tab_inside, stages,
                  s_tab_split ? "one block a stage" : "one block",
                  parts == 3 ? "" : ", no running sums",
                  (unsigned)(mem.fast_free / 1024u),
                  (unsigned)((s_tab_fast ? bytes : probe) / 1024u));
    }
    return have ? 0 : -1;
}

static int amp_reserve(void)
{
    if (s_amp == NULL) {
        s_amp = (ag_amp_t *)ag_malloc(sizeof(ag_amp_t));
        if (s_amp == NULL) {
            return -1;
        }
        memset(s_amp, 0, sizeof(*s_amp));
    }
    return 0;
}

/* Positions into the chain, then the chain into the filters, without clearing
 * them: a coefficient may change under a ringing note, a reset may not. */
static void apply_knobs(void)
{
    ag_amp_pot_apply(&s_cfg, &s_fit, s_model);
    if (s_amp != NULL) {
        (void)ag_amp_set_knobs(s_amp, &s_cfg);
    }
}

/* The whole file, since a preset is read once and is a hundred kilobytes. */
static void *read_all(const char *path, uint32_t *out_n)
{
    ag_stat_t   st;
    ag_handle_t f;
    void       *buf;
    int32_t     got;

    if (ag_stat(path, &st) != AG_OK || st.size == 0u || st.size > (4u << 20)) {
        return NULL;
    }
    buf = ag_malloc((size_t)st.size);
    if (buf == NULL) {
        return NULL;
    }
    f = ag_open(path, AG_O_RDONLY);
    if (f < 0) {
        ag_free(buf);
        return NULL;
    }
    got = ag_read(f, buf, (size_t)st.size);
    (void)ag_close(f);
    if (got != (int32_t)st.size) {
        ag_free(buf);
        return NULL;
    }
    *out_n = (uint32_t)st.size;
    return buf;
}

/* The cabinet: an impulse at whatever rate it was recorded at.  ag_ir resamples
 * it to ours, so a 48 kHz cabinet off somebody else's card works. */
static int cabinet_load(const int16_t *mono, uint32_t frames, uint32_t rate)
{
    if (!s_ir_ready) {
        if (ag_ir_init(&s_ir, RATE) != 0) {
            return -1;
        }
        s_ir_ready = 1;
    }
    /*
     * In the block the stream works in, whatever that turned out to be, and
     * after the init above because that one starts from zero.
     *
     * A cabinet still partitioned for 256 frames against a 64-frame stream
     * does not run at all: the overlap-add loop never reaches a whole block,
     * so it costs a microsecond and does nothing.  That is how it hid - not
     * as a fault but as a cabinet that had apparently become free.
     */
    (void)ag_ir_set_block(&s_ir, s_chunk);
    if (ag_ir_load(&s_ir, mono, frames, rate) != 0) {
        return -1;
    }
    ag_ir_set_bypass(&s_ir, !s_cab_on);
    return 0;
}

/*
 * Change the block while the amplifier is playing.
 *
 * This is a machine setting and not a knob: it is the latency a guitarist
 * feels in the hands, bought with arithmetic.  256 frames is 11.6 ms of
 * convolution and about 58 ms round trip once the converter's rings are
 * counted; 64 is 2.9 and about 14, and costs roughly half as much again per
 * second of audio because the same impulse becomes four times as many
 * partitions of a quarter-sized transform.  Nobody can choose that from a
 * datasheet - it is chosen by playing through it.
 *
 * Three things have to move together and in this order: the converter cuts
 * its ring to the new block, the stream takes whatever it was granted, and
 * the cabinet is partitioned again for it.  Doing any of them alone is one of
 * the faults this whole thread of work was about.
 *
 * Called from the key handler, which runs between blocks on the audio thread,
 * so nothing is in the chain while it happens.  The sound stops for as long
 * as the impulse takes to transform - about a fifth of a second - and that is
 * honest: it is a different machine afterwards.
 */
static int cabinet_from_wav(const char *path);

/*
 * Change the oversampling while the amplifier plays.
 *
 * It is the largest single lever on what this chain costs - the valves are
 * run that many times per sample - and therefore on whether it fits.  On this
 * board, with the take at drive 9 where they work hardest: 1x leaves the
 * pedal at 71% of its core and 2x at 95%.  At 95% there is nothing left for
 * the radio, and what a listener hears is the sound breaking up when he plays
 * hard.
 *
 * What it costs in sound is the other half, and that is a listening question,
 * which is why it is a knob and not a number chosen here.
 *
 * ag_amp_set_voicing rather than set_knobs: the oversampling is the one
 * setting whose filter state cannot survive the change, so the chain is
 * cleared.  That is a click, once, and it is honest.
 */
static void cab_thread(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ag_sem_take(s_cab_go, 1000u);
        if (s_cab_quit) {
            break;
        }
        if (s_ir_ready) {
            ag_ir_process_n(&s_ir, s_cab_in, s_cab_out, s_chunk);
        }
        ag_sem_give(s_cab_done);
    }
    ag_thread_exit();
}

/*
 * Started when there is a loudspeaker to run and a core to run it on.
 *
 * On a machine with one core this is a loss - two context switches a block
 * for nothing - so it is only taken when the system core is somewhere else,
 * which on this chip it always is.
 */
static void cab_start(void)
{
    if (s_cab_thread != NULL || !s_ir_ready || !s_cab_two_core) {
        return;
    }
    s_cab_quit = 0;
    s_cab_primed = 0;
    s_cab_go = ag_sem_create(1, 0);
    s_cab_done = ag_sem_create(1, 0);
    if (s_cab_go == NULL || s_cab_done == NULL) {
        goto fail;
    }
    s_cab_thread = ag_thread_create(cab_thread, NULL, "stomp.cab", 4096, 4,
                                    (uint32_t)AG_THREAD_SYS_CORE);
    if (s_cab_thread == NULL) {
        goto fail;
    }
    ag_printf("stomp: the loudspeaker runs on the system core\n");
    return;
fail:
    if (s_cab_go != NULL) {
        ag_sem_delete(s_cab_go);
        s_cab_go = NULL;
    }
    if (s_cab_done != NULL) {
        ag_sem_delete(s_cab_done);
        s_cab_done = NULL;
    }
    ag_printf("stomp: no thread for the loudspeaker; it runs in line\n");
}

static void cab_stop(void)
{
    if (s_cab_thread == NULL) {
        return;
    }
    /* Whatever it is holding is finished first: the convolution's state is
     * about to be rebuilt and a reader inside it would read a half-built
     * partition. */
    if (s_cab_primed) {
        (void)ag_sem_take(s_cab_done, 1000u);
        s_cab_primed = 0;
    }
    s_cab_quit = 1;
    ag_sem_give(s_cab_go);
    (void)ag_thread_join(s_cab_thread, 1000u);
    s_cab_thread = NULL;
    ag_sem_delete(s_cab_go);
    ag_sem_delete(s_cab_done);
    s_cab_go = NULL;
    s_cab_done = NULL;
}

static void os_apply(int os)
{
    if (s_amp == NULL || os == s_cfg.os || os < 1 || os > 8) {
        return;
    }
    {
        const int was = s_cfg.os;
        s_cfg.os = os;
        s_fit.os = os;
        if (ag_amp_set_voicing(s_amp, &s_cfg) != 0) {
            s_cfg.os = was;
            s_fit.os = was;
            ag_printf("stomp: %dx is not an oversampling this chain has\n", os);
            return;
        }
    }
    ag_amp_reset(s_amp);
    ag_printf("stomp: valves at %dx\n", os);
    dirty_all();
}

static void block_apply(uint32_t frames)
{
    char path[sizeof(s_cab)];

    if (!s_sound || frames == s_chunk || frames == 0u) {
        return;
    }
    status("changing the block");
    present_now();
    cab_stop(); /* the partitions are about to be rebuilt under it */

    ag_pcm_set_chunk(&s_out, frames);
    {
        /* The board has a ceiling on what it will spend on DMA, so take what
         * was granted rather than what was asked for. */
        const int32_t period = ag_pcm_period(&s_out);
        if (period > 0 && frames > (uint32_t)period) {
            frames = (uint32_t)period;
            ag_pcm_set_chunk(&s_out, frames);
        }
    }
    s_chunk = frames;

    (void)snprintf(path, sizeof(path), "%s", s_cab);
    if (path[0] != '\0') {
        if (cabinet_from_wav(path) == 0) {
            ag_ir_set_bypass(&s_ir, !s_cab_on);
        } else {
            s_cab_on = 0;
            ag_printf("stomp: the cabinet does not fit a %u-frame block\n",
                      (unsigned)s_chunk);
        }
    }
    cab_start();
    ag_printf("stomp: block %u frames, %u.%u ms\n", (unsigned)s_chunk,
              (unsigned)(s_chunk * 1000u / RATE),
              (unsigned)((s_chunk * 10000u / RATE) % 10u));
    status("");
    ag_pcm_pace_start(&s_out);
    dirty_all();
}

static int cabinet_from_wav(const char *path)
{
    ag_wav_pcm_t w;
    int          rc;
    uint32_t     frames;
    if (ag_wav_load(path, &w) != 0) {
        return -1;
    }
    frames = w.frames;
    /*
     * `cabms` applies here too, and it did not: the truncation lived only on
     * the preset path, so a cabinet named on the command line was loaded whole.
     * On the Audio Kit that was the difference between a chain at 65% of its
     * block and one at 120% - a 150 ms impulse against the 46 ms asked for,
     * three times the partitions, and nothing in the output saying so except
     * the length it printed.
     */
    if (s_cab_ms > 0u) {
        const uint32_t keep = s_cab_ms * w.rate / 1000u;
        if (keep > 0u && keep < frames) {
            frames = keep;
        }
    }
    rc = cabinet_load(w.data, frames, w.rate);
    ag_wav_free(&w);
    return rc;
}

/*
 * `pot treble=8` from the command line, so that a render can be made at a
 * stated setting and two of them compared.  A knob that cannot be set without a
 * finger cannot be tested, and "the knobs work" is a claim like any other.
 */
static int pot_set(const char *spec)
{
    const int n = ag_amp_pot_count(s_model);
    char      name[16];
    int       i, at = 0, v = 0, seen = 0;

    while (spec[at] != '\0' && spec[at] != '=' && at < (int)sizeof(name) - 1) {
        char c = spec[at];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }
        name[at] = c;
        at++;
    }
    name[at] = '\0';
    if (spec[at] != '=') {
        return -1;
    }
    for (at++; spec[at] >= '0' && spec[at] <= '9'; at++) {
        v = v * 10 + (spec[at] - '0');
        seen = 1;
    }
    if (!seen || v > 10) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (strcmp(ag_amp_pot_name(s_model, i), name) == 0) {
            s_cfg.pot[ag_amp_pot_id(s_model, i)] = (float)v / 10.0f;
            apply_knobs();
            ag_printf("stomp: %s = %d\n", name, v);
            return 0;
        }
    }
    ag_printf("stomp: %s has no %s\n", ag_amp_model_name(s_model), name);
    return -1;
}

/* Take the model, the knob set and the pot positions from a chain that is now
 * loaded, so the screen and the sound agree about what is playing. */
static void adopt_amp(void)
{
    int i;
    s_model = s_amp->cfg.model;
    s_cfg = s_amp->cfg;
    s_fit = s_amp->cfg; /* before a knob has touched it */
    for (i = 0; i < AG_AMP_POT_N; i++) {
        s_cfg.pot[i] = 0.5f; /* noon, which is where the fit left it */
    }
    if (s_sel >= item_count()) {
        s_sel = item_count() - 1;
    }
    apply_knobs();
    dirty_all();
}

static int amp_from_preset(const char *path)
{
    uint32_t n = 0;
    void    *buf = read_all(path, &n);
    int      tab_n = 0, stages = 0, rc;

    if (buf == NULL) {
        status("preset: cannot read it");
        return -1;
    }
    if (ag_amp_preset_shape(buf, n, &tab_n, &stages) != 0 || tab_n < 8 ||
        stages < 1 || amp_reserve() != 0 || tab_reserve(tab_n, stages) != 0) {
        ag_free(buf);
        status("preset: not one this build reads");
        return -1;
    }
    rc = s_tab_split
             ? ag_amp_preset_load_split(s_amp, buf, n, s_tabs, (float)RATE)
             : ag_amp_preset_load(s_amp, buf, n, s_tab, (float)RATE);
    if (rc != 0) {
        ag_free(buf);
        status("preset: refused");
        return -1;
    }
    {
        /* The loudspeaker travels inside the preset, so a chain cannot be
         * copied while its cabinet stays behind. */
        uint32_t       frames = 0, rate = 0;
        const int16_t *ir = ag_amp_preset_ir(buf, n, &frames, &rate);
        if (ir != NULL && frames > 0u) {
            /*
             * Shorter, when asked.  The convolution is partitioned in blocks of
             * AG_IR_BLOCK, so its cost is proportional to the impulse's length
             * and nothing else: 200 ms is eighteen partitions and 46 ms is
             * four.  A loudspeaker's impulse is mostly over in a few tens of
             * milliseconds and the rest is room; where the line should be drawn
             * is a listening question, so this is a number rather than a
             * decision taken here.
             */
            if (s_cab_ms > 0u) {
                const uint32_t keep = s_cab_ms * rate / 1000u;
                if (keep > 0u && keep < frames) {
                    frames = keep;
                }
            }
            (void)cabinet_load(ir, frames, rate);
            (void)snprintf(s_cab, sizeof(s_cab), "%s", "from the preset");
        }
    }
    ag_free(buf);
    ag_amp_reset(s_amp);
    adopt_amp();
    ag_printf("stomp: preset %s: %s, %d stages, %d points, cabinet %s\n",
              leaf(path), ag_amp_model_name(s_model), s_amp->n, s_amp->tab_n,
              s_ir_ready ? "yes" : "none");
    status("");
    return 0;
}

/*
 * A built-in model, which has no baked curve until one is solved for.  Costs a
 * preset load's worth of arithmetic and then some: measured at 882 ms.
 */
static int amp_from_model(int model)
{
    ag_ckt_t     *scratch;
    ag_amp_cfg_t  cfg;
    float        *probe;
    const int     probe_n = 4096;
    int           rc;

    /*
     * The model first, because it says how many stages it has - and that is
     * how much of the curve table is wanted.
     *
     * It used to reserve for AG_AMP_STAGES whatever the model was, on the
     * grounds that nothing had said how many there were yet.  Something had:
     * this call, one line further down.  Four stages is 96 KB and the Audio
     * Kit had 98 139 bytes of internal memory free, so the whole table missed
     * by a hundred and sixty-five bytes and went to external memory - where
     * every lookup, on every oversampled sample, goes through the cache.  The
     * JCM800 has two stages and wants 48 KB.
     */
    ag_amp_model(&cfg, model, (float)RATE);
    if (s_no_adaa) {
        cfg.adaa = 0;        /* measuring what it costs; see `noadaa` */
    }
    {
        /*
         * Three arrays a stage, and two if that is what it takes to keep the
         * curves in fast memory.
         *
         * The third is the running sums; see tab_reserve for what they buy
         * and what they cost.  The order matters: a board with room keeps
         * them, and only one that is short gives them up - the SLO's four
         * stages are 96 KB with and 64 without, against about 80 free.
         */
        const int stages = cfg.n_stages > 0 ? cfg.n_stages : AG_AMP_STAGES;
        if (amp_reserve() != 0) {
            status("no room for the chain");
            return -1;
        }
        s_tab_parts = 3;
        if (tab_reserve(AG_AMP_TAB_N, stages) != 0) {
            status("no room for the chain");
            return -1;
        }
        if (s_tab_inside < stages) {
            const int was_inside = s_tab_inside;
            s_tab_parts = 2;
            tab_release();
            if (tab_reserve(AG_AMP_TAB_N, stages) != 0 ||
                s_tab_inside <= was_inside) {
                /* No better without them: take the three back. */
                s_tab_parts = 3;
                tab_release();
                if (tab_reserve(AG_AMP_TAB_N, stages) != 0) {
                    status("no room for the chain");
                    return -1;
                }
            }
        }
        cfg.tab_hat = (uint8_t)(s_tab_parts == 3 ? 1 : 0);
    }
    scratch = (ag_ckt_t *)ag_malloc(sizeof(ag_ckt_t));
    probe = (float *)ag_malloc((size_t)probe_n * sizeof(float));
    if (scratch == NULL || probe == NULL) {
        ag_free(scratch);
        ag_free(probe);
        status("no room to bake it");
        return -1;
    }
    ag_amp_probe_pluck(probe, probe_n, (float)RATE);
    rc = s_tab_split ? ag_amp_build_split(s_amp, scratch, &cfg, s_tabs,
                                         AG_AMP_TAB_N, probe, probe_n)
                     : ag_amp_build(s_amp, scratch, &cfg, s_tab, AG_AMP_TAB_N,
                                    probe, probe_n);
    ag_free(scratch);
    ag_free(probe);
    if (rc != 0) {
        status("the bake failed");
        return -1;
    }
    ag_amp_reset(s_amp);
    adopt_amp();
    status("");
    return 0;
}

/* Digits, without pulling in a libc for one number. */
static int atoi_small(const char *s)
{
    int n = 0;
    if (s == NULL) {
        return 0;
    }
    while (*s >= (char)0x30 && *s <= (char)0x39) {
        n = n * 10 + (*s++ - 0x30);
    }
    return n;
}

/* One block: the take, through the valves, through the loudspeaker, out. */
static AG_HOT void audio_block(void)
{
    const ag_time_t t0 = ag_micros();
    uint32_t        i;
    float           peak = 0.0f;

    /*
     * A guitar if the board has one, the take otherwise.
     *
     * The live block is read before the valves run rather than a sample at a
     * time: the converter delivers in blocks and asking it for one frame is
     * asking it to wait for the other 255.  A short answer is padded with
     * silence rather than held for, because a pedal that stalls waiting for
     * input has stopped being a pedal.
     */
    if (s_live) {
        const int32_t got = ag_pcm_read(&s_out, s_in, (int32_t)s_chunk);
        uint32_t      have = (got > 0) ? (uint32_t)got : 0u;
        if (have < s_chunk) {
            memset(s_in + have * s_in_ch, 0,
                   (s_chunk - have) * s_in_ch * sizeof(int16_t));
            s_in_holes += s_chunk - have;
            if (got < 0) {
                s_in_starved++;
            }
        }
    }

    /*
     * The wait for the converter is not the valves' cost.
     *
     * ag_pcm_read blocks until a block has arrived, so with a guitar plugged
     * in that wait lands inside whatever timer is running - and it read as the
     * valves costing 8000 us against the 5450 the same chain takes on a file.
     * Timed apart it is a wait, which is what a pedal should be doing between
     * blocks rather than something to optimise away.
     */
    s_us_in_blk = (uint32_t)(ag_micros() - t0);
    s_us_in += s_us_in_blk;
    /* The longest the input ever made a block wait.  A live codec hands its
       frames over in whatever size its DMA was given, so a read of one block
       can be waiting for a whole buffer - which is a hole of that length in
       everything downstream, and nothing else in the status line shows it. */
    if (s_us_in_blk > s_worst_in) {
        s_worst_in = s_us_in_blk;
    }
    if (s_us_in_blk > s_win_in) {
        s_win_in = s_us_in_blk;
    }
    const ag_time_t t_valves = ag_micros();

    for (i = 0; i < s_chunk; i++) {
        float x = 0.0f, y;
        int   v;
        if (s_live) {
            /*
             * What the converter sent, before anything is done to it.
             *
             * "It gets worse the louder I play" has to be answered at the
             * input first: a guitar that drives the codec into its own rail
             * is already a square wave by the time the model sees it, and
             * every counter in this pedal watches the output.  Full scale is
             * counted, not approached: 32700 of 32768 is a sixth of a
             * decibel from the end.
             */
            const int16_t raw = s_in[i * s_in_ch];
            if (raw >= 32700 || raw <= -32700) {
                s_in_clip++;
                s_in_clip_win++;
            }
            /* Left channel: a guitar is one string of wire, and a codec that
             * was opened in stereo sends the same thing twice or sends the
             * other jack - neither is worth summing in. */
            if (s_in_sum && s_in_ch > 1) {
                /* Added, not averaged: the guitar is on one channel and the
                 * other carries only the inverted microphone, so the sum is
                 * the guitar at its own level rather than half of it. */
                x = ((float)s_in[i * s_in_ch] + (float)s_in[i * s_in_ch + 1]) *
                    (1.0f / 32768.0f);
            } else {
                x = (float)s_in[i * s_in_ch] * (1.0f / 32768.0f);
            }
        } else if (s_di.data != NULL && s_di.frames > 0u && !s_take_done) {
            x = (float)s_di.data[s_di_at] * (1.0f / 32768.0f);
            if (++s_di_at >= s_di.frames) {
                s_di_at = 0;
                if (!s_di_loop) {
                    /* Rendering to a file: the take is the length of the
                     * job, and looping would write until the card filled. */
                    s_take_done = 1;
                }
            }
        }
        y = (s_amp != NULL) ? ag_amp_tick(s_amp, x) : x;
        if (y > peak) {
            peak = y;
        } else if (-y > peak) {
            peak = -y;
        }
        v = (int)(y * (32767.0f / (float)AMP_HEAD) +
                  (y >= 0.0f ? 0.5f : -0.5f));
        if (v > 32767) {
            v = 32767;
            s_clipped++;
            s_clipped_win++;
        } else if (v < -32768) {
            v = -32768;
            s_clipped++;
            s_clipped_win++;
        }
        s_mono[i] = (int16_t)v;
    }
    s_amp_peak = peak;
    {
        /* Which stage the long block was in.  The averages cannot say: a
           block that took sixteen milliseconds instead of six is one block in
           a window of three hundred, and it disappears into the mean. */
        const uint32_t took = (uint32_t)(ag_micros() - t_valves);
        s_us_valves += took;
        if (took > s_win_v_max) {
            s_win_v_max = took;
        }
        if (took > s_run_v_max) {
            s_run_v_max = took;
        }
    }
    const ag_time_t t_ir = ag_micros();
    if (s_cab_thread != NULL) {
        /*
         * The loudspeaker is a block behind, on the other core.
         *
         * Take what it finished while the valves were working, then give it
         * this block and let go.  The wait is the only place the two threads
         * meet, and in a chain that fits it is over before it starts: the
         * other core had a whole block period for work that takes a fifth of
         * one.
         */
        uint32_t k;
        if (s_cab_primed) {
            (void)ag_sem_take(s_cab_done, 100u);
            for (k = 0; k < s_chunk * 2u; k++) {
                s_stereo[k] = s_cab_out[k];
            }
        } else {
            /* The first block has nothing behind it: silence rather than
             * whatever the buffer held. */
            for (k = 0; k < s_chunk * 2u; k++) {
                s_stereo[k] = 0;
            }
        }
        for (k = 0; k < s_chunk; k++) {
            s_cab_in[k] = s_mono[k];
        }
        s_cab_primed = 1;
        ag_sem_give(s_cab_go);
    } else if (s_ir_ready) {
        ag_ir_process_n(&s_ir, s_mono, s_stereo, s_chunk);
    } else {
        for (i = 0; i < s_chunk; i++) {
            s_stereo[2 * i] = s_mono[i];
            s_stereo[2 * i + 1] = s_mono[i];
        }
    }
    {
        const uint32_t took = (uint32_t)(ag_micros() - t_ir);
        s_us_cab += took;
        if (took > s_win_c_max) {
            s_win_c_max = took;
        }
        if (took > s_run_c_max) {
            s_run_c_max = took;
        }
    }

    /*
     * And whatever somebody else brought, in the pedal's effects loop: after
     * the cabinet, before the trim.  One call a block, in place, on the
     * stereo buffer this chain already carries.
     */
    if (s_fx_ops != NULL) {
        const ag_time_t t_fx = ag_micros();
        (void)s_fx_ops->process(NULL, s_stereo, s_chunk);
        s_us_fx += (uint32_t)(ag_micros() - t_fx);
    }
    /*
     * The output trim, and the peak measured where it matters.
     *
     * The master is the amplifier's own control and sits in front of the
     * loudspeaker; what leaves the converter is what comes out of the cabinet,
     * and a cabinet is not a unity-gain thing.  This DAC has no amplifier after
     * it either - line level is as loud as the board gets - so the last gain in
     * the chain belongs here, where it can be turned without moving the
     * amplifier's own setting off where it was measured.
     */
    {
        int      hi = 0;
        uint32_t n = s_chunk * 2u;
        /*
         * The trim and the headroom in one multiply: the room the valves were
         * given is taken back here, where going over the rail is the
         * converter's business, counted, and shown as CLIP - rather than in
         * the middle of the chain where nothing could see it.
         */
        const int   num = s_vol_num * AMP_HEAD;
        /*
         * The knee, in the units the samples are already in.  Worked out
         * once a block: nothing here depends on the sample.
         */
        const float kt = OUT_KNEE_T * 32767.0f;
        const float ks = 32767.0f - kt;          /* what is left above it   */
        const float kinv = 1.0f / (2.0f * ks);   /* the knee's own width    */
        for (i = 0; i < n; i++) {
            int v = s_stereo[i];
            {
                v = (v * num) >> 6;
                {
                    /*
                     * Soft above the threshold, and the rail only when the
                     * knee has nothing left - which is where the old hard
                     * clamp started.
                     */
                    const int   neg = v < 0;
                    float       a = (float)(neg ? -v : v);
                    if (a > kt) {
                        float u = (a - kt) * kinv;
                        if (u >= 1.0f) {
                            u = 1.0f;
                            s_out_clipped++;
                        }
                        a = kt + ks * (2.0f * u - u * u);
                        s_soft++;
                    }
                    v = (int)(neg ? -a : a);
                    if (v > 32767) {
                        v = 32767;
                    } else if (v < -32768) {
                        v = -32768;
                    }
                }
                s_stereo[i] = (int16_t)v;
            }
            if (v < 0) {
                v = -v;
            }
            if (v > hi) {
                hi = v;
            }
        }
        s_peak = (float)hi / 32768.0f;
    }
    /*
     * Three numbers instead of one.
     *
     * The single load figure covers the whole block and cannot say whether the
     * budget went on the valves, on the loudspeaker or on the plumbing around
     * them - and a bench that times the first two in isolation said 65% while
     * the running loop said 84-99%.  Rather than attribute the gap by
     * subtraction, which is how the cabinet was blamed for 40% it did not
     * cost, the loop times its own parts.
     */
    s_us_total += (uint32_t)(ag_micros() - t0);
    s_us_blocks++;
    /*
     * The load is the work, and waiting for the converter is not work.
     *
     * It reads as work because it is spent inside the block, and that one
     * confusion is what made this board look too slow for its own amplifier:
     * 7.4 ms of a block stalled in a read that could not be answered, counted
     * as processing, printed as 126% cpu over a chain using 64% of the time
     * it had.  The wait is the slack - the proof there is room - and it has
     * its own column in the breakdown.
     */
    {
        const uint32_t spent = (uint32_t)(ag_micros() - t0);
        ag_pcm_mark_render(&s_out,
                           (spent > s_us_in_blk) ? (spent - s_us_in_blk) : 0u);
    }
    {
        /*
         * The send is timed because this is where the clock ends up.
         *
         * Give the input ring enough depth and the read stops blocking - the
         * frames are already there - so the loop is held instead by the
         * output, which cannot take a block until it has played one.  The
         * wait does not disappear, it moves; and a breakdown that times only
         * the read goes quiet about four milliseconds of every block.
         */
        const ag_time_t t_out = ag_micros();
        const int32_t wrote =
            ag_pcm_write(&s_out, s_stereo, (int32_t)s_chunk);
        const uint32_t out_us = (uint32_t)(ag_micros() - t_out);
        s_us_out += out_us;
        {
            /* What this block cost with neither wait in it. */
            const uint32_t spent = (uint32_t)(ag_micros() - t0);
            const uint32_t waits = s_us_in_blk + out_us;
            const uint32_t busy = (spent > waits) ? (spent - waits) : 0u;
            if (busy > s_worst_busy) {
                s_worst_busy = busy;
            }
            if (busy > s_win_busy) {
                s_win_busy = busy;
            }
            if (busy > (s_chunk * 1000000u) / RATE) {
                s_over++;
            }
        }
        s_blocks++;
        if (s_blocks == 1u) {
            /* The first block is the one worth saying out loud: on a box with
             * no screen it is the only evidence that the chain reached the
             * converter at all, and a sink that refuses is silent otherwise. */
            ag_printf("stomp: first block: %d of %u frames written\n",
                      (int)wrote, (unsigned)s_chunk);
        }
        if (wrote < 0) {
            s_short++; /* the sink refused it; frames and bytes differ by sink */
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Doing something about it                                               */
/* ---------------------------------------------------------------------- */

/*
 * A built-in model.  With no sound open this only lays the screen out - the
 * knob set and the voicing - and costs nothing.  With sound open it has to be
 * baked, which is most of a second, so the block loop is told to stop first and
 * the screen says what is happening rather than appearing to hang.
 */
static void set_model(int m)
{
    dirty_all(); /* the knob count changes, so nothing is where it was */
    s_model = m;
    (void)snprintf(s_preset, sizeof(s_preset), "%s", ag_amp_model_name(m));
    if (s_sound) {
        status("baking the curves, about a second");
        present_now();
        (void)amp_from_model(m);
        return;
    }
    ag_amp_model(&s_cfg, s_model, (float)RATE);
    if (s_no_adaa) {
        s_cfg.adaa = 0;      /* measuring what it costs; see `noadaa` */
    }
    s_fit = s_cfg;
    ag_amp_pot_apply(&s_cfg, &s_fit, s_model);
    if (s_sel >= item_count()) {
        s_sel = item_count() - 1;
    }
}

/* Put the selected knob on a mark, if that is a move at all. */
static void slider_set(int v)
{
    const int id = ag_amp_pot_id(s_model, s_sel);

    if (s_sel >= knob_n() || marks(s_cfg.pot[id]) == v) {
        return;
    }
    s_cfg.pot[id] = (float)v / 10.0f;
    apply_knobs();
    dirty_item(s_sel);
    dirty_slider();
    /*
     * Which knob moved and where to, in the same words `pot drive=8` uses.
     *
     * One line per mark actually passed - the early return above means a
     * finger sliding within one mark says nothing - and it goes to the
     * console, which is not the screen the panel is painted on.  It is what
     * answers "the slider moved a different knob from the one I grabbed"
     * without anybody having to photograph a phone.
     */
    ag_printf("stomp: %s = %d\n", ag_amp_pot_name(s_model, s_sel), v);
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
    apply_knobs(); /* into the chain as well, under the note that is playing */
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
        if (s_ir_ready) {
            ag_ir_set_bypass(&s_ir, !s_cab_on);
        }
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
        if (s_ir_ready) {
            ag_ir_set_bypass(&s_ir, 1);
        }
        break;
    default:
        join(next, sizeof(next), s_dir, s_names[s_bsel]);
        if (s_browse == BROWSE_CAB) {
            s_cab_on = 1;
            if (cabinet_from_wav(next) != 0) {
                status("that wav is not a cabinet this reads");
            } else {
                (void)snprintf(s_cab, sizeof(s_cab), "%s", next);
            }
        } else {
            /* A preset carries its own loudspeaker, its own knob set and its
             * own curves: this is a different amplifier, not a setting. */
            if (amp_from_preset(next) == 0) {
                (void)snprintf(s_preset, sizeof(s_preset), "%s", next);
            }
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
    /*
     * The text panel turns a knob with left and right, without opening it
     * first.  The painted one has a slider to open because a knob there is a
     * picture and the slider is where the number is; a row of text already
     * shows the number, so the extra press would buy nothing.
     *
     * Digits set it outright, which is what a phone's keyboard is good at:
     * 0 is off, 9 is nine, and the tenth mark is `=`, because there is no key
     * for ten.
     */
    if (s_text && !s_editing) {
        int set = -1;
        switch (k) {
        case AG_KEY_LEFT:
            if (s_sel < knob_n()) {
                nudge(-1);
                return;
            }
            break;
        case AG_KEY_RIGHT:
            if (s_sel < knob_n()) {
                nudge(1);
                return;
            }
            break;
        case AG_KEY_MINUS:
            if (s_sel < knob_n()) {
                nudge(-1);
                return;
            }
            break;
        /* One column here, so up and down are one row, not the painted
         * panel's walk across three knobs to a row. */
        case AG_KEY_UP:
            if (s_sel > 0) {
                s_sel--;
                dirty_item(s_sel);
                dirty_item(s_sel + 1);
            }
            return;
        case AG_KEY_DOWN:
            if (s_sel + 1 < item_count()) {
                s_sel++;
                dirty_item(s_sel);
                dirty_item(s_sel - 1);
            }
            return;
        case AG_KEY_EQUAL:
            set = 10;
            break;
        case AG_KEY_0:
            set = 0;
            break;
        case AG_KEY_1:
        case AG_KEY_2:
        case AG_KEY_3:
        case AG_KEY_4:
        case AG_KEY_5:
        case AG_KEY_6:
        case AG_KEY_7:
        case AG_KEY_8:
        case AG_KEY_9:
            set = 1 + (int)(k - AG_KEY_1);
            break;
        default:
            break;
        }
        if (set >= 0 && s_sel < knob_n()) {
            s_cfg.pot[ag_amp_pot_id(s_model, s_sel)] = (float)set / 10.0f;
            apply_knobs();
            dirty_item(s_sel);
            return;
        }
    }

    switch (k) {
    case AG_KEY_ESC:
    case AG_KEY_Q:
        if (s_menu) {
            s_menu = 0;
            dirty_all();
        } else if (s_editing) {
            s_editing = 0;
        } else {
            *quit = 1;
        }
        break;
    case AG_KEY_ENTER:
        activate();
        break;
    case AG_KEY_B:
        /* Round the three ways, because this is meant to be A/B'd by ear and
         * a setting you can only walk one way is not an A/B. */
        block_apply((s_chunk > 128u) ? 128u : (s_chunk > 64u ? 64u : 256u));
        break;
    case AG_KEY_O:
        /* And O walks the oversampling, for whoever has a keyboard. */
        os_apply(s_cfg.os >= 4 ? 1 : (s_cfg.os * 2));
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

    /*
     * The open menu takes the tap before anything under it does - the same
     * lesson the slider taught: a panel painted over the knobs that lets the
     * touch through to them picks a different control than the one being
     * reached for.
     */
    if (s_menu) {
        int mx, my, mw, mh;
        menu_box(&mx, &my, &mw, &mh, sw, sh);
        if (px < mx || px >= mx + mw || py < my || py >= my + mh) {
            s_menu = 0; /* a tap outside is the way out */
            dirty_all();
            return;
        }
        for (i = 0; i < MENU_N; i++) {
            const int ry = my + menu_row_y(i);
            const int rh = (k_menu[i].kind == MROW_HEAD) ? MENU_HEAD
                                                         : MENU_ROW;
            if (k_menu[i].kind == MROW_HEAD || py < ry || py >= ry + rh) {
                continue;
            }
            s_msel = i;
            s_menu = 0;
            dirty_all();
            if (k_menu[i].kind == MROW_BLK) {
                block_apply(k_menu[i].val);
            } else {
                os_apply((int)k_menu[i].val);
            }
            break;
        }
        return;
    }

    {
        int gx, gy, gw, gh;
        gear_target(&gx, &gy, &gw, &gh, sw);
        if (px >= gx && px < gx + gw && py >= gy && py < gy + gh) {
            s_menu = 1;
            s_msel = -1;
            dirty_all();
            return;
        }
    }

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

    /*
     * The open slider takes the tap before anything under it does.
     *
     * It is painted over the knobs, and without this the touch went straight
     * through to whichever knob was behind it: reaching for the slider picked
     * a different control instead, which is the one thing a slider must never
     * do.  Inside, the tap sets the mark and the finger keeps it (see
     * AG_EV_POINTER_MOVE); outside, it closes - a tap on the background is how
     * a panel like this is dismissed, and it must not also count as a pick.
     */
    if (s_editing) {
        int sx, sy, sw2, sh2;
        slider_box(&sx, &sy, &sw2, &sh2, sw, sh);
        if (px >= sx && px < sx + sw2 && py >= sy && py < sy + sh2) {
            s_drag = 1;
            slider_set(slider_mark_at(px, sw));
            return;
        }
        s_editing = 0;
        s_drag = 0;
        dirty_all();
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

/*
 * Open the sink and load the take.  `sink` is what ag_pcm understands -
 * pcmvirt, pcmmix, pcmnull, a /dev node, or a path ending in .wav, which writes
 * a file that can be carried off the card and listened to.
 */
static int sound_start(const char *sink, const char *di_path)
{
    if (di_path != NULL && di_path[0] != '\0') {
        /*
         * As much of the take as the arena will hold in one block, and no more.
         * On the S3-Zero the largest free block is 192 KB against a seven-second
         * take of 308 - and the take is played in a loop, so four seconds of it
         * is a shorter loop rather than a failure.  A quarter of the block is
         * left alone for what the cabinet and the sink still have to allocate.
         */
        ag_meminfo_t mem;
        uint32_t     cap;
        ag_meminfo(&mem);
        cap = (uint32_t)((mem.arena_largest / 4u) * 3u / sizeof(int16_t));
        if (ag_wav_load_max(di_path, &s_di, cap) != 0) {
            /* Say which of the two it was.  "cannot read" over a file that is
             * plainly there, on a board where the answer is either the memory
             * or the filesystem, is a message that costs an hour. */
            ag_stat_t st;
            ag_meminfo(&mem);
            ag_printf("stomp: cannot read %s (%u bytes on disk, arena %u free, "
                      "largest %u)\n",
                      di_path,
                      ag_stat(di_path, &st) == AG_OK ? (unsigned)st.size : 0u,
                      (unsigned)mem.arena_free, (unsigned)mem.arena_largest);
            return -1;
        }
        ag_printf("stomp: take %s, %u frames, %u.%02u s of it\n",
                  leaf(di_path), (unsigned)s_di.frames,
                  (unsigned)(s_di.frames / RATE),
                  (unsigned)((s_di.frames % RATE) * 100u / RATE));
        if (s_di.rate != RATE) {
            /* Not resampled: the chain is fitted at one rate and a take at
             * another would play at the wrong pitch while looking right. */
            ag_printf("stomp: %s is %u Hz, and the chain runs at %u\n", di_path,
                      (unsigned)s_di.rate, (unsigned)RATE);
            ag_wav_free(&s_di);
            return -1;
        }
    }
    if (ag_pcm_open(&s_out, sink, RATE, 2) != 0) {
        ag_printf("stomp: no sink called %s\n", sink);
        ag_wav_free(&s_di);
        return -1;
    }
    /*
     * The converter decides the block, not the convolution.
     *
     * A ring of a few DMA buffers cannot answer a read for more frames than
     * half of it inside the block those frames belong to: the read blocks
     * past its own deadline, every block is late, and the wait is counted
     * somewhere that looks like work.  On the A1S that read as 126% cpu with
     * 48% of real work - a board apparently too slow for the amplifier it was
     * comfortably running.
     *
     * So ask, and come down if asked to.  Going up is not done: a block
     * larger than the chain was fitted for is the caller's decision.
     */
    /* Said first, because the converter cuts its ring to fit the block. */
    ag_pcm_set_chunk(&s_out, s_chunk);
    {
        /* And then ask what it granted: a board has a ceiling on what it will
         * spend, and working in a block it did not grant is the whole fault
         * this handshake exists to prevent. */
        const int32_t period = ag_pcm_period(&s_out);
        if (period > 0 && s_chunk > (uint32_t)period) {
            ag_printf("stomp: %s serves %d frames at a time; the block comes "
                      "down from %u\n",
                      s_out.path, (int)period, (unsigned)s_chunk);
            s_chunk = (uint32_t)period;
            ag_pcm_set_chunk(&s_out, s_chunk);
        }
    }
    /* A file sink is a render, not a performance: the take is played once and
     * then this stops, rather than filling the card with a loop. */
    s_di_loop = !ends_with(s_out.path, ".wav");
    s_sound = 1;
    return 0;
}

int ag_main(int argc, char **argv)
{
    int         quit = 0;
    int         i, want_bench = 0;
    const char *sink = NULL;
    const char *di_path = NULL;
    const char *preset = NULL;
    const char *cab_path = NULL;
    int         want_os = 0;
    int         want_live = 0;
    const char *set[AG_AMP_POT_N];
    int         n_set = 0;

    for (i = 1; i < argc; i++) {
        const int m = ag_amp_model_by_name(argv[i]);
        if (m >= 0) {
            s_model = m;
        } else if (strcmp(argv[i], "bench") == 0) {
            want_bench = 1;
        } else if (strcmp(argv[i], "play") == 0 && i + 1 < argc) {
            di_path = argv[++i];
        } else if (strcmp(argv[i], "out") == 0 && i + 1 < argc) {
            sink = argv[++i];
        } else if (strcmp(argv[i], "cab") == 0 && i + 1 < argc) {
            /*
             * The cabinet, by name, because a board with no screen cannot use
             * the chooser and the chooser was the only way in.  That is not a
             * convenience: the whole point of the box on the desk is to run
             * the chain a preset would, and a chain without its loudspeaker is
             * not that chain - it is a distortion being judged through
             * nothing.
             */
            cab_path = argv[++i];
        } else if (strcmp(argv[i], "noadaa") == 0) {
            /* For measuring only: what the antialiasing costs, on the board,
               in the chain rather than in a bench of its own.  It is what
               keeps the model from making noise that is not in the amplifier,
               so this is never a setting to play on. */
            s_no_adaa = 1;
        } else if (strcmp(argv[i], "stats") == 0) {
            /* The counters into the journal, for a measurement.  Not for
               playing: see the journal block in the loop. */
            s_journal = 1;
        } else if (strcmp(argv[i], "quiet") == 0) {
            /*
             * No periodic status while playing.
             *
             * The line is about sixty characters and the console is a UART at
             * 115200 or a WebSocket to a phone; either takes longer than a
             * block once the block is 64 frames, and the loop comes back to
             * find the clock has moved on.  Measured: one resync every two
             * seconds, which is exactly how often it was printed.
             *
             * Not silence, though: once every thirty seconds instead of every
             * two, so the counters can still be read and the cost is a
             * fifteenth.  A number nobody can see is how a board ends up with
             * a fault nobody can name.
             */
            s_quiet = 1;
        } else if (strcmp(argv[i], "fx") == 0) {
            /* A name, or nothing for the first effect there is. */
            if (i + 1 < argc && argv[i + 1][0] != '\0' &&
                strchr(argv[i + 1], '=') == NULL) {
                (void)snprintf(s_fx_name, sizeof(s_fx_name), "%s", argv[++i]);
            } else {
                s_fx_name[0] = '\0';
            }
            s_fx_amount = (s_fx_amount < 0) ? 100 : s_fx_amount;
        } else if (strcmp(argv[i], "fxamount") == 0 && i + 1 < argc) {
            s_fx_amount = atoi_small(argv[++i]);
        } else if (strcmp(argv[i], "cab2") == 0) {
            /* The loudspeaker on the system core, a block behind.  Off by
             * default: that core is for effects. */
            s_cab_two_core = 1;
        } else if (strcmp(argv[i], "notext") == 0) {
            /* No panel of any kind: play, and leave the console alone.  For a
             * run being measured, where the console is the instrument. */
            s_text = 0;
        } else if (strcmp(argv[i], "blk") == 0 && i + 1 < argc) {
            /*
             * Frames a block, and therefore latency.
             *
             * 256 is what the convolution wanted when it was a transform; 64
             * is 2.9 ms and is what a guitar wants.  The converter's own
             * buffer has to come down with it - `audio.dma_frames` in
             * BOARD.CFG, at least twice this - or the chain is punctual into a
             * ring that is still a tenth of a second deep.
             */
            const int v = atoi_small(argv[++i]);
            if (v >= 16 && v <= (int)CHUNK) {
                s_chunk = (uint32_t)v;
            }
        } else if (strcmp(argv[i], "cabms") == 0 && i + 1 < argc) {
            s_cab_ms = (uint32_t)atoi_small(argv[++i]);
        } else if (strcmp(argv[i], "vol") == 0 && i + 1 < argc) {
            /* 0 to 10 on the output trim, 5 being unity, in the same steps a
             * knob has: about 2.4 dB each, +12 at the top. */
            static const short k_vol[11] = { 8, 12, 18, 27, 42, 64,
                                             96, 144, 168, 208, 255 };
            int v = atoi_small(argv[++i]);
            s_vol_num = k_vol[v < 0 ? 0 : (v > 10 ? 10 : v)];
        } else if (strcmp(argv[i], "inmix") == 0) {
            /*
             * Live input, with the two channels summed instead of the left one
             * taken - which on the AI-Thinker Audio Kit is not a stereo
             * preference but a way round a fault in the board.
             *
             * Its line input is wired to LIN2/RIN2, the same pins as the second
             * microphone, so the microphone is always in the signal and cannot
             * be switched off from software.  It arrives inverted on the right
             * channel, though, and what is inverted on one channel and not the
             * other cancels when the two are added.  A guitar in a mono jack
             * grounds the ring, so the sum keeps the guitar and loses the room.
             *
             * The proper repair is two capacitors off the board (C19, C20);
             * this is what can be done without a soldering iron.
             */
            want_live = 1;
            s_in_sum = 1;
        } else if (strcmp(argv[i], "in") == 0) {
            /* Take the guitar rather than a file.  It is the same device the
             * sound goes out of, so there is nothing to name. */
            want_live = 1;
        } else if (strcmp(argv[i], "os") == 0 && i + 1 < argc) {
            /* The oversampling the chain runs at, over whatever the preset was
             * baked with.  It is the largest single lever on what this costs -
             * two valves at 4x are twice two valves at 2x - and on a board that
             * cannot make its deadline it is the first thing to try. */
            want_os = atoi_small(argv[++i]);
        } else if (strcmp(argv[i], "pot") == 0 && i + 1 < argc) {
            /* Kept and applied after the chain is loaded, because the preset
             * brings its own positions with it and would overwrite these. */
            if (n_set < (int)(sizeof(set) / sizeof(set[0]))) {
                set[n_set++] = argv[++i];
            } else {
                i++;
            }
        } else if (ends_with(argv[i], ".preset")) {
            preset = argv[i];
        }
    }
    set_model(s_model);

    /* Acquired once and kept: releasing between frames hands the panel back to
     * the console, which then writes its own lines over the amplifier. */
    if (ag_gfx_acquire(&s_info) != AG_OK) {
        /*
         * No panel, or somebody else has it.  Either way there is still an
         * amplifier to run: the box on the desk has no screen at all.
         */
        s_headless = 1;
        s_info.width = 0;
        s_info.height = 0;
        ag_printf("stomp: no display - playing without one\n");
    }

    /*
     * With no panel the console is the panel.  That is not a consolation
     * prize on this board: PHONE.SYS serves the console over HTTP with a
     * keyboard under it, so a pedal with no screen is still a pedal whose
     * knobs can be turned from a phone.
     *
     * `notext` puts it back to the silent headless behaviour, for a run being
     * measured that wants the console to itself.
     */
    if (s_text && !s_headless) {
        s_text = 0; /* the painted panel is the better one when there is one */
    }
    if (s_text) {
        ag_coninfo_t ci;
        ag_coninfo(&ci);
        if (ci.cols >= 20u && ci.rows >= 12u) {
            s_text_w = ci.cols;
            s_text_h = ci.rows;
        }
        /*
         * Quiet by force.  The status lines scroll, and a panel drawn at
         * fixed rows plus something that scrolls is a panel that walks up the
         * screen.  The same numbers go into two rows of the panel instead.
         */
        s_quiet = 1;
        ag_cls();
        ag_cursor(false);
        dirty_all();
    }
    if (want_bench) {
        /*
         * The screen if there is one, and the chain if a preset was named.  On
         * the board the second is the interesting half: the emulator can only
         * count instructions, and what a board answers is time.
         */
        /*
         * A named model is a chain too.  This used to measure only a preset,
         * so `stomp jcm800 bench` on a board with no screen did nothing at all
         * and said nothing about it - which is the one combination the board
         * on the desk actually is.  Baking the model costs the solver's 882 ms
         * once and then the numbers are the same numbers.
         */
        const int have_chain = (preset != NULL) ? (amp_from_preset(preset) == 0)
                                                : (amp_from_model(s_model) == 0);
        if (have_chain && cab_path != NULL) {
            /*
             * The cabinet counts in the measurement or the measurement is of
             * half the chain.  Subtracting a total from a total is how "the
             * cabinet costs 40% here against 12% on the S3" got written down
             * without anybody timing the convolution itself.
             */
            if (cabinet_from_wav(cab_path) == 0) {
                s_cab_on = 1;
                ag_ir_set_bypass(&s_ir, 0);
            }
        }
        if (have_chain) {
            bench_audio();
        } else {
            ag_printf("stomp: no chain to measure\n");
        }
        if (!s_headless) {
            bench(); /* releases the display itself, so the report is read */
        }
        return 0;
    }
    ag_printf("stomp: %s, %d knobs, %ux%u\n", ag_amp_model_name(s_model),
              ag_amp_pot_count(s_model), (unsigned)s_info.width,
              (unsigned)s_info.height);

    if (sink != NULL || di_path != NULL) {
        draw(); /* the screen first: loading a preset takes a moment */
        if (preset != NULL) {
            status("loading the preset");
            present_now();
            if (amp_from_preset(preset) == 0) {
                (void)snprintf(s_preset, sizeof(s_preset), "%s", preset);
            }
        } else {
            status("baking the curves, about a second");
            present_now();
            (void)amp_from_model(s_model);
        }
        if (want_os > 0 && s_amp != NULL) {
            /* ag_amp_set_voicing rather than set_knobs: the oversampling is the
             * one setting whose state cannot survive the change, and nothing is
             * playing yet, so clearing the filters costs nothing here. */
            s_cfg.os = want_os;
            s_fit.os = s_cfg.os;
            if (ag_amp_set_voicing(s_amp, &s_cfg) == 0) {
                ag_printf("stomp: oversampling %dx\n", want_os);
            } else {
                ag_printf("stomp: %dx is not an oversampling this chain has\n",
                          want_os);
            }
        }
        if (cab_path != NULL) {
            /*
             * After the chain, because a preset brings a cabinet of its own
             * and this is meant to replace it, and before the sound starts,
             * because loading an impulse allocates its partitions and that is
             * not something to do between two blocks of audio.
             */
            status("loading the cabinet");
            present_now();
            const int cab_ok = cabinet_from_wav(cab_path);
            /* And take it off again: every other status says what is
             * happening now, and this one stayed in the header for the rest
             * of the run - a panel that said "loading the cabinet" under an
             * amplifier that had been playing for an hour. */
            status("");
            if (cab_ok == 0) {
                s_cab_on = 1;
                ag_ir_set_bypass(&s_ir, 0);
                (void)snprintf(s_cab, sizeof(s_cab), "%s", cab_path);
                ag_printf("stomp: cabinet %s, %u ms of it\n", cab_path,
                          (unsigned)(s_ir.ir_frames * 1000u / RATE));
            } else {
                ag_printf("stomp: cannot load the cabinet %s\n", cab_path);
            }
        }
        for (i = 0; i < n_set; i++) {
            (void)pot_set(set[i]); /* after the chain, which brings its own */
        }
        const uint32_t blk_asked = s_chunk;
        if (sound_start(sink != NULL ? sink : "pcmnull", di_path) != 0) {
            if (!s_headless) {
                ag_gfx_release();
            }
            return 1;
        }
        ag_printf("stomp: sound = %s @ %u Hz, %u frames a block\n", s_out.path,
                  (unsigned)RATE, (unsigned)s_chunk);
        /*
         * The cabinet is partitioned for the block, and the converter may
         * have just shortened it - so it is built again rather than left
         * convolving against a block that no longer arrives.
         */
        if (s_chunk != blk_asked && s_cab_on && s_cab[0] != '\0') {
            char path[sizeof(s_cab)];
            (void)snprintf(path, sizeof(path), "%s", s_cab);
            if (cabinet_from_wav(path) == 0) {
                ag_ir_set_bypass(&s_ir, 0);
            } else {
                s_cab_on = 0;
                ag_printf("stomp: the cabinet does not fit the shorter block; "
                          "running without it\n");
            }
        }
        /* The rate and the block are settled, which is what an effect has
         * to be told before it can take whatever memory it needs. */
        fx_start(RATE, s_chunk, 2u);
        cab_start();
        if (want_live) {
            /* Ask for one block before promising anything.  A device with no
             * line in answers -AG_ENOSYS, and finding that out now is better
             * than a silence nobody can explain later. */
            const int32_t probe =
                ag_pcm_read(&s_out, s_in, (int32_t)s_chunk);
            if (probe < 0) {
                ag_printf("stomp: %s has no input (%d) - playing the take\n",
                          s_out.path, (int)probe);
            } else {
                s_live = 1;
                s_in_ch = (s_out.channels > 0) ? s_out.channels : 2;
                ag_printf("stomp: in from %s, %d channels\n", s_out.path,
                          s_in_ch);
            }
        }
    } else if (preset != NULL) {
        (void)amp_from_preset(preset);
        (void)snprintf(s_preset, sizeof(s_preset), "%s", preset);
    }
    draw();
    painter_start();

    if (s_sound) {
        ag_pcm_pace_start(&s_out);
    }
    while (!quit) {
        ag_event_t ev;

        /*
         * With sound running the block is the clock and events are drained
         * between blocks; without it there is nothing to wait for but an event.
         *
         * Bounded, and that is not tidiness.  An unbounded drain hands the loop
         * to whatever is posting: one device that reports continuously - a pad
         * layer with nothing plugged into it will do - and the audio block below
         * is never reached at all, which is a silence with no error in it.
         */
        int drained = 0;
        while (drained++ < 32 && ag_poll_event(&ev, s_sound ? 0 : 100)) {
            switch (ev.type) {
            case AG_EV_QUIT:
                /* Said out loud while it is being found out who asks a pedal
                 * to stop, and why it happens when a slot is merely left. */
                ag_printf("stomp: QUIT received, leaving\n");
                quit = 1;
                break;
            case AG_EV_FOCUS_LOST:
                /* The slot was left.  Stop painting into a screen that is
                 * about to drop every frame - in the journal that was 13
                 * flushes dropped every two seconds and a seventh of the
                 * sound gone with them. */
                s_on_screen = 0;
                break;
            case AG_EV_FOCUS_GAINED:
                s_on_screen = 1;
                /*
                 * The kernel took the screen away when the slot was left
                 * (ag_session_focus force-releases it); a draw without
                 * taking it back is dropped, flush by flush, and the slot
                 * comes back to whatever the console left there.
                 */
                if (!s_headless) {
                    const ag_err_t got = ag_gfx_acquire(&s_info);
                    ag_printf("stomp: focus back, screen %d (%ux%u)\n",
                              (int)got, (unsigned)s_info.width,
                              (unsigned)s_info.height);
                }
                draw(); /* somebody else had the panel: none of it is ours */
                break;
            case AG_EV_KEY_DOWN:
                s_touch_ms = ag_millis();
                on_key(ev.key.keycode, &quit);
                break;
            case AG_EV_POINTER_DOWN:
                s_touch_ms = ag_millis();
                on_tap(ev.ptr.x, ev.ptr.y);
                break;
            /*
             * The finger that is already on the slider keeps it.  Without
             * this it could only be tapped, one mark at a time, which is not
             * what a slider is for.
             */
            case AG_EV_POINTER_MOVE:
                if (s_drag && s_editing) {
                    s_touch_ms = ag_millis();
                    slider_set(slider_mark_at(ev.ptr.x, (int)s_info.width));
                }
                break;
            case AG_EV_POINTER_UP:
                s_drag = 0;
                break;
            default:
                break;
            }
            if (quit) {
                break;
            }
        }
        if (ag_interrupted()) {
            quit = 1;
        }
        if (quit) {
            break;
        }
        if (s_sound) {
            audio_block();
            if (s_take_done) {
                quit = 1;
            }
            {
                /* The meter and the load are only worth four times a
                 * second, and marking the strip is what makes them move. */
                const uint32_t ms = ag_millis();
                /*
                 * And it puts itself away.  A slider left open sits over the
                 * knobs, and the hand that opened it has usually gone back to
                 * the guitar: a tap on the background closes it (see on_tap)
                 * and so does five seconds of nothing, so neither leaves the
                 * panel covered.  Not while a finger is still on it.
                 */
                if (s_editing && !s_drag && s_touch_ms != 0u &&
                    ms - s_touch_ms >= SLIDER_IDLE_MS) {
                    s_editing = 0;
                    dirty_all();
                }
                /*
                 * The strip, only when it says something different.
                 *
                 * It used to be marked four times a second whatever it read,
                 * and on a remote screen that is 320x26 pixels over the air
                 * sixteen times a second for a bar that had not moved: with a
                 * phone attached the amplifier went from 13 resyncs in thirty
                 * seconds to 608.  A picture is sent because it changed, not
                 * because a timer went off.
                 */
                if (ms - s_hdr_ms >= 250u) {
                    char now_head[96];
                    s_hdr_ms = ms;
                    head_text(now_head, sizeof(now_head));
                    if (strcmp(now_head, s_hdr_shown) != 0) {
                        (void)snprintf(s_hdr_shown, sizeof(s_hdr_shown), "%s",
                                       now_head);
                        dirty(0, 0, (int)s_info.width, TOP_H);
                    }
                }
                /*
                 * And the same numbers to the console, because on this board
                 * the screen is a buffer nobody can see.  "It plays" and "it
                 * plays without dropping blocks" are different claims, and only
                 * the counters can tell them apart.
                 */
                if (ms - s_say_ms >= (s_text ? 1000u
                                             : (s_quiet ? 30000u : 2000u))) {
                    s_say_ms = ms;
                    ag_pcm_poll_stats(&s_out);
                    /*
                     * The panel's own two rows rather than a line that
                     * scrolls.  Once a second, and two rows of forty is under
                     * a millisecond of the serial line - a third of one audio
                     * block, against ten for the printed pair.
                     */
                    if (s_text) {
                        const uint32_t n = s_us_blocks;
                        const uint32_t budget = (s_chunk * 1000000u) / RATE;
                        text_status(budget, (n > 0u) ? s_us_total / n : 0u,
                                    s_out.load_pct);
                        if (n > 0u) {
                            s_us_in = 0;
                            s_us_valves = 0;
                            s_us_cab = 0;
                            s_us_total = 0;
                            s_us_blocks = 0;
                        }
                        s_out_clipped = 0;
                    } else if (s_us_blocks > 0u) {
                        const uint32_t n = s_us_blocks;
                        const uint32_t budget = (s_chunk * 1000000u) / RATE;
                        /* Kept for the journal line, which is read from
                           another slot - standing in this one to read the
                           console turns the painting back on. */
                        s_av_valves = s_us_valves / n;
                        s_av_cab = s_us_cab / n;
                        s_av_work = s_us_total / n;
                        s_av_budget = budget;
                        ag_printf("stomp: of %u us a block: waiting in "
                                  "%u, out %u, valves %u, cabinet %u, "
                                  "%s %u, the rest %u (%u work)\n",
                                  (unsigned)budget, (unsigned)(s_us_in / n),
                                  (unsigned)(s_us_out / n),
                                  (unsigned)(s_us_valves / n),
                                  (unsigned)(s_us_cab / n),
                                  (s_fx_ops != NULL) ? s_fx_name : "effect",
                                  (unsigned)(s_us_fx / n),
                                  (unsigned)((s_us_total - s_us_in -
                                              s_us_valves - s_us_cab -
                                              s_us_fx) / n),
                                  (unsigned)(s_us_total / n));
                        s_us_in = 0;
                        s_us_out = 0;
                        s_us_valves = 0;
                        s_us_fx = 0;
                        s_us_cab = 0;
                        s_us_total = 0;
                        s_us_blocks = 0;
                    }
                    /*
                     * `late` alone says almost nothing here: the sink's write
                     * blocks, so the loop reaches the pacing call after its
                     * due time on nearly every block and the counter climbs
                     * for ever while the sound is perfect.  `resync` is the
                     * one that means a whole block was lost from the clock,
                     * and `drop` that one was thrown away.
                     */
                    if (s_journal) {
                        /*
                         * The same numbers in the journal, every window, and
                         * ONLY when `stats` was asked for.
                         *
                         * Writing them unasked was a regression Maxim heard
                         * before any counter of mine showed it: five journal
                         * lines every two seconds, written from the audio
                         * thread itself, in a system where a journal line is
                         * already known to stall a real-time thread.  An
                         * instrument that changes what it measures is worse
                         * than no instrument, and this one was left switched
                         * on for everybody.
                         *
                         * The console they are printed on belongs to this
                         * slot, and reading it means standing in this slot,
                         * which turns the panel's painting back on: the very
                         * thing being measured, and worth nine per cent of
                         * the blocks.  The journal is readable from any slot
                         * and from the wire, which is the only honest way to
                         * watch a pedal played from somewhere else.  Every
                         * window rather than every fifth, because `log`
                         * shows a screenful and a line half a minute old has
                         * already scrolled away.
                         */
                        {
                            /* Short on purpose: the journal is read on a
                               forty-column screen and a line that wraps is a
                               number nobody can read. */
                            ag_log(AG_LOG_INFO, "stomp",
                                   "v%u c%u w%u/%u", (unsigned)s_av_valves,
                                   (unsigned)s_av_cab, (unsigned)s_av_work,
                                   (unsigned)s_av_budget);
                            /* Lost blocks first and few of them: the screen
                               is forty columns wide and a number cut in half
                               reads as a smaller number, which is worse than
                               no number at all. */
                            ag_log(AG_LOG_INFO, "stomp", "rsy%u n%u",
                                   (unsigned)s_out.resync, (unsigned)s_blocks);
                            ag_log(AG_LOG_INFO, "stomp", "late%u ovr%u wb%u",
                                   (unsigned)s_out.late, (unsigned)s_over,
                                   (unsigned)s_worst_busy);
                            ag_log(AG_LOG_INFO, "stomp", "in%u/%u bz%u",
                                   (unsigned)s_win_in, (unsigned)s_worst_in,
                                   (unsigned)s_win_busy);
                            ag_log(AG_LOG_INFO, "stomp", "pt%u %uus",
                                   (unsigned)s_paints,
                                   (unsigned)s_paint_us);
                            ag_log(AG_LOG_INFO, "stomp", "inclip%u/%u",
                                   (unsigned)s_in_clip_win,
                                   (unsigned)s_in_clip);
                            ag_log(AG_LOG_INFO, "stomp", "vmax%u cmax%u",
                                   (unsigned)s_win_v_max,
                                   (unsigned)s_win_c_max);
                            s_in_clip_win = 0;
                            s_win_v_max = 0;
                            s_win_c_max = 0;
                            s_win_in = 0;
                            s_win_busy = 0;
                            s_paints = 0;
                            s_paint_us = 0;
                        }
                    }
                    if (!s_text) {
                        /* The rail early in the line, because the console
                         * wraps at forty columns and a number that falls off
                         * the end is a number nobody reads. */
                        ag_printf("stomp: %u blocks, %u%% cpu, rail %u, "
                                  "soft %u, hole %u, %u late, %u drop, "
                                  "%u resync, over %u, worst busy %u us, "
                                  "amp %u%%, out %u%%%s\n",
                                  (unsigned)s_blocks,
                                  (unsigned)s_out.load_pct,
                                  (unsigned)s_clipped_win,
                                  (unsigned)s_soft,
                                  (unsigned)s_in_holes,
                                  (unsigned)s_out.late, (unsigned)s_out.drop,
                                  (unsigned)s_out.resync,
                                  (unsigned)s_over,
                                  (unsigned)s_worst_busy,
                                  (unsigned)(s_amp_peak * 100.0f),
                                  (unsigned)(s_peak * 100.0f),
                                  s_out_clipped > 0u ? " CLIP" : "");
                        /* Not reset: the worst hole of the whole session is
                         * what somebody who has been playing wants to know,
                         * and a window's worst is gone before he can ask. */
                        s_clipped_win = 0;
                        s_soft = 0;
                        s_in_holes = 0;
                    }
                }
            }
            /*
             * The screen only when the block loop is ahead of the sink.  A full
             * paint is 4.3 ms and a block is 11.6, so this is not a formality:
             * without it the first chooser opened would be a dropout.
             */
            /*
             * The text panel's marks count too.  Its rows are not rectangles,
             * so s_ndirty stays at zero while a turned knob is waiting to be
             * drawn - and without this the row never appeared until something
             * else asked for the whole screen.
             */
            /*
             * Six milliseconds of slack is the painted panel's price - 4.3 ms
             * for a full paint against an 11.6 ms block.  A text row is forty
             * characters handed to a console driver that queues them, so it
             * asks for a millisecond; at 64 frames a block there is never six
             * to be had, and with the painted panel's threshold a turned knob
             * was applied to the sound and never drawn.
             */
            /*
             * As many steps as the room before the next block allows, not one
             * a block.
             *
             * A whole repaint is eight bands and present() does one of them;
             * at one call a block that is eight blocks, and on an instrument a
             * block is 2.9 ms - so a panel took a quarter of a second to
             * appear and a turned knob crawled.  The slack is re-read between
             * steps, so this gives back the core the moment the next block is
             * close, and a band that had to wait for the wire ends the burst
             * by itself.
             */
            /*
             * Only when nobody else is painting.  With the painter thread
             * running this loop does nothing at all - which is the point of
             * it: the block loop stops lending the picture the microseconds
             * it does not have.
             */
            while (s_painter == NULL &&
                   (s_dirty_all || s_ndirty > 0 || s_text_all ||
                    s_text_rows != 0u) &&
                   ag_pcm_slack_us(&s_out) >= REDRAW_SLACK_US) {
                ag_pcm_poll_stats(&s_out);
                present();
            }
            ag_pcm_pace_wait(&s_out);
        } else {
            present_now();
        }
    }
    cab_stop();
    fx_stop();
    painter_stop();
    if (s_text) {
        /* Hand the console back the way it was found: a cursor, and a line
         * below the panel rather than on top of it. */
        ag_gotoxy(0, (uint16_t)(s_text_h - 1u));
        ag_cursor(true);
        (void)ag_print("\n");
    }
    if (s_sound) {
        ag_pcm_close(&s_out);
        /*
         * The worst single pass through each stage, said once, at the end.
         *
         * The averages fit and the sound still breaks, so the block that did
         * not fit is the whole question - and there is no way to watch for it
         * as it happens: printing a line costs more than a 64-frame block, so
         * an instrument that reports while it runs becomes the fault it is
         * looking for.  Counting is free, and saying it afterwards is free.
         */
        ag_printf("stomp: worst pass - valves %u us, cabinet %u us, of a "
                  "%u us block (worst busy %u)\n",
                  (unsigned)s_run_v_max, (unsigned)s_run_c_max,
                  (unsigned)((s_chunk * 1000000u) / RATE),
                  (unsigned)s_worst_busy);
        ag_printf("stomp: %u samples over the rail%s\n", (unsigned)s_clipped,
                  s_clipped > 0u ? " - turn the master down" : "");
    }
    ag_wav_free(&s_di);
    if (s_ir_ready) {
        ag_ir_free(&s_ir);
    }
    if (!s_headless) {
        ag_gfx_release();
    }
    return 0;
}
