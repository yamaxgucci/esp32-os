/*
 * Host-only stub so IR/FX can link without the Argon kernel.
 * Do not include this path when building .AXE images.
 */
#ifndef AG_HOST_ARGON_STUB_H
#define AG_HOST_ARGON_STUB_H

#include <stdlib.h>

#define ag_malloc malloc
#define ag_free   free

/*
 * The markers that decide where a function or a constant lives in a .AXE.
 * Nothing on the host: there is no code arena here and no flash to divide it
 * against, and the attribute would put the marked functions in a section this
 * build has no linker script for.  Defined rather than left out, because the
 * DSP sources are shared and the marks are in them.
 */
#define AG_HOT
#define AG_HOT_RODATA

#endif
