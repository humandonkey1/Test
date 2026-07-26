/* ===========================================================================
 * HYDRA / SGI - Structural Grammar Induction.
 *
 * A third compression mechanism, alongside LZ and context mixing.
 *
 * ---------------------------------------------------------------------------
 * WHY A THIRD ONE
 *
 * LZ asks "have I seen these bytes before?" and answers with (distance,
 * length).  Context mixing asks "given what I just saw, what comes next?"
 * and answers with a probability.  Both are enormously effective and both
 * share a blind spot: they treat the input as an undifferentiated stream of
 * bytes.  Neither ever asks what *produced* it.
 *
 * Enormous quantities of real data are machine generated -- log lines,
 * telemetry rows, database exports, serialised records, sensor dumps.  Such
 * a file is the output of a short program run many times.  A 16 MB telemetry
 * table is a four-line loop.  LZ codes it as a few hundred thousand
 * (distance, length) pairs; CM codes it as a few million well-predicted
 * bits.  Both are coding the *output* of the program.
 *
 * SGI codes the program.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT ACTUALLY DOES
 *
 * 1. RECORD INDUCTION.  Find the period at which the input repeats its
 *    structure -- not its bytes.  A delimiter histogram plus a periodicity
 *    scan recovers the row length even when no two rows are alike.
 *
 * 2. FIELD SEGMENTATION.  Split each record into fields at positions where
 *    the character *class* changes consistently across records.  This finds
 *    column boundaries in CSV, JSON, fixed-width binary and log lines alike,
 *    without knowing any of those formats.
 *
 * 3. LAW INFERENCE.  For each field independently, search a small space of
 *    generating laws:
 *
 *      CONST      the field never changes
 *      CYCLE      the field cycles through k values with period p
 *      COUNTER    the field is an integer advancing by a fixed step
 *      FLOAT_LIN  the field is a decimal advancing by a fixed step
 *      ENUM       the field takes few distinct values, coded by index
 *      RESIDUAL   no law found; hand the column to the entropy coder
 *
 *    A field with a law costs *nothing per record*.  Its entire contribution
 *    to the output is the law itself, written once in the header.
 *
 * 4. RESIDUAL ROUTING.  Fields with no law are gathered column-wise -- all
 *    of field 3's values contiguously -- and only those reach the entropy
 *    stage.  Values of one column resemble each other far more than the
 *    bytes adjacent to them in the original row, which is exactly the
 *    locality a context model wants.
 *
 * The decoder re-runs the laws.  For a fully-lawful file it never touches
 * an entropy coder at all: it walks the field table and writes bytes, which
 * is why decode is memory-bandwidth bound rather than model bound.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS BUYS, AND WHAT IT DOES NOT
 *
 * On machine generated data the ratio is not incrementally better, it is
 * categorically different, because the output size stops depending on the
 * input size at all.  A law costs the same whether the loop ran a thousand
 * times or a billion.
 *
 * On prose, on photographs, on already-compressed data, there is no law to
 * find.  SGI detects this during induction, declines, and the block falls
 * through to the engines that do work there.  This is a specialist, and it
 * is honest about it.
 * ========================================================================= */
#ifndef HZ_SGI_H
#define HZ_SGI_H

#include "hz_int.h"

/* ---- field laws ---------------------------------------------------------- */
#define SGI_LAW_CONST     0   /* same bytes every record                     */
#define SGI_LAW_CYCLE     1   /* cycles through a table with period p        */
#define SGI_LAW_COUNTER   2   /* integer, value = base + step * index        */
#define SGI_LAW_FLOAT_LIN 3   /* fixed-point decimal, base + step * index    */
#define SGI_LAW_ENUM      4   /* few distinct values, index coded per record */
#define SGI_LAW_RESIDUAL  5   /* no law; column routed to the entropy stage  */

#define SGI_MAX_FIELDS    64
#define SGI_MAX_ENUM      256
#define SGI_MAX_CYCLE     4096

/* Compress with structural grammar induction.
 * Returns bytes written, or 0 if no exploitable structure was found (in
 * which case the caller should use another engine). */
size_t hz_sgi_compress(uint8_t *dst, size_t dst_cap,
                       const uint8_t *src, size_t n, int level);

/* Returns 0 on success. */
int hz_sgi_decompress(uint8_t *dst, size_t n,
                      const uint8_t *src, size_t src_size);

/* Cheap test: is this input worth attempting?  Runs the record and field
 * induction only, and reports how much of each record is explained by a
 * law.  Returns a score in 0..100. */
int hz_sgi_probe(const uint8_t *src, size_t n);

#endif /* HZ_SGI_H */
