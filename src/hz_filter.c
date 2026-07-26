/* ===========================================================================
 * HYDRA: reversible preprocessing filters.
 *
 * None of these make data smaller by themselves.  They rewrite it into a
 * form the entropy stage can model, and each one is an exact bijection so
 * the pipeline stays lossless.
 *
 *   delta - subtract the value `stride` bytes back.  Turns smooth numeric
 *           series (audio samples, image rows, sensor logs, float arrays)
 *           into small residuals clustered near zero.
 *   exe   - convert x86 relative call/jump targets to absolute.  The same
 *           function called from many sites then produces one repeated
 *           operand instead of a different displacement every time.
 *   lrm   - de-duplicate long range repeats that sit outside the entropy
 *           coder's practical window.
 * ========================================================================= */
#include "hz_int.h"

/* ---- delta -------------------------------------------------------------- */
void hz_delta_fwd(uint8_t *buf, size_t n, int stride)
{
    size_t i;
    uint8_t prev[16];
    int k = 0;
    if (stride < 1 || stride > 16) return;
    memset(prev, 0, sizeof(prev));
    for (i = 0; i < n; ++i) {
        uint8_t v = buf[i];
        buf[i] = (uint8_t)(v - prev[k]);
        prev[k] = v;
        if (++k == stride) k = 0;
    }
}

void hz_delta_rev(uint8_t *buf, size_t n, int stride)
{
    size_t i;
    uint8_t prev[16];
    int k = 0;
    if (stride < 1 || stride > 16) return;
    memset(prev, 0, sizeof(prev));
    for (i = 0; i < n; ++i) {
        uint8_t v = (uint8_t)(buf[i] + prev[k]);
        buf[i] = v;
        prev[k] = v;
        if (++k == stride) k = 0;
    }
}

/* ---- byte transpose (shuffle) ------------------------------------------
 * Regroups fixed width records column-wise: all first bytes, then all second
 * bytes, and so on.  For arrays of multi-byte numbers this is transformative
 * -- the exponent bytes of a float series are nearly constant and become a
 * long run, while the mantissa bytes, which are close to noise, are isolated
 * where they can no longer break up the runs in everything else.
 *
 * The tail that does not fill a whole record is copied through unchanged, so
 * the transform is defined for every length and is exactly invertible.
 * ------------------------------------------------------------------------ */
void hz_shuf_fwd(uint8_t *buf, uint8_t *scratch, size_t n, int width)
{
    size_t rec, r;
    int c;
    if (width < 2 || width > 16) return;
    rec = n / (size_t)width;
    if (rec < 2) return;
    for (c = 0; c < width; ++c) {
        uint8_t *dstcol = scratch + (size_t)c * rec;
        const uint8_t *srcp = buf + c;
        for (r = 0; r < rec; ++r) dstcol[r] = srcp[r * (size_t)width];
    }
    memcpy(buf, scratch, rec * (size_t)width);
}

void hz_shuf_rev(uint8_t *buf, uint8_t *scratch, size_t n, int width)
{
    size_t rec, r;
    int c;
    if (width < 2 || width > 16) return;
    rec = n / (size_t)width;
    if (rec < 2) return;
    for (c = 0; c < width; ++c) {
        const uint8_t *srccol = buf + (size_t)c * rec;
        uint8_t *dstp = scratch + c;
        for (r = 0; r < rec; ++r) dstp[r * (size_t)width] = srccol[r];
    }
    memcpy(buf, scratch, rec * (size_t)width);
}

/* ---- x86 branch filter --------------------------------------------------
 * Rewrites the 4 byte operand of E8 (call) and E9 (jmp) from relative to
 * absolute, so that many call sites targeting one function collapse onto a
 * single repeated operand instead of a different displacement each time.
 *
 * Getting this exactly reversible is the entire difficulty, and it is worth
 * writing down the argument because two natural formulations are wrong.
 *
 * Let T be the set of words that equal the sign extension of their own low
 * 24 bits -- precisely the signed displacements a near branch can hold.  The
 * operand map on T is
 *
 *     v -> sext24(v + addr)          (encode)
 *     w -> sext24(w - addr)          (decode)
 *
 * which is addition modulo 2^24, a bijection of T onto itself.  T is closed
 * under it, so "is this operand in T" gives the same answer before and after
 * the transform.  The opcode byte itself is never written.
 *
 * That still leaves the scanning order.  A transform at position q rewrites
 * bytes q+1..q+4, which overlaps the operand window of any position within
 * four bytes of it.  If both sides scanned forward, the encoder wouldjudge 
 * position p against the original bytes while the decoder judged the same
 * position against bytes a later transform had already altered, and the two
 * would diverge.  The fix is to make the two passes see identical state:
 *
 *     encoder scans backwards,  decoder scans forwards,
 *     and neither one skips.
 *
 * Then when either side examines position i, exactly the transforms at
 * positions greater than i have been applied, in both directions.  The
 * decisions therefore agree at every position, and overlapping transforms
 * unwind in the exact reverse of the order they were applied.
 *
 * Rejected alternatives, recorded so they are not tried again:
 *   - testing the *result* for near-branch-ness and skipping the write when
 *     it falls outside: not invertible, the decoder cannot tell the two
 *     cases apart.
 *   - accepting any word whose top byte is 0x00 or 0xFF: that set has 2^25
 *     members but sext24 produces only 2^24 values, so the map is two-to-one.
 *   - forward scan on both sides with a four byte skip: diverges whenever a
 *     transform lands inside an earlier operand window.
 * ------------------------------------------------------------------------ */
HZ_INLINE uint32_t hz_sext24(uint32_t v)
{
    v &= 0x00FFFFFFu;
    return (v & 0x00800000u) ? (v | 0xFF000000u) : v;
}

/* true when v is a canonical sign extended 24 bit displacement */
HZ_INLINE int hz_in_s24(uint32_t v)
{
    return v == hz_sext24(v);
}

void hz_exe_fwd(uint8_t *buf, size_t n)
{
    size_t i;
    if (n < 5) return;
    /* backwards, so that the decoder's forward pass observes the same state */
    for (i = n - 5 + 1; i-- > 0; ) {
        if ((buf[i] & 0xFE) == 0xE8) {
            uint8_t *p = buf + i + 1;
            uint32_t v = hz_get32le(p);
            if (hz_in_s24(v))
                hz_put32le(p, hz_sext24(v + (uint32_t)(i + 5)));
        }
    }
}

void hz_exe_rev(uint8_t *buf, size_t n)
{
    size_t i;
    if (n < 5) return;
    for (i = 0; i + 5 <= n; ++i) {
        if ((buf[i] & 0xFE) == 0xE8) {
            uint8_t *p = buf + i + 1;
            uint32_t w = hz_get32le(p);
            if (hz_in_s24(w))
                hz_put32le(p, hz_sext24(w - (uint32_t)(i + 5)));
        }
    }
}


/* ---- long range matcher -------------------------------------------------
 * A coarse LZ pass over the whole block with a large stride, meant to catch
 * duplication at distances the entropy coder will never see.  Output is a
 * simple token stream:
 *
 *   varint literal_run, literal bytes, [varint match_len, varint distance]
 *
 * A match_len of 0 terminates.  Minimum match is deliberately large: short
 * matches are better left to the following stage, which codes them with
 * context rather than an explicit distance.
 * ------------------------------------------------------------------------ */
/* Minimum match and probe stride.
 *
 * The stride is the sensitive one.  Hashing every 16th position misses the
 * start of a repeat that begins mid-stride, and the matcher then only picks
 * it up later -- on a backup set of near-identical images that cost 15% of
 * the final size (50.7x against 59.4x).  Stride 4 finds essentially all of
 * them for a modest cost in the analysis pass, which is not on the decode
 * path at all. */
#define HZ_LRM_MIN   32
#define HZ_LRM_STEP  4
#define HZ_LRM_BITS  20

HZ_INLINE void lrm_put(uint8_t **op, size_t v)
{
    while (v >= 128) { *(*op)++ = (uint8_t)(v | 128u); v >>= 7; }
    *(*op)++ = (uint8_t)v;
}

HZ_INLINE size_t lrm_get(const uint8_t **ip, const uint8_t *iend, int *err)
{
    size_t v = 0; int shift = 0;
    for (;;) {
        uint8_t b;
        if (*ip >= iend) { *err = 1; return 0; }
        b = *(*ip)++;
        v |= (size_t)(b & 127u) << shift;
        if (!(b & 128u)) break;
        shift += 7;
        if (shift > 63) { *err = 1; return 0; }
    }
    return v;
}

size_t hz_lrm_fwd(uint8_t *dst, size_t dst_cap, const uint8_t *src, size_t n)
{
    uint32_t *tab;
    uint8_t  *op = dst, *omax = dst + dst_cap;
    size_t    i = 0, anchor = 0;

    if (n < HZ_LRM_MIN * 4) return 0;

    tab = (uint32_t *)calloc((size_t)1 << HZ_LRM_BITS, sizeof(uint32_t));
    if (!tab) return 0;

    while (i + HZ_LRM_MIN <= n) {
        uint32_t h = hz_hash8(hz_rd64(src + i), HZ_LRM_BITS);
        uint32_t cand = tab[h];
        tab[h] = (uint32_t)(i + 1);

        if (cand) {
            size_t cpos = cand - 1, l = 0, maxl = n - i;
            while (l + 8 <= maxl && hz_rd64(src + cpos + l) == hz_rd64(src + i + l)) l += 8;
            while (l < maxl && src[cpos + l] == src[i + l]) ++l;
            if (l >= HZ_LRM_MIN) {
                size_t litlen = i - anchor;
                size_t need = litlen + 40;
                if ((size_t)(omax - op) < need) { free(tab); return 0; }
                lrm_put(&op, litlen);
                memcpy(op, src + anchor, litlen); op += litlen;
                lrm_put(&op, l);
                lrm_put(&op, i - cpos);
                i += l;
                anchor = i;
                continue;
            }
        }
        i += HZ_LRM_STEP;
    }

    {
        size_t litlen = n - anchor;
        if ((size_t)(omax - op) < litlen + 20) { free(tab); return 0; }
        lrm_put(&op, litlen);
        memcpy(op, src + anchor, litlen); op += litlen;
        lrm_put(&op, 0);           /* terminator */
    }

    free(tab);
    return (size_t)(op - dst);
}

int hz_lrm_rev(uint8_t *dst, size_t dst_cap, size_t *out_size,
               const uint8_t *src, size_t n)
{
    const uint8_t *ip = src, *iend = src + n;
    size_t pos = 0;
    int err = 0;

    for (;;) {
        size_t litlen = lrm_get(&ip, iend, &err);
        size_t mlen, dist;
        if (err) return -1;
        if (litlen > (size_t)(iend - ip)) return -1;
        if (litlen > dst_cap - pos) return -1;
        memcpy(dst + pos, ip, litlen);
        pos += litlen; ip += litlen;

        mlen = lrm_get(&ip, iend, &err);
        if (err) return -1;
        if (mlen == 0) break;
        dist = lrm_get(&ip, iend, &err);
        if (err) return -1;
        if (dist == 0 || dist > pos) return -1;
        if (mlen > dst_cap - pos) return -1;
        {
            size_t k;
            const uint8_t *m = dst + pos - dist;
            for (k = 0; k < mlen; ++k) dst[pos + k] = m[k];
            pos += mlen;
        }
    }
    *out_size = pos;
    return 0;
}

/* ---- content analysis ---------------------------------------------------
 * Cheap, sampled heuristics.  Every decision made here is recorded in the
 * block header, so a wrong guess costs ratio but never correctness.
 * ------------------------------------------------------------------------ */

/* Order-0 entropy of a byte histogram, in millibits per byte.
 * Integer only: a table of 256 * log2 values would be overkill, so the log
 * is evaluated with the same fixed point exp2 machinery the coder uses,
 * inverted by a short binary search.  Precision of a few millibits is far
 * more than a filter decision needs. */
static uint32_t hist_entropy_mbits(const uint32_t *freq, uint32_t total)
{
    uint64_t acc = 0;
    int i;
    if (!total) return 0;
    for (i = 0; i < 256; ++i) {
        uint32_t f = freq[i];
        uint32_t q;         /* p in Q16 */
        int lo, hi, mid;
        if (!f) continue;
        q = (uint32_t)(((uint64_t)f << 16) / total);
        if (!q) continue;
        /* find x with squash-free identity: we want -log2(q/65536) */
        lo = 0; hi = 16 << 10;               /* millibits, 0..16384 */
        while (lo < hi) {
            uint64_t v;
            mid = (lo + hi) >> 1;
            /* v = 2^-(mid/1024) in Q16 == 65536 >> (mid/1024) refined */
            v = hz_fx_exp2((uint32_t)(((16u << 10) - (uint32_t)mid) * 64u));
            v >>= 16;                        /* now 2^(16 - mid/1024) */
            if (v > q) lo = mid + 1; else hi = mid;
        }
        acc += (uint64_t)f * (uint32_t)lo;
    }
    return (uint32_t)(acc / total);
}

/* Sample a byte histogram of the residual after subtracting `stride` back.
 * stride 0 means "no transform", i.e. the raw bytes. */
static uint32_t residual_entropy(const uint8_t *p, size_t n, int stride)
{
    uint32_t freq[256];
    size_t i, step, cnt = 0;
    uint32_t total = 0;

    memset(freq, 0, sizeof(freq));
    if (n < (size_t)(stride ? stride : 1) * 16) return 0xFFFFFFFFu;

    /* Sample contiguous windows rather than isolated bytes.
     *
     * A strided sample of a strided transform is the trap here: taking one
     * byte every `step` positions means every sample lands on the same
     * column of the record, so a 4 byte layout is only ever observed
     * through one of its four fields and the residual looks like noise.
     * Reading whole runs keeps every column represented in proportion. */
    {
        const size_t WIN = 512;
        size_t nwin = 32, w;
        size_t span = n - (size_t)stride;

        if (span < WIN * nwin) { nwin = 1; }
        step = nwin > 1 ? span / nwin : 0;
        /* align each window start to the stride so phases line up */
        if (stride > 1 && step) step -= step % (size_t)stride;

        for (w = 0; w < nwin && cnt < 16384; ++w) {
            size_t start = (size_t)stride + w * step;
            size_t end = start + WIN;
            if (end > n) end = n;
            for (i = start; i < end && cnt < 16384; ++i, ++cnt) {
                uint8_t v = stride ? (uint8_t)(p[i] - p[i - stride]) : p[i];
                ++freq[v];
                ++total;
            }
        }
    }
    if (!total) return 0xFFFFFFFFu;
    return hist_entropy_mbits(freq, total);
}

void hz_analyze(const uint8_t *src, size_t n, hz_analysis *a)
{
    size_t i, step;
    uint32_t freq[256];
    uint64_t best = (uint64_t)-1;
    uint64_t plain;
    int s, bs = 0;
    size_t e8 = 0, samples = 0;

    memset(a, 0, sizeof(*a));
    memset(freq, 0, sizeof(freq));
    if (n < 64) return;

    /* order-0 entropy on a sample */
    step = n > 65536 ? n / 65536 : 1;
    for (i = 0; i < n; i += step) { ++freq[src[i]]; ++samples; }
    {
        double h = 0.0;
        for (i = 0; i < 256; ++i) if (freq[i]) {
            double q = (double)freq[i] / (double)samples;
            /* log2 without libm: count leading zeros of the reciprocal is
             * not accurate enough here, so use the identity via frexp-free
             * Newton steps.  A coarse value is all the heuristic needs. */
            double x = q, lg = 0.0;
            while (x < 0.5) { x *= 2.0; lg -= 1.0; }
            /* refine with two terms of the series around 1 */
            {
                double t = (x - 1.0) / (x + 1.0);
                lg += 2.8853900817779268 * (t + t * t * t / 3.0);
            }
            h -= q * lg;
        }
        a->order0_entropy = h;
    }

    /* ---- record width for the byte transpose ----
     * Look for a fixed stride at which the byte values repeat far more often
     * than chance.  Real numeric arrays give themselves away through their
     * high-order bytes: in a float64 series the top byte is nearly constant,
     * so column 7 has a very low symbol count while column 0 looks random.
     * Score each candidate width by how skewed its columns are overall. */
    {
        int wbest = 0;
        uint32_t sbest = 0;
        int wcand[4];
        int nw = 0, wi;

        wcand[nw++] = 2; wcand[nw++] = 4; wcand[nw++] = 8; wcand[nw++] = 16;

        for (wi = 0; wi < nw; ++wi) {
            int w = wcand[wi];
            uint32_t score = 0;
            int col;
            if (n < (size_t)w * 512) continue;
            for (col = 0; col < w; ++col) {
                uint32_t colfreq[256];
                uint32_t total = 0, distinct = 0;
                size_t k, stride = (size_t)w, lim = 4096;
                memset(colfreq, 0, sizeof(colfreq));
                for (k = (size_t)col; k < n && total < lim; k += stride) {
                    if (!colfreq[src[k]]) ++distinct;
                    ++colfreq[src[k]];
                    ++total;
                }
                /* a column with few distinct values is highly compressible */
                if (distinct <= 4)        score += 100;
                else if (distinct <= 16)  score += 60;
                else if (distinct <= 64)  score += 20;
            }
            /* normalise so wide records are not favoured just for having
             * more columns to accumulate score from */
            score = score / (uint32_t)w;
            if (score > sbest) { sbest = score; wbest = w; }
        }
        /* Only propose it when at least one column is genuinely degenerate;
         * otherwise the transpose just scrambles locality for nothing. */
        a->best_shuffle_width = (sbest >= 20) ? wbest : 0;
    }

    /* ---- delta ----
     * Score every stride by the order-0 entropy of the residual it produces
     * and keep the best.  Entropy is the right currency here because it is
     * what the coder downstream will actually spend: a 16 bit waveform has
     * large residuals in absolute terms but a far tighter distribution, and
     * a magnitude based test misses exactly that case.
     *
     * Ties go to the smaller stride, which keeps residuals local. */
    plain = residual_entropy(src, n, 0);
    for (s = 1; s <= 16; ++s) {
        uint64_t sc = residual_entropy(src, n, s);
        if (sc + 8 < best) { best = sc; bs = s; }   /* 8 mbit hysteresis */
    }

    /* Apply it only on a clear win.  Being too eager destroys the byte level
     * structure the context models feed on -- text is the classic victim --
     * so the gate is deliberately strict: the residual must be meaningfully
     * cheaper AND the data must not already be strongly symbolic. */
    if (best != (uint64_t)-1 && plain != 0xFFFFFFFFu &&
        best + 200 < plain &&              /* >= 0.2 bits per byte saved */
        a->order0_entropy > 5.0)
        a->best_delta_stride = bs;
    else
        a->best_delta_stride = 0;

    /* x86 heuristic: a meaningful density of E8/E9 with near-branch operands */
    step = n > 1u << 20 ? n / (1u << 20) : 1;
    for (i = 0; i + 5 < n; i += step) {
        if ((src[i] & 0xFE) == 0xE8) {
            uint8_t top = src[i + 4];
            if (top == 0x00 || top == 0xFF) ++e8;
        }
    }
    a->is_x86 = (e8 * 800 > (n / step)) ? 1 : 0;

    /* long range duplication: sample fingerprints and count collisions */
    if (n >= (1u << 18)) {
        uint32_t *t = (uint32_t *)calloc(1u << 16, sizeof(uint32_t));
        size_t hits = 0, tries = 0;
        if (t) {
            for (i = 0; i + 8 < n; i += 512) {
                uint32_t h = hz_hash8(hz_rd64(src + i), 16);
                if (t[h] && memcmp(src + t[h] - 1, src + i, 8) == 0) ++hits;
                t[h] = (uint32_t)(i + 1);
                ++tries;
            }
            free(t);
            a->lrm_worth = (tries && hits * 20 > tries) ? 1 : 0;
        }
    }
}
