/* ===========================================================================
 * HYDRA / RCD - Recursive Content Distillation.  See hz_rcd.c for the design.
 * ========================================================================= */
#ifndef HZ_RCD_H
#define HZ_RCD_H

#include <stddef.h>
#include <stdint.h>

/* Returns bytes written, or 0 if the input has no exploitable structure. */
size_t hz_rcd_compress(uint8_t *dst, size_t dst_cap,
                       const uint8_t *src, size_t n);

/* Returns 0 on success. */
int hz_rcd_decompress(uint8_t *dst, size_t n,
                      const uint8_t *src, size_t src_size);

#endif
