/*
 * ArgonOS port: ESP-IDF - pins and buses, the constants only.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_IO_H
#define ARGON_PORT_IMPL_IO_H

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#define AG_PORT_GPIO_PINS SOC_GPIO_PIN_COUNT

/* LEDC low-speed channels.  Four timers behind them: beyond four distinct
 * frequencies the channels share and the last one set wins. */
#define AG_PORT_PWM_CHANNELS 8

/*
 * The longest single SPI transfer, and it is the size of the bounce buffer.
 * The bounce buffer exists because an application's data lives in PSRAM while
 * the DMA engine wants internal memory, so every transfer is copied through one
 * - and this limit is what one costs.
 */
/*
 * The largest single transfer io->spi_xfer will take.
 *
 * Not the same thing as the bounce buffer any more (see AG_PORT_SPI_BOUNCE in
 * io_hw.c): a caller whose buffer is already fit for DMA is handed to the
 * peripheral directly and can be any size up to this, while one that needs
 * copying is copied a bounce-buffer at a time.  The number is large because a
 * transfer costs the same setup whatever it carries, and a panel being fed a
 * frame cares about nothing else.
 */
#define AG_PORT_SPI_MAX_XFER 8192

/*
 * Analogue input, off by default, and the reason is in the Kconfig help:
 * linking esp_adc runs a constructor before app_main that stops the boot under
 * QEMU - and QEMU is how this system is verified.  With the option off the ABI
 * entry is NULL rather than a stub that fails, so an application asks with
 * AG_HAS and adapts, which is what the whole feature-probing convention is for.
 */
#if CONFIG_ARGON_ENABLE_ADC
#define AG_PORT_HAS_ADC 1
#define AG_PORT_ADC_CHANNELS SOC_ADC_MAX_CHANNEL_NUM
#else
#define AG_PORT_HAS_ADC 0
#define AG_PORT_ADC_CHANNELS 0
#endif

/*
 * Which pin an ADC1 channel measures.
 *
 * A mapping rather than a base pin, because the two targets disagree about the
 * shape of the answer: on the S3 channel N is GPIO N+1, and on the original
 * ESP32 the eight channels are scattered over two banks in an order nobody
 * would guess.  The pin has to be known and not merely the channel, because
 * whoever measures claims the pin, so that two processes cannot fight over one
 * channel's attenuation.
 *
 * -1 means "this channel does not reach a pin here", and that is also the
 * answer for a target this port has not been told about: measuring the wrong
 * pin is worse than not measuring.
 */
#if CONFIG_IDF_TARGET_ESP32S3
#define AG_PORT_ADC_GPIO(ch) (((ch) >= 0 && (ch) < 10) ? ((ch) + 1) : -1)
#elif CONFIG_IDF_TARGET_ESP32
/* ADC2 is unusable while Wi-Fi runs, so only ADC1 is offered: channels 0..7
 * are GPIO 36, 37, 38, 39, 32, 33, 34, 35 - and 37 and 38 are not bonded out
 * on a WROOM module, which is the board's business rather than the port's. */
#define AG_PORT_ADC_GPIO(ch) \
    (((ch) >= 0 && (ch) < 4) ? (36 + (ch))    \
                             : (((ch) >= 4 && (ch) < 8) ? (28 + (ch)) : -1))
#else
#define AG_PORT_ADC_GPIO(ch) (-1)
#endif

#endif /* ARGON_PORT_IMPL_IO_H */
