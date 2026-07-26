/* ===========================================================================
 * HYDRA - a from-scratch general purpose data compressor.
 *
 * Public API.  No third-party code, no external dependencies, C99.
 *
 * Copyright (c) 2026.  See LICENSE.
 * ========================================================================= */
#ifndef HYDRA_H
#define HYDRA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HYDRA_VERSION_MAJOR 1
#define HYDRA_VERSION_MINOR 0
#define HYDRA_VERSION_PATCH 0

/* ---- levels -------------------------------------------------------------
 *  1 .. 3   FAST   : LZ engine, GB/s class decode
 *  4 .. 6   STRONG : context mixing, moderate memory
 *  7 .. 9   MAX    : context mixing, wide models, best ratio
 * ------------------------------------------------------------------------ */
#define HYDRA_LEVEL_MIN     1
#define HYDRA_LEVEL_MAX     9
#define HYDRA_LEVEL_DEFAULT 5

/* ---- error codes -------------------------------------------------------- */
typedef enum {
    HYDRA_OK              =  0,
    HYDRA_E_NOMEM         = -1,
    HYDRA_E_CORRUPT       = -2,
    HYDRA_E_DSTSIZE       = -3,
    HYDRA_E_SRCSIZE       = -4,
    HYDRA_E_BADMAGIC      = -5,
    HYDRA_E_BADVERSION    = -6,
    HYDRA_E_CHECKSUM      = -7,
    HYDRA_E_PARAM         = -8,
    HYDRA_E_IO            = -9,
    HYDRA_E_INTERNAL      = -10
} hydra_status;

const char *hydra_strerror(int code);
const char *hydra_version_string(void);

/* ---- options ------------------------------------------------------------ */
typedef struct {
    int      level;        /* 1..9                                         */
    int      block_log;    /* log2 of block size, 16..30 (0 = auto)        */
    int      checksum;     /* 1 = append 64-bit content digest             */
    int      enable_delta; /* allow the numeric de-correlation filter      */
    int      enable_exe;   /* allow the x86 branch-target filter           */
    int      enable_lrm;   /* allow the long range de-duplicator           */
    int      force_method; /* -1 auto, 0 raw, 1 fast, 2 cm                 */
    int      verbose;      /* emit per-block diagnostics on stderr         */
    int      threads;      /* 0 = auto (one per core), 1 = single threaded */
    int      enable_sgi;   /* allow structural grammar induction           */
} hydra_opts;

/* Number of usable cores, as hydra would pick with threads = 0. */
int hydra_cpu_count(void);

/* Fill *o with the defaults for `level`. */
void hydra_opts_init(hydra_opts *o, int level);

/* Worst case output size for an input of `src_size` bytes. */
size_t hydra_bound(size_t src_size);

/* One-shot buffer compression.
 * Returns the number of bytes written to dst, or a negative hydra_status. */
int64_t hydra_compress(void *dst, size_t dst_cap,
                       const void *src, size_t src_size,
                       const hydra_opts *opts);

/* One-shot buffer decompression.
 * Returns the number of bytes written to dst, or a negative hydra_status. */
int64_t hydra_decompress(void *dst, size_t dst_cap,
                         const void *src, size_t src_size);

/* Reads the stored content size from a frame header.
 * Returns the size, or a negative hydra_status if it is not present. */
int64_t hydra_frame_content_size(const void *src, size_t src_size);

/* 64-bit content digest used by the frame checksum (also public: handy
 * for verifying round trips in tests). */
uint64_t hydra_digest(const void *data, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* HYDRA_H */
