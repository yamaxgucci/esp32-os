/*
 * ArgonOS firmware entry point.
 *
 * Deliberately empty: everything lives in the kernel component so that the
 * same kernel can be embedded into a different firmware image if a product
 * needs it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/kernel.h>

#include "sdkconfig.h"

#if defined(CONFIG_ARGON_BOARD_PROBE) && CONFIG_ARGON_BOARD_PROBE
void ag_board_probe(void);
#endif

void app_main(void)
{
#if defined(CONFIG_ARGON_BOARD_PROBE) && CONFIG_ARGON_BOARD_PROBE
    /* Bring-up build: the hardware, with nothing else running.  Never
     * returns; see main/probe.c for why this exists at all. */
    ag_board_probe();
#endif
    ag_kernel_main();
}
