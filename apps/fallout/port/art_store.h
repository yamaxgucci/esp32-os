/*
 * The art store: a few megabytes of flash holding game art already unpacked.
 *
 * A miss in the art cache costs a read from the SD card and an inflate -
 * measured at about 1.6 ms per kilobyte plus the decompression, and on a town
 * map that adds up to whole seconds of the walk.  The board has 16 MB of
 * flash and was only ever using half of it, so the other half becomes a place
 * to keep art in the shape the game wants it: no archive, no compression, no
 * filesystem, addressed by the cache's own key.
 *
 * What it is NOT: a second cache with its own policy.  It never evicts during
 * play and it never writes during play, because writing to flash erases
 * sectors, and an erase stops everything executing from flash for tens of
 * milliseconds - exactly the stutter this is meant to remove.  It fills while
 * the map is loading and the game is already standing still, and after that
 * it only answers questions.
 */
#ifndef AG_ART_STORE_H
#define AG_ART_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the store should do this run, set before init():
 *
 *   0  off entirely - no reads, no writes.  This is what an honest
 *      measurement needs: the same binary, the same map, one flag apart.
 *   1  on (the default).
 *   2  on, but empty it first - for measuring a cold store, and for getting
 *      out of a bad one.
 */
void ag_art_store_mode(int mode);

/*
 * Which kinds of art may be handed out in place, as a bit per object type:
 * 1 item, 2 critter, 4 scenery, 8 wall, 16 tile, and so on up.  The default is
 * 0x1F - the five that carry the cost and are only ever drawn from.
 *
 * It is a setting because in-place art is not free in every direction: the
 * bytes come out of flash through the processor cache, and a miss there costs
 * about three hundred cycles.  For art that is loaded once and drawn once that
 * is nothing against a read from the card; for a critter animation redrawn
 * every frame it might not be, and the only way to know is to turn one kind
 * off and measure.  Maxim noticed the brahmin moving jerkily on 11 September
 * 2026, which is what this exists to answer.
 */
void ag_art_store_in_place(unsigned kinds);

/*
 * How much art is gathered before it is written, in kilobytes (-flashbatch N).
 *
 * Writing to flash switches off the cache that feeds the processor from that
 * same chip, so the whole board pauses for as long as a write takes.  Many
 * small writes make many short pauses; one large write makes a single long
 * one.  Which is better is a question about this board, so it is a setting.
 */
void ag_art_store_batch_kb(unsigned kb);

/*
 * How much of the store partition may be filled (-flashcap N kilobytes, 0 for
 * all of it).  Everything put in must be erased first and written after, and
 * the board stops with its cache off for both, so the ceiling is what keeps
 * play from paying for art it will not look at.
 */
void ag_art_store_cap_kb(unsigned kb);

/* 0: write straight from the arena.  32: relay through internal memory. */
void ag_art_store_stage_kb(unsigned kb);

/*
 * Whether art may be written to flash while the game is being played
 * (-flashwhen 1), or only while a map is loading (-flashwhen 0, the default).
 * Writing costs about seven milliseconds a kilobyte on this board, so during
 * play it is felt as a freeze every time a batch goes out.
 */
void ag_art_store_write_in_play(int on);

/*
 * Art leaving the cache in RAM, offered to the store on its way out.  This is
 * the only way anything gets in: what the game used and could not keep.
 */
void ag_art_store_put(int fid, const unsigned char *data, int size);

/*
 * A whole eviction offered at once.
 *
 * The cache does not give up one picture when it runs short - it gives up
 * about three hundred kilobytes of them, and they should reach the flash as
 * one approach to the chip.  What costs time is the approach itself: the data
 * cache is off for its whole length, so everything running from flash stops.
 * Fifty separate writes are fifty separate freezes scattered through the
 * game; one is one, at a moment the cache has already chosen.
 */
typedef struct {
    int                  fid;
    const unsigned char *data;
    int                  size;
} ag_art_put_t;

void ag_art_store_put_many(const ag_art_put_t *items, int count);

/* Open the store and read its index.  Safe to call on a machine with no store
 * partition: everything below then answers "not here" and the game runs
 * exactly as it did before. */
void ag_art_store_init(void);

/* Is there a store at all?  For reports. */
bool ag_art_store_present(void);

/* The size of this art in the store, or -1 when it is not there. */
int ag_art_store_size(int fid);

/*
 * A pointer straight into the store, or null.
 *
 * This is the one that matters: the art stays in flash and the engine reads
 * it where it lies, so the cache entry costs the size of its own struct and
 * nothing more.  Nothing is copied, nothing needs evicting to make room, and
 * the three megabytes the art cache used to hold are free for something else.
 *
 * Null when the art is not in the store, or when it is outside the mapped
 * window - the caller then falls back to ag_art_store_read, which copies.
 *
 * The memory is READ ONLY.  It is mapped flash, so a write does not corrupt
 * anything quietly: the hardware refuses it and the program faults on the
 * spot, which is why this is safe to hand to an engine written in 1997
 * without auditing every line that touches a picture.
 */
const unsigned char *ag_art_store_ptr(int fid, int *size);

/* Copy this art out of the store.  False when it is not there, in which case
 * nothing was written to data. */
bool ag_art_store_read(int fid, int size, unsigned char *data);

/*
 * The waiting list: art that was read off the card at a moment the store was
 * not allowed to write.
 *
 * The store fills during a map load, and that covers the map - but not the
 * save panel, not a dialogue head, not a critter animation first seen in a
 * fight.  Those are read from the card every time, and they are what is left
 * of the cost: 53 interface misses for 441 ms, one 300 KB panel for 198 of
 * them.
 *
 * Remembering them and writing them at the NEXT load is the whole trick: the
 * price is paid where the game is already waiting, and the second time the
 * panel opens it comes out of flash.  ag_art_store_want records a key;
 * ag_art_store_wanted hands the list back and empties it.
 */
void ag_art_store_want(int fid);
unsigned ag_art_store_wanted(int *out, unsigned cap);

/* The filling window.  Between begin and end, ag_art_store_offer() may write;
 * outside it, offers are ignored.  end() commits the index. */
void ag_art_store_fill_begin(void);
void ag_art_store_offer(int fid, const unsigned char *data, int size);
void ag_art_store_fill_end(void);

/* Empty the store.  Only the header is erased - the data sectors are erased
 * lazily as they are reused, because erasing eight megabytes takes the best
 * part of a minute. */
void ag_art_store_clear(void);

/* One line for the diagnostics panel. */
void ag_art_store_report(char *out, size_t cap);

/* Which kinds of art were written, in play and at a load. */
void ag_art_store_kinds(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AG_ART_STORE_H */
