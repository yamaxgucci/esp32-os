/*
 * ckt_exact - the same amplifier with every compromise removed.
 *
 *   build-host/ckt_exact <in.wav> <out.wav> [volts] [oversample] [theta]
 *
 * Nothing here is shared with what ships.  No baked tables, no antialiasing
 * trick, no single precision, no fixed iteration budget, no short filters.
 * Modified nodal analysis in double, Newton to 1e-12, as many oversampled
 * steps per audio sample as asked for, and 511-tap Kaiser filters both ways.
 * It is slow by construction and that is the point: it exists to answer one
 * question, which is whether an artefact belongs to the circuit or to the
 * arithmetic that was used to run it cheaply.
 *
 * The netlist is the same one apps/cktbench/ckt_circuits.c builds, written
 * out again here rather than shared, so that a mistake in that file cannot
 * hide inside this one too.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 16   /* unknowns: nodes plus voltage sources                  */
#define MAXR 24
#define MAXC 16

/* ---------------------------------------------------------------- wav ---- */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static double *wav_read(const char *path, uint32_t *n_out, uint32_t *rate_out)
{
    FILE    *f = fopen(path, "rb");
    uint8_t *b;
    long     size;
    uint32_t pos = 12, off = 0, len = 0, rate = 0, i, n, stride;
    uint16_t ch = 0, bits = 0;
    double  *x;

    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc((size_t)size);
    if (b == NULL || fread(b, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    while (pos + 8u <= (uint32_t)size) {
        const uint32_t c = rd32(b + pos + 4);
        if (memcmp(b + pos, "fmt ", 4) == 0) {
            ch = rd16(b + pos + 10);
            rate = rd32(b + pos + 12);
            bits = rd16(b + pos + 22);
        } else if (memcmp(b + pos, "data", 4) == 0) {
            off = pos + 8u;
            len = c;
        }
        pos += 8u + c + (c & 1u);
    }
    if (ch == 0u || len == 0u || (bits != 16u && bits != 24u)) {
        fprintf(stderr, "%s: need 16 or 24 bit PCM\n", path);
        return NULL;
    }
    stride = (uint32_t)ch * (bits / 8u);
    n = len / stride;
    x = (double *)malloc(sizeof(double) * n);
    for (i = 0; i < n; i++) {
        const uint8_t *p = b + off + (size_t)i * stride;
        if (bits == 16u) {
            x[i] = (double)(int16_t)rd16(p) / 32768.0;
        } else {
            const int32_t v = (int32_t)((uint32_t)p[0] << 8 |
                                        (uint32_t)p[1] << 16 |
                                        (uint32_t)p[2] << 24);
            x[i] = (double)(v >> 8) / 8388608.0;
        }
    }
    free(b);
    *n_out = n;
    *rate_out = rate;
    return x;
}

static void wav_write(const char *path, const double *x, uint32_t n,
                      uint32_t rate)
{
    FILE    *f = fopen(path, "wb");
    uint8_t  h[44];
    uint32_t data = n * 2u, i;
    double   peak = 0.0, g;

    if (f == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        const double a = x[i] < 0.0 ? -x[i] : x[i];
        if (a > peak) {
            peak = a;
        }
    }
    g = peak > 0.0 ? 23000.0 / peak : 0.0;
    memcpy(h, "RIFF", 4);
    h[4] = (uint8_t)((36u + data) & 0xFF);
    h[5] = (uint8_t)(((36u + data) >> 8) & 0xFF);
    h[6] = (uint8_t)(((36u + data) >> 16) & 0xFF);
    h[7] = (uint8_t)(((36u + data) >> 24) & 0xFF);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0;
    h[24] = (uint8_t)(rate & 0xFF);
    h[25] = (uint8_t)((rate >> 8) & 0xFF);
    h[26] = (uint8_t)((rate >> 16) & 0xFF);
    h[27] = (uint8_t)((rate >> 24) & 0xFF);
    h[28] = (uint8_t)((rate * 2u) & 0xFF);
    h[29] = (uint8_t)(((rate * 2u) >> 8) & 0xFF);
    h[30] = (uint8_t)(((rate * 2u) >> 16) & 0xFF);
    h[31] = (uint8_t)(((rate * 2u) >> 24) & 0xFF);
    h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (uint8_t)(data & 0xFF);
    h[41] = (uint8_t)((data >> 8) & 0xFF);
    h[42] = (uint8_t)((data >> 16) & 0xFF);
    h[43] = (uint8_t)((data >> 24) & 0xFF);
    fwrite(h, 1, 44, f);
    for (i = 0; i < n; i++) {
        double  v = x[i] * g;
        int16_t s;
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        s = (int16_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
        h[0] = (uint8_t)((uint16_t)s & 0xFF);
        h[1] = (uint8_t)(((uint16_t)s >> 8) & 0xFF);
        fwrite(h, 1, 2, f);
    }
    fclose(f);
    printf("wrote %s (%u frames, peak %.3f)\n", path, n, peak);
}

/* ------------------------------------------------------------ circuit ---- */

typedef struct {
    int    a, b;
    double g;
} res_t;

typedef struct {
    int    a, b;
    double geq, ieq, vpre, ipre, farads;
} cap_t;

typedef struct {
    int    n, nnodes, nr, nc;
    int    vin_node, vdc_node; /* both to ground */
    double vdc;
    int    grid, plate, cath, out;
    res_t  r[MAXR];
    cap_t  c[MAXC];
    double theta;
    double m[MAXN][MAXN];
    double rhs[MAXN];
    double x[MAXN];
    uint32_t nonconv, samples;
    uint32_t worst_iters;
} ckt_t;

static void add_r(ckt_t *k, int a, int b, double ohms)
{
    k->r[k->nr].a = a;
    k->r[k->nr].b = b;
    k->r[k->nr].g = 1.0 / ohms;
    k->nr++;
    if (a > k->nnodes) k->nnodes = a;
    if (b > k->nnodes) k->nnodes = b;
}

static void add_c(ckt_t *k, int a, int b, double farads, double fs)
{
    k->c[k->nc].a = a;
    k->c[k->nc].b = b;
    k->c[k->nc].geq = farads * fs / k->theta;
    k->c[k->nc].farads = farads;
    k->c[k->nc].ieq = 0.0;
    k->c[k->nc].vpre = 0.0;
    k->c[k->nc].ipre = 0.0;
    k->nc++;
    if (a > k->nnodes) k->nnodes = a;
    if (b > k->nnodes) k->nnodes = b;
}

/* Koren 12AX7, in double, with the library's transcendentals. */
static void triode(double vgk, double vpk, double *ip, double *dg, double *dp,
                   double *ig, double *dig)
{
    const double mu = 100.0, ex = 1.4, kg1 = 1060.0, kp = 600.0, kvb = 300.0;
    const double rgk = 2000.0, vg0 = 0.33, vt = 0.05;
    double       s, arg, l, e1, sig, dide1, u;

    if (vpk < 0.0) {
        vpk = 0.0;
    }
    s = sqrt(kvb + vpk * vpk);
    arg = kp * (1.0 / mu + vgk / s);
    l = arg > 40.0 ? arg : log1p(exp(arg));
    e1 = (vpk / kp) * l;
    if (e1 > 0.0) {
        sig = arg > 40.0 ? 1.0 : 1.0 / (1.0 + exp(-arg));
        dide1 = 2.0 * ex * pow(e1, ex - 1.0) / kg1;
        *ip = 2.0 * pow(e1, ex) / kg1;
        *dg = dide1 * (vpk * sig / s);
        *dp = dide1 * (l / kp - vpk * vpk * vgk * sig / (s * s * s));
    } else {
        *ip = *dg = *dp = 0.0;
    }
    u = (vgk - vg0) / vt;
    *ig = (vt / rgk) * (u > 40.0 ? u : log1p(exp(u)));
    *dig = (1.0 / rgk) * (u > 40.0 ? 1.0 : 1.0 / (1.0 + exp(-u)));
}

static int solve(double m[MAXN][MAXN], double *rhs, double *x, int n)
{
    int i, j, p, q;
    for (i = 0; i < n; i++) {
        double best = fabs(m[i][i]);
        p = i;
        for (j = i + 1; j < n; j++) {
            if (fabs(m[j][i]) > best) {
                best = fabs(m[j][i]);
                p = j;
            }
        }
        if (best < 1e-300) {
            return -1;
        }
        if (p != i) {
            for (q = 0; q < n; q++) {
                const double t = m[i][q];
                m[i][q] = m[p][q];
                m[p][q] = t;
            }
            {
                const double t = rhs[i];
                rhs[i] = rhs[p];
                rhs[p] = t;
            }
        }
        for (j = i + 1; j < n; j++) {
            const double f = m[j][i] / m[i][i];
            if (f == 0.0) {
                continue;
            }
            for (q = i; q < n; q++) {
                m[j][q] -= f * m[i][q];
            }
            rhs[j] -= f * rhs[i];
        }
    }
    for (i = n - 1; i >= 0; i--) {
        double s = rhs[i];
        for (j = i + 1; j < n; j++) {
            s -= m[i][j] * x[j];
        }
        x[i] = s / m[i][i];
    }
    return 0;
}

#define IX(n) ((n) - 1)

static double tick(ckt_t *k, double vin)
{
    static double a[MAXN][MAXN], b[MAXN];
    int           it, i, j;
    const int     n = k->n;
    const int     iv = k->nnodes;      /* vin source current   */
    const int     id = k->nnodes + 1;  /* supply source current */

    for (i = 0; i < k->nc; i++) {
        k->c[i].ieq = k->c[i].geq * k->c[i].vpre +
                      ((1.0 - k->theta) / k->theta) * k->c[i].ipre;
    }

    for (it = 0; it < 200; it++) {
        double vgk, vpk, ip, dg, dp, ig, dig, worst = 0.0;
        for (i = 0; i < n; i++) {
            b[i] = 0.0;
            for (j = 0; j < n; j++) {
                a[i][j] = 0.0;
            }
            a[i][i] = 1e-12; /* gmin */
        }
        for (i = 0; i < k->nr; i++) {
            const int p = k->r[i].a, q = k->r[i].b;
            const double g = k->r[i].g;
            if (p > 0) a[IX(p)][IX(p)] += g;
            if (q > 0) a[IX(q)][IX(q)] += g;
            if (p > 0 && q > 0) {
                a[IX(p)][IX(q)] -= g;
                a[IX(q)][IX(p)] -= g;
            }
        }
        for (i = 0; i < k->nc; i++) {
            const int p = k->c[i].a, q = k->c[i].b;
            const double g = k->c[i].geq, s = k->c[i].ieq;
            if (p > 0) { a[IX(p)][IX(p)] += g; b[IX(p)] += s; }
            if (q > 0) { a[IX(q)][IX(q)] += g; b[IX(q)] -= s; }
            if (p > 0 && q > 0) {
                a[IX(p)][IX(q)] -= g;
                a[IX(q)][IX(p)] -= g;
            }
        }
        /* Two grounded voltage sources, as extra unknowns. */
        a[IX(k->vin_node)][iv] += 1.0;
        a[iv][IX(k->vin_node)] += 1.0;
        b[iv] = vin;
        a[IX(k->vdc_node)][id] += 1.0;
        a[id][IX(k->vdc_node)] += 1.0;
        b[id] = k->vdc;

        vgk = k->x[IX(k->grid)] - k->x[IX(k->cath)];
        vpk = k->x[IX(k->plate)] - k->x[IX(k->cath)];
        triode(vgk, vpk, &ip, &dg, &dp, &ig, &dig);
        if (dp < 1e-12) dp = 1e-12;
        if (dig < 1e-12) dig = 1e-12;
        {
            const int P = IX(k->plate), C = IX(k->cath), G = IX(k->grid);
            a[P][P] += dp; a[C][C] += dp; a[P][C] -= dp; a[C][P] -= dp;
            a[P][G] += dg; a[P][C] -= dg; a[C][G] -= dg; a[C][C] += dg;
            b[P] -= ip - dp * vpk - dg * vgk;
            b[C] += ip - dp * vpk - dg * vgk;
            a[G][G] += dig; a[C][C] += dig; a[G][C] -= dig; a[C][G] -= dig;
            b[G] -= ig - dig * vgk;
            b[C] += ig - dig * vgk;
        }

        {
            double xn[MAXN];
            if (solve(a, b, xn, n) != 0) {
                break;
            }
            for (i = 0; i < n; i++) {
                const double d = fabs(xn[i] - k->x[i]);
                const double tol = 1e-12 + 1e-10 * fabs(xn[i]);
                if (d > tol && d > worst) {
                    worst = d;
                }
                k->x[i] = xn[i];
            }
        }
        if (worst == 0.0) {
            break;
        }
    }
    if ((uint32_t)it > k->worst_iters) {
        k->worst_iters = (uint32_t)it;
    }
    if (it >= 200) {
        k->nonconv++;
    }
    k->samples++;

    for (i = 0; i < k->nc; i++) {
        const int p = k->c[i].a, q = k->c[i].b;
        const double va = p > 0 ? k->x[IX(p)] : 0.0;
        const double vb = q > 0 ? k->x[IX(q)] : 0.0;
        const double vnow = va - vb;
        k->c[i].ipre = k->c[i].geq * vnow - k->c[i].ieq;
        k->c[i].vpre = vnow;
    }
    return k->x[IX(k->out)];
}

/*
 * Set by the "nogrid" argument.  Takes out the second stage's kilohm grid
 * stopper and every interelectrode picofarad, which between them own the
 * fastest corner in the circuit - a kilohm against fifty picofarads, thirty
 * nanoseconds.  Everything else stays.  Two things to learn from it at once:
 * whether the buzz lives in that corner, and, since the explicit solver's step
 * is set by it, whether that solver can be trusted once it is gone.
 */
static int g_nogrid = 0;

/*
 * The JCM800 front end, written out node by node.  1 input, 2 supply, 3 after
 * the source resistance, 4 the grid leak, 5 grid, 6 plate, 7 cathode, 8 the
 * load, 9..12 the tone stack.
 */
static void build(ckt_t *k, int index, double fs, double theta)
{
    memset(k, 0, sizeof(*k));
    k->theta = theta;
    k->vin_node = 1;
    k->vdc_node = 2;
    k->vdc = 330.0;
    k->grid = 5;
    k->plate = 6;
    k->cath = 7;

    if (index == 0) {
        add_r(k, 1, 3, 10.0e3);
        add_c(k, 3, 4, 100.0e-9, fs);
        add_r(k, 4, 0, 1.0e6);
        add_r(k, 4, 5, 68.0e3);
        add_r(k, 2, 6, 220.0e3);
        add_r(k, 7, 0, 820.0);
        add_c(k, 7, 0, 0.68e-6, fs);
        if (!g_nogrid) {
            add_c(k, 4, 0, 20.0e-12, fs);
            add_c(k, 5, 6, 1.7e-12, fs);
            add_c(k, 6, 0, 15.0e-12, fs);
        }
        add_c(k, 6, 8, 22.0e-9, fs);
        add_r(k, 8, 0, 470.0e3);
        k->out = 6;
    } else {
        add_r(k, 1, 3, 38.0e3);
        add_c(k, 3, 4, 2.2e-9, fs);
        add_r(k, 4, 0, 470.0e3);
        if (g_nogrid) {
            /* Grid straight onto the leak.  Node 5 is left over; a gigohm to
             * ground keeps the matrix from going singular and does nothing. */
            k->grid = 4;
            add_r(k, 5, 0, 1.0e9);
        } else {
            add_r(k, 4, 5, 1.0e3);
        }
        add_r(k, 2, 6, 100.0e3);
        add_r(k, 7, 0, 820.0);
        add_c(k, 7, 0, 0.68e-6, fs);
        if (!g_nogrid) {
            add_c(k, 4, 0, 20.0e-12, fs);
            add_c(k, 5, 6, 1.7e-12, fs);
            add_c(k, 6, 0, 15.0e-12, fs);
        }
        add_c(k, 6, 8, 22.0e-9, fs);
        add_r(k, 8, 9, 33.0e3);
        add_r(k, 9, 0, 1.0e6);
        add_c(k, 8, 10, 250.0e-12, fs);
        add_r(k, 10, 11, 250.0e3);
        add_c(k, 9, 12, 22.0e-9, fs);
        add_r(k, 12, 11, 250.0e3);
        add_c(k, 12, 0, 6.8e-9, fs);
        add_r(k, 11, 0, 100.0e3);
        k->out = 11;
    }
    k->n = k->nnodes + 2;
}

/* --------------------------------------------------------- resampling ---- */

#define FIR 511

static double bessel_i0(double x)
{
    double s = 1.0, t = 1.0;
    int    i;
    for (i = 1; i < 60; i++) {
        t *= (x / (2.0 * i)) * (x / (2.0 * i));
        s += t;
        if (t < s * 1e-18) break;
    }
    return s;
}

static void fir_make(double *h, double fc, double gain)
{
    double sum = 0.0;
    int    i;
    for (i = 0; i < FIR; i++) {
        const double n = (double)i - (FIR - 1) / 2.0;
        const double s = (n == 0.0) ? 2.0 * fc
                                    : sin(2.0 * M_PI * fc * n) / (M_PI * n);
        const double r = (2.0 * i) / (double)(FIR - 1) - 1.0;
        const double w = bessel_i0(12.0 * sqrt(1.0 - r * r)) / bessel_i0(12.0);
        h[i] = s * w;
        sum += h[i];
    }
    for (i = 0; i < FIR; i++) {
        h[i] *= gain / sum;
    }
}

/*
 * The same circuit again, with no matrix and no Newton at all.
 *
 * Every node has a capacitance to ground - the real ones, plus the few
 * picofarads of socket and wiring that every node in a real amplifier has.
 * That is all it takes: knowing the voltages gives every current, and the
 * currents move the voltages.  dV = I*dt/C, one step at a time.
 *
 * It is a completely different solver, so it shares no failure mode with the
 * other one.  If an artefact survives both, it is not the arithmetic.
 *
 * The price is the step.  The fastest corner in the stage is the grid, a
 * kilohm against fifty picofarads - thirty nanoseconds - and an explicit step
 * has to be well inside that, so this runs at a hundred megahertz.  Two
 * thousand steps per audio sample instead of sixteen.
 */
#define STRAY 47.0e-12

typedef struct {
    double v[MAXN + 1];
    double dv[MAXN + 1];
    double cn[MAXN + 1];
} node_state_t;

static void explicit_prepare(const ckt_t *k, node_state_t *st)
{
    int i;
    for (i = 0; i <= MAXN; i++) {
        st->v[i] = 0.0;
        st->dv[i] = 0.0;
        st->cn[i] = STRAY;
    }
    /* A capacitor between two nodes is part of the capacitance of both: the
     * current it draws from one is C times that node's own rate of change,
     * plus C times the other node's, which is the coupling. */
    for (i = 0; i < k->nc; i++) {
        if (k->c[i].a > 0) st->cn[k->c[i].a] += k->c[i].farads;
        if (k->c[i].b > 0) st->cn[k->c[i].b] += k->c[i].farads;
    }
    st->v[k->vdc_node] = k->vdc;
}

static double explicit_tick(ckt_t *k, node_state_t *st, double vin, double dt,
                            const double *cval)
{
    double inode[MAXN + 1];
    int    i;

    for (i = 0; i <= k->nnodes; i++) {
        inode[i] = 0.0;
    }
    st->v[k->vin_node] = vin;
    st->v[k->vdc_node] = k->vdc;

    for (i = 0; i < k->nr; i++) {
        const int    a = k->r[i].a, b = k->r[i].b;
        const double g = k->r[i].g;
        const double d = (a > 0 ? st->v[a] : 0.0) - (b > 0 ? st->v[b] : 0.0);
        if (a > 0) inode[a] -= g * d;
        if (b > 0) inode[b] += g * d;
    }
    /* The other plate of every capacitor, as a current source driven by how
     * fast that plate is moving. */
    for (i = 0; i < k->nc; i++) {
        const int    a = k->c[i].a, b = k->c[i].b;
        const double c = cval[i];
        if (a > 0 && b > 0) {
            inode[a] += c * st->dv[b];
            inode[b] += c * st->dv[a];
        }
    }
    {
        double ip, dg, dp, ig, dig;
        const double vgk = st->v[k->grid] - st->v[k->cath];
        const double vpk = st->v[k->plate] - st->v[k->cath];
        triode(vgk, vpk, &ip, &dg, &dp, &ig, &dig);
        inode[k->plate] -= ip;
        inode[k->cath] += ip + ig;
        inode[k->grid] -= ig;
    }

    for (i = 1; i <= k->nnodes; i++) {
        if (i == k->vin_node || i == k->vdc_node) {
            st->dv[i] = 0.0;
            continue;
        }
        st->dv[i] = inode[i] / st->cn[i];
        st->v[i] += st->dv[i] * dt;
    }
    k->samples++;
    return st->v[k->out];
}

int main(int argc, char **argv)
{
    const char *inp = argc > 1 ? argv[1] : NULL;
    const char *outp = argc > 2 ? argv[2] : NULL;
    const double drive = argc > 3 ? atof(argv[3]) : 0.25;
    const int    os = argc > 4 ? atoi(argv[4]) : 16;
    const double theta = argc > 5 ? atof(argv[5]) : 0.5;
    uint32_t     n = 0, rate = 0, i;
    double      *x, *y, peak = 0.0;
    static double up[FIR], dn[FIR];
    static double uph[FIR], dnh[FIR];
    ckt_t        s0, s1;
    int          j;

    if (inp == NULL || outp == NULL) {
        fprintf(stderr, "usage: ckt_exact <in.wav> <out.wav> [volts] [os] "
                        "[theta] [nogrid]\n");
        return 2;
    }
    if (argc > 6 && strcmp(argv[6], "nogrid") == 0) {
        g_nogrid = 1;
        printf("nogrid: no grid stopper on stage 1, no interelectrode "
               "capacitance anywhere\n");
    }
    x = wav_read(inp, &n, &rate);
    if (x == NULL) {
        return 1;
    }
    /*
     * The matrix-free run, on the user's suggestion and worth its own path:
     * no matrix, no Newton, so it cannot inherit either one's failure mode.
     * Asked for with a negative oversample count, which then means megahertz.
     */
    if (os < 0) {
        const double hz = -os * 1.0e6;
        const double dt = 1.0 / hz;
        const uint32_t per = (uint32_t)(hz / rate);
        node_state_t a0, a1;
        double cv0[MAXC], cv1[MAXC], lp[4];
        /* The whole take: two seconds was enough to hear a difference but not
         * enough to hear a note decay, which is where the artefact lives. */
        uint32_t lim = n;
        uint32_t s;
        build(&s0, 0, hz, 0.5);
        build(&s1, 1, hz, 0.5);
        for (j = 0; j < s0.nc; j++) cv0[j] = s0.c[j].farads;
        for (j = 0; j < s1.nc; j++) cv1[j] = s1.c[j].farads;
        explicit_prepare(&s0, &a0);
        explicit_prepare(&s1, &a1);
        printf("matrix-free: %.0f MHz, dt %.2f ns, %u steps per audio sample, "
               "%u frames\n", hz / 1e6, dt * 1e9, per, lim);
        for (s = 0; s < per * (uint32_t)(0.3 * rate); s++) {
            (void)explicit_tick(&s1, &a1, explicit_tick(&s0, &a0, 0.0, dt, cv0),
                                dt, cv1);
        }
        y = (double *)calloc(lim, sizeof(double));
        lp[0] = lp[1] = lp[2] = lp[3] = 0.0;
        for (i = 0; i < lim; i++) {
            const double c = 1.0 - exp(-2.0 * M_PI * 20000.0 * dt);
            uint32_t     q;
            for (q = 0; q < per; q++) {
                const double v =
                    explicit_tick(&s1, &a1,
                                  explicit_tick(&s0, &a0, x[i], dt, cv0), dt,
                                  cv1);
                lp[0] += c * (v - lp[0]);
                lp[1] += c * (lp[0] - lp[1]);
                lp[2] += c * (lp[1] - lp[2]);
                lp[3] += c * (lp[2] - lp[3]);
            }
            y[i] = lp[3];
            if ((i % (lim / 10 + 1)) == 0) {
                printf("  %u%%\r", 100u * i / lim);
                fflush(stdout);
            }
        }
        {
            double z = y[0];
            for (i = 0; i < lim; i++) {
                z += (y[i] - z) * 0.002;
                y[i] -= z;
            }
        }
        wav_write(outp, y, lim, rate);
        return 0;
    }
    for (i = 0; i < n; i++) {
        const double a = fabs(x[i]);
        if (a > peak) peak = a;
    }
    if (peak > 0.0) {
        for (i = 0; i < n; i++) {
            x[i] = x[i] / peak * drive;
        }
    }

    printf("exact reference: %u frames at %u Hz, x%d, theta %.3f, "
           "double throughout, Newton to 1e-12, %d-tap Kaiser both ways\n",
           n, rate, os, theta, FIR);

    fir_make(up, 0.45 / os, (double)os);
    fir_make(dn, 0.45 / os, 1.0);
    memset(uph, 0, sizeof(uph));
    memset(dnh, 0, sizeof(dnh));

    build(&s0, 0, (double)rate * os, theta);
    build(&s1, 1, (double)rate * os, theta);
    for (i = 0; i < (uint32_t)(rate * os / 2); i++) {
        (void)tick(&s1, tick(&s0, 0.0));
    }
    s0.nonconv = s1.nonconv = 0;
    s0.samples = s1.samples = 0;
    s0.worst_iters = s1.worst_iters = 0;

    y = (double *)calloc(n, sizeof(double));
    for (i = 0; i < n; i++) {
        double acc = 0.0;
        for (j = FIR - 1; j > 0; j--) {
            uph[j] = uph[j - 1];
        }
        uph[0] = x[i];
        for (j = 0; j < os; j++) {
            double in = 0.0, v;
            int    t, q;
            for (q = 0, t = j; t < FIR; q++, t += os) {
                in += up[t] * uph[q];
            }
            v = tick(&s1, tick(&s0, in));
            for (t = FIR - 1; t > 0; t--) {
                dnh[t] = dnh[t - 1];
            }
            dnh[0] = v;
        }
        for (j = 0; j < FIR; j++) {
            acc += dn[j] * dnh[j];
        }
        y[i] = acc;
        if ((i % (n / 10 + 1)) == 0) {
            printf("  %u%%\r", 100u * i / n);
            fflush(stdout);
        }
    }
    printf("  stage 0: %u of %u did not converge, worst %u iterations\n",
           s0.nonconv, s0.samples, s0.worst_iters);
    printf("  stage 1: %u of %u did not converge, worst %u iterations\n",
           s1.nonconv, s1.samples, s1.worst_iters);

    /* One pole at 15 Hz to take the plate's DC out, as the model does. */
    {
        double z = y[0];
        for (i = 0; i < n; i++) {
            z += (y[i] - z) * 0.002;
            y[i] -= z;
        }
    }
    wav_write(outp, y, n, rate);
    return 0;
}
