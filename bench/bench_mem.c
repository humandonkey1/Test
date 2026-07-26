/* ===========================================================================
 * HYDRA: in-memory benchmark.
 *
 * Measures the codec itself, with no file I/O, no process startup and no
 * allocation inside the timed region -- the same methodology the fast
 * compressors publish their throughput numbers with.
 *
 * Each measurement is the best of N runs over a warmed buffer.  Best-of is
 * the right statistic here: we are trying to measure the cost of the code,
 * and every source of noise on a shared machine only ever adds time.
 *
 * Every timed compression is verified once, outside the timing loop.  A
 * throughput figure for a codec that does not round trip is meaningless.
 * ========================================================================= */
#include "hydra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

static int read_all(const char *path, uint8_t **out, size_t *outn)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    size_t cap = 1 << 20, n = 0;
    if (!f) return -1;
    buf = (uint8_t *)malloc(cap);
    if (!buf) { fclose(f); return -1; }
    for (;;) {
        size_t got;
        if (n == cap) {
            uint8_t *nb = (uint8_t *)realloc(buf, cap * 2);
            if (!nb) { free(buf); fclose(f); return -1; }
            buf = nb; cap *= 2;
        }
        got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) break;
    }
    fclose(f);
    *out = buf; *outn = n;
    return 0;
}

/* memcpy throughput on this machine: the ceiling any decoder is measured
 * against, since no decompressor can move bytes faster than a plain copy. */
static double memcpy_ceiling(uint8_t *dst, const uint8_t *src, size_t n, int runs)
{
    double best = 1e30;
    int i;
    for (i = 0; i < runs; ++i) {
        double t0 = now_sec();
        memcpy(dst, src, n);
        {
            double dt = now_sec() - t0;
            if (dt < best) best = dt;
        }
    }
    return (double)n / 1e6 / best;
}

int main(int argc, char **argv)
{
    const char *path;
    uint8_t *in = NULL, *cbuf = NULL, *dbuf = NULL;
    size_t n = 0, cap;
    int level, lo = 1, hi = 9, runs = 3, i;

    if (argc < 2) {
        fprintf(stderr, "usage: bench_mem FILE [level] [runs]\n");
        return 2;
    }
    path = argv[1];
    if (argc > 2) { lo = hi = atoi(argv[2]); }
    if (argc > 3) { runs = atoi(argv[3]); }
    if (runs < 1) runs = 1;

    if (read_all(path, &in, &n) < 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }

    cap = hydra_bound(n);
    cbuf = (uint8_t *)malloc(cap);
    dbuf = (uint8_t *)malloc(n ? n : 1);
    if (!cbuf || !dbuf) { fprintf(stderr, "oom\n"); return 1; }

    /* warm both buffers so the timed runs never touch a cold page */
    memset(cbuf, 0, cap);
    memset(dbuf, 0, n);

    printf("%s  %lu bytes\n", path, (unsigned long)n);
    printf("  %-8s %12s %8s %11s %11s\n", "level", "out", "ratio", "enc MB/s", "dec MB/s");

    for (level = lo; level <= hi; ++level) {
        hydra_opts o;
        double tbest_c = 1e30, tbest_d = 1e30;
        int64_t cs = -1, ds = -1;

        hydra_opts_init(&o, level);
        o.checksum = 0;     /* time the codec, not the digest */

        for (i = 0; i < runs; ++i) {
            double t0 = now_sec(), dt;
            cs = hydra_compress(cbuf, cap, in, n, &o);
            dt = now_sec() - t0;
            if (cs < 0) { fprintf(stderr, "compress L%d failed: %s\n",
                                  level, hydra_strerror((int)cs)); return 1; }
            if (dt < tbest_c) tbest_c = dt;
        }
        for (i = 0; i < runs; ++i) {
            double t0 = now_sec(), dt;
            ds = hydra_decompress(dbuf, n, cbuf, (size_t)cs);
            dt = now_sec() - t0;
            if (ds < 0) { fprintf(stderr, "decompress L%d failed: %s\n",
                                  level, hydra_strerror((int)ds)); return 1; }
            if (dt < tbest_d) tbest_d = dt;
        }
        if ((size_t)ds != n || memcmp(in, dbuf, n) != 0) {
            fprintf(stderr, "L%d ROUND TRIP MISMATCH\n", level);
            return 1;
        }

        printf("  L%-7d %12ld %8.3f %11.1f %11.1f\n",
               level, (long)cs, (double)n / (double)cs,
               (double)n / 1e6 / tbest_c, (double)n / 1e6 / tbest_d);
        fflush(stdout);
    }

    printf("  %-8s %12s %8s %11s %11.1f   (hardware ceiling)\n",
           "memcpy", "-", "-", "-", memcpy_ceiling(dbuf, in, n, 20));

    free(in); free(cbuf); free(dbuf);
    return 0;
}
