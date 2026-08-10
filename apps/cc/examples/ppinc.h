/*
 * Included by selftest.c, so that `#include` itself is one of the things the
 * self-test checks: the guard has to hold, and the macros and the struct have
 * to arrive.  No function bodies here - globals in the including file must
 * still precede every function, and a function in a header would close that
 * window.
 */
#ifndef PPINC_H
#define PPINC_H

#define INC_MARK  41
#define INC_BOX(w, h) ((w) * (h))

struct incpt {
    int x;
    int y;
};

#endif
