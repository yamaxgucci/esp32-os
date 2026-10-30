/*
 * ArgonOS Mega Drive — sound milestone helpers (WAV/mock/net, line mix).
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef MD_SOUND_H
#define MD_SOUND_H

#include <stdint.h>

#define MD_SOUND_RATE 22050u
#define MD_SOUND_NET_PORT_DEFAULT 5558u

void md_sound_set_path(const char *path); /* mock/net/pcmvirt/… or WAV path */
void md_sound_set_net_port(uint16_t port); /* obsolete; virt port is in PCMVIRT.SYS */

/*
 * The rate the YM2612 and the PSG are run at: MD_SOUND_RATE, or on the CYD
 * half of it, each sample then given twice (`hifi` asks for the whole rate).
 * Set before the machine is powered on; the chips take it at reset.
 */
uint32_t md_sound_synth_rate(void);
void     md_sound_set_hifi(int on);
void md_sound_init(void);
void md_sound_close(void);
void md_sound_begin_frame(int lines_per_frame);
void md_sound_line(int line, int lines_per_frame);

/*
 * The lines a frame's samples are mixed on: every eighth and the last.
 * The YM2612's update starts by refreshing all six channels, and run once a
 * line from flash that was most of what sound cost on the CYD - 6-8 ms a
 * frame for one or two samples a line.  A write to the chip still catches it
 * up to its own time first, so batching changes nothing that is heard.
 */
#define MD_SOUND_LINE_DUE(line, lines) \
    ((((line) & 7) == 7) || ((line) == (lines) - 1))

void md_sound_end_frame(void);

/* Per-frame YM clock/buffer reset (native YM); no-op-ish for ag_fm. */
void md_ym_frame_begin(int lines_per_frame);

/* Chip fronts call these when registers change; samples are produced in md_sound_line. */
void md_ym_render(int16_t *left, int16_t *right, int samples);
void md_psg_render(int16_t *left, int16_t *right, int samples);

#endif /* MD_SOUND_H */
