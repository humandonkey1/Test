/* ===========================================================================
 * HYDRA: command line front end.
 *
 *   hydra c [-N] [opts] in out     compress
 *   hydra d in out                 decompress
 *   hydra t file                   round trip test, nothing written
 *   hydra b file                   benchmark every level
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
    uint8_t *buf = NULL;
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

static int write_all(const char *path, const uint8_t *buf, size_t n)
{
    FILE *f = fopen(path, "wb");
    size_t w;
    if (!f) return -1;
    w = fwrite(buf, 1, n, f);
    fclose(f);
    return w == n ? 0 : -1;
}

static void human(double bytes, char *out, size_t cap)
{
    const char *u[] = { "B", "KiB", "MiB", "GiB" };
    int i = 0;
    while (bytes >= 1024.0 && i < 3) { bytes /= 1024.0; ++i; }
    snprintf(out, cap, "%.2f %s", bytes, u[i]);
}

static void usage(void)
{
    fprintf(stderr,
        "%s\n"
        "usage:\n"
        "  hydra c [-1..-9] [-v] [--blind] [--no-filters] IN OUT   compress\n"
        "  hydra d IN OUT                                decompress\n"
        "  hydra t [-1..-9] FILE                         verify round trip\n"
        "  hydra b FILE                                  benchmark all levels\n",
        hydra_version_string());
}

static int cmd_compress(int argc, char **argv)
{
    hydra_opts o;
    uint8_t *in = NULL, *out = NULL;
    size_t n = 0, cap;
    int64_t r;
    const char *fin = NULL, *fout = NULL;
    int i;
    double t0, t1;
    char h1[32], h2[32];

    hydra_opts_init(&o, HYDRA_LEVEL_DEFAULT);

    for (i = 0; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] >= '1' && argv[i][1] <= '9' && !argv[i][2])
            hydra_opts_init(&o, argv[i][1] - '0');
        else if (!strcmp(argv[i], "-v")) o.verbose = 1;
        else if (!strcmp(argv[i], "--no-filters"))
            o.enable_delta = o.enable_exe = o.enable_lrm = 0;
        else if (!strcmp(argv[i], "--no-checksum")) o.checksum = 0;
        else if (!strcmp(argv[i], "--blind")) o.blind = 1;
        else if (!fin) fin = argv[i];
        else fout = argv[i];
    }
    if (!fin || !fout) { usage(); return 2; }

    if (read_all(fin, &in, &n) < 0) { fprintf(stderr, "cannot read %s\n", fin); return 1; }
    cap = hydra_bound(n);
    out = (uint8_t *)malloc(cap);
    if (!out) { fprintf(stderr, "out of memory\n"); free(in); return 1; }

    t0 = now_sec();
    r = hydra_compress(out, cap, in, n, &o);
    t1 = now_sec();

    if (r < 0) {
        fprintf(stderr, "compress failed: %s\n", hydra_strerror((int)r));
        free(in); free(out); return 1;
    }
    if (write_all(fout, out, (size_t)r) < 0) {
        fprintf(stderr, "cannot write %s\n", fout);
        free(in); free(out); return 1;
    }

    human((double)n, h1, sizeof(h1));
    human((double)r, h2, sizeof(h2));
    printf("%s -> %s  (%.3fx)  %.2f MB/s  level %d\n",
           h1, h2, n ? (double)n / (double)r : 0.0,
           (t1 > t0) ? (double)n / 1e6 / (t1 - t0) : 0.0, o.level);

    free(in); free(out);
    return 0;
}

static int cmd_decompress(int argc, char **argv)
{
    uint8_t *in = NULL, *out = NULL;
    size_t n = 0;
    int64_t want, r;
    double t0, t1;
    char h1[32];

    if (argc < 2) { usage(); return 2; }
    if (read_all(argv[0], &in, &n) < 0) { fprintf(stderr, "cannot read %s\n", argv[0]); return 1; }

    want = hydra_frame_content_size(in, n);
    if (want < 0) {
        fprintf(stderr, "bad frame: %s\n", hydra_strerror((int)want));
        free(in); return 1;
    }
    out = (uint8_t *)malloc((size_t)want ? (size_t)want : 1);
    if (!out) { fprintf(stderr, "out of memory\n"); free(in); return 1; }

    t0 = now_sec();
    r = hydra_decompress(out, (size_t)want, in, n);
    t1 = now_sec();

    if (r < 0) {
        fprintf(stderr, "decompress failed: %s\n", hydra_strerror((int)r));
        free(in); free(out); return 1;
    }
    if (write_all(argv[1], out, (size_t)r) < 0) {
        fprintf(stderr, "cannot write %s\n", argv[1]);
        free(in); free(out); return 1;
    }
    human((double)r, h1, sizeof(h1));
    printf("%s restored  %.2f MB/s\n", h1,
           (t1 > t0) ? (double)r / 1e6 / (t1 - t0) : 0.0);
    free(in); free(out);
    return 0;
}

static int roundtrip(const uint8_t *in, size_t n, int level, int quiet)
{
    hydra_opts o;
    uint8_t *cbuf, *dbuf;
    size_t cap;
    int64_t cs, ds;
    double t0, t1, t2;
    int ok = 0;

    hydra_opts_init(&o, level);
    cap = hydra_bound(n);
    cbuf = (uint8_t *)malloc(cap);
    dbuf = (uint8_t *)malloc(n ? n : 1);
    if (!cbuf || !dbuf) { free(cbuf); free(dbuf); return -1; }

    t0 = now_sec();
    cs = hydra_compress(cbuf, cap, in, n, &o);
    t1 = now_sec();
    if (cs < 0) {
        fprintf(stderr, "L%d compress failed: %s\n", level, hydra_strerror((int)cs));
        free(cbuf); free(dbuf); return -1;
    }
    ds = hydra_decompress(dbuf, n, cbuf, (size_t)cs);
    t2 = now_sec();
    if (ds < 0) {
        fprintf(stderr, "L%d decompress failed: %s\n", level, hydra_strerror((int)ds));
        free(cbuf); free(dbuf); return -1;
    }
    if ((size_t)ds != n || (n && memcmp(in, dbuf, n) != 0)) {
        size_t i = 0;
        while (i < n && in[i] == dbuf[i]) ++i;
        fprintf(stderr, "L%d MISMATCH at offset %lu (got %ld of %lu bytes)\n",
                level, (unsigned long)i, (long)ds, (unsigned long)n);
        free(cbuf); free(dbuf); return -1;
    }
    ok = 1;
    if (!quiet) {
        printf("  L%d %10lu -> %10ld  %8.3fx   enc %7.2f MB/s   dec %7.2f MB/s\n",
               level, (unsigned long)n, (long)cs, n ? (double)n / (double)cs : 0.0,
               (t1 > t0) ? (double)n / 1e6 / (t1 - t0) : 0.0,
               (t2 > t1) ? (double)n / 1e6 / (t2 - t1) : 0.0);
    }
    free(cbuf); free(dbuf);
    return ok ? 0 : -1;
}

static int cmd_test(int argc, char **argv)
{
    uint8_t *in = NULL;
    size_t n = 0;
    int level = 0, i, rc = 0;
    const char *path = NULL;

    for (i = 0; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] >= '1' && argv[i][1] <= '9' && !argv[i][2])
            level = argv[i][1] - '0';
        else path = argv[i];
    }
    if (!path) { usage(); return 2; }
    if (read_all(path, &in, &n) < 0) { fprintf(stderr, "cannot read %s\n", path); return 1; }

    printf("%s  (%lu bytes)\n", path, (unsigned long)n);
    if (level) rc = roundtrip(in, n, level, 0);
    else for (i = 1; i <= 9; ++i) if (roundtrip(in, n, i, 0) < 0) rc = 1;

    free(in);
    if (rc == 0) printf("round trip OK\n");
    return rc ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }
    if (!strcmp(argv[1], "c")) return cmd_compress(argc - 2, argv + 2);
    if (!strcmp(argv[1], "d")) return cmd_decompress(argc - 2, argv + 2);
    if (!strcmp(argv[1], "t")) return cmd_test(argc - 2, argv + 2);
    if (!strcmp(argv[1], "b")) return cmd_test(argc - 2, argv + 2);
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) { usage(); return 0; }
    usage();
    return 2;
}
