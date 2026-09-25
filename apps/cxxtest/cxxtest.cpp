/*
 * ArgonOS - what a C++ .AXE has to be able to do.
 *
 * This is the loader-and-toolchain test for C++, the way apps/hello is for the
 * two-part image.  Every check here is a thing that fails silently rather than
 * loudly when the plumbing is wrong: constructors that never ran leave zeroed
 * objects and the program carries on, a vtable relocated by the wrong bias
 * calls the wrong function, a local static without a guard is built twice.
 *
 *   run t:\cxxtest.axe
 *
 * It prints one line per check and exits non-zero on the first failure, so
 * `argon test ... "errorlevel"` says which one.  The destructors print after
 * the summary on purpose: seeing them at all is the last check, and seeing
 * them in reverse order is the one after that.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

#include "ag_cxx.h"

AG_APP("CXXTEST", "1.0", "argon", 0);

static int s_failures;

/* No libc is linked here on purpose - this image is the C++ runtime and
 * nothing else, so that a missing symbol names the runtime rather than the
 * shim. */
static bool same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static void check(const char *what, bool ok)
{
    ag_printf("%-34s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        ++s_failures;
    }
}

/* ---- constructors, and their order -------------------------------------- */

/*
 * Namespace-scope objects are constructed in the order they are defined within
 * one translation unit.  The tally is written by the constructors themselves,
 * so a zero here means .init_array never ran - which is what happens when the
 * linker script does not name it and ld puts it somewhere the image does not
 * carry.
 */
static char s_order[8];
static int s_order_len;

static void note(char c)
{
    if (s_order_len < (int)sizeof(s_order) - 1) {
        s_order[s_order_len++] = c;
    }
}

struct Marker {
    char tag;
    explicit Marker(char t) : tag(t) { note(tag); }
    ~Marker() { ag_printf("  ~Marker %c\n", tag); }
};

static Marker s_first('a');
static Marker s_second('b');

/*
 * A constant built by a constructor rather than by the assembler: the value has
 * to be computed at run time, which is the difference between .init_array
 * working and the object merely being zero-initialised like any other bss.
 */
struct Computed {
    unsigned value;
    Computed() : value(0)
    {
        for (unsigned i = 1; i <= 100; ++i) {
            value += i;
        }
    }
};

static Computed s_computed;

/* ---- virtual dispatch --------------------------------------------------- */

/*
 * The vtable is a table of addresses in the code part, held in the data part,
 * and it is the case most likely to break under a two-part image: one bias
 * applied where the other belongs sends every virtual call into nothing.
 */
struct Shape {
    virtual ~Shape() {}
    virtual int area() const = 0;
    virtual const char *name() const { return "shape"; }
};

struct Square : Shape {
    int side;
    explicit Square(int s) : side(s) {}
    int area() const override { return side * side; }
    const char *name() const override { return "square"; }
};

struct Rect : Shape {
    int w, h;
    Rect(int a, int b) : w(a), h(b) {}
    int area() const override { return w * h; }
};

/* ---- templates ---------------------------------------------------------- */

template <typename T>
static T largest(const T *v, int n)
{
    T best = v[0];
    for (int i = 1; i < n; ++i) {
        if (best < v[i]) {
            best = v[i];
        }
    }
    return best;
}

/* ---- a function-local static -------------------------------------------- */

struct Once {
    int built;
    Once() : built(1) { ++s_once_builds; }
    static int s_once_builds;
};

int Once::s_once_builds = 0;

static const Once &once()
{
    static Once instance;
    return instance;
}

/* ---- the run ------------------------------------------------------------ */

extern "C" int ag_cxx_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ag_printf("CXXTEST - C++ in a .AXE\n\n");

    s_order[s_order_len] = '\0';
    check("static constructors ran", s_order_len == 2);
    check("in order of definition", s_order[0] == 'a' && s_order[1] == 'b');
    check("constructor computed a value", s_computed.value == 5050);

    Square sq(7);
    Rect re(3, 5);
    Shape *shapes[2] = { &sq, &re };
    check("virtual call through a base", shapes[0]->area() == 49 &&
                                             shapes[1]->area() == 15);
    check("overridden and inherited", same(shapes[0]->name(), "square") &&
                                          same(shapes[1]->name(), "shape"));

    /* new and delete, over the process arena.  The array form matters on its
     * own: it stores a count of its own ahead of the objects. */
    Shape *heap = new Square(9);
    bool heap_ok = heap->area() == 81;
    delete heap;

    int *block = new int[256];
    for (int i = 0; i < 256; ++i) {
        block[i] = i * 3;
    }
    heap_ok = heap_ok && block[255] == 765;
    delete[] block;
    check("new and delete", heap_ok);

    /* Destructors through a base pointer: the virtual one has to be found the
     * same way the virtual call was. */
    static int destroyed = 0;
    struct Counted : Shape {
        int *sink;
        explicit Counted(int *s) : sink(s) {}
        ~Counted() override { ++*sink; }
        int area() const override { return 0; }
    };
    Shape *counted = new Counted(&destroyed);
    delete counted;
    check("virtual destructor", destroyed == 1);

    const int ints[5] = { 4, 17, 2, 9, 11 };
    const char chars[4] = { 'q', 'z', 'a', 'm' };
    check("templates", largest(ints, 5) == 17 && largest(chars, 4) == 'z');

    check("local static built once", once().built == 1 && once().built == 1 &&
                                         Once::s_once_builds == 1);

    ag_printf("\n%d failure(s); destructors follow:\n",
              s_failures);
    return s_failures == 0 ? 0 : 1;
}
