/*
 * ArgonOS firmware entry point.
 *
 * Deliberately empty: everything lives in the kernel component so that the
 * same kernel can be embedded into a different firmware image if a product
 * needs it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/kernel.h>

void app_main(void)
{
    ag_kernel_main();
}
