/* ===========================================================================
 * HYDRA / tools/proof2.c
 *
 * The honest version of the enumeration.
 *
 * proof.c enumerated 1 to 3 byte files, where the frame header dwarfs the
 * payload and every output looks "larger" for an uninteresting reason.  This
 * program removes that objection three ways:
 *
 *   1. It measures the raw entropy coder with no frame at all, so there is
 *      no header to blame.
 *   2. It enumerates every file of a size where a header cannot dominate.
 *   3. It reports the distribution of output sizes, not just a verdict.
 *
 * The question being tested is precise:
 *
 *     Can a lossless codec make *every* input of length n shorter?
 *
 * Not "usually", not "on realistic data" -- every one.  That is what "132x
 * on all files in the world" requires.
 * ========================================================================= */
#include "hydra.h"
#include "hz_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Enumerate every file of n bytes and histogram the *payload* size, with the
 * container excluded so the header cannot be blamed for anything. */
static void enumerate_payload(int n, int level)
{
    unsigned long long count = 1, i;
    size_t cap = (size_t)n * 4 + 4096;
    uint8_t *in  = (uint8_t *)malloc((size_t)n);
    uint8_t *out = (uint8_t *)malloc(cap);
    uint8_t *back = (uint8_t *)malloc((size_t)n);
    size_t *hist;
    size_t maxout = 0;
    unsigned long long smaller = 0, notsmaller = 0;
    int k;

    for (k = 0; k < n; ++k) count *= 256;
    hist = (size_t *)calloc(cap + 1, sizeof(size_t));
    if (!in || !out || !back || !hist) { fprintf(stderr, "oom\n"); exit(1); }

    for (i = 0; i < count; ++i) {
        unsigned long long v = i;
        size_t cs;
        for (k = 0; k < n; ++k) { in[k] = (uint8_t)(v & 0xFF); v >>= 8; }

        /* raw engine, no frame, no header, no checksum */
        cs = hz_mid_compress(out, cap, in, (size_t)n, level);
        if (cs == 0) { cs = (size_t)n; }        /* engine declined: stored */
        else {
            if (hz_mid_decompress(back, (size_t)n, out, cs) != 0 ||
                memcmp(in, back, (size_t)n) != 0) {
                fprintf(stderr, "ROUND TRIP FAILURE at %llu\n", i);
                exit(1);
            }
        }

        if (cs <= cap) ++hist[cs];
        if (cs > maxout) maxout = cs;
        if (cs < (size_t)n) ++smaller; else ++notsmaller;
    }

    printf("  n = %d bytes, %llu files enumerated, entropy coder only\n", n, count);
    printf("    strictly smaller than input : %llu (%.4f%%)\n",
           smaller, 100.0 * (double)smaller / (double)count);
    printf("    not smaller                 : %llu (%.4f%%)\n",
           notsmaller, 100.0 * (double)notsmaller / (double)count);
    printf("    output size distribution:\n");
    {
        size_t s;
        for (s = 0; s <= maxout && s <= cap; ++s)
            if (hist[s])
                printf("      %3zu bytes : %-14zu %s\n", s, hist[s],
                       s < (size_t)n ? "(shrunk)" : (s == (size_t)n ? "(same)" : "(grew)"));
    }
    printf("\n");

    free(in); free(out); free(back); free(hist);
}

/* The pigeonhole statement, computed rather than asserted.
 *
 * If a codec maps every n-byte input to at most m bytes, and it is lossless,
 * the map is injective: distinct inputs give distinct outputs.  So the number
 * of available outputs must be at least the number of inputs.  Below we
 * compute both counts exactly for small n and report the shortfall. */
static void pigeonhole_exact(void)
{
    int n;
    printf("  %-6s %-22s %-24s %s\n",
           "n", "inputs (256^n)", "outputs of <= n-1 bytes", "shortfall");
    for (n = 1; n <= 8; ++n) {
        /* inputs = 256^n ; outputs = sum_{k=0}^{n-1} 256^k = (256^n - 1)/255 */
        double inputs = 1.0, outputs;
        int k;
        for (k = 0; k < n; ++k) inputs *= 256.0;
        outputs = (inputs - 1.0) / 255.0;
        printf("  %-6d %-22.0f %-24.0f %.0f inputs have nowhere to go\n",
               n, inputs, outputs, inputs - outputs);
    }
    printf("\n");
    printf("  Every row is the same statement: to shrink even by one byte,\n");
    printf("  255 out of every 256 inputs must map somewhere that is already\n");
    printf("  taken.  Shrinking *all* of them is not a hard engineering\n");
    printf("  problem, it is a request for an injective map into a smaller\n");
    printf("  set.\n\n");
}

int main(int argc, char **argv)
{
    int level = argc > 1 ? atoi(argv[1]) : 5;
    int maxn  = argc > 2 ? atoi(argv[2]) : 3;
    int n;

    printf("=====================================================================\n");
    printf(" Can a lossless codec shrink EVERY input?  Measured, not argued.\n");
    printf(" %s, level %d, entropy coder in isolation (no frame header)\n",
           hydra_version_string(), level);
    printf("=====================================================================\n\n");

    printf("PART 1 -- exhaustive enumeration\n\n");
    for (n = 1; n <= maxn; ++n) enumerate_payload(n, level);

    printf("PART 2 -- the counting, computed exactly\n\n");
    pigeonhole_exact();

    printf("=====================================================================\n");
    printf(" Conclusion\n");
    printf("=====================================================================\n");
    printf(" Part 1 is an experiment: every file of the stated size, run\n");
    printf(" through the real coder, every one verified to round trip.  The\n");
    printf(" fraction that shrinks is what it is.\n\n");
    printf(" Part 2 explains it without mentioning any algorithm.  The limit\n");
    printf(" is not in the code; it is in how many short files exist.\n\n");
    printf(" This is why hydra targets the data that IS compressible, and\n");
    printf(" why it stores incompressible blocks verbatim instead of\n");
    printf(" pretending: 1.000x is the correct answer for random input, and\n");
    printf(" anything claiming otherwise is either lossy or miscounting.\n");
    return 0;
}
