/*
 * Shared helpers: resolve audio_out path and open a PCM /dev node.
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_AUDIO_OUT_H
#define AG_AUDIO_OUT_H

#include <argon/argon.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 if s equals lit ignoring ASCII case. */
static inline int ag_audio_out_ieq(const char *s, const char *lit)
{
    if (s == NULL || lit == NULL) {
        return 0;
    }
    for (; *lit && *s; lit++, s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (*lit != c) {
            return 0;
        }
    }
    return *lit == '\0' && *s == '\0';
}

static inline void ag_audio_out_copy(char *out, size_t outlen, const char *src)
{
    size_t n = 0;
    if (out == NULL || outlen == 0) {
        return;
    }
    if (src == NULL) {
        out[0] = '\0';
        return;
    }
    while (src[n] && n + 1u < outlen) {
        out[n] = src[n];
        n++;
    }
    out[n] = '\0';
}

/*
 * Normalize CLI/cfg value into out[].
 *   mock/nul/empty → /dev/pcmnull
 *   mix/pcmmix     → /dev/pcmmix (multi-app sum → pcmvirt|pcm0|pcmnull)
 *   net/tcp        → /dev/pcmvirt
 *   audio/i2s      → /dev/pcm0
 * Returns 1 if result is a PCM device path, 0 if caller should treat as WAV file.
 */
static inline int ag_audio_out_resolve(const char *in, char *out, size_t outlen)
{
    size_t n;
    if (out == NULL || outlen < 8u) {
        return 0;
    }
    out[0] = '\0';
    if (in == NULL || in[0] == '\0' || ag_audio_out_ieq(in, "mock") ||
        ag_audio_out_ieq(in, "nul") || ag_audio_out_ieq(in, "null") ||
        ag_audio_out_ieq(in, "pcmnull")) {
        ag_audio_out_copy(out, outlen, "/dev/pcmnull");
        return 1;
    }
    if (ag_audio_out_ieq(in, "mix") || ag_audio_out_ieq(in, "pcmmix")) {
        ag_audio_out_copy(out, outlen, "/dev/pcmmix");
        return 1;
    }
    if (ag_audio_out_ieq(in, "net") || ag_audio_out_ieq(in, "tcp") ||
        ag_audio_out_ieq(in, "pcmvirt")) {
        ag_audio_out_copy(out, outlen, "/dev/pcmvirt");
        return 1;
    }
    if (ag_audio_out_ieq(in, "audio") || ag_audio_out_ieq(in, "i2s") ||
        ag_audio_out_ieq(in, "pcm0")) {
        ag_audio_out_copy(out, outlen, "/dev/pcm0");
        return 1;
    }
    if ((in[0] == 'n' || in[0] == 'N') && (in[1] == 'e' || in[1] == 'E') &&
        (in[2] == 't' || in[2] == 'T') && in[3] == ':') {
        ag_audio_out_copy(out, outlen, "/dev/pcmvirt");
        return 1;
    }
    /* Device path or bare pcm* name. */
    if (in[0] == '/' ||
        ((in[0] == 'd' || in[0] == 'D') && in[1] == ':') ||
        ((in[0] == 'p' || in[0] == 'P') && (in[1] == 'c' || in[1] == 'C') &&
         (in[2] == 'm' || in[2] == 'M'))) {
        n = 0;
        while (in[n] && n + 1u < outlen) {
            char c = in[n];
            if (c == '\\') {
                c = '/';
            }
            out[n] = c;
            n++;
        }
        out[n] = '\0';
        if (out[0] != '/' && !(out[0] != '\0' && out[1] == ':')) {
            char tmp[AG_PATH_MAX];
            size_t i = 0;
            const char *prefix = "/dev/";
            while (prefix[i] && i + 1u < sizeof(tmp)) {
                tmp[i] = prefix[i];
                i++;
            }
            n = 0;
            while (out[n] && i + 1u < sizeof(tmp)) {
                tmp[i++] = out[n++];
            }
            tmp[i] = '\0';
            ag_audio_out_copy(out, outlen, tmp);
        }
        return 1;
    }
    ag_audio_out_copy(out, outlen, in);
    return 0;
}

static inline ag_handle_t ag_audio_out_open_dev(const char *dev_path,
                                                uint32_t rate, uint8_t channels)
{
    ag_handle_t    h;
    ag_audio_fmt_t fmt;
    ag_err_t       err;

    h = ag_dev_open(dev_path);
    if (h < 0) {
        return h;
    }
    fmt.rate = rate;
    fmt.channels = channels;
    fmt.bits = 16;
    err = ag_dev_ioctl(h, AG_IOC_AUDIO_SETFMT, &fmt, sizeof(fmt));
    if (err != AG_OK) {
        (void)ag_dev_close(h);
        return (ag_handle_t)err;
    }
    return h;
}

#ifdef __cplusplus
}
#endif

#endif /* AG_AUDIO_OUT_H */
