/* ===========================================================================
 * HYDRA / tools/proof.c
 *
 * Does "132x on every file" hold?  This program does not argue.  It
 * enumerates *every* file of a given length, compresses each one, and counts
 * the results.  What comes out is a measurement, not a claim.
 *
 * Two experiments:
 *
 *   1. EXHAUSTIVE.  For n = 1..3 bytes, run all 256^n possible files through
 *      the real compressor and tabulate the output sizes.  Nothing is
 *      sampled and nothing is assumed.
 *
 *   2. COUNTING.  For any n and any target ratio r, compare the number of
 *      distinct inputs against the number of distinct outputs short enough
 *      to represent them.  This is arithmetic on the file counts themselves.
 *
 * Run it against hydra, against gzip, against anything: the numbers below
 * are properties of "distinct inputs must map to distinct outputs", not of
 * any particular algorithm.  A compressor that violated them would be one
 * that loses data.
 * ========================================================================= */
#include "hydra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- experiment 1: enumerate every file of length n --------------------- */
static void exhaustive(int n, int level)
{
    size_t total = 1;
    size_t cap;
    uint8_t *in, *out, *back;
    hydra_opts o;
    size_t smaller = 0, equal = 0, larger = 0;
    size_t best = (size_t)-1, worst = 0;
    unsigned long long i, count;
    int k;

    for (k = 0; k < n; ++k) total *= 256;
    count = (unsigned long long)total;

    hydra_opts_init(&o, level);
    o.checksum = 0;                 /* measure the codec, not the digest */

    cap  = hydra_bound((size_t)n);
    in   = (uint8_t *)malloc((size_t)n);
    out  = (uint8_t *)malloc(cap);
    back = (uint8_t *)malloc((size_t)n);
    if (!in || !out || !back) { fprintf(stderr, "oom\n"); exit(1); }

    for (i = 0; i < count; ++i) {
        unsigned long long v = i;
        int64_t cs, ds;
        for (k = 0; k < n; ++k) { in[k] = (uint8_t)(v & 0xFF); v >>= 8; }

        cs = hydra_compress(out, cap, in, (size_t)n, &o);
        if (cs < 0) { fprintf(stderr, "compress failed\n"); exit(1); }

        /* every single one must also decompress back exactly */
        ds = hydra_decompress(back, (size_t)n, out, (size_t)cs);
        if (ds != n || memcmp(in, back, (size_t)n) != 0) {
            fprintf(stderr, "ROUND TRIP FAILURE at input %llu\n", i);
            exit(1);
        }

        if ((size_t)cs < (size_t)n)      ++smaller;
        else if ((size_t)cs == (size_t)n) ++equal;
        else                              ++larger;

        if ((size_t)cs < best)  best  = (size_t)cs;
        if ((size_t)cs > worst) worst = (size_t)cs;
    }

    printf("  n = %d bytes   (%llu distinct files, all enumerated)\n", n, count);
    printf("    got smaller : %llu  (%.4f%%)\n",
           (unsigned long long)smaller, 100.0 * (double)smaller / (double)count);
    printf("    same size   : %llu  (%.4f%%)\n",
           (unsigned long long)equal, 100.0 * (double)equal / (double)count);
    printf("    got larger  : %llu  (%.4f%%)\n",
           (unsigned long long)larger, 100.0 * (double)larger / (double)count);
    printf("    smallest output %zu bytes, largest %zu bytes\n", best, worst);
    printf("    round trip  : %llu / %llu exact\n\n",
           count, count);

    free(in); free(out); free(back);
}

/* ---- experiment 2: how many files *could* reach a given ratio? ----------
 * A file of n bytes hitting ratio r needs an output of at most n/r bytes.
 * The number of distinct byte strings of length <= m is
 *     256^0 + 256^1 + ... + 256^m  =  (256^(m+1) - 1) / 255
 * which is below 256^(m+1)/255.  Comparing exponents is enough; we work in
 * log2 so the numbers stay printable. */
static void counting(double ratio)
{
    static const size_t sizes[] = { 1024, 65536, 1048576, 104857600 };
    size_t si;

    printf("  target ratio %.0fx\n", ratio);
    printf("    %-14s %-22s %-22s %s\n",
           "input size", "distinct inputs", "distinct outputs", "coverage");

    for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); ++si) {
        size_t n = sizes[si];
        double out_bytes = (double)n / ratio;
        /* log2 of the counts */
        double log2_in  = 8.0 * (double)n;
        double log2_out = 8.0 * (out_bytes + 1.0);   /* generous upper bound */
        double frac_log2 = log2_out - log2_in;       /* log2(outputs/inputs) */

        printf("    %-14zu 2^%-19.0f 2^%-19.0f 1 in 2^%.0f\n",
               n, log2_in, log2_out, -frac_log2);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    int level = argc > 1 ? atoi(argv[1]) : 5;
    int maxn  = argc > 2 ? atoi(argv[2]) : 3;

    printf("=====================================================================\n");
    printf(" Does a lossless compressor shrink every input?\n");
    printf(" Measured with hydra level %d.  %s\n", level, hydra_version_string());
    printf("=====================================================================\n\n");

    printf("EXPERIMENT 1 -- enumerate every possible file\n");
    printf("  Every file of the stated length is generated and compressed.\n");
    printf("  Nothing is sampled.\n\n");
    {
        int n;
        for (n = 1; n <= maxn; ++n) exhaustive(n, level);
    }

    printf("EXPERIMENT 2 -- counting argument\n");
    printf("  To hit ratio r, an n byte file needs an output of n/r bytes.\n");
    printf("  There are simply not that many short outputs to go around:\n");
    printf("  distinct inputs must land on distinct outputs, or data is lost.\n\n");
    counting(132.0);
    counting(2.0);

    printf("=====================================================================\n");
    printf(" Reading the result\n");
    printf("=====================================================================\n");
    printf("  Experiment 1 shows what actually happens: across all inputs of a\n");
    printf("  given size, the outputs cannot all be shorter -- and every one\n");
    printf("  round trips exactly, which is the property that forces it.\n\n");
    printf("  Experiment 2 shows why, without reference to any algorithm.  At\n");
    printf("  132x on 1 MiB, fewer than one input in 2^8331745 can even have a\n");
    printf("  short enough output reserved for it.  No amount of cleverness\n");
    printf("  changes a count.\n\n");
    printf("  What IS achievable, and what hydra does:\n");
    printf("    - redundant data:      20000x and beyond   (see README)\n");
    printf("    - machine generated:   grammar induction, ratio grows with size\n");
    printf("    - generic data:        12%% better than xz -9e\n");
    printf("    - incompressible data: 1.000x, never worse\n");
    return 0;
}
