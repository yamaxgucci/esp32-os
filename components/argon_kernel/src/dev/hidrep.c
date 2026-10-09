/*
 * ArgonOS - a mouse's report layout, from its report descriptor.
 *
 * See argon/hidrep.h for why.  The descriptor is a list of short items (HID
 * 1.11, 6.2.2): a prefix byte holding the size, the type (main, global,
 * local) and the tag, then 0, 1, 2 or 4 bytes of data.  Globals - usage page,
 * logical minimum, report size, count and ID - hold until changed; locals -
 * the usages - apply to the next main item and are then forgotten.  Every
 * Input main item lays `count` fields of `size` bits end to end, one report ID
 * at a time, and that running offset is all a layout is.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/hidrep.h>

#include <string.h>

#define TYPE_MAIN   0u
#define TYPE_GLOBAL 1u
#define TYPE_LOCAL  2u

#define MAIN_INPUT  0x8u

#define GLOBAL_USAGE_PAGE 0x0u
#define GLOBAL_REPORT_SIZE 0x7u
#define GLOBAL_REPORT_ID  0x8u
#define GLOBAL_REPORT_COUNT 0x9u
#define GLOBAL_PUSH       0xAu
#define GLOBAL_POP        0xBu

#define LOCAL_USAGE     0x0u
#define LOCAL_USAGE_MIN 0x1u
#define LOCAL_USAGE_MAX 0x2u

#define PAGE_DESKTOP 0x01u
#define PAGE_BUTTON  0x09u
#define USAGE_X      0x30u
#define USAGE_Y      0x31u
#define USAGE_WHEEL  0x38u

#define MAX_USAGES 16
#define MAX_IDS    8
#define MAX_STACK  4

typedef struct {
    uint16_t page;
    uint32_t size;
    uint32_t count;
    uint8_t  id;
} globals_t;

/* The running bit offset of each report ID seen so far. */
typedef struct {
    uint8_t  id[MAX_IDS];
    uint32_t off[MAX_IDS];
    unsigned n;
} offsets_t;

static uint32_t *offset_of(offsets_t *o, uint8_t id)
{
    for (unsigned i = 0; i < o->n; i++) {
        if (o->id[i] == id) {
            return &o->off[i];
        }
    }
    if (o->n == MAX_IDS) {
        return NULL;
    }
    o->id[o->n] = id;
    o->off[o->n] = 0;
    return &o->off[o->n++];
}

/* The first button field of each report ID: the axes decide later which
 * report is the mouse's, and buttons come before them in every mouse. */
typedef struct {
    uint8_t  id[MAX_IDS];
    uint16_t off[MAX_IDS];
    uint8_t  count[MAX_IDS];
    unsigned n;
} buttons_t;

static void buttons_note(buttons_t *b, uint8_t id, uint32_t off, uint32_t count)
{
    for (unsigned i = 0; i < b->n; i++) {
        if (b->id[i] == id) {
            return;
        }
    }
    if (b->n == MAX_IDS) {
        return;
    }
    b->id[b->n] = id;
    b->off[b->n] = (uint16_t)off;
    b->count[b->n] = (uint8_t)(count > 8u ? 8u : count);
    b->n++;
}

bool ag_hidrep_parse_mouse(const uint8_t *desc, size_t len,
                           ag_hidrep_mouse_t *out)
{
    ag_hidrep_mouse_t m;
    memset(&m, 0, sizeof(m));
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (desc == NULL || out == NULL) {
        return false;
    }

    globals_t g;
    memset(&g, 0, sizeof(g));
    globals_t stack[MAX_STACK];
    unsigned  depth = 0;
    offsets_t offs;
    memset(&offs, 0, sizeof(offs));

    uint32_t usages[MAX_USAGES];
    unsigned n_usages = 0;
    uint32_t umin = 0, umax = 0;
    bool     have_range = false;
    bool     have_id = false; /* the mouse's report ID is settled */
    buttons_t btns;
    memset(&btns, 0, sizeof(btns));

    size_t i = 0;
    while (i < len) {
        const uint8_t prefix = desc[i++];
        if (prefix == 0xFEu) {
            /* a long item: its data size is the next byte, and nothing in it
             * is anything a mouse uses */
            if (i + 1 >= len) {
                break;
            }
            i += 2u + desc[i];
            continue;
        }
        const unsigned size = (prefix & 3u) == 3u ? 4u : (prefix & 3u);
        const unsigned type = (prefix >> 2) & 3u;
        const unsigned tag = prefix >> 4;
        if (i + size > len) {
            break;
        }
        uint32_t v = 0;
        for (unsigned k = 0; k < size; k++) {
            v |= (uint32_t)desc[i + k] << (8u * k);
        }
        i += size;

        if (type == TYPE_GLOBAL) {
            switch (tag) {
            case GLOBAL_USAGE_PAGE:   g.page = (uint16_t)v; break;
            case GLOBAL_REPORT_SIZE:  g.size = v; break;
            case GLOBAL_REPORT_COUNT: g.count = v; break;
            case GLOBAL_REPORT_ID:    g.id = (uint8_t)v; break;
            case GLOBAL_PUSH:
                if (depth < MAX_STACK) {
                    stack[depth++] = g;
                }
                break;
            case GLOBAL_POP:
                if (depth > 0) {
                    g = stack[--depth];
                }
                break;
            default: break;
            }
            continue;
        }

        if (type == TYPE_LOCAL) {
            /* A four-byte usage carries its own page in the top half. */
            const uint32_t full = (size == 4u) ? v : (((uint32_t)g.page << 16) | v);
            if (tag == LOCAL_USAGE && n_usages < MAX_USAGES) {
                usages[n_usages++] = full;
            } else if (tag == LOCAL_USAGE_MIN) {
                umin = full;
                have_range = true;
            } else if (tag == LOCAL_USAGE_MAX) {
                umax = full;
                have_range = true;
            }
            continue;
        }

        if (type != TYPE_MAIN) {
            continue;
        }

        if (tag == MAIN_INPUT) {
            uint32_t *off = offset_of(&offs, g.id);
            const bool constant = (v & 1u) != 0;
            for (uint32_t f = 0; off != NULL && f < g.count; f++) {
                uint32_t usage = 0;
                if (!constant) {
                    if (f < n_usages) {
                        usage = usages[f];
                    } else if (have_range && umin + f <= umax) {
                        usage = umin + f;
                    } else if (n_usages > 0) {
                        usage = usages[n_usages - 1];
                    }
                }
                const uint16_t page = (uint16_t)(usage >> 16);
                const uint16_t u = (uint16_t)(usage & 0xFFFFu);
                const uint32_t at = *off + f * g.size;

                if (usage != 0 && page == PAGE_DESKTOP &&
                    (u == USAGE_X || u == USAGE_Y || u == USAGE_WHEEL) &&
                    (!have_id || m.report_id == g.id) && g.size <= 32u) {
                    if (!have_id) {
                        m.report_id = g.id;
                        have_id = true;
                    }
                    if (u == USAGE_X && m.x_size == 0) {
                        m.x_off = (uint16_t)at;
                        m.x_size = (uint8_t)g.size;
                    } else if (u == USAGE_Y && m.y_size == 0) {
                        m.y_off = (uint16_t)at;
                        m.y_size = (uint8_t)g.size;
                    } else if (u == USAGE_WHEEL && m.wheel_size == 0) {
                        m.wheel_off = (uint16_t)at;
                        m.wheel_size = (uint8_t)g.size;
                    }
                }
            }
            if (!constant && g.page == PAGE_BUTTON && g.size == 1u && off != NULL) {
                buttons_note(&btns, g.id, *off, g.count);
            }
            if (off != NULL) {
                *off += g.count * g.size;
            }
        }
        /* Every main item ends the locals that went with it. */
        n_usages = 0;
        umin = umax = 0;
        have_range = false;
    }

    if (m.x_size == 0 || m.y_size == 0) {
        return false;
    }
    for (unsigned k = 0; k < btns.n; k++) {
        if (btns.id[k] == m.report_id) {
            m.btn_off = btns.off[k];
            m.btn_count = btns.count[k];
        }
    }
    *out = m;
    return true;
}

/* `size` bits at bit `off`, least significant first, as the HID spec lays them. */
static uint32_t bits(const uint8_t *p, size_t len, uint32_t off, uint32_t size,
                     bool *ok)
{
    if (size == 0) {
        return 0;
    }
    if ((off + size + 7u) / 8u > len) {
        *ok = false;
        return 0;
    }
    uint32_t v = 0;
    for (uint32_t b = 0; b < size; b++) {
        const uint32_t at = off + b;
        if (p[at >> 3] & (1u << (at & 7u))) {
            v |= 1u << b;
        }
    }
    return v;
}

static int32_t sign(uint32_t v, uint32_t size)
{
    if (size == 0 || size >= 32u) {
        return (int32_t)v;
    }
    const uint32_t top = 1u << (size - 1u);
    return (v & top) ? (int32_t)(v | ~((top << 1) - 1u)) : (int32_t)v;
}

bool ag_hidrep_decode_mouse(const ag_hidrep_mouse_t *m, const uint8_t *data,
                            size_t len, uint8_t *buttons, int32_t *dx,
                            int32_t *dy, int32_t *wheel)
{
    if (m == NULL || data == NULL || m->x_size == 0 || m->y_size == 0) {
        return false;
    }
    if (m->report_id != 0) {
        if (len < 1u || data[0] != m->report_id) {
            return false;
        }
        data++;
        len--;
    }
    bool ok = true;
    const uint32_t b = bits(data, len, m->btn_off, m->btn_count, &ok);
    const uint32_t x = bits(data, len, m->x_off, m->x_size, &ok);
    const uint32_t y = bits(data, len, m->y_off, m->y_size, &ok);
    bool wok = true;
    const uint32_t w = bits(data, len, m->wheel_off, m->wheel_size, &wok);
    if (!ok) {
        return false;
    }
    if (buttons != NULL) {
        *buttons = (uint8_t)b;
    }
    if (dx != NULL) {
        *dx = sign(x, m->x_size);
    }
    if (dy != NULL) {
        *dy = sign(y, m->y_size);
    }
    if (wheel != NULL) {
        *wheel = wok ? sign(w, m->wheel_size) : 0;
    }
    return true;
}
