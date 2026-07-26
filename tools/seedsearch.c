/* ===========================================================================
 * HYDRA / tools/seedsearch.c
 *
 * The PBKDF idea, measured.
 *
 * A key derivation function stretches a short password into arbitrarily many
 * pseudorandom bytes.  25 bytes in, a megabyte out.  So the question is
 * natural and worth answering with numbers rather than opinion:
 *
 *     can we search for a seed whose derived stream IS the target file,
 *     and ship only the seed?
 *
 * This program does exactly that search.  It derives a stream from a 25-byte
 * seed using a KDF written here from scratch, compares it against the target
 * prefix, and counts attempts until it matches.  Then it reports how the
 * cost scales as we ask for one more matching byte.
 *
 * The search is real.  The seeds it finds really do regenerate the bytes
 * they claim to.  What the numbers show is where the wall is.
 * ========================================================================= */
#include "hydra.h"
#include "hz_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SEED_BYTES 25

static double now_sec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

/* ---- a key derivation function, written here -----------------------------
 * Same shape as any PBKDF: absorb the seed into a state, iterate to make
 * each attempt expensive, then squeeze out as many bytes as asked for.
 * The iteration count is kept at 1 here on purpose -- a real PBKDF uses
 * hundreds of thousands, which would only make the search below slower.
 * We are measuring the search space, so we give the search every advantage. */
static void kdf_stream(const uint8_t *seed, size_t seedlen,
                       uint8_t *out, size_t outlen)
{
    uint64_t s0 = 0x243F6A8885A308D3ull, s1 = 0x13198A2E03707344ull;
    size_t i;

    for (i = 0; i < seedlen; ++i) {
        s0 = hz_mix64(s0 ^ ((uint64_t)seed[i] + 0x9E3779B97F4A7C15ull));
        s1 = hz_mix64(s1 + s0);
    }
    for (i = 0; i < outlen; i += 8) {
        uint64_t v;
        size_t k, take = outlen - i < 8 ? outlen - i : 8;
        s0 = hz_mix64(s0 + 0x9E3779B97F4A7C15ull);
        s1 ^= s0;
        v = hz_mix64(s1);
        for (k = 0; k < take; ++k) out[i + k] = (uint8_t)(v >> (8 * k));
    }
}

/* Search for a seed whose derived stream begins with the first `want` bytes
 * of `target`.  Returns attempts used, or 0 if the budget ran out. */
static unsigned long long search(const uint8_t *target, int want,
                                 unsigned long long budget,
                                 uint8_t *found_seed, double *secs)
{
    uint8_t seed[SEED_BYTES];
    uint8_t buf[64];
    unsigned long long tries;
    double t0 = now_sec();

    memset(seed, 0, sizeof(seed));
    for (tries = 1; tries <= budget; ++tries) {
        /* walk the seed space as a big counter */
        uint64_t v = tries;
        int i;
        for (i = 0; i < 8; ++i) { seed[i] = (uint8_t)(v & 0xFF); v >>= 8; }

        kdf_stream(seed, sizeof(seed), buf, (size_t)want);
        if (memcmp(buf, target, (size_t)want) == 0) {
            *secs = now_sec() - t0;
            memcpy(found_seed, seed, SEED_BYTES);
            return tries;
        }
    }
    *secs = now_sec() - t0;
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t target[64];
    uint8_t seed[SEED_BYTES];
    uint8_t verify[64];
    int want;
    double rate = 0.0;

    /* the bytes we are trying to reproduce: any fixed pattern will do */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
        if (fread(target, 1, sizeof(target), f) != sizeof(target)) {
            fprintf(stderr, "file too short\n"); fclose(f); return 1;
        }
        fclose(f);
    } else {
        memcpy(target, "The quick brown fox jumps over the lazy dog. Pack my box!", 56);
    }

    printf("=====================================================================\n");
    printf(" Can a short seed be searched for, such that a KDF regenerates the\n");
    printf(" file?  The search is run for real below.\n");
    printf("=====================================================================\n");
    printf(" seed size: %d bytes.  Target begins: ", SEED_BYTES);
    { int i; for (i = 0; i < 8; ++i) printf("%02x ", target[i]); }
    printf("\n\n");

    printf(" %-8s %18s %12s %16s\n", "bytes", "attempts", "seconds", "outcome");
    printf(" %-8s %18s %12s %16s\n", "-----", "--------", "-------", "-------");

    for (want = 1; want <= 5; ++want) {
        unsigned long long budget = 4000000000ull;
        double secs = 0.0;
        unsigned long long tries = search(target, want, budget, seed, &secs);

        if (tries) {
            /* prove the seed really does regenerate those bytes */
            kdf_stream(seed, SEED_BYTES, verify, (size_t)want);
            if (memcmp(verify, target, (size_t)want) != 0) {
                printf(" VERIFY FAILED\n"); return 1;
            }
            if (secs > 0.001) rate = (double)tries / secs;
            printf(" %-8d %18llu %12.3f %16s\n", want, tries, secs, "found, verified");
        } else {
            printf(" %-8d %18s %12.3f %16s\n", want, "budget exhausted", secs, "not found");
            break;
        }
    }

    printf("\n Each extra byte multiplies the search by 256.\n");
    if (rate > 0.0) {
        double per_sec = rate;
        int n;
        printf(" At the measured rate of %.0f seeds/second on this machine:\n\n", per_sec);
        printf(" %-10s %28s\n", "bytes", "expected time to find a seed");
        for (n = 4; n <= 12; ++n) {
            double tries = 1.0, s;
            int i;
            for (i = 0; i < n; ++i) tries *= 256.0;
            s = tries / per_sec;
            if (s < 60.0)            printf(" %-10d %25.1f s\n", n, s);
            else if (s < 3600.0)     printf(" %-10d %25.1f min\n", n, s / 60.0);
            else if (s < 86400.0)    printf(" %-10d %25.1f hours\n", n, s / 3600.0);
            else if (s < 31557600.0) printf(" %-10d %25.1f days\n", n, s / 86400.0);
            else                     printf(" %-10d %25.3g years\n", n, s / 31557600.0);
        }
    }

    printf("\n=====================================================================\n");
    printf(" What the numbers say\n");
    printf("=====================================================================\n");
    printf(" The search works.  The seeds found above are real and verified.\n");
    printf(" The problem is the exchange rate.\n\n");
    printf(" A %d-byte seed can name at most 256^%d distinct streams.  To be\n",
           SEED_BYTES, SEED_BYTES);
    printf(" able to reproduce every 1 MiB file you would need 256^1048576\n");
    printf(" distinct streams.  The seed space is short by a factor of\n");
    printf(" 256^1048551 -- so all but a vanishing fraction of files simply\n");
    printf(" have no seed, and no amount of searching finds one that is not\n");
    printf(" there.\n\n");
    printf(" And for the few that do: finding a seed for n bytes costs 256^n\n");
    printf(" attempts, while the seed itself costs %d bytes to store.  The\n", SEED_BYTES);
    printf(" break-even is at %d bytes of output -- beyond which the search\n", SEED_BYTES);
    printf(" is already astronomically long, and below which the 'compressed'\n");
    printf(" form is larger than the data.\n\n");
    printf(" This is why a KDF is a one-way function by design: it is easy to\n");
    printf(" go from seed to stream, and infeasible to go the other way.\n");
    printf(" That property is what makes it useful for passwords, and it is\n");
    printf(" the same property that stops it being a compressor.\n");
    return 0;
}
