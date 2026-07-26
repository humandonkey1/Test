/* ===========================================================================
 * HYDRA: the adaptive primitives that sit between the contexts and the coder.
 *
 *   hz_ctr      - a 16 bit probability with an adaptive learning rate
 *   hz_bitmap   - a byte-indexed state map feeding a probability + count
 *   hz_mixer    - a gated logistic mixer, integer only
 *   hz_apm      - a two dimensional adaptive probability map (SSE stage)
 *
 * Every update rule here is a pure integer function of state that both the
 * encoder and decoder possess before the bit is coded, which is what keeps
 * the two sides locked together.
 * ========================================================================= */
#ifndef HZ_MODEL_H
#define HZ_MODEL_H

#include "hz_int.h"

/* ---- adaptive bit counter ----------------------------------------------
 * Packed into 32 bits: probability in the high 22, a visit count in the low
 * 10.  Early on the estimate moves at 1/(n+2) which is the optimal running
 * mean; once the count saturates it settles into a fixed exponential window
 * so the model can still track drift.                                     */
typedef uint32_t hz_ctr;

#define HZ_CTR_INIT      (2048u << 10)     /* p = 0.5, n = 0 */
#define HZ_CTR_CNTMAX    1023u
#define HZ_CTR_P(c)      ((int)((c) >> 10))          /* Q22 -> Q12 base */
#define HZ_CTR_N(c)      ((int)((c) & HZ_CTR_CNTMAX))

/* probability in Q16 for the coder */
HZ_INLINE int hz_ctr_p16(hz_ctr c) {
    int p = (int)(c >> 10);                  /* Q22 value, 0..4194303 */
    p >>= 6;                                 /* -> Q16 */
    return hz_clampi(p, 1, 65535);
}

HZ_INLINE void hz_ctr_update(hz_ctr *c, int bit, int limit)
{
    uint32_t v = *c;
    int      n = (int)(v & HZ_CTR_CNTMAX);
    int32_t  p = (int32_t)(v >> 10);          /* Q22 */
    int32_t  target = bit ? (int32_t)4194303 : 0;
    int32_t  d;
    int      rate;

    if (n < limit) ++n;
    rate = (int)hz_rate_tab[n < 255 ? n : 255];    /* ~65536/(n+2) */

    /* Round the step away from zero.  With plain truncation the product
     * (target - p) * rate underflows once the estimate gets close to the
     * extreme, so the counter freezes several hundred units short of it and
     * puts a hard floor -- around 1e-4 bits per bit -- under the output on
     * perfectly predictable data.  Rounding up costs nothing and lets a
     * genuinely certain context reach genuine certainty. */
    d = target - p;
    if (d > 0)      p += (int32_t)((((int64_t)d * rate) + 65535) >> 16);
    else if (d < 0) p -= (int32_t)(((((int64_t)(-d) * rate) + 65535) >> 16));

    if (p < 0) p = 0;
    if (p > 4194303) p = 4194303;
    *c = ((uint32_t)p << 10) | (uint32_t)n;
}

/* ---- gated logistic mixer ----------------------------------------------
 * p = squash( sum_i w_i * stretch(p_i) )
 *
 * Weights live in Q16 and are trained by online gradient descent on coding
 * loss; for the logistic link the gradient reduces to (bit - p) * input,
 * which needs no division.  A separate weight vector is selected per gate
 * value, so the mixer specialises by context (order-0 byte state, match
 * state, and so on) instead of averaging over everything at once.        */
typedef struct {
    int32_t *w;         /* [ngate][nin] Q16 */
    int      nin;
    int      ngate;
    int      lrate;
    /* scratch for the current prediction */
    int32_t  st[64];    /* stretched inputs */
    int      nact;
    int32_t *wrow;
    int      pr;        /* last prediction, Q16 */
} hz_mixer;

int  hz_mixer_init(hz_mixer *m, int nin, int ngate, int lrate);
void hz_mixer_free(hz_mixer *m);

HZ_INLINE void hz_mixer_begin(hz_mixer *m, int gate)
{
    m->nact = 0;
    m->wrow = m->w + (size_t)gate * m->nin;
}

/* add an input given as a Q16 probability */
HZ_INLINE void hz_mixer_add(hz_mixer *m, int p16)
{
    m->st[m->nact++] = hz_stretch(p16);
}

/* add an input already in the logit domain */
HZ_INLINE void hz_mixer_add_st(hz_mixer *m, int s)
{
    m->st[m->nact++] = hz_clampi(s, -HZ_ST_MAX, HZ_ST_MAX);
}

HZ_INLINE int hz_mixer_predict(hz_mixer *m)
{
    int64_t sum = 0;
    int i;
    for (i = 0; i < m->nact; ++i)
        sum += (int64_t)m->st[i] * m->wrow[i];
    sum >>= 16;
    /* Clamp to the full logit domain, not half of it: on highly predictable
     * data the mixer legitimately wants to output near-certainty, and
     * truncating here would put a floor under the compressed size. */
    m->pr = hz_squash(hz_clampi((int)sum, -HZ_ST_MAX, HZ_ST_MAX));
    return m->pr;
}

HZ_INLINE void hz_mixer_update(hz_mixer *m, int bit)
{
    /* err is the coding error in Q16 probability units */
    int err = ((bit << 16) - m->pr);
    int i;
    int scaled = (err * m->lrate) >> 10;
    for (i = 0; i < m->nact; ++i) {
        m->wrow[i] += (int32_t)(((int64_t)m->st[i] * scaled) >> 14);
        /* keep weights in a range where the Q16 dot product cannot overflow */
        if (m->wrow[i] >  (1 << 22)) m->wrow[i] =  (1 << 22);
        if (m->wrow[i] < -(1 << 22)) m->wrow[i] = -(1 << 22);
    }
}

/* ---- adaptive probability map (SSE) ------------------------------------
 * Refines an incoming probability given a small context.  The logit axis is
 * split into 33 buckets and the two straddling entries are interpolated,
 * then only those two are trained.  This is what lets the model correct a
 * systematic bias that the mixer alone cannot express.                    */
typedef struct {
    uint16_t *t;        /* [nctx][33] Q16 */
    int       nctx;
    int       rate;
    int       idx;      /* last touched pair */
    int       w;        /* interpolation weight of the upper entry */
} hz_apm;

int  hz_apm_init(hz_apm *a, int nctx, int rate);
void hz_apm_free(hz_apm *a);

HZ_INLINE int hz_apm_pp(hz_apm *a, int p16, int ctx)
{
    int s  = hz_stretch(p16) + 4096;         /* 1..8191 */
    int lo = s >> 8;                         /* 0..31   */
    int wt = s & 255;
    uint16_t *r = a->t + (size_t)ctx * 33 + lo;
    a->idx = (int)(r - a->t);
    a->w   = wt;
    return (r[0] * (256 - wt) + r[1] * wt) >> 8;
}

/* Move one entry toward the target, rounding away from zero so that the
 * step never underflows to nothing near the extremes. */
HZ_INLINE int hz_apm_step(int cur, int target, int w, int g)
{
    int d = ((target - cur) * w) >> 8;
    if (d > 0)      cur += (d + (1 << g) - 1) >> g;
    else if (d < 0) cur -= ((-d) + (1 << g) - 1) >> g;
    return hz_clampi(cur, 0, 65535);
}

HZ_INLINE void hz_apm_update(hz_apm *a, int bit)
{
    int target = bit ? 65535 : 0;
    uint16_t *r = a->t + a->idx;
    int g = a->rate;
    r[0] = (uint16_t)hz_apm_step((int)r[0], target, 256 - a->w, g);
    r[1] = (uint16_t)hz_apm_step((int)r[1], target, a->w,       g);
}

/* ---- indirect bit history ----------------------------------------------
 * A compact 8 bit sketch of what a context has seen: the last few bits plus
 * saturating counts of zeros and ones.  Feeding the *history* into a learned
 * table, rather than a raw frequency, lets the model distinguish "steady
 * run of ones" from "noisy but ones-leaning", which behave very differently.
 * ------------------------------------------------------------------------ */
extern uint8_t hz_state_next[256][2];
void hz_states_init(void);

#endif /* HZ_MODEL_H */
