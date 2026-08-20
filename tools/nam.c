/*
 * The WaveNet a NAM capture holds, and the JSON it is wrapped in.
 *
 * The whole of the design note is in nam.h.  What matters while reading this
 * file: every loop here is in the order the Go implementation runs it, and every
 * accumulator is a float rather than a double, because the gate on this port is
 * a render that matches one made by that implementation bit for bit.  Widening
 * an accumulator would be an improvement in the abstract and would break the one
 * check that says this is the same model.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Algorithm and parameter layout: Neural Amp Modeler / nlpodyssey/waveny.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nam.h"

/* ------------------------------------------------------------------------ */
/* errors                                                                    */
/* ------------------------------------------------------------------------ */

static char g_err[512];

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    return -1;
}

const char *nam_err(void) { return g_err[0] != 0 ? g_err : "no error"; }

/* ------------------------------------------------------------------------ */
/* JSON                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Enough JSON for this schema and no more.
 *
 * There is no document tree: the file stays in one buffer and a "node" is an
 * offset into it.  Looking a key up rescans its object, which for a 300 kB file
 * asked a few hundred questions is nothing, and it means a capture of any size
 * costs one allocation plus the weights.
 *
 * The one subtlety is skipping a value: brackets are counted, but only the kind
 * the value opened with, and strings are stepped over whole - so an object
 * containing arrays and an array containing objects both come out right without
 * a stack.
 */

static int j_ws(const char *s, int i)
{
    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') {
        i++;
    }
    return i;
}

/* Offset just past the closing quote of the string at i. */
static int j_str_end(const char *s, int i)
{
    i++;
    while (s[i] != 0 && s[i] != '"') {
        if (s[i] == '\\' && s[i + 1] != 0) {
            i++;
        }
        i++;
    }
    return s[i] == '"' ? i + 1 : -1;
}

/* Offset just past the value at i. */
static int j_skip(const char *s, int i)
{
    i = j_ws(s, i);
    if (s[i] == '{' || s[i] == '[') {
        const char open = s[i];
        const char close = (open == '{') ? '}' : ']';
        int        depth = 0;
        while (s[i] != 0) {
            if (s[i] == '"') {
                i = j_str_end(s, i);
                if (i < 0) {
                    return -1;
                }
                continue;
            }
            if (s[i] == open) {
                depth++;
            } else if (s[i] == close) {
                depth--;
                if (depth == 0) {
                    return i + 1;
                }
            }
            i++;
        }
        return -1;
    }
    if (s[i] == '"') {
        return j_str_end(s, i);
    }
    while (s[i] != 0 && strchr(",}] \t\r\n", s[i]) == NULL) {
        i++;
    }
    return i;
}

/* The value of `key` in the object at `obj`, or -1 if the object has no such
 * key.  -1 is also what a missing object gives, so a caller can chain. */
static int j_get(const char *s, int obj, const char *key)
{
    const size_t klen = strlen(key);
    int          i;

    if (obj < 0) {
        return -1;
    }
    i = j_ws(s, obj);
    if (s[i] != '{') {
        return -1;
    }
    i++;
    for (;;) {
        int ks, ke;
        i = j_ws(s, i);
        if (s[i] == '}' || s[i] == 0) {
            return -1;
        }
        if (s[i] != '"') {
            return -1;
        }
        ks = i + 1;
        ke = j_str_end(s, i);
        if (ke < 0) {
            return -1;
        }
        i = j_ws(s, ke);
        if (s[i] != ':') {
            return -1;
        }
        i = j_ws(s, i + 1);
        if ((size_t)(ke - 1 - ks) == klen && memcmp(s + ks, key, klen) == 0) {
            return i;
        }
        i = j_skip(s, i);
        if (i < 0) {
            return -1;
        }
        i = j_ws(s, i);
        if (s[i] == ',') {
            i++;
        }
    }
}

/* First element of the array at `arr`, -1 if it is empty or not an array. */
static int j_first(const char *s, int arr)
{
    int i;
    if (arr < 0) {
        return -1;
    }
    i = j_ws(s, arr);
    if (s[i] != '[') {
        return -1;
    }
    i = j_ws(s, i + 1);
    return s[i] == ']' ? -1 : i;
}

/* The element after `elem`, -1 at the end of the array. */
static int j_next(const char *s, int elem)
{
    int i = j_skip(s, elem);
    if (i < 0) {
        return -1;
    }
    i = j_ws(s, i);
    if (s[i] != ',') {
        return -1;
    }
    i = j_ws(s, i + 1);
    return s[i] == ']' ? -1 : i;
}

static int j_count(const char *s, int arr)
{
    int n = 0, i;
    for (i = j_first(s, arr); i >= 0; i = j_next(s, i)) {
        n++;
    }
    return n;
}

static int j_int(const char *s, int i)
{
    return i < 0 ? 0 : (int)strtol(s + i, NULL, 10);
}

static double j_num(const char *s, int i)
{
    return i < 0 ? 0.0 : strtod(s + i, NULL);
}

static int j_is(const char *s, int i, const char *lit)
{
    return i >= 0 && strncmp(s + i, lit, strlen(lit)) == 0;
}

/* A string value equal to `lit`.  Only used on schema keywords, so escapes do
 * not have to be undone. */
static int j_str_is(const char *s, int i, const char *lit)
{
    const size_t n = strlen(lit);
    return i >= 0 && s[i] == '"' && strncmp(s + i + 1, lit, n) == 0 &&
           s[i + 1 + n] == '"';
}

/* An array of integers into `out`; the count, or -1 if it does not fit. */
static int j_ints(const char *s, int arr, int *out, int max)
{
    int n = 0, i;
    for (i = j_first(s, arr); i >= 0; i = j_next(s, i)) {
        if (n >= max) {
            return -1;
        }
        out[n++] = j_int(s, i);
    }
    return n;
}

/* ------------------------------------------------------------------------ */
/* matrices                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Row-major with a stride, which is what makes a *view* of a few columns free:
 * `stride` stays the underlying buffer's width while `cols` narrows.  The layer
 * history buffers are 70 000 columns wide and every operation runs on a window
 * of a few thousand of them, so this is the whole reason the model can be fed in
 * blocks at all.
 *
 * Views may start before the window a caller was given - a dilated convolution
 * reaches back by (kernel-1) * dilation columns - which is why the buffers are
 * allocated with the receptive field in front of the first sample and why
 * `d` is allowed to be interior to the allocation.
 */
typedef struct {
    int    rows;
    int    stride;
    int    cols;
    float *d;
} nm_mat;

static int nm_alloc(nm_mat *m, int rows, int cols)
{
    m->rows = rows;
    m->stride = cols;
    m->cols = cols;
    m->d = (float *)calloc((size_t)rows * (size_t)cols, sizeof(float));
    return m->d != NULL ? 0 : fail("out of memory for a %dx%d matrix", rows,
                                   cols);
}

static nm_mat nm_cols(const nm_mat *m, int from, int n)
{
    nm_mat v;
    v.rows = m->rows;
    v.stride = m->stride;
    v.cols = n;
    v.d = m->d + from;
    return v;
}

static nm_mat nm_top(const nm_mat *m, int rows)
{
    nm_mat v = *m;
    v.rows = rows;
    return v;
}

static void nm_zero(nm_mat *m)
{
    int i;
    for (i = 0; i < m->rows; i++) {
        memset(m->d + (size_t)i * (size_t)m->stride, 0,
               sizeof(float) * (size_t)m->cols);
    }
}

static void nm_copy(nm_mat *dst, const nm_mat *src)
{
    const int n = dst->cols < src->cols ? dst->cols : src->cols;
    int       i;
    for (i = 0; i < dst->rows; i++) {
        memmove(dst->d + (size_t)i * (size_t)dst->stride,
                src->d + (size_t)i * (size_t)src->stride,
                sizeof(float) * (size_t)n);
    }
}

/* c = a * b, and c += a * b.  The accumulator is a float on purpose; see the
 * note at the top of this file. */
static void nm_product(const nm_mat *a, const nm_mat *b, nm_mat *c)
{
    int i, j, k;
    for (i = 0; i < c->rows; i++) {
        const float *arow = a->d + (size_t)i * (size_t)a->stride;
        float       *crow = c->d + (size_t)i * (size_t)c->stride;
        for (j = 0; j < c->cols; j++) {
            float        v = 0.0f;
            const float *bp = b->d + j;
            for (k = 0; k < a->cols; k++) {
                v += arow[k] * *bp;
                bp += b->stride;
            }
            crow[j] = v;
        }
    }
}

static void nm_add_product(const nm_mat *a, const nm_mat *b, nm_mat *c)
{
    int i, j, k;
    for (i = 0; i < c->rows; i++) {
        const float *arow = a->d + (size_t)i * (size_t)a->stride;
        float       *crow = c->d + (size_t)i * (size_t)c->stride;
        for (j = 0; j < c->cols; j++) {
            float        v = crow[j];
            const float *bp = b->d + j;
            for (k = 0; k < a->cols; k++) {
                v += arow[k] * *bp;
                bp += b->stride;
            }
            crow[j] = v;
        }
    }
}

static void nm_add(nm_mat *a, const nm_mat *b)
{
    int i, j;
    for (i = 0; i < a->rows; i++) {
        float       *arow = a->d + (size_t)i * (size_t)a->stride;
        const float *brow = b->d + (size_t)i * (size_t)b->stride;
        for (j = 0; j < b->cols; j++) {
            arow[j] += brow[j];
        }
    }
}

/* One value per row, added along the whole row - a bias over time. */
static void nm_add_bias(nm_mat *m, const float *bias)
{
    int i, j;
    for (i = 0; i < m->rows; i++) {
        const float v = bias[i];
        float      *row = m->d + (size_t)i * (size_t)m->stride;
        for (j = 0; j < m->cols; j++) {
            row[j] += v;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* activations                                                               */
/* ------------------------------------------------------------------------ */

enum { ACT_TANH = 0, ACT_SIGMOID, ACT_LEAKYRELU };

/*
 * LeakyReLU is what every capture here uses and the loader refuses a slope other
 * than the 0.01 written below.
 *
 * Tanh and Sigmoid are here for the older models and are the one place this port
 * cannot promise bit-exactness: the reference computes them in double precision
 * with Go's own implementation, and libm's answer can differ in the last bit.
 * It has no effect on any measurement in this tree - nothing here uses them -
 * but a render of such a model should not be described as identical.
 */
static void act_apply(int act, nm_mat *m)
{
    int i, j;
    for (i = 0; i < m->rows; i++) {
        float *row = m->d + (size_t)i * (size_t)m->stride;
        for (j = 0; j < m->cols; j++) {
            const float v = row[j];
            if (act == ACT_LEAKYRELU) {
                if (v < 0.0f) {
                    row[j] = v * 0.01f;
                }
            } else if (act == ACT_TANH) {
                row[j] = (float)tanh((double)v);
            } else {
                row[j] = (float)(1.0 / (1.0 + exp(-(double)v)));
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* the parameter stream                                                      */
/* ------------------------------------------------------------------------ */

/*
 * The weights are one flat array and every block below takes what it needs in
 * the order the trainer wrote it.  Reading them in the wrong order does not
 * fail; it plays something else.  The count check in nam_load is what catches
 * that, so `over` is watched: taking one number too many is a misread topology,
 * not a rounding difference.
 */
typedef struct {
    const float *w;
    int          n;
    int          i;
    int          over;
} wstream_t;

static float w_next(wstream_t *r)
{
    if (r->i >= r->n) {
        r->over++;
        return 0.0f;
    }
    return r->w[r->i++];
}

/* ------------------------------------------------------------------------ */
/* convolutions                                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
    int     in_ch, out_ch, k, dilation, has_bias;
    nm_mat *w;    /* k matrices, each (out_ch, in_ch) */
    float  *bias; /* out_ch, or NULL */
} conv1d_t;

typedef struct {
    int    in_ch, out_ch, has_bias;
    nm_mat w; /* (out_ch, in_ch) */
    float *bias;
} conv1x1_t;

static int conv1d_init(conv1d_t *c, int in_ch, int out_ch, int k, int dilation,
                       int bias)
{
    int i;
    c->in_ch = in_ch;
    c->out_ch = out_ch;
    c->k = k;
    c->dilation = dilation;
    c->has_bias = bias;
    c->w = (nm_mat *)calloc((size_t)(k > 0 ? k : 1), sizeof(nm_mat));
    c->bias = bias ? (float *)calloc((size_t)out_ch, sizeof(float)) : NULL;
    if (c->w == NULL || (bias && c->bias == NULL)) {
        return fail("out of memory for a convolution");
    }
    for (i = 0; i < k; i++) {
        if (nm_alloc(&c->w[i], out_ch, in_ch) != 0) {
            return -1;
        }
    }
    return 0;
}

/* out, then in, then the kernel tap - the order the trainer serialises it. */
static void conv1d_params(conv1d_t *c, wstream_t *r)
{
    int i, j, k;
    for (i = 0; i < c->out_ch; i++) {
        for (j = 0; j < c->in_ch; j++) {
            for (k = 0; k < c->k; k++) {
                c->w[k].d[(size_t)i * (size_t)c->w[k].stride + (size_t)j] =
                    w_next(r);
            }
        }
    }
    if (c->has_bias) {
        for (i = 0; i < c->out_ch; i++) {
            c->bias[i] = w_next(r);
        }
    }
}

static void conv1d_process(const conv1d_t *c, const nm_mat *in, nm_mat *out,
                           int in_start, int n, int out_start)
{
    nm_mat o = nm_cols(out, out_start, n);
    int    k;

    {
        const int offset = c->dilation * (1 - c->k);
        nm_mat    v = nm_cols(in, in_start + offset, n);
        nm_product(&c->w[0], &v, &o);
    }
    for (k = 1; k < c->k; k++) {
        const int offset = c->dilation * (k + 1 - c->k);
        nm_mat    v = nm_cols(in, in_start + offset, n);
        nm_add_product(&c->w[k], &v, &o);
    }
    if (c->has_bias) {
        nm_add_bias(&o, c->bias);
    }
}

static int conv1x1_init(conv1x1_t *c, int in_ch, int out_ch, int bias)
{
    c->in_ch = in_ch;
    c->out_ch = out_ch;
    c->has_bias = bias;
    c->bias = bias ? (float *)calloc((size_t)out_ch, sizeof(float)) : NULL;
    if (bias && c->bias == NULL) {
        return fail("out of memory for a 1x1 convolution");
    }
    return nm_alloc(&c->w, out_ch, in_ch);
}

static void conv1x1_params(conv1x1_t *c, wstream_t *r)
{
    int i, j;
    for (i = 0; i < c->out_ch; i++) {
        for (j = 0; j < c->in_ch; j++) {
            c->w.d[(size_t)i * (size_t)c->w.stride + (size_t)j] = w_next(r);
        }
    }
    if (c->has_bias) {
        for (i = 0; i < c->out_ch; i++) {
            c->bias[i] = w_next(r);
        }
    }
}

static void conv1x1_process(const conv1x1_t *c, const nm_mat *in, nm_mat *out)
{
    nm_product(&c->w, in, out);
    if (c->has_bias) {
        nm_add_bias(out, c->bias);
    }
}

/* ------------------------------------------------------------------------ */
/* a layer                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    conv1d_t  front;
    conv1x1_t mixin;
    conv1x1_t post;
    int       act;
    nm_mat    state;
    nm_mat    tmp;
} layer_t;

static int layer_init(layer_t *l, int cond_size, int channels, int k,
                      int dilation, int act, int max_frames)
{
    l->act = act;
    if (conv1d_init(&l->front, channels, channels, k, dilation, 1) != 0 ||
        conv1x1_init(&l->mixin, cond_size, channels, 0) != 0 ||
        conv1x1_init(&l->post, channels, channels, 1) != 0 ||
        nm_alloc(&l->state, channels, max_frames) != 0 ||
        nm_alloc(&l->tmp, channels, max_frames) != 0) {
        return -1;
    }
    return 0;
}

static void layer_params(layer_t *l, wstream_t *r)
{
    conv1d_params(&l->front, r);
    conv1x1_params(&l->mixin, r);
    conv1x1_params(&l->post, r);
}

/*
 * One residual layer: a dilated convolution of the running signal, plus the
 * conditioning (the dry input, here), through the activation; what comes out is
 * added to the head's accumulator and, after a 1x1, back onto the input to make
 * the next layer's input.
 */
static void layer_process(layer_t *l, const nm_mat *in, const nm_mat *cond,
                          nm_mat *head_in, nm_mat *out, int in_start,
                          int out_start)
{
    const int n = cond->cols;
    const int channels = l->front.in_ch;
    nm_mat    top, outv, inv;

    conv1d_process(&l->front, in, &l->state, in_start, n, 0);
    conv1x1_process(&l->mixin, cond, &l->tmp);
    nm_add(&l->state, &l->tmp);
    act_apply(l->act, &l->state);

    top = nm_top(&l->state, channels);
    nm_add(head_in, &top);

    outv = nm_cols(out, out_start, n);
    conv1x1_process(&l->post, &top, &outv);
    inv = nm_cols(in, in_start, n);
    nm_add(&outv, &inv);
}

/* ------------------------------------------------------------------------ */
/* a layer array                                                             */
/* ------------------------------------------------------------------------ */

/*
 * 65536 columns of history plus the receptive field, which is the reference's
 * number and has to be, because *when* the buffers are rewound is visible in the
 * output: a rewind copies the tail back to the front, and the convolutions then
 * read from a different place in the allocation.  The arithmetic is the same
 * either way in exact terms; in float it is not, and the test here is exactness.
 */
#define ARRAY_BUFFER 65536
#define MAX_LAYERS   64

typedef struct {
    conv1x1_t rechannel;
    layer_t  *layers;
    int       n_layers;
    nm_mat   *bufs; /* one per layer, (channels, ARRAY_BUFFER + receptive) */
    int       buf_start;
    int       receptive;
    int       channels, head_size;

    /* The head is either a 1x1 rechannel or, in newer captures, a convolution
     * over time - which needs history that the caller's head matrix does not
     * keep, so the array holds its own copy of it. */
    int       head_kernel;
    conv1d_t  head_conv;
    conv1x1_t head_rechannel;
    nm_mat    head_buf;
    int       head_buf_start;
} array_t;

typedef struct {
    int input_size, cond_size, channels, head_size, head_bias;
    int n_dil;
    int dil[MAX_LAYERS];
    int ks[MAX_LAYERS];
    int head_kernel;
    int act;
} array_cfg_t;

static int array_init(array_t *a, const array_cfg_t *c, int max_frames)
{
    int i, cols;

    a->n_layers = c->n_dil;
    a->channels = c->channels;
    a->head_size = c->head_size;
    a->head_kernel = c->head_kernel > 1 ? c->head_kernel : 0;
    a->layers = (layer_t *)calloc((size_t)c->n_dil, sizeof(layer_t));
    a->bufs = (nm_mat *)calloc((size_t)c->n_dil, sizeof(nm_mat));
    if (a->layers == NULL || a->bufs == NULL) {
        return fail("out of memory for a layer array");
    }
    if (conv1x1_init(&a->rechannel, c->input_size, c->channels, 0) != 0) {
        return -1;
    }
    if (a->head_kernel > 0) {
        if (conv1d_init(&a->head_conv, c->channels, c->head_size,
                        c->head_kernel, 1, c->head_bias) != 0 ||
            nm_alloc(&a->head_buf, c->channels,
                     ARRAY_BUFFER + c->head_kernel) != 0) {
            return -1;
        }
        a->head_buf_start = c->head_kernel - 1;
    } else if (conv1x1_init(&a->head_rechannel, c->channels, c->head_size,
                            c->head_bias) != 0) {
        return -1;
    }

    a->receptive = 0;
    for (i = 0; i < c->n_dil; i++) {
        if (layer_init(&a->layers[i], c->cond_size, c->channels, c->ks[i],
                       c->dil[i], c->act, max_frames) != 0) {
            return -1;
        }
        a->receptive += (c->ks[i] - 1) * c->dil[i];
    }
    if (ARRAY_BUFFER - max_frames <= a->receptive) {
        return fail("a block of %d samples does not fit a receptive field of %d",
                    max_frames, a->receptive);
    }
    cols = ARRAY_BUFFER + a->receptive;
    for (i = 0; i < c->n_dil; i++) {
        if (nm_alloc(&a->bufs[i], c->channels, cols) != 0) {
            return -1;
        }
    }
    a->buf_start = a->receptive;
    return 0;
}

static void array_params(array_t *a, wstream_t *r)
{
    int i;
    conv1x1_params(&a->rechannel, r);
    for (i = 0; i < a->n_layers; i++) {
        layer_params(&a->layers[i], r);
    }
    if (a->head_kernel > 0) {
        conv1d_params(&a->head_conv, r);
    } else {
        conv1x1_params(&a->head_rechannel, r);
    }
}

/*
 * Rewind: the buffers are finite, so when the write position runs out, each
 * layer's last (kernel-1) * dilation columns are copied back to the front and
 * the position returns to the receptive field.  Only what a convolution can
 * still reach is kept; everything older is unreachable by construction.
 */
static void array_rewind(array_t *a)
{
    const int start = a->receptive;
    int       i;
    for (i = 0; i < a->n_layers; i++) {
        const layer_t *l = &a->layers[i];
        const int      d = (l->front.k - 1) * l->front.dilation;
        nm_mat         to = nm_cols(&a->bufs[i], start - d, d);
        nm_mat         from = nm_cols(&a->bufs[i], a->buf_start - d, d);
        nm_copy(&to, &from);
    }
    a->buf_start = start;
}

static void array_prepare(array_t *a, int n)
{
    if (a->buf_start + n > a->bufs[0].cols) {
        array_rewind(a);
    }
}

static void array_process(array_t *a, const nm_mat *in, const nm_mat *cond,
                          nm_mat *head_in, nm_mat *out, nm_mat *head_out)
{
    const int last = a->n_layers - 1;
    int       i, n;

    {
        nm_mat dst = nm_cols(&a->bufs[0], a->buf_start, in->cols);
        conv1x1_process(&a->rechannel, in, &dst);
    }
    for (i = 0; i < last; i++) {
        layer_process(&a->layers[i], &a->bufs[i], cond, head_in,
                      &a->bufs[i + 1], a->buf_start, a->buf_start);
    }
    layer_process(&a->layers[last], &a->bufs[last], cond, head_in, out,
                  a->buf_start, 0);

    if (a->head_kernel == 0) {
        conv1x1_process(&a->head_rechannel, head_in, head_out);
        return;
    }
    /* The head's own history, appended to and rewound the same way. */
    n = head_in->cols;
    if (a->head_buf_start + n > a->head_buf.cols) {
        const int keep = a->head_kernel - 1;
        nm_mat    to = nm_cols(&a->head_buf, 0, keep);
        nm_mat    from = nm_cols(&a->head_buf, a->head_buf_start - keep, keep);
        nm_copy(&to, &from);
        a->head_buf_start = keep;
    }
    {
        nm_mat to = nm_cols(&a->head_buf, a->head_buf_start, n);
        nm_copy(&to, head_in);
    }
    conv1d_process(&a->head_conv, &a->head_buf, head_out, a->head_buf_start, n,
                   0);
    a->head_buf_start += n;
}

/* ------------------------------------------------------------------------ */
/* the model                                                                 */
/* ------------------------------------------------------------------------ */

#define MAX_ARRAYS 8

struct nam_model {
    array_t arrays[MAX_ARRAYS];
    int     n_arrays;
    nm_mat  array_out[MAX_ARRAYS];
    nm_mat  head[MAX_ARRAYS + 1];
    nm_mat  cond;
    float   head_scale;
    int     num_frames; /* what the buffers are currently shaped for */
    int     max_frames;
    int     receptive;
    int     rate;
};

/*
 * Every resizable buffer is allocated once at the largest block and only its
 * width changes, but it is *zeroed* whenever the width changes - because the
 * reference allocates a fresh matrix at that moment, and a fresh matrix is zero.
 * Nothing here depends on it, since each is fully written before it is read; it
 * is done because "nothing depends on it" is exactly the kind of claim that
 * stops being true quietly.
 */
static void set_frames(nam_model_t *m, int n)
{
    int i;

    if (n == m->num_frames) {
        return;
    }
    m->cond.cols = n;
    nm_zero(&m->cond);
    for (i = 0; i <= m->n_arrays; i++) {
        m->head[i].cols = n;
        nm_zero(&m->head[i]);
    }
    for (i = 0; i < m->n_arrays; i++) {
        int j;
        m->array_out[i].cols = n;
        nm_zero(&m->array_out[i]);
        for (j = 0; j < m->arrays[i].n_layers; j++) {
            m->arrays[i].layers[j].state.cols = n;
            m->arrays[i].layers[j].tmp.cols = n;
            nm_zero(&m->arrays[i].layers[j].state);
            nm_zero(&m->arrays[i].layers[j].tmp);
        }
    }
    m->num_frames = n;
}

void nam_process(nam_model_t *m, const float *in, float *out, int n)
{
    int i;

    if (n <= 0) {
        return;
    }
    /*
     * A block longer than the buffers is split rather than refused, because the
     * two are the same thing: a host test renders 70 000 samples in blocks of
     * 4096 and of 333 and the outputs are bit-identical, which is the property
     * that makes this safe to do behind the caller's back.  Refusing quietly, as
     * this used to, left the caller with an untouched output buffer.
     */
    while (n > m->max_frames) {
        nam_process(m, in, out, m->max_frames);
        in += m->max_frames;
        out += m->max_frames;
        n -= m->max_frames;
    }
    set_frames(m, n);
    for (i = 0; i < m->n_arrays; i++) {
        array_prepare(&m->arrays[i], n);
    }
    for (i = 0; i < n; i++) {
        m->cond.d[i] = in[i];
    }
    nm_zero(&m->head[0]);

    array_process(&m->arrays[0], &m->cond, &m->cond, &m->head[0],
                  &m->array_out[0], &m->head[1]);
    for (i = 1; i < m->n_arrays; i++) {
        array_process(&m->arrays[i], &m->array_out[i - 1], &m->cond,
                      &m->head[i], &m->array_out[i], &m->head[i + 1]);
    }
    for (i = 0; i < n; i++) {
        out[i] = m->head_scale * m->head[m->n_arrays].d[i];
    }
    /* Finalize: the write position moves on by the block just written. */
    for (i = 0; i < m->n_arrays; i++) {
        m->arrays[i].buf_start += n;
    }
}

int nam_receptive_field(const nam_model_t *m) { return m->receptive; }
int nam_sample_rate(const nam_model_t *m) { return m->rate; }

/* ------------------------------------------------------------------------ */
/* loading                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * Everything below is the schema check, and it is deliberately unforgiving.  The
 * captures in this tree all say the same thing - one layer array, eight
 * channels, twenty-three layers, LeakyReLU, a sixteen-tap head - and the format
 * can say a great deal more than that.  Anything it says that is not implemented
 * here is an error rather than a default, because the failure mode of a default
 * is a render that sounds like an amplifier and is not the one that was asked
 * for.
 */
static int switch_off(const char *s, int layer, const char *key)
{
    const int o = j_get(s, layer, key);
    if (o < 0 || j_is(s, o, "null")) {
        return 0;
    }
    if (j_is(s, j_get(s, o, "active"), "true")) {
        return fail("%s is enabled and is not implemented here", key);
    }
    if (j_int(s, j_get(s, o, "groups")) > 1) {
        return fail("%s uses %d groups and is not implemented here", key,
                    j_int(s, j_get(s, o, "groups")));
    }
    return 0;
}

/* A name, as either the old string form or a newer per-layer object. */
static int act_by_name(const char *s, int i, int *act)
{
    if (j_str_is(s, i, "Tanh")) {
        *act = ACT_TANH;
    } else if (j_str_is(s, i, "Sigmoid")) {
        *act = ACT_SIGMOID;
    } else if (j_str_is(s, i, "LeakyReLU")) {
        *act = ACT_LEAKYRELU;
    } else {
        return fail("activation %.20s is not implemented here",
                    i < 0 ? "(none)" : s + i);
    }
    return 0;
}

/* The activation, insisting every layer of the array agrees - the array holds
 * one, as the reference does. */
static int read_act(const char *s, int layer, int *act)
{
    const int raw = j_get(s, layer, "activation");
    int       e, first = -1;

    if (raw < 0 || j_is(s, raw, "null")) {
        *act = ACT_TANH;
        return 0;
    }
    if (s[raw] == '"') {
        return act_by_name(s, raw, act);
    }
    for (e = j_first(s, raw); e >= 0; e = j_next(s, e)) {
        int one = -1;
        if (act_by_name(s, j_get(s, e, "type"), &one) != 0) {
            return -1;
        }
        if (first < 0) {
            first = one;
        } else if (one != first) {
            return fail("the layers do not all use the same activation");
        }
        if (one == ACT_LEAKYRELU) {
            const double slope = j_num(s, j_get(s, e, "negative_slope"));
            if (slope != 0.01) {
                return fail("LeakyReLU slope %g is not the 0.01 implemented"
                            " here", slope);
            }
        }
    }
    if (first < 0) {
        return fail("the activation list is empty");
    }
    *act = first;
    return 0;
}

/* One layer array's configuration, and how many parameters that shape needs. */
static int read_array(const char *s, int layer, array_cfg_t *c, int *params)
{
    static const char *const off[] = {
        "head1x1",           "conv_pre_film",         "conv_post_film",
        "input_mixin_pre_film", "input_mixin_post_film", "activation_pre_film",
        "activation_post_film", "layer1x1_post_film",  "head1x1_post_film"
    };
    size_t i;
    int    head, e, ks_n, kdefault;

    memset(c, 0, sizeof(*c));
    for (i = 0; i < sizeof(off) / sizeof(off[0]); i++) {
        if (switch_off(s, layer, off[i]) != 0) {
            return -1;
        }
    }
    {
        const int l1 = j_get(s, layer, "layer1x1");
        if (l1 >= 0 && !j_is(s, l1, "null") &&
            (!j_is(s, j_get(s, l1, "active"), "true") ||
             j_int(s, j_get(s, l1, "groups")) > 1)) {
            return fail("layer1x1 must be active and ungrouped");
        }
    }
    if (j_int(s, j_get(s, layer, "groups_input")) > 1) {
        return fail("a grouped input convolution is not implemented here");
    }
    if (j_int(s, j_get(s, layer, "groups_input_mixin")) > 1) {
        return fail("a grouped input mixin is not implemented here");
    }
    if (j_is(s, j_get(s, layer, "gated"), "true")) {
        return fail("gated layers are not implemented here");
    }
    for (e = j_first(s, j_get(s, layer, "gating_mode")); e >= 0;
         e = j_next(s, e)) {
        if (!j_str_is(s, e, "none")) {
            return fail("a gating mode other than none is not implemented here");
        }
    }
    for (e = j_first(s, j_get(s, layer, "secondary_activation")); e >= 0;
         e = j_next(s, e)) {
        if (!j_is(s, e, "null")) {
            return fail("a secondary activation is not implemented here");
        }
    }

    c->input_size = j_int(s, j_get(s, layer, "input_size"));
    c->cond_size = j_int(s, j_get(s, layer, "condition_size"));
    c->channels = j_int(s, j_get(s, layer, "channels"));
    c->head_size = j_int(s, j_get(s, layer, "head_size"));
    c->head_bias = j_is(s, j_get(s, layer, "head_bias"), "true");
    {
        const int b = j_get(s, layer, "bottleneck");
        if (b >= 0 && !j_is(s, b, "null") && j_int(s, b) != c->channels) {
            return fail("a bottleneck of %d differs from %d channels",
                        j_int(s, b), c->channels);
        }
    }
    head = j_get(s, layer, "head");
    if (head >= 0 && !j_is(s, head, "null")) {
        c->head_size = j_int(s, j_get(s, head, "out_channels"));
        c->head_bias = j_is(s, j_get(s, head, "bias"), "true");
        c->head_kernel = j_int(s, j_get(s, head, "kernel_size"));
    }
    if (read_act(s, layer, &c->act) != 0) {
        return -1;
    }

    c->n_dil = j_ints(s, j_get(s, layer, "dilations"), c->dil, MAX_LAYERS);
    if (c->n_dil < 1) {
        return fail("a layer array needs between 1 and %d dilations",
                    MAX_LAYERS);
    }
    kdefault = j_int(s, j_get(s, layer, "kernel_size"));
    ks_n = j_ints(s, j_get(s, layer, "kernel_sizes"), c->ks, MAX_LAYERS);
    if (ks_n < 0) {
        return fail("more kernel sizes than the %d dilations allowed",
                    MAX_LAYERS);
    }
    for (i = (size_t)(ks_n < 0 ? 0 : ks_n); i < (size_t)c->n_dil; i++) {
        c->ks[i] = kdefault;
    }
    for (i = 0; i < (size_t)c->n_dil; i++) {
        if (c->ks[i] < 1) {
            return fail("layer %u has no kernel size", (unsigned)i);
        }
    }
    if (c->channels < 1 || c->head_size < 1 || c->input_size < 1 ||
        c->cond_size < 1) {
        return fail("a layer array is missing its shape");
    }

    /* Counted the same way the loader consumes it. */
    *params += c->channels * c->input_size;
    for (i = 0; i < (size_t)c->n_dil; i++) {
        *params += c->channels * c->channels * c->ks[i] + c->channels;
        *params += c->channels * c->cond_size;
        *params += c->channels * c->channels + c->channels;
    }
    *params += c->head_size * c->channels *
               (c->head_kernel > 1 ? c->head_kernel : 1);
    if (c->head_bias) {
        *params += c->head_size;
    }
    return 0;
}

static char *read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long  n;

    if (f == NULL) {
        (void)fail("cannot open %s", path);
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    n = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        (void)fclose(f);
        (void)fail("%s is empty", path);
        return NULL;
    }
    b = (char *)malloc((size_t)n + 1u);
    if (b == NULL || fread(b, 1, (size_t)n, f) != (size_t)n) {
        (void)fclose(f);
        free(b);
        (void)fail("cannot read %s", path);
        return NULL;
    }
    (void)fclose(f);
    b[n] = 0;
    *len = n;
    return b;
}

/* The submodel a container should be played at: the one with the parameter count
 * asked for, the last otherwise - which is the largest, and is what the format
 * puts there. */
static int pick_submodel(const char *s, int subs, int want, int verbose)
{
    int last = -1, last_i = -1, hit = -1, hit_i = -1, i, n = 0;

    for (i = j_first(s, subs); i >= 0; i = j_next(s, i)) {
        const int model = j_get(s, i, "model");
        if (want > 0 && j_count(s, j_get(s, model, "weights")) == want) {
            hit = i;
            hit_i = n;
        }
        last = i;
        last_i = n;
        n++;
    }
    if (hit < 0) {
        hit = last;
        hit_i = last_i;
    }
    if (hit >= 0 && verbose) {
        printf("  container: %d submodels, using #%d (max_value %g,"
               " %d weights)\n", n, hit_i,
               j_num(s, j_get(s, hit, "max_value")),
               j_count(s, j_get(s, j_get(s, hit, "model"), "weights")));
    }
    return hit;
}

nam_model_t *nam_load(const char *path, int want_weights, int verbose)
{
    long         len = 0;
    char        *buf;
    nam_model_t *m = NULL;
    array_cfg_t  cfg[MAX_ARRAYS];
    float       *weights = NULL;
    wstream_t    ws;
    int          model = -1, arch, layers, e, i;
    int          n_arrays = 0, params = 1, have = 0, rate = 0;

    g_err[0] = 0;
    buf = read_file(path, &len);
    if (buf == NULL) {
        return NULL;
    }

    arch = j_get(buf, 0, "architecture");
    if (j_str_is(buf, arch, "WaveNet")) {
        model = 0;
    } else if (j_str_is(buf, arch, "SlimmableContainer")) {
        const int subs = j_get(buf, j_get(buf, 0, "config"), "submodels");
        const int sub = pick_submodel(buf, subs, want_weights, verbose);
        if (sub < 0) {
            (void)fail("the container holds no submodels");
            goto done;
        }
        model = j_get(buf, sub, "model");
    } else {
        (void)fail("architecture %.30s is not supported", arch < 0 ? "(none)"
                                                                  : buf + arch);
        goto done;
    }

    if (!j_str_is(buf, j_get(buf, model, "architecture"), "WaveNet")) {
        (void)fail("the submodel is not a WaveNet");
        goto done;
    }
    {
        const int cfgo = j_get(buf, model, "config");
        const int head = j_get(buf, cfgo, "head");
        if (head >= 0 && !j_is(buf, head, "null")) {
            (void)fail("a top level head is not implemented here");
            goto done;
        }
        rate = j_int(buf, j_get(buf, model, "sample_rate"));
        layers = j_get(buf, cfgo, "layers");
    }
    for (e = j_first(buf, layers); e >= 0; e = j_next(buf, e)) {
        if (n_arrays >= MAX_ARRAYS) {
            (void)fail("more than %d layer arrays", MAX_ARRAYS);
            goto done;
        }
        if (read_array(buf, e, &cfg[n_arrays], &params) != 0) {
            goto done;
        }
        n_arrays++;
    }
    if (n_arrays < 1) {
        (void)fail("the model has no layer arrays");
        goto done;
    }
    for (i = 1; i < n_arrays; i++) {
        if (cfg[i].input_size != cfg[i - 1].head_size) {
            (void)fail("layer array %d takes %d inputs where the one before it"
                       " has a head of %d", i, cfg[i].input_size,
                       cfg[i - 1].head_size);
            goto done;
        }
    }

    /* The weights, and the check that says the topology above was read right. */
    {
        const int warr = j_get(buf, model, "weights");
        have = j_count(buf, warr);
        if (verbose) {
            printf("  architecture needs %d parameters, file has %d", params,
                   have);
        }
        if (have != params) {
            if (verbose) {
                printf("  -- MISMATCH\n");
            }
            (void)fail("the file has %d parameters and this architecture needs"
                       " %d; it has not been read correctly", have, params);
            goto done;
        }
        if (verbose) {
            printf("  -- matches\n");
        }
        weights = (float *)malloc(sizeof(float) * (size_t)have);
        if (weights == NULL) {
            (void)fail("out of memory for %d weights", have);
            goto done;
        }
        i = 0;
        for (e = j_first(buf, warr); e >= 0; e = j_next(buf, e)) {
            /* strtof rather than strtod and a cast: one rounding step from the
             * decimal in the file to the float the model uses, which is what
             * the reference's JSON reader does. */
            weights[i++] = strtof(buf + e, NULL);
        }
    }

    m = (nam_model_t *)calloc(1, sizeof(*m));
    if (m == NULL) {
        (void)fail("out of memory for the model");
        goto done;
    }
    m->n_arrays = n_arrays;
    m->max_frames = NAM_BLOCK;
    m->rate = rate;
    m->num_frames = -1;
    if (nm_alloc(&m->cond, cfg[0].cond_size, NAM_BLOCK) != 0 ||
        nm_alloc(&m->head[0], cfg[0].channels, NAM_BLOCK) != 0) {
        goto fail_model;
    }
    for (i = 0; i < n_arrays; i++) {
        if (array_init(&m->arrays[i], &cfg[i], NAM_BLOCK) != 0 ||
            nm_alloc(&m->array_out[i], cfg[i].channels, NAM_BLOCK) != 0 ||
            nm_alloc(&m->head[i + 1], cfg[i].head_size, NAM_BLOCK) != 0) {
            goto fail_model;
        }
        if (verbose) {
            printf("  layer array %d: %d channels, %d layers, head %d taps\n", i,
                   cfg[i].channels, cfg[i].n_dil, cfg[i].head_kernel);
        }
    }

    ws.w = weights;
    ws.n = have;
    ws.i = 0;
    ws.over = 0;
    for (i = 0; i < n_arrays; i++) {
        array_params(&m->arrays[i], &ws);
    }
    /* The output scale is the last parameter, not the one in the config - the
     * reference takes it from the stream and so the count above includes it. */
    m->head_scale = w_next(&ws);
    if (ws.over != 0 || ws.i != ws.n) {
        (void)fail("the weights were consumed %d short and %d over", ws.n - ws.i,
                   ws.over);
        goto fail_model;
    }

    m->receptive = 1;
    for (i = 0; i < n_arrays; i++) {
        m->receptive += m->arrays[i].receptive;
    }
    /*
     * Warm up on silence for one receptive field, exactly as the reference does,
     * so that the first real sample sees the history it would have had.  It is
     * not a no-op: the biases give a model an output on a zero input, and the
     * layers' own history of that is part of the state.
     */
    {
        float z = 0.0f, o = 0.0f;
        for (i = 0; i < m->receptive; i++) {
            nam_process(m, &z, &o, 1);
        }
    }
    goto done;

fail_model:
    nam_free(m);
    m = NULL;

done:
    free(weights);
    free(buf);
    return m;
}

void nam_free(nam_model_t *m)
{
    int i, j, k;

    if (m == NULL) {
        return;
    }
    for (i = 0; i < m->n_arrays; i++) {
        array_t  *a = &m->arrays[i];
        /* An array that ran out of memory half way through has its count set and
         * its arrays not, and this is the path a failed load takes out. */
        const int nl = (a->layers != NULL && a->bufs != NULL) ? a->n_layers : 0;
        for (j = 0; j < nl; j++) {
            layer_t *l = &a->layers[j];
            for (k = 0; k < l->front.k; k++) {
                free(l->front.w[k].d);
            }
            free(l->front.w);
            free(l->front.bias);
            free(l->mixin.w.d);
            free(l->post.w.d);
            free(l->post.bias);
            free(l->state.d);
            free(l->tmp.d);
            free(a->bufs[j].d);
        }
        free(a->layers);
        free(a->bufs);
        free(a->rechannel.w.d);
        if (a->head_kernel > 0) {
            for (k = 0; k < a->head_conv.k; k++) {
                free(a->head_conv.w[k].d);
            }
            free(a->head_conv.w);
            free(a->head_conv.bias);
            free(a->head_buf.d);
        } else {
            free(a->head_rechannel.w.d);
            free(a->head_rechannel.bias);
        }
        free(m->array_out[i].d);
        free(m->head[i + 1].d);
    }
    free(m->head[0].d);
    free(m->cond.d);
    free(m);
}
