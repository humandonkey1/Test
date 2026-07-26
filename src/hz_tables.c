/* ===========================================================================
 * HYDRA: startup tables.
 *
 * squash / stretch / adaptation rates, all derived from the integer exp2 in
 * hz_int.h so that the result is bit identical on every platform.
 *
 * The two curves are built as an exact pair: stretch is defined as the
 * nearest inverse of the squash table that actually ships, never of the
 * ideal real valued function.  Any disagreement between them would show up
 * as a systematic bias the mixer cannot correct.
 * ========================================================================= */
#include "hz_int.h"

uint16_t hz_squash_tab[HZ_ST_SIZE];
int16_t  hz_stretch_tab[4096];
int16_t  hz_stretch_lo[HZ_TAIL];
int16_t  hz_stretch_hi[HZ_TAIL];
uint16_t hz_rate_tab[256];

static int hz_tables_ready = 0;

/* squash(x) = 65536 / (1 + e^(-x/256)), evaluated in fixed point.
 * Symmetry is imposed explicitly so squash(-x) == 65536 - squash(x). */
static uint32_t squash_raw(int x)
{
    uint64_t e, r;
    uint32_t arg;
    int neg = x < 0;

    if (neg) x = -x;
    if (x > HZ_ST_MAX) x = HZ_ST_MAX;

    arg = (uint32_t)(((uint64_t)(uint32_t)x * HZ_EXP_SCALE_NUM) >> 12); /* Q16.16 */
    e   = hz_fx_exp2(arg);                                              /* Q16.16 */

    /* p = e / (1 + e) in Q16 */
    r = (uint64_t)((e << 16) / (e + 65536u));
    if (r < 1) r = 1;
    if (r > 65535u) r = 65535u;
    if (neg) r = 65536u - r;
    if (r < 1) r = 1;
    if (r > 65535u) r = 65535u;
    return (uint32_t)r;
}

/* Nearest x (monotone sweep) such that hz_squash_tab[x] is closest to p. */
static int invert_nearest(int p, int *cursor)
{
    int x = *cursor, best_x, best_d;
    while (x < HZ_ST_MAX && (int)hz_squash_tab[x + 1 + 4096] <= p) ++x;
    best_x = x;
    best_d = abs((int)hz_squash_tab[x + 4096] - p);
    if (x < HZ_ST_MAX && abs((int)hz_squash_tab[x + 1 + 4096] - p) < best_d)
        best_x = x + 1;
    *cursor = x;
    return best_x;
}

void hz_tables_init(void)
{
    int i, x, cursor;

    if (hz_tables_ready) return;

    for (x = -4096; x < 4096; ++x)
        hz_squash_tab[x + 4096] = (uint16_t)squash_raw(x);

    /* coarse body: bucket i covers p in [i*16, i*16+15], bind to midpoint */
    cursor = -HZ_ST_MAX;
    for (i = 0; i < 4096; ++i)
        hz_stretch_tab[i] = (int16_t)invert_nearest(i * 16 + 8, &cursor);

    /* full resolution tails */
    cursor = -HZ_ST_MAX;
    for (i = 0; i < HZ_TAIL; ++i)
        hz_stretch_lo[i] = (int16_t)invert_nearest(i, &cursor);

    cursor = -HZ_ST_MAX;
    for (i = 0; i < HZ_TAIL; ++i)
        hz_stretch_hi[i] = (int16_t)invert_nearest(65536 - HZ_TAIL + i, &cursor);

    /* 65536/(n+2): the running mean of a stationary estimator, with a floor
     * so a saturated counter never stops tracking drift entirely. */
    for (i = 0; i < 256; ++i) {
        uint32_t r = 65536u / (uint32_t)(i + 2);
        if (r < 24) r = 24;
        hz_rate_tab[i] = (uint16_t)r;
    }

    hz_tables_ready = 1;
}
