/* ===========================================================================
 * HYDRA: the frame layer.
 *
 * Splits the input into blocks, decides per block which filters and which
 * entropy engine to use, and writes a self describing container.
 *
 * The guiding rule for every automatic decision here is that a wrong guess
 * must cost ratio, never correctness: each choice is recorded in the block
 * header, and the RAW fallback guarantees the output can never grow by more
 * than the header itself.
 * ========================================================================= */
#include "hydra.h"
#include "hz_int.h"
#include "hz_pool.h"
#include "hz_sgi.h"
#include <stdio.h>

/* ---- version / errors --------------------------------------------------- */
const char *hydra_version_string(void) { return "hydra 1.1.0"; }

#if defined(HZ_THREADS)
#include <unistd.h>
int hz_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > HZ_MAX_THREADS) n = HZ_MAX_THREADS;
    return (int)n;
}
#endif

int hydra_cpu_count(void) { return hz_cpu_count(); }

const char *hydra_strerror(int code)
{
    switch (code) {
        case HYDRA_OK:           return "ok";
        case HYDRA_E_NOMEM:      return "out of memory";
        case HYDRA_E_CORRUPT:    return "corrupt or truncated stream";
        case HYDRA_E_DSTSIZE:    return "destination buffer too small";
        case HYDRA_E_SRCSIZE:    return "source size invalid";
        case HYDRA_E_BADMAGIC:   return "not a hydra stream";
        case HYDRA_E_BADVERSION: return "unsupported format version";
        case HYDRA_E_CHECKSUM:   return "checksum mismatch";
        case HYDRA_E_PARAM:      return "invalid parameter";
        case HYDRA_E_IO:         return "io error";
        case HYDRA_E_INTERNAL:   return "internal error";
        default:                 return "unknown error";
    }
}

/* ---- content digest -----------------------------------------------------
 * A 64 bit fingerprint used only to detect accidental corruption.  Four
 * independent accumulators over 32 byte stripes so it is not dominated by
 * the tail, then an avalanche finish. */
uint64_t hydra_digest(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint64_t a = HZ_PRIME64_1, b = HZ_PRIME64_2, c = HZ_PRIME64_3, d = len * HZ_PRIME64_1;

    while ((size_t)(end - p) >= 32) {
        a = hz_mix64(a ^ hz_rd64(p));
        b = hz_mix64(b ^ hz_rd64(p + 8));
        c = hz_mix64(c ^ hz_rd64(p + 16));
        d = hz_mix64(d ^ hz_rd64(p + 24));
        p += 32;
    }
    while ((size_t)(end - p) >= 8) { a = hz_mix64(a ^ hz_rd64(p)); p += 8; }
    while (p < end) { b = hz_mix64(b ^ (uint64_t)*p++); }

    return hz_mix64(a ^ (b + 0x9E3779B9u)) ^ hz_mix64(c ^ (d + 0x85EBCA77u));
}

/* ---- options ------------------------------------------------------------ */
void hydra_opts_init(hydra_opts *o, int level)
{
    if (!o) return;
    if (level < HYDRA_LEVEL_MIN) level = HYDRA_LEVEL_MIN;
    if (level > HYDRA_LEVEL_MAX) level = HYDRA_LEVEL_MAX;
    o->level        = level;
    o->block_log    = 0;
    o->checksum     = 1;
    o->enable_delta = 1;
    o->enable_exe   = 1;
    o->enable_lrm   = 1;
    o->force_method = -1;
    o->verbose      = 0;
    o->threads      = 0;
    o->enable_sgi   = 1;
}

/* Choose the block size.
 *
 * Blocks are what bound memory, but every split throws away history: the
 * models restart cold and any repeat that spans the boundary is invisible.
 * A file that lands just over a block size is the worst case -- 67 MiB
 * against a 64 MiB block leaves a 3 MiB orphan that compresses badly and
 * drags the whole ratio down.
 *
 * So rather than a fixed size per level, grow the block until it covers the
 * whole input, up to the level's ceiling.  One block is always the best
 * choice for ratio when it fits. */
static int auto_block_log(int level, size_t src_size)
{
    int bl, maxbl;

    /* The FAST engine has no cross-block model to lose: its matches rarely
     * reach past a megabyte anyway, so splitting costs almost no ratio and
     * buys parallelism on both sides.  The context models are the opposite
     * -- every split restarts them cold -- so those keep the whole input in
     * one block whenever it fits. */
    if (level <= 3)      maxbl = 20;   /* 1 MiB, favour parallelism */
    else if (level <= 6) maxbl = 26;   /* 64 MiB                    */
    else                 maxbl = 28;   /* 256 MiB                   */
    if (maxbl > HZ_BLOCK_LOG_MAX) maxbl = HZ_BLOCK_LOG_MAX;

    bl = HZ_BLOCK_LOG_MIN;
    while (bl < maxbl && ((size_t)1 << bl) < src_size) ++bl;
    return bl;
}

size_t hydra_bound(size_t src_size)
{
    /* frame header + terminator + digest, plus per block worst case */
    size_t nblocks = src_size / ((size_t)1 << HZ_BLOCK_LOG_MIN) + 1;
    return src_size + nblocks * 32 + 64;
}

/* The probe writes into the low half of `tmp` while the filtered candidate
 * sits in the high half; this makes that split explicit at the call sites. */
HZ_INLINE uint8_t *pb_lo(uint8_t *tmp, size_t half) { (void)half; return tmp; }

/* Measure what the entropy stage actually spends on a slice.
 *
 * Used to settle filter decisions by measurement rather than by heuristic.
 * The probe runs the same engine the block will use, because the engines
 * disagree about filters: the STRONG parser sees a delta residual as noise
 * that breaks its matches, while the context models see a tighter
 * distribution.  Asking the wrong one produces exactly the wrong answer. */
static size_t hz_probe_cost(uint8_t *scratch, size_t scratch_cap,
                            const uint8_t *data, size_t n, int method, int level)
{
    size_t r = 0;
    if (scratch_cap < n / 2) return 0;
    if (method == HZ_M_CM)        r = hz_cm_compress(scratch, scratch_cap, data, n, level);
    else if (method == HZ_M_MID)  r = hz_mid_compress(scratch, scratch_cap, data, n, level);
    else                          r = hz_fast_compress(scratch, scratch_cap, data, n, level);
    return r ? r : n;      /* did not fit: charge the full size */
}

/* ---- block header ------------------------------------------------------- */
typedef struct {
    uint8_t method;
    uint8_t nfilters;
    uint8_t fid[HZ_MAX_FILTERS];
    uint8_t fparam[HZ_MAX_FILTERS];
    uint32_t usize;   /* length the entropy stage produces (pre-unfilter) */
    uint32_t osize;   /* bytes this block contributes to the final output */
    uint32_t csize;   /* payload length                                   */
} hz_bhdr;

static size_t bhdr_size(const hz_bhdr *h) { return 2 + 2 * (size_t)h->nfilters + 12; }

static void bhdr_write(uint8_t *p, const hz_bhdr *h)
{
    int i;
    *p++ = h->method;
    *p++ = h->nfilters;
    for (i = 0; i < h->nfilters; ++i) { *p++ = h->fid[i]; *p++ = h->fparam[i]; }
    hz_put32le(p, h->usize); p += 4;
    hz_put32le(p, h->osize); p += 4;
    hz_put32le(p, h->csize);
}

/* =========================================================================
 * Compression
 * ========================================================================= */
/* ---- per-block compression job -----------------------------------------
 * Everything a block needs is either read-only (the source, the options) or
 * private to the worker (three scratch buffers and an output buffer), so
 * jobs never touch each other's state and the pool needs no locking beyond
 * claiming the next index.
 *
 * Each job writes a complete block -- header and payload -- into its own
 * buffer.  The caller concatenates them in index order, which is what keeps
 * the output byte identical to a single threaded run. */
typedef struct {
    uint8_t *buf;        /* output for this block            */
    size_t   cap;
    size_t   len;
    uint8_t *work;       /* staging for the filtered data    */
    uint8_t *tmp;
    uint8_t *shufbuf;
    int      err;
} hz_jobbuf;

typedef struct {
    const uint8_t    *src;
    size_t            src_size;
    size_t            bsize;
    size_t            work_cap;
    const hydra_opts *opts;
    hz_jobbuf        *jobs;
    int               njobs;
} hz_cjob_ctx;

static void hz_compress_block(void *vctx, int index)
{
    hz_cjob_ctx *c   = (hz_cjob_ctx *)vctx;
    hz_jobbuf   *jb  = &c->jobs[index];
    const hydra_opts *opts = c->opts;
    size_t pos       = (size_t)index * c->bsize;
    size_t n         = hz_minz(c->bsize, c->src_size - pos);
    const uint8_t *blk = c->src + pos;
    size_t work_cap  = c->work_cap;
    uint8_t *work    = jb->work;
    uint8_t *tmp     = jb->tmp;
    uint8_t *shufbuf = jb->shufbuf;

    jb->len = 0;
    jb->err = HYDRA_OK;

        hz_bhdr h;
        hz_analysis an;
        int block_method;
        size_t stage_len = n;
        uint8_t *stage;
        size_t hsz, csz = 0;
        int used_cm = 0;

        memset(&h, 0, sizeof(h));

        /* Which engine will code this block?  The filter probe needs to know,
         * because the answer changes the decision. */
        {
            int mth = opts->force_method;
            if (mth < 0) {
                if (opts->level <= 3)      mth = HZ_M_FAST;
                else if (opts->level <= 6) mth = HZ_M_MID;
                else                      mth = HZ_M_CM;
            }
            block_method = mth;
        }

        /* Structural grammar induction gets first refusal, on the raw
         * block and before any filter runs.
         *
         * Order matters here and it cost a debugging round to see why: the
         * long range de-duplicator rewrites repeated rows into distance
         * references, which is precisely the structure the grammar needs to
         * observe.  Running SGI afterwards showed it a stream with the
         * regularity already stripped out, and it declined on data it
         * models near-perfectly.
         *
         * When the input is machine generated this does not merely beat the
         * other engines, it changes what the output size depends on: a law
         * costs the same whether the generating loop ran a thousand times
         * or a billion.  When there is no structure it declines cheaply. */
        if (opts->enable_sgi && n >= 4096) {
            size_t sgz = hz_sgi_compress(jb->buf + 14, jb->cap - 32,
                                         blk, n, opts->level);
            if (sgz) {
                /* Compare like with like.
                 *
                 * A grammar is mostly fixed cost, so its bytes per input
                 * byte keep falling as the block grows, while a probe of a
                 * rival engine measures a rate that does not.  Comparing
                 * the two directly flatters whichever saw more data.
                 *
                 * So estimate what the rival would spend on the *whole*
                 * block by scaling its probe rate, and require the grammar
                 * to beat that outright.  Ties go to the rival: the generic
                 * engines degrade gracefully on data that only half fits
                 * their assumptions, and a grammar does not. */
                size_t probe = hz_minz(n, (size_t)1 << 18);
                size_t rival = hz_probe_cost(jb->work, work_cap,
                                             blk, probe, block_method,
                                             opts->level);
                if (rival) {
                    double rival_full =
                        (double)rival * (double)n / (double)probe;
                    if ((double)sgz >= rival_full * 0.9) sgz = 0;
                }
            }
            if (sgz) {
                hz_bhdr sh;
                memset(&sh, 0, sizeof(sh));
                sh.method   = HZ_M_SGI;
                sh.nfilters = 0;
                sh.usize    = (uint32_t)n;
                sh.osize    = (uint32_t)n;
                sh.csize    = (uint32_t)sgz;
                bhdr_write(jb->buf, &sh);
                jb->len = bhdr_size(&sh) + sgz;
                if (opts->verbose)
                    fprintf(stderr, "[hydra] block %8lu -> %8lu  m=sgi\n",
                            (unsigned long)n, (unsigned long)sgz);
                return;
            }
        }

        /* ---- pick filters ---- */
        hz_analyze(blk, n, &an);

        /* Filtering needs a private copy; the input is const. */
        if (n > work_cap) { jb->err = HYDRA_E_INTERNAL; return; }
        memcpy(work, blk, n);
        stage = work;

        /* Long range de-duplication, decided by measurement.
         *
         * Removing a distant repeat is not automatically a win.  The context
         * models can code a repeat that is *within* their reach for far less
         * than an explicit length-and-distance pair costs, and pulling it out
         * also strips the surrounding context they were using to predict.
         * Measured on a SQL dump full of near-identical statements, the
         * filter turned 20.6x into 17.3x.
         *
         * So the filter only survives if a probe says it earns its place.
         * The probe compresses a slice of the raw block and the same slice
         * of the de-duplicated block, and the comparison is per input byte
         * because the two slices cover different amounts of original data. */
        if (opts->enable_lrm && an.lrm_worth && h.nfilters + 1 < HZ_MAX_FILTERS) {
            size_t l = hz_lrm_fwd(tmp, work_cap, stage, stage_len);
            int keep = 0;

            if (l && l + l / 16 < stage_len) {
                size_t probe_raw = hz_minz(stage_len, (size_t)1 << 19);
                size_t probe_lrm = hz_minz(l, (size_t)1 << 19);

                if (probe_raw >= 8192 && probe_lrm >= 4096 &&
                    work_cap > probe_raw + probe_lrm + 65536) {
                    /* cost per byte of *original* data in each form */
                    size_t c_raw = hz_probe_cost(shufbuf, work_cap,
                                                 stage, probe_raw,
                                                 block_method, opts->level);
                    size_t c_lrm = hz_probe_cost(shufbuf, work_cap,
                                                 tmp, probe_lrm,
                                                 block_method, opts->level);
                    if (c_raw && c_lrm) {
                        /* scale the de-duplicated cost back to the same span
                         * of source bytes: probe_lrm bytes of filtered data
                         * stand for probe_lrm * (stage_len / l) originals */
                        double span_lrm = (double)probe_lrm * (double)stage_len / (double)l;
                        double per_raw  = (double)c_raw / (double)probe_raw;
                        double per_lrm  = (double)c_lrm / span_lrm;
                        keep = per_lrm < per_raw * 0.98;
                    }
                } else {
                    keep = 1;   /* too small to probe meaningfully */
                }
            }

            if (keep) {
                memcpy(work, tmp, l);
                stage_len = l;
                h.fid[h.nfilters] = HZ_F_LRM;
                h.fparam[h.nfilters] = 0;
                ++h.nfilters;
            }
        }

        if (opts->enable_exe && an.is_x86 && h.nfilters + 1 < HZ_MAX_FILTERS) {
            hz_exe_fwd(stage, stage_len);
            h.fid[h.nfilters] = HZ_F_EXE;
            h.fparam[h.nfilters] = 0;
            ++h.nfilters;
        }
        /* Byte transpose, decided the same way: by measurement.  It runs
         * before delta so that any delta afterwards operates within a
         * column, which is what makes the pair effective on numeric arrays. */
        if (opts->enable_delta && an.best_shuffle_width &&
            h.nfilters + 1 < HZ_MAX_FILTERS) {
            int w = an.best_shuffle_width;
            size_t probe = hz_minz(stage_len, (size_t)1 << 19);
            size_t off   = (stage_len - probe) / 2;
            int use_shuf = 0;

            probe -= probe % (size_t)w;
            if (probe >= 8192) {
                size_t half = work_cap / 2;
                if (half > probe + 8192) {
                    size_t plain_c = hz_probe_cost(pb_lo(tmp, half), half,
                                                   stage + off, probe,
                                                   block_method, opts->level);
                    memcpy(tmp + half, stage + off, probe);
                    hz_shuf_fwd(tmp + half, shufbuf, probe, w);
                    {
                        size_t shuf_c = hz_probe_cost(pb_lo(tmp, half), half,
                                                      tmp + half, probe,
                                                      block_method, opts->level);
                        if (plain_c && shuf_c)
                            use_shuf = (shuf_c + shuf_c / 64) < plain_c;
                    }
                }
            }
            if (use_shuf) {
                hz_shuf_fwd(stage, shufbuf, stage_len, w);
                h.fid[h.nfilters] = HZ_F_SHUF;
                h.fparam[h.nfilters] = (uint8_t)w;
                ++h.nfilters;
            }
        }

        /* Delta is the one filter whose benefit cannot be predicted reliably
         * from the data alone.  A residual with lower order-0 entropy still
         * loses whenever the context models were already exploiting the
         * original byte structure -- smooth images are the standard example:
         * the entropy test says yes, and measuring says it costs 5%.
         *
         * So do not guess.  Trial encode a slice both ways and keep the
         * winner.  The slice is small enough that the extra work is a few
         * percent of the block, and it converts a heuristic that is wrong
         * on real data into a decision that is right by construction. */
        if (opts->enable_delta && an.best_delta_stride &&
            h.nfilters + 1 < HZ_MAX_FILTERS) {
            int use_delta = 1;
            /* Probe a slice from the middle of the block: the start is
             * often a header or a warm-up region that behaves nothing like
             * the bulk, and judging the whole block by it is how the
             * previous heuristic went wrong. */
            size_t probe = hz_minz(stage_len, (size_t)1 << 19);
            size_t off   = (stage_len - probe) / 2;

            if (probe >= 4096) {
                size_t plain_c, delta_c;
                uint8_t *pb = tmp;
                size_t half = work_cap / 2;

                if (half > probe + 8192) {
                    plain_c = hz_probe_cost(pb, half, stage + off, probe,
                                            block_method, opts->level);
                    memcpy(pb + half, stage + off, probe);
                    hz_delta_fwd(pb + half, probe, an.best_delta_stride);
                    delta_c = hz_probe_cost(pb, half, pb + half, probe,
                                            block_method, opts->level);
                    /* Require a clear win: the filter costs a pass over the
                     * data and adds a header, so a coin-flip margin is not
                     * worth taking. */
                    if (plain_c && delta_c)
                        use_delta = (delta_c + delta_c / 64) < plain_c;
                }
            }

            if (use_delta) {
                hz_delta_fwd(stage, stage_len, an.best_delta_stride);
                h.fid[h.nfilters] = HZ_F_DELTA;
                h.fparam[h.nfilters] = (uint8_t)an.best_delta_stride;
                ++h.nfilters;
            }
        }

        h.usize = (uint32_t)n;

        /* ---- entropy stage ---- */
        {

            int method = block_method;
            h.method = (uint8_t)method;
            hsz = bhdr_size(&h);
            if (hsz + 8 > jb->cap) { jb->err = HYDRA_E_DSTSIZE; return; }

            if (method == HZ_M_CM) {
                csz = hz_cm_compress(jb->buf + hsz, jb->cap - hsz,
                                     stage, stage_len, opts->level);
                used_cm = 1;
            } else if (method == HZ_M_MID) {
                csz = hz_mid_compress(jb->buf + hsz, jb->cap - hsz,
                                      stage, stage_len, opts->level);
            } else if (method == HZ_M_FAST) {
                csz = hz_fast_compress(jb->buf + hsz, jb->cap - hsz,
                                       stage, stage_len, opts->level);
            }

            /* Fall back to storing the *original* bytes whenever the coded
             * form is not smaller.  Note this compares against n, not
             * stage_len: a filter that expanded the data must not be able
             * to make the stored form larger than the input. */
            if (csz == 0 || csz >= n) {
                h.method = HZ_M_RAW;
                h.nfilters = 0;
                hsz = bhdr_size(&h);
                if (hsz + n > jb->cap) { jb->err = HYDRA_E_DSTSIZE; return; }
                memcpy(jb->buf + hsz, blk, n);
                csz = n;
                stage_len = n;
                used_cm = 0;
            }
        }

        h.csize = (uint32_t)csz;
        /* usize is what the entropy stage produces -- the length the filters
         * are undone from.  osize is what the block contributes to the final
         * output, which differs whenever a filter changes the length.
         *
         * These were conflated before, and the LRM filter is exactly the
         * case where they differ: it shrinks the data, so the decoder sized
         * the block by the filtered length and every later block landed at
         * the wrong offset.  The old code papered over it by assuming an
         * LRM block is always the last one -- true for a single-block frame,
         * false the moment the input needs two. */
        h.usize = (uint32_t)stage_len;
        h.osize = (uint32_t)n;
        bhdr_write(jb->buf, &h);
        jb->len = hsz + csz;

        if (opts->verbose) {
            fprintf(stderr,
                "[hydra] block %8zu -> %8zu  m=%s filters=%d delta=%d x86=%d lrm=%d H0=%.2f\n",
                n, csz,
                h.method == HZ_M_RAW ? "raw" :
                    (used_cm ? "cm" : (h.method == HZ_M_MID ? "mid" : "fast")),
                h.nfilters, an.best_delta_stride, an.is_x86, an.lrm_worth,
                an.order0_entropy);
        }

        /* The frame stores the original block length separately from the
         * filtered length, so record it now that the header is written. */
}

int64_t hydra_compress(void *dstv, size_t dst_cap,
                       const void *srcv, size_t src_size,
                       const hydra_opts *opts_in)
{
    const uint8_t *src = (const uint8_t *)srcv;
    uint8_t *dst = (uint8_t *)dstv;
    hydra_opts opts;
    size_t out = 0, bsize, work_cap;
    hz_cjob_ctx ctx;
    hz_jobbuf *jobs = NULL;
    int njobs = 0, nthreads, i, allocated = 0;
    int64_t rc = HYDRA_E_INTERNAL;

    if (!dst || (!src && src_size)) return HYDRA_E_PARAM;

    if (opts_in) opts = *opts_in; else hydra_opts_init(&opts, HYDRA_LEVEL_DEFAULT);
    if (opts.level < HYDRA_LEVEL_MIN || opts.level > HYDRA_LEVEL_MAX)
        return HYDRA_E_PARAM;
    if (opts.block_log == 0) opts.block_log = auto_block_log(opts.level, src_size);
    if (opts.block_log < HZ_BLOCK_LOG_MIN || opts.block_log > HZ_BLOCK_LOG_MAX)
        return HYDRA_E_PARAM;
    if (opts.threads < 0) return HYDRA_E_PARAM;

    hz_tables_init();
    bsize = (size_t)1 << opts.block_log;

    /* frame header */
    if (dst_cap < 14) return HYDRA_E_DSTSIZE;
    dst[0] = HZ_MAGIC0; dst[1] = HZ_MAGIC1; dst[2] = HZ_MAGIC2; dst[3] = HZ_MAGIC3;
    dst[4] = HZ_FORMAT_VERSION;
    dst[5] = (uint8_t)(HZ_FLAG_CSIZE | (opts.checksum ? HZ_FLAG_DIGEST : 0));
    hz_put64le(dst + 6, (uint64_t)src_size);
    out = 14;

    njobs = (int)((src_size + bsize - 1) / bsize);
    if (njobs == 0) njobs = 0;

    nthreads = opts.threads ? opts.threads : hz_cpu_count();
    if (nthreads < 1) nthreads = 1;
    if (nthreads > njobs) nthreads = njobs;
    if (nthreads > HZ_MAX_THREADS) nthreads = HZ_MAX_THREADS;

    work_cap = bsize + bsize / 4 + 65536;

    if (njobs > 0) {
        jobs = (hz_jobbuf *)calloc((size_t)njobs, sizeof(hz_jobbuf));
        if (!jobs) return HYDRA_E_NOMEM;

        /* Every concurrent worker needs its own scratch, and every block
         * needs somewhere to put its result.  Allocating per job rather
         * than per thread keeps the job function free of any notion of
         * which worker is running it. */
        for (i = 0; i < njobs; ++i) {
            size_t n = hz_minz(bsize, src_size - (size_t)i * bsize);
            jobs[i].cap     = n + n / 4 + 65536;
            jobs[i].buf     = (uint8_t *)malloc(jobs[i].cap);
            jobs[i].work    = (uint8_t *)malloc(work_cap);
            jobs[i].tmp     = (uint8_t *)malloc(work_cap);
            jobs[i].shufbuf = (uint8_t *)malloc(work_cap);
            if (!jobs[i].buf || !jobs[i].work || !jobs[i].tmp || !jobs[i].shufbuf) {
                rc = HYDRA_E_NOMEM; goto done;
            }
            ++allocated;
        }

        ctx.src = src; ctx.src_size = src_size; ctx.bsize = bsize;
        ctx.work_cap = work_cap; ctx.opts = &opts;
        ctx.jobs = jobs; ctx.njobs = njobs;

        hz_pool_run(hz_compress_block, &ctx, njobs, nthreads);

        for (i = 0; i < njobs; ++i) {
            if (jobs[i].err != HYDRA_OK) { rc = jobs[i].err; goto done; }
            if (jobs[i].len > dst_cap - out) { rc = HYDRA_E_DSTSIZE; goto done; }
            memcpy(dst + out, jobs[i].buf, jobs[i].len);
            out += jobs[i].len;
        }
    }

    if (out + 1 > dst_cap) { rc = HYDRA_E_DSTSIZE; goto done; }
    dst[out++] = HZ_BLK_END;

    if (opts.checksum) {
        if (out + 8 > dst_cap) { rc = HYDRA_E_DSTSIZE; goto done; }
        hz_put64le(dst + out, hydra_digest(src, src_size));
        out += 8;
    }

    rc = (int64_t)out;

done:
    if (jobs) {
        for (i = 0; i < njobs; ++i) {
            free(jobs[i].buf); free(jobs[i].work);
            free(jobs[i].tmp); free(jobs[i].shufbuf);
        }
        free(jobs);
    }
    (void)allocated;
    return rc;
}

/* =========================================================================
 * Decompression
 * ========================================================================= */
int64_t hydra_frame_content_size(const void *srcv, size_t src_size)
{
    const uint8_t *src = (const uint8_t *)srcv;
    if (src_size < 14) return HYDRA_E_SRCSIZE;
    if (src[0] != HZ_MAGIC0 || src[1] != HZ_MAGIC1 ||
        src[2] != HZ_MAGIC2 || src[3] != HZ_MAGIC3) return HYDRA_E_BADMAGIC;
    if (src[4] != HZ_FORMAT_VERSION) return HYDRA_E_BADVERSION;
    if (!(src[5] & HZ_FLAG_CSIZE)) return HYDRA_E_CORRUPT;
    return (int64_t)hz_get64le(src + 6);
}

/* ---- per-block decompression job ---------------------------------------
 * The scan pass below walks the block headers and records where each block
 * starts and how long its output is.  Once that index exists the blocks are
 * fully independent again, so decoding them is another fork-join.
 *
 * Scanning is cheap -- it reads two integers per block and skips the
 * payload -- and it has to happen anyway to validate the frame. */
typedef struct {
    const uint8_t *payload;
    size_t         csize;
    size_t         usize;      /* size the entropy stage produces */
    size_t         osize;      /* bytes contributed to the output  */
    size_t         outpos;     /* where this block lands in dst    */
    uint8_t        method;
    uint8_t        nfilters;
    uint8_t        fid[HZ_MAX_FILTERS];
    uint8_t        fparam[HZ_MAX_FILTERS];
    int            err;
} hz_dblk;

typedef struct {
    hz_dblk *blk;
    uint8_t *dst;
    size_t   dst_cap;
} hz_djob_ctx;

static void hz_decompress_block(void *vctx, int index)
{
    hz_djob_ctx *c = (hz_djob_ctx *)vctx;
    hz_dblk *b = &c->blk[index];
    uint8_t *tmp = NULL;
    size_t len;
    int i;

    b->err = HYDRA_OK;

    /* A block whose only filter is the long range one can decode straight
     * into the output; anything else needs a staging buffer because the
     * filters run in place on the entropy stage's result. */
    tmp = (uint8_t *)malloc(b->usize + 64);
    if (!tmp) { b->err = HYDRA_E_NOMEM; return; }

    if (b->method == HZ_M_RAW) {
        if (b->csize != b->usize) { b->err = HYDRA_E_CORRUPT; goto out; }
        memcpy(tmp, b->payload, b->csize);
    } else if (b->method == HZ_M_FAST) {
        if (hz_fast_decompress(tmp, b->usize, b->payload, b->csize) != 0) {
            b->err = HYDRA_E_CORRUPT; goto out;
        }
    } else if (b->method == HZ_M_MID) {
        if (hz_mid_decompress(tmp, b->usize, b->payload, b->csize) != 0) {
            b->err = HYDRA_E_CORRUPT; goto out;
        }
    } else if (b->method == HZ_M_SGI) {
        if (hz_sgi_decompress(tmp, b->usize, b->payload, b->csize) != 0) {
            b->err = HYDRA_E_CORRUPT; goto out;
        }
    } else {
        if (hz_cm_decompress(tmp, b->usize, b->payload, b->csize) != 0) {
            b->err = HYDRA_E_CORRUPT; goto out;
        }
    }

    len = b->usize;
    for (i = (int)b->nfilters - 1; i >= 0; --i) {
        switch (b->fid[i]) {
            case HZ_F_DELTA:
                hz_delta_rev(tmp, len, b->fparam[i]);
                break;
            case HZ_F_EXE:
                hz_exe_rev(tmp, len);
                break;
            case HZ_F_SHUF: {
                uint8_t *sb = (uint8_t *)malloc(len ? len : 1);
                if (!sb) { b->err = HYDRA_E_NOMEM; goto out; }
                hz_shuf_rev(tmp, sb, len, b->fparam[i]);
                free(sb);
                break;
            }
            case HZ_F_LRM: {
                size_t outn = 0;
                if (i != 0) { b->err = HYDRA_E_CORRUPT; goto out; }
                if (hz_lrm_rev(c->dst + b->outpos, b->osize,
                               &outn, tmp, len) != 0) {
                    b->err = HYDRA_E_CORRUPT; goto out;
                }
                if (outn != b->osize) { b->err = HYDRA_E_CORRUPT; goto out; }
                free(tmp);
                return;
            }
            default:
                b->err = HYDRA_E_CORRUPT; goto out;
        }
    }

    if (len != b->osize) { b->err = HYDRA_E_CORRUPT; goto out; }
    if (len > c->dst_cap - b->outpos) { b->err = HYDRA_E_CORRUPT; goto out; }
    memcpy(c->dst + b->outpos, tmp, len);

out:
    free(tmp);
}

int64_t hydra_decompress(void *dstv, size_t dst_cap,
                         const void *srcv, size_t src_size)
{
    const uint8_t *src = (const uint8_t *)srcv;
    uint8_t *dst = (uint8_t *)dstv;
    size_t ip = 0, op = 0;
    uint64_t declared;
    int flags, i, nblk = 0, cap_blk = 0, nthreads;
    hz_dblk *blk = NULL;
    hz_djob_ctx ctx;
    int64_t rc = HYDRA_E_INTERNAL;

    if (!src || src_size < 15) return HYDRA_E_SRCSIZE;
    if (src[0] != HZ_MAGIC0 || src[1] != HZ_MAGIC1 ||
        src[2] != HZ_MAGIC2 || src[3] != HZ_MAGIC3) return HYDRA_E_BADMAGIC;
    if (src[4] != HZ_FORMAT_VERSION) return HYDRA_E_BADVERSION;
    flags = src[5];
    if (!(flags & HZ_FLAG_CSIZE)) return HYDRA_E_CORRUPT;
    declared = hz_get64le(src + 6);
    ip = 14;

    if (declared > dst_cap) return HYDRA_E_DSTSIZE;
    hz_tables_init();

    /* ---- pass 1: index the blocks and validate every header ---- */
    for (;;) {
        hz_dblk b;
        memset(&b, 0, sizeof(b));

        if (ip >= src_size) { rc = HYDRA_E_CORRUPT; goto done; }
        if (src[ip] == HZ_BLK_END) { ++ip; break; }

        if (ip + 2 > src_size) { rc = HYDRA_E_CORRUPT; goto done; }
        b.method   = src[ip++];
        b.nfilters = src[ip++];
        if (b.method > HZ_M_SGI || b.nfilters >= HZ_MAX_FILTERS) {
            rc = HYDRA_E_CORRUPT; goto done;
        }
        if (ip + 2u * b.nfilters + 12u > src_size) { rc = HYDRA_E_CORRUPT; goto done; }
        for (i = 0; i < (int)b.nfilters; ++i) {
            b.fid[i]    = src[ip++];
            b.fparam[i] = src[ip++];
        }
        b.usize = hz_get32le(src + ip); ip += 4;
        b.osize = hz_get32le(src + ip); ip += 4;
        b.csize = hz_get32le(src + ip); ip += 4;

        if (b.csize > src_size - ip) { rc = HYDRA_E_CORRUPT; goto done; }
        b.payload = src + ip;
        ip += b.csize;

        /* Where this block's output starts.  For an LRM block the final
         * length is only known after decoding, so such a block must be the
         * last one -- the encoder never produces LRM on any other block
         * because LRM only ever runs when the whole input is one block. */
        /* Placement uses osize, the block's true contribution.  Deriving it
         * from usize is wrong for any length-changing filter. */
        b.outpos = op;
        if (b.osize > dst_cap - op) { rc = HYDRA_E_CORRUPT; goto done; }
        op += b.osize;

        if (nblk == cap_blk) {
            int ncap = cap_blk ? cap_blk * 2 : 16;
            hz_dblk *nb = (hz_dblk *)realloc(blk, (size_t)ncap * sizeof(hz_dblk));
            if (!nb) { rc = HYDRA_E_NOMEM; goto done; }
            blk = nb; cap_blk = ncap;
        }
        blk[nblk++] = b;
    }

    if (op != (size_t)declared) { rc = HYDRA_E_CORRUPT; goto done; }

    /* ---- pass 2: decode the blocks, in parallel when there is more
     * than one ---- */
    if (nblk > 0) {
        nthreads = hz_cpu_count();
        if (nthreads > nblk) nthreads = nblk;

        ctx.blk = blk; ctx.dst = dst; ctx.dst_cap = dst_cap;
        hz_pool_run(hz_decompress_block, &ctx, nblk, nthreads);

        for (i = 0; i < nblk; ++i)
            if (blk[i].err != HYDRA_OK) { rc = blk[i].err; goto done; }
    }

    if (flags & HZ_FLAG_DIGEST) {
        if (ip + 8 > src_size) { rc = HYDRA_E_CORRUPT; goto done; }
        if (hz_get64le(src + ip) != hydra_digest(dst, (size_t)declared)) {
            rc = HYDRA_E_CHECKSUM; goto done;
        }
        ip += 8;
    }

    rc = (int64_t)declared;

done:
    free(blk);
    return rc;
}
