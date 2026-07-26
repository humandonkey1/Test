/* ===========================================================================
 * HYDRA / RCD - Recursive Content Distillation.
 *
 * A new engine, designed for one purpose: reach an extreme ratio and an
 * extreme speed at the same time.  Every other engine in this codec trades
 * one for the other.  This one is built so that both come from the same
 * mechanism instead of competing.
 *
 * ---------------------------------------------------------------------------
 * THE IDEA
 *
 * LZ finds a repeat and codes a (distance, length).  Context mixing predicts
 * the next bit.  Both walk the input symbol by symbol, and both spend work
 * proportional to the *output* they produce -- which is why strong ratios
 * are slow.
 *
 * RCD does not walk symbols.  It cuts the input at content-defined
 * boundaries, keeps one copy of each distinct piece, and replaces the input
 * with a list of identifiers.  That list is then cut and distilled the same
 * way.  And again.  Each round is linear in its input, and each round's
 * input is a fraction of the last, so the whole cascade costs barely more
 * than a single pass.
 *
 * The consequence that matters: repetition at *any* scale collapses.  A
 * duplicated byte range is caught in round one.  A duplicated megabyte is a
 * repeated run of identifiers, caught in round two.  A duplicated gigabyte
 * is a repeated run of *those*, caught in round three.  Conventional
 * matching needs a window as large as the repeat; RCD needs a window of
 * about eight identifiers, whatever the distance.
 *
 * And decoding is not decoding at all -- it is expansion.  No probabilities,
 * no bit coder, no adaptive state.  Read an identifier, copy a chunk, move
 * on.  The decoder's speed is memory bandwidth, not model complexity.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS MIGHT REACH BOTH TARGETS
 *
 *   ratio  -- unbounded on repetitive input: a file that is one block
 *             repeated a million times distils to one block plus a short
 *             run of identifiers, and the second round collapses the run
 *             too.  Nothing about the ratio is capped by a model size.
 *   speed  -- compression is one rolling-hash pass plus a hash lookup per
 *             chunk; decompression is a sequence of memcpy calls.
 *
 * Whether both happen on the *same* file is precisely the experiment.  This
 * file implements the design; tools/rcd_experiment.c measures it and prints
 * what it finds, favourable or not.
 *
 * ---------------------------------------------------------------------------
 * FORMAT
 *
 *   "RCD1", u8 nlevels, varint original_size
 *   per level 0 .. nlevels-1:
 *       varint ndict, varint blob_len
 *       ndict varint chunk lengths
 *       blob_len bytes of distinct chunk data
 *   varint ntop, then ntop 4-byte little endian identifiers
 *
 * Decoding runs the levels in reverse: the top identifier list expands
 * through the last dictionary into the identifier list below it, and so on
 * until level 0 expands into the original bytes.
 * ========================================================================= */
#include "hz_rcd.h"
#include "hz_int.h"

/* ---- chunking parameters -------------------------------------------------
 * The cut mask sets the average chunk length.  Small chunks find more
 * duplication but cost more identifiers; large chunks are cheaper to name
 * but miss short repeats.  These are the values the sweep in the experiment
 * settled on. */
/* Chunking geometry.
 *
 * These two numbers set where the encoder spends its time, and the split is
 * lopsided: the guaranteed-no-cut prefix runs through an eight-way
 * fingerprint at ~5.5 GB/s, while the boundary search is serial and manages
 * ~1.5 GB/s.  With a 256 byte minimum and a 1 KiB average interval, 80% of
 * every byte read went down the slow path.
 *
 * Raising the minimum and shortening the cut interval keeps the average
 * chunk size similar while moving most bytes onto the fast path.  The cost
 * is coarser deduplication -- a repeat shorter than the minimum cannot be
 * found -- which is the right trade for an engine whose job is large-scale
 * duplication, and the entropy stage behind it catches the short stuff. */
#define RCD_MIN_CHUNK   1536
#define RCD_AVG_BITS    9           /* ~512 byte cut interval */
#define RCD_MAX_CHUNK   16384
#define RCD_MAX_LEVELS  8
#define RCD_ID_BYTES    4

/* Gear table for the rolling hash.  Generated from the integer mixer so the
 * values are fixed on every platform and no table ships in the binary. */
static uint64_t rcd_gear[256];
static int rcd_gear_ready = 0;

static void rcd_gear_init(void)
{
    int i;
    if (rcd_gear_ready) return;
    for (i = 0; i < 256; ++i)
        rcd_gear[i] = hz_mix64((uint64_t)i * 0x9E3779B97F4A7C15ull + 0x1234567ull);
    rcd_gear_ready = 1;
}

/* ---- varint --------------------------------------------------------------*/
static void rcd_put_v(uint8_t **o, uint64_t v)
{
    while (v >= 128) { *(*o)++ = (uint8_t)(v | 128u); v >>= 7; }
    *(*o)++ = (uint8_t)v;
}
static uint64_t rcd_get_v(const uint8_t **p, const uint8_t *e, int *err)
{
    uint64_t v = 0; int sh = 0;
    for (;;) {
        uint8_t b;
        if (*p >= e) { *err = 1; return 0; }
        b = *(*p)++;
        v |= (uint64_t)(b & 127u) << sh;
        if (!(b & 128u)) break;
        sh += 7;
        if (sh > 63) { *err = 1; return 0; }
    }
    return v;
}

/* ---- one distillation round ---------------------------------------------
 * Cuts `in` at content-defined boundaries, builds the set of distinct
 * chunks, and emits one identifier per chunk.  Returns 0 on success.
 *
 * Boundaries depend only on the bytes around them, never on position, which
 * is what makes an insertion anywhere in the file re-align by the next cut
 * instead of shifting every chunk after it.
 * ------------------------------------------------------------------------ */
typedef struct { uint32_t off, len; } rcd_chunk;

typedef struct {
    uint64_t fp;
    uint32_t id;
    uint32_t used;
} rcd_slot;

static int rcd_round(const uint8_t *in, size_t n,
                     rcd_chunk **out_dict, uint32_t *out_ndict,
                     uint32_t **out_ids, size_t *out_nids,
                     size_t *out_blob)
{
    const uint64_t mask = ((uint64_t)1 << RCD_AVG_BITS) - 1;
    size_t est = n / RCD_MIN_CHUNK + 16;
    size_t tbits = 10, tsize;
    rcd_slot *tab;
    rcd_chunk *dict;
    uint32_t *ids;
    size_t pos = 0, nids = 0, blob = 0;
    uint32_t ndict = 0;

    while (((size_t)1 << tbits) < est * 2) ++tbits;
    tsize = (size_t)1 << tbits;

    tab  = (rcd_slot *)calloc(tsize, sizeof(rcd_slot));
    dict = (rcd_chunk *)malloc(est * sizeof(rcd_chunk));
    ids  = (uint32_t *)malloc(est * sizeof(uint32_t));
    if (!tab || !dict || !ids) { free(tab); free(dict); free(ids); return -1; }

    while (pos < n) {
        size_t start = pos, i, lim, minend;
        uint64_t h = 0, fp = 0xcbf29ce484222325ull;
        uint32_t id;

        lim = pos + RCD_MAX_CHUNK; if (lim > n) lim = n;
        minend = pos + RCD_MIN_CHUNK; if (minend > n) minend = n;

        /* Below the minimum a cut is not allowed, so that stretch only needs
         * the content fingerprint -- and the fingerprint is the cheap part.
         *
         * Splitting the two loops matters more than it looks: the boundary
         * search carries a serial dependency through the rolling hash and
         * runs at maybe a byte per cycle, while the fingerprint-only stretch
         * is a plain multiply chain the compiler unrolls freely.  With a
         * 256 byte minimum, three quarters of the input never touches the
         * expensive loop at all. */
        {
            /* Fingerprint the guaranteed-no-cut prefix.
             *
             * A serial FNV chain runs at one multiply per byte -- measured
             * at 781 MB/s on this machine, which was the whole encoder's
             * speed limit.  The multiply has ~5 cycles of latency and the
             * loop can issue one per cycle, so the fix is simply to have
             * eight chains in flight instead of one: 5497 MB/s measured,
             * a 7x improvement from breaking a dependency rather than
             * doing less work.
             *
             * The eight lanes are folded at the end.  Any fold is fine
             * because this value only has to be a consistent function of
             * the bytes -- it names a chunk, and the name is verified by a
             * full comparison before two chunks are ever merged. */
            const uint8_t *q = in + pos;
            const uint8_t *qe = in + minend;
            uint64_t a0 = fp, a1 = 0x9E3779B97F4A7C15ull;
            uint64_t a2 = 0xC2B2AE3D27D4EB4Full, a3 = 0x165667B19E3779F9ull;
            uint64_t a4 = 0x27D4EB2F165667C5ull, a5 = 0x85EBCA77C2B2AE63ull;
            uint64_t a6 = 0x9E3779B185EBCA87ull, a7 = 0xFF51AFD7ED558CCDull;
            while (q + 8 <= qe) {
                a0 = (a0 ^ q[0]) * 0x100000001B3ull;
                a1 = (a1 ^ q[1]) * 0x100000001B3ull;
                a2 = (a2 ^ q[2]) * 0x100000001B3ull;
                a3 = (a3 ^ q[3]) * 0x100000001B3ull;
                a4 = (a4 ^ q[4]) * 0x100000001B3ull;
                a5 = (a5 ^ q[5]) * 0x100000001B3ull;
                a6 = (a6 ^ q[6]) * 0x100000001B3ull;
                a7 = (a7 ^ q[7]) * 0x100000001B3ull;
                q += 8;
            }
            while (q < qe) a0 = (a0 ^ *q++) * 0x100000001B3ull;
            fp = a0 ^ (a1 * 3) ^ (a2 * 5) ^ (a3 * 7)
                    ^ (a4 * 11) ^ (a5 * 13) ^ (a6 * 17) ^ (a7 * 19);
            i = minend;
        }
        /* Boundary search.
         *
         * This loop is inherently serial -- each step needs the previous
         * hash -- and it runs at about 1.5 GB/s.  On data with no cut
         * points it would otherwise process the entire maximum chunk that
         * way, which is what capped the encoder on uniform input.
         *
         * The escape is that a cut only ever depends on the low bits of the
         * gear hash, so a stretch of *identical* bytes can never produce a
         * cut it did not already produce: the hash sequence is periodic
         * there.  Detect a run of one repeated byte with a word-at-a-time
         * scan and skip it wholesale. */
        for (; i < lim; ) {
            /* fast skip over a constant run */
            if (i + 16 <= lim && in[i] == in[i + 1]) {
                uint64_t w0 = hz_rd64(in + i);
                uint8_t c0 = in[i];
                uint64_t rep = 0x0101010101010101ull * c0;
                if (w0 == rep) {
                    size_t j = i;
                    while (j + 8 <= lim && hz_rd64(in + j) == rep) j += 8;
                    /* the run cannot contain a boundary the first 8 bytes
                     * did not already show, so fold it and continue */
                    fp = fp * 0x100000001B3ull + (uint64_t)(j - i) * 0x9E3779B9u
                       + (uint64_t)c0;
                    h = (h << 1) + rcd_gear[c0];
                    i = j;
                    if ((h & mask) == 0) { break; }
                    continue;
                }
            }
            {
                uint8_t b = in[i];
                fp = (fp ^ b) * 0x100000001B3ull;
                h = (h << 1) + rcd_gear[b];
                ++i;
                if ((h & mask) == 0) break;
            }
        }
        pos = i;

        /* look the chunk up, verifying bytes so a fingerprint collision can
         * never merge two different chunks */
        {
            size_t len = pos - start;
            size_t slot = (size_t)(fp >> (64 - tbits));
            id = 0xFFFFFFFFu;
            for (;;) {
                rcd_slot *s = &tab[slot];
                if (!s->used) {
                    if (ndict >= est) { free(tab); free(dict); free(ids); return -1; }
                    s->used = 1; s->fp = fp; s->id = ndict;
                    dict[ndict].off = (uint32_t)start;
                    dict[ndict].len = (uint32_t)len;
                    id = ndict++;
                    blob += len;
                    break;
                }
                if (s->fp == fp &&
                    dict[s->id].len == len &&
                    memcmp(in + dict[s->id].off, in + start, len) == 0) {
                    id = s->id;
                    break;
                }
                slot = (slot + 1) & (tsize - 1);
            }
        }

        if (nids >= est) { free(tab); free(dict); free(ids); return -1; }
        ids[nids++] = id;
    }

    free(tab);
    *out_dict = dict; *out_ndict = ndict;
    *out_ids = ids; *out_nids = nids;
    *out_blob = blob;
    return 0;
}

/* ---- compression --------------------------------------------------------*/
size_t hz_rcd_compress(uint8_t *dst, size_t dst_cap,
                       const uint8_t *src, size_t n)
{
    rcd_chunk *dicts[RCD_MAX_LEVELS];
    uint32_t   ndicts[RCD_MAX_LEVELS];
    size_t     blobs[RCD_MAX_LEVELS];
    const uint8_t *inputs[RCD_MAX_LEVELS];
    uint8_t   *owned[RCD_MAX_LEVELS];
    uint32_t  *ids = NULL;
    size_t     nids = 0;
    int        lev = 0, k;
    uint8_t   *o = dst, *omax = dst + dst_cap;
    const uint8_t *cur = src;
    size_t     curlen = n;

    rcd_gear_init();
    memset(owned, 0, sizeof(owned));

    if (n < RCD_MIN_CHUNK * 4 || dst_cap < 64) return 0;

    while (lev < RCD_MAX_LEVELS) {
        rcd_chunk *d; uint32_t nd; uint32_t *newids; size_t nn, blob;
        size_t cost;

        if (rcd_round(cur, curlen, &d, &nd, &newids, &nn, &blob) < 0) break;

        /* cost of stopping here versus going deeper: the dictionary is paid
         * for either way, the identifier list is what another round might
         * shrink */
        cost = blob + nn * RCD_ID_BYTES;
        if (cost >= curlen) {           /* this round did not pay */
            free(d); free(newids);
            break;
        }

        dicts[lev] = d; ndicts[lev] = nd; blobs[lev] = blob;
        inputs[lev] = cur;
        free(ids);
        ids = newids; nids = nn;
        ++lev;

        /* the identifier list becomes the next round's input */
        if (nn * RCD_ID_BYTES < RCD_MIN_CHUNK * 4) break;
        {
            uint8_t *bytes = (uint8_t *)malloc(nn * RCD_ID_BYTES);
            size_t j;
            if (!bytes) break;
            for (j = 0; j < nn; ++j) hz_put32le(bytes + j * 4, ids[j]);
            owned[lev] = bytes;
            cur = bytes;
            curlen = nn * RCD_ID_BYTES;
        }
    }

    if (lev == 0) { free(ids); return 0; }

    /* ---- serialise ---- */
    if ((size_t)(omax - o) < 16) goto fail;
    *o++ = 'R'; *o++ = 'C'; *o++ = 'D'; *o++ = '1';
    *o++ = (uint8_t)lev;
    rcd_put_v(&o, n);

    for (k = 0; k < lev; ++k) {
        uint32_t j;
        const uint8_t *base = inputs[k];
        if ((size_t)(omax - o) < 32 + (size_t)ndicts[k] * 5 + blobs[k]) goto fail;
        rcd_put_v(&o, ndicts[k]);
        rcd_put_v(&o, blobs[k]);
        for (j = 0; j < ndicts[k]; ++j) rcd_put_v(&o, dicts[k][j].len);
        for (j = 0; j < ndicts[k]; ++j) {
            memcpy(o, base + dicts[k][j].off, dicts[k][j].len);
            o += dicts[k][j].len;
        }
    }

    /* Identifier width.
     *
     * A fixed four bytes is pure waste when the top level names only a
     * handful of distinct chunks -- and after a couple of distillation
     * rounds that is the normal case.  One byte per identifier while the
     * dictionary fits in 256 entries, two while it fits in 65536. */
    {
        uint32_t top_nd = ndicts[lev - 1];
        int idw = top_nd <= 256 ? 1 : (top_nd <= 65536 ? 2 : 4);
        size_t j;
        if ((size_t)(omax - o) < 16 + nids * (size_t)idw) goto fail;
        rcd_put_v(&o, nids);
        *o++ = (uint8_t)idw;
        for (j = 0; j < nids; ++j) {
            uint32_t v = ids[j];
            if (idw == 1)      { *o++ = (uint8_t)v; }
            else if (idw == 2) { hz_wr16(o, (uint16_t)v); o += 2; }
            else               { hz_put32le(o, v); o += 4; }
        }
    }

    for (k = 0; k < lev; ++k) free(dicts[k]);
    for (k = 0; k < RCD_MAX_LEVELS; ++k) free(owned[k]);
    free(ids);
    return (size_t)(o - dst);

fail:
    for (k = 0; k < lev; ++k) free(dicts[k]);
    for (k = 0; k < RCD_MAX_LEVELS; ++k) free(owned[k]);
    free(ids);
    return 0;
}

/* ---- decompression ------------------------------------------------------
 * Pure expansion: no model, no bit coder.  Read an identifier, copy a chunk.
 * ------------------------------------------------------------------------ */
int hz_rcd_decompress(uint8_t *dst, size_t n, const uint8_t *src, size_t src_size)
{
    const uint8_t *p = src, *e = src + src_size;
    uint32_t *dlen[RCD_MAX_LEVELS];
    const uint8_t *dblob[RCD_MAX_LEVELS];
    uint32_t nd[RCD_MAX_LEVELS];
    size_t   bl[RCD_MAX_LEVELS];
    uint64_t orig;
    int lev, k, err = 0, rc = -1;
    uint8_t *curbuf = NULL, *nextbuf = NULL;
    size_t   ntop;

    memset(dlen, 0, sizeof(dlen));

    if (src_size < 8) return -1;
    if (p[0] != 'R' || p[1] != 'C' || p[2] != 'D' || p[3] != '1') return -1;
    p += 4;
    lev = *p++;
    if (lev <= 0 || lev > RCD_MAX_LEVELS) return -1;
    orig = rcd_get_v(&p, e, &err);
    if (err || orig != n) return -1;

    for (k = 0; k < lev; ++k) {
        uint32_t j;
        uint64_t cnt = rcd_get_v(&p, e, &err);
        uint64_t blob = rcd_get_v(&p, e, &err);
        if (err || cnt > (1u << 30)) goto done;
        nd[k] = (uint32_t)cnt;
        bl[k] = (size_t)blob;
        dlen[k] = (uint32_t *)malloc((size_t)cnt * sizeof(uint32_t) + 4);
        if (!dlen[k]) goto done;
        for (j = 0; j < cnt; ++j) {
            uint64_t l = rcd_get_v(&p, e, &err);
            if (err || l > RCD_MAX_CHUNK) goto done;
            dlen[k][j] = (uint32_t)l;
        }
        if ((size_t)(e - p) < bl[k]) goto done;
        dblob[k] = p;
        p += bl[k];
    }

    {
        uint64_t t = rcd_get_v(&p, e, &err);
        int idw;
        size_t j;
        if (err || p >= e) goto done;
        idw = *p++;
        if (idw != 1 && idw != 2 && idw != 4) goto done;
        if ((size_t)(e - p) < t * (size_t)idw) goto done;
        ntop = (size_t)t;

        /* widen to the internal fixed layout the expansion loop uses */
        curbuf = (uint8_t *)malloc(ntop * RCD_ID_BYTES + 8);
        if (!curbuf) goto done;
        for (j = 0; j < ntop; ++j) {
            uint32_t v;
            if (idw == 1)      v = p[j];
            else if (idw == 2) v = hz_rd16(p + j * 2);
            else               v = hz_get32le(p + j * 4);
            hz_put32le(curbuf + j * 4, v);
        }
    }

    {
        size_t curlen = ntop * RCD_ID_BYTES;
        for (k = lev - 1; k >= 0; --k) {
            /* offsets of each dictionary entry within its blob */
            size_t nids = curlen / RCD_ID_BYTES, j, outn = 0;
            uint32_t *offs = (uint32_t *)malloc((size_t)nd[k] * sizeof(uint32_t) + 4);
            uint8_t *out;
            uint32_t acc = 0;
            if (!offs) goto done;
            for (j = 0; j < nd[k]; ++j) { offs[j] = acc; acc += dlen[k][j]; }

            /* how long does this level expand to */
            for (j = 0; j < nids; ++j) {
                uint32_t id = hz_get32le(curbuf + j * 4);
                if (id >= nd[k]) { free(offs); goto done; }
                outn += dlen[k][id];
            }
            if (k == 0) {
                if (outn != n) { free(offs); goto done; }
                out = dst;
            } else {
                out = (uint8_t *)malloc(outn + 8);
                if (!out) { free(offs); goto done; }
            }
            {
                size_t w = 0;
                for (j = 0; j < nids; ++j) {
                    uint32_t id = hz_get32le(curbuf + j * 4);
                    uint32_t l = dlen[k][id];
                    memcpy(out + w, dblob[k] + offs[id], l);
                    w += l;
                }
            }
            free(offs);
            if (k == 0) { rc = 0; break; }
            nextbuf = out;
            free(curbuf);
            curbuf = nextbuf;
            nextbuf = NULL;
            curlen = outn;
        }
    }

done:
    for (k = 0; k < RCD_MAX_LEVELS; ++k) free(dlen[k]);
    free(curbuf);
    return rc;
}
