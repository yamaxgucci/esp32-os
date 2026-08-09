/*
 * ArgonOS Mega Drive — sound milestone helpers (WAV/mock/net, line mix).
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef MD_SOUND_H
#define MD_SOUND_H

#include <stdint.h>

#define MD_SOUND_RATE 22050u
#define MD_SOUND_NET_PORT_DEFAULT 5558u

void md_sound_set_path(const char *path); /* NULL/"mock" → discard; "net" → TCP; else WAV */
void md_sound_set_net_port(uint16_t port);
void md_sound_init(void);
void md_sound_close(void);
void md_sound_begin_frame(int lines_per_frame);
void md_sound_line(int line, int lines_per_frame);
void md_sound_end_frame(void);

/* Chip fronts call these when registers change; samples are produced in md_sound_line. */
void md_ym_render(int16_t *left, int16_t *right, int samples);
void md_psg_render(int16_t *left, int16_t *right, int samples);

#endif /* MD_SOUND_H */
