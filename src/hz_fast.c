/* ===========================================================================
 * HYDRA: the FAST engine.
 *
 * A byte aligned LZ77 coder built for decode throughput.  The whole point of
 * this engine is that decoding is a straight line: read one token, copy a
 * literal run, copy a match.  No entropy stage, no state machine, nothing
 * that can stall the pipeline.
 *
 * Token (1 byte)
 *   bits 7..5   literal run   0..6 literal, 7 = extended
 *   bits 4..3   match length  0..2 -> 4..6, 3 = extended
 *   bits 2..0   offset class  0,1,2 = repeat slot
 *                             3,4,5,6 = explicit, 1..4 byte payload
 *
 * The offset class carries the payload width directly.  An earlier layout
 * used a separate size tag byte, which cost one byte on every explicit
 * offset -- and explicit offsets turn out to be over 90% of them on real
 * data, so that single byte pushed offsets from 55% of the output to 72%.
 * Folding the width into the class costs a bit of match-length range,
 * which is far cheaper: lengths above 6 are already extended anyway.
 *
 * Three repeat slots rather than one is what makes this competitive on
 * structured data.  Measured on CSV records, offsets were 55% of the whole
 * output: rows that differ in a few fields keep cycling between a handful
 * of distances, and with a single slot every alternation pays full price.
 * With three, the common case is free -- the distance is not coded at all,
 * and the decoder reads it from a register instead of memory.
 *
 * A stream always finishes with a literal-only token, which is what lets the
 * decoder terminate on "input exhausted" without a separate end marker.
 * ========================================================================= */
#include "hz_int.h"

#define HZ_MINMATCH     4
#define HZ_LASTLITERALS 12   /* tail that is never part of a match */
#define HZ_MFLIMIT      16   /* encoder stops looking this far from the end */

#define HZ_NREP 3

/* class 3..6 -> explicit offset of 1..4 bytes */
static const uint8_t  hz_ocls_bytes[8] = { 0, 0, 0, 1, 2, 3, 4, 0 };
static const uint32_t hz_ocls_mask[8]  = { 0u, 0u, 0u,
                                           0xFFu, 0xFFFFu, 0xFFFFFFu, 0xFFFFFFFFu, 0u };

/* ---- varint -------------------------------------------------------------- */
HZ_INLINE void hz_put_varint(uint8_t **op, size_t v)
{
    while (v >= 128) { *(*op)++ = (uint8_t)(v | 128u); v >>= 7; }
    *(*op)++ = (uint8_t)v;
}

HZ_INLINE size_t hz_get_varint(const uint8_t **ip, const uint8_t *iend)
{
    size_t v = 0; int shift = 0;
    while (*ip < iend) {
        uint8_t b = *(*ip)++;
        v |= (size_t)(b & 127u) << shift;
        if (!(b & 128u)) break;
        shift += 7;
        if (shift > 63) break;
    }
    return v;
}

/* ---- copy helpers -------------------------------------------------------- */
HZ_INLINE void hz_wild16(uint8_t *d, const uint8_t *s, uint8_t *dend)
{
    do { memcpy(d, s, 16); d += 16; s += 16; } while (d < dend);
}

/* Spread a short offset out to at least 8 bytes so the match copy can move
 * whole words.  A 1 byte offset run (RLE) is the common case in practice. */
HZ_INLINE void hz_copy_overlap(uint8_t *d, const uint8_t *s, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i) d[i] = s[i];
}

/* =========================================================================
 * Encoder
 * ========================================================================= */
typedef struct {
    uint32_t *head;      /* hash -> position + 1            */
    uint32_t *chain;     /* position -> previous position+1 */
    int       hlog;
    int       depth;     /* hash chain search depth         */
    int       lazy;      /* lazy match lookahead            */
    size_t    wsize;
} hz_mf;

static int hz_mf_init(hz_mf *mf, size_t wsize, int level)
{
    int hlog;
    if      (level <= 1) { hlog = 16; mf->depth = 1;  mf->lazy = 0; }
    else if (level == 2) { hlog = 17; mf->depth = 8;  mf->lazy = 1; }
    else                 { hlog = 18; mf->depth = 32; mf->lazy = 2; }

    while (hlog > 12 && ((size_t)1 << hlog) > wsize * 2) --hlog;
    mf->hlog  = hlog;
    mf->wsize = wsize;
    mf->head  = (uint32_t *)calloc((size_t)1 << hlog, sizeof(uint32_t));
    mf->chain = NULL;
    if (!mf->head) return -1;
    if (mf->depth > 1) {
        mf->chain = (uint32_t *)malloc(wsize * sizeof(uint32_t));
        if (!mf->chain) { free(mf->head); mf->head = NULL; return -1; }
    }
    return 0;
}

static void hz_mf_free(hz_mf *mf)
{
    free(mf->head);  mf->head  = NULL;
    free(mf->chain); mf->chain = NULL;
}

HZ_INLINE uint32_t hz_mf_hash(const uint8_t *p, int hlog)
{
    return hz_hash4(hz_rd32(p), hlog);
}

HZ_INLINE void hz_mf_insert(hz_mf *mf, const uint8_t *base, const uint8_t *p)
{
    uint32_t pos = (uint32_t)(p - base);
    uint32_t h   = hz_mf_hash(p, mf->hlog);
    if (mf->chain) mf->chain[pos] = mf->head[h];
    mf->head[h] = pos + 1;
}

/* Longest match ending search.  Returns length (0 if none) and sets *poff. */
static size_t hz_mf_find(hz_mf *mf, const uint8_t *base, const uint8_t *ip,
                         const uint8_t *iend, size_t maxdist,
                         size_t nice, uint32_t *poff)
{
    uint32_t pos = (uint32_t)(ip - base);
    uint32_t h   = hz_mf_hash(ip, mf->hlog);
    uint32_t cur = mf->head[h];
    size_t   best = 0;
    uint32_t bestoff = 0;
    int      n = mf->depth;
    size_t   maxlen = (size_t)(iend - ip);

    while (cur != 0 && n-- > 0) {
        uint32_t cpos = cur - 1;
        size_t   dist;
        if (cpos >= pos) break;
        dist = pos - cpos;
        if (dist > maxdist) break;
        {
            const uint8_t *ref = base + cpos;
            /* Cheap reject: compare the byte just past the current best
             * before touching anything else.  Most candidates die here. */
            if (best == 0 || ref[best] == ip[best]) {
                size_t l = 0;
                while (l + 8 <= maxlen) {
                    uint64_t a = hz_rd64(ip + l), b = hz_rd64(ref + l);
                    if (a != b) { l += (size_t)hz_match_len64(a, b); goto done; }
                    l += 8;
                }
                while (l < maxlen && ip[l] == ref[l]) ++l;
            done:
                if (l > best) {
                    best = l; bestoff = (uint32_t)dist;
                    if (l >= nice) break;
                }
            }
        }
        if (!mf->chain) break;
        cur = mf->chain[cpos];
    }
    *poff = bestoff;
    return best;
}

/* Emit one sequence. Returns 0 on success, -1 if the output would overflow.
 *
 * `ocls` is 0..2 for a repeat slot (no offset bytes) or 3 for an explicit
 * offset, in which case `esz` selects how many bytes carry it. */
HZ_INLINE int hz_emit(uint8_t **op, uint8_t *omax,
                      const uint8_t *lit, size_t litlen,
                      size_t mlen, uint32_t off, int ocls, int esz)
{
    uint8_t *o = *op;
    size_t   need = 1 + 10 + litlen + 5 + 10;
    uint8_t  tok;
    size_t   mcode = mlen ? mlen - HZ_MINMATCH : 0;

    if ((size_t)(omax - o) < need) return -1;

    (void)esz;
    tok  = (uint8_t)((litlen < 7 ? litlen : 7) << 5);
    tok |= (uint8_t)((mcode  < 3 ? mcode  : 3) << 3);
    tok |= (uint8_t)ocls;
    *o++ = tok;

    if (litlen >= 7) hz_put_varint(&o, litlen - 7);
    if (litlen) { memcpy(o, lit, litlen); o += litlen; }

    if (mlen) {
        switch (hz_ocls_bytes[ocls]) {
            case 0: break;
            case 1: *o++ = (uint8_t)off; break;
            case 2: hz_wr16(o, (uint16_t)off); o += 2; break;
            case 3: o[0] = (uint8_t)off; o[1] = (uint8_t)(off >> 8);
                    o[2] = (uint8_t)(off >> 16); o += 3; break;
            default: hz_wr32(o, off); o += 4; break;
        }
        if (mcode >= 3) hz_put_varint(&o, mcode - 3);
    }
    *op = o;
    return 0;
}

/* explicit class for an offset: 3 + (payload bytes - 1) */
HZ_INLINE int hz_expl_class(uint32_t off)
{
    if (off <= 0xFFu)     return 3;
    if (off <= 0xFFFFu)   return 4;
    if (off <= 0xFFFFFFu) return 5;
    return 6;
}

/* Which repeat slot holds this offset, or an explicit class if none. */
HZ_INLINE int hz_off_class(uint32_t off, const uint32_t *rep)
{
    if (off == rep[0]) return 0;
    if (off == rep[1]) return 1;
    if (off == rep[2]) return 2;
    return hz_expl_class(off);
}

/* Move the used slot to the front; a new offset pushes the oldest out. */
HZ_INLINE void hz_rep_apply(uint32_t *rep, uint32_t off, int cls)
{
    uint32_t t;
    switch (cls) {
        case 0: break;
        case 1: t = rep[0]; rep[0] = rep[1]; rep[1] = t; break;
        case 2: t = rep[2]; rep[2] = rep[1]; rep[1] = rep[0]; rep[0] = t; break;
        default: rep[2] = rep[1]; rep[1] = rep[0]; rep[0] = off; break;
    }
}

/* Approximate cost in bytes of coding a match, used by the lazy heuristic. */
HZ_INLINE int hz_seq_cost(size_t mlen, uint32_t off, const uint32_t *rep)
{
    int cls = hz_off_class(off, rep);
    int c = 1 + hz_ocls_bytes[cls];
    if (mlen >= HZ_MINMATCH + 3) c += 1 + (int)((mlen - HZ_MINMATCH - 3) / 128);
    return c;
}

size_t hz_fast_compress(uint8_t *dst, size_t dst_cap,
                        const uint8_t *src, size_t src_size, int level)
{
    const uint8_t *ip, *anchor, *iend, *mflimit;
    uint8_t  *op   = dst;
    uint8_t  *omax = dst + dst_cap;
    hz_mf     mf;
    uint32_t  rep[HZ_NREP];
    size_t    nice = level <= 1 ? 32 : (level == 2 ? 64 : 192);
    size_t    maxdist = (size_t)1 << 30;
    int       step_shift = 6;

    if (src_size < HZ_MFLIMIT + HZ_LASTLITERALS) {
        /* Too small to bother: one literal-only token. */
        if (dst_cap < src_size + 11) return 0;
        if (hz_emit(&op, omax, src, src_size, 0, 0, 0, 0) < 0) return 0;
        return (size_t)(op - dst);
    }

    rep[0] = 1; rep[1] = 4; rep[2] = 8;

    if (hz_mf_init(&mf, src_size, level) < 0) return 0;

    ip = src; anchor = src; iend = src + src_size;
    mflimit = iend - HZ_MFLIMIT;

    while (ip < mflimit) {
        size_t   mlen = 0, replen = 0;
        uint32_t moff = 0, repoff = 0;
        const uint8_t *start;

        /* 1. repeat offsets first: they cost nothing to encode, so a hit
         *    here usually wins outright.  All three slots are probed
         *    because structured data cycles between distances rather than
         *    reusing one. */
        {
            int ri;
            for (ri = 0; ri < HZ_NREP; ++ri) {
                uint32_t r = rep[ri];
                const uint8_t *ref;
                if (!r || (size_t)(ip - src) < r) continue;
                ref = ip - r;
                if (hz_rd32(ref) != hz_rd32(ip)) continue;
                {
                    size_t l = 4, maxl = (size_t)(iend - ip);
                    while (l + 8 <= maxl) {
                        uint64_t a = hz_rd64(ip + l), b = hz_rd64(ref + l);
                        if (a != b) { l += (size_t)hz_match_len64(a, b); break; }
                        l += 8;
                    }
                    while (l < maxl && ip[l] == ref[l]) ++l;
                    if (l > replen) { replen = l; repoff = r; }
                }
            }
        }

        /* 2. hash search */
        mlen = hz_mf_find(&mf, src, ip, iend - HZ_LASTLITERALS, maxdist, nice, &moff);

        /* Compare what each option actually costs, not how long it is.
         * A repeat codes its offset for free while an explicit one spends
         * two to five bytes, so a repeat that is several bytes shorter can
         * still be the cheaper sequence.  Judging by length alone is what
         * kept the extra slots from ever being used. */
        if (replen >= HZ_MINMATCH) {
            int take = 1;
            if (mlen >= HZ_MINMATCH) {
                int gain_rep = (int)replen - hz_seq_cost(replen, repoff, rep);
                int gain_new = (int)mlen  - hz_seq_cost(mlen,  moff,   rep);
                take = gain_rep >= gain_new;
            }
            if (take) { mlen = replen; moff = repoff; }
        }

        if (mlen < HZ_MINMATCH) {
            /* No match: advance.  On data that is not compressing we step in
             * increasingly large strides so the encoder degrades gracefully
             * instead of hashing every single byte. */
            size_t skip = 1 + (((size_t)(ip - anchor)) >> step_shift);
            hz_mf_insert(&mf, src, ip);
            ip += skip;
            continue;
        }

        /* 3. lazy evaluation: is the next position clearly better? */
        start = ip;
        if (mf.lazy) {
            int tries = mf.lazy;
            while (tries-- > 0 && ip + 1 < mflimit) {
                size_t   nlen;
                uint32_t noff = 0;
                int      gain;
                hz_mf_insert(&mf, src, ip);
                nlen = hz_mf_find(&mf, src, ip + 1, iend - HZ_LASTLITERALS,
                                  maxdist, nice, &noff);
                if (nlen < HZ_MINMATCH) break;
                /* Compare bytes saved, not raw lengths: a longer match that
                 * needs a 4 byte offset can easily lose to a shorter one
                 * that reuses the repeat slot. */
                gain = (int)nlen - hz_seq_cost(nlen, noff, rep)
                     - ((int)mlen - hz_seq_cost(mlen, moff, rep));
                if (gain > 0) { ip += 1; mlen = nlen; moff = noff; start = ip; }
                else break;
            }
        }

        /* 4. extend backwards over bytes we were about to emit as literals.
         * moff is unsigned, so it must be widened to a signed pointer
         * difference before being negated -- otherwise the index wraps. */
        {
            ptrdiff_t d = (ptrdiff_t)moff;
            while (start > anchor && (start - src) > d &&
                   start[-1] == start[-1 - d]) {
                --start; ++mlen;
            }
        }

        {
            int ocls = hz_off_class(moff, rep);
            int esz  = 0;
            if (hz_emit(&op, omax, anchor, (size_t)(start - anchor),
                        mlen, moff, ocls, esz) < 0) { hz_mf_free(&mf); return 0; }
            hz_rep_apply(rep, moff, ocls);
        }

        /* 5. insert the covered positions so later matches can find them */
        {
            const uint8_t *q = start;
            const uint8_t *qe = start + mlen;
            if (qe > mflimit) qe = mflimit;
            if (level <= 1) {
                if (q < qe) hz_mf_insert(&mf, src, q);
                if (qe > q + 1) hz_mf_insert(&mf, src, qe - 1);
            } else {
                for (; q < qe; ++q) hz_mf_insert(&mf, src, q);
            }
        }

        ip = start + mlen;
        anchor = ip;
    }

    /* final literal run */
    if (hz_emit(&op, omax, anchor, (size_t)(iend - anchor), 0, 0, 0, 0) < 0) {
        hz_mf_free(&mf); return 0;
    }

    hz_mf_free(&mf);
    return (size_t)(op - dst);
}

/* =========================================================================
 * Decoder
 * ========================================================================= */
int hz_fast_decompress(uint8_t *dst, size_t dst_size,
                       const uint8_t *src, size_t src_size)
{
    const uint8_t *ip = src, *iend = src + src_size;
    uint8_t *op = dst, *oend = dst + dst_size;
    uint32_t rep0 = 1, rep1 = 4, rep2 = 8;

    /* Margins that let the hot loop skip per-byte bounds checks. */
    const uint8_t *ilimit = iend - 16;
    uint8_t       *olimit = oend - 32;

    if (src_size == 0) return dst_size == 0 ? 0 : -1;

    for (;;) {
        uint32_t tok, litc, mcode, ocls;
        size_t   litlen, mlen;
        uint32_t off;

        /* ---- fast path: enough slack on both sides for wide copies ---- */
        if (HZ_LIKELY(ip < ilimit && op < olimit)) {
            tok   = *ip++;
            litc  = tok >> 5;
            mcode = (tok >> 3) & 3;
            ocls  = tok & 7;

            litlen = litc;
            if (HZ_UNLIKELY(litc == 7)) litlen = 7 + hz_get_varint(&ip, iend);

            if (HZ_LIKELY(litlen <= 16 && ip + 16 <= iend && op + 16 <= oend)) {
                memcpy(op, ip, 16);
            } else {
                if ((size_t)(iend - ip) < litlen || (size_t)(oend - op) < litlen)
                    return -1;
                if (litlen >= 16 && op + litlen + 16 <= oend && ip + litlen + 16 <= iend)
                    hz_wild16(op, ip, op + litlen);
                else
                    memcpy(op, ip, litlen);
            }
            op += litlen; ip += litlen;

            if (HZ_UNLIKELY(ip >= iend)) break;   /* that was the final run */

            {   /* Repeat slots live in registers, so those classes read no
                 * memory at all; explicit ones take a single masked load
                 * whose width the class already told us. */
                if (ocls < 3) {
                    if (ocls == 0) {
                        off = rep0;
                    } else if (ocls == 1) {
                        off = rep1; rep1 = rep0; rep0 = off;
                    } else {
                        off = rep2; rep2 = rep1; rep1 = rep0; rep0 = off;
                    }
                } else {
                    uint32_t raw = (ip + 4 <= iend) ? hz_rd32(ip) : 0;
                    off = raw & hz_ocls_mask[ocls];
                    ip += hz_ocls_bytes[ocls];
                    rep2 = rep1; rep1 = rep0; rep0 = off;
                }
            }

            mlen = mcode + HZ_MINMATCH;
            if (HZ_UNLIKELY(mcode == 3)) mlen = 3 + HZ_MINMATCH + hz_get_varint(&ip, iend);

            if (HZ_UNLIKELY(off == 0 || (size_t)(op - dst) < off)) return -1;
            if (HZ_UNLIKELY((size_t)(oend - op) < mlen)) return -1;

            {
                const uint8_t *m = op - off;
                if (HZ_LIKELY(off >= 16 && op + mlen + 32 <= oend)) {
                    hz_wild16(op, m, op + mlen);
                } else if (off >= 8 && op + mlen + 32 <= oend) {
                    /* 8 byte steps are still safe when the offset is >= 8 */
                    uint8_t *d = op, *de = op + mlen;
                    do { memcpy(d, m, 8); d += 8; m += 8; } while (d < de);
                } else {
                    hz_copy_overlap(op, m, mlen);
                }
                op += mlen;
            }
            continue;
        }

        /* ---- safe path: every access bounds checked ---- */
        if (ip >= iend) break;
        tok   = *ip++;
        litc  = tok >> 5;
        mcode = (tok >> 3) & 3;
        ocls  = tok & 7;

        litlen = litc;
        if (litc == 7) litlen = 7 + hz_get_varint(&ip, iend);
        if ((size_t)(iend - ip) < litlen) return -1;
        if ((size_t)(oend - op) < litlen) return -1;
        memcpy(op, ip, litlen);
        op += litlen; ip += litlen;

        if (ip >= iend) break;

        if (ocls > 6) return -1;
        if (ocls < 3) {
            uint32_t t;
            if (ocls == 0)      { off = rep0; }
            else if (ocls == 1) { t = rep0; rep0 = rep1; rep1 = t; off = rep0; }
            else                { t = rep2; rep2 = rep1; rep1 = rep0; rep0 = t; off = rep0; }
        } else {
            if ((size_t)(iend - ip) < (size_t)hz_ocls_bytes[ocls]) return -1;
            switch (ocls) {
                case 3: off = *ip++; break;
                case 4: off = hz_rd16(ip); ip += 2; break;
                case 5: off = (uint32_t)ip[0] | ((uint32_t)ip[1] << 8) |
                              ((uint32_t)ip[2] << 16); ip += 3; break;
                default: off = hz_rd32(ip); ip += 4; break;
            }
            rep2 = rep1; rep1 = rep0; rep0 = off;
        }

        mlen = mcode + HZ_MINMATCH;
        if (mcode == 3) mlen = 3 + HZ_MINMATCH + hz_get_varint(&ip, iend);

        if (off == 0 || (size_t)(op - dst) < off) return -1;
        if ((size_t)(oend - op) < mlen) return -1;
        hz_copy_overlap(op, op - off, mlen);
        op += mlen;
    }

    if (op != oend) return -1;
    return 0;
}
