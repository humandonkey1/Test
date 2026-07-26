/* ===========================================================================
 * HYDRA: mixer / APM allocation and the bit-history state machine.
 * ========================================================================= */
#include "hz_model.h"

int hz_mixer_init(hz_mixer *m, int nin, int ngate, int lrate)
{
    size_t n = (size_t)nin * (size_t)ngate;
    size_t i;
    if (nin <= 0 || nin > 64 || ngate <= 0) return -1;
    m->w = (int32_t *)malloc(n * sizeof(int32_t));
    if (!m->w) return -1;
    /* Start every weight at 1/nin so the initial prediction is the plain
     * average of the inputs rather than an arbitrary one. */
    for (i = 0; i < n; ++i) m->w[i] = (int32_t)(65536 / nin);
    m->nin = nin; m->ngate = ngate; m->lrate = lrate;
    m->nact = 0; m->wrow = m->w; m->pr = 32768;
    return 0;
}

void hz_mixer_free(hz_mixer *m)
{
    free(m->w); m->w = NULL;
}

int hz_apm_init(hz_apm *a, int nctx, int rate)
{
    int c, i;
    a->t = (uint16_t *)malloc((size_t)nctx * 33 * sizeof(uint16_t));
    if (!a->t) return -1;
    /* Identity initialisation: bucket i maps back to the probability that
     * produced it, so a fresh APM is a no-op and can only improve. */
    for (c = 0; c < nctx; ++c)
        for (i = 0; i < 33; ++i)
            a->t[(size_t)c * 33 + i] = (uint16_t)hz_squash((i - 16) * 256);
    a->nctx = nctx; a->rate = rate; a->idx = 0; a->w = 0;
    return 0;
}

void hz_apm_free(hz_apm *a)
{
    free(a->t); a->t = NULL;
}

/* ---------------------------------------------------------------------------
 * Bit history states.
 *
 * A state packs (n0, n1, last-bit-run) into one byte.  Transitions cap the
 * opposing count when a context turns decisive, which is the mechanism that
 * makes the sketch react fast to a change of regime instead of being dragged
 * by ancient statistics:
 *
 *   - increment the count for the observed bit
 *   - if the opposite count is large, halve it (bounded discounting)
 *
 * The table is generated once at startup by enumerating the reachable
 * (n0, n1) pairs under those rules and assigning each a slot.
 * ------------------------------------------------------------------------ */
uint8_t hz_state_next[256][2];

static int hz_states_ready = 0;

#define HZ_NMAX 20

static int8_t  st_n0[256], st_n1[256];
static int     st_count;
static int16_t st_index[HZ_NMAX + 1][HZ_NMAX + 1];

static int state_lookup(int n0, int n1);

/* bounded discount: a run of one symbol may not leave more than a small
 * residue of the other, otherwise the model stops adapting */
static void discount(int *a, int b)
{
    if (b < 2) return;
    if (*a > 6) *a = 6 + ((*a - 6) >> 1);
    if (b > 4 && *a > 4) *a = 4;
    if (b > 8 && *a > 3) *a = 3;
    if (b > 12 && *a > 2) *a = 2;
}

static int state_alloc(int n0, int n1)
{
    if (n0 > HZ_NMAX) n0 = HZ_NMAX;
    if (n1 > HZ_NMAX) n1 = HZ_NMAX;
    if (st_index[n0][n1] >= 0) return st_index[n0][n1];
    if (st_count >= 256) {
        /* Out of slots: fall back to the nearest already allocated pair. */
        int bi = 0, bd = 1 << 30, i;
        for (i = 0; i < st_count; ++i) {
            int d = (st_n0[i] - n0) * (st_n0[i] - n0) +
                    (st_n1[i] - n1) * (st_n1[i] - n1);
            if (d < bd) { bd = d; bi = i; }
        }
        return bi;
    }
    st_index[n0][n1] = (int16_t)st_count;
    st_n0[st_count] = (int8_t)n0;
    st_n1[st_count] = (int8_t)n1;
    return st_count++;
}

static int state_lookup(int n0, int n1)
{
    if (n0 > HZ_NMAX) n0 = HZ_NMAX;
    if (n1 > HZ_NMAX) n1 = HZ_NMAX;
    if (st_index[n0][n1] >= 0) return st_index[n0][n1];
    return state_alloc(n0, n1);
}

void hz_states_init(void)
{
    int i, j, s, changed, pass;
    if (hz_states_ready) return;

    for (i = 0; i <= HZ_NMAX; ++i)
        for (j = 0; j <= HZ_NMAX; ++j)
            st_index[i][j] = -1;
    st_count = 0;

    /* Breadth-first from the empty state so the low numbered, most common
     * states end up densely packed at the front of the table. */
    state_alloc(0, 0);
    for (pass = 0; pass < 64; ++pass) {
        changed = 0;
        s = st_count;
        for (i = 0; i < s; ++i) {
            int a, b;
            a = st_n0[i]; b = st_n1[i];
            {   int na = a + 1, nb = b; discount(&nb, na);
                if (st_index[na > HZ_NMAX ? HZ_NMAX : na][nb] < 0) {
                    state_alloc(na, nb); changed = 1; } }
            {   int na = a, nb = b + 1; discount(&na, nb);
                if (st_index[na][nb > HZ_NMAX ? HZ_NMAX : nb] < 0) {
                    state_alloc(na, nb); changed = 1; } }
        }
        if (!changed) break;
    }

    for (i = 0; i < 256; ++i) {
        int a = i < st_count ? st_n0[i] : 0;
        int b = i < st_count ? st_n1[i] : 0;
        int na, nb;
        na = a + 1; nb = b; discount(&nb, na);
        hz_state_next[i][0] = (uint8_t)state_lookup(na, nb);
        na = a; nb = b + 1; discount(&na, nb);
        hz_state_next[i][1] = (uint8_t)state_lookup(na, nb);
    }

    hz_states_ready = 1;
}
