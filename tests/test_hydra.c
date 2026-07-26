/* ===========================================================================
 * HYDRA test suite.
 *
 * Correctness first: every case below round trips through the public API and
 * compares byte for byte.  A compressor that is fast and small but wrong is
 * worthless, so the bar here is that nothing ever comes back different -- on
 * structured data, on random data, on adversarial data, on every boundary
 * size, and under a deliberately corrupted stream.
 * ========================================================================= */
#include "hydra.h"
#include "hz_int.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
static char g_case[256];

static void fail(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "  FAIL [%s]: ", g_case);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    ++g_fail;
}

/* ---- deterministic pseudo random ---------------------------------------- */
static uint64_t rstate = 0x243F6A8885A308D3ull;
static void rseed(uint64_t s) { rstate = s ? s : 1; }
static uint32_t rnd(void)
{
    rstate ^= rstate << 13; rstate ^= rstate >> 7; rstate ^= rstate << 17;
    return (uint32_t)(rstate >> 32);
}
static uint32_t rnd_below(uint32_t n) { return n ? rnd() % n : 0; }

/* ---- the core check ------------------------------------------------------ */
static int check_roundtrip(const uint8_t *data, size_t n, int level, const char *what)
{
    hydra_opts o;
    uint8_t *cbuf = NULL, *dbuf = NULL;
    size_t cap;
    int64_t cs, ds;
    int ok = 1;

    snprintf(g_case, sizeof(g_case), "%s n=%lu L%d", what, (unsigned long)n, level);

    hydra_opts_init(&o, level);
    cap = hydra_bound(n);
    cbuf = (uint8_t *)malloc(cap);
    dbuf = (uint8_t *)malloc(n ? n : 1);
    if (!cbuf || !dbuf) { fail("out of memory"); free(cbuf); free(dbuf); return 0; }

    cs = hydra_compress(cbuf, cap, data, n, &o);
    if (cs < 0) { fail("compress: %s", hydra_strerror((int)cs)); ok = 0; goto out; }
    if ((size_t)cs > cap) { fail("compressor overran bound (%ld > %lu)",
                                 (long)cs, (unsigned long)cap); ok = 0; goto out; }

    /* the frame must advertise the right content size */
    if (hydra_frame_content_size(cbuf, (size_t)cs) != (int64_t)n) {
        fail("frame content size wrong"); ok = 0; goto out;
    }

    ds = hydra_decompress(dbuf, n, cbuf, (size_t)cs);
    if (ds < 0) { fail("decompress: %s", hydra_strerror((int)ds)); ok = 0; goto out; }
    if ((size_t)ds != n) { fail("size %ld != %lu", (long)ds, (unsigned long)n); ok = 0; goto out; }
    if (n && memcmp(data, dbuf, n) != 0) {
        size_t i = 0;
        while (i < n && data[i] == dbuf[i]) ++i;
        fail("byte %lu differs: %02x vs %02x",
             (unsigned long)i, data[i], dbuf[i]);
        ok = 0; goto out;
    }

out:
    if (ok) ++g_pass;
    free(cbuf); free(dbuf);
    return ok;
}

static void check_all_levels(const uint8_t *d, size_t n, const char *what)
{
    int l;
    for (l = HYDRA_LEVEL_MIN; l <= HYDRA_LEVEL_MAX; ++l)
        check_roundtrip(d, n, l, what);
}

/* ---- generators ---------------------------------------------------------- */
static void gen_zeros(uint8_t *b, size_t n)  { memset(b, 0, n); }
static void gen_ones(uint8_t *b, size_t n)   { memset(b, 0xFF, n); }
static void gen_random(uint8_t *b, size_t n) { size_t i; for (i = 0; i < n; ++i) b[i] = (uint8_t)rnd(); }
static void gen_counter(uint8_t *b, size_t n){ size_t i; for (i = 0; i < n; ++i) b[i] = (uint8_t)i; }

static void gen_text(uint8_t *b, size_t n)
{
    static const char *w[] = {
        "the","of","and","to","in","a","is","that","for","it","as","was","with",
        "be","by","on","not","he","this","are","or","his","from","at","which",
        "compression","entropy","model","context","probability","arithmetic"
    };
    size_t p = 0;
    while (p < n) {
        const char *s = w[rnd_below(31)];
        size_t l = strlen(s);
        if (p + l + 1 > n) break;
        memcpy(b + p, s, l); p += l;
        b[p++] = (rnd_below(12) == 0) ? '.' : ' ';
    }
    while (p < n) b[p++] = ' ';
}

static void gen_records(uint8_t *b, size_t n)
{
    size_t p = 0;
    uint32_t id = 1000;
    while (p + 32 <= n) {
        char line[64];
        int l = snprintf(line, sizeof(line), "%08u,user%03u,%s,%04u\n",
                         id++, (unsigned)rnd_below(200),
                         (rnd_below(2) ? "active" : "closed"),
                         (unsigned)rnd_below(9999));
        if (p + (size_t)l > n) break;
        memcpy(b + p, line, (size_t)l);
        p += (size_t)l;
    }
    while (p < n) b[p++] = '\n';
}

/* a smooth 16 bit waveform: the case the delta filter exists for */
static void gen_audio(uint8_t *b, size_t n)
{
    size_t i;
    int32_t v = 0, dv = 0;
    for (i = 0; i + 1 < n; i += 2) {
        dv += (int32_t)rnd_below(64) - 32;
        if (dv > 400) dv = 400;
        if (dv < -400) dv = -400;
        v += dv;
        if (v > 30000) { v = 30000; dv = -dv; }
        if (v < -30000) { v = -30000; dv = -dv; }
        b[i] = (uint8_t)v; b[i + 1] = (uint8_t)(v >> 8);
    }
    while (i < n) b[i++] = 0;
}

/* synthetic machine code: lots of E8 relative calls to a few targets */
static void gen_code(uint8_t *b, size_t n)
{
    size_t i = 0;
    while (i + 8 < n) {
        uint32_t r = rnd_below(10);
        if (r < 3) {
            int32_t target = (int32_t)rnd_below(4096) * 16;
            int32_t rel = target - (int32_t)(i + 5);
            b[i++] = 0xE8;
            b[i++] = (uint8_t)rel; b[i++] = (uint8_t)(rel >> 8);
            b[i++] = (uint8_t)(rel >> 16); b[i++] = (uint8_t)(rel >> 24);
        } else if (r < 5) {
            b[i++] = 0x48; b[i++] = 0x89; b[i++] = (uint8_t)(0xC0 + rnd_below(64));
        } else if (r < 7) {
            b[i++] = 0x8B; b[i++] = (uint8_t)(0x40 + rnd_below(8)); b[i++] = (uint8_t)rnd_below(64);
        } else {
            b[i++] = (uint8_t)rnd();
        }
    }
    while (i < n) b[i++] = 0x90;
}

/* float64 time series: exercises the byte transpose + delta pair */
static void gen_f64(uint8_t *b, size_t n)
{
    size_t i;
    double v = 1000.0;
    for (i = 0; i + 8 <= n; i += 8) {
        union { double d; uint8_t b[8]; } u;
        v += ((double)(int)(rnd() % 2001) - 1000.0) / 1000.0;
        u.d = v;
        memcpy(b + i, u.b, 8);
    }
    while (i < n) b[i++] = 0;
}

/* fixed width binary records: transposable, but not numeric */
static void gen_struct_array(uint8_t *b, size_t n)
{
    size_t i;
    uint32_t id = 0;
    for (i = 0; i + 16 <= n; i += 16) {
        uint32_t a = id++, c = 0x40000000u + (rnd() & 0xFFFF);
        uint16_t d = (uint16_t)(rnd() % 8);
        memcpy(b + i, &a, 4);
        memcpy(b + i + 4, &c, 4);
        memcpy(b + i + 8, &d, 2);
        b[i + 10] = (uint8_t)(rnd() % 4);
        b[i + 11] = 0;
        memset(b + i + 12, 0, 4);
    }
    while (i < n) b[i++] = 0;
}

/* machine generated telemetry: the case SGI exists for */
static void gen_telemetry(uint8_t *b, size_t n)
{
    size_t p = 0;
    unsigned i = 0;
    while (p + 64 < n) {
        char line[96];
        int l = snprintf(line, sizeof(line),
                         "sensor_%03u,2026-07-26T12:00:%02u,%u.%03u,OK\n",
                         i % 50, i % 60, 20 + (i % 7), (i % 100) * 10);
        if (p + (size_t)l > n) break;
        memcpy(b + p, line, (size_t)l);
        p += (size_t)l;
        ++i;
    }
    while (p < n) b[p++] = '\n';
}

/* structured rows where only some columns follow a law -- exercises the
 * mixed lawful/residual path rather than the all-lawful fast one */
static void gen_semi_lawful(uint8_t *b, size_t n)
{
    size_t p = 0;
    unsigned i = 0;
    while (p + 80 < n) {
        char line[128];
        int l = snprintf(line, sizeof(line),
                         "%08u,%s,%u,%08x,CONSTANT\n",
                         i, (i & 1) ? "alpha" : "beta",
                         i % 13, (unsigned)rnd());
        if (p + (size_t)l > n) break;
        memcpy(b + p, line, (size_t)l);
        p += (size_t)l;
        ++i;
    }
    while (p < n) b[p++] = '\n';
}

/* Rows that look structured but are not: same shape, no law anywhere.
 * SGI must decline rather than produce a bloated header. */
static void gen_pseudo_structured(uint8_t *b, size_t n)
{
    size_t p = 0;
    while (p + 80 < n) {
        char line[128];
        int l = snprintf(line, sizeof(line), "%u,%u,%u,%u,%u\n",
                         (unsigned)rnd(), (unsigned)rnd(), (unsigned)rnd(),
                         (unsigned)rnd(), (unsigned)rnd());
        if (p + (size_t)l > n) break;
        memcpy(b + p, line, (size_t)l);
        p += (size_t)l;
    }
    while (p < n) b[p++] = '\n';
}

/* Ragged rows: field counts vary, so the split must be rejected. */
static void gen_ragged(uint8_t *b, size_t n)
{
    size_t p = 0;
    while (p + 40 < n) {
        char line[128];
        int k = 1 + (int)rnd_below(6), j, l = 0;
        for (j = 0; j < k && l < 100; ++j)
            l += snprintf(line + l, sizeof(line) - (size_t)l, "%s%u",
                          j ? "," : "", (unsigned)rnd_below(1000));
        l += snprintf(line + l, sizeof(line) - (size_t)l, "\n");
        if (p + (size_t)l > n) break;
        memcpy(b + p, line, (size_t)l);
        p += (size_t)l;
    }
    while (p < n) b[p++] = '\n';
}

/* long range duplication: a payload that repeats far apart */
static void gen_lrm(uint8_t *b, size_t n)
{
    size_t unit = 4096, i;
    if (n < unit * 4) { gen_text(b, n); return; }
    gen_text(b, unit);
    for (i = unit; i + unit <= n; i += unit) {
        if (rnd_below(3)) memcpy(b + i, b, unit);
        else gen_text(b + i, unit);
    }
    while (i < n) b[i++] = ' ';
}

/* alternating high/low entropy: forces per-block decisions to change */
static void gen_mixed(uint8_t *b, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t chunk = 8192 + rnd_below(24576);
        if (chunk > n - i) chunk = n - i;
        if (rnd_below(2)) gen_random(b + i, chunk);
        else gen_text(b + i, chunk);
        i += chunk;
    }
}

/* ========================================================================= */
static void test_generators(void)
{
    struct { const char *name; void (*fn)(uint8_t *, size_t); } gens[] = {
        { "zeros",   gen_zeros   },
        { "ones",    gen_ones    },
        { "random",  gen_random  },
        { "counter", gen_counter },
        { "text",    gen_text    },
        { "records", gen_records },
        { "audio",   gen_audio   },
        { "code",    gen_code    },
        { "lrm",     gen_lrm     },
        { "f64",     gen_f64     },
        { "structs", gen_struct_array },
        { "telemetry", gen_telemetry },
        { "semilaw", gen_semi_lawful },
        { "pseudostruct", gen_pseudo_structured },
        { "ragged",  gen_ragged  },
        { "mixed",   gen_mixed   }
    };
    size_t sizes[] = { 1024, 64 * 1024, 700 * 1024 };
    size_t gi, si;
    uint8_t *buf = (uint8_t *)malloc(1024 * 1024);
    if (!buf) { fail("alloc"); return; }

    printf("-- generated content, all levels\n");
    for (gi = 0; gi < sizeof(gens) / sizeof(gens[0]); ++gi) {
        for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); ++si) {
            rseed(0x1234 + gi * 977 + si);
            gens[gi].fn(buf, sizes[si]);
            check_all_levels(buf, sizes[si], gens[gi].name);
        }
    }
    free(buf);
}

static void test_sizes(void)
{
    uint8_t *buf = (uint8_t *)malloc(70000);
    size_t n;
    int l;
    if (!buf) { fail("alloc"); return; }

    printf("-- every small size, and the block boundaries\n");
    /* every size up to 300: catches off-by-one in the tail handling */
    for (n = 0; n <= 300; ++n) {
        size_t i;
        for (i = 0; i < n; ++i) buf[i] = (uint8_t)(i * 7 + (i >> 3));
        for (l = 1; l <= 9; l += 4) check_roundtrip(buf, n, l, "tiny");
    }
    /* sizes that are deliberately not multiples of any record width, so the
     * transpose tail path is exercised in both directions */
    {
        size_t odd[] = { 4097, 4098, 4099, 4101, 4103, 4107, 65521, 65535 };
        size_t k;
        for (k = 0; k < sizeof(odd) / sizeof(odd[0]); ++k) {
            size_t j;
            uint8_t *ob = (uint8_t *)malloc(odd[k]);
            if (!ob) continue;
            rseed(555 + k);
            gen_f64(ob, odd[k]);
            for (j = 0; j < 3; ++j)
                check_roundtrip(ob, odd[k], (int)(1 + j * 4), "f64 odd size");
            free(ob);
        }
    }

    /* around the 64 KiB minimum block size */
    {
        size_t edges[] = { 65534, 65535, 65536, 65537, 65538 };
        size_t k;
        for (k = 0; k < 5; ++k) {
            rseed(99 + k);
            gen_text(buf, edges[k]);
            check_roundtrip(buf, edges[k], 1, "block edge");
            check_roundtrip(buf, edges[k], 5, "block edge");
        }
    }
    free(buf);
}

/* Multi-block frames where a length-changing filter fires.
 *
 * Regression for a real bug: the block header stored only the *filtered*
 * length, and the decoder used it to place the block in the output.  For
 * the long range filter, which shrinks its input, every block after the
 * first then landed at the wrong offset.  It stayed hidden because a
 * single-block frame is the common case and there the two lengths coincide.
 * Caught on Wikipedia-format XML at exactly 2^20 + 1 bytes -- one byte past
 * the FAST block size. */
static void test_multiblock_filters(void)
{
    /* sizes that straddle the 1 MiB FAST block boundary */
    size_t sizes[] = { (1u << 20) - 1, (1u << 20), (1u << 20) + 1,
                       (1u << 20) + 4096, 3u << 20 };
    size_t k;
    uint8_t *buf = (uint8_t *)malloc(4u << 20);

    printf("-- multi-block frames with length-changing filters\n");
    if (!buf) { fail("alloc"); return; }

    for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
        size_t n = sizes[k], p = 0;
        int l;
        /* Highly duplicated structured text: guarantees the long range
         * filter engages, which is the condition that exposed the bug. */
        rseed(4242 + k);
        while (p < n) {
            static const char *unit =
                "  <page>\n    <title>Example</title>\n    <id>12345</id>\n"
                "    <revision><timestamp>2026-07-26T12:00:00Z</timestamp>\n"
                "    <text xml:space=\"preserve\">Lorem ipsum dolor sit amet, "
                "consectetur adipiscing elit, sed do eiusmod tempor.</text>\n"
                "    </revision>\n  </page>\n";
            size_t ul = strlen(unit);
            if (p + ul > n) break;
            memcpy(buf + p, unit, ul);
            p += ul;
        }
        while (p < n) buf[p++] = '\n';

        for (l = 1; l <= 9; l += 2)
            check_roundtrip(buf, n, l, "multiblock lrm");
    }
    free(buf);
}

static void test_adversarial(void)
{
    uint8_t *buf = (uint8_t *)malloc(300000);
    size_t n = 300000, i;
    if (!buf) { fail("alloc"); return; }

    printf("-- adversarial patterns\n");

    /* every byte value in a repeating cycle of a length that is coprime with
     * everything the model uses: defeats naive periodic prediction */
    for (i = 0; i < n; ++i) buf[i] = (uint8_t)(i % 251);
    check_all_levels(buf, n, "coprime cycle");

    /* run lengths that straddle the token boundaries of the fast coder */
    {
        size_t p = 0;
        uint8_t v = 0;
        while (p < n) {
            size_t run = 1 + (p % 300);
            if (run > n - p) run = n - p;
            memset(buf + p, v++, run);
            p += run;
        }
        check_all_levels(buf, n, "graded runs");
    }

    /* data that looks like a hydra frame: must not confuse the container */
    {
        for (i = 0; i + 4 <= n; i += 4) {
            buf[i] = 'H'; buf[i+1] = 'Y'; buf[i+2] = 'D'; buf[i+3] = 'R';
        }
        check_all_levels(buf, n, "magic soup");
    }

    /* incompressible with a compressible prefix: exercises the RAW fallback */
    {
        rseed(7);
        memset(buf, 'x', 1000);
        gen_random(buf + 1000, n - 1000);
        check_all_levels(buf, n, "prefix then noise");
    }

    /* single byte repeated with one flipped bit far in: the match model must
     * recover rather than mispredict forever */
    {
        memset(buf, 0x5A, n);
        buf[n / 2] = 0x5B;
        buf[n - 1] = 0x00;
        check_all_levels(buf, n, "one defect");
    }

    /* alternating two bytes: maximum work for a bitwise model, minimum entropy */
    for (i = 0; i < n; ++i) buf[i] = (i & 1) ? 0xAA : 0x55;
    check_all_levels(buf, n, "alternating");

    free(buf);
}

static void test_random_fuzz(void)
{
    size_t iter;
    uint8_t *buf = (uint8_t *)malloc(200000);
    if (!buf) { fail("alloc"); return; }

    printf("-- randomized fuzz over content and size\n");
    for (iter = 0; iter < 400; ++iter) {
        size_t n = rnd_below(150000) + 1;
        size_t i = 0;
        int level = 1 + (int)rnd_below(9);
        rseed(iter * 7919 + 13);

        /* build a random mixture so no single model dominates */
        while (i < n) {
            size_t chunk = 1 + rnd_below(4096);
            uint32_t kind = rnd_below(6);
            if (chunk > n - i) chunk = n - i;
            switch (kind) {
                case 0: memset(buf + i, (uint8_t)rnd(), chunk); break;
                case 1: gen_random(buf + i, chunk); break;
                case 2: gen_text(buf + i, chunk); break;
                case 3: {
                    size_t k;
                    for (k = 0; k < chunk; ++k) buf[i + k] = (uint8_t)(k & 0x0F);
                    break;
                }
                case 4:
                    if (i >= chunk) memcpy(buf + i, buf + i - chunk, chunk);
                    else memset(buf + i, 0, chunk);
                    break;
                default: gen_audio(buf + i, chunk); break;
            }
            i += chunk;
        }
        check_roundtrip(buf, n, level, "fuzz");
    }
    free(buf);
}

/* A corrupted stream must be rejected or produce wrong-but-safe output --
 * never a crash and never a read past the end of the buffer. */
static void test_corruption(void)
{
    uint8_t *buf = (uint8_t *)malloc(50000);
    uint8_t *cbuf, *dbuf;
    size_t n = 50000, cap;
    int64_t cs;
    size_t trial;
    int detected = 0, total = 0;

    if (!buf) { fail("alloc"); return; }
    printf("-- corrupted input is handled safely\n");
    rseed(31337);
    gen_text(buf, n);

    cap = hydra_bound(n);
    cbuf = (uint8_t *)malloc(cap);
    dbuf = (uint8_t *)malloc(n);
    if (!cbuf || !dbuf) { fail("alloc"); free(buf); free(cbuf); free(dbuf); return; }

    cs = hydra_compress(cbuf, cap, buf, n, NULL);
    if (cs < 0) { fail("setup compress"); goto out; }

    for (trial = 0; trial < 3000; ++trial) {
        uint8_t *copy = (uint8_t *)malloc((size_t)cs);
        size_t where;
        int64_t r;
        if (!copy) break;
        memcpy(copy, cbuf, (size_t)cs);

        switch (trial % 3) {
            case 0:  /* flip a bit */
                where = rnd_below((uint32_t)cs);
                copy[where] ^= (uint8_t)(1u << rnd_below(8));
                r = hydra_decompress(dbuf, n, copy, (size_t)cs);
                break;
            case 1:  /* truncate */
                where = 1 + rnd_below((uint32_t)cs - 1);
                r = hydra_decompress(dbuf, n, copy, where);
                break;
            default: /* replace a byte */
                where = rnd_below((uint32_t)cs);
                copy[where] = (uint8_t)rnd();
                r = hydra_decompress(dbuf, n, copy, (size_t)cs);
                break;
        }
        ++total;
        if (r < 0) ++detected;
        else if (r == (int64_t)n && memcmp(dbuf, buf, n) == 0) ++detected; /* benign */
        free(copy);
    }
    printf("     %d/%d corruptions rejected or benign\n", detected, total);
    if (detected != total) fail("%d corruptions silently produced wrong output",
                                total - detected);
    else ++g_pass;

out:
    free(buf); free(cbuf); free(dbuf);
}

static void test_api_edges(void)
{
    uint8_t small[64], out[256], back[64];
    int64_t r;
    printf("-- api edge cases\n");
    memset(small, 'q', sizeof(small));
    snprintf(g_case, sizeof(g_case), "api");

    /* empty input */
    r = hydra_compress(out, sizeof(out), small, 0, NULL);
    if (r < 0) fail("empty compress: %s", hydra_strerror((int)r));
    else {
        int64_t d = hydra_decompress(back, sizeof(back), out, (size_t)r);
        if (d != 0) fail("empty decompress returned %ld", (long)d);
        else ++g_pass;
    }

    /* destination far too small */
    r = hydra_compress(out, 4, small, sizeof(small), NULL);
    if (r >= 0) fail("undersized dst was not rejected"); else ++g_pass;

    /* garbage input */
    memset(out, 0xAB, sizeof(out));
    r = hydra_decompress(back, sizeof(back), out, sizeof(out));
    if (r >= 0) fail("garbage was not rejected"); else ++g_pass;

    /* truncated header */
    r = hydra_decompress(back, sizeof(back), out, 3);
    if (r >= 0) fail("short input was not rejected"); else ++g_pass;

    /* bad level */
    {
        hydra_opts o;
        hydra_opts_init(&o, 5);
        o.level = 99;
        r = hydra_compress(out, sizeof(out), small, sizeof(small), &o);
        if (r >= 0) fail("bad level accepted"); else ++g_pass;
    }
}

int main(void)
{
    printf("hydra test suite (%s)\n\n", hydra_version_string());

    test_api_edges();
    test_sizes();
    test_generators();
    test_multiblock_filters();
    test_adversarial();
    test_random_fuzz();
    test_corruption();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
