/* ===========================================================================
 * HYDRA internals: portable memory access, deterministic fixed point math,
 * hashing and the shared frame layout constants.
 *
 * Everything here is integer-only on purpose.  The context mixing coder has
 * to run the *exact* same model update on the encoder and the decoder, so a
 * single ULP of floating point drift anywhere would desynchronise the two.
 * ========================================================================= */
#ifndef HZ_INT_H
#define HZ_INT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* ---- compiler helpers --------------------------------------------------- */
#if defined(__GNUC__) || defined(__clang__)
#  define HZ_INLINE   static inline __attribute__((always_inline))
#  define HZ_NOINLINE __attribute__((noinline))
#  define HZ_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define HZ_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define HZ_RESTRICT __restrict__
#else
#  define HZ_INLINE   static inline
#  define HZ_NOINLINE
#  define HZ_LIKELY(x)   (x)
#  define HZ_UNLIKELY(x) (x)
#  define HZ_RESTRICT
#endif

/* ---- unaligned little endian access ------------------------------------- */
HZ_INLINE uint16_t hz_rd16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
HZ_INLINE uint32_t hz_rd32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
HZ_INLINE uint64_t hz_rd64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
HZ_INLINE void hz_wr16(void *p, uint16_t v) { memcpy(p, &v, 2); }
HZ_INLINE void hz_wr32(void *p, uint32_t v) { memcpy(p, &v, 4); }
HZ_INLINE void hz_wr64(void *p, uint64_t v) { memcpy(p, &v, 8); }

/* Explicit little-endian store/load for anything that hits the bitstream. */
HZ_INLINE void hz_put32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
HZ_INLINE uint32_t hz_get32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
HZ_INLINE void hz_put64le(uint8_t *p, uint64_t v) {
    hz_put32le(p, (uint32_t)v); hz_put32le(p + 4, (uint32_t)(v >> 32));
}
HZ_INLINE uint64_t hz_get64le(const uint8_t *p) {
    return (uint64_t)hz_get32le(p) | ((uint64_t)hz_get32le(p + 4) << 32);
}

/* ---- bit tricks --------------------------------------------------------- */
HZ_INLINE int hz_ctz64(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(v);
#else
    int n = 0; while (!(v & 1)) { v >>= 1; ++n; } return n;
#endif
}
HZ_INLINE int hz_clz32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return v ? __builtin_clz(v) : 32;
#else
    int n = 0; if (!v) return 32; while (!(v & 0x80000000u)) { v <<= 1; ++n; } return n;
#endif
}
HZ_INLINE int hz_log2_floor(uint32_t v) { return 31 - hz_clz32(v | 1); }

/* Count of equal leading bytes between two little endian words. */
HZ_INLINE int hz_match_len64(uint64_t a, uint64_t b) {
    uint64_t d = a ^ b;
    return d ? (hz_ctz64(d) >> 3) : 8;
}

/* ---- hashing ------------------------------------------------------------
 * A small multiply-shift family.  These are not cryptographic; they only
 * need good avalanche in the high bits because every user shifts right.   */
#define HZ_PRIME32_1 2654435761u
#define HZ_PRIME32_2 2246822519u
#define HZ_PRIME64_1 0x9E3779B185EBCA87ull
#define HZ_PRIME64_2 0xC2B2AE3D27D4EB4Full
#define HZ_PRIME64_3 0x165667B19E3779F9ull

HZ_INLINE uint32_t hz_hash4(uint32_t v, int bits) {
    return (v * HZ_PRIME32_1) >> (32 - bits);
}
HZ_INLINE uint32_t hz_hash5(uint64_t v, int bits) {
    return (uint32_t)(((v << 24) * HZ_PRIME64_1) >> (64 - bits));
}
HZ_INLINE uint32_t hz_hash6(uint64_t v, int bits) {
    return (uint32_t)(((v << 16) * HZ_PRIME64_1) >> (64 - bits));
}
HZ_INLINE uint32_t hz_hash8(uint64_t v, int bits) {
    return (uint32_t)((v * HZ_PRIME64_1) >> (64 - bits));
}
HZ_INLINE uint32_t hz_mix32(uint32_t h) {
    h ^= h >> 15; h *= HZ_PRIME32_2; h ^= h >> 13; h *= HZ_PRIME32_1;
    h ^= h >> 16; return h;
}
HZ_INLINE uint64_t hz_mix64(uint64_t h) {
    h ^= h >> 33; h *= HZ_PRIME64_2; h ^= h >> 29; h *= HZ_PRIME64_3;
    h ^= h >> 32; return h;
}

/* ---- deterministic fixed point exp2 -------------------------------------
 * HZ_EXP2T[k] = 2^(2^-(k+1)) in Q31.  Binary decomposition of the fraction
 * gives 2^f with a relative error below 3e-5, identically on every target. */
static const uint32_t HZ_EXP2T[16] = {
    3037000500u, 2553802834u, 2341847524u, 2242560872u,
    2194507417u, 2170868212u, 2159144272u, 2153306067u,
    2150392887u, 2148937775u, 2148210589u, 2147847087u,
    2147665360u, 2147574502u, 2147529075u, 2147506361u
};

/* arg is Q16.16, returns 2^arg in Q16.16 as a 64 bit value.
 * The integer part may reach 24, so the result is widened deliberately:
 * squashing the full logit domain needs e^16, which does not fit in 32. */
HZ_INLINE uint64_t hz_fx_exp2(uint32_t arg)
{
    uint32_t ip = arg >> 16, fr = arg & 0xFFFFu, r = 1u << 31;
    int k;
    for (k = 0; k < 16; ++k)
        if (fr & (0x8000u >> k))
            r = (uint32_t)(((uint64_t)r * HZ_EXP2T[k]) >> 31);
    if (ip > 40) ip = 40;
    return ((uint64_t)(r >> 15)) << ip;   /* Q31 -> Q16.16 then scale */
}

/* 65536 / (256 * ln 2) expressed as num/4096 */
#define HZ_EXP_SCALE_NUM 1512775u

/* ---- squash / stretch ---------------------------------------------------
 * squash(x) = 65536 / (1 + e^(-x/256))  domain [-2047,2047] -> [1,65535]
 * stretch   = its inverse               domain [0,65535]    -> [-2047,2047]
 *
 * Probabilities are carried at 16 bits everywhere so that a saturated model
 * can still drive the coder below a thousandth of a bit per symbol.  The
 * stretch table is indexed by the top 12 bits, which is ample: the mixer
 * only ever needs the logit to within a quarter of a unit.
 * Both tables are built once from the integer exp2 above, so every target
 * produces byte identical output.                                        */
/* The logit domain is +-HZ_ST_MAX at a scale of 1/256 nat per step, which
 * reaches e^16 and therefore spans the whole 16 bit probability range.  A
 * narrower domain would cap how certain the model is ever allowed to be and
 * would put a hard floor under the output size on very predictable data. */
#define HZ_ST_MAX  4095
#define HZ_ST_SIZE 8192                    /* index = x + 4096 */

extern uint16_t hz_squash_tab[HZ_ST_SIZE]; /* index = x + 4096 */
extern int16_t  hz_stretch_tab[4096];      /* index = p16 >> 4 */

/* The 12 bit index above is plenty across the middle of the range but it
 * collapses the tails: every p below 16 lands in bucket 0, and it is exactly
 * there that a strong model spends its time.  Two dedicated tables give the
 * outer 512 codes their own full resolution entry. */
#define HZ_TAIL 512
extern int16_t  hz_stretch_lo[HZ_TAIL];   /* p in [0, 511]           */
extern int16_t  hz_stretch_hi[HZ_TAIL];   /* p in [65024, 65535]     */

void hz_tables_init(void);

/* logit -> probability, Q16.  Purely table driven: any clamp applied here
 * that the table did not also apply would break stretch/squash agreement. */
HZ_INLINE int hz_squash(int x) {
    if (x < -HZ_ST_MAX) x = -HZ_ST_MAX;
    else if (x > HZ_ST_MAX) x = HZ_ST_MAX;
    return (int)hz_squash_tab[x + 4096];
}
/* probability (Q16) -> logit */
HZ_INLINE int hz_stretch(int p) {
    if (HZ_UNLIKELY(p < HZ_TAIL))            return (int)hz_stretch_lo[p];
    if (HZ_UNLIKELY(p >= 65536 - HZ_TAIL))   return (int)hz_stretch_hi[p - (65536 - HZ_TAIL)];
    return (int)hz_stretch_tab[(unsigned)p >> 4];
}

/* Adaptation rates for the counters: HZ_RATE[n] ~= 65536/(n+2), capped so
 * that (delta * rate) can never leave the signed 32 bit range. */
extern uint16_t hz_rate_tab[256];

/* ---- misc --------------------------------------------------------------- */
HZ_INLINE int hz_clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
HZ_INLINE size_t hz_minz(size_t a, size_t b) { return a < b ? a : b; }
HZ_INLINE size_t hz_maxz(size_t a, size_t b) { return a > b ? a : b; }

/* ---- frame / block layout ----------------------------------------------
 *
 * Frame
 *   magic      4   "HYDR"
 *   version    1
 *   flags      1   bit0 content-size present, bit1 digest present
 *   blocks ...
 *   terminator 1   HZ_BLK_END
 *   digest     8   (optional)
 *
 * Block
 *   method     1   see HZ_M_*
 *   nfilters   1
 *   filters    2 * nfilters   (id, param)
 *   usize      4   size of this block before any filtering
 *   csize      4   size of the payload that follows
 *   payload  csize
 * ------------------------------------------------------------------------ */
#define HZ_MAGIC0 'H'
#define HZ_MAGIC1 'Y'
#define HZ_MAGIC2 'D'
#define HZ_MAGIC3 'R'
#define HZ_FORMAT_VERSION 1

#define HZ_FLAG_CSIZE  0x01
#define HZ_FLAG_DIGEST 0x02

#define HZ_M_RAW  0
#define HZ_M_FAST 1
#define HZ_M_MID  2
#define HZ_M_CM   3
#define HZ_BLK_END 0xFF

#define HZ_F_NONE  0
#define HZ_F_DELTA 1
#define HZ_F_EXE   2
#define HZ_F_LRM   3
#define HZ_F_SHUF  4
#define HZ_MAX_FILTERS 6

#define HZ_MAX_THREADS 64

#define HZ_BLOCK_LOG_MIN 16
#define HZ_BLOCK_LOG_MAX 30

/* ---- internal entry points ---------------------------------------------- */

/* FAST LZ engine (hz_fast.c) */
size_t hz_fast_compress(uint8_t *dst, size_t dst_cap,
                        const uint8_t *src, size_t src_size, int level);
int    hz_fast_decompress(uint8_t *dst, size_t dst_size,
                          const uint8_t *src, size_t src_size);

/* STRONG engine (hz_mid.c): LZ parse coded through the range coder. */
size_t hz_mid_compress(uint8_t *dst, size_t dst_cap,
                       const uint8_t *src, size_t n, int level);
int    hz_mid_decompress(uint8_t *dst, size_t n,
                         const uint8_t *src, size_t src_size);

/* Context mixing engine (hz_cm.c).  The payload is self describing: the
 * first byte records the table geometry, so the decoder needs no level. */
size_t hz_cm_compress(uint8_t *dst, size_t dst_cap,
                      const uint8_t *src, size_t src_size, int level);
int    hz_cm_decompress(uint8_t *dst, size_t dst_size,
                        const uint8_t *src, size_t src_size);

/* Filters (hz_filter.c) */
void   hz_delta_fwd(uint8_t *buf, size_t n, int stride);
void   hz_delta_rev(uint8_t *buf, size_t n, int stride);
void   hz_shuf_fwd(uint8_t *buf, uint8_t *scratch, size_t n, int width);
void   hz_shuf_rev(uint8_t *buf, uint8_t *scratch, size_t n, int width);
void   hz_exe_fwd(uint8_t *buf, size_t n);
void   hz_exe_rev(uint8_t *buf, size_t n);
size_t hz_lrm_fwd(uint8_t *dst, size_t dst_cap, const uint8_t *src, size_t n);
int    hz_lrm_rev(uint8_t *dst, size_t dst_cap, size_t *out_size,
                  const uint8_t *src, size_t n);

/* Content analysis (hz_filter.c) */
typedef struct {
    int    best_shuffle_width; /* 0 if a byte transpose does not apply */
    int    best_delta_stride;  /* 0 if delta does not help          */
    int    is_x86;             /* heuristic: looks like x86 machine code */
    int    lrm_worth;          /* heuristic: long range dupes present    */
    double order0_entropy;     /* bits per byte                     */
} hz_analysis;
void hz_analyze(const uint8_t *src, size_t n, hz_analysis *a);

#endif /* HZ_INT_H */
