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
#define AG_PORT_SPI_MAX_XFER 1024

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

/* ADC1 channel N is GPIO N+1 on the S3; other targets differ and say so. */
#if CONFIG_IDF_TARGET_ESP32S3
#define AG_PORT_ADC_FIRST_GPIO 1
#else
#define AG_PORT_ADC_FIRST_GPIO (-1)
#endif

#endif /* ARGON_PORT_IMPL_IO_H */
