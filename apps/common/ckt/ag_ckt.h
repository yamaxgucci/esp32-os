/*
 * ag_ckt - nonlinear analogue circuit solver, one audio sample at a time.
 *
 * This is the real thing and not a waveshaper: a netlist of resistors,
 * capacitors, sources and nonlinear devices is assembled into a modified nodal
 * analysis matrix, capacitors are replaced by their trapezoidal companion model
 * at the sample rate, and every sample is solved by Newton-Raphson until the
 * node voltages stop moving.  It is what SPICE does, with the transient
 * timestep nailed to 1/fs and the tolerances loosened to what audio can hear.
 *
 * The point of writing it this way is that the answer to "how many components
 * fit" then comes out of the structure rather than out of an opinion:
 *
 *   - a resistor is stamped once into the constant matrix and costs nothing
 *     per sample, so its count is unlimited;
 *   - a capacitor adds an unknown, and the solve is O(n^3) in the number of
 *     unknowns, so capacitors get more expensive as they are added;
 *   - a diode, JFET or triode has to be re-linearised on every Newton
 *     iteration of every sample, and only the triode needs transcendental
 *     arithmetic to do it.
 *
 * Node 0 is ground.  Nodes are numbered by the caller and need not be dense.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_CKT_H
#define AG_CKT_H

#include <stdint.h>

/*
 * Sized for a six-stage preamp with a tone stack: three nodes per valve stage
 * plus the input and the supply rail.  The two n x n matrices are the whole
 * memory cost of the solver - 36 x 36 floats twice is 10 KB, which is nothing
 * next to the PSRAM an application has, and the O(n^3) solve is what stops
 * this from being raised further.
 */
#define AG_CKT_MAX_NODES 32 /* excluding ground                            */
#define AG_CKT_MAX_UNK   36 /* nodes + voltage sources                     */
#define AG_CKT_MAX_R     96
#define AG_CKT_MAX_C     32
#define AG_CKT_MAX_V     4
#define AG_CKT_MAX_NL    12

/*
 * Where between the two ends of the step the capacitors are carried.
 *
 * A half is the trapezoidal rule: second order accurate, and its companion
 * recursion has its pole at exactly z = -1 - on the unit circle, at Nyquist,
 * marginally stable by construction.  One is backward Euler: no ringing and
 * a badly damped top end.
 *
 * In a linear network the trapezoidal pole is harmless, because nothing
 * listens at Nyquist.  Through a valve that is clipping it is not: the
 * nonlinearity mixes whatever sits there straight back down into the audio
 * band.  Measured on a guitar take, content at half the oversampled rate was
 * -85 dB out of the nearly-linear first stage and -50 dB out of the clipping
 * second one, and oversampling did not help - the pole just moves to the new
 * Nyquist.
 *
 * A little over a half puts the pole at -(1-theta)/theta, inside the circle.
 * 0.55 gives -0.818, which dies away in about ten samples.  It costs no
 * instructions at all, only different constants.
 *
 * The baked model in ag_stage.c carries the same recursion and must use the
 * same value, which is why this lives here rather than in one .c file.
 */
#ifndef AG_CKT_THETA
#define AG_CKT_THETA 0.55f
#endif
#define AG_CKT_THETA_C ((1.0f - AG_CKT_THETA) / AG_CKT_THETA)

/* Koren's 12AX7-style triode.  Defaults are the widely used 12AX7 fit. */
typedef struct ag_triode_model {
    float mu;  /* amplification factor            (100)   */
    float ex;  /* exponent                        (1.4)   */
    float kg1; /* plate current scale             (1060)  */
    float kp;  /* knee sharpness                  (600)   */
    float kvb; /* plate-voltage knee              (300)   */
    float rgk; /* grid conduction resistance      (2000)  */
    float vg0; /* grid conduction knee, volts     (0.33)  */
} ag_triode_model_t;

/* Shichman-Hodges N-channel JFET: the cheap nonlinearity, no exp anywhere. */
typedef struct ag_jfet_model {
    float beta;   /* transconductance parameter, A/V^2  */
    float vto;    /* pinch-off, negative volts          */
    float lambda; /* channel-length modulation, 1/V     */
} ag_jfet_model_t;

void ag_triode_model_12ax7(ag_triode_model_t *m);
void ag_jfet_model_j201(ag_jfet_model_t *m);

/*
 * The valve, on its own, so that its derivatives can be checked against finite
 * differences without a circuit around them.  A Jacobian that disagrees with
 * its own function does not make Newton fail loudly - it makes it oscillate
 * around the answer with an amplitude set by the size of the disagreement,
 * which reads as "the solver is not quite converging" and sends you looking at
 * the solver.
 *
 * ip / ig are the plate and grid currents; the four derivatives are their
 * partials with respect to the two controlling voltages.
 */
typedef struct ag_triode_op {
    float ip, dip_dvgk, dip_dvpk;
    float ig, dig_dvgk;
} ag_triode_op_t;

void ag_triode_eval(const ag_triode_model_t *m, float vgk, float vpk,
                    ag_triode_op_t *out);


enum {
    AG_NL_DIODE_PAIR = 0, /* antiparallel pair, the clipper in every pedal */
    AG_NL_TRIODE,
    AG_NL_JFET
};

typedef struct ag_ckt_nl {
    uint8_t kind;
    uint8_t a, b, c; /* diode: a,b.  triode: g,p,k.  jfet: g,d,s.          */
    float   p0, p1;  /* diode: Is, n*Vt                                     */
    union {
        ag_triode_model_t triode;
        ag_jfet_model_t   jfet;
    } m;
} ag_ckt_nl_t;

/*
 * Every device above behind one two-port interface, so that code which does
 * not care which device it has - the table baker in ag_stage.c - does not have
 * to know.  Ports are (a,c) and (b,c) in the node numbering above:
 *
 *   triode: v1 = Vgk, v2 = Vpk, i1 = grid current, i2 = plate current
 *   JFET:   v1 = Vgs, v2 = Vds, i1 = 0,            i2 = drain current
 *   diodes: v1 = across the pair, v2 unused,       i2 = 0
 *
 * j is the 2x2 Jacobian in row-major order (di1/dv1, di1/dv2, di2/dv1,
 * di2/dv2); pass NULL if the derivatives are not wanted.
 */
void ag_ckt_dev_eval(const ag_ckt_nl_t *d, float v1, float v2, float *i1,
                     float *i2, float *j);
/* 1 for a diode pair, 2 for anything with a control terminal. */
int ag_ckt_dev_ports(const ag_ckt_nl_t *d);

typedef struct ag_ckt {
    float fs;
    int   n;      /* unknowns in use: nodes + sources                      */
    int   nnodes; /* highest node number used                              */

    /* netlist */
    struct {
        uint8_t a, b;
        float   g; /* conductance, 1/R                                     */
    } r[AG_CKT_MAX_R];
    int nr;

    struct {
        uint8_t a, b;
        float   geq;  /* 2C/T                                              */
        float   ieq;  /* companion source, updated each sample             */
        float   vpre; /* voltage last sample                               */
        float   ipre; /* current last sample                               */
    } c[AG_CKT_MAX_C];
    int nc;

    struct {
        uint8_t a, b;
        uint8_t driven; /* 1 = value comes from the input each sample      */
        float   v;
    } v[AG_CKT_MAX_V];
    int nv;

    ag_ckt_nl_t nl[AG_CKT_MAX_NL];
    int         nnl;

    /* solver state */
    float g0[AG_CKT_MAX_UNK][AG_CKT_MAX_UNK]; /* constant stamp            */
    float rhs0[AG_CKT_MAX_UNK];               /* constant right-hand side  */
    float a[AG_CKT_MAX_UNK][AG_CKT_MAX_UNK];  /* working matrix            */
    float rhs[AG_CKT_MAX_UNK];
    float x[AG_CKT_MAX_UNK]; /* solution, kept as the next warm start      */
    float d[AG_CKT_MAX_UNK]; /* equilibration scales, powers of two        */
    int   perm[AG_CKT_MAX_UNK];

    /*
     * A circuit with no nonlinear device has nothing to iterate on and nothing
     * that changes between samples except the right-hand side, so its matrix
     * is factored once here at build time and every sample is a pair of
     * triangular solves.  That is the difference between O(n^3) and O(n^2) per
     * sample, and for a tone stack - which is all any tone control is - it is
     * the difference between affordable and not.
     */
    uint8_t linear;

    /* what it cost - the bench reads these */
    uint32_t iters_last;
    uint32_t iters_total;
    uint32_t samples;
    uint32_t nonconverged;

    uint8_t built;
} ag_ckt_t;

void ag_ckt_init(ag_ckt_t *k, float fs);

/* All return 0 on success, -1 if a table is full or a node is out of range. */
int ag_ckt_add_r(ag_ckt_t *k, int a, int b, float ohms);
int ag_ckt_add_c(ag_ckt_t *k, int a, int b, float farads);
int ag_ckt_add_vdc(ag_ckt_t *k, int a, int b, float volts);
int ag_ckt_add_vin(ag_ckt_t *k, int a, int b); /* driven by ag_ckt_tick     */
int ag_ckt_add_diode_pair(ag_ckt_t *k, int a, int b, float is, float nvt);
int ag_ckt_add_triode(ag_ckt_t *k, int g, int p, int cath,
                      const ag_triode_model_t *m);
int ag_ckt_add_jfet(ag_ckt_t *k, int g, int d, int s,
                    const ag_jfet_model_t *m);

/* Assemble the constant stamp.  Call once, after the netlist is complete. */
int  ag_ckt_build(ag_ckt_t *k);
void ag_ckt_reset(ag_ckt_t *k);

/* One sample.  Returns the voltage on out_node. */
float ag_ckt_tick(ag_ckt_t *k, float vin, int out_node);

/*
 * The constant matrix on its own - resistors, capacitor companion
 * conductances and sources, with every nonlinear device removed.
 *
 * This is what a fixed-topology model is built out of: with the devices taken
 * out, the circuit is linear, so its response to the input, to the capacitor
 * states and to the device currents can be worked out once and stored as a
 * handful of numbers.  See ag_stage.h.
 *
 * factor_g0 overwrites the working matrix, which for a circuit with devices in
 * it is scratch that ag_ckt_tick refills every iteration - so calling these is
 * safe at any time.  Both return 0 on success.
 */
int  ag_ckt_factor_g0(ag_ckt_t *k);
void ag_ckt_apply_g0(ag_ckt_t *k, const float *rhs, float *x);

/* Where a node's unknown lives in the solution vector, or -1 for ground. */
static inline int ag_ckt_index(int node) { return node > 0 ? node - 1 : -1; }

#endif /* AG_CKT_H */
