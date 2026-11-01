/*
 * The CYD's Mega Drive memory: 68000 RAM, Z80 RAM and VRAM in 512-byte pages.
 *
 * After the image is loaded the heap's free memory is in pieces, many of them
 * under a kilobyte, and every block taken from it is one of a process's 32
 * resources - so md_main.c takes the largest blocks it is given and cuts
 * pages out of them.
 * No access the cores make crosses a page: 68000 longs are two word accesses,
 * VRAM patterns are 32-byte aligned and sprite entries 8.
 */
#ifndef MD_PAGES_H
#define MD_PAGES_H

#define MD_PAGE_SHIFT 9
#define MD_PAGE       (1u << MD_PAGE_SHIFT)
#define MD_PAGE_MASK  (MD_PAGE - 1u)

#define MD_RAM_PAGES  (0x10000u >> MD_PAGE_SHIFT)
#define MD_ZRAM_PAGES (0x2000u >> MD_PAGE_SHIFT)
#define MD_VRAM_PAGES (0x10000u >> MD_PAGE_SHIFT)

extern unsigned char *md_ram[MD_RAM_PAGES];
extern unsigned char *md_zram[MD_ZRAM_PAGES];
extern unsigned char *md_vram[MD_VRAM_PAGES];

#ifdef MD_CYCPROF
/* A profiling build only: where a 68000 instruction's time goes, in CPU
 * cycles - fetch, finding the handler, running it, and the memory accesses
 * inside it (part of "exec").  md_main.c prints them with `profile`. */
static inline unsigned int md_ccount(void)
{
    unsigned int c;
    __asm__ volatile("rsr %0, ccount" : "=a"(c));
    return c;
}
extern unsigned int md_cp_fetch, md_cp_decode, md_cp_exec, md_cp_n;
extern unsigned int md_cp_mem, md_cp_memn;
#endif

#endif
