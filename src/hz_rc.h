/* ===========================================================================
 * HYDRA: binary range coder.
 *
 * A 32 bit low / 32 bit range carry-propagating coder driven by 16 bit
 * probabilities.  One byte is shifted out per renormalisation step.
 *
 * Encoder carry handling uses the cache + pending-0xFF scheme: instead of
 * propagating a carry backwards through the output we keep the last emitted
 * byte in `cache` and count how many 0xFF bytes are queued behind it.  When a
 * carry finally appears the whole run flips in one pass.  That keeps the
 * encoder branch-light and, more importantly, keeps it exactly reversible.
 *
 * The coder is symmetric by construction: encode_bit and decode_bit perform
 * the identical range split, so any probability sequence the encoder used is
 * reproduced by the decoder as long as the model updates match.
 * ========================================================================= */
#ifndef HZ_RC_H
#define HZ_RC_H

#include "hz_int.h"

#define HZ_RC_TOP    (1u << 24)
#define HZ_RC_PBITS  16               /* probabilities are Q16 */

/* ---- encoder ------------------------------------------------------------ */
typedef struct {
    uint64_t low;
    uint32_t range;
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    uint8_t  cache;
    int64_t  pending;    /* number of queued 0xFF bytes, -1 = nothing yet */
    int      overflow;
} hz_enc;

HZ_INLINE void hz_enc_init(hz_enc *e, uint8_t *buf, size_t cap)
{
    e->low = 0; e->range = 0xFFFFFFFFu;
    e->buf = buf; e->cap = cap; e->pos = 0;
    /* `pending` counts the bytes held back: one `cache` byte plus
     * (pending - 1) queued 0xFF bytes.  Starting at 1 with cache = 0 means
     * the stream opens with a single zero byte that the decoder discards
     * while priming; that byte is what a carry out of the first symbol
     * would land in, which is exactly why it has to exist. */
    e->cache = 0; e->pending = 1; e->overflow = 0;
}

HZ_INLINE void hz_enc_shift_low(hz_enc *e)
{
    uint32_t hi = (uint32_t)(e->low >> 32);      /* the carry, 0 or 1 */
    /* Flush only once the pending run can no longer absorb a carry, i.e.
     * either a carry just arrived (hi = 1) or the new top byte is not 0xFF. */
    if (hi != 0 || e->low < 0xFF000000ull) {
        uint8_t b = e->cache;
        do {
            if (e->pos < e->cap) e->buf[e->pos] = (uint8_t)(b + hi);
            ++e->pos;
            b = 0xFF;                            /* the queued run */
        } while (--e->pending);
        e->cache = (uint8_t)(e->low >> 24);
    }
    ++e->pending;
    e->low = (e->low << 8) & 0xFFFFFFFFull;
    if (e->pos > e->cap) e->overflow = 1;
}

/* Encode `bit` whose probability of being 1 is p1 (Q16, 1..65535). */
HZ_INLINE void hz_enc_bit(hz_enc *e, int p1, int bit)
{
    uint32_t bound = (uint32_t)(((uint64_t)e->range * (uint32_t)p1) >> HZ_RC_PBITS);
    /* Guard the split so neither side can collapse to a zero width range. */
    if (bound == 0) bound = 1;
    else if (bound >= e->range) bound = e->range - 1;

    if (bit) {
        e->range = bound;
    } else {
        e->low  += bound;
        e->range -= bound;
    }
    while (e->range < HZ_RC_TOP) {
        e->range <<= 8;
        hz_enc_shift_low(e);
    }
}

/* Encode a raw bit with probability 1/2 (used for escapes and headers). */
HZ_INLINE void hz_enc_bit_raw(hz_enc *e, int bit)
{
    e->range >>= 1;
    if (!bit) e->low += e->range;
    while (e->range < HZ_RC_TOP) {
        e->range <<= 8;
        hz_enc_shift_low(e);
    }
}

HZ_INLINE size_t hz_enc_flush(hz_enc *e)
{
    int i;
    for (i = 0; i < 5; ++i) hz_enc_shift_low(e);
    if (e->pos > e->cap) e->overflow = 1;
    return e->pos;
}

/* ---- decoder ------------------------------------------------------------ */
typedef struct {
    uint32_t range;
    uint32_t code;
    const uint8_t *buf;
    size_t   size;
    size_t   pos;
} hz_dec;

HZ_INLINE uint8_t hz_dec_byte(hz_dec *d)
{
    return d->pos < d->size ? d->buf[d->pos++] : (uint8_t)0;
}

HZ_INLINE void hz_dec_init(hz_dec *d, const uint8_t *buf, size_t size)
{
    int i;
    d->buf = buf; d->size = size; d->pos = 0;
    d->range = 0xFFFFFFFFu; d->code = 0;
    hz_dec_byte(d);                       /* the encoder's priming byte */
    for (i = 0; i < 4; ++i) d->code = (d->code << 8) | hz_dec_byte(d);
}

HZ_INLINE int hz_dec_bit(hz_dec *d, int p1)
{
    uint32_t bound = (uint32_t)(((uint64_t)d->range * (uint32_t)p1) >> HZ_RC_PBITS);
    int bit;
    if (bound == 0) bound = 1;
    else if (bound >= d->range) bound = d->range - 1;

    if (d->code < bound) {
        d->range = bound;
        bit = 1;
    } else {
        d->code  -= bound;
        d->range -= bound;
        bit = 0;
    }
    while (d->range < HZ_RC_TOP) {
        d->range <<= 8;
        d->code = (d->code << 8) | hz_dec_byte(d);
    }
    return bit;
}

HZ_INLINE int hz_dec_bit_raw(hz_dec *d)
{
    int bit;
    d->range >>= 1;
    if (d->code < d->range) {
        bit = 1;
    } else {
        d->code -= d->range;
        bit = 0;
    }
    while (d->range < HZ_RC_TOP) {
        d->range <<= 8;
        d->code = (d->code << 8) | hz_dec_byte(d);
    }
    return bit;
}

#endif /* HZ_RC_H */
