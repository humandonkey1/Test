/* ===========================================================================
 * HYDRA: the STRONG engine.
 *
 * The FAST engine spends more than half its output on raw offset bytes and
 * another third on tokens, because a byte aligned format cannot charge less
 * than eight bits for anything.  This engine keeps an LZ parse but codes
 * every field through the binary range coder with small adaptive contexts,
 * which is where those bits come back.
 *
 * It is deliberately much lighter than the full context mixing model: one
 * counter lookup per bit, no mixer, no bit-history tables.  That keeps
 * decode in the tens of MB/s instead of under one, filling the gap between
 * the two extremes.
 *
 * Per sequence:
 *   literal length  gamma code, context = bucket of the previous length
 *   literals        8 bits, context = previous byte x partial byte
 *   offset class    tree coded, context = previous class
 *   offset          gamma length + raw bits when the class is "new"
 *   match length    gamma code, context = offset class
 *
 * Encoder and decoder share one description of that layout through the
 * HZ_MID_SEQ macros below, so the two cannot drift apart silently.
 * ========================================================================= */
#include "hz_int.h"
#include "hz_rc.h"
#include "hz_model.h"

#define HZ_MID_MINMATCH 4
#define HZ_MID_NREP     3
#define HZ_MID_GAMMAMAX 40

typedef struct {
    hz_ctr lit[256][256];     /* order-1 x partial byte             */
    hz_ctr litlen[8][64];     /* run length gamma                   */
    hz_ctr ismatch[64];       /* does a match follow this run       */
    hz_ctr oclass[4][4];      /* offset class tree, ctx = prev class*/
    hz_ctr onbits[4][64];     /* significant bits of a new offset   */
    hz_ctr obits[32];         /* the offset payload bits            */
    hz_ctr mlen[4][64];       /* match length gamma, ctx = class    */
} hz_mid_model;

static void hz_mid_model_init(hz_mid_model *m)
{
    hz_ctr *p = (hz_ctr *)m;
    size_t i, cnt = sizeof(*m) / sizeof(hz_ctr);
    for (i = 0; i < cnt; ++i) p[i] = HZ_CTR_INIT;
}

/* ---- primitive bit helpers ---------------------------------------------- */
HZ_INLINE void mid_enc(hz_enc *e, hz_ctr *c, int bit)
{
    hz_enc_bit(e, hz_ctr_p16(*c), bit);
    hz_ctr_update(c, bit, 255);
}
HZ_INLINE int mid_dec(hz_dec *d, hz_ctr *c)
{
    int bit = hz_dec_bit(d, hz_ctr_p16(*c));
    hz_ctr_update(c, bit, 255);
    return bit;
}

/* Gamma code: unary bit-count in ctx[0..], then the payload in ctx[32..].
 * Values are stored as v+1 so zero is representable. */
static void mid_enc_gamma(hz_enc *e, hz_ctr *ctx, uint64_t v)
{
    uint64_t x = v + 1;
    int nb = 0, i;
    while ((x >> (nb + 1)) && nb < HZ_MID_GAMMAMAX - 1) ++nb;
    for (i = 0; i < nb; ++i) mid_enc(e, &ctx[i < 31 ? i : 31], 1);
    mid_enc(e, &ctx[nb < 31 ? nb : 31], 0);
    for (i = nb - 1; i >= 0; --i)
        mid_enc(e, &ctx[32 + (i < 31 ? i : 31)], (int)((x >> i) & 1));
}

static uint64_t mid_dec_gamma(hz_dec *d, hz_ctr *ctx)
{
    int nb = 0, i;
    uint64_t x;
    while (nb < HZ_MID_GAMMAMAX - 1) {
        if (!mid_dec(d, &ctx[nb < 31 ? nb : 31])) break;
        ++nb;
    }
    x = 1;
    for (i = nb - 1; i >= 0; --i)
        x = (x << 1) | (uint64_t)mid_dec(d, &ctx[32 + (i < 31 ? i : 31)]);
    return x - 1;
}

/* One literal byte.  The partial-byte node runs 1..255 and ends at 256..511,
 * so the low eight bits of the final node are the byte itself -- that is the
 * property the decoder relies on, and it is why the node must not wrap. */
HZ_INLINE void mid_enc_literal(hz_enc *e, hz_mid_model *m, int prev, int c)
{
    int node = 1, b;
    for (b = 7; b >= 0; --b) {
        int bit = (c >> b) & 1;
        mid_enc(e, &m->lit[prev][node], bit);
        node = (node << 1) | bit;
    }
}

HZ_INLINE int mid_dec_literal(hz_dec *d, hz_mid_model *m, int prev)
{
    int node = 1, b;
    for (b = 0; b < 8; ++b)
        node = (node << 1) | mid_dec(d, &m->lit[prev][node]);
    return node & 0xFF;
}

HZ_INLINE int mid_lctx(size_t last)
{
    return last < 4 ? (int)last : (last < 16 ? 4 : (last < 64 ? 5 : 6));
}

/* ---- repeat offset slots ------------------------------------------------- */
HZ_INLINE int oclass_of(uint32_t off, const uint32_t *rep)
{
    if (off == rep[0]) return 0;
    if (off == rep[1]) return 1;
    if (off == rep[2]) return 2;
    return 3;
}

HZ_INLINE void rep_apply(uint32_t *rep, uint32_t off, int cls)
{
    uint32_t t;
    switch (cls) {
        case 0: break;
        case 1: t = rep[0]; rep[0] = rep[1]; rep[1] = t; break;
        case 2: t = rep[2]; rep[2] = rep[1]; rep[1] = rep[0]; rep[0] = t; break;
        default: rep[2] = rep[1]; rep[1] = rep[0]; rep[0] = off; break;
    }
}

/* ---- match finder -------------------------------------------------------- */
typedef struct {
    uint32_t *head;
    uint32_t *chain;
    int hlog, depth;
} hz_mid_mf;

static int mid_mf_init(hz_mid_mf *mf, size_t wsize, int level)
{
    int hlog = level <= 4 ? 17 : 18;
    while (hlog > 12 && ((size_t)1 << hlog) > wsize * 2) --hlog;
    mf->hlog  = hlog;
    mf->depth = level <= 4 ? 12 : (level <= 5 ? 32 : 96);
    mf->head  = (uint32_t *)calloc((size_t)1 << hlog, sizeof(uint32_t));
    mf->chain = (uint32_t *)malloc((wsize ? wsize : 1) * sizeof(uint32_t));
    if (!mf->head || !mf->chain) { free(mf->head); free(mf->chain); return -1; }
    return 0;
}
static void mid_mf_free(hz_mid_mf *mf) { free(mf->head); free(mf->chain); }

HZ_INLINE void mid_insert(hz_mid_mf *mf, const uint8_t *base, size_t pos)
{
    uint32_t h = hz_hash5(hz_rd64(base + pos), mf->hlog);
    mf->chain[pos] = mf->head[h];
    mf->head[h] = (uint32_t)(pos + 1);
}

static size_t mid_find(hz_mid_mf *mf, const uint8_t *base, size_t pos,
                       size_t end, size_t nice, uint32_t *poff)
{
    uint32_t h = hz_hash5(hz_rd64(base + pos), mf->hlog);
    uint32_t cur = mf->head[h];
    size_t best = 0, maxlen = end - pos;
    uint32_t bestoff = 0;
    int n = mf->depth;

    while (cur && n-- > 0) {
        size_t cpos = cur - 1;
        if (cpos >= pos) break;
        if (best == 0 || base[cpos + best] == base[pos + best]) {
            size_t l = 0;
            while (l + 8 <= maxlen &&
                   hz_rd64(base + cpos + l) == hz_rd64(base + pos + l)) l += 8;
            while (l < maxlen && base[cpos + l] == base[pos + l]) ++l;
            if (l > best) {
                best = l;
                bestoff = (uint32_t)(pos - cpos);
                if (l >= nice) break;
            }
        }
        cur = mf->chain[cpos];
    }
    *poff = bestoff;
    return best;
}

/* =========================================================================
 * Encoder
 * ========================================================================= */
size_t hz_mid_compress(uint8_t *dst, size_t dst_cap,
                       const uint8_t *src, size_t n, int level)
{
    hz_mid_model *m;
    hz_mid_mf mf;
    hz_enc e;
    size_t pos = 0, anchor = 0, lastlit = 0;
    uint32_t rep[HZ_MID_NREP] = { 1, 4, 8 };
    size_t nice = level <= 4 ? 32 : (level <= 5 ? 64 : 192);
    int prev_cls = 3;
    size_t safe = n > 8 ? n - 8 : 0;

    if (dst_cap < 16) return 0;
    m = (hz_mid_model *)malloc(sizeof(*m));
    if (!m) return 0;
    hz_mid_model_init(m);
    if (mid_mf_init(&mf, n ? n : 1, level) < 0) { free(m); return 0; }
    hz_enc_init(&e, dst, dst_cap);

    while (pos < safe) {
        size_t mlen = 0, i;
        uint32_t moff = 0;
        int cls;

        for (i = 0; i < HZ_MID_NREP; ++i) {
            uint32_t r = rep[i];
            if (r && (size_t)r <= pos) {
                const uint8_t *a = src + pos, *b = src + pos - r;
                size_t l = 0, maxl = n - pos;
                while (l + 8 <= maxl && hz_rd64(a + l) == hz_rd64(b + l)) l += 8;
                while (l < maxl && a[l] == b[l]) ++l;
                if (l >= HZ_MID_MINMATCH && l > mlen) { mlen = l; moff = r; }
            }
        }
        {
            uint32_t ho = 0;
            size_t hl = mid_find(&mf, src, pos, n, nice, &ho);
            if (hl >= HZ_MID_MINMATCH && hl > mlen + 1) { mlen = hl; moff = ho; }
        }

        if (mlen < HZ_MID_MINMATCH) {
            mid_insert(&mf, src, pos);
            ++pos;
            continue;
        }

        /* literals, then the flag saying a match follows */
        {
            size_t litlen = pos - anchor;
            mid_enc_gamma(&e, m->litlen[mid_lctx(lastlit)], litlen);
            for (i = 0; i < litlen; ++i)
                mid_enc_literal(&e, m, (anchor + i) ? src[anchor + i - 1] : 0,
                                src[anchor + i]);
            lastlit = litlen;
        }
        mid_enc(&e, &m->ismatch[prev_cls * 16 + (int)(lastlit & 15)], 1);

        cls = oclass_of(moff, rep);
        mid_enc(&e, &m->oclass[prev_cls][0], (cls >> 1) & 1);
        mid_enc(&e, &m->oclass[prev_cls][1 + ((cls >> 1) & 1)], cls & 1);

        if (cls == 3) {
            int nb = hz_log2_floor(moff), k;
            mid_enc_gamma(&e, m->onbits[prev_cls], (uint64_t)nb);
            for (k = nb - 1; k >= 0; --k)
                mid_enc(&e, &m->obits[k], (int)((moff >> k) & 1));
        }
        mid_enc_gamma(&e, m->mlen[cls], mlen - HZ_MID_MINMATCH);

        rep_apply(rep, moff, cls);
        prev_cls = cls;

        {
            size_t q = pos, qe = pos + mlen;
            if (qe > safe) qe = safe;
            for (; q < qe; ++q) mid_insert(&mf, src, q);
        }
        pos += mlen;
        anchor = pos;

        if (e.overflow) { mid_mf_free(&mf); free(m); return 0; }
    }

    /* trailing literals and the terminator flag */
    {
        size_t litlen = n - anchor, i;
        mid_enc_gamma(&e, m->litlen[mid_lctx(lastlit)], litlen);
        for (i = 0; i < litlen; ++i)
            mid_enc_literal(&e, m, (anchor + i) ? src[anchor + i - 1] : 0,
                            src[anchor + i]);
        mid_enc(&e, &m->ismatch[prev_cls * 16 + (int)(litlen & 15)], 0);
    }

    {
        size_t out = hz_enc_flush(&e);
        mid_mf_free(&mf);
        free(m);
        if (e.overflow) return 0;
        return out;
    }
}

/* =========================================================================
 * Decoder
 * ========================================================================= */
int hz_mid_decompress(uint8_t *dst, size_t n, const uint8_t *src, size_t src_size)
{
    hz_mid_model *m;
    hz_dec d;
    size_t pos = 0, lastlit = 0;
    uint32_t rep[HZ_MID_NREP] = { 1, 4, 8 };
    int prev_cls = 3;

    m = (hz_mid_model *)malloc(sizeof(*m));
    if (!m) return -1;
    hz_mid_model_init(m);
    hz_dec_init(&d, src, src_size);

    for (;;) {
        uint64_t litlen = mid_dec_gamma(&d, m->litlen[mid_lctx(lastlit)]);
        uint64_t mlen;
        uint32_t moff;
        int cls, hi, lo;
        size_t i;

        if (litlen > (uint64_t)(n - pos)) { free(m); return -1; }
        for (i = 0; i < (size_t)litlen; ++i) {
            dst[pos] = (uint8_t)mid_dec_literal(&d, m, pos ? dst[pos - 1] : 0);
            ++pos;
        }
        lastlit = (size_t)litlen;

        if (!mid_dec(&d, &m->ismatch[prev_cls * 16 + (int)(lastlit & 15)]))
            break;

        hi = mid_dec(&d, &m->oclass[prev_cls][0]);
        lo = mid_dec(&d, &m->oclass[prev_cls][1 + hi]);
        cls = (hi << 1) | lo;

        if (cls == 3) {
            uint64_t nb = mid_dec_gamma(&d, m->onbits[prev_cls]);
            int k;
            if (nb > 31) { free(m); return -1; }
            moff = 1u << (int)nb;
            for (k = (int)nb - 1; k >= 0; --k)
                moff |= (uint32_t)mid_dec(&d, &m->obits[k]) << k;
        } else {
            moff = rep[cls];
        }

        mlen = mid_dec_gamma(&d, m->mlen[cls]) + HZ_MID_MINMATCH;

        if (moff == 0 || (size_t)moff > pos) { free(m); return -1; }
        if (mlen > (uint64_t)(n - pos)) { free(m); return -1; }
        {
            const uint8_t *s = dst + pos - moff;
            for (i = 0; i < (size_t)mlen; ++i) dst[pos + i] = s[i];
            pos += (size_t)mlen;
        }

        rep_apply(rep, moff, cls);
        prev_cls = cls;
    }

    free(m);
    return pos == n ? 0 : -1;
}
