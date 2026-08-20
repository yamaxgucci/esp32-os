/*
 * nam - playing a NAM/TONE3000 capture, in this tree, in C.
 *
 * WHY THIS EXISTS
 *
 * Every fitted voicing in apps/common/tube was measured against a capture of a
 * real amplifier, and the only thing on this machine that could play one was a
 * patched build of a Go project sitting in a session scratchpad under %TEMP%.
 * That is the single most losable dependency in the whole exercise: a disk
 * cleanup takes it, and with it the ability to check any tone match ever again.
 * There is no Torch here and no NAM plugin.
 *
 * So the inference is in the tree.  It is a straight port of that fork - the
 * upstream realtime WaveNet plus the four things newer captures need (LeakyReLU,
 * per-layer kernel sizes, a convolutional head, the TONE3000 container) - and
 * "straight" is meant literally: the loops are in the same order and the
 * accumulators are the same width, because the test is that a render comes back
 * **bit for bit** identical to one the Go binary made.  A port that is merely
 * close is not a reference; it is a second opinion, and then there is no way to
 * tell which of the two is the amplifier.
 *
 * WHAT IT REFUSES
 *
 * The schema in these files carries FiLM conditioning, grouped convolutions,
 * gating, secondary activations and a top-level head.  All of them are switched
 * off in every capture seen here, and every one of them is *checked* rather than
 * ignored, because a model that loads with the wrong topology does not fail - it
 * plays something that is not the amplifier it claims to be, confidently.
 *
 * The other check is arithmetic: the architecture implies exactly how many
 * numbers it needs and the file says how many it has.  If they differ by one,
 * something has been misread.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The algorithm and the parameter layout are those of Neural Amp Modeler and of
 * nlpodyssey/waveny, both Apache-2.0.  See LICENSING.md, "Ported code, and where
 * it came from".
 */
#ifndef NAM_H
#define NAM_H

typedef struct nam_model nam_model_t;

/*
 * Load a .nam.  `want_weights` picks a submodel out of a container by its
 * parameter count, 0 for the largest one, which is what the format's "slimmable"
 * containers put last.  `verbose` prints the container, the parameter check and
 * the layer shapes - the three lines that are the difference between playing the
 * amplifier and playing something else.
 *
 * NULL on failure, with nam_err() saying why.
 */
nam_model_t *nam_load(const char *path, int want_weights, int verbose);
void         nam_free(nam_model_t *m);

/*
 * One block: n input samples in, n output samples out, state carried across
 * calls.  Blocks may be any length up to what nam_load prepared for, and the
 * block length is part of the arithmetic - the reference renders were made in
 * 4096-sample blocks and a different block size gives a slightly different
 * answer, because the model's own history buffers are rewound at different
 * moments.  NAM_BLOCK is that block size.
 */
#define NAM_BLOCK 4096
void nam_process(nam_model_t *m, const float *in, float *out, int n);

/* How many samples of history the model needs before its output means anything.
 * nam_load has already run that many zeros through it, as the reference does. */
int nam_receptive_field(const nam_model_t *m);

/* The sample rate the capture declares, 0 if it does not say.  Captures are
 * 48 kHz and everything in this tree is 22.05 or 44.1, so something has to
 * resample - see tools/wavrate.c. */
int nam_sample_rate(const nam_model_t *m);

const char *nam_err(void);

#endif /* NAM_H */
