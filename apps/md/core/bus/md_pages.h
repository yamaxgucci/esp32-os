/*
 * The CYD's Mega Drive memory: 68000 RAM, Z80 RAM and VRAM in 1 KB pages.
 *
 * After the image is loaded the heap's free memory is in pieces, few of them
 * 4 KB, and every block taken from it is one of a process's 32 resources - so
 * md_main.c takes the largest blocks it is given and cuts pages out of them.
 * No access the cores make crosses a page: 68000 longs are two word accesses,
 * VRAM patterns are 32-byte aligned and sprite entries 8.
 */
#ifndef MD_PAGES_H
#define MD_PAGES_H

#define MD_PAGE_SHIFT 10
#define MD_PAGE       (1u << MD_PAGE_SHIFT)
#define MD_PAGE_MASK  (MD_PAGE - 1u)

#define MD_RAM_PAGES  (0x10000u >> MD_PAGE_SHIFT)
#define MD_ZRAM_PAGES (0x2000u >> MD_PAGE_SHIFT)
#define MD_VRAM_PAGES (0x10000u >> MD_PAGE_SHIFT)

extern unsigned char *md_ram[MD_RAM_PAGES];
extern unsigned char *md_zram[MD_ZRAM_PAGES];
extern unsigned char *md_vram[MD_VRAM_PAGES];

#endif
