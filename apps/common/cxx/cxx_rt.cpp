/*
 * ArgonOS - the C++ runtime a .AXE gets instead of libstdc++.
 *
 * An application links -nostdlib, so nothing arrives from the toolchain except
 * libgcc.  For C that was enough; C++ asks for four things more, and all four
 * are small enough to keep here where they can be read:
 *
 *   - somebody to run the static constructors before main and the destructors
 *     after.  On a hosted system that is crt1.o; here it is ag_main below;
 *   - operator new and operator delete, over the process arena;
 *   - the guard functions that make a function-local static initialise once;
 *   - the two or three __cxa_* symbols the compiler emits references to and
 *     never explains.
 *
 * What is deliberately missing is exceptions and typeinfo.  Both need a
 * runtime that would have to come from libstdc++ - unwinding wants .eh_frame,
 * which the image discards, and a personality routine - and mkaxe therefore
 * compiles C++ with -fno-exceptions -fno-rtti by default.  A port that throws
 * needs that decision revisited, not this file extended a little.
 *
 * The STL is not here either, and cannot be faked: std::string and std::vector
 * have out-of-line code in libstdc++.a.  Ask for it with `--libs stdc++` and
 * find out per port; the linker takes only the members that resolve something.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

#include "ag_cxx.h"

typedef __SIZE_TYPE__ ag_size_t;

/* ---- constructors and destructors --------------------------------------- */

/*
 * The two arrays the linker script in mkaxe.py brackets.  Each entry is the
 * absolute address of a function in the code part, held in a word in the data
 * part, and the loader has already relocated all of them by the time anything
 * here runs.
 *
 * The names say .init_array, and on Xtensa the entries actually came out of
 * .ctors: the script merges the two into one ascending range so that this side
 * does not have to care which compiler produced them.  See mkaxe.py.
 */
typedef void (*ag_cxx_fn_t)(void);

extern "C" {
extern ag_cxx_fn_t __ag_init_array_start[];
extern ag_cxx_fn_t __ag_init_array_end[];
extern ag_cxx_fn_t __ag_fini_array_start[];
extern ag_cxx_fn_t __ag_fini_array_end[];
}

/*
 * Destructors registered by __cxa_atexit: one per object with a static storage
 * duration and a non-trivial destructor.  A fixed table rather than a list,
 * because the alternative is allocating during construction - which is exactly
 * when the arena may not be there yet, and a failure there has nowhere to go.
 *
 * The count is generous for a port: what registers here is namespace-scope
 * objects and function-local statics, not instances.  Overflowing it is a
 * mistake worth hearing about rather than one worth surviving, so it panics.
 */
#define AG_CXX_ATEXIT_MAX 64

namespace {

struct atexit_entry {
    void (*fn)(void *);
    void *arg;
};

atexit_entry g_atexit[AG_CXX_ATEXIT_MAX];
int g_atexit_count;
bool g_finished;

}  // namespace

void ag_cxx_init(void)
{
    for (ag_cxx_fn_t *p = __ag_init_array_start; p != __ag_init_array_end; ++p) {
        (*p)();
    }
}

void ag_cxx_fini(void)
{
    if (g_finished) {
        return;
    }
    g_finished = true;

    /* Reverse order of registration, and the atexit list before the fini
     * array: the standard says destruction is the reverse of construction, and
     * a __cxa_atexit registration happens after the constructor it undoes. */
    for (int i = g_atexit_count - 1; i >= 0; --i) {
        g_atexit[i].fn(g_atexit[i].arg);
    }
    g_atexit_count = 0;

    for (ag_cxx_fn_t *p = __ag_fini_array_end; p != __ag_fini_array_start; ) {
        (*--p)();
    }
}

/*
 * The image's entry point.  The application writes ag_cxx_main instead of
 * ag_main; mkaxe finds ag_main here and points the header at it.
 *
 * ag_exit is not called: returning is how a .AXE ends, and the loader wants the
 * status back.
 */
extern "C" int ag_main(int argc, char **argv)
{
    ag_cxx_init();
    int rc = ag_cxx_main(argc, argv);
    ag_cxx_fini();
    return rc;
}

/* ---- new and delete ----------------------------------------------------- */

/*
 * Over the process arena, which is what ag_malloc is.  Failure panics rather
 * than returns null: with -fno-exceptions there is no bad_alloc to throw, and
 * a null returned from a throwing operator new is a null the compiler is
 * entitled to assume cannot happen, so it would fault somewhere else and blame
 * the wrong line.
 *
 * The nothrow forms do return null - that is what they are for.
 *
 * The declarations here are written out rather than taken from <new>: the
 * header is available even freestanding, but it drags in <exception>, and the
 * mangled names are short enough to be checked by eye against the compiler's.
 */
namespace std {
struct nothrow_t { explicit nothrow_t() = default; };
extern const nothrow_t nothrow;
const nothrow_t nothrow{};
enum class align_val_t : ag_size_t {};
}  // namespace std

static void *ag_cxx_alloc(ag_size_t n)
{
    /* Zero bytes still has to come back with an address of its own: two
     * objects, however empty, must not compare equal. */
    void *p = ag_malloc(n ? n : 1);
    if (p == NULL) {
        ag_panic("C++: out of memory in operator new");
    }
    return p;
}

void *operator new(ag_size_t n) { return ag_cxx_alloc(n); }
void *operator new[](ag_size_t n) { return ag_cxx_alloc(n); }

void *operator new(ag_size_t n, const std::nothrow_t &) noexcept
{
    return ag_malloc(n ? n : 1);
}
void *operator new[](ag_size_t n, const std::nothrow_t &) noexcept
{
    return ag_malloc(n ? n : 1);
}

void operator delete(void *p) noexcept { ag_free(p); }
void operator delete[](void *p) noexcept { ag_free(p); }

/* The sized forms, which C++14 lets the compiler emit instead of the plain
 * ones.  The size is of no use to an allocator that records its own. */
void operator delete(void *p, ag_size_t) noexcept { ag_free(p); }
void operator delete[](void *p, ag_size_t) noexcept { ag_free(p); }

void operator delete(void *p, const std::nothrow_t &) noexcept { ag_free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { ag_free(p); }

/*
 * Over-aligned types, C++17.  ag_malloc aligns to what the ABI needs and no
 * more, so anything stricter is rounded up by hand and the original pointer is
 * kept in the word below the one handed out.
 */
void *operator new(ag_size_t n, std::align_val_t a)
{
    ag_size_t align = (ag_size_t)a;
    if (align < sizeof(void *)) {
        align = sizeof(void *);
    }
    char *raw = (char *)ag_cxx_alloc(n + align + sizeof(void *));
    char *aligned = (char *)(((ag_size_t)raw + sizeof(void *) + align - 1) &
                             ~(align - 1));
    ((void **)aligned)[-1] = raw;
    return aligned;
}

void *operator new[](ag_size_t n, std::align_val_t a)
{
    return operator new(n, a);
}

void operator delete(void *p, std::align_val_t) noexcept
{
    if (p != NULL) {
        ag_free(((void **)p)[-1]);
    }
}

void operator delete[](void *p, std::align_val_t a) noexcept
{
    operator delete(p, a);
}

void operator delete(void *p, ag_size_t, std::align_val_t a) noexcept
{
    operator delete(p, a);
}

void operator delete[](void *p, ag_size_t, std::align_val_t a) noexcept
{
    operator delete(p, a);
}

/* ---- the __cxa_* the compiler references -------------------------------- */

extern "C" {

/*
 * The identity of the shared object being unloaded.  There is one image and it
 * is never unloaded piecewise, so the value is only ever compared, never used.
 */
void *__dso_handle = &__dso_handle;

/*
 * Registers a destructor for an object with static storage duration.  The dso
 * argument is ignored for the reason above.
 */
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)dso;
    if (g_atexit_count >= AG_CXX_ATEXIT_MAX) {
        ag_panic("C++: too many static destructors; raise AG_CXX_ATEXIT_MAX");
    }
    g_atexit[g_atexit_count].fn = fn;
    g_atexit[g_atexit_count].arg = arg;
    ++g_atexit_count;
    return 0;
}

/* Called on unload in a hosted program; ag_cxx_fini does the work here. */
void __cxa_finalize(void *dso)
{
    (void)dso;
    ag_cxx_fini();
}

/*
 * A pure virtual was called - which means during construction or destruction of
 * a base, because there is no other way to reach one.  There is no recovering:
 * the object is not what its vtable says it is.
 */
void __cxa_pure_virtual(void) { ag_panic("C++: pure virtual function called"); }

void __cxa_deleted_virtual(void)
{
    ag_panic("C++: deleted virtual function called");
}

/*
 * The guard around a function-local static.  The ABI gives eight bytes; the
 * first says whether the initialisation has finished, and the second is used
 * here to catch the one case that is a program error rather than a race - a
 * static whose initialiser reaches the same static again, which would
 * otherwise run the constructor twice on the same storage.
 *
 * There is no locking, so two threads racing to initialise the same static
 * would both run its constructor.  That is a real hole and it is left open
 * knowingly: a port initialises its statics on the thread that starts it, and
 * closing it properly needs an atomic the ABI does not expose yet.  An
 * application that cannot promise that should be built with
 * -fno-threadsafe-statics and initialise by hand.
 */
int __cxa_guard_acquire(unsigned char *guard)
{
    if (guard[0] != 0) {
        return 0;
    }
    if (guard[1] != 0) {
        ag_panic("C++: recursive initialisation of a local static");
    }
    guard[1] = 1;
    return 1;
}

void __cxa_guard_release(unsigned char *guard)
{
    guard[0] = 1;
    guard[1] = 0;
}

void __cxa_guard_abort(unsigned char *guard) { guard[1] = 0; }

}  // extern "C"
