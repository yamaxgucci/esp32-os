/* See art_store.h for what this is and how a location's art gets into it. */
#include "art_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <argon/abi.h>
#include <argon/argon.h>

extern "C" uint32_t ag_us(void);

namespace {

/* ---------------------------------------------------------------------- */
/* The shape of the thing                                                  */
/* ---------------------------------------------------------------------- */
/*
 * The store holds the art of ONE location, and it is emptied every time a
 * location is entered - which is also what loading a save does.  That is
 * Maxim's design, and it is simpler than what came before it in every way:
 *
 *   - there is no index on flash and no header, because nothing has to
 *     survive a restart: whatever was there belongs to a location we have
 *     left.  The index lives in RAM, where appending to it is free.
 *   - nothing is read out of the map's files to fill it.  The game fills it
 *     by playing: art that the cache in RAM throws out is written here on the
 *     way out, so what ends up in flash is exactly what the location used.
 *   - flash is erased once, at the start, in a single stretch.  After that
 *     every write goes into ground that is already clear, which is the
 *     difference between tens of milliseconds and a few.
 *
 * Why writing has to be arranged so carefully: flash cannot be written over,
 * only erased in whole sectors and then programmed - and while either runs,
 * the cache that serves the processor its code and data from that same chip
 * is switched off.  Everything stops.  So the erasing happens where the game
 * is already waiting, and during play there is nothing left to do but
 * program bytes into ground that is ready for them.
 */

/* The index: one entry per stored picture, kept sorted by key so a lookup is
 * a binary search.  Two thousand is more than a town has asked for. */
const uint32_t kMaxEntries = 2048u;

struct Entry {
    int32_t fid;
    uint32_t off;
    uint32_t len;
};

Entry s_index[kMaxEntries];
uint32_t s_count;

ag_store_t s_store;
uint32_t s_bytes;          /* how big the partition is                     */

/*
 * How much of it we are willing to fill (-flashcap N, in kilobytes).
 *
 * Filling the whole partition is not free: everything put in has to be
 * erased first and written after, and the board is stopped with its cache off
 * for the whole of both.  Measured on a single map: filling 7733 KB cost 63
 * seconds of writing and 21 of erasing, and one thirty-second window of play
 * spent 24 of its seconds simply stopped.  Filling 5672 KB cost 42 and 16.
 * The art that matters is a small part of what fits, so there is a ceiling,
 * and it is a setting because it belongs to the map, not to the chip.
 */
uint32_t s_cap_bytes;      /* 0 = fill the whole partition */
uint32_t s_fill_at;        /* where the next write goes                    */
uint32_t s_erased_to;      /* everything below this is clear and writable  */

/*
 * The staging buffer: art waits here until there is enough of it to be worth
 * a trip to the flash.
 *
 * Writing stops the processor's cache while it runs, so the choice is between
 * many short stops and few long ones, and which is better is a question about
 * this board rather than about programming.  The size is a setting
 * (-flashbatch N, in kilobytes) so it can be measured instead of argued.
 */
uint8_t *s_stage;
uint32_t s_stage_cap = 300u * 1024u;   /* Maxim asked for 300 KB */
uint32_t s_stage_len;
Entry s_stage_index[256];
uint32_t s_stage_n;

/* The mapping over what has been written, so a stored picture can be handed
 * out where it lies instead of copied.  Dropped before a write and taken
 * again after: bytes under a live mapping must not change beneath it. */
ag_store_map_t s_map;
const uint8_t *s_base;
uint32_t s_map_from, s_map_to;

bool s_full;
bool s_filling;
int s_mode = 1;

/*
 * Whether art may be written while the game is being played.
 *
 * Measured on the board: writing costs about seven milliseconds per kilobyte,
 * and clearing ground first does not change it - the cost is the programming
 * itself, not the erasing.  So moving three megabytes of art into flash is
 * twenty-one seconds of a game that does not move, however it is divided: in
 * sixty-four kilobyte batches that is fifty-four freezes of four hundred
 * milliseconds each, which is what "it lags even when I stand still" was.
 *
 * With this off, art is only written while a map is loading - where the game
 * is already waiting and the same seconds are not felt.  Left as a setting
 * because the number above belongs to this flash chip, and the next board may
 * have a different one.
 */
bool s_write_in_play = true;

/*
 * Erasing, by the megabyte, as the writing gets near the end of what is clear.
 *
 * The whole partition used to be erased on entering a location: 8192 KB in
 * about nine seconds, of which a town actually used four and a half.  Half of
 * that time and half of that wear bought nothing.  So the ground is cleared a
 * megabyte at a time, and only while a map is loading - erasing during play
 * would be a second of frozen game, which is exactly what this whole thing
 * exists to avoid.  Running out during play simply stops the store taking
 * more until the next load.
 */
const uint32_t kEraseChunk = 1024u * 1024u;

/*
 * And during play, in much smaller pieces.
 *
 * Erasing measures 2.3 milliseconds a kilobyte on this board, so a megabyte
 * is over two seconds - far too long to take in one bite while somebody is
 * walking.  A hundred and twenty-eight kilobytes is about three hundred
 * milliseconds, which is no worse than the stalls the game already has, and
 * it happens a handful of times rather than constantly.
 */
const uint32_t kEraseChunkPlay = 128u * 1024u;

/* How much freshly written art may sit outside the mapping before it is
 * extended.  Until then those pictures are copied rather than pointed at. */
const uint32_t kMapGrow = 256u * 1024u;

/* The most internal memory the staging buffer may take.  See init. */
#define AG_STAGE_INTERNAL_MAX (32u * 1024u)

/* Which object types may be handed out in place; see ag_art_store_in_place. */
unsigned s_in_place_kinds = 0x1Fu;

/* Counters for the report. */
uint32_t s_hits, s_misses, s_in_place, s_copied, s_by_spi;
uint32_t s_bytes_read, s_read_us;
uint32_t s_bytes_written, s_write_us, s_write_batches;
uint32_t s_erase_us, s_erase_kb, s_clears;

/*
 * The same two costs, but per window, so a report answers "how much of these
 * thirty seconds was the board simply stopped?".  The totals above cannot:
 * they are dominated by the map load and hide what play actually pays.
 */
uint32_t s_win_write_us, s_win_erase_us, s_win_write_kb, s_win_erase_kb;
uint32_t s_win_batches;

/*
 * Kilobytes by kind, not just counts.
 *
 * "Four and a half megabytes held everything this town needs" and "the store
 * filled eight" are both true, and the difference is the whole question: what
 * else went in.  Counting pictures cannot answer it - a critter frame and a
 * tile differ by a factor of thirty - so the bytes are counted by kind.
 */
uint32_t s_kb_load_kind[16], s_kb_play_kind[16];

/*
 * Which of the stored files the game ever came back for.
 *
 * "Four and a half megabytes held everything this town needs" against a store
 * that fills eight: the difference is either art the town does need and the
 * smaller store was refusing, or art nobody ever asks for twice.  One bit per
 * index slot answers it, and the bits cost 256 bytes.
 */
uint8_t  s_touched[(2048 + 7) / 8];
uint32_t s_touched_n, s_touched_kb;

/*
 * Art that left RAM and did NOT reach flash, counted by why.
 *
 * Maxim asked for this as an independent check: in a store that is big enough
 * and working correctly, "lost" is zero and everything the cache gives up is
 * kept.  Anything else is a number that means something specific - no room
 * left, too big to stage, or the store is not taking writes at all - and each
 * of those points somewhere different.
 */
uint32_t s_lost_full, s_lost_big, s_lost_closed, s_lost_dup;

/*
 * Kept at a map load and kept while playing, counted apart.
 *
 * They are different things and only the second one is what Maxim means by
 * "moved to flash": writing during a load costs nothing anyone feels, while
 * writing during play stops the game for as long as it takes.  One number for
 * both let a report claim there had been moves in a run where writing during
 * play was switched off.
 */
uint32_t s_kept_load, s_kept_play;

/*
 * The staging buffer lives in the arena and is large; the bounce buffer is
 * internal and small.
 *
 * Both facts matter and they pull in opposite directions.  A flash driver can
 * only program from internal memory, so a write straight out of the arena is
 * copied through the driver's own tiny buffer a few hundred bytes at a time -
 * seven milliseconds a kilobyte instead of three and a half.  But internal
 * memory is two hundred kilobytes for the entire machine, and taking three
 * hundred of it for staging starved the drivers and the screen and made
 * everything crawl.
 *
 * So: gather in the arena, where there is room for three hundred kilobytes,
 * and hand it to the flash thirty-two at a time through a piece of internal
 * memory small enough that nobody misses it.  Full speed, one flush per three
 * hundred kilobytes rather than per thirty-two, and the mapping is rebuilt a
 * tenth as often - which is the part that was quietly costing the game.
 */
uint8_t *s_bounce;
uint32_t s_bounce_bytes;
bool s_want_bounce = true;
bool s_stage_internal;

/* What kind of art gets written while the game is being played, and what at
 * a load - Maxim's question: if a picture is written during play, why was it
 * not written when the map loaded?  The answer is that it had not been asked
 * for yet, and this says which kinds those are. */
uint32_t s_kept_play_kind[16];
uint32_t s_kept_load_kind[16];

/* What the last map load cost, and how much of it was this store's doing.
 * Maxim's question: maps were slow long before any of this existed, so the
 * load has to say how much of itself is flash and how much is everything
 * else. */
uint32_t s_load_t0, s_load_ms, s_load_write_ms, s_load_erase_ms;

const ag_store_api_t *store_api()
{
    const ag_api_t *api = ag_api();
    return (api != nullptr) ? api->store : nullptr;
}

/* ---------------------------------------------------------------------- */
/* The index                                                               */
/* ---------------------------------------------------------------------- */

const Entry *find(int fid)
{
    uint32_t lo = 0;
    uint32_t hi = s_count;

    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;

        if (s_index[mid].fid == (int32_t)fid) {
            return &s_index[mid];
        }
        if (s_index[mid].fid < (int32_t)fid) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return nullptr;
}

void index_insert(int fid, uint32_t off, uint32_t len)
{
    if (s_count >= kMaxEntries) {
        s_full = true;
        return;
    }

    uint32_t at = 0;

    while (at < s_count && s_index[at].fid < (int32_t)fid) {
        at++;
    }
    if (at < s_count && s_index[at].fid == (int32_t)fid) {
        return; /* already known */
    }

    memmove(&s_index[at + 1u], &s_index[at],
            (size_t)(s_count - at) * sizeof(Entry));
    s_index[at].fid = (int32_t)fid;
    s_index[at].off = off;
    s_index[at].len = len;
    s_count++;
}

/* ---------------------------------------------------------------------- */
/* The mapping                                                             */
/* ---------------------------------------------------------------------- */

void unmap_data()
{
    if (s_map != 0) {
        store_api()->unmap(s_map);
        s_map = 0;
    }
    s_base = nullptr;
    s_map_from = 0;
    s_map_to = 0;
}

/*
 * Map what has been written so far.
 *
 * Verified against a partition read, because a mapping is granted in MMU
 * pages and a pointer off by a page offset would hand back art that decodes
 * to noise rather than an error.  Sixty-four bytes settle it.
 */
void map_data()
{
    const ag_store_api_t *api = store_api();

    unmap_data();
    if (s_store == nullptr || s_fill_at == 0u) {
        return;
    }

    uint32_t want = s_fill_at;

    while (want >= (1u << 16)) {
        const void *addr = nullptr;

        if (api->map(s_store, 0, want, &addr, &s_map) == AG_OK) {
            uint8_t probe[64];

            if (api->read(s_store, 0, probe, sizeof(probe)) == AG_OK
                    && memcmp(probe, addr, sizeof(probe)) == 0) {
                s_base = (const uint8_t *)addr;
                s_map_from = 0;
                s_map_to = want;
                return;
            }
            api->unmap(s_map);
            s_map = 0;
            return;
        }
        want /= 2u;
    }
}

/* ---------------------------------------------------------------------- */
/* Writing                                                                 */
/* ---------------------------------------------------------------------- */

/* Clear another megabyte, if there is any left to clear.  False when the
 * partition has been used up. */
bool erase_more()
{
    const ag_store_api_t *api = store_api();

    if (s_erased_to >= s_bytes) {
        return false;
    }

    uint32_t want = s_bytes - s_erased_to;
    const uint32_t bite = s_filling ? kEraseChunk : kEraseChunkPlay;

    if (want > bite) {
        want = bite;
    }

    const uint32_t t0 = ag_us();
    const bool ok = api->erase(s_store, s_erased_to, want) == AG_OK;

    {
        const uint32_t spent = ag_us() - t0;

        s_erase_us += spent;
        s_win_erase_us += spent;
    }
    if (!ok) {
        return false;
    }
    s_erased_to += want;
    s_erase_kb += want >> 10;
    return true;
}

/*
 * Program bytes from the arena into flash, through the internal bounce buffer.
 * The mapping must already be down.
 */
bool write_through(uint32_t at, const uint8_t *src, uint32_t len)
{
    const ag_store_api_t *api = store_api();
    uint32_t done = 0;

    while (done < len) {
        uint32_t piece = len - done;

        if (s_bounce != nullptr) {
            if (piece > s_bounce_bytes) {
                piece = s_bounce_bytes;
            }
            memcpy(s_bounce, src + done, piece);
        }

        const uint32_t t0 = ag_us();
        const bool ok = api->write(s_store, at + done,
                                   (s_bounce != nullptr) ? s_bounce
                                                         : (src + done),
                                   piece) == AG_OK;

        {
            const uint32_t spent = ag_us() - t0;

            s_write_us += spent;
            s_win_write_us += spent;
        }
        if (!ok) {
            return false;
        }
        done += piece;
    }
    return true;
}

/* Push the staged art into flash in one go, then take the mapping again. */
void stage_flush()
{
    if (s_store == nullptr || s_stage_len == 0u) {
        s_stage_len = 0;
        s_stage_n = 0;
        return;
    }

    const ag_store_api_t *api = store_api();
    const uint32_t base = s_fill_at;

    unmap_data();

    const bool ok = write_through(base, s_stage, s_stage_len);

    s_write_batches++;
    (void)api;

    if (ok) {
        s_bytes_written += s_stage_len;
        s_fill_at += s_stage_len;
        for (uint32_t i = 0; i < s_stage_n; i++) {
            index_insert(s_stage_index[i].fid, base + s_stage_index[i].off,
                         s_stage_index[i].len);
        }
    }

    s_stage_len = 0;
    s_stage_n = 0;
    memset(s_touched, 0, sizeof(s_touched));
    s_touched_n = 0;
    s_touched_kb = 0;
    map_data();
}

bool in_place_allowed(int fid)
{
    const unsigned kind = ((unsigned)fid >> 24) & 0x0Fu;

    return (s_in_place_kinds & (1u << kind)) != 0u;
}

} // namespace

/* ---------------------------------------------------------------------- */

void ag_art_store_mode(int mode)
{
    s_mode = mode;
}

void ag_art_store_in_place(unsigned kinds)
{
    s_in_place_kinds = kinds;
}

/*
 * Whether writes are relayed through a piece of internal memory
 * (-flashstage 32) or handed to the driver straight from the arena
 * (-flashstage 0).
 *
 * A flash driver can only program from internal memory.  Given a source in
 * the arena it relays the bytes through its own buffer, a few hundred at a
 * time; ours is thirty-two kilobytes, so the trips are far fewer.  Measured
 * at 6.9 against 3.5 milliseconds a kilobyte - but the report is not the
 * judge, the game is, so both ways stay reachable.
 */
/* How much of the store partition may be filled, in kilobytes; 0 = all. */
void ag_art_store_cap_kb(unsigned kb)
{
    s_cap_bytes = kb * 1024u;
}

void ag_art_store_stage_kb(unsigned kb)
{
    s_want_bounce = (kb != 0u);
}

void ag_art_store_batch_kb(unsigned kb)
{
    if (kb >= 4u && kb <= 512u) {
        s_stage_cap = kb * 1024u;
    }
}

void ag_art_store_init(void)
{
    const ag_store_api_t *api = store_api();

    s_store = nullptr;
    s_count = 0;
    s_fill_at = 0;
    s_erased_to = 0;
    s_stage_len = 0;
    s_stage_n = 0;

    if (s_mode == 0 || api == nullptr || api->open == nullptr) {
        return;
    }

    s_store = api->open("artfs");
    if (s_store == nullptr) {
        return;
    }

    s_bytes = (uint32_t)api->bytes(s_store);
    if (s_cap_bytes != 0u && s_bytes > s_cap_bytes) {
        s_bytes = s_cap_bytes;
    }

    /*
     * The staging buffer wants to be in INTERNAL memory, and this is not a
     * preference - it decides how fast writing is.
     *
     * A flash driver can only program from memory the processor can reach
     * without going through the flash cache itself, so a source in PSRAM is
     * copied through a small internal bounce buffer, a few hundred bytes at a
     * time, disabling the cache for each hop.  One three-hundred-kilobyte
     * write becomes hundreds of little ones.  Measured here at seven
     * milliseconds per kilobyte, about four times what this class of chip
     * programs at, which is exactly the shape of that.
     *
     * Internal memory is scarce - a couple of hundred kilobytes for the whole
     * system - so this asks for a modest piece of it and falls back to the
     * arena if it cannot have one.  Writing still works from the arena; it is
     * just slow, and the report says which one it got.
     */
    const ag_api_t *root = ag_api();

    if (root != nullptr && root->mem != nullptr
            && root->mem->alloc_caps != nullptr) {
        if (s_want_bounce) {
            /*
             * Take what internal memory will give, and say so either way.
             *
             * Writing to flash from the arena costs 7.4 ms a kilobyte and
             * from internal memory 3.5 - thirty seconds of a map load.  The
             * ask for 32 KB was failing silently and the store quietly wrote
             * the slow way for a whole day; a refusal that says nothing is
             * how that happens.  So: ask for less rather than give up, and
             * print what was got.
             */
            for (uint32_t want = AG_STAGE_INTERNAL_MAX;
                    want >= 4096u && s_bounce == nullptr; want >>= 1) {
                s_bounce = (uint8_t *)root->mem->alloc_caps(want, AG_MEM_FAST);
                if (s_bounce != nullptr) {
                    s_bounce_bytes = want;
                }
            }
            ag_printf("fallout: flash relay %u KB of internal memory\n",
                      (unsigned)(s_bounce_bytes >> 10));
        }
        s_stage_internal = (s_bounce != nullptr);
    }

    s_stage = (uint8_t *)malloc(s_stage_cap);

    if (false) {
        /*
         * A SMALL piece, and the smallness is the point.
         *
         * Internal memory is about two hundred kilobytes for the whole
         * machine - the drivers, the screen and the sound all live off it.
         * Asking for three hundred here took it from them and the board
         * slowed to a crawl, which is a far worse trade than a slow write.
         * Thirty-two kilobytes is plenty: the speed comes from the buffer
         * being internal at all, not from it being large, so this only means
         * more batches, each of them quick.
         */
    }

    if (s_stage == nullptr) {
        /* Without somewhere to gather art there is nothing to write, and a
         * store that cannot be written is not a store. */
        s_store = nullptr;
    }
}

bool ag_art_store_present(void)
{
    return s_store != nullptr;
}

int ag_art_store_size(int fid)
{
    const Entry *e = find(fid);

    return (e != nullptr) ? (int)e->len : -1;
}

const unsigned char *ag_art_store_ptr(int fid, int *size)
{
    /* ARGON: mark this file as one the game came back for.  Doing it in
     * find() counted the duplicate checks that happen while writing, so
     * it only belongs on the read path. */
    {
        const Entry *e = find(fid);

        if (e != nullptr) {
            const size_t   slot = (size_t)(e - s_index);
            const unsigned bit = 1u << (slot & 7u);

            if ((s_touched[slot >> 3] & bit) == 0u) {
                s_touched[slot >> 3] |= (uint8_t)bit;
                s_touched_n++;
                s_touched_kb += e->len;
            }
        }
    }
    /*
     * Interface art, heads, inventory and skilldex are NOT handed out where
     * they lie, and that is not caution - it is measured.  The engine draws
     * INTO those pictures: fontDrawText writes the inventory's numbers onto
     * the button art, the interface writes its lights, the world map writes
     * its tab labels, and the action menu is composed into the cursor art and
     * read back from it.  Flash is read-only to the processor and a write
     * there stops the whole board, so those kinds live in RAM.  See the note
     * in game_mouse.cc, which cost a broken context menu to learn.
     */
    if (!in_place_allowed(fid)) {
        return nullptr;
    }

    const Entry *e = find(fid);

    if (e == nullptr || s_base == nullptr) {
        return nullptr;
    }
    if (e->off < s_map_from || e->off + e->len > s_map_to) {
        return nullptr;
    }

    if (size != nullptr) {
        *size = (int)e->len;
    }
    s_hits++;
    s_in_place++;
    return s_base + (e->off - s_map_from);
}

bool ag_art_store_read(int fid, int size, unsigned char *data)
{
    const Entry *e = find(fid);

    if (e == nullptr || data == nullptr || size <= 0
            || (uint32_t)size != e->len) {
        s_misses++;
        return false;
    }

    const uint32_t t0 = ag_us();
    bool ok;

    if (s_base != nullptr && e->off >= s_map_from
            && e->off + e->len <= s_map_to) {
        memcpy(data, s_base + (e->off - s_map_from), e->len);
        s_copied++;
        ok = true;
    } else {
        ok = store_api()->read(s_store, e->off, data, e->len) == AG_OK;
        s_by_spi++;
    }
    s_read_us += ag_us() - t0;

    if (ok) {
        s_hits++;
        s_bytes_read += e->len;
    } else {
        s_misses++;
    }
    return ok;
}

/*
 * Art on its way out of the cache in RAM.
 *
 * This is the only way anything gets into the store: the game decides what is
 * worth keeping by using it, the cache decides what to let go of, and what it
 * lets go of lands here.  Nothing is read from the map's files to fill this -
 * what is in flash is what was actually needed, in the order it was needed.
 */
void ag_art_store_put(int fid, const unsigned char *data, int size)
{
    const ag_art_put_t one = { fid, data, size };

    ag_art_store_put_many(&one, 1);
}

/*
 * A whole eviction, written in one approach to the chip.
 *
 * The cache in RAM does not run short of one picture: when it needs room it
 * gives up a batch of them, and that batch belongs in flash as a single
 * visit.  Writing switches off the cache that feeds the processor from the
 * same chip, so the board stops for the length of the visit - and the length
 * hardly depends on how many bytes go: it is the visit that costs.  Fifty
 * pictures written one by one are fifty freezes scattered through play; the
 * same fifty written together are one, at a moment the cache has chosen.
 *
 * The erase for the whole batch is done first, for the same reason.
 */
void ag_art_store_put_many(const ag_art_put_t *items, int count)
{
    if (s_store == nullptr || items == nullptr || count <= 0) {
        s_lost_closed += (count > 0) ? (uint32_t)count : 0u;
        return;
    }
    if (!s_filling && !s_write_in_play) {
        s_lost_closed += (uint32_t)count;
        return;
    }
    if (s_full) {
        s_lost_full += (uint32_t)count;
        return;
    }

    /* What is genuinely new, so the erase below is asked for the right size. */
    uint32_t total = 0;

    for (int i = 0; i < count; i++) {
        if (items[i].data == nullptr || items[i].size <= 0) {
            s_lost_closed++;
            continue;
        }
        if (find(items[i].fid) != nullptr) {
            s_lost_dup++;
            continue;
        }
        total += (uint32_t)items[i].size;
    }

    if (total == 0u) {
        return;
    }

    for (int i = 0; i < count; i++) {
        const int fid = items[i].fid;
        const int size = items[i].size;

        if (items[i].data == nullptr || size <= 0) {
            continue;
        }
        if (find(fid) != nullptr) {
            continue;
        }

        /*
         * Into the gathering buffer, and out to flash only when it is full.
         *
         * This is the shape that played well and that I removed: what makes a
         * write expensive is the visit to the chip, not the bytes, because
         * the data cache is off for its whole length.  Evictions arrive one
         * picture at a time whatever the cache is asked to free, so the
         * batching has to live here, where they land - three hundred
         * kilobytes of them leave together, in one visit.
         */
        if (s_stage == nullptr) {
            /* No buffer: the old way, one visit per picture. */
            while (s_fill_at + (uint32_t)size > s_erased_to) {
                if (!erase_more()) {
                    s_full = true;
                    s_lost_full++;
                    return;
                }
            }

            const uint32_t base = s_fill_at;

            if (!write_through(base, items[i].data, (uint32_t)size)) {
                return;
            }
            s_write_batches++;
            s_bytes_written += (uint32_t)size;
            s_fill_at += (uint32_t)size;
            index_insert(fid, base, (uint32_t)size);
        } else {
            if ((uint32_t)size > s_stage_cap) {
                s_lost_big++;
                continue;
            }
            if (s_stage_len + (uint32_t)size > s_stage_cap
                    || s_stage_n >= 256u) {
                stage_flush();
            }

            /* Room has to exist in flash for what is being gathered. */
            while (s_fill_at + s_stage_len + (uint32_t)size > s_erased_to) {
                if (!erase_more()) {
                    s_full = true;
                    s_lost_full++;
                    stage_flush();
                    return;
                }
            }

            memcpy(s_stage + s_stage_len, items[i].data, (size_t)size);
            s_stage_index[s_stage_n].fid = fid;
            s_stage_index[s_stage_n].off = s_stage_len;
            s_stage_index[s_stage_n].len = (uint32_t)size;
            s_stage_n++;
            s_stage_len += (uint32_t)size;
        }

        {
            const unsigned kind = ((unsigned)fid >> 24) & 0x0Fu;

            if (s_filling) {
                s_kept_load++;
                s_kept_load_kind[kind]++;
                s_kb_load_kind[kind] += (uint32_t)size;
            } else {
                s_kept_play++;
                s_kept_play_kind[kind]++;
                s_kb_play_kind[kind] += (uint32_t)size;
            }
        }
    }

    /* Extend the mapping over what has actually been written. */
    if (s_fill_at - s_map_to >= kMapGrow) {
        map_data();
    }
}

unsigned s_when_set;

void ag_art_store_write_in_play(int on)
{
    s_write_in_play = (on != 0);
    s_when_set++;
}

void ag_art_store_fill_begin(void)
{
    if (s_store == nullptr) {
        return;
    }
    s_filling = true;

    /* A location is being entered, so everything here belongs to the one
     * being left.  Empty it, and clear the ground for what comes next. */
    s_load_t0 = ag_us();
    s_load_write_ms = s_write_us / 1000u;
    s_load_erase_ms = s_erase_us / 1000u;
    ag_art_store_clear();
}

void ag_art_store_fill_end(void)
{
    /* Whatever is still gathered goes out here, inside the wait. */
    stage_flush();

    /*
     * Clear a megabyte of reserve before the game starts moving.
     *
     * Erasing costs 2.3 milliseconds a kilobyte, so during play even a small
     * bite is a third of a second of nothing happening.  Here, where the map
     * is loading and the wait is already being paid, a megabyte is two
     * seconds nobody separates from the rest of the load - and it is enough
     * for everything a location asks for after it has started.
     */
    while (s_erased_to < s_fill_at + kEraseChunk && erase_more()) {
        /* keep going until there is a megabyte of clear ground ahead */
    }

    s_filling = false;

    if (s_load_t0 != 0u) {
        s_load_ms = (ag_us() - s_load_t0) / 1000u;
        s_load_write_ms = (s_write_us / 1000u) - s_load_write_ms;
        s_load_erase_ms = (s_erase_us / 1000u) - s_load_erase_ms;
        s_load_t0 = 0;
    }
}

void ag_art_store_clear(void)
{
    const ag_store_api_t *api = store_api();

    if (s_store == nullptr) {
        return;
    }

    /* Nothing gathered survives a clear - it would be written past a store
     * that no longer holds what the index describes. */
    s_stage_len = 0;
    s_stage_n = 0;

    unmap_data();
    s_count = 0;
    s_fill_at = 0;
    s_stage_len = 0;
    s_stage_n = 0;
    s_full = false;
    s_clears++;

    /*
     * Clear the ground for this location in one stretch, here, where the game
     * is already waiting for a map to load.
     *
     * The whole partition, rather than a guessed portion: a guess that is too
     * small stops the store taking art halfway through a town, and a guess
     * that is too large costs nothing extra because erasing is charged by
     * what it actually erases.  How long it takes is measured and printed -
     * see the [artflash] line - so the decision can be revisited with a
     * number rather than an opinion.
     */
    s_erased_to = 0;
    (void)api;

    /* One megabyte to start with; more is cleared as the location needs it. */
    (void)erase_more();
}

void ag_art_store_want(int fid)
{
    (void)fid;
}

unsigned ag_art_store_wanted(int *out, unsigned cap)
{
    (void)out;
    (void)cap;
    return 0;
}

void ag_art_store_offer(int fid, const unsigned char *data, int size)
{
    /* During a load the rule is the same as during play: what the game read
     * is what gets kept. */
    ag_art_store_put(fid, data, size);
}

void ag_art_store_kinds(char *out, size_t cap)
{
    static const char *const name[16] = {
        "item", "critter", "scenery", "wall", "tile", "misc", "iface",
        "inven", "head", "backgrnd", "skilldex", "?", "?", "?", "?", "?"
    };

    if (out == nullptr || cap == 0u) {
        return;
    }

    /*
     * The report says which way the store is actually working, not which way
     * it was asked to.  Two switches in a row looked ignored, and each time
     * the argument had in fact reached the program - the evidence for that
     * belongs in the same line as the numbers it explains.
     */
    int pos = snprintf(out, cap,
                       "[artkept] staged from %s, writes in play %s (set %u times)"
                       " | in play:",
                       s_stage_internal ? "internal memory" : "the arena",
                       s_write_in_play ? "on" : "off",
                       (unsigned)s_when_set);

    for (unsigned k = 0; k < 16u && pos > 0 && (size_t)pos < cap; k++) {
        if (s_kept_play_kind[k] != 0u) {
            pos += snprintf(out + pos, cap - (size_t)pos, " %s %u/%uKB",
                            name[k], (unsigned)s_kept_play_kind[k],
                            (unsigned)(s_kb_play_kind[k] >> 10));
        }
    }
    if (pos > 0 && (size_t)pos < cap) {
        pos += snprintf(out + pos, cap - (size_t)pos, " | at load:");
    }
    for (unsigned k = 0; k < 16u && pos > 0 && (size_t)pos < cap; k++) {
        if (s_kept_load_kind[k] != 0u) {
            pos += snprintf(out + pos, cap - (size_t)pos, " %s %u/%uKB",
                            name[k], (unsigned)s_kept_load_kind[k],
                            (unsigned)(s_kb_load_kind[k] >> 10));
        }
    }
}

void ag_art_store_report(char *out, size_t cap)
{
    if (out == nullptr || cap == 0u) {
        return;
    }
    if (s_store == nullptr) {
        snprintf(out, cap, "[artflash] no store partition");
        return;
    }

    const uint32_t asked = s_hits + s_misses;
    const uint32_t pct = (asked != 0u) ? (s_hits * 100u / asked) : 0u;

    snprintf(out, cap,
             "[artflash] %u%% of %u asks | %u files, %u of %u KB%s | "
             "mapped %u KB, %u in place / %u copied / %u by SPI | "
             "read %u KB in %u ms | wrote %u KB in %u ms over %u batches | "
             "%u clears erased %u KB in %u ms | "
             "left RAM: %u kept while playing, %u kept at load, "
             "%u already here, %u no room, %u too big, %u not allowed | "
             "last map load %u ms, of it %u writing and %u erasing",
             (unsigned)pct, (unsigned)asked, (unsigned)s_count,
             (unsigned)(s_fill_at >> 10), (unsigned)(s_bytes >> 10),
             s_full ? " FULL" : "",
             (unsigned)((s_map_to - s_map_from) >> 10),
             (unsigned)s_in_place, (unsigned)s_copied, (unsigned)s_by_spi,
             (unsigned)(s_bytes_read >> 10), (unsigned)(s_read_us / 1000u),
             (unsigned)(s_bytes_written >> 10),
             (unsigned)(s_write_us / 1000u), (unsigned)s_write_batches,
             (unsigned)s_clears, (unsigned)s_erase_kb,
             (unsigned)(s_erase_us / 1000u),
             (unsigned)s_kept_play, (unsigned)s_kept_load,
             (unsigned)s_lost_dup, (unsigned)s_lost_full,
             (unsigned)s_lost_big, (unsigned)s_lost_closed,
             (unsigned)s_load_ms, (unsigned)s_load_write_ms,
             (unsigned)s_load_erase_ms);

    /*
     * And what this window alone paid: the part of the last thirty seconds in
     * which the board was stopped with its cache off, writing or erasing.
     */
    {
        const size_t at = strlen(out);

        if (at + 1u < cap) {
            snprintf(out + at, cap - at,
                     " | this window: stopped %u ms writing and %u ms erasing"
                     " | %u of %u stored files ever asked for (%u of %u KB)",
                     (unsigned)(s_win_write_us / 1000u),
                     (unsigned)(s_win_erase_us / 1000u),
                     (unsigned)s_touched_n, (unsigned)s_count,
                     (unsigned)(s_touched_kb >> 10),
                     (unsigned)(s_fill_at >> 10));
        }
        s_win_write_us = 0;
        s_win_erase_us = 0;
    }
}
