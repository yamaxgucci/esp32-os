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
#ifndef _gwenesis_vdp_H_
#define _gwenesis_vdp_H_

#pragma once

#define BIT(v, idx) (((v) >> (idx)) & 1)
#define BITS(v, idx, n) (((v) >> (idx)) & ((1 << (n)) - 1))

// VDP registers
#define REG0_DISABLE_DISPLAY (gwenesis_vdp_regs[0] & 1)
#define REG0_HVLATCH BIT(gwenesis_vdp_regs[0], 1)
#define REG0_LINE_INTERRUPT BIT(gwenesis_vdp_regs[0], 4)
#define REG1_PAL BIT(gwenesis_vdp_regs[1], 3)
#define REG1_240_LINE ((gwenesis_vdp_regs[1] & 0x08) >> 3)
#define REG1_DMA_ENABLED BIT(gwenesis_vdp_regs[1], 4)
#define REG1_VBLANK_INTERRUPT BIT(gwenesis_vdp_regs[1], 5)
#define REG1_DISP_ENABLED BIT(gwenesis_vdp_regs[1], 6)
#define REG2_NAMETABLE_A (BITS(gwenesis_vdp_regs[2], 3, 3) << 13)
#define REG3_NAMETABLE_W BITS(gwenesis_vdp_regs[3], 1, 5)
#define REG4_NAMETABLE_B (BITS(gwenesis_vdp_regs[4], 0, 3) << 13)
//#define REG5_SAT_ADDRESS ((gwenesis_vdp_regs[5] & (mode_h40 ? 0x7E : 0x7F)) << 9)
//#define REG5_SAT_SIZE (mode_h40 ? (1 << 10) : (1 << 9))
#define REG5_SAT_ADDRESS ((gwenesis_vdp_regs[5] & ((gwenesis_vdp_regs[12] & 0x01) ? 0x7E : 0x7F)) << 9)
#define REG5_SAT_SIZE ((gwenesis_vdp_regs[12] & 0x01) ? (1 << 10) : (1 << 9))
#define REG10_LINE_COUNTER BITS(gwenesis_vdp_regs[10], 0, 8)
#define REG10_COLUMN_COUNTER BITS(gwenesis_vdp_regs[10], 8, 15)
#define REG11_HSCROLL_MODE ((gwenesis_vdp_regs[11] & 3))
#define REG11_VSCROLL_MODE ((gwenesis_vdp_regs[11] & 4) >> 2)
#define REG12_RS0 (gwenesis_vdp_regs[12] & 0x80) >> 7
#define REG12_RS1 (gwenesis_vdp_regs[12] & 0x01) >> 0
#define REG12_MODE_H40 (gwenesis_vdp_regs[12] & 1)
#define REG13_HSCROLL_ADDRESS (gwenesis_vdp_regs[13] << 10)
#define REG15_DMA_INCREMENT gwenesis_vdp_regs[15]
#define REG16_UNUSED1 ((gwenesis_vdp_regs[16] & 0xc0) >> 6)
#define REG16_VSCROLL_SIZE ((gwenesis_vdp_regs[16] >> 4) & 3)
#define REG16_UNUSED2 ((gwenesis_vdp_regs[16] & 0x0c) >> 2)
#define REG16_HSCROLL_SIZE (gwenesis_vdp_regs[16] & 3)
#define REG17_WINDOW_HPOS BITS(gwenesis_vdp_regs[17], 0, 5)
#define REG17_WINDOW_RIGHT ((gwenesis_vdp_regs[17] & 0x80) >> 7)
#define REG18_WINDOW_DOWN ((gwenesis_vdp_regs[0x12] & 0x80) >> 7)
#define REG18_WINDOW_VPOS BITS(gwenesis_vdp_regs[18], 0, 5)
#define REG19_DMA_LENGTH (gwenesis_vdp_regs[19] | (gwenesis_vdp_regs[20] << 8))
#define REG21_DMA_SRCADDR_LOW (gwenesis_vdp_regs[21] | (gwenesis_vdp_regs[22] << 8))
#define REG23_DMA_SRCADDR_HIGH ((gwenesis_vdp_regs[23] & 0x7F) << 16)
#define REG23_DMA_TYPE BITS(gwenesis_vdp_regs[23], 6, 2)

//VDP status register
#define STATUS_FIFO_EMPTY (1 << 9)
#define STATUS_FIFO_FULL (1 << 8)
#define STATUS_VIRQPENDING (1 << 7)
#define STATUS_SPRITEOVERFLOW (1 << 6)
#define STATUS_SPRITECOLLISION (1 << 5)
#define STATUS_ODDFRAME (1 << 4)
#define STATUS_VBLANK (1 << 3)
#define STATUS_HBLANK (1 << 2)
#define STATUS_DMAPROGRESS (1 << 1)
#define STATUS_PAL (1 << 0)

#define VRAM_MAX_SIZE 0x10000    // VRAM maximum size
#ifdef ARGON_MD_BENCH_VRAM32
/* MEASUREMENT ONLY: 32 KB of VRAM, addresses folded into it.  The picture is
 * wrong; the work a frame costs is not.  Not a build anyone plays. */
#define VRAM_MASK 0x7FFF
#else
#define VRAM_MASK 0xFFFF
#endif
#ifdef ARGON_MD_SMALL
/*
 * The CYD keeps VRAM in pages (md_pages.h), as many as fit in IRAM - memory its
 * byte-addressable heap does not have - and the rest wherever the heap has a
 * piece.  The ESP32 reads and writes IRAM only a whole word at a time, so every
 * VRAM access goes through an aligned word; the bytes sit in it exactly as
 * they would in ordinary memory.  No 4-byte access crosses a page: patterns
 * are 32-byte aligned, sprite entries 8.
 */
#include "md_pages.h"
/* volatile: GCC otherwise turns "a word, then one byte of it" back into a
 * byte load or store - l8ui into IRAM, a LoadStoreError on the first frame. */
#if defined(MD_PSRAM_PAGES) && !defined(MD_FLAT_VRAM)
#define MD_FLAT_VRAM 1
#endif
#ifdef MD_FLAT_VRAM
/* A board with PSRAM: VRAM is one block in memory that takes byte accesses
 * (md_small_alloc), so neither the page table nor volatile. */
extern unsigned char *VRAM;
#define VRAM_W(A)   (((unsigned int *)(void *)VRAM)[((A) & VRAM_MASK) >> 2])
#else
#define VRAM_W(A)   (((volatile unsigned int *)(void *)md_vram[((A) & VRAM_MASK) >> MD_PAGE_SHIFT]) \
                     [((A) & MD_PAGE_MASK) >> 2])
#endif
#define VRAM_RD8(A) ((unsigned char)(VRAM_W(A) >> (((A) & 3u) << 3)))
#define VRAM_WR8(A, V) do { unsigned int a_ = (A), s_ = (a_ & 3u) << 3;      \
    VRAM_W(a_) = (VRAM_W(a_) & ~(0xFFu << s_)) |                              \
                 ((unsigned int)(unsigned char)(V) << s_); } while (0)
#else
#define VRAM_W(A)   (((unsigned int *)(void *)VRAM)[((A) & VRAM_MASK) >> 2])
#define VRAM_RD8(A) (VRAM[(A) & VRAM_MASK])
#define VRAM_WR8(A, V) (VRAM[(A) & VRAM_MASK] = (V))
#endif
#define CRAM_MAX_SIZE 0x40       // CRAM maximum size
#define VSRAM_MAX_SIZE 0x40      // VSRAM maximum size
#ifdef ARGON_MD_SMALL
/* the CYD: 80 sprites of 8 bytes are all the table holds - the rest of its
 * 1 KB in H40 is never read, and the 384 bytes were the glass's */
#define SAT_CACHE_MAX_SIZE 0x280
#else
#define SAT_CACHE_MAX_SIZE 0x400 // SAT CACHE maximum size
#endif
#define REG_SIZE 0x20            // REGISTERS total
#define FIFO_SIZE 0x4            // FIFO maximum size

#define COLOR_3B_TO_8B(c)  (((c) << 5) | ((c) << 2) | ((c) >> 1))
#define CRAM_R(c)          COLOR_3B_TO_8B(BITS((c), 1, 3))
#define CRAM_G(c)          COLOR_3B_TO_8B(BITS((c), 5, 3))
#define CRAM_B(c)          COLOR_3B_TO_8B(BITS((c), 9, 3))

#define MODE_SHI           BITS(gwenesis_vdp_regs[12], 3, 1)

#define SHADOW_COLOR(r,g,b) \
    do { r >>= 1; g >>= 1; b >>= 1; } while (0)
#define HIGHLIGHT_COLOR(r,g,b) \
    do { SHADOW_COLOR(r,g,b); r |= 0x80; g |= 0x80; b |= 0x80; } while(0)

// While we draw the planes, we use bit 0x80 on each pixel to save the
// high-priority flag, so that we can later prioritize.
#define PIXATTR_HIPRI        0x80
#define PIXATTR_LOWPRI       0x00
#define PIXATTR_SPRITE       0x40
#define PIXATTR_SPRITE_HIPRI 0xC0


// After mixing code, we use free bits 0x80 and 0x40 to indicate the
// shadow/highlight effect to apply on each pixel. Notice that we use
// 0x80 to indicate normal drawing and 0x00 to indicate shadowing,
// which does match exactly the semantic of PIXATTR_HIPRI. This simplifies
// mixing code quite a bit.
#define SHI_NORMAL(x)        ((x) | 0x80)
#define SHI_HIGHLIGHT(x)     ((x) | 0x40)
#define SHI_SHADOW(x)        ((x) & 0x3F)

#define SHI_IS_SHADOW(x)     (!((x) & 0x80))
#define SHI_IS_HIGHLIGHT(x)  ((x) & 0x40)

void gwenesis_vdp_reset();
void gwenesis_vdp_set_hblank();
void gwenesis_vdp_clear_hblank();
void gwenesis_vdp_set_vblank();
void gwenesis_vdp_clear_vblank();

unsigned int gwenesis_vdp_get_reg(int reg);
void gwenesis_vdp_set_reg(int reg, unsigned char value);

unsigned int gwenesis_vdp_read_memory_8(unsigned int address);
unsigned int gwenesis_vdp_read_memory_16(unsigned int address);

void gwenesis_vdp_write_memory_8(unsigned int address, unsigned int value);
void gwenesis_vdp_write_memory_16(unsigned int address, unsigned int value);

void gwenesis_vdp_set_buffers(unsigned char *screen_buffer, unsigned char *scaled_buffer);
void gwenesis_vdp_set_buffer(unsigned short *ptr_screen_buffer);
void gwenesis_vdp_render_line(int line);

void gwenesis_vdp_render_config();

unsigned int gwenesis_vdp_get_status();
void gwenesis_vdp_get_debug_status(char *s);
unsigned short gwenesis_vdp_get_cram(int index);
void gwenesis_vdp_get_vram(unsigned char *raw_buffer, int palette);
void gwenesis_vdp_get_vram_raw(unsigned char *raw_buffer);
void gwenesis_vdp_get_cram_raw(unsigned char *raw_buffer);

int gwenesis_vdp_vcounter();
int gwenesis_vdp_hcounter();
unsigned short gwenesis_vdp_hvcounter();

void gwenesis_vdp_gfx_save_state();
void gwenesis_vdp_gfx_load_state();
void gwenesis_vdp_mem_save_state();
void gwenesis_vdp_mem_load_state();

#endif