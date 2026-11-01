/*
Gwenesis : Genesis & megadrive Emulator.

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.
This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with
this program. If not, see <http://www.gnu.org/licenses/>.

__author__ = "bzhxx"
__contact__ = "https://github.com/bzhxx"
__license__ = "GPLv3"

*/
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "m68k.h"
#include "gwenesis_vdp.h"
#include "gwenesis_io.h"
#include "gwenesis_bus.h"
#include "gwenesis_savestate.h"

//#include <assert.h>

#if GNW_TARGET_MARIO !=0 || GNW_TARGET_ZELDA!=0 || ARGON_TARGET !=0
  #pragma GCC optimize("Ofast")
#endif

#if GNW_TARGET_MARIO != 0 | GNW_TARGET_ZELDA != 0

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
#include "stm32h7b0xx.h"
extern unsigned char* VRAM;

#else

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
#ifdef ARGON_MD_SMALL
extern unsigned char *VRAM;
#else
extern unsigned char VRAM[];
#endif

#endif

extern unsigned short CRAM[];            // CRAM - Palettes
#ifndef ARGON_MD_SMALL
extern unsigned char SAT_CACHE[]__attribute__((aligned(4)));        // Sprite cache
#endif
extern unsigned char gwenesis_vdp_regs[]; // Registers

extern unsigned short CRAM565[];    // CRAM - Palettes

extern unsigned short VSRAM[];        // VSRAM - Scrolling

#ifdef ARGON_MD_SMALL
/*
 * The CYD: the registers, the vertical scroll and the palette read through
 * pointers - the live VDP, or the copy a pass of a frame with raster effects
 * draws from (md_raster.c).
 */
extern unsigned char  *md_vr_regs;
extern unsigned short *md_vr_vsram;
extern unsigned short *md_vr_cram565;
#define gwenesis_vdp_regs md_vr_regs
#define VSRAM md_vr_vsram
#define CRAM565 md_vr_cram565
#endif

// Define screen buffers: original and scaled for host RGB
unsigned char *screen, *scaled_screen;

// Define screen buffers for embedded 565 format
#ifdef ARGON_MD_SMALL
/*
 * The CYD: both cores draw lines - the system core its passes, the
 * application core what its frames leave time for (md_main.c, md_help_draw) -
 * so what a line is drawn in is each core's own.  PRID's bit 13 is the core,
 * as ESP-IDF reads it; not volatile, so a function reads it once.
 */
static inline __attribute__((always_inline)) int md_vdp_core(void)
{
#ifdef __XTENSA__
  unsigned int id;
  __asm__("rsr %0, prid" : "=a"(id));
  return (int)((id >> 13) & 1u);
#else
  return 0; /* the host harness: one core */
#endif
}
static unsigned short *screen_buffer_line_c[2];
static unsigned short *screen_buffer_c[2];
#define screen_buffer_line (screen_buffer_line_c[md_vdp_core()])
#define screen_buffer (screen_buffer_c[md_vdp_core()])
#else
static unsigned short *screen_buffer_line=0;
static unsigned short *screen_buffer=0;
#endif

/*
 * ArgonOS: pixels between the starts of two consecutive lines.  Upstream steps
 * by SCREEN_WIDTH, which forces the caller's buffer to be exactly 320 wide;
 * making it a variable lets the emulator render straight into a window of a
 * wider framebuffer and skip a full-frame copy per frame.
 */
static int screen_buffer_stride = SCREEN_WIDTH;

    // Overflow is the maximum size we can draw outside to avoid
    // wasting time and code in clipping. The maximum object is a 4x4 sprite,
    // so 32 pixels (on both side) is enough.

enum { PIX_OVERFLOW = 32 };

#ifdef ARGON_MD_SMALL
static uint8_t render_buffer_c[2][SCREEN_WIDTH + PIX_OVERFLOW*2] __attribute__((aligned(4)));
/* one: only shadow/highlight draws into it, and then the application core
 * does not help (md_help_draw) - 384 bytes the CYD did not have */
static uint8_t sprite_buffer[SCREEN_WIDTH + PIX_OVERFLOW*2];
#define render_buffer (render_buffer_c[md_vdp_core()])
#else
static uint8_t render_buffer[SCREEN_WIDTH + PIX_OVERFLOW*2] __attribute__((aligned(4)));
static uint8_t sprite_buffer[SCREEN_WIDTH + PIX_OVERFLOW*2];
#endif

// Define VIDEO MODE
static int mode_h40;
int mode_pal;

// Define screen W/H
int screen_width;
int screen_height;

int gwenesis_H32upscaler;

int sprite_overflow;
bool sprite_collision;

// Window Plane and A plane spearation
static int base_w;
static int PlanA_firstcol;
static int PlanA_lastcol;

static int Window_firstcol;
static int Window_lastcol;

// 16 bits access to VRAM
#define FETCH16VRAM(A)  ( VRAM_RD8((A)+1) | (VRAM_RD8(A) << 8) )

#define VDP_GFX_DISABLE_LOGGING 1

#if !VDP_GFX_DISABLE_LOGGING
#include <stdarg.h>
void vdpg_log(const char *subs, const char *fmt, ...) {
  extern int frame_counter;
  extern int scan_line;

  va_list va;

  printf("%06d:%03d :[%s] vc:%03x hc:%03x", frame_counter, scan_line, subs,gwenesis_vdp_vcounter(),gwenesis_vdp_hcounter());

  va_start(va, fmt);
  vfprintf(stdout, fmt, va);
  va_end(va);
  printf("\n");
}
#else
	#define vdpg_log(...)  do {} while(0)
#endif
/******************************************************************************
 *
 *  set screen buffers in which the rendering occurs
 *  Set original and scaled screen buffer for host
 *
 ******************************************************************************/
//host
void gwenesis_vdp_set_buffers(unsigned char *screen_buf, unsigned char *scaled_buffer)
{
    screen = screen_buf;
    scaled_screen = scaled_buffer;
}
//embedded
void gwenesis_vdp_set_buffer(unsigned short *ptr_screen_buffer)
{
    screen_buffer_line = ptr_screen_buffer;
    screen_buffer = ptr_screen_buffer;
}

/* ArgonOS addition; see screen_buffer_stride above.  0 is a stride too: every
 * line over the last, in one row (the CYD's headless bench).  Read as "the
 * default" it drew a whole frame, 150 KB, past the end of that row. */
void gwenesis_vdp_set_buffer_stride(int pixels)
{
    screen_buffer_stride = (pixels >= 0) ? pixels : SCREEN_WIDTH;
}

/******************************************************************************
 *
 *  Draw  Sprite character /8pixels in row
 *  without checking overdraw for pixels collision detection
 *  with Horizontal flip variation
 *  for Shadow/highlight :
 *    draw in fresh line buffer using draw_pattern_xxfliph_sprite(..)
 *  otherwise:
 *   draw over dirty planes using draw_pattern_xxfliph_sprite_over_planes(..)
 *
 ******************************************************************************/

 #define PIX0(P) ( ((P) & 0x000000F0 ) >>   4 )
 #define PIX1(P) ( ((P) & 0x0000000F ) >>   0 )
 #define PIX2(P) ( ((P) & 0x0000F000 ) >>  12 )
 #define PIX3(P) ( ((P) & 0x00000F00 ) >>   8 )
 #define PIX4(P) ( ((P) & 0x00F00000 ) >>  20 )
 #define PIX5(P) ( ((P) & 0x000F0000 ) >>  16 )
 #define PIX6(P) ( ((P) & 0xF0000000 ) >>  28 )
 #define PIX7(P) ( ((P) & 0x0F000000 ) >>  24 )

static inline __attribute__((always_inline))
void draw_pattern_nofliph_sprite(uint8_t *scr, uint32_t p, uint8_t attrs)
{
  if (p == 0) return;

  /*  not transparent pixel to write AND not already a sprite*/
  if (((PIX0(p))) && ((scr[0] & PIXATTR_SPRITE) == 0)) scr[0] = attrs | (PIX0(p));
  if (((PIX1(p))) && ((scr[1] & PIXATTR_SPRITE) == 0)) scr[1] = attrs | (PIX1(p));
  if (((PIX2(p))) && ((scr[2] & PIXATTR_SPRITE) == 0)) scr[2] = attrs | (PIX2(p));
  if (((PIX3(p))) && ((scr[3] & PIXATTR_SPRITE) == 0)) scr[3] = attrs | (PIX3(p));
  if (((PIX4(p))) && ((scr[4] & PIXATTR_SPRITE) == 0)) scr[4] = attrs | (PIX4(p));
  if (((PIX5(p))) && ((scr[5] & PIXATTR_SPRITE) == 0)) scr[5] = attrs | (PIX5(p));
  if (((PIX6(p))) && ((scr[6] & PIXATTR_SPRITE) == 0)) scr[6] = attrs | (PIX6(p));
  if (((PIX7(p))) && ((scr[7] & PIXATTR_SPRITE) == 0)) scr[7] = attrs | (PIX7(p));
}

static inline __attribute__((always_inline))
void draw_pattern_fliph_sprite(uint8_t *scr, uint32_t p, uint8_t attrs)
{
  if (p == 0) return;

  /*  not transparent pixel to write AND not already a sprite*/
  if (((PIX7(p))) && ((scr[0] & PIXATTR_SPRITE) == 0)) scr[0] = attrs | (PIX7(p));
  if (((PIX6(p))) && ((scr[1] & PIXATTR_SPRITE) == 0)) scr[1] = attrs | (PIX6(p));
  if (((PIX5(p))) && ((scr[2] & PIXATTR_SPRITE) == 0)) scr[2] = attrs | (PIX5(p));
  if (((PIX4(p))) && ((scr[3] & PIXATTR_SPRITE) == 0)) scr[3] = attrs | (PIX4(p));
  if (((PIX3(p))) && ((scr[4] & PIXATTR_SPRITE) == 0)) scr[4] = attrs | (PIX3(p));
  if (((PIX2(p))) && ((scr[5] & PIXATTR_SPRITE) == 0)) scr[5] = attrs | (PIX2(p));
  if (((PIX1(p))) && ((scr[6] & PIXATTR_SPRITE) == 0)) scr[6] = attrs | (PIX1(p));
  if (((PIX0(p))) && ((scr[7] & PIXATTR_SPRITE) == 0)) scr[7] = attrs | (PIX0(p));

}

static inline __attribute__((always_inline))
void draw_pattern_nofliph_sprite_over_planes(uint8_t *scr, uint32_t p, uint8_t attrs)
{
  if (p == 0) return; 

  /* High priority */
  if (attrs & PIXATTR_HIPRI) {

  /*  not transparent pixel to write AND not already a sprite*/
  if (((PIX0(p))) && ((scr[0] & PIXATTR_SPRITE) == 0)) scr[0] = attrs | (PIX0(p));
  if (((PIX1(p))) && ((scr[1] & PIXATTR_SPRITE) == 0)) scr[1] = attrs | (PIX1(p));
  if (((PIX2(p))) && ((scr[2] & PIXATTR_SPRITE) == 0)) scr[2] = attrs | (PIX2(p));
  if (((PIX3(p))) && ((scr[3] & PIXATTR_SPRITE) == 0)) scr[3] = attrs | (PIX3(p));
  if (((PIX4(p))) && ((scr[4] & PIXATTR_SPRITE) == 0)) scr[4] = attrs | (PIX4(p));
  if (((PIX5(p))) && ((scr[5] & PIXATTR_SPRITE) == 0)) scr[5] = attrs | (PIX5(p));
  if (((PIX6(p))) && ((scr[6] & PIXATTR_SPRITE) == 0)) scr[6] = attrs | (PIX6(p));
  if (((PIX7(p))) && ((scr[7] & PIXATTR_SPRITE) == 0)) scr[7] = attrs | (PIX7(p));

  }
  /* Low priority */
  else {

  /*  not transparent pixel to write AND not already a sprite or higher priority*/
  if (((PIX0(p))) && ((scr[0] & PIXATTR_SPRITE_HIPRI) == 0)) scr[0] = attrs | (PIX0(p));
  if (((PIX1(p))) && ((scr[1] & PIXATTR_SPRITE_HIPRI) == 0)) scr[1] = attrs | (PIX1(p));
  if (((PIX2(p))) && ((scr[2] & PIXATTR_SPRITE_HIPRI) == 0)) scr[2] = attrs | (PIX2(p));
  if (((PIX3(p))) && ((scr[3] & PIXATTR_SPRITE_HIPRI) == 0)) scr[3] = attrs | (PIX3(p));
  if (((PIX4(p))) && ((scr[4] & PIXATTR_SPRITE_HIPRI) == 0)) scr[4] = attrs | (PIX4(p));
  if (((PIX5(p))) && ((scr[5] & PIXATTR_SPRITE_HIPRI) == 0)) scr[5] = attrs | (PIX5(p));
  if (((PIX6(p))) && ((scr[6] & PIXATTR_SPRITE_HIPRI) == 0)) scr[6] = attrs | (PIX6(p));
  if (((PIX7(p))) && ((scr[7] & PIXATTR_SPRITE_HIPRI) == 0)) scr[7] = attrs | (PIX7(p));
  
  }
}

static inline __attribute__((always_inline))
void draw_pattern_fliph_sprite_over_planes(uint8_t *scr, uint32_t p, uint8_t attrs)
{
  if (p == 0) return;

  /* High priority */
  if (attrs & PIXATTR_HIPRI) {

  /*  not transparent pixel to write AND not already a sprite*/
  if (((PIX7(p))) && ((scr[0] & PIXATTR_SPRITE) == 0)) scr[0] = attrs | (PIX7(p));
  if (((PIX6(p))) && ((scr[1] & PIXATTR_SPRITE) == 0)) scr[1] = attrs | (PIX6(p));
  if (((PIX5(p))) && ((scr[2] & PIXATTR_SPRITE) == 0)) scr[2] = attrs | (PIX5(p));
  if (((PIX4(p))) && ((scr[3] & PIXATTR_SPRITE) == 0)) scr[3] = attrs | (PIX4(p));
  if (((PIX3(p))) && ((scr[4] & PIXATTR_SPRITE) == 0)) scr[4] = attrs | (PIX3(p));
  if (((PIX2(p))) && ((scr[5] & PIXATTR_SPRITE) == 0)) scr[5] = attrs | (PIX2(p));
  if (((PIX1(p))) && ((scr[6] & PIXATTR_SPRITE) == 0)) scr[6] = attrs | (PIX1(p));
  if (((PIX0(p))) && ((scr[7] & PIXATTR_SPRITE) == 0)) scr[7] = attrs | (PIX0(p));

  }
  /* Low priority */
  else {

  /*  not transparent pixel to write AND not already a sprite or higher priority*/
  if (((PIX7(p))) && ((scr[0] & PIXATTR_SPRITE_HIPRI) == 0)) scr[0] = attrs | (PIX7(p));
  if (((PIX6(p))) && ((scr[1] & PIXATTR_SPRITE_HIPRI) == 0)) scr[1] = attrs | (PIX6(p));
  if (((PIX5(p))) && ((scr[2] & PIXATTR_SPRITE_HIPRI) == 0)) scr[2] = attrs | (PIX5(p));
  if (((PIX4(p))) && ((scr[3] & PIXATTR_SPRITE_HIPRI) == 0)) scr[3] = attrs | (PIX4(p));
  if (((PIX3(p))) && ((scr[4] & PIXATTR_SPRITE_HIPRI) == 0)) scr[4] = attrs | (PIX3(p));
  if (((PIX2(p))) && ((scr[5] & PIXATTR_SPRITE_HIPRI) == 0)) scr[5] = attrs | (PIX2(p));
  if (((PIX1(p))) && ((scr[6] & PIXATTR_SPRITE_HIPRI) == 0)) scr[6] = attrs | (PIX1(p));
  if (((PIX0(p))) && ((scr[7] & PIXATTR_SPRITE_HIPRI) == 0)) scr[7] = attrs | (PIX0(p));
  
  }

}

/******************************************************************************
 *
 *  Draw  characters/8pixels in row
 *  without checking overdraw for pixels collision detection
 *  with Horizontal flip variation for plane A & B
 *
 ******************************************************************************/


static inline __attribute__((always_inline)) void
draw_pattern_nofliph_planeB(uint8_t *scr, uint32_t p, uint8_t attrs) {

  const uint8_t back = gwenesis_vdp_regs[7];

  if (p == 0) {

    scr[0] = back;
    scr[1] = back;
    scr[2] = back;
    scr[3] = back;
    scr[4] = back;
    scr[5] = back;
    scr[6] = back;
    scr[7] = back;

    return;
  }

  scr[0] = PIX0(p) ? attrs | (PIX0(p)) : back;
  scr[1] = PIX1(p) ? attrs | (PIX1(p)) : back;
  scr[2] = PIX2(p) ? attrs | (PIX2(p)) : back;
  scr[3] = PIX3(p) ? attrs | (PIX3(p)) : back;
  scr[4] = PIX4(p) ? attrs | (PIX4(p)) : back;
  scr[5] = PIX5(p) ? attrs | (PIX5(p)) : back;
  scr[6] = PIX6(p) ? attrs | (PIX6(p)) : back;
  scr[7] = PIX7(p) ? attrs | (PIX7(p)) : back;
}

static inline __attribute__((always_inline)) void
draw_pattern_fliph_planeB(uint8_t *scr, uint32_t p, uint8_t attrs) {

  const uint8_t back = gwenesis_vdp_regs[7];
  if (p == 0) {

    scr[0] = back;
    scr[1] = back;
    scr[2] = back;
    scr[3] = back;
    scr[4] = back;
    scr[5] = back;
    scr[6] = back;
    scr[7] = back;

    return;
  }

  scr[0] = PIX7(p) ? attrs | (PIX7(p)) : back;
  scr[1] = PIX6(p) ? attrs | (PIX6(p)) : back;
  scr[2] = PIX5(p) ? attrs | (PIX5(p)) : back;
  scr[3] = PIX4(p) ? attrs | (PIX4(p)) : back;
  scr[4] = PIX3(p) ? attrs | (PIX3(p)) : back;
  scr[5] = PIX2(p) ? attrs | (PIX2(p)) : back;
  scr[6] = PIX1(p) ? attrs | (PIX1(p)) : back;
  scr[7] = PIX0(p) ? attrs | (PIX0(p)) : back;


}

static inline __attribute__((always_inline)) void
draw_pattern_nofliph_planeAoverB(uint8_t *scr, uint32_t p, uint8_t attrs) {

  if (p == 0) return;

  if (attrs & PIXATTR_HIPRI) {

    if (PIX0(p)) scr[0] = attrs | (PIX0(p));
    if (PIX1(p)) scr[1] = attrs | (PIX1(p));
    if (PIX2(p)) scr[2] = attrs | (PIX2(p));
    if (PIX3(p)) scr[3] = attrs | (PIX3(p));
    if (PIX4(p)) scr[4] = attrs | (PIX4(p));
    if (PIX5(p)) scr[5] = attrs | (PIX5(p));
    if (PIX6(p)) scr[6] = attrs | (PIX6(p));
    if (PIX7(p)) scr[7] = attrs | (PIX7(p));

  } else {

    if (PIX0(p) && ((scr[0] & PIXATTR_HIPRI) == 0)) scr[0] = attrs | (PIX0(p));
    if (PIX1(p) && ((scr[1] & PIXATTR_HIPRI) == 0)) scr[1] = attrs | (PIX1(p));
    if (PIX2(p) && ((scr[2] & PIXATTR_HIPRI) == 0)) scr[2] = attrs | (PIX2(p));
    if (PIX3(p) && ((scr[3] & PIXATTR_HIPRI) == 0)) scr[3] = attrs | (PIX3(p));
    if (PIX4(p) && ((scr[4] & PIXATTR_HIPRI) == 0)) scr[4] = attrs | (PIX4(p));
    if (PIX5(p) && ((scr[5] & PIXATTR_HIPRI) == 0)) scr[5] = attrs | (PIX5(p));
    if (PIX6(p) && ((scr[6] & PIXATTR_HIPRI) == 0)) scr[6] = attrs | (PIX6(p));
    if (PIX7(p) && ((scr[7] & PIXATTR_HIPRI) == 0)) scr[7] = attrs | (PIX7(p));

  }
}

static inline __attribute__((always_inline)) void
draw_pattern_fliph_planeAoverB(uint8_t *scr, uint32_t p, uint8_t attrs) {

    if (p == 0) return;

    if (attrs & PIXATTR_HIPRI) {

    if (PIX7(p)) scr[0] = attrs | (PIX7(p));
    if (PIX6(p)) scr[1] = attrs | (PIX6(p));
    if (PIX5(p)) scr[2] = attrs | (PIX5(p));
    if (PIX4(p)) scr[3] = attrs | (PIX4(p));
    if (PIX3(p)) scr[4] = attrs | (PIX3(p));
    if (PIX2(p)) scr[5] = attrs | (PIX2(p));
    if (PIX1(p)) scr[6] = attrs | (PIX1(p));
    if (PIX0(p)) scr[7] = attrs | (PIX0(p));

  } else {

    if (PIX7(p) && ((scr[0] & PIXATTR_HIPRI) == 0)) scr[0] = attrs | (PIX7(p));
    if (PIX6(p) && ((scr[1] & PIXATTR_HIPRI) == 0)) scr[1] = attrs | (PIX6(p));
    if (PIX5(p) && ((scr[2] & PIXATTR_HIPRI) == 0)) scr[2] = attrs | (PIX5(p));
    if (PIX4(p) && ((scr[3] & PIXATTR_HIPRI) == 0)) scr[3] = attrs | (PIX4(p));
    if (PIX3(p) && ((scr[4] & PIXATTR_HIPRI) == 0)) scr[4] = attrs | (PIX3(p));
    if (PIX2(p) && ((scr[5] & PIXATTR_HIPRI) == 0)) scr[5] = attrs | (PIX2(p));
    if (PIX1(p) && ((scr[6] & PIXATTR_HIPRI) == 0)) scr[6] = attrs | (PIX1(p));
    if (PIX0(p) && ((scr[7] & PIXATTR_HIPRI) == 0)) scr[7] = attrs | (PIX0(p));

  }

}

/******************************************************************************
 *
 *  Draw  characters/8pixels in row
 *  with/without checking overdraw for pixels collision detection
 *  used for sprites and planes drawing
 *
 ******************************************************************************/
static inline __attribute__((always_inline))
void draw_pattern_sprite(uint8_t *scr, uint16_t name, int paty) {

  // uint16_t pat_addr = name << 5; //name * 32;
  // uint8_t pat_palette = BITS(name, 13, 2);
  // //unsigned int is_pat_pri = name & 0x8000;
  // uint8_t *pattern = VRAM + pat_addr;
 // uint8_t attrs = (pat_palette << 4) | ((name & 0x8000) ? PIXATTR_SPRITE_HIPRI : PIXATTR_SPRITE);
  uint8_t attrs = ( (name & 0x6000 ) >> 9 ) + ((name & 0x8000) >> 8) + PIXATTR_SPRITE;

  unsigned int  pattern;

  // Vertical flip ?
  // if (name & 0x1000)
  //   pattern += (7 - paty) * 4;
  // else
  //   pattern += paty * 4;

  // unsigned int  pattern;

  // Vertical flip ?
  if (name & 0x1000)
    pattern = VRAM_W((((name & 0x07FF) << 5) + ((7 - paty) * 4))); //) pat_addr;
  else
    pattern = VRAM_W((((name & 0x07FF) << 5) + (paty * 4)));

  // Horizontal flip ?
  if (name & 0x0800)
    draw_pattern_fliph_sprite(scr, pattern, attrs);
  else
    draw_pattern_nofliph_sprite(scr, pattern, attrs);
}

static inline __attribute__((always_inline))
void draw_pattern_sprite_over_planes(uint8_t *scr, uint16_t name, int paty) {

  // uint16_t pat_addr = name << 5 ; //* 32;
  // int pat_palette = BITS(name, 13, 2);
  // int is_pat_pri = name & 0x8000;
  // uint8_t *pattern = VRAM + pat_addr;
  // uint8_t attrs = (pat_palette << 4) | (is_pat_pri ? PIXATTR_SPRITE_HIPRI : PIXATTR_SPRITE);

  // Vertical flip ?
  // if (name & 0x1000)
  //   pattern += (7 - paty) * 4;
  // else
  //   pattern += paty * 4;

  //uint8_t attrs = ( (name & 0x6000 ) >> 9 ) | ((name & 0x8000) ? PIXATTR_SPRITE_HIPRI : PIXATTR_SPRITE);
  uint8_t attrs = ( (name & 0x6000 ) >> 9 ) + ((name & 0x8000) >> 8) + PIXATTR_SPRITE;
  //uint8_t attrs = ( (name >>9) & 0x70 ) | PIXATTR_SPRITE;

  unsigned int  pattern;

  // Vertical flip ?
  if (name & 0x1000)
    pattern = VRAM_W((((name & 0x07FF) << 5) + ((7 - paty) * 4))); //) pat_addr;
  else
    pattern = VRAM_W((((name & 0x07FF) << 5) + (paty * 4)));

  // Horizontal flip ?
  if (name & 0x0800)
    draw_pattern_fliph_sprite_over_planes (scr, pattern, attrs);
  else
    draw_pattern_nofliph_sprite_over_planes (scr, pattern, attrs);
}

#ifdef ARGON_MD_SMALL
/*
 * The CYD: a plane's pattern row as two words, all eight pixels at once.
 *
 * Pixel by pixel - a nibble out, a test, a byte stored, eight times - the
 * planes were 15-16 thousand of the 22 thousand cycles a line took (Sonic,
 * 30 Oct 2026).  Here a cell is a few dozen operations on words and two
 * stores when it lands on a word - which bought less than it looks: plane B
 * went from ~7700 cycles a line to ~7250, and with its name-table row found
 * once a line (name_row) to ~6800.  The rest is the ESP32 itself, roughly an
 * instruction a cycle: the renderer measured the same from IRAM as from
 * flash, and the same drawn a second time with everything in the cache.
 *
 * pat_cell: the row's pixels in memory order, pixel 0 in the low byte of lo;
 * PIX0..PIX7 above say which nibble is which.  nz_mask: 0xFF in each byte
 * that is not 0 (each byte 0..15).
 */
static inline __attribute__((always_inline)) void
pat_cell(uint32_t p, int fliph, uint32_t *lo, uint32_t *hi)
{
  const uint32_t a = (p >> 4) & 0x0F0F0F0Fu; /* byte k: pixel 2k   */
  const uint32_t b = p & 0x0F0F0F0Fu;        /* byte k: pixel 2k+1 */
  if (!fliph) {
    *lo = (a & 0xFFu) | ((b & 0xFFu) << 8) | ((a & 0xFF00u) << 8) |
          ((b & 0xFF00u) << 16);
    *hi = ((a >> 16) & 0xFFu) | ((b >> 8) & 0xFF00u) | ((a >> 8) & 0xFF0000u) |
          (b & 0xFF000000u);
  } else {
    *lo = (b >> 24) | ((a >> 16) & 0xFF00u) | (b & 0xFF0000u) |
          ((a << 8) & 0xFF000000u);
    *hi = ((b >> 8) & 0xFFu) | (a & 0xFF00u) | ((b << 16) & 0xFF0000u) |
          (a << 24);
  }
}

static inline __attribute__((always_inline)) uint32_t nz_mask(uint32_t v)
{
  const uint32_t nz = (v + 0x0F0F0F0Fu) & 0x10101010u;
  return (nz << 4) - (nz >> 4);
}

/* Eight bytes at scr, which is on a word or is not - the same for a whole
 * line, so the branch goes the same way every cell. */
static inline __attribute__((always_inline)) void
cell_put(uint8_t *scr, uint32_t lo, uint32_t hi)
{
  if (((unsigned long)scr & 3u) == 0u) {
    ((uint32_t *)(void *)scr)[0] = lo;
    ((uint32_t *)(void *)scr)[1] = hi;
    return;
  }
  scr[0] = (uint8_t)lo;
  scr[1] = (uint8_t)(lo >> 8);
  scr[2] = (uint8_t)(lo >> 16);
  scr[3] = (uint8_t)(lo >> 24);
  scr[4] = (uint8_t)hi;
  scr[5] = (uint8_t)(hi >> 8);
  scr[6] = (uint8_t)(hi >> 16);
  scr[7] = (uint8_t)(hi >> 24);
}

static inline __attribute__((always_inline)) void
cell_get(const uint8_t *scr, uint32_t *lo, uint32_t *hi)
{
  if (((unsigned long)scr & 3u) == 0u) {
    *lo = ((const uint32_t *)(const void *)scr)[0];
    *hi = ((const uint32_t *)(const void *)scr)[1];
    return;
  }
  *lo = (uint32_t)scr[0] | ((uint32_t)scr[1] << 8) | ((uint32_t)scr[2] << 16) |
        ((uint32_t)scr[3] << 24);
  *hi = (uint32_t)scr[4] | ((uint32_t)scr[5] << 8) | ((uint32_t)scr[6] << 16) |
        ((uint32_t)scr[7] << 24);
}

/* A name-table entry: one word read where FETCH16VRAM reads two bytes, each
 * through the page table.  Entries are two bytes on an even address. */
static inline __attribute__((always_inline)) unsigned int vram_name(unsigned int a)
{
  const uint32_t w = VRAM_W(a);
  const uint32_t h = (a & 2u) ? (w >> 16) : (w & 0xFFFFu);
  return ((h & 0xFFu) << 8) | ((h >> 8) & 0xFFu);
}
#define FETCH_NAME(A) vram_name(A)

/*
 * A plane's name-table row as words.  Without column scrolling every cell of
 * a line is on the same row of the table, so the row is found once a line,
 * not once a cell: a row is 64, 128 or 256 bytes on a multiple of its own size
 * and never crosses a page.
 */
static inline __attribute__((always_inline)) const volatile unsigned int *
name_row(unsigned int a)
{
  return (const volatile unsigned int *)(void *)md_vram[(a & VRAM_MASK) >> MD_PAGE_SHIFT] +
         ((a & MD_PAGE_MASK) >> 2);
}

static inline __attribute__((always_inline)) unsigned int
row_name(const volatile unsigned int *row, unsigned int col)
{
  const uint32_t w = row[col >> 1];
  const uint32_t h = (col & 1u) ? (w >> 16) : (w & 0xFFFFu);
  return ((h & 0xFFu) << 8) | ((h >> 8) & 0xFFu);
}

/* The pattern row of a cell: `name`'s vertical flip and row `paty`. */
static inline __attribute__((always_inline)) uint32_t
cell_row(unsigned int name, int paty)
{
  return VRAM_W((((name & 0x07FF) << 5) + (((name & 0x1000) ? 7 - paty : paty) * 4)));
}
#else
#define FETCH_NAME(A) FETCH16VRAM(A)
#endif

static inline __attribute__((always_inline))
void draw_pattern_planeB(uint8_t *scr, uint16_t name, int paty) {
#ifdef ARGON_MD_SMALL
  {
    const uint32_t bk = gwenesis_vdp_regs[7] * 0x01010101u;
    const uint32_t p = cell_row(name, paty);
    if (p == 0) {
      cell_put(scr, bk, bk);
      return;
    }
    const uint32_t at = (uint32_t)(((name & 0x6000) >> 9) + ((name & 0x8000) >> 8)) *
                        0x01010101u;
    uint32_t lo, hi;
    pat_cell(p, name & 0x0800, &lo, &hi);
    const uint32_t ml = nz_mask(lo), mh = nz_mask(hi);
    cell_put(scr, (ml & (lo | at)) | (~ml & bk), (mh & (hi | at)) | (~mh & bk));
    return;
  }
#endif
 // uint16_t pat_addr = name  << 5; // * 32;
 // uint8_t pat_palette = BITS(name, 13, 2);
 // unsigned int is_pat_pri = name & 0x8000;
  //uint8_t *pattern = VRAM + pat_addr;

  uint8_t attrs = ( (name & 0x6000 ) >> 9 ) + ((name & 0x8000) >> 8);

  unsigned int  pattern;

  // Vertical flip ?
  if (name & 0x1000)
    pattern = VRAM_W((((name & 0x07FF) << 5) + ((7 - paty) * 4))); //) pat_addr;
  else
    pattern = VRAM_W((((name & 0x07FF) << 5) + (paty * 4)));

//  if ((*(unsigned int *)pattern) == 0 ) return;
 // uint8_t *pattern = VRAM + ((name << 5) & 0xFFFF); //) pat_addr;
  //uint32_t pattern = VRAM[(name & 0x07FF) << 5]; //) pat_addr;

  // Horizontal flip ?
  if (name & 0x0800)
    draw_pattern_fliph_planeB(scr, pattern, attrs);

  else
    draw_pattern_nofliph_planeB(scr, pattern, attrs);

}

static inline __attribute__((always_inline))
void draw_pattern_planeA(uint8_t *scr, uint16_t name, int paty) {
#ifdef ARGON_MD_SMALL
  {
    const uint32_t p = cell_row(name, paty);
    if (p == 0)
      return;
    const uint32_t at = (uint32_t)(((name & 0x6000) >> 9) + ((name & 0x8000) >> 8)) *
                        0x01010101u;
    uint32_t lo, hi, olo, ohi;
    pat_cell(p, name & 0x0800, &lo, &hi);
    uint32_t ml = nz_mask(lo), mh = nz_mask(hi);
    cell_get(scr, &olo, &ohi);
    if (!(name & 0x8000)) {
      /* low priority: not over plane B's high-priority pixels */
      const uint32_t xl = (olo >> 7) & 0x01010101u, xh = (ohi >> 7) & 0x01010101u;
      ml &= ~((xl << 8) - xl);
      mh &= ~((xh << 8) - xh);
    }
    cell_put(scr, (ml & (lo | at)) | (~ml & olo), (mh & (hi | at)) | (~mh & ohi));
    return;
  }
#endif
  // uint16_t pat_addr = name << 5; //* 32;
  // uint8_t pat_palette = BITS(name, 13, 2);
  // unsigned int is_pat_pri = name & 0x8000;
  // uint8_t *pattern = VRAM + pat_addr;

    uint8_t attrs = ( (name & 0x6000 ) >> 9 ) + ((name & 0x8000) >> 8);




  unsigned int  pattern;

  // Vertical flip ?
  // if (name & 0x1000)
  //   pattern += (7 - paty) * 4;
  // else
  //   pattern += paty * 4;

  // Vertical flip ?
  if (name & 0x1000)
    pattern = VRAM_W((((name & 0x07FF) << 5) + ((7 - paty) * 4))); //) pat_addr;
  else
    pattern = VRAM_W((((name & 0x07FF) << 5) + (paty * 4)));

  // Horizontal flip ?
  if (name & 0x0800)
    draw_pattern_fliph_planeAoverB(scr, pattern, attrs);

  else
    draw_pattern_nofliph_planeAoverB(scr, pattern, attrs);

}

static  uint16_t ntwidth_x2;
static  uint16_t ntw_mask, nth_mask;

/******************************************************************************
 *
 *  Return the Horizontal scrolling
 *
 ******************************************************************************/

static inline __attribute__((always_inline))
unsigned int get_hscroll_vram(int line)
{

    int mode = REG11_HSCROLL_MODE;
    unsigned int table = REG13_HSCROLL_ADDRESS ;
    int idx;

    switch (mode)
    {
    case 0: // Full screen scrolling
        idx = 0;
        break;
    case 1: // First 8 lines
        idx = (line & 7);
        break;
    case 2: // Every row
        idx = (line & ~7);
        break;
    case 3: // Every line
        idx = line;
        break;
    }

    return table + idx*4;
}

/* a line's horizontal scroll, plane A (0) or B (2): from VRAM, or from the
 * copy a raster pass draws with (md_raster.c) */
#ifdef ARGON_MD_SMALL
extern uint16_t *const *md_vr_hs;
extern uint8_t *md_vr_sat;
#endif
static inline __attribute__((always_inline)) unsigned int md_hs_fetch(int line, unsigned int off)
{
#ifdef ARGON_MD_SMALL
  if (md_vr_hs != NULL)
    return md_vr_hs[line >> 4][((line & 15) << 1) | (off >> 1)]; /* 16-line pieces (md_raster.c) */
#endif
  return FETCH16VRAM(get_hscroll_vram(line) + off);
}
/******************************************************************************
 *
 *  Render PLANE B on screen line
 *
 ******************************************************************************/
 //__attribute__((optimize("unroll-loops")))
static inline __attribute__((always_inline))
void draw_line_b(int line)
{
  uint8_t *scr  = &render_buffer[PIX_OVERFLOW];

  unsigned int ntaddr = REG4_NAMETABLE_B;
  uint16_t scrollx=md_hs_fetch(line, 2) & 0x3FF;
  uint16_t *vsram = &VSRAM[1];
  uint8_t *end = scr + screen_width;

  //bool column_scrolling = BIT(gwenesis_vdp_regs[11], 2);
  const unsigned int column_scrolling = gwenesis_vdp_regs[11] & 0x4;

  // Invert horizontal scrolling (because it goes right, but we need to offset
  // of the first screen pixel)
  scrollx = -scrollx;
  uint8_t col = (scrollx >> 3) & ntw_mask;
  uint8_t patx = scrollx & 7;

  unsigned int numcell = 0;
  scr -= patx;
#ifdef ARGON_MD_SMALL
  if (!column_scrolling) {
    const uint16_t scrolly = *vsram + line;
    const volatile unsigned int *nrow =
        name_row(ntaddr + ((scrolly >> 3) & nth_mask) * ntwidth_x2);
    const int paty = scrolly & 7;
    for (; scr < end; scr += 8) {
      draw_pattern_planeB(scr, row_name(nrow, col), paty);
      col = (col + 1) & ntw_mask;
    }
    return;
  }
#endif
  while (scr < end) {
    // Calculate vertical scrolling for the current line
    uint16_t scrolly = *vsram + line;
    uint8_t row = (scrolly >> 3) & nth_mask;
    uint8_t paty = scrolly & 7;

   // unsigned int nt = ntaddr + row * (2 * ntwidth);
    unsigned int nt = ntaddr + row * ntwidth_x2;

    draw_pattern_planeB(scr, FETCH_NAME(nt + col * 2), paty);
    col = (col + 1) & ntw_mask;
    scr += 8;
    numcell++;

    // If per-column scrolling is active, increment VSRAM pointer
    if (column_scrolling && (numcell & 1) == 0)
      vsram += 2;
    }
}
/******************************************************************************
 *
 *  Render PLANE A and Window on screen line
 *
 ******************************************************************************/
//_attribute__((optimize("unroll-loops")))
static inline __attribute__((always_inline))
void draw_line_aw(int line) {

  uint8_t *scr  = &render_buffer[PIX_OVERFLOW];

  unsigned int ntaddr = REG2_NAMETABLE_A;
  uint16_t scrollx=md_hs_fetch(line, 0) & 0x3FF;
  uint16_t *vsram = &VSRAM[0];

  // Check if we are in the window region only
  // if it's the case, we cancel the plane A drawing
  int Window_line = REG18_WINDOW_VPOS * 8;
  //bool window_down = BIT(gwenesis_vdp_regs[18], 7);
  int window_down = gwenesis_vdp_regs[18] & 0x80;

  int PlanA_first = PlanA_firstcol;
  int PlanA_last = PlanA_lastcol;
  int Window_last = Window_lastcol;
  int Window_first = Window_firstcol;

  if (window_down) {
    if (line > Window_line) {
      PlanA_first = PlanA_last = 0;
      Window_last = screen_width;
      Window_first = 0;
    }
  } else {

    if (line < Window_line) {
      PlanA_first = PlanA_last = 0;
      Window_last = screen_width;
      Window_first = 0;
    }
  }

  // First draw A plane
  uint8_t *pos = scr + PlanA_first; // scr + screen_width;
  uint8_t *end = scr + PlanA_last;  // scr + screen_width

   //bool column_scrolling = BIT(gwenesis_vdp_regs[11], 2);
  const unsigned int column_scrolling = gwenesis_vdp_regs[11] & 0x4;

  // Invert horizontal scrolling (because it goes right, but we need to offset
  // of the first screen pixel)
  scrollx = -scrollx;
  uint8_t col = (scrollx >> 3) & ntw_mask;
  uint8_t patx = scrollx & 7;

  unsigned int numcell = 0;
  pos -= patx;
#ifdef ARGON_MD_SMALL
  if (!column_scrolling && pos < end) {
    const uint16_t scrolly = *vsram + line;
    const volatile unsigned int *nrow =
        name_row(ntaddr + ((scrolly >> 3) & nth_mask) * ntwidth_x2);
    const int paty = scrolly & 7;
    for (; pos < end; pos += 8) {
      draw_pattern_planeA(pos, row_name(nrow, col), paty);
      col = (col + 1) & ntw_mask;
    }
  }
#endif
  while (pos < end) {
    // Calculate vertical scrolling for the current line
    uint16_t scrolly = *vsram + line;
    uint8_t row = (scrolly >> 3) & nth_mask;
    uint8_t paty = scrolly & 7;

   // unsigned int nt = ntaddr + row * (2 * ntwidth);
    unsigned int nt = ntaddr + row * ntwidth_x2;

    draw_pattern_planeA(pos, FETCH_NAME(nt + col * 2), paty);

    col = (col + 1) & ntw_mask;
    pos += 8;
    numcell++;

    // If per-column scrolling is active, increment VSRAM pointer
    if (column_scrolling && (numcell & 1) == 0)
      vsram += 2;
  }

  // Second Draw Window Plane
  int row = line >> 3;
  int paty = line & 7;
  //int wdwidth = (screen_width == 320 ? 64 : 32);
  //unsigned int nt = base_w + row * 2 * wdwidth + Window_first / 4;

  int wdwidth_x2 = (screen_width == 320 ? 128 : 64);

  unsigned int nt = base_w + row * wdwidth_x2 + Window_first / 4;

  for (int i = Window_first / 8; i < Window_last / 8; ++i) {
    draw_pattern_planeA(end, FETCH_NAME(nt), paty);
    nt += 2;
    end += 8;
  }
}

/******************************************************************************
 *
 *  Render SPRITES on screen line
 *
 ******************************************************************************/

//__attribute__((optimize("unroll-loops")))
static inline __attribute__((always_inline)) 
void draw_sprites_over_planes(int line)
{
    uint8_t *scr;

    scr = &render_buffer[PIX_OVERFLOW];

   // uint8_t mask = mode_h40 ? 0x7E : 0x7F;
   // uint8_t *start_table = VRAM + ((gwenesis_vdp_regs[5] & mask) << 9);

    uint8_t *start_table = VRAM + (REG5_SAT_ADDRESS & VRAM_MASK);

    // This is both the size of the table as seen by the VDP
    // *and* the maximum number of sprites that are processed
    // (important in case of infinite loops in links).
    const int SPRITE_TABLE_SIZE     = (screen_width == 320) ?  80 :  64;
    const int MAX_SPRITES_PER_LINE  = (screen_width == 320) ?  20 :  16;
    const int MAX_PIXELS_PER_LINE   = (screen_width == 320) ? 320 : 256;

    bool masking = false, one_sprite_nonzero = false; // overdraw = false;
    int sidx = 0, num_sprites = 0, num_pixels = 0;
    for (int i = 0; (i < SPRITE_TABLE_SIZE) && sidx < (SPRITE_TABLE_SIZE); ++i)
    {
#ifdef ARGON_MD_SMALL
        unsigned int ent_[2];   /* the entry by words: VRAM is in IRAM */
        uint8_t *table, *cache;
        if (md_vr_sat != NULL) {
          /* the frame's own table (md_raster.c), not the next frame's */
          table = cache = md_vr_sat + sidx * 8;
        } else {
          unsigned int o_ = (unsigned int)(REG5_SAT_ADDRESS & VRAM_MASK) + sidx * 8;
          ent_[0] = VRAM_W(o_); ent_[1] = VRAM_W(o_ + 4);
          /* no cache of the table's first half here: its 640 bytes are
           * the record's copy (md_raster.c) - Castlevania Bloodlines' trick
           * with a moved table is not followed */
          table = cache = (uint8_t *)ent_;
        }
#else
        uint8_t *table = start_table + sidx*8;
        uint8_t *cache = SAT_CACHE + sidx*8;
#endif
        //uint8_t *cache = start_table + sidx*8;
        

        int sy = ((cache[0] & 0x3) << 8) | cache[1];
        int sx = ((table[6] & 0x3) << 8) | table[7];
        uint16_t name = (table[4] << 8) | table[5];


        int sh = BITS(cache[2], 0, 2) + 1;
        int link = BITS(cache[3], 0, 7);

        int isflipv = table[4] & 0x10;
        int isfliph = table[4] & 0x8;

        int sw = BITS(table[2], 2, 2) + 1;

        sy -= 128;
        if ((line >= sy) && (line < sy+sh*8))
        {
            // Sprite masking: a sprite on column 0 masks
            // any lower-priority sprite, but with the following conditions
            //   * it only works from the second visible sprite on each line
            //   * if the previous line had a sprite pixel overflow, it
            //     works even on the first sprite
            // Notice that we need to continue parsing the table after masking
            // to see if we reach a pixel overflow (because it would affect masking
            // on next line).
            if (sx == 0)
            {
                if (one_sprite_nonzero || (sprite_overflow == line-1))
                    masking = true;
            }
            else
                one_sprite_nonzero = true;

            int row = (line - sy) >> 3;
            int paty = (line - sy) & 7;
            if (isflipv)
                row = sh - row - 1;

            sx -= 128;
            if ((sx > (-sw * 8)) && (sx < screen_width) && !masking) {

              name += row;

              if (isfliph) {
                name += sh * (sw - 1);
                for (int p = 0; (p < sw) && (num_pixels < MAX_PIXELS_PER_LINE); p++) {

                  draw_pattern_sprite_over_planes(scr + sx + p * 8, name, paty);
                  name -= sh;
                  num_pixels += 8;

                }
              } else {
                for (int p = 0; (p < sw) && (num_pixels < MAX_PIXELS_PER_LINE); p++) {

                  draw_pattern_sprite_over_planes(scr + sx + p * 8, name, paty);
                  name += sh;
                  num_pixels += 8;

                }
              }
            }
            else
                num_pixels += sw*8;

            if (num_pixels >= MAX_PIXELS_PER_LINE)
            {
                sprite_overflow = line;
                break;
            }
            if (++num_sprites >= MAX_SPRITES_PER_LINE)
                break;
        }

        if (link == 0) break;
        sidx = link;
    }

  //  if (overdraw)
  //      sprite_collision = true;
}
static inline __attribute__((always_inline)) 
void draw_sprites(int line)
{
  uint8_t *scr;

  scr = &sprite_buffer[PIX_OVERFLOW];

  // uint8_t mask = mode_h40 ? 0x7E : 0x7F;
  // uint8_t *start_table = VRAM + ((gwenesis_vdp_regs[5] & mask) << 9);

  uint8_t *start_table = VRAM + (REG5_SAT_ADDRESS & VRAM_MASK);

  // This is both the size of the table as seen by the VDP
  // *and* the maximum number of sprites that are processed
  // (important in case of infinite loops in links).
  const int SPRITE_TABLE_SIZE = (screen_width == 320) ? 80 : 64;
  const int MAX_SPRITES_PER_LINE = (screen_width == 320) ? 20 : 16;
  const int MAX_PIXELS_PER_LINE = (screen_width == 320) ? 320 : 256;

  bool masking = false, one_sprite_nonzero = false; // overdraw = false;
  int sidx = 0, num_sprites = 0, num_pixels = 0;
  for (int i = 0; i < SPRITE_TABLE_SIZE && sidx < SPRITE_TABLE_SIZE; ++i) {
#ifdef ARGON_MD_SMALL
    unsigned int ent_[2];   /* the entry by words: VRAM is in IRAM */
    uint8_t *table;
    if (md_vr_sat != NULL) {
      table = md_vr_sat + sidx * 8; /* the frame's own table (md_raster.c) */
    } else {
      unsigned int o_ = (unsigned int)(REG5_SAT_ADDRESS & VRAM_MASK) + sidx * 8;
      ent_[0] = VRAM_W(o_); ent_[1] = VRAM_W(o_ + 4);
      table = (uint8_t *)ent_;
    }
#else
    uint8_t *table = start_table + sidx * 8;
#endif
    uint8_t *cache = table;

    //uint8_t *cache = SAT_CACHE + sidx * 8;

    int sy = ((cache[0] & 0x3) << 8) | cache[1];
    int sx = ((table[6] & 0x3) << 8) | table[7];
    uint16_t name = (table[4] << 8) | table[5];

    int sh = BITS(cache[2], 0, 2) + 1;
    int link = BITS(cache[3], 0, 7);

    int isflipv = table[4] & 0x10;
    int isfliph = table[4] & 0x8;

    int sw = BITS(table[2], 2, 2) + 1;

    sy -= 128;
    if (line >= sy && line < sy + sh * 8) {
      // Sprite masking: a sprite on column 0 masks
      // any lower-priority sprite, but with the following conditions
      //   * it only works from the second visible sprite on each line
      //   * if the previous line had a sprite pixel overflow, it
      //     works even on the first sprite
      // Notice that we need to continue parsing the table after masking
      // to see if we reach a pixel overflow (because it would affect masking
      // on next line).
      if (sx == 0) {
        if (one_sprite_nonzero || sprite_overflow == line - 1)
          masking = true;
      } else
        one_sprite_nonzero = true;

      int row = (line - sy) >> 3;
      int paty = (line - sy) & 7;
      if (isflipv)
        row = sh - row - 1;

      sx -= 128;
      if (sx > -sw * 8 && sx < screen_width && !masking) {

        name += row;

        if (isfliph) {
          name += sh * (sw - 1);
          for (int p = 0; p < sw && num_pixels < MAX_PIXELS_PER_LINE; p++) {

            draw_pattern_sprite(scr + sx + p * 8, name, paty);
            name -= sh;
            num_pixels += 8;
          }
        } else {
          for (int p = 0; p < sw && num_pixels < MAX_PIXELS_PER_LINE; p++) {

            draw_pattern_sprite(scr + sx + p * 8, name, paty);
            name += sh;
            num_pixels += 8;
          }
        }
      } else
        num_pixels += sw * 8;

      if (num_pixels >= MAX_PIXELS_PER_LINE) {
        sprite_overflow = line;
        break;
      }
      if (++num_sprites >= MAX_SPRITES_PER_LINE)
        break;
    }

    if (link == 0)
      break;
    sidx = link;
    }

  //  if (overdraw)
  //      sprite_collision = true;
}
/******************************************************************************
 *
 *  Parse PLANE A/B size,scrolling at the start of image rendering
 *
 ******************************************************************************/
//static unsigned short current_line[320];

void gwenesis_vdp_render_config()
{
    mode_h40 = REG12_MODE_H40;
    mode_pal = REG1_PAL;

    int ntwidth = BITS(gwenesis_vdp_regs[16], 0, 2);
    int ntheight = BITS(gwenesis_vdp_regs[16], 4, 2);
    ntwidth = (ntwidth + 1) * 32;
    ntheight = (ntheight + 1) * 32;
    ntw_mask = ntwidth - 1;
    nth_mask = ntheight - 1;
    ntwidth_x2= ntwidth *2;

    // Window & A planes separation

    if (mode_h40)
        base_w = ((REG3_NAMETABLE_W & 0x1e) << 11);
    else
        base_w = ((REG3_NAMETABLE_W & 0x1f) << 11);


    bool window_right = BIT(gwenesis_vdp_regs[17], 7);

    // int window_is_bugged = 0;
    PlanA_firstcol = 0;
    PlanA_lastcol = screen_width;

    Window_firstcol = 0;
    Window_lastcol = 0;

    if (window_right) {

      Window_firstcol = REG17_WINDOW_HPOS * 16;
      Window_lastcol = screen_width;

      if (Window_firstcol > Window_lastcol)
        Window_firstcol = Window_lastcol;

      PlanA_firstcol = 0;
      PlanA_lastcol = Window_firstcol;

    } else {
      Window_firstcol = 0;
      Window_lastcol = REG17_WINDOW_HPOS * 16;
      if (Window_lastcol > screen_width)
        Window_lastcol = screen_width;

      PlanA_firstcol = Window_lastcol;
      PlanA_lastcol = screen_width;
      // if (Window_lastcol != 0)
      //      window_is_bugged = 1;
    }
}

/******************************************************************************
 *
 *  Render a line on screen
 *  Get selected line and render it on screen processing each plane.
 *
 ******************************************************************************/

//#define CONV(b)   ((0b11 1110 0000 0000 0000 0000 0000 & b)>>10) | ((0b00000 1111 1100 0000 0000 & b)>>5) | ((0b0000000000011111 & b))
//#define SPACE(c)  ((0b00 0000 0000 1111 1000 0000 0000 & c)<<10) | ((0b00000 0000 0111 1110 0000 & c)<<5) | ((0b0000000000011111 & c))

#define CONV(b)   ((0x3e00000 & b)>>10) | ((0xfc00 & b)>>5) | ((0x1f & b))
#define SPACE(c)  ((0xe800 & c)<<10) | ((0x7e0 & c)<<5) | ((0x1f & c))

__attribute__((optimize("unroll-loops"))) static void
blit_4to5_line(uint16_t *in, uint16_t *out) {

  uint16_t *src_row = in;
  uint16_t *dest_row = out;
  for (int x_src = 0, x_dst = 0; x_src < 256; x_src += 4, x_dst += 5) {
    uint32_t b0 = SPACE(src_row[x_src]);
    uint32_t b1 = SPACE(src_row[x_src + 1]);
    uint32_t b2 = SPACE(src_row[x_src + 2]);
    uint32_t b3 = SPACE(src_row[x_src + 3]);

    dest_row[x_dst] = CONV(b0);
    dest_row[x_dst + 1] = CONV((b0 + b0 + b0 + b1) >> 2);
    dest_row[x_dst + 2] = CONV((b1 + b2) >> 1);
    dest_row[x_dst + 3] = CONV((b2 + b2 + b2 + b3) >> 2);
    dest_row[x_dst + 4] = CONV(b3);
  }
}

#ifdef MD_VDPPROF
static inline unsigned int md_ccount(void)
{
    unsigned int c;
    __asm__ volatile("rsr %0, ccount" : "=a"(c));
    return c;
}
/* -DMD_VDPPROF: a line's cycles by part - plane B, plane A and the window,
 * the sprites, the colours - and the lines counted */
unsigned int md_vp_b, md_vp_a, md_vp_s, md_vp_c, md_vp_n;
unsigned int md_vp_cold, md_vp_warm, md_vp_wn;
#define VP_MARK(v) do { const unsigned int c_ = md_ccount(); v += c_ - vp_; vp_ = c_; } while (0)
#else
#define VP_MARK(v) do { } while (0)
#endif

MD_HOT_VDP void gwenesis_vdp_render_line(int line)
{
  mode_h40 = REG12_MODE_H40;
  //mode_pal = REG1_PAL;

  vdpg_log(__FUNCTION__,": %3d",line);

  //unsigned int line = scan_line;
  //  if (line == 0) gwenesis_vdp_render_config();

  // interlace mode not implemented
  if (BITS(gwenesis_vdp_regs[12], 1, 2) != 0)
    return;

  if (line >= (REG1_PAL ? 240 : 224))
    return;


#if GNW_TARGET_MARIO != 0 | GNW_TARGET_ZELDA != 0 | ARGON_TARGET != 0

  screen_buffer_line = &screen_buffer[line * screen_buffer_stride];

  // Disable display >> black screen
  if (REG0_DISABLE_DISPLAY){
    memset(screen_buffer_line, 0, SCREEN_WIDTH*2);
    return;
  }

  // Display is not enabled. fill with background colour
  if (REG1_DISP_ENABLED == 0) {
    for (int px=0; px < 320;px++) 
      screen_buffer_line[px]=CRAM565[0];
    return;
  }
  #else

  // Disable display >> black screen
  if (REG0_DISABLE_DISPLAY){
    memset(screen, 0, SCREEN_WIDTH*3);
    return;
  }

  // Dsiaply is not enabled. fill with background colour
  if (REG1_DISP_ENABLED == 0) {
      uint16_t rgb565 = CRAM565[0];
      uint8_t r, g, b;
      r = (rgb565 & 0xF800) >> 8;
      g = (rgb565 & 0X07E0) >> 3;
      b = (rgb565 & 0x001F) << 3;

    for (int px=0; px < 320;px++) {
      screen[3*px]=r;
      screen[3*px+1]=g;
      screen[3*px+2]=b;
    }
    return;
  }

#endif

  uint8_t *pb = &render_buffer[PIX_OVERFLOW];
  uint8_t *ps = &sprite_buffer[PIX_OVERFLOW];

#ifdef MD_VDPPROF
  unsigned int vp_ = md_ccount();
  md_vp_n++;
#endif
  if (MODE_SHI)
    memset(ps, 0, 320);

  draw_line_b(line);
  VP_MARK(md_vp_b);
  draw_line_aw(line);
  VP_MARK(md_vp_a);

  if (MODE_SHI)
    draw_sprites(line);
  else
    draw_sprites_over_planes(line);
  VP_MARK(md_vp_s);

#if GNW_TARGET_MARIO != 0 | GNW_TARGET_ZELDA != 0 | ARGON_TARGET != 0

  if (screen_width == 320) {
    /* Mode Highlight/shadow is enabled */
    if (MODE_SHI) {
      for (int x = 0; x < screen_width; x++) {
        uint8_t plane = pb[x];
        uint8_t sprite = ps[x];

        if ((plane & 0xC0) < (sprite & 0xC0)) {
          switch (sprite & 0x3F) {
          // Palette=3, Sprite=14 :> draw plane, force highlight
          case 0x3E:
            screen_buffer_line[x] = 0x8410 | CRAM565[plane] >> 1;
            break;
          // Palette=3, Sprite=15 :> draw plane, force shadow
          case 0x3F:
            screen_buffer_line[x] = CRAM565[plane] >> 1;
            break;
          // draw sprite, normal
          default:
            screen_buffer_line[x] = CRAM565[sprite];
            break;
          }
        } else {
          screen_buffer_line[x] = CRAM565[plane];
        }
      }

      /* Normal mode*/
    } else {

      uint32_t *video_out = (uint32_t *)&screen_buffer_line[0];

#ifdef ARGON_MD_SMALL
      /* four pixels a word read out of the line, two words out */
      const uint32_t *in4 = (const uint32_t *)(const void *)pb;
      for (int x = 0; x < screen_width; x += 4) {
        const uint32_t w = *in4++;
        video_out[0] = CRAM565[w & 0xFFu] | (uint32_t)CRAM565[(w >> 8) & 0xFFu] << 16;
        video_out[1] = CRAM565[(w >> 16) & 0xFFu] | (uint32_t)CRAM565[w >> 24] << 16;
        video_out += 2;
      }
      if (0)
#endif
      for (int x = 0; x < screen_width; x += 2) {

        // screen_buffer_line[x] = CRAM565[pb[x]];
        //  2 pixels : 32 bits write  access is faster
        *video_out++ = CRAM565[pb[x]] | CRAM565[pb[x + 1]] << 16;
      }
    }
    VP_MARK(md_vp_c);
    /* upscale mode H32 to H40 */
  } else {

#ifdef ARGON_MD_SMALL
    /*
     * The 256 pixels in the row's own last 256, and upscaled into the row in
     * place: blit_4to5_line reads four pixels before it writes five and never
     * writes past what it has read.  On the stack the two lines were 1152
     * bytes of every thread that draws, which on the CYD is two of them.
     */
    uint16_t *const buffer_line_H32 = &screen_buffer_line[320 - 256];
#else
    uint16_t buffer_line_H32[256];
#endif

    /* Mode Highlight/shadow is enabled */
    if (MODE_SHI) {
      for (int x = 0; x < screen_width; x++) {
        uint8_t plane = pb[x];
        uint8_t sprite = ps[x];

        if ((plane & 0xC0) < (sprite & 0xC0)) {
          switch (sprite & 0x3F) {
          // Palette=3, Sprite=14 :> draw plane, force highlight
          case 0x3E:
            buffer_line_H32[x] = 0x8410 | CRAM565[plane] >> 1;
            break;
          // Palette=3, Sprite=15 :> draw plane, force shadow
          case 0x3F:
            buffer_line_H32[x] = CRAM565[plane] >> 1;
            break;
          // draw sprite, normal
          default:
            buffer_line_H32[x] = CRAM565[sprite];
            break;
          }
        } else {
          buffer_line_H32[x] = CRAM565[plane];
        }
      }

      /* Normal mode*/
    } else {

      for (int x = 0; x < screen_width; x++)
        buffer_line_H32[x] = CRAM565[pb[x]];
    }

#ifdef ARGON_MD_SMALL
    blit_4to5_line(buffer_line_H32, screen_buffer_line);
#else
    uint16_t scaled_buffer_line[320];
    
    blit_4to5_line(buffer_line_H32, scaled_buffer_line);

    uint32_t *video_out = (uint32_t *)&screen_buffer_line[0];

    for (int x = 0; x < 320; x += 2) {

      // screen_buffer_line[x] = CRAM565[pb[x]];
      //  2 pixels : 32 bits write  access is faster
      *video_out++ = scaled_buffer_line[x] | scaled_buffer_line[x + 1] << 16;
    }
#endif
  }
#else
  uint16_t rgb565;
  uint16_t tmp_line[320];
  uint16_t scaled_line[320];

  /* Mode Highlight/shadow is enabled */
  if (MODE_SHI) {
    for (int x = 0; x < screen_width; x++) {
      uint8_t plane = pb[x];
      uint8_t sprite = ps[x];

      if ((plane & 0xC0) < (sprite & 0xC0) ) {
        switch (sprite & 0x3F) {
        // Palette=3, Sprite=14 :> draw plane, force highlight
        case 0x3E:
          rgb565 = 0x8410 | CRAM565[plane] >> 1;
          break;
        // Palette=3, Sprite=15 :> draw plane, force shadow
        case 0x3F:
          rgb565 = CRAM565[plane] >> 1;
          break;
        // draw sprite, normal
        default:
          rgb565 = CRAM565[sprite];
          break;
        }
      } else {
        rgb565 = CRAM565[plane];
      }
      tmp_line[x]=rgb565;
      /*
      uint8_t r, g, b;
      r = (rgb565 & 0xF800) >> 8;
      g = (rgb565 & 0X07E0) >> 3;
      b = (rgb565 & 0x001F) << 3;
      int pixel = ((240 - screen_height) / 2 + (line)) * 320 + (x) +
                  (320 - screen_width) / 2;
      screen[pixel * 4 + 2] = r;
      screen[pixel * 4 + 1] = g;
      screen[pixel * 4 + 0] = b;
      */
    }

    /* Normal mode*/
  } else {
    for (int x = 0; x < screen_width; x++) {
      tmp_line[x]=CRAM565[pb[x]];

      /*
      rgb565 = CRAM565[pb[x]];
      uint8_t r, g, b;
      r = (rgb565 & 0xF800) >> 8;
      g = (rgb565 & 0X07E0) >> 3;
      b = (rgb565 & 0x001F) << 3;

      int pixel = ((240 - screen_height) / 2 + (line)) * 320 + (x) +
                  (320 - screen_width) / 2;
      screen[pixel * 4 + 2] = r;
      screen[pixel * 4 + 1] = g;
      screen[pixel * 4 + 0] = b;
      */
    }
  }
  // Mode H32  horizontal upscale 256 >>> 320
  if ( screen_width == 256) {

  blit_4to5_line(tmp_line,scaled_line);

  // RGB565 to RGB24
  for (int x = 0; x < 320; x++) {
    uint8_t r, g, b;
    r = (scaled_line[x] & 0xF800) >> 8;
    g = (scaled_line[x] & 0X07E0) >> 3;
    b = (scaled_line[x] & 0x001F) << 3;
    int pixel = ((240 - screen_height) / 2 + (line)) * 320 + (x); // + (320 - screen_width) / 2;
    screen[pixel * 4 + 2] = r;
    screen[pixel * 4 + 1] = g;
    screen[pixel * 4 + 0] = b;
  }
  // Mode H40
  } else {

  // RGB565 to RGB24
  for (int x = 0; x < 320; x++) {
    uint8_t r, g, b;
    r = (tmp_line[x] & 0xF800) >> 8;
    g = (tmp_line[x] & 0X07E0) >> 3;
    b = (tmp_line[x] & 0x001F) << 3;
    int pixel = ((240 - screen_height) / 2 + (line)) * 320 + (x);// + (320 - screen_width) / 2;
    screen[pixel * 4 + 2] = r;
    screen[pixel * 4 + 1] = g;
    screen[pixel * 4 + 0] = b;
  }
  }


#endif
}

void gwenesis_vdp_gfx_save_state() {
  /*
  SaveState* state;
  state = saveGwenesisStateOpenForWrite("vdp_gfx");
  saveGwenesisStateSetBuffer(state, "render_buffer", render_buffer, sizeof(render_buffer));
  saveGwenesisStateSetBuffer(state, "sprite_buffer", sprite_buffer, sizeof(sprite_buffer));
  saveGwenesisStateSet(state, "mode_h40", mode_h40);
  saveGwenesisStateSet(state, "mode_pal", mode_pal);
  saveGwenesisStateSet(state, "screen_width", screen_width);
  saveGwenesisStateSet(state, "screen_height", screen_height);
  saveGwenesisStateSet(state, "sprite_overflow", sprite_overflow);
  saveGwenesisStateSet(state, "sprite_collision", sprite_collision);
  saveGwenesisStateSet(state, "base_w", base_w);
  saveGwenesisStateSet(state, "PlanA_firstcol", PlanA_firstcol);
  saveGwenesisStateSet(state, "PlanA_lastcol", PlanA_lastcol);
  saveGwenesisStateSet(state, "Window_firstcol", Window_firstcol);
  saveGwenesisStateSet(state, "Window_lastcol", Window_lastcol);
  */
}

void gwenesis_vdp_gfx_load_state() {
  /*
    SaveState* state = saveGwenesisStateOpenForRead("vdp_gfx");
    saveGwenesisStateGetBuffer(state, "render_buffer", render_buffer, sizeof(render_buffer));
    saveGwenesisStateGetBuffer(state, "sprite_buffer", sprite_buffer, sizeof(sprite_buffer));
    mode_h40 = saveGwenesisStateGet(state, "mode_h40");
    mode_pal = saveGwenesisStateGet(state, "mode_pal");
    screen_width = saveGwenesisStateGet(state, "screen_width");
    screen_height = saveGwenesisStateGet(state, "screen_height");
    sprite_overflow = saveGwenesisStateGet(state, "sprite_overflow");
    sprite_collision = saveGwenesisStateGet(state, "sprite_collision");
    base_w = saveGwenesisStateGet(state, "base_w");
    PlanA_firstcol = saveGwenesisStateGet(state, "PlanA_firstcol");
    PlanA_lastcol = saveGwenesisStateGet(state, "PlanA_lastcol");
    Window_firstcol = saveGwenesisStateGet(state, "Window_firstcol");
    Window_lastcol = saveGwenesisStateGet(state, "Window_lastcol");
    */
}
