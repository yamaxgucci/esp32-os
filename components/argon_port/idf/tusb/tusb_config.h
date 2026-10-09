/*
 * ArgonOS port: ESP-IDF - TinyUSB device configuration.
 *
 * The tinyusb component ships no config of its own; the port owns this file the
 * way the TinyUSB model intends, and it is injected onto the tinyusb library's
 * include path by argon_port/CMakeLists.txt (the same mechanism esp_tinyusb
 * uses to inject its own).  It is trimmed to the one thing this board needs: a
 * device, not a host, one mass-storage LUN, full speed on the OTG peripheral.
 *
 * esp_tinyusb is deliberately NOT used - see main/idf_component.yml - so this
 * file is the only place the class is enabled, and usbd_hw.c owns the
 * tud_msc_* callbacks that a hand-written backend needs.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "tusb_option.h"
#include "esp_attr.h"

/* Device only, full speed, on the OTG peripheral (root hub port 0). */
#define CFG_TUSB_RHPORT0_MODE    (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

/* FreeRTOS: TinyUSB queues its events onto a task that calls tud_task(). */
#define CFG_TUSB_OS              OPT_OS_FREERTOS

/* Buffer DMA, the mode the ESP-IDF DCD is tested with.  On the S3 there is no
 * cache to maintain, so the DCD's buffers only need 4-byte alignment in DRAM. */
#define CFG_TUD_DWC2_DMA_ENABLE  1
#define CFG_TUSB_MEM_SECTION     TU_ATTR_ALIGNED(4) DRAM_ATTR

/* One mass-storage LUN.  The endpoint buffer is 512 bytes - the card's sector -
 * which is the conservative choice; 8192 would quadruple throughput at a cost
 * in the scarce internal RAM the port's task and FIFO already draw on. */
#define CFG_TUD_MSC              1
#define CFG_TUD_MSC_EP_BUFSIZE   512

/* Logging off; the port prints its own messages through the kernel. */
#define CFG_TUSB_DEBUG           0
