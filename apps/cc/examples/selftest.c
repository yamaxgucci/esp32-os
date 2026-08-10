/*
 * Argon CC self-test: arithmetic, arrays, calls and console, in the subset
 * itself.  Compile it with CC on the guest and run it - the exit code is the
 * number of checks that did not hold, so `errorlevel` printing 0 is the pass.
 *
 *   run t:\cc.axe t:\st.c t:\st.axe
 *   run t:\st.axe
 *   errorlevel
 *
 * Host tests can only tell that this compiles.  Whether the code it generates
 * is right - the frame it allocates, the registers it keeps a value in, the
 * order it passes arguments - only shows when it runs.
 */

#include "ppinc.h"
/* Twice on purpose: what makes that legal is the guard inside the header. */
#include "ppinc.h"

#define ONE     1
#define TWO     (ONE + ONE)
#define FOUR    (TWO * TWO)
#define SQ(x)   ((x) * (x))
#define ADD(a, b) ((a) + (b))
#define SCALE(v)  ((v) << TWO)
#define TAG     'Q'
#define GREET   "cc selftest\n"

/* Defined but empty, and defined then taken away: both are still `#ifdef`-able. */
#define FEATURE
#define GONE 1
#undef GONE

struct pt {
    int x;
    int y;
};

struct rec {
    char tag;
    int  n;
    char name[4];
};

struct box {
    struct rec r;
    int        k;
};

struct node {
    int          v;
    struct node *next;
};

int  gsum;
int  garr[8];
char gch;
int  gguard;
char gbuf[8];

struct pt gp;
struct pt gpa[4];
int       gpguard;

int six(int a, int b, int c, int d, int e, int f)
{
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6;
}

int prec(int a, int b)
{
    return a + b / 2;
}

int prec2(int a, int b)
{
    return a * b + a % b;
}

int cmp(int a, int b)
{
    if (a < b) {
        return 1;
    }
    if (a > b) {
        return 2;
    }
    return 3;
}

int deep(int a, int b)
{
    return (a + b) * (a - b) + (a * b) / (a + 1) - b % 3;
}

int gcd(int a, int b)
{
    int t;
    while (b > 0) {
        t = a % b;
        a = b;
        b = t;
    }
    return a;
}

int fill(int n)
{
    int i;
    int s;
    s = 0;
    for (i = 0; i < n; i = i + 1) {
        garr[i] = i * i + 1;
    }
    for (i = 0; i < n; i = i + 1) {
        s = s + garr[i];
    }
    return s;
}

int locsum(void)
{
    int a[6];
    int i;
    int s;
    s = 0;
    for (i = 0; i < 6; i = i + 1) {
        a[i] = i * 3 - 1;
    }
    for (i = 0; i < 6; i = i + 1) {
        s = s + a[i];
    }
    return s;
}

int bits(int x, int y)
{
    return (x & y) + (x | y) * 2 + (x ^ y) * 4;
}

int shifts(int v, int n)
{
    return (v << n) + (v >> 1) + (1 << 8);
}

int shr(int v, int n)
{
    return v >> n;
}

int shrc(int v)
{
    return v >> 2;
}

/* `&` is looser than `==`, so this is x & (3 == 3) and not (x & 3) == 3. */
int prec3(int x)
{
    return x & 3 == 3;
}

int prec4(int a, int b)
{
    return a << b + 1;
}

int slen(char *s)
{
    int n;
    n = 0;
    while (*s != 0) {
        n = n + 1;
        s = s + 1;
    }
    return n;
}

int sat(char *s, int i)
{
    return s[i];
}

/* Bytes packed next to each other: a store must not reach its neighbours. */
int chars(void)
{
    char b[8];
    int  i;
    int  s;
    s = 0;
    for (i = 0; i < 8; i = i + 1) {
        b[i] = i * 16 + 3;
    }
    for (i = 0; i < 8; i = i + 1) {
        s = s + b[i];
    }
    return s;
}

/* A char holds a byte and reads back unsigned. */
int trunc8(void)
{
    char b[2];
    b[0] = 300;
    b[1] = 255;
    return b[0] * 1000 + b[1];
}

int ptrstep(void)
{
    int *p;
    garr[0] = 11;
    garr[1] = 22;
    garr[2] = 33;
    p = garr;
    p = p + 2;
    return *p;
}

int addrof(void)
{
    int  n;
    int *p;
    n = 1;
    p = &n;
    *p = 42;
    return n;
}

int cptr(void)
{
    char  b[4];
    char *p;
    p = &b[1];
    *p = 200;
    b[0] = 1;
    return b[0] * 1000 + b[1];
}

int cparam(char c)
{
    return c;
}

int stfields(void)
{
    struct pt p;
    p.x = 3;
    p.y = 4;
    return p.x * p.x + p.y * p.y;
}

/* A byte field and a word field share a struct without treading on each other. */
int stmixed(void)
{
    struct rec r;
    r.n = 123456;
    r.tag = 200;
    r.name[0] = 7;
    r.name[3] = 9;
    return r.tag + r.n + r.name[0] + r.name[3];
}

int stnest(void)
{
    struct box b;
    b.k = 5;
    b.r.n = 11;
    b.r.tag = 'a';
    b.r.name[1] = 3;
    return b.k * 1000 + b.r.n * 10 + b.r.tag + b.r.name[1];
}

/* An array of structs steps by the whole struct, not by a word. */
int starr(void)
{
    struct pt a[4];
    int       i;
    int       s;
    s = 0;
    for (i = 0; i < 4; i = i + 1) {
        a[i].x = i + 1;
        a[i].y = (i + 1) * 10;
    }
    for (i = 0; i < 4; i = i + 1) {
        s = s + a[i].x * a[i].y;
    }
    return s;
}

int setpt(struct pt *p, int x, int y)
{
    p->x = x;
    p->y = y;
    return p->x + p->y;
}

int stptr(void)
{
    struct pt p;
    int       r;
    r = setpt(&p, 3, 9);
    return r * 100 + p.x * 10 + p.y;
}

int ststep(void)
{
    struct pt *p;
    int        i;
    int        s;
    for (i = 0; i < 4; i = i + 1) {
        gpa[i].x = i;
        gpa[i].y = i * 2;
    }
    p = gpa;
    p = p + 2;
    s = p->x * 10 + p->y;
    p[1].y = 77;
    return s * 100 + gpa[3].y;
}

/* A node points at its own type, whose size was not known when it was written. */
int stlist(void)
{
    struct node  a;
    struct node  b;
    struct node  c;
    struct node *n;
    int          s;
    a.v = 1;
    b.v = 2;
    c.v = 4;
    a.next = &b;
    b.next = &c;
    c.next = 0;
    s = 0;
    n = &a;
    while (n != 0) {
        s = s * 10 + n->v;
        n = n->next;
    }
    return s;
}

int stglob(void)
{
    gpguard = 999;
    gp.x = 5;
    gp.y = 6;
    gpa[0].x = 1;
    return gp.x * 100 + gp.y * 10 + gpa[0].x;
}

/* The preprocessor: a name that is text, and text that is chosen by a name. */
int ppconst(void)
{
    int a[FOUR];
    int i;
    i = 0;
    while (i < FOUR) {
        a[i] = i * TWO;
        i = i + 1;
    }
    return a[FOUR - ONE] + TAG;
}

int ppcall(int n)
{
    return SQ(n) + ADD(n, ONE) + SCALE(n) + SQ(ADD(ONE, ONE));
}

/* The argument is a sum, so only the parentheses around it give 25 and not 11. */
int ppparen(void)
{
    return SQ(ADD(TWO, 3));
}

int ppcond(void)
{
    int v;
    v = 0;
#ifdef FEATURE
    v = v + 1;
#else
    this is not C at all ]]]
#endif
#ifdef GONE
    v = v + 100;
#endif
#ifndef GONE
    v = v + 10;
#endif
#ifdef FEATURE
#ifndef GONE
    v = v + 1000;
#endif
#endif
    return v;
}

int incsum(struct incpt *p)
{
    return p->x * 10 + p->y;
}

int ppinclude(void)
{
    int          grid[INC_BOX(2, 3)];
    struct incpt p;
    p.x = 4;
    p.y = 2;
    grid[INC_BOX(2, 3) - 1] = INC_MARK;
    return incsum(&p) + grid[5];
}

int check(int got, int want)
{
    if (got == want) {
        return 0;
    }
    ag_printf("FAIL got %d want %d\n", got, want);
    return 1;
}

int ag_main(void)
{
    int bad;
    bad = 0;

    ag_print(GREET);

    bad = bad + check(prec(10, 4), 12);
    bad = bad + check(prec2(17, 5), 87);
    bad = bad + check(six(1, 2, 3, 4, 5, 6), 91);
    bad = bad + check(cmp(2, 5), 1);
    bad = bad + check(cmp(5, 2), 2);
    bad = bad + check(cmp(4, 4), 3);
    bad = bad + check(deep(9, 4), 67);
    bad = bad + check(gcd(48, 18), 6);
    bad = bad + check(fill(5), 35);
    bad = bad + check(locsum(), 39);

    gsum = 7;
    gsum = gsum * 3 + 1;
    bad = bad + check(gsum, 22);

    bad = bad + check(six(1, 2, 3, 4, 5, 6) + prec(10, 4), 103);

    bad = bad + check(bits(12, 10), 60);
    bad = bad + check(shifts(5, 3), 298);
    bad = bad + check(shr(-16, 2), -4);
    bad = bad + check(shrc(-16), -4);
    bad = bad + check(~0, -1);
    bad = bad + check(~5 & 255, 250);
    bad = bad + check(prec3(5), 1);
    bad = bad + check(prec4(1, 2), 8);

    bad = bad + check(slen("hello"), 5);
    bad = bad + check(sat("hello", 1), 101);
    bad = bad + check(chars(), 472);
    bad = bad + check(trunc8(), 44255);
    bad = bad + check(ptrstep(), 33);
    bad = bad + check(addrof(), 42);
    bad = bad + check(cptr(), 1200);
    bad = bad + check(cparam('z'), 122);
    bad = bad + check(cparam(300), 44);

    gguard = 12345;
    gch = 'A';
    gbuf[0] = 'B';
    gbuf[1] = 0;
    bad = bad + check(gch, 65);
    bad = bad + check(gbuf[0], 66);
    bad = bad + check(gguard, 12345);
    bad = bad + check(slen(gbuf), 1);

    bad = bad + check(stfields(), 25);
    bad = bad + check(stmixed(), 123672);
    bad = bad + check(stnest(), 5210);
    bad = bad + check(starr(), 300);
    bad = bad + check(stptr(), 1239);
    bad = bad + check(ststep(), 2477);
    bad = bad + check(stlist(), 124);
    bad = bad + check(stglob(), 561);
    bad = bad + check(gpguard, 999);
    bad = bad + check(gguard, 12345);

    bad = bad + check(ppconst(), 87);
    bad = bad + check(ppcall(3), 29);
    bad = bad + check(ppparen(), 25);
    bad = bad + check(ppcond(), 1011);
    bad = bad + check(ppinclude(), 83);

    ag_print("failures: ");
    ag_print_int(bad);
    ag_print("\n");
    return bad;
}
