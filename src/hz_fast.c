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
 *   bits 4..2   match length  0..6 -> 4..10, 7 = extended
 *   bits 1..0   offset class  0 = repeat last, 1 = 1 byte, 2 = 2, 3 = 4
 *
 * The repeat-offset class is the reason this beats a plain LZ77 layout on
 * structured data: records that differ in a few fields keep reusing the same
 * distance, and here that distance costs zero bytes to re-encode.
 *
 * A stream always finishes with a literal-only token, which is what lets the
 * decoder terminate on "input exhausted" without a separate end marker.
 * ========================================================================= */
#include "hz_int.h"

#define HZ_MINMATCH     4
#define HZ_LASTLITERALS 12   /* tail that is never part of a match */
#define HZ_MFLIMIT      16   /* encoder stops looking this far from the end */

static const uint8_t  hz_off_bytes[4] = { 0, 1, 2, 4 };
static const uint32_t hz_off_mask[4]  = { 0u, 0xFFu, 0xFFFFu, 0xFFFFFFFFu };

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

/* Emit one sequence. Returns 0 on success, -1 if the output would overflow. */
HZ_INLINE int hz_emit(uint8_t **op, uint8_t *omax,
                      const uint8_t *lit, size_t litlen,
                      size_t mlen, uint32_t off, int ocls)
{
    uint8_t *o = *op;
    size_t   need = 1 + 10 + litlen + 4 + 10;
    uint8_t  tok;
    size_t   mcode = mlen ? mlen - HZ_MINMATCH : 0;

    if ((size_t)(omax - o) < need) return -1;

    tok  = (uint8_t)((litlen < 7 ? litlen : 7) << 5);
    tok |= (uint8_t)((mcode  < 7 ? mcode  : 7) << 2);
    tok |= (uint8_t)ocls;
    *o++ = tok;

    if (litlen >= 7) hz_put_varint(&o, litlen - 7);
    if (litlen) { memcpy(o, lit, litlen); o += litlen; }

    if (mlen) {
        switch (ocls) {
            case 0: break;
            case 1: *o++ = (uint8_t)off; break;
            case 2: hz_wr16(o, (uint16_t)off); o += 2; break;
            default: hz_wr32(o, off); o += 4; break;
        }
        if (mcode >= 7) hz_put_varint(&o, mcode - 7);
    }
    *op = o;
    return 0;
}

HZ_INLINE int hz_off_class(uint32_t off, uint32_t rep)
{
    if (off == rep)    return 0;
    if (off <= 0xFF)   return 1;
    if (off <= 0xFFFF) return 2;
    return 3;
}

/* Approximate cost in bytes of coding a match, used by the lazy heuristic. */
HZ_INLINE int hz_seq_cost(size_t mlen, uint32_t off, uint32_t rep)
{
    int c = 1 + hz_off_bytes[hz_off_class(off, rep)];
    if (mlen >= HZ_MINMATCH + 7) c += 1 + (int)((mlen - HZ_MINMATCH - 7) / 128);
    return c;
}

size_t hz_fast_compress(uint8_t *dst, size_t dst_cap,
                        const uint8_t *src, size_t src_size, int level)
{
    const uint8_t *ip, *anchor, *iend, *mflimit;
    uint8_t  *op   = dst;
    uint8_t  *omax = dst + dst_cap;
    hz_mf     mf;
    uint32_t  rep  = 0;
    size_t    nice = level <= 1 ? 32 : (level == 2 ? 64 : 192);
    size_t    maxdist = (size_t)1 << 30;
    int       step_shift = 6;

    if (src_size < HZ_MFLIMIT + HZ_LASTLITERALS) {
        /* Too small to bother: one literal-only token. */
        if (dst_cap < src_size + 11) return 0;
        if (hz_emit(&op, omax, src, src_size, 0, 0, 0) < 0) return 0;
        return (size_t)(op - dst);
    }

    if (hz_mf_init(&mf, src_size, level) < 0) return 0;

    ip = src; anchor = src; iend = src + src_size;
    mflimit = iend - HZ_MFLIMIT;

    while (ip < mflimit) {
        size_t   mlen = 0, replen = 0;
        uint32_t moff = 0;
        const uint8_t *start;

        /* 1. repeat offset probe first: it is the cheapest sequence we can
         *    possibly emit, so a hit here usually wins outright. */
        if (rep && (size_t)(ip - src) >= rep) {
            const uint8_t *ref = ip - rep;
            if (hz_rd32(ref) == hz_rd32(ip)) {
                size_t l = 4, maxl = (size_t)(iend - ip) - HZ_LASTLITERALS + 4;
                if (maxl > (size_t)(iend - ip)) maxl = (size_t)(iend - ip);
                while (l + 8 <= maxl) {
                    uint64_t a = hz_rd64(ip + l), b = hz_rd64(ref + l);
                    if (a != b) { l += (size_t)hz_match_len64(a, b); break; }
                    l += 8;
                }
                while (l < maxl && ip[l] == ref[l]) ++l;
                replen = l;
            }
        }

        /* 2. hash search */
        mlen = hz_mf_find(&mf, src, ip, iend - HZ_LASTLITERALS, maxdist, nice, &moff);

        /* A repeat match only has to be within a byte or two of the hashed
         * match to be the better deal, because it codes its offset for free. */
        if (replen >= HZ_MINMATCH &&
            (mlen < HZ_MINMATCH || replen + 2 >= mlen)) {
            mlen = replen; moff = rep;
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
            if (hz_emit(&op, omax, anchor, (size_t)(start - anchor),
                        mlen, moff, ocls) < 0) { hz_mf_free(&mf); return 0; }
            rep = moff;
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
    if (hz_emit(&op, omax, anchor, (size_t)(iend - anchor), 0, 0, 0) < 0) {
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
    uint32_t rep = 0;

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
            mcode = (tok >> 2) & 7;
            ocls  = tok & 3;

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

            {   /* branch-free offset fetch */
                uint32_t raw = (ip + 4 <= iend) ? hz_rd32(ip) : 0;
                uint32_t got = raw & hz_off_mask[ocls];
                ip += hz_off_bytes[ocls];
                off = ocls ? got : rep;
                rep = off;
            }

            mlen = mcode + HZ_MINMATCH;
            if (HZ_UNLIKELY(mcode == 7)) mlen = 7 + HZ_MINMATCH + hz_get_varint(&ip, iend);

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
        mcode = (tok >> 2) & 7;
        ocls  = tok & 3;

        litlen = litc;
        if (litc == 7) litlen = 7 + hz_get_varint(&ip, iend);
        if ((size_t)(iend - ip) < litlen) return -1;
        if ((size_t)(oend - op) < litlen) return -1;
        memcpy(op, ip, litlen);
        op += litlen; ip += litlen;

        if (ip >= iend) break;

        if ((size_t)(iend - ip) < (size_t)hz_off_bytes[ocls]) return -1;
        switch (ocls) {
            case 0: off = rep; break;
            case 1: off = *ip++; break;
            case 2: off = hz_rd16(ip); ip += 2; break;
            default: off = hz_rd32(ip); ip += 4; break;
        }
        rep = off;

        mlen = mcode + HZ_MINMATCH;
        if (mcode == 7) mlen = 7 + HZ_MINMATCH + hz_get_varint(&ip, iend);

        if (off == 0 || (size_t)(op - dst) < off) return -1;
        if ((size_t)(oend - op) < mlen) return -1;
        hz_copy_overlap(op, op - off, mlen);
        op += mlen;
    }

    if (op != oend) return -1;
    return 0;
}
