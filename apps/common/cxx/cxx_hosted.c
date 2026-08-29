/*
 * ArgonOS - the handful of hosted-system symbols libstdc++ and newlib insist on.
 *
 * Link this with cxx_rt.cpp and cxx_throw.cpp in an image that uses the STL.
 * Two archives reach past the C library for things a bare-metal target does not
 * normally have, and the linker will not finish without them:
 *
 *   - libstdc++'s eh_globals.o keeps the in-flight exception per thread and
 *     asks POSIX for the storage.  It is pulled in by any object that has a
 *     catch block, which several of the string and vector instantiations do,
 *     even when nothing in the image ever throws;
 *   - newlib's strtoul and its neighbours reach for the per-thread reentrancy
 *     structure through __getreent.  They arrive with std::stoi and friends,
 *     which live in the same archive member as the rest of std::string.
 *
 * All of it is small, and none of it is a stub in the bad sense: the mutexes
 * are real ArgonOS mutexes, and the keys really are per-thread.  What is
 * missing is named where it is missing.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

#include <errno.h>
#include <pthread.h>
#include <reent.h>

/* ---- newlib's reentrancy ------------------------------------------------- */

/*
 * One structure for the process rather than one per thread.  The members that
 * can arrive here - the string-to-number conversions - use it for errno and
 * nothing else, so the cost of sharing it is that two threads converting a
 * malformed number at the same moment can read each other's ERANGE.
 *
 * It is deliberately not a full newlib environment: if something ever pulls in
 * a member that wants the file table or the locale out of here, it will find
 * them zeroed, and the right answer then is to add that function to
 * apps/common/libc rather than to grow this.
 */
static struct _reent s_reent;

struct _reent *__getreent(void) { return &s_reent; }

/* ---- mutexes ------------------------------------------------------------- */

/*
 * pthread_mutex_t is a 32-bit word here and ag_mutex_t is a pointer, so the
 * handle lives in the word itself and no table is needed.
 *
 * A statically initialised mutex arrives as _PTHREAD_MUTEX_INITIALIZER with no
 * call to pthread_mutex_init, so the handle has to be made on first use.  Two
 * threads reaching an untouched static mutex at the same instant would make two
 * of them and one would be lost; that is the one race left here, it needs a
 * compare-and-swap the ABI does not offer yet, and libstdc++ only uses such a
 * mutex from the locale machinery, which this image does not have.
 */
#define AG_PTHREAD_UNSET ((pthread_mutex_t)0xFFFFFFFFu)

static ag_mutex_t handle_of(pthread_mutex_t *m)
{
    if (*m == AG_PTHREAD_UNSET || *m == 0) {
        ag_mutex_t created = ag_mutex_create();
        if (created == NULL) {
            return NULL;
        }
        *m = (pthread_mutex_t)(uintptr_t)created;
    }
    return (ag_mutex_t)(uintptr_t)*m;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
    (void)attr;
    ag_mutex_t created = ag_mutex_create();
    if (created == NULL) {
        return ENOMEM;
    }
    *m = (pthread_mutex_t)(uintptr_t)created;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    if (*m != AG_PTHREAD_UNSET && *m != 0) {
        ag_mutex_delete((ag_mutex_t)(uintptr_t)*m);
    }
    *m = AG_PTHREAD_UNSET;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    ag_mutex_t h = handle_of(m);
    if (h == NULL) {
        return ENOMEM;
    }
    /* Forever: a lock that gives up is not a lock, and the caller has no way
     * to report the failure anyway. */
    ag_mutex_lock(h, UINT32_MAX);
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    ag_mutex_t h = handle_of(m);
    if (h == NULL) {
        return ENOMEM;
    }
    return ag_mutex_lock(h, 0) ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (*m == AG_PTHREAD_UNSET || *m == 0) {
        return EINVAL;
    }
    ag_mutex_unlock((ag_mutex_t)(uintptr_t)*m);
    return 0;
}

/*
 * Not a mutex operation at all.  libstdc++ decides whether the program is
 * threaded by taking the address of pthread_cancel and seeing whether it is
 * null - the old trick for a weakly linked pthreads.  Here it is a real
 * reference to a real symbol, so the answer is yes, and the locking above is
 * used rather than skipped.
 *
 * Cancelling a thread is another matter: FreeRTOS can delete a task, but
 * unwinding one out of somebody else's critical section is not something to
 * offer casually, and nothing asks.
 */
int pthread_cancel(pthread_t thread)
{
    (void)thread;
    return ENOSYS;
}

/* ---- thread-specific data ------------------------------------------------ */

/*
 * A table rather than real thread-local storage, because a process gets four
 * threads and libstdc++ asks for exactly one key.  Both bounds are checked, and
 * running out of either is reported the way POSIX says rather than silently
 * handing back somebody else's slot.
 *
 * The destructor a key carries is remembered and never run: it fires when a
 * thread ends, and the ABI has no way to be told about that yet.  For the one
 * user here it would free the exception globals of a thread that has finished,
 * which on a process-owned arena the process teardown reclaims anyway.
 */
#define AG_TSD_KEYS 8
#define AG_TSD_THREADS 8

static struct {
    bool used;
    void (*dtor)(void *);
} s_keys[AG_TSD_KEYS];

static struct {
    ag_thread_t owner;
    void *value[AG_TSD_KEYS];
} s_slots[AG_TSD_THREADS];

static ag_thread_t this_thread(void)
{
    if (!AG_HAS(g_ag_api->task, self)) {
        return NULL;
    }
    return g_ag_api->task->self();
}

static int slot_of(bool create)
{
    ag_thread_t me = this_thread();
    int free_slot = -1;

    for (int i = 0; i < AG_TSD_THREADS; ++i) {
        if (s_slots[i].owner == me) {
            return i;
        }
        if (s_slots[i].owner == NULL && free_slot < 0) {
            free_slot = i;
        }
    }
    /* NULL is also what a missing task->self reports, and then every thread
     * shares slot 0 - which is the single-threaded case, correctly. */
    if (!create || free_slot < 0) {
        return -1;
    }
    s_slots[free_slot].owner = me;
    return free_slot;
}

int pthread_key_create(pthread_key_t *key, void (*dtor)(void *))
{
    for (int i = 0; i < AG_TSD_KEYS; ++i) {
        if (!s_keys[i].used) {
            s_keys[i].used = true;
            s_keys[i].dtor = dtor;
            *key = (pthread_key_t)i;
            return 0;
        }
    }
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key)
{
    if (key >= AG_TSD_KEYS || !s_keys[key].used) {
        return EINVAL;
    }
    s_keys[key].used = false;
    s_keys[key].dtor = NULL;
    for (int i = 0; i < AG_TSD_THREADS; ++i) {
        s_slots[i].value[key] = NULL;
    }
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    if (key >= AG_TSD_KEYS) {
        return NULL;
    }
    int slot = slot_of(false);
    return slot < 0 ? NULL : s_slots[slot].value[key];
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    if (key >= AG_TSD_KEYS || !s_keys[key].used) {
        return EINVAL;
    }
    int slot = slot_of(true);
    if (slot < 0) {
        return ENOMEM;
    }
    s_slots[slot].value[key] = (void *)value;
    return 0;
}
