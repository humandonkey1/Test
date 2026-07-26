/* ===========================================================================
 * HYDRA / SGI - Structural Grammar Induction.  See hz_sgi.h for the idea.
 * ========================================================================= */
#include "hz_sgi.h"
#include "hz_rc.h"
#include "hz_model.h"

/* ===========================================================================
 * 1. RECORD INDUCTION
 *
 * Find the period at which structure repeats.  Two independent routes,
 * because machine generated data comes in two shapes:
 *
 *   delimited  - variable length rows ended by a separator (CSV, logs, JSON
 *                lines).  The separator is whichever byte appears with a
 *                strikingly regular spacing.
 *   fixed      - constant length records with no separator (binary tables,
 *                database pages, sensor frames).  Found by scanning for the
 *                period that maximises positional byte agreement.
 * ========================================================================= */

typedef struct {
    int      kind;        /* 0 = none, 1 = delimited, 2 = fixed width */
    uint8_t  delim;
    size_t   width;       /* fixed width, when kind == 2              */
    size_t   nrec;
    size_t   first;       /* offset of the first record               */
    size_t   reclen_best; /* record length chosen by the lane fitter  */
} sgi_layout;

/* Locate a delimiter: a byte whose occurrences are numerous and evenly
 * spaced.  Evenness matters more than count -- a space is common in prose
 * too, but its spacing is erratic, whereas a record separator is not. */
static int find_delimiter(const uint8_t *src, size_t n, uint8_t *out_delim,
                          size_t *out_avg)
{
    size_t count[256];
    size_t last[256];
    uint64_t var[256];
    size_t i, limit = n < (1u << 20) ? n : (1u << 20);
    int c, best = -1;
    double bestq = 0.0;

    memset(count, 0, sizeof(count));
    memset(last, 0, sizeof(last));
    memset(var, 0, sizeof(var));

    for (i = 0; i < limit; ++i) {
        uint8_t b = src[i];
        if (count[b]) {
            size_t gap = i - last[b];
            var[b] += (uint64_t)gap * gap;
        }
        last[b] = i;
        ++count[b];
    }

    for (c = 0; c < 256; ++c) {
        double mean, ex2, cv, q;
        if (count[c] < 16) continue;
        mean = (double)limit / (double)count[c];
        if (mean < 4.0 || mean > 4096.0) continue;
        ex2 = (double)var[c] / (double)(count[c] - 1);
        /* coefficient of variation of the gap; 0 = perfectly regular */
        cv = (ex2 / (mean * mean)) - 1.0;
        if (cv < 0.0) cv = 0.0;
        q = 1.0 / (1.0 + cv);

        /* Regularity alone is not structure.
         *
         * In a table of numbers the decimal point recurs with beautiful
         * regularity, and on records.csv it beat the newline outright --
         * which split the file across its own rows and destroyed every
         * column.  A record terminator is a specific, small set of bytes;
         * anything else has to be dramatically more regular before it is
         * believed to be one. */
        if (c == '\n' || c == '\r' || c == 0) q *= 2.5;
        else if (c == ';' || c == '|' || c == 0x1E) q *= 1.2;

        if (q > bestq) { bestq = q; best = c; *out_avg = (size_t)mean; }
    }

    if (best < 0 || bestq < 0.55) return 0;
    *out_delim = (uint8_t)best;
    return 1;
}

/* Fixed width: try every plausible period and keep the strongest, favouring
 * the smallest period among near-equals so we find the true record rather
 * than a multiple of it. */
/* Find the record width of a fixed layout.
 *
 * Raw positional agreement is the wrong test on its own.  A frame holding a
 * counter, a cycling id and a constant agrees at only a third of its byte
 * positions -- the counter changes every record by design -- so a threshold
 * tuned for "most bytes repeat" rejects exactly the structured binary data
 * this is meant to find.
 *
 * What identifies a record width is that the agreement is *concentrated*:
 * some byte positions agree almost always and others almost never.  A wrong
 * width smears agreement evenly across positions.  So the score below is the
 * count of positions that individually agree at least 90% of the time, and
 * the smallest width reaching a useful count wins. */
static size_t find_fixed_width(const uint8_t *src, size_t n)
{
    size_t p, best = 0;
    uint32_t bestcols = 0;

    for (p = 2; p <= 1024 && p * 16 < n; ++p) {
        size_t nrec = n / p, r, col;
        uint32_t strong = 0;
        size_t sample = nrec < 512 ? nrec : 512;
        if (sample < 8) continue;

        for (col = 0; col < p; ++col) {
            size_t agree = 0;
            for (r = 1; r < sample; ++r)
                if (src[r * p + col] == src[(r - 1) * p + col]) ++agree;
            if (agree * 10 >= (sample - 1) * 9) ++strong;
        }
        /* Take the *first* width that pins down a quarter of its columns.
         *
         * Every multiple of the true record width scores at least as well as
         * the width itself -- 1020 bytes "works" for a 12 byte frame because
         * it is 85 frames laid end to end.  Scanning upward and stopping at
         * the first qualifier returns the fundamental period rather than a
         * harmonic of it, which is what the lane fitter needs. */
        if (strong * 4 >= p) { bestcols = strong; best = p; break; }
    }
    (void)bestcols;
    return best;
}

/* Is this plausibly text?  A delimiter search on binary data finds spurious
 * newlines -- 0x0a occurs about once every 256 random bytes, which looks
 * regular enough to fool the gap statistics.  Requiring that most bytes be
 * printable costs one pass and removes the whole failure mode. */
static int looks_textual(const uint8_t *src, size_t n)
{
    size_t i, step = n / 8192, printable = 0, seen = 0;
    if (step == 0) step = 1;
    for (i = 0; i < n; i += step, ++seen) {
        uint8_t b = src[i];
        if ((b >= 0x20 && b < 0x7F) || b == '\n' || b == '\r' || b == '\t')
            ++printable;
    }
    return seen && printable * 10 >= seen * 9;
}

static void induce_layout(const uint8_t *src, size_t n, sgi_layout *L)
{
    uint8_t d = 0;
    size_t avg = 0;

    memset(L, 0, sizeof(*L));
    if (n < 1024) return;

    if (looks_textual(src, n) && find_delimiter(src, n, &d, &avg)) {
        size_t i, cnt = 0;
        for (i = 0; i < n; ++i) if (src[i] == d) ++cnt;
        if (cnt >= 8) {
            L->kind = 1;
            L->delim = d;
            L->nrec = cnt;
            L->first = 0;
            return;
        }
    }

    {
        size_t w = find_fixed_width(src, n);
        if (w) {
            L->kind = 2;
            L->width = w;
            L->nrec = n / w;
            L->first = 0;
        }
    }
}

/* ===========================================================================
 * 2. FIELD SEGMENTATION
 *
 * Within a record, split at positions where the character class changes the
 * same way across most records.  Classes are coarse on purpose: digit,
 * letter, and everything else.  A column boundary in real data is nearly
 * always a class transition, and using only three classes keeps the
 * detection stable when the *values* differ wildly between rows.
 * ========================================================================= */

HZ_INLINE int chclass(uint8_t c)
{
    if (c >= '0' && c <= '9') return 0;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
    return 2;
}

typedef struct {
    size_t start;      /* offset within the record  */
    size_t len;        /* fixed length, or 0 if variable */
} sgi_field;

/* ===========================================================================
 * 3. LAW INFERENCE - the per-field models
 * ========================================================================= */

typedef struct {
    uint8_t  law;
    uint8_t  width;         /* byte width of the rendered field       */
    uint8_t  pad;           /* '0' zero padded, ' ' space, 0 none     */
    uint8_t  nenum;
    uint32_t period;        /* CYCLE                                  */
    int64_t  base;          /* COUNTER / FLOAT_LIN                    */
    int64_t  step;
    uint8_t  decimals;      /* FLOAT_LIN                              */
    /* CONST / CYCLE / ENUM payload lives in the table below           */
    uint32_t tab_off;
    uint32_t tab_len;
} sgi_law;


/* Parse a decimal integer, returning 0 if the whole span is not one. */
static int parse_int(const uint8_t *p, size_t len, int64_t *out)
{
    int64_t v = 0;
    size_t i = 0;
    int neg = 0;
    if (!len || len > 18) return 0;
    if (p[0] == '-') { neg = 1; i = 1; if (len == 1) return 0; }
    for (; i < len; ++i) {
        if (p[i] < '0' || p[i] > '9') return 0;
        v = v * 10 + (p[i] - '0');
    }
    *out = neg ? -v : v;
    return 1;
}

/* Parse a fixed-point decimal such as "20.010" into scaled integer 20010
 * with decimals = 3.  Only a fixed number of decimals is accepted, which is
 * what machine generated output produces. */
static int parse_fixed(const uint8_t *p, size_t len, int64_t *out, int *dec)
{
    size_t i, dot = (size_t)-1;
    int64_t v = 0;
    int neg = 0;
    size_t start = 0;

    if (!len || len > 20) return 0;
    if (p[0] == '-') { neg = 1; start = 1; if (len == 1) return 0; }
    for (i = start; i < len; ++i) {
        if (p[i] == '.') {
            if (dot != (size_t)-1) return 0;
            dot = i;
            continue;
        }
        if (p[i] < '0' || p[i] > '9') return 0;
        v = v * 10 + (p[i] - '0');
    }
    if (dot == (size_t)-1) return 0;
    *dec = (int)(len - dot - 1);
    if (*dec < 1 || *dec > 9) return 0;
    *out = neg ? -v : v;
    return 1;
}

/* Render helpers, exact inverses of the parsers above. */
static size_t render_int(uint8_t *o, int64_t v, int width, int pad)
{
    uint8_t tmp[24];
    int n = 0, i;
    int neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
    size_t used;

    do { tmp[n++] = (uint8_t)('0' + (u % 10)); u /= 10; } while (u);
    if (neg) tmp[n++] = '-';

    used = 0;
    if (pad && width > n) {
        int k = width - n;
        /* zero padding goes after any sign, space padding before it */
        if (pad == '0' && neg) {
            o[used++] = '-';
            --n;
            while (k--) o[used++] = '0';
        } else {
            while (k--) o[used++] = (uint8_t)pad;
        }
    }
    for (i = n - 1; i >= 0; --i) o[used++] = tmp[i];
    return used;
}

static size_t render_fixed(uint8_t *o, int64_t v, int dec, int width, int pad)
{
    uint8_t tmp[32];
    int n = 0, i, k;
    int neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
    size_t used = 0;

    for (k = 0; k < dec; ++k) { tmp[n++] = (uint8_t)('0' + (u % 10)); u /= 10; }
    tmp[n++] = '.';
    do { tmp[n++] = (uint8_t)('0' + (u % 10)); u /= 10; } while (u);
    if (neg) tmp[n++] = '-';

    if (pad && width > n) {
        int j = width - n;
        while (j--) o[used++] = (uint8_t)pad;
    }
    for (i = n - 1; i >= 0; --i) o[used++] = tmp[i];
    return used;
}

/* ===========================================================================
 * Record splitting
 *
 * Produces, for each record, the span of every field.  Delimited records are
 * split on the delimiter; fixed width records are split at class boundaries
 * that hold across the sample.
 * ========================================================================= */

typedef struct {
    uint32_t off;      /* offset of the field within the whole input */
    uint32_t len;
} sgi_span;

typedef struct {
    size_t     nrec;
    int        nfield;
    sgi_span  *span;       /* nrec * nfield */
    uint8_t    fsep;       /* field separator, 0 if fixed width */
    uint8_t    rsep;       /* record separator, 0 if fixed width */
    size_t     reclen;     /* fixed record length, 0 if delimited */
    size_t     covered;    /* bytes of input accounted for */
} sgi_split;

static void sgi_split_free(sgi_split *s) { free(s->span); s->span = NULL; }

/* Choose the field separator inside a record: the most frequent byte that is
 * not the record separator and appears a consistent number of times per
 * record.  Consistency is the test -- a comma that appears exactly five
 * times in every row is a column separator; one that appears a varying
 * number of times is just punctuation. */
static int find_field_sep(const uint8_t *src, size_t n, uint8_t rsep,
                          uint8_t *out, int *out_count)
{
    size_t rowstart = 0, i;
    int c, counts[256], first[256], stable[256], rows = 0;
    int best = -1, bestn = 0;

    memset(counts, 0, sizeof(counts));
    memset(first, -1, sizeof(first));
    for (c = 0; c < 256; ++c) stable[c] = 1;

    for (i = 0; i < n && rows < 64; ++i) {
        if (src[i] != rsep) continue;
        {
            int rc[256];
            size_t k;
            memset(rc, 0, sizeof(rc));
            for (k = rowstart; k < i; ++k) ++rc[src[k]];
            for (c = 0; c < 256; ++c) {
                if (c == rsep) { stable[c] = 0; continue; }
                if (first[c] < 0) first[c] = rc[c];
                else if (rc[c] != first[c]) stable[c] = 0;
            }
            ++rows;
            rowstart = i + 1;
        }
    }
    if (rows < 4) return 0;

    for (c = 0; c < 256; ++c)
        if (stable[c] && first[c] > bestn) { bestn = first[c]; best = c; }

    if (best < 0 || bestn < 1) return 0;
    *out = (uint8_t)best;
    *out_count = bestn;
    return 1;
}

/* Split delimited input into records and fields. */
static int split_delimited(const uint8_t *src, size_t n, uint8_t rsep,
                           sgi_split *S)
{
    uint8_t fsep = 0;
    int fcount = 0, nfield;
    size_t i, rowstart = 0, rec = 0, nrec = 0;

    memset(S, 0, sizeof(*S));

    for (i = 0; i < n; ++i) if (src[i] == rsep) ++nrec;
    if (nrec < 8) return 0;

    if (find_field_sep(src, n, rsep, &fsep, &fcount)) {
        nfield = fcount + 1;
    } else {
        fsep = 0;
        nfield = 1;
    }
    if (nfield > SGI_MAX_FIELDS) return 0;

    S->span = (sgi_span *)malloc(nrec * (size_t)nfield * sizeof(sgi_span));
    if (!S->span) return 0;

    rowstart = 0;
    for (i = 0; i < n && rec < nrec; ++i) {
        if (src[i] != rsep) continue;
        {
            size_t p = rowstart, q;
            int f = 0;
            sgi_span *row = S->span + rec * (size_t)nfield;
            if (fsep) {
                for (q = rowstart; q <= i && f < nfield; ++q) {
                    if (q == i || src[q] == fsep) {
                        row[f].off = (uint32_t)p;
                        row[f].len = (uint32_t)(q - p);
                        ++f;
                        p = q + 1;
                    }
                }
            } else {
                row[0].off = (uint32_t)rowstart;
                row[0].len = (uint32_t)(i - rowstart);
                f = 1;
            }
            /* a row with the wrong field count means the layout guess was
             * wrong; bail rather than produce a broken model */
            if (f != nfield) { sgi_split_free(S); return 0; }
            ++rec;
            rowstart = i + 1;
        }
    }

    S->nrec    = rec;
    S->nfield  = nfield;
    S->fsep    = fsep;
    S->rsep    = rsep;
    S->reclen  = 0;
    S->covered = rowstart;      /* trailing bytes after the last rsep */
    return rec >= 8;
}

/* Split fixed width input.  Field boundaries are positions where the
 * character class changes in the same place across most records. */
static int split_fixed(const uint8_t *src, size_t n, size_t w, sgi_split *S)
{
    size_t nrec = n / w, r, i;
    int nfield = 0;
    uint8_t *isbound;
    size_t sample = nrec < 256 ? nrec : 256;

    memset(S, 0, sizeof(*S));
    if (nrec < 8 || w < 2) return 0;

    isbound = (uint8_t *)calloc(w + 1, 1);
    if (!isbound) return 0;

    for (i = 1; i < w; ++i) {
        size_t agree = 0;
        for (r = 0; r < sample; ++r) {
            const uint8_t *rp = src + r * w;
            if (chclass(rp[i]) != chclass(rp[i - 1])) ++agree;
        }
        if (agree * 4 >= sample * 3) isbound[i] = 1;
    }
    isbound[0] = 1;

    for (i = 0; i < w; ++i) if (isbound[i]) ++nfield;
    if (nfield == 0 || nfield > SGI_MAX_FIELDS) { free(isbound); return 0; }

    S->span = (sgi_span *)malloc(nrec * (size_t)nfield * sizeof(sgi_span));
    if (!S->span) { free(isbound); return 0; }

    for (r = 0; r < nrec; ++r) {
        sgi_span *row = S->span + r * (size_t)nfield;
        int f = 0;
        size_t start = 0;
        for (i = 1; i <= w; ++i) {
            if (i == w || isbound[i]) {
                row[f].off = (uint32_t)(r * w + start);
                row[f].len = (uint32_t)(i - start);
                ++f;
                start = i;
            }
        }
        if (f != nfield) { free(isbound); sgi_split_free(S); return 0; }
    }

    free(isbound);
    S->nrec    = nrec;
    S->nfield  = nfield;
    S->fsep    = 0;
    S->rsep    = 0;
    S->reclen  = w;
    S->covered = nrec * w;
    return 1;
}

/* ===========================================================================
 * 3. LAW INFERENCE
 *
 * For one column, search the law space in order of decreasing payoff.  The
 * first law that explains every sampled record wins; a law that explains
 * only most of them is rejected outright, because a per-record exception
 * list costs more than it saves and reintroduces exactly the per-record cost
 * the whole design is trying to remove.
 * ========================================================================= */

typedef struct {
    uint8_t  law;
    uint8_t  decimals;
    uint8_t  pad;
    uint8_t  fixlen;      /* rendered width, 0 = natural */
    uint32_t period;
    int64_t  base;
    int64_t  step;
    /* literal table for CONST / CYCLE / ENUM */
    uint8_t *tab;
    uint32_t tablen;
    uint32_t nval;
    uint32_t valwidth;    /* fixed width of each table entry, 0 = varying */
} sgi_fieldlaw;

static void law_free(sgi_fieldlaw *L) { free(L->tab); L->tab = NULL; }

HZ_INLINE const uint8_t *fval(const uint8_t *src, const sgi_split *S,
                              size_t rec, int f, uint32_t *len)
{
    const sgi_span *sp = &S->span[rec * (size_t)S->nfield + (size_t)f];
    *len = sp->len;
    return src + sp->off;
}

/* -- CONST: identical in every record ------------------------------------- */
static int try_const(const uint8_t *src, const sgi_split *S, int f,
                     sgi_fieldlaw *L)
{
    uint32_t l0, l;
    const uint8_t *v0 = fval(src, S, 0, f, &l0);
    size_t r;
    for (r = 1; r < S->nrec; ++r) {
        const uint8_t *v = fval(src, S, r, f, &l);
        if (l != l0 || memcmp(v, v0, l0) != 0) return 0;
    }
    L->law = SGI_LAW_CONST;
    L->tab = (uint8_t *)malloc(l0 ? l0 : 1);
    if (!L->tab) return 0;
    memcpy(L->tab, v0, l0);
    L->tablen = l0;
    L->nval = 1;
    L->valwidth = l0;
    return 1;
}

/* -- CYCLE: the column repeats with some period --------------------------- */
static int try_cycle(const uint8_t *src, const sgi_split *S, int f,
                     sgi_fieldlaw *L)
{
    size_t p, r;
    size_t maxp = S->nrec / 4;
    if (maxp > SGI_MAX_CYCLE) maxp = SGI_MAX_CYCLE;

    for (p = 1; p <= maxp; ++p) {
        int ok = 1;
        for (r = p; r < S->nrec; ++r) {
            uint32_t la, lb;
            const uint8_t *a = fval(src, S, r, f, &la);
            const uint8_t *b = fval(src, S, r - p, f, &lb);
            if (la != lb || memcmp(a, b, la) != 0) { ok = 0; break; }
        }
        if (ok) {
            size_t total = 0, off = 0;
            uint32_t l;
            for (r = 0; r < p; ++r) { fval(src, S, r, f, &l); total += l + 1; }
            L->tab = (uint8_t *)malloc(total ? total : 1);
            if (!L->tab) return 0;
            for (r = 0; r < p; ++r) {
                const uint8_t *v = fval(src, S, r, f, &l);
                L->tab[off++] = (uint8_t)l;
                memcpy(L->tab + off, v, l);
                off += l;
            }
            L->law = SGI_LAW_CYCLE;
            L->period = (uint32_t)p;
            L->tablen = (uint32_t)off;
            L->nval = (uint32_t)p;
            return 1;
        }
    }
    return 0;
}

/* -- COUNTER: integer advancing by a constant step ------------------------ */
static int try_counter(const uint8_t *src, const sgi_split *S, int f,
                       sgi_fieldlaw *L)
{
    int64_t v0, v1, step;
    uint32_t l0, l1, l;
    const uint8_t *p0 = fval(src, S, 0, f, &l0);
    const uint8_t *p1 = fval(src, S, 1, f, &l1);
    size_t r;
    int pad = 0, fixlen = 0;

    if (S->nrec < 3) return 0;
    if (!parse_int(p0, l0, &v0) || !parse_int(p1, l1, &v1)) return 0;
    step = v1 - v0;

    /* zero padded fields keep a constant width; detect it so the renderer
     * reproduces the exact bytes */
    if (l0 == l1 && l0 > 1 && p0[0] == '0') { pad = '0'; fixlen = (int)l0; }

    for (r = 2; r < S->nrec; ++r) {
        int64_t v;
        const uint8_t *p = fval(src, S, r, f, &l);
        if (!parse_int(p, l, &v)) return 0;
        if (v != v0 + step * (int64_t)r) return 0;
        if (fixlen && (int)l != fixlen) return 0;
    }
    L->law = SGI_LAW_COUNTER;
    L->base = v0;
    L->step = step;
    L->pad = (uint8_t)pad;
    L->fixlen = (uint8_t)fixlen;
    return 1;
}

/* -- FLOAT_LIN: fixed-point decimal advancing by a constant step ---------- */
static int try_float_lin(const uint8_t *src, const sgi_split *S, int f,
                         sgi_fieldlaw *L)
{
    int64_t v0, v1, step;
    int d0, d1, d;
    uint32_t l0, l1, l;
    const uint8_t *p0 = fval(src, S, 0, f, &l0);
    const uint8_t *p1 = fval(src, S, 1, f, &l1);
    size_t r;

    if (S->nrec < 3) return 0;
    if (!parse_fixed(p0, l0, &v0, &d0)) return 0;
    if (!parse_fixed(p1, l1, &v1, &d1)) return 0;
    if (d0 != d1) return 0;
    step = v1 - v0;

    for (r = 2; r < S->nrec; ++r) {
        int64_t v;
        const uint8_t *p = fval(src, S, r, f, &l);
        if (!parse_fixed(p, l, &v, &d)) return 0;
        if (d != d0) return 0;
        if (v != v0 + step * (int64_t)r) return 0;
    }
    L->law = SGI_LAW_FLOAT_LIN;
    L->base = v0;
    L->step = step;
    L->decimals = (uint8_t)d0;
    return 1;
}

/* -- ENUM: few distinct values ------------------------------------------- */
static int try_enum(const uint8_t *src, const sgi_split *S, int f,
                    sgi_fieldlaw *L)
{
    uint8_t *tab;
    uint32_t *toff;
    uint32_t n = 0, total = 0, cap = 4096;
    size_t r;

    tab = (uint8_t *)malloc(cap);
    toff = (uint32_t *)malloc(SGI_MAX_ENUM * sizeof(uint32_t));
    if (!tab || !toff) { free(tab); free(toff); return 0; }

    for (r = 0; r < S->nrec; ++r) {
        uint32_t l, i;
        const uint8_t *v = fval(src, S, r, f, &l);
        int found = 0;
        if (l > 255) { free(tab); free(toff); return 0; }
        for (i = 0; i < n; ++i) {
            const uint8_t *e = tab + toff[i];
            if (e[0] == (uint8_t)l && memcmp(e + 1, v, l) == 0) { found = 1; break; }
        }
        if (found) continue;
        if (n >= SGI_MAX_ENUM) { free(tab); free(toff); return 0; }
        while (total + l + 1 > cap) {
            uint8_t *nt = (uint8_t *)realloc(tab, cap * 2);
            if (!nt) { free(tab); free(toff); return 0; }
            tab = nt; cap *= 2;
        }
        toff[n] = total;
        tab[total++] = (uint8_t)l;
        memcpy(tab + total, v, l);
        total += l;
        ++n;
    }

    free(toff);
    /* Only worth it when the alphabet is genuinely small relative to the
     * number of records; otherwise the table is the data. */
    if (n * 8 > S->nrec) { free(tab); return 0; }
    L->law = SGI_LAW_ENUM;
    L->tab = tab;
    L->tablen = total;
    L->nval = n;
    return 1;
}

static void infer_field(const uint8_t *src, const sgi_split *S, int f,
                        sgi_fieldlaw *L)
{
    memset(L, 0, sizeof(*L));
    if (try_const(src, S, f, L)) return;
    if (try_counter(src, S, f, L)) return;
    if (try_float_lin(src, S, f, L)) return;
    if (try_cycle(src, S, f, L)) return;
    if (try_enum(src, S, f, L)) return;
    L->law = SGI_LAW_RESIDUAL;
}


/* ===========================================================================
 * BINARY FIELD LAWS
 *
 * The text laws above parse digits.  An enormous amount of machine generated
 * data is not text at all: sensor frames, network captures, database pages,
 * serialised structs.  Those hold the same laws -- a counter, a constant, a
 * cycling identifier -- encoded as little endian integers rather than ASCII.
 *
 * Reading them needs nothing new conceptually, only a different accessor.
 * A fixed width record is split into aligned integer lanes of 1, 2, 4 and 8
 * bytes, and each lane is tested for the same laws.  A lane that carries a
 * law costs nothing per record, exactly as before.
 * ========================================================================= */

/* Read a little endian unsigned integer of `w` bytes. */
HZ_INLINE uint64_t bin_get(const uint8_t *p, int w)
{
    uint64_t v = 0;
    int i;
    for (i = w - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

HZ_INLINE void bin_put(uint8_t *p, uint64_t v, int w)
{
    int i;
    for (i = 0; i < w; ++i) { p[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}

/* Fit a law to one aligned lane of a fixed width record.
 *
 * `off` is the byte offset of the lane inside the record and `w` its width.
 * Returns the law, filling *base / *step / *period as appropriate.
 *
 * Steps are computed modulo 2^(8w) so that a wrapping counter -- which real
 * hardware produces constantly -- is still recognised as a counter rather
 * than dismissed at the wrap. */
static int bin_fit(const uint8_t *src, size_t nrec, size_t reclen,
                   size_t off, int w, uint64_t *base, uint64_t *step,
                   uint32_t *period)
{
    uint64_t mask = (w >= 8) ? ~0ull : ((1ull << (8 * w)) - 1);
    uint64_t v0, v1, d;
    size_t r;

    if (nrec < 3) return SGI_LAW_RESIDUAL;

    v0 = bin_get(src + off, w);
    v1 = bin_get(src + reclen + off, w);

    /* constant */
    {
        int same = 1;
        for (r = 1; r < nrec; ++r)
            if (bin_get(src + r * reclen + off, w) != v0) { same = 0; break; }
        if (same) { *base = v0; return SGI_LAW_BCONST; }
    }

    /* arithmetic progression, wrapping */
    d = (v1 - v0) & mask;
    {
        int ok = 1;
        uint64_t expect = v1;
        for (r = 2; r < nrec; ++r) {
            expect = (expect + d) & mask;
            if (bin_get(src + r * reclen + off, w) != expect) { ok = 0; break; }
        }
        if (ok) { *base = v0; *step = d; return SGI_LAW_BCOUNTER; }
    }

    /* cycle: the lane repeats with some period */
    {
        size_t p, maxp = nrec / 3;
        if (maxp > SGI_MAX_CYCLE) maxp = SGI_MAX_CYCLE;
        for (p = 1; p <= maxp; ++p) {
            int ok = 1;
            for (r = p; r < nrec; ++r) {
                if (bin_get(src + r * reclen + off, w) !=
                    bin_get(src + (r - p) * reclen + off, w)) { ok = 0; break; }
            }
            if (ok) { *period = (uint32_t)p; return SGI_LAW_BCYCLE; }
        }
    }

    return SGI_LAW_RESIDUAL;
}

/* A lane assignment for a fixed width record: which widths at which offsets. */
typedef struct {
    uint8_t  off;
    uint8_t  w;
    uint8_t  law;
    uint64_t base;
    uint64_t step;
    uint32_t period;
    uint32_t tab_off;    /* offset of this lane's cycle table in the blob */
} sgi_lane;

/* Choose a lane layout for a record of `reclen` bytes.
 *
 * Wide lanes are tried first at each position: a 4 byte counter read as four
 * 1 byte lanes looks like noise in the upper bytes, so finding the widest
 * lane that carries a law is what makes the difference.  Anything left over
 * becomes 1 byte residual lanes. */
static int bin_layout(const uint8_t *src, size_t nrec, size_t reclen,
                      sgi_lane *lane, int maxlane)
{
    size_t off = 0;
    int n = 0;

    while (off < reclen && n < maxlane) {
        int widths[4], nw = 0, wi, chosen = 0;
        uint64_t base = 0, step = 0;
        uint32_t period = 0;
        int law = SGI_LAW_RESIDUAL;

        if (reclen - off >= 8 && (off % 8) == 0) widths[nw++] = 8;
        if (reclen - off >= 4 && (off % 4) == 0) widths[nw++] = 4;
        if (reclen - off >= 2 && (off % 2) == 0) widths[nw++] = 2;
        widths[nw++] = 1;

        for (wi = 0; wi < nw; ++wi) {
            uint64_t b = 0, s = 0;
            uint32_t p = 0;
            int l = bin_fit(src, nrec, reclen, off, widths[wi], &b, &s, &p);
            if (l != SGI_LAW_RESIDUAL) {
                chosen = widths[wi]; law = l; base = b; step = s; period = p;
                break;
            }
        }
        if (!chosen) chosen = 1;      /* residual, one byte at a time */

        lane[n].off    = (uint8_t)off;
        lane[n].w      = (uint8_t)chosen;
        lane[n].law    = (uint8_t)law;
        lane[n].base   = base;
        lane[n].step   = step;
        lane[n].period = period;
        ++n;
        off += (size_t)chosen;
    }
    return (off == reclen) ? n : 0;
}

/* ===========================================================================
 * 4. SERIALISATION
 *
 * Header: layout, then one descriptor per field.  A lawful field's entire
 * cost lives here and does not grow with the record count -- that is the
 * whole point of the design.
 *
 * Residual fields are then emitted column-wise: every value of field 3
 * contiguously, then every value of field 4.  Values of one column resemble
 * each other far more than the bytes beside them in the original row, so
 * whatever codes this stream afterwards sees far tighter statistics than it
 * would on the interleaved original.
 * ========================================================================= */

#define SGI_MAGIC 0xA7

HZ_INLINE void sgi_put_v(uint8_t **o, uint64_t v)
{
    while (v >= 128) { *(*o)++ = (uint8_t)(v | 128u); v >>= 7; }
    *(*o)++ = (uint8_t)v;
}
HZ_INLINE uint64_t sgi_get_v(const uint8_t **p, const uint8_t *e, int *err)
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
HZ_INLINE void sgi_put_s(uint8_t **o, int64_t v)
{
    sgi_put_v(o, (uint64_t)((v << 1) ^ (v >> 63)));   /* zigzag */
}
HZ_INLINE int64_t sgi_get_s(const uint8_t **p, const uint8_t *e, int *err)
{
    uint64_t u = sgi_get_v(p, e, err);
    return (int64_t)((u >> 1) ^ (~(u & 1) + 1));
}


/* ===========================================================================
 * BINARY PATH: serialise and replay a lane layout
 *
 * Header:  magic, kind=3, reclen, nrec, nlane, total, then per lane
 *          (off, w, law, and the law's parameters).
 * Payload: cycle tables, then residual lanes column-wise, then the tail.
 * ========================================================================= */
#define SGI_KIND_BIN 3

static size_t sgi_bin_compress(uint8_t *dst, size_t dst_cap,
                               const uint8_t *src, size_t n, size_t reclen)
{
    sgi_lane lane[SGI_MAX_FIELDS];
    uint8_t *o = dst, *omax = dst + dst_cap;
    size_t nrec = n / reclen, r;
    int nl, i;
    size_t lawful = 0;

    if (nrec < 16) return 0;
    nl = bin_layout(src, nrec, reclen, lane, SGI_MAX_FIELDS);
    if (nl <= 0) return 0;

    for (i = 0; i < nl; ++i)
        if (lane[i].law != SGI_LAW_RESIDUAL) lawful += lane[i].w;
    /* need most of the record explained, else the generic engines do better */
    if (lawful * 2 < reclen) return 0;

    if ((size_t)(omax - o) < 64) return 0;
    *o++ = SGI_MAGIC;
    *o++ = SGI_KIND_BIN;
    sgi_put_v(&o, reclen);
    sgi_put_v(&o, nrec);
    sgi_put_v(&o, (uint64_t)nl);
    sgi_put_v(&o, n);

    for (i = 0; i < nl; ++i) {
        if ((size_t)(omax - o) < 32) return 0;
        *o++ = lane[i].off;
        *o++ = lane[i].w;
        *o++ = lane[i].law;
        switch (lane[i].law) {
            case SGI_LAW_BCONST:
                sgi_put_v(&o, lane[i].base);
                break;
            case SGI_LAW_BCOUNTER:
                sgi_put_v(&o, lane[i].base);
                sgi_put_v(&o, lane[i].step);
                break;
            case SGI_LAW_BCYCLE:
                sgi_put_v(&o, lane[i].period);
                break;
            default:
                break;
        }
    }

    /* cycle tables: period entries of w bytes each */
    for (i = 0; i < nl; ++i) {
        if (lane[i].law != SGI_LAW_BCYCLE) continue;
        if ((size_t)(omax - o) < (size_t)lane[i].period * lane[i].w + 8) return 0;
        for (r = 0; r < lane[i].period; ++r) {
            memcpy(o, src + r * reclen + lane[i].off, lane[i].w);
            o += lane[i].w;
        }
    }

    /* residual lanes, column-wise */
    for (i = 0; i < nl; ++i) {
        if (lane[i].law != SGI_LAW_RESIDUAL) continue;
        if ((size_t)(omax - o) < nrec * lane[i].w + 8) return 0;
        for (r = 0; r < nrec; ++r) {
            memcpy(o, src + r * reclen + lane[i].off, lane[i].w);
            o += lane[i].w;
        }
    }

    /* tail: bytes after the last whole record */
    {
        size_t tail = n - nrec * reclen;
        if ((size_t)(omax - o) < tail + 8) return 0;
        sgi_put_v(&o, tail);
        memcpy(o, src + nrec * reclen, tail);
        o += tail;
    }
    return (size_t)(o - dst);
}

static int sgi_bin_decompress(uint8_t *dst, size_t n,
                              const uint8_t *src, size_t src_size)
{
    const uint8_t *p = src, *e = src + src_size;
    sgi_lane lane[SGI_MAX_FIELDS];
    const uint8_t *ctab[SGI_MAX_FIELDS];
    const uint8_t *rescur[SGI_MAX_FIELDS];
    size_t reclen, nrec, declared, r;
    int nl, i, err = 0;

    p += 2;                                   /* magic + kind */
    reclen   = (size_t)sgi_get_v(&p, e, &err);
    nrec     = (size_t)sgi_get_v(&p, e, &err);
    nl       = (int)sgi_get_v(&p, e, &err);
    declared = (size_t)sgi_get_v(&p, e, &err);
    if (err || nl <= 0 || nl > SGI_MAX_FIELDS) return -1;
    if (declared != n || reclen == 0 || reclen > 4096) return -1;
    if (nrec > n / reclen) return -1;

    for (i = 0; i < nl; ++i) {
        if (p + 3 > e) return -1;
        lane[i].off = *p++;
        lane[i].w   = *p++;
        lane[i].law = *p++;
        lane[i].base = lane[i].step = 0;
        lane[i].period = 0;
        if (lane[i].w < 1 || lane[i].w > 8) return -1;
        if ((size_t)lane[i].off + lane[i].w > reclen) return -1;
        switch (lane[i].law) {
            case SGI_LAW_BCONST:
                lane[i].base = sgi_get_v(&p, e, &err); break;
            case SGI_LAW_BCOUNTER:
                lane[i].base = sgi_get_v(&p, e, &err);
                lane[i].step = sgi_get_v(&p, e, &err); break;
            case SGI_LAW_BCYCLE:
                lane[i].period = (uint32_t)sgi_get_v(&p, e, &err);
                if (lane[i].period == 0) err = 1;
                break;
            case SGI_LAW_RESIDUAL: break;
            default: return -1;
        }
        if (err) return -1;
    }

    for (i = 0; i < nl; ++i) {
        if (lane[i].law != SGI_LAW_BCYCLE) continue;
        if ((size_t)(e - p) < (size_t)lane[i].period * lane[i].w) return -1;
        ctab[i] = p;
        p += (size_t)lane[i].period * lane[i].w;
    }
    for (i = 0; i < nl; ++i) {
        if (lane[i].law != SGI_LAW_RESIDUAL) continue;
        if ((size_t)(e - p) < nrec * lane[i].w) return -1;
        rescur[i] = p;
        p += nrec * lane[i].w;
    }

    if (nrec * reclen > n) return -1;
    for (r = 0; r < nrec; ++r) {
        uint8_t *rec = dst + r * reclen;
        for (i = 0; i < nl; ++i) {
            uint8_t *f = rec + lane[i].off;
            int w = lane[i].w;
            uint64_t mask = (w >= 8) ? ~0ull : ((1ull << (8 * w)) - 1);
            switch (lane[i].law) {
                case SGI_LAW_BCONST:
                    bin_put(f, lane[i].base, w);
                    break;
                case SGI_LAW_BCOUNTER:
                    bin_put(f, (lane[i].base + lane[i].step * (uint64_t)r) & mask, w);
                    break;
                case SGI_LAW_BCYCLE:
                    memcpy(f, ctab[i] + (size_t)(r % lane[i].period) * w, (size_t)w);
                    break;
                default:
                    memcpy(f, rescur[i], (size_t)w);
                    rescur[i] += w;
                    break;
            }
        }
    }

    {
        uint64_t tail = sgi_get_v(&p, e, &err);
        size_t pos = nrec * reclen;
        if (err || (size_t)(e - p) < tail || pos + tail != n) return -1;
        memcpy(dst + pos, p, (size_t)tail);
    }
    return 0;
}

size_t hz_sgi_compress(uint8_t *dst, size_t dst_cap,
                       const uint8_t *src, size_t n, int level)
{
    sgi_layout LO;
    sgi_split  S;
    sgi_fieldlaw *laws = NULL;
    uint8_t *o = dst, *omax = dst + dst_cap;
    int f, split_ok = 0, nresid = 0;
    size_t lawful_bytes = 0, total_bytes = 0;

    (void)level;
    if (n < 4096 || dst_cap < 64) return 0;

    induce_layout(src, n, &LO);

    /* Fixed width records get the binary lane fitter first.  The text path
     * below parses digits, so an integer counter stored as four raw bytes is
     * invisible to it -- and that is most machine generated binary data. */
    if (LO.kind == 2 && LO.width >= 1 && LO.width <= 4096) {
        /* The detector returns the smallest period that pins down columns,
         * but the true record is often a multiple of it: a 12 byte frame
         * whose first field is a 4 byte counter looks 4-periodic, because
         * the counter's low byte alone repeats the pattern.  Try the
         * detected width and a few multiples, keep whichever fits most of
         * the record under a law. */
        static const int MULT[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
        size_t bestz = 0;
        int mi;
        for (mi = 0; mi < (int)(sizeof(MULT) / sizeof(MULT[0])); ++mi) {
            size_t w = LO.width * (size_t)MULT[mi];
            size_t bz;
            if (w < 2 || w > 4096 || w * 16 > n) continue;
            bz = sgi_bin_compress(dst, dst_cap, src, n, w);
            if (bz && (bestz == 0 || bz < bestz)) {
                /* keep the best; re-encode at the end so dst holds it */
                bestz = bz;
                LO.reclen_best = w;
            }
        }
        if (bestz) {
            size_t bz = sgi_bin_compress(dst, dst_cap, src, n, LO.reclen_best);
            if (bz) return bz;
        }
    }

    if (LO.kind == 1)      split_ok = split_delimited(src, n, LO.delim, &S);
    else if (LO.kind == 2) split_ok = split_fixed(src, n, LO.width, &S);
    if (!split_ok) return 0;

    laws = (sgi_fieldlaw *)calloc((size_t)S.nfield, sizeof(sgi_fieldlaw));
    if (!laws) { sgi_split_free(&S); return 0; }

    for (f = 0; f < S.nfield; ++f) {
        size_t r, bytes = 0;
        infer_field(src, &S, f, &laws[f]);
        for (r = 0; r < S.nrec; ++r) { uint32_t l; fval(src, &S, r, f, &l); bytes += l; }
        total_bytes += bytes;
        if (laws[f].law == SGI_LAW_RESIDUAL) ++nresid;
        else lawful_bytes += bytes;
    }

    /* Decline unless the laws explain a worthwhile share.  Below this the
     * column-wise reshuffle is not worth the header, and the generic
     * engines will do better on the original layout. */
    if (total_bytes == 0 || lawful_bytes * 5 < total_bytes) {
        for (f = 0; f < S.nfield; ++f) law_free(&laws[f]);
        free(laws); sgi_split_free(&S);
        return 0;
    }

    /* ---- header ---- */
    if ((size_t)(omax - o) < 64) goto fail;
    *o++ = SGI_MAGIC;
    *o++ = (uint8_t)LO.kind;
    *o++ = S.rsep;
    *o++ = S.fsep;
    sgi_put_v(&o, S.reclen);
    sgi_put_v(&o, S.nrec);
    sgi_put_v(&o, (uint64_t)S.nfield);
    sgi_put_v(&o, n);
    sgi_put_v(&o, S.covered);

    for (f = 0; f < S.nfield; ++f) {
        sgi_fieldlaw *L = &laws[f];
        if ((size_t)(omax - o) < 32 + L->tablen) goto fail;
        *o++ = L->law;
        switch (L->law) {
            case SGI_LAW_CONST:
                sgi_put_v(&o, L->tablen);
                memcpy(o, L->tab, L->tablen); o += L->tablen;
                break;
            case SGI_LAW_CYCLE:
                sgi_put_v(&o, L->period);
                sgi_put_v(&o, L->tablen);
                memcpy(o, L->tab, L->tablen); o += L->tablen;
                break;
            case SGI_LAW_COUNTER:
                sgi_put_s(&o, L->base);
                sgi_put_s(&o, L->step);
                *o++ = L->pad;
                *o++ = L->fixlen;
                break;
            case SGI_LAW_FLOAT_LIN:
                sgi_put_s(&o, L->base);
                sgi_put_s(&o, L->step);
                *o++ = L->decimals;
                break;
            case SGI_LAW_ENUM:
                sgi_put_v(&o, L->nval);
                sgi_put_v(&o, L->tablen);
                memcpy(o, L->tab, L->tablen); o += L->tablen;
                break;
            default:
                break;
        }
    }

    /* ---- ENUM index streams, one byte per record per enum field ---- */
    for (f = 0; f < S.nfield; ++f) {
        sgi_fieldlaw *L = &laws[f];
        size_t r;
        if (L->law != SGI_LAW_ENUM) continue;
        if ((size_t)(omax - o) < S.nrec + 8) goto fail;
        for (r = 0; r < S.nrec; ++r) {
            uint32_t l, i, off = 0;
            const uint8_t *v = fval(src, &S, r, f, &l);
            int idx = -1;
            for (i = 0; i < L->nval; ++i) {
                const uint8_t *e = L->tab + off;
                if (e[0] == (uint8_t)l && memcmp(e + 1, v, l) == 0) { idx = (int)i; break; }
                off += 1u + e[0];
            }
            if (idx < 0) goto fail;
            *o++ = (uint8_t)idx;
        }
    }

    /* ---- residual columns, contiguous per field ---- */
    for (f = 0; f < S.nfield; ++f) {
        size_t r;
        if (laws[f].law != SGI_LAW_RESIDUAL) continue;
        for (r = 0; r < S.nrec; ++r) {
            uint32_t l;
            const uint8_t *v = fval(src, &S, r, f, &l);
            if ((size_t)(omax - o) < l + 8) goto fail;
            sgi_put_v(&o, l);
            memcpy(o, v, l); o += l;
        }
    }

    /* ---- tail: bytes after the last complete record ---- */
    {
        size_t tail = n - S.covered;
        if ((size_t)(omax - o) < tail + 8) goto fail;
        sgi_put_v(&o, tail);
        memcpy(o, src + S.covered, tail); o += tail;
    }

    for (f = 0; f < S.nfield; ++f) law_free(&laws[f]);
    free(laws); sgi_split_free(&S);
    return (size_t)(o - dst);

fail:
    for (f = 0; f < S.nfield; ++f) law_free(&laws[f]);
    free(laws); sgi_split_free(&S);
    return 0;
}

/* ===========================================================================
 * 5. DECODER
 *
 * Re-runs the laws.  For a fully lawful block this touches no probability
 * model and no bit coder at all -- it walks a small table and writes bytes,
 * which is why throughput here is bounded by memory bandwidth rather than
 * by anything algorithmic.
 * ========================================================================= */

typedef struct {
    uint8_t        law;
    uint8_t        decimals;
    uint8_t        pad;
    uint8_t        fixlen;
    uint32_t       period;
    int64_t        base;
    int64_t        step;
    const uint8_t *tab;
    uint32_t       tablen;
    uint32_t       nval;
    const uint8_t *idx;      /* ENUM index stream                  */
    const uint8_t *res;      /* RESIDUAL column cursor             */
    const uint8_t *res_end;
    /* Flattened table: entry i is at ptr[i] with length len[i].
     *
     * The table is stored length-prefixed, so reaching entry i by walking
     * costs O(i).  With a 4096-entry cycle replayed a million times that
     * walk dominates everything else the decoder does.  Building a direct
     * index once, at open time, turns every later lookup into two loads. */
    const uint8_t **ptr;
    uint32_t       *len;
    uint32_t        cursor;  /* CYCLE position, avoids the modulo */
} sgi_dlaw;

/* Build the direct index for a length-prefixed table. */
static int tab_index(sgi_dlaw *d)
{
    uint32_t off = 0, i;
    if (d->nval == 0) return -1;
    d->ptr = (const uint8_t **)malloc(d->nval * sizeof(*d->ptr));
    d->len = (uint32_t *)malloc(d->nval * sizeof(*d->len));
    if (!d->ptr || !d->len) return -1;
    for (i = 0; i < d->nval; ++i) {
        if (off >= d->tablen) return -1;
        d->len[i] = d->tab[off];
        d->ptr[i] = d->tab + off + 1;
        off += 1u + d->tab[off];
        if (off > d->tablen) return -1;
    }
    return 0;
}

static void sgi_dlaw_free(sgi_dlaw *L, int nfield)
{
    int i;
    if (!L) return;
    for (i = 0; i < nfield; ++i) { free(L[i].ptr); free(L[i].len); }
    free(L);
}

int hz_sgi_decompress(uint8_t *dst, size_t n, const uint8_t *src, size_t src_size)
{
    const uint8_t *p = src, *e = src + src_size;
    sgi_dlaw *L = NULL;
    size_t nrec, reclen, covered, declared, r;
    int kind, nfield, f, err = 0;
    size_t fast_done = 0;
    uint8_t numbuf[48];
    uint8_t rsep, fsep;
    uint8_t *o = dst, *oend = dst + n;

    if (src_size < 8 || src[0] != SGI_MAGIC) return -1;
    if (src[1] == SGI_KIND_BIN) return sgi_bin_decompress(dst, n, src, src_size);
    p += 2;
    kind = src[1]; (void)kind;
    rsep = *p++;
    fsep = *p++;
    reclen   = (size_t)sgi_get_v(&p, e, &err);
    nrec     = (size_t)sgi_get_v(&p, e, &err);
    nfield   = (int)sgi_get_v(&p, e, &err);
    declared = (size_t)sgi_get_v(&p, e, &err);
    covered  = (size_t)sgi_get_v(&p, e, &err);
    if (err || nfield <= 0 || nfield > SGI_MAX_FIELDS) return -1;
    if (declared != n || covered > n) return -1;

    L = (sgi_dlaw *)calloc((size_t)nfield, sizeof(sgi_dlaw));
    if (!L) return -1;

    for (f = 0; f < nfield; ++f) {
        if (p >= e) { err = 1; break; }
        L[f].law = *p++;
        switch (L[f].law) {
            case SGI_LAW_CONST:
                L[f].tablen = (uint32_t)sgi_get_v(&p, e, &err);
                if (err || (size_t)(e - p) < L[f].tablen) { err = 1; break; }
                L[f].tab = p; p += L[f].tablen;
                break;
            case SGI_LAW_CYCLE:
                L[f].period = (uint32_t)sgi_get_v(&p, e, &err);
                L[f].tablen = (uint32_t)sgi_get_v(&p, e, &err);
                if (err || L[f].period == 0 || (size_t)(e - p) < L[f].tablen) { err = 1; break; }
                L[f].tab = p; p += L[f].tablen;
                break;
            case SGI_LAW_COUNTER:
                L[f].base = sgi_get_s(&p, e, &err);
                L[f].step = sgi_get_s(&p, e, &err);
                if (p + 2 > e) { err = 1; break; }
                L[f].pad = *p++;
                L[f].fixlen = *p++;
                break;
            case SGI_LAW_FLOAT_LIN:
                L[f].base = sgi_get_s(&p, e, &err);
                L[f].step = sgi_get_s(&p, e, &err);
                if (p >= e) { err = 1; break; }
                L[f].decimals = *p++;
                if (L[f].decimals < 1 || L[f].decimals > 9) err = 1;
                break;
            case SGI_LAW_ENUM:
                L[f].nval   = (uint32_t)sgi_get_v(&p, e, &err);
                L[f].tablen = (uint32_t)sgi_get_v(&p, e, &err);
                if (err || L[f].nval == 0 || (size_t)(e - p) < L[f].tablen) { err = 1; break; }
                L[f].tab = p; p += L[f].tablen;
                break;
            case SGI_LAW_RESIDUAL:
                break;
            default:
                err = 1;
        }
        if (err) break;
    }
    if (err) { sgi_dlaw_free(L, nfield); return -1; }

    /* Build the direct table indexes now, so the replay loop never walks. */
    for (f = 0; f < nfield; ++f) {
        if (L[f].law == SGI_LAW_CYCLE) {
            L[f].nval = L[f].period;
            if (tab_index(&L[f]) != 0) { sgi_dlaw_free(L, nfield); return -1; }
        } else if (L[f].law == SGI_LAW_ENUM) {
            if (tab_index(&L[f]) != 0) { sgi_dlaw_free(L, nfield); return -1; }
        }
    }

    /* enum index streams, in field order */
    for (f = 0; f < nfield; ++f) {
        if (L[f].law != SGI_LAW_ENUM) continue;
        if ((size_t)(e - p) < nrec) { sgi_dlaw_free(L, nfield); return -1; }
        L[f].idx = p;
        p += nrec;
    }

    /* residual columns: each is a run of (varint length, bytes) */
    for (f = 0; f < nfield; ++f) {
        if (L[f].law != SGI_LAW_RESIDUAL) continue;
        L[f].res = p;
        for (r = 0; r < nrec; ++r) {
            uint64_t l = sgi_get_v(&p, e, &err);
            if (err || (size_t)(e - p) < l) { sgi_dlaw_free(L, nfield); return -1; }
            p += l;
        }
        L[f].res_end = p;
    }

    /* ---- replay ----
     *
     * Two loops, not one.  The common case -- every field lawful, so every
     * value is a pointer and a length already in cache -- gets a tight loop
     * with the bounds check hoisted to the record level: one record cannot
     * exceed a known maximum, so checking once per record instead of once
     * per field removes a branch from the innermost path.
     *
     * Anything with a residual column, a rendered number or a variable
     * record length falls to the general loop below, which checks
     * everything.  Correctness is identical; only the speed differs. */
    {
        int all_table = 1;
        size_t maxrec = 0;

        for (f = 0; f < nfield; ++f) {
            sgi_dlaw *d = &L[f];
            if (d->law == SGI_LAW_CONST) {
                maxrec += d->tablen;
            } else if (d->law == SGI_LAW_CYCLE || d->law == SGI_LAW_ENUM) {
                uint32_t i, m = 0;
                for (i = 0; i < d->nval; ++i) if (d->len[i] > m) m = d->len[i];
                maxrec += m;
            } else {
                all_table = 0;
                break;
            }
        }
        maxrec += (size_t)nfield + 2 + 16;   /* +16: wide-store overhang */

        if (all_table && !reclen) {
            /* The reservation is a worst case, so the final records --
             * where the remaining room is exact rather than generous --
             * are handed to the checked loop instead of being rejected. */
            for (r = 0; r < nrec && (size_t)(oend - o) >= maxrec; ++r) {
                for (f = 0; f < nfield; ++f) {
                    sgi_dlaw *d = &L[f];
                    const uint8_t *v;
                    uint32_t vlen;
                    if (d->law == SGI_LAW_CONST) {
                        v = d->tab; vlen = d->tablen;
                    } else if (d->law == SGI_LAW_CYCLE) {
                        uint32_t c = d->cursor;
                        v = d->ptr[c]; vlen = d->len[c];
                        d->cursor = (c + 1 == d->period) ? 0 : c + 1;
                    } else {
                        uint32_t i = d->idx[r];
                        if (i >= d->nval) { sgi_dlaw_free(L, nfield); return -1; }
                        v = d->ptr[i]; vlen = d->len[i];
                    }
                    /* Short field copies dominate here; a 16 byte store is
                     * one instruction and the slack was reserved above. */
                    if (vlen <= 16) memcpy(o, v, 16);
                    else            memcpy(o, v, vlen);
                    o += vlen;
                    if (fsep && f + 1 < nfield) *o++ = fsep;
                }
                if (rsep) *o++ = rsep;
            }
            /* r now indexes the first record the fast loop declined; the
             * checked loop below finishes from exactly there. */
            fast_done = r;
        }
    }

    for (r = fast_done; r < nrec; ++r) {
        for (f = 0; f < nfield; ++f) {
            sgi_dlaw *d = &L[f];
            uint32_t vlen = 0;
            const uint8_t *v = NULL;

            switch (d->law) {
                case SGI_LAW_CONST:
                    v = d->tab; vlen = d->tablen;
                    break;
                case SGI_LAW_CYCLE: {
                    uint32_t c = d->cursor;
                    v = d->ptr[c]; vlen = d->len[c];
                    d->cursor = (c + 1 == d->period) ? 0 : c + 1;
                    break;
                }
                case SGI_LAW_ENUM: {
                    uint32_t i = d->idx[r];
                    if (i >= d->nval) { sgi_dlaw_free(L, nfield); return -1; }
                    v = d->ptr[i]; vlen = d->len[i];
                    break;
                }
                /* Render into scratch, then copy exactly what was produced.
                 *
                 * Rendering straight into the output needs a worst case
                 * reservation, and on the very last record the remaining
                 * room is exact rather than generous -- so a block that
                 * decoded perfectly for a quarter of a million records
                 * failed on the final one.  Going through scratch costs a
                 * short memcpy and removes the whole class of problem. */
                case SGI_LAW_COUNTER: {
                    size_t w = render_int(numbuf,
                                          d->base + d->step * (int64_t)r,
                                          d->fixlen, d->pad);
                    if ((size_t)(oend - o) < w) { sgi_dlaw_free(L, nfield); return -1; }
                    memcpy(o, numbuf, w);
                    o += w;
                    goto after_field;
                }
                case SGI_LAW_FLOAT_LIN: {
                    size_t w = render_fixed(numbuf,
                                            d->base + d->step * (int64_t)r,
                                            d->decimals, 0, 0);
                    if ((size_t)(oend - o) < w) { sgi_dlaw_free(L, nfield); return -1; }
                    memcpy(o, numbuf, w);
                    o += w;
                    goto after_field;
                }
                case SGI_LAW_RESIDUAL: {
                    uint64_t l = sgi_get_v(&d->res, d->res_end, &err);
                    if (err || (size_t)(d->res_end - d->res) < l) { sgi_dlaw_free(L, nfield); return -1; }
                    v = d->res; vlen = (uint32_t)l;
                    d->res += l;
                    break;
                }
                default:
                    sgi_dlaw_free(L, nfield); return -1;
            }

            if ((size_t)(oend - o) < vlen) { sgi_dlaw_free(L, nfield); return -1; }
            memcpy(o, v, vlen);
            o += vlen;

        after_field:
            if (fsep && f + 1 < nfield) {
                if (o >= oend) { sgi_dlaw_free(L, nfield); return -1; }
                *o++ = fsep;
            }
        }
        if (rsep) {
            if (o >= oend) { sgi_dlaw_free(L, nfield); return -1; }
            *o++ = rsep;
        }
        if (reclen) {
            if ((size_t)(o - dst) != (r + 1) * reclen) { sgi_dlaw_free(L, nfield); return -1; }
        }
    }

    /* ---- tail ---- */
    {
        uint64_t tail = sgi_get_v(&p, e, &err);
        if (err || (size_t)(e - p) < tail) { sgi_dlaw_free(L, nfield); return -1; }
        if ((size_t)(oend - o) < tail) { sgi_dlaw_free(L, nfield); return -1; }
        memcpy(o, p, tail);
        o += tail;
    }

    sgi_dlaw_free(L, nfield);
    return (size_t)(o - dst) == n ? 0 : -1;
}

int hz_sgi_probe(const uint8_t *src, size_t n)
{
    sgi_layout LO;
    sgi_split S;
    int f, ok = 0, score;
    size_t lawful = 0, total = 0;
    sgi_fieldlaw L;

    if (n < 4096) return 0;
    induce_layout(src, n, &LO);
    if (LO.kind == 1)      ok = split_delimited(src, n, LO.delim, &S);
    else if (LO.kind == 2) ok = split_fixed(src, n, LO.width, &S);
    if (!ok) return 0;

    for (f = 0; f < S.nfield; ++f) {
        size_t r, bytes = 0;
        infer_field(src, &S, f, &L);
        for (r = 0; r < S.nrec; ++r) { uint32_t l; fval(src, &S, r, f, &l); bytes += l; }
        total += bytes;
        if (L.law != SGI_LAW_RESIDUAL) lawful += bytes;
        law_free(&L);
    }
    sgi_split_free(&S);
    score = total ? (int)((lawful * 100) / total) : 0;
    return score;
}
