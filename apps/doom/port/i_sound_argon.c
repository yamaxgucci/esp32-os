/*
 * ArgonOS doomgeneric sound: DMX lumps → 8-ch mix → /dev/pcm*.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

#include "audio_out.h"
#include "deh_str.h"
#include "doomgeneric_argon.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#define OUT_RATE 11025u
#define MIX_CH   8
#define MIX_MAX  512
#define DMX_HDR  8
#define DMX_PAD  16

typedef struct {
    const uint8_t *pcm;
    int            len;
    int            pos;
    int            vol;
    int            left;
    int            right;
} ch_t;

static char        s_want[32] = "pcmvirt";
static int         s_use_prefix = 1;
static int         s_open_logged;
static ag_handle_t s_fd = -1;
static ch_t        s_ch[MIX_CH];
static int16_t     s_out[MIX_MAX * 2];

static snddevice_t k_devs[] = {
    SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32,
};

static int snd_GetSfxLumpNum(sfxinfo_t *sfxinfo);

void doom_argon_set_sound_path(const char *path)
{
    size_t i = 0;
    if (path == NULL) {
        s_want[0] = '\0';
        return;
    }
    while (path[i] && i + 1u < sizeof(s_want)) {
        s_want[i] = path[i];
        i++;
    }
    s_want[i] = '\0';
}

static int clamp16(int v)
{
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return v;
}

static void set_pan(ch_t *c, int vol, int sep)
{
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 127) {
        vol = 127;
    }
    if (sep < 0) {
        sep = 0;
    }
    if (sep > 254) {
        sep = 254;
    }
    c->vol = vol;
    c->left = 254 - sep;
    c->right = sep;
}

static int load_dmx(sfxinfo_t *sfx, ch_t *c)
{
    int      lump;
    int      size;
    uint8_t *raw;
    uint16_t fmt;
    uint32_t count;
    int      nsamp;

    if (sfx == NULL) {
        return 0;
    }
    if (sfx->link != NULL) {
        sfx = sfx->link;
    }
    lump = sfx->lumpnum;
    if (lump < 0) {
        lump = snd_GetSfxLumpNum(sfx);
        sfx->lumpnum = lump;
    }
    if (lump < 0) {
        return 0;
    }
    size = W_LumpLength((unsigned)lump);
    if (size < DMX_HDR + DMX_PAD * 2 + 1) {
        return 0;
    }
    raw = (uint8_t *)W_CacheLumpNum(lump, PU_SOUND);
    if (raw == NULL) {
        return 0;
    }
    fmt = (uint16_t)(raw[0] | (raw[1] << 8));
    if ((fmt & 0xffu) != 3u) {
        return 0;
    }
    count = (uint32_t)raw[4] | ((uint32_t)raw[5] << 8) |
            ((uint32_t)raw[6] << 16) | ((uint32_t)raw[7] << 24);
    nsamp = (int)count - DMX_PAD * 2;
    if (nsamp < 1) {
        nsamp = size - DMX_HDR - DMX_PAD * 2;
    }
    if (nsamp < 1) {
        return 0;
    }
    if (DMX_HDR + DMX_PAD + nsamp > size) {
        nsamp = size - DMX_HDR - DMX_PAD;
    }
    if (nsamp < 1) {
        return 0;
    }
    c->pcm = raw + DMX_HDR + DMX_PAD;
    c->len = nsamp;
    c->pos = 0;
    return 1;
}

static boolean snd_Init(boolean use_sfx_prefix)
{
    char        resolved[AG_PATH_MAX];
    ag_handle_t h;

    s_use_prefix = use_sfx_prefix ? 1 : 0;
    if (ag_audio_out_ieq(s_want, "nosound")) {
        ag_printf("doom: sound off\n");
        return false;
    }

    (void)ag_audio_out_resolve(s_want[0] ? s_want : "pcmvirt", resolved,
                               sizeof(resolved));
    h = ag_audio_out_open_dev(resolved, OUT_RATE, 2);
    if (h < 0 && !ag_audio_out_ieq(resolved, "/dev/pcmnull")) {
        ag_printf("doom: %s failed (%d), trying /dev/pcmnull\n", resolved,
                  (int)h);
        h = ag_audio_out_open_dev("/dev/pcmnull", OUT_RATE, 2);
        if (h >= 0) {
            ag_audio_out_copy(resolved, sizeof(resolved), "/dev/pcmnull");
        }
    }
    if (h < 0) {
        ag_printf("doom: no pcm device (%d)\n", (int)h);
        return false;
    }
    s_fd = h;
    if (!s_open_logged) {
        s_open_logged = 1;
        ag_printf("doom: sfx %s @ %u Hz\n", resolved, (unsigned)OUT_RATE);
    }
    return true;
}

static void snd_Shutdown(void)
{
    int i;
    for (i = 0; i < MIX_CH; i++) {
        s_ch[i].pcm = NULL;
        s_ch[i].len = 0;
        s_ch[i].pos = 0;
    }
    if (s_fd >= 0) {
        (void)ag_dev_close(s_fd);
        s_fd = -1;
    }
}

static int snd_GetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    char namebuf[16];

    if (sfxinfo == NULL) {
        return -1;
    }
    if (sfxinfo->link != NULL) {
        sfxinfo = sfxinfo->link;
    }
    if (s_use_prefix) {
        M_snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfxinfo->name));
    } else {
        M_snprintf(namebuf, sizeof(namebuf), "%s", DEH_String(sfxinfo->name));
    }
    return W_CheckNumForName(namebuf);
}

static void snd_Update(void)
{
    int n;
    int i;
    int c;
    int sl, sr;

    if (s_fd < 0) {
        return;
    }
    n = (int)(OUT_RATE * (unsigned)snd_maxslicetime_ms / 1000u);
    if (n < 1) {
        n = 1;
    }
    if (n > MIX_MAX) {
        n = MIX_MAX;
    }

    for (i = 0; i < n; i++) {
        sl = 0;
        sr = 0;
        for (c = 0; c < MIX_CH; c++) {
            ch_t *ch = &s_ch[c];
            int   s;
            if (ch->pcm == NULL || ch->pos >= ch->len) {
                continue;
            }
            s = ((int)ch->pcm[ch->pos] - 128) * ch->vol;
            sl += s * ch->left;
            sr += s * ch->right;
            ch->pos++;
        }
        s_out[i * 2] = (int16_t)clamp16(sl / 254);
        s_out[i * 2 + 1] = (int16_t)clamp16(sr / 254);
    }
    (void)ag_dev_write(s_fd, s_out, (size_t)n * 4u);
}

static void snd_UpdateSoundParams(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= MIX_CH) {
        return;
    }
    set_pan(&s_ch[channel], vol, sep);
}

static int snd_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    ch_t *c;

    if (channel < 0 || channel >= MIX_CH) {
        return -1;
    }
    c = &s_ch[channel];
    c->pcm = NULL;
    c->len = 0;
    c->pos = 0;
    if (!load_dmx(sfxinfo, c)) {
        return -1;
    }
    set_pan(c, vol, sep);
    return channel;
}

static void snd_StopSound(int channel)
{
    if (channel < 0 || channel >= MIX_CH) {
        return;
    }
    s_ch[channel].pcm = NULL;
    s_ch[channel].len = 0;
    s_ch[channel].pos = 0;
}

static boolean snd_SoundIsPlaying(int channel)
{
    ch_t *c;
    if (channel < 0 || channel >= MIX_CH) {
        return false;
    }
    c = &s_ch[channel];
    return c->pcm != NULL && c->pos < c->len;
}

static void snd_CacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    (void)sounds;
    (void)num_sounds;
}

sound_module_t DG_sound_module = {
    k_devs,
    (int)(sizeof k_devs / sizeof k_devs[0]),
    snd_Init,
    snd_Shutdown,
    snd_GetSfxLumpNum,
    snd_Update,
    snd_UpdateSoundParams,
    snd_StartSound,
    snd_StopSound,
    snd_SoundIsPlaying,
    snd_CacheSounds,
};
