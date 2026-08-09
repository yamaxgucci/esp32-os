/*
 * ArgonOS Mega Drive — gwenesis SN76489 API over the SMS integer PSG.
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <string.h>

#include "gwenesis_sn76489.h"
#include "md_sound.h"
#include "sn76489.h"

/* sn76489.c references this for SetContext helpers. */
sn76489_t psg_sn;

int16 gwenesis_sn76489_buffer[1];
int   sn76489_index;
int   sn76489_clock;

static sn76489_t s_psg;
static int       s_clock;

void gwenesis_SN76489_Init(int PSGClockValue, int SamplingRate, int freq_divisor)
{
    (void)SamplingRate;
    (void)freq_divisor;
    sn76489_init(&s_psg, (uint32_t)PSGClockValue, MD_SOUND_RATE,
                 SN76489_NOISE_BITS_SMS, SN76489_NOISE_TAPPED_SMS);
    sn76489_set_output_channels(&s_psg, 0xffu);
    psg_sn = s_psg;
    sn76489_index = 0;
    sn76489_clock = 0;
    s_clock = 0;
}

void gwenesis_SN76489_Reset(void)
{
    sn76489_reset(&s_psg, 3579545u, MD_SOUND_RATE, SN76489_NOISE_BITS_SMS,
                  SN76489_NOISE_TAPPED_SMS);
    psg_sn = s_psg;
}

void gwenesis_SN76489_start(void)
{
    sn76489_index = 0;
    sn76489_clock = 0;
    s_clock = 0;
}

void gwenesis_SN76489_Write(int data, int target)
{
    (void)target;
    sn76489_write(&s_psg, (uint8_t)data);
}

void gwenesis_SN76489_run(int target)
{
    s_clock = target;
    sn76489_clock = target;
}

void md_psg_render(int16_t *left, int16_t *right, int samples)
{
    if (samples > 0) {
        sn76489_execute_samples(&s_psg, left, right, (uint32_t)samples);
    }
}

void gwenesis_SN76489_SetContext(uint8 *data)
{
    if (data) {
        memcpy(&s_psg, data, sizeof(s_psg));
        psg_sn = s_psg;
    }
}

void gwenesis_SN76489_GetContext(uint8 *data)
{
    if (data) {
        memcpy(data, &s_psg, sizeof(s_psg));
    }
}

uint8 *gwenesis_SN76489_GetContextPtr(void)
{
    return (uint8 *)&s_psg;
}

int gwenesis_SN76489_GetContextSize(void)
{
    return (int)sizeof(s_psg);
}

void gwenesis_sn76489_save_state(void) {}
void gwenesis_sn76489_load_state(void) {}
