#ifndef SHARED_H_
#define SHARED_H_

#undef INLINE
#if __STDC_VERSION__ >= 199901L
#define INLINE static inline
#elif defined(__GNUC__)
#define INLINE static __inline__
#else
#define INLINE static
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

/* Xtensa / ArgonOS hosts are little-endian; Z80 PAIR and render tables need this. */
#ifndef LSB_FIRST
#define LSB_FIRST 1
#endif

#define NOZIP_SUPPORT 1
#define USE_Z80 1

#include "z80.h"
#include "sms.h"
#include "pio.h"
#include "memz80.h"
#include "vdp.h"
#include "render.h"
#include "tms.h"
#include "sn76489.h"
#include "ym2413.h"
#include "fmintf.h"
#include "sound.h"
#include "system.h"
#include "loadrom.h"
#include "config.h"
#include "z80_wrap.h"
#include "sound_output.h"
#include "smsplus.h"

#endif /* SHARED_H_ */
