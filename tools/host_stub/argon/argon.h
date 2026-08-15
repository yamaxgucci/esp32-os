/*
 * Host-only stub so IR/FX can link without the Argon kernel.
 * Do not include this path when building .AXE images.
 */
#ifndef AG_HOST_ARGON_STUB_H
#define AG_HOST_ARGON_STUB_H

#include <stdlib.h>

#define ag_malloc malloc
#define ag_free   free

#endif
