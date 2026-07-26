/* ===========================================================================
 * HYDRA: the context mixing engine.
 *
 * One bit at a time, MSB first within each byte.  For every bit:
 *
 *   1. each model looks up its context and produces a probability
 *   2. a gated logistic mixer combines them
 *   3. two APM stages refine the result
 *   4. the range coder consumes the final probability
 *   5. every model that contributed is updated with the true bit
 *
 * Encoder and decoder run the identical code path; the only difference is
 * where the bit comes from.  That is enforced structurally below by having
 * both call the same predict/update pair.
 *
 * Models
 *   - orders 1..6 over a shared hash table of bit-history slots
 *   - a word model keyed on the current alphanumeric run
 *   - a sparse model that skips a byte, for interleaved/columnar data
 *   - a match model that predicts the continuation of the longest repeat
 *
 * The match model is what carries highly redundant input: once it locks on,
 * it drives the mixer to near-certainty and the coder spends a tiny fraction
 * of a bit per byte, which is where the very large ratios come from.
 * ========================================================================= */
#include "hz_int.h"
#include "hz_rc.h"
#include "hz_model.h"

/* ---- geometry -----------------------------------------------------------
 * Hashed contexts: orders 1,2,3,4,6,8, a word model, a sparse model, and
 * two record-relative models keyed on the previous record's bytes.
 * Mixer inputs: those 8, plus direct order-0 and order-1, plus the match
 * prediction, a match-length term and a constant bias. */
#define HZ_NHASH    11
#define HZ_NMIXIN   (HZ_NHASH + 5)

/* A slot covers one *nibble* of the byte tree: 4 bits form 15 internal
 * nodes (1 + 2 + 4 + 8).  Contexts are re-hashed at the nibble boundary
 * with the high nibble folded in, so the low four bits get their own
 * statistics instead of aliasing onto the high four.  The struct is exactly
 * 16 bytes, so a lookup touches a single cache line. */
typedef struct {
    uint8_t  chk;     /* context checksum, for collision detection */
    uint8_t  st[15];  /* bit-history states, one per node of the nibble tree */
} hz_slot;

typedef struct {
    hz_slot *tab;
    uint32_t mask;
    int      bits;
} hz_htab;

static int hz_htab_init(hz_htab *h, int bits)
{
    size_t n = (size_t)1 << bits;
    h->tab = (hz_slot *)calloc(n, sizeof(hz_slot));
    if (!h->tab) return -1;
    h->mask = (uint32_t)(n - 1);
    h->bits = bits;
    return 0;
}
static void hz_htab_free(hz_htab *h) { free(h->tab); h->tab = NULL; }

/* Find the slot for a context hash.  Two-way associative: on a checksum
 * miss we take the neighbour, and if that is also taken we reset the one
 * whose history carries the least evidence.  Silently sharing a slot
 * between two contexts is far more damaging than dropping an old one. */
HZ_INLINE hz_slot *hz_htab_get(hz_htab *h, uint32_t hash)
{
    uint32_t i = hash & h->mask;
    uint8_t  c = (uint8_t)(hash >> 24);
    hz_slot *a = &h->tab[i];
    hz_slot *b = &h->tab[i ^ 1];
    if (a->chk == c) return a;
    if (b->chk == c) return b;
    /* prefer to evict the slot whose root node has seen less */
    if (a->st[0] <= b->st[0]) {
        memset(a, 0, sizeof(*a)); a->chk = c; return a;
    }
    memset(b, 0, sizeof(*b)); b->chk = c; return b;
}

/* ---- per-state learned probability -------------------------------------
 * Maps a bit-history state to a probability, learned globally.  This is the
 * indirect step: contexts with the same *shape* of history share statistics
 * even when they are completely different byte strings. */
typedef struct { hz_ctr p[256]; } hz_smap;

static void hz_smap_init(hz_smap *s)
{
    int i;
    for (i = 0; i < 256; ++i) s->p[i] = HZ_CTR_INIT;
}

/* ---- match model -------------------------------------------------------- */
typedef struct {
    uint32_t *tab;      /* hash of last 8 bytes -> position */
    uint32_t  mask;
    size_t    ptr;      /* position in the history that we are tracking */
    size_t    len;      /* how many bytes have matched so far */
    int       expected; /* the byte the match predicts */
    hz_ctr    cm[64 * 256];  /* (length bucket, expected bit ctx) -> p */
} hz_match;

static int hz_match_init(hz_match *m, int bits)
{
    size_t i;
    m->tab = (uint32_t *)calloc((size_t)1 << bits, sizeof(uint32_t));
    if (!m->tab) return -1;
    m->mask = (uint32_t)(((size_t)1 << bits) - 1);
    m->ptr = 0; m->len = 0; m->expected = -1;
    for (i = 0; i < 64 * 256; ++i) m->cm[i] = HZ_CTR_INIT;
    return 0;
}
static void hz_match_free(hz_match *m) { free(m->tab); m->tab = NULL; }

/* ---- the model bundle --------------------------------------------------- */
typedef struct {
    hz_htab  ht;
    hz_smap  sm[HZ_NHASH];
    hz_ctr  *o0;          /* order 0: direct, 256 nodes */
    hz_ctr  *o1;          /* order 1: direct, 256*256   */
    hz_match mm;
    hz_mixer mx;
    hz_apm   apm1, apm2;

    /* rolling context state */
    const uint8_t *hist;
    size_t   hpos;
    uint32_t c4;          /* last 4 bytes */
    uint64_t c8;          /* last 8 bytes */
    uint32_t wordhash;
    uint32_t rec_stride;  /* detected record width, 0 = unknown */
    uint32_t rec_pos;     /* byte offset within the current record */
    uint64_t rec_hash;    /* digest of the whole previous record   */
    int      c0;          /* current partial byte, with a leading 1 bit */
    int      bitpos;

    /* per-nibble precomputed context slots */
    uint32_t chash[HZ_NHASH];
    uint8_t *cstate[HZ_NHASH];
    int      nibnode;     /* partial nibble with a leading 1 bit, 1..15 */

    int      nmodels;
    int      memlog;
} hz_cm;

static void hz_cm_free(hz_cm *m)
{
    hz_htab_free(&m->ht);
    hz_match_free(&m->mm);
    hz_mixer_free(&m->mx);
    hz_apm_free(&m->apm1);
    hz_apm_free(&m->apm2);
    free(m->o0); m->o0 = NULL;
    free(m->o1); m->o1 = NULL;
}

/* Build the model for an explicit geometry.
 *
 * htbits is the single number that defines the layout, and it is the number
 * stored in the payload header.  Both sides go through this function so
 * there is exactly one description of the model's shape. */
/* Detect a fixed record width by positional agreement.
 *
 * Cheap and deliberately conservative: a width only counts if bytes at the
 * same offset in consecutive records agree far more often than chance.  The
 * smallest qualifying width wins, since every multiple of the true width
 * scores just as well and using a multiple would waste the context.
 *
 * Returning 0 disables the record models entirely, which is the right
 * answer for text and for anything else without a fixed layout. */
static uint32_t hz_detect_stride(const uint8_t *src, size_t n)
{
    static const uint32_t CAND[] = { 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 };
    size_t sample = n < (1u << 18) ? n : (1u << 18);
    uint32_t best = 0;
    unsigned ci;
    size_t base_hits = 0, i;

    if (sample < 4096) return 0;

    /* baseline: how often does a byte equal the one before it at all */
    for (i = 1; i < sample; ++i) if (src[i] == src[i - 1]) ++base_hits;

    for (ci = 0; ci < sizeof(CAND) / sizeof(CAND[0]); ++ci) {
        uint32_t w = CAND[ci];
        size_t hits = 0, cnt = 0;
        if ((size_t)w * 64 > sample) break;
        for (i = w; i < sample; ++i, ++cnt)
            if (src[i] == src[i - w]) ++hits;
        /* must beat both chance and the trivial adjacent-byte agreement */
        if (cnt && hits * 100 > cnt * 30 && hits * 4 > base_hits * 5) {
            best = w;
            break;
        }
    }
    return best;
}

static int hz_cm_init_geom(hz_cm *m, int htbits, const uint8_t *hist)
{
    int i, mmbits;
    size_t j;

    hz_tables_init();
    hz_states_init();
    memset(m, 0, sizeof(*m));

    if (htbits < 16 || htbits > 26) return -1;
    mmbits = htbits - 2;
    if (mmbits < 14) mmbits = 14;
    m->memlog = htbits;

    if (hz_htab_init(&m->ht, htbits) < 0) return -1;
    if (hz_match_init(&m->mm, mmbits) < 0) { hz_cm_free(m); return -1; }

    m->o0 = (hz_ctr *)malloc(256 * sizeof(hz_ctr));
    m->o1 = (hz_ctr *)malloc(256 * 256 * sizeof(hz_ctr));
    if (!m->o0 || !m->o1) { hz_cm_free(m); return -1; }
    for (j = 0; j < 256; ++j)       m->o0[j] = HZ_CTR_INIT;
    for (j = 0; j < 256 * 256; ++j) m->o1[j] = HZ_CTR_INIT;

    for (i = 0; i < HZ_NHASH; ++i) hz_smap_init(&m->sm[i]);

    /* The mixer is gated on the current partial byte and on whether the
     * match model is live: those two facts change which models deserve
     * trust more than anything else does. */
    if (hz_mixer_init(&m->mx, HZ_NMIXIN, 256 * 2, 6) < 0) { hz_cm_free(m); return -1; }
    if (hz_apm_init(&m->apm1, 256, 6) < 0) { hz_cm_free(m); return -1; }
    if (hz_apm_init(&m->apm2, 256 * 16, 6) < 0) { hz_cm_free(m); return -1; }

    m->hist = hist;
    m->hpos = 0;
    m->c0 = 1;
    m->bitpos = 0;
    m->nmodels = HZ_NMIXIN;
    return 0;
}

/* Choose the geometry for a given level and input size.
 *
 * Memory grows with level, but it is capped against how much data there
 * actually is to model.  A table far larger than the input is not free:
 * contexts scatter across cold slots, each accumulating too few
 * observations to become confident, and the two-way eviction never gets a
 * chance to resolve anything.  Measured on 4 MiB of English text, going
 * from 8M to 16M slots *cost* half a percent.  Keeping roughly a handful of
 * slots per input byte is where the curve flattens. */
static int hz_cm_geometry(int level, size_t hsize)
{
    int htbits, cap;

    /* Slots per level.  These are budgets for the *whole* model, and with
     * eleven contexts each inserting per position a level needs roughly
     * eleven slots per input byte before eviction stops hurting. */
    if      (level <= 4) htbits = 21;
    else if (level <= 6) htbits = 23;
    else if (level <= 8) htbits = 25;
    else                 htbits = 26;

    /* Smallest power of two covering the input, plus headroom.
     *
     * The cap has to scale with the number of *contexts*, not just the
     * input: every model inserts its own entry per position, so N models
     * over n bytes want room for N*n slots, not n.  When two record-aware
     * contexts were added the table silently became 1.4x more crowded, and
     * the symptom was ugly -- cost per value climbing from 1.77 bits at
     * 12k values to 2.70 at 400k, a model getting *worse* as it saw more
     * data, which is exactly what eviction thrash looks like. */
    cap = 16;
    while (cap < 26 && ((size_t)1 << cap) < hsize) ++cap;
    cap += 2;
    {
        /* one extra doubling per four contexts beyond the original eight */
        int extra = (HZ_NHASH - 8 + 3) / 4;
        cap += extra;
    }
    if (htbits > cap) htbits = cap;
    if (htbits < 16) htbits = 16;
    if (htbits > 26) htbits = 26;
    return htbits;
}

/* Compute the per-nibble context slots.
 *
 * `nib` is 0 at a byte boundary and 1 once the high nibble is known; in the
 * second case its value is folded into every hash so that the low nibble is
 * modelled under a genuinely different context.  Without that fold the two
 * halves of a byte would share one set of 15 nodes and collide constantly. */
static void hz_cm_refresh(hz_cm *m, int nib, int highnib)
{
    uint32_t salt = nib ? (uint32_t)(highnib | 0x10) * HZ_PRIME32_2 : 0;
    uint32_t h;
    int k = 0;

    /* orders 1,2,3,4,6,8 */
    m->chash[k++] = hz_hash4((m->c4 & 0x000000FFu) + (1u << 8)  + salt, m->ht.bits);
    m->chash[k++] = hz_hash4((m->c4 & 0x0000FFFFu) + (2u << 16) + salt, m->ht.bits);
    m->chash[k++] = hz_hash4((m->c4 & 0x00FFFFFFu) + (3u << 24) + salt, m->ht.bits);
    m->chash[k++] = hz_hash4(m->c4 * HZ_PRIME32_2 + 4u + salt,          m->ht.bits);
    m->chash[k++] = hz_hash6((m->c8 & 0x0000FFFFFFFFFFFFull) + salt,    m->ht.bits);
    m->chash[k++] = hz_hash8(m->c8 + salt,                              m->ht.bits);
    /* word model: hash of the current run of letters/digits */
    m->chash[k++] = hz_hash4(m->wordhash + 7u + salt, m->ht.bits);
    /* sparse: bytes at distance 2 and 4, for columnar records */
    h = (uint32_t)((m->c4 >> 8) & 0xFF) | (((uint32_t)((m->c4 >> 24) & 0xFF)) << 8);
    m->chash[k++] = hz_hash4(h * HZ_PRIME32_1 + 11u + salt, m->ht.bits);

    /* Record-relative contexts.
     *
     * On an array of fixed width values -- floats, structs, samples -- the
     * strongest predictor of a byte is not the byte before it but the
     * corresponding byte one record back.
     *
     * The first version of this hashed a single aligned byte, and that was
     * not enough.  Measured on a quantised sensor series: order-1 over
     * *whole* 8-byte words costs 1.37 bits per value, while a model keyed
     * on one byte of the previous word was still spending nearly three
     * times that.  The law relates entire values, so the context has to be
     * the entire previous value.
     *
     * Three contexts now:
     *   - the whole previous record, rolled into one hash, plus position
     *   - the aligned byte alone, which stays useful when records are long
     *     and only the nearby column matters
     *   - the aligned byte paired with the byte just coded
     *
     * m->rec_hash carries the rolling digest of the previous record; it is
     * recomputed once per record boundary rather than per byte. */
    {
        uint32_t st = m->rec_stride;
        uint32_t prevb = 0, posn = 0;
        if (st && m->hpos >= st) {
            prevb = m->hist[m->hpos - st];
            posn  = m->rec_pos;
        }
        m->chash[k++] = hz_hash8(m->rec_hash * HZ_PRIME64_1
                                 + (uint64_t)posn * 0x9E3779B9u
                                 + 29u + salt, m->ht.bits);
        m->chash[k++] = hz_hash4(prevb * 2654435761u + posn * 40503u
                                 + 17u + salt, m->ht.bits);
        m->chash[k++] = hz_hash4((prevb | ((m->c4 & 0xFF) << 8)
                                  | (posn << 16)) * HZ_PRIME32_2
                                 + 23u + salt, m->ht.bits);
    }

    for (k = 0; k < HZ_NHASH; ++k)
        m->cstate[k] = hz_htab_get(&m->ht, m->chash[k])->st;

    m->nibnode = 1;
}

/* Locate/extend the match model against the history. */
/* Locate or extend the match: the model that carries redundant data.
 *
 * Two details matter for ratio.  First, when a match breaks we immediately
 * look for a replacement in the same call rather than waiting for the next
 * byte -- at a record boundary the old match dies and a new one is
 * available at once, and skipping a byte there costs a full mispredicted
 * symbol.  Second, the candidate is verified against real history before
 * being trusted, so a hash collision cannot inject a confident wrong
 * prediction; that is the failure mode that makes a match model actively
 * harmful rather than merely useless. */
static void hz_cm_match_update(hz_cm *m)
{
    uint32_t h;
    size_t pos = m->hpos;

    if (pos < 8) { m->mm.len = 0; m->mm.expected = -1; return; }

    h = hz_hash8(m->c8, 32) & m->mm.mask;

    if (m->mm.len > 0) {
        if (m->mm.ptr < pos && m->hist[m->mm.ptr] == m->hist[pos - 1]) {
            ++m->mm.ptr;
            if (m->mm.len < 65534) ++m->mm.len;
        } else {
            m->mm.len = 0;
        }
    }

    if (m->mm.len == 0) {
        uint32_t cand = m->mm.tab[h];
        if (cand > 0 && (size_t)cand < pos) {
            size_t l = 0, lim = pos < 64 ? pos : 64;
            while (l < lim && l < (size_t)cand &&
                   m->hist[cand - 1 - l] == m->hist[pos - 1 - l]) ++l;
            /* Require a real run before trusting it.  Below this the
             * prediction is mostly noise and the mixer has to spend capacity
             * learning to ignore it, which costs more than leaving the input
             * at zero.  Swept on text and record data: 6 and 8 are clearly
             * worse, 16 starts losing genuine matches, 10 is the plateau. */
            if (l >= 10) { m->mm.ptr = cand; m->mm.len = l; }
        }
    }

    m->mm.tab[h] = (uint32_t)pos;
    m->mm.expected = (m->mm.len > 0 && m->mm.ptr < pos) ? m->hist[m->mm.ptr] : -1;
}

/* ---- prediction --------------------------------------------------------- */
HZ_INLINE int hz_cm_predict(hz_cm *m)
{
    int i, p, gate, mgate;
    int node = m->c0;               /* 1..255, the partial byte */
    int tn   = m->nibnode;          /* 1..15, node inside the current nibble */
    int ti   = tn - 1;              /* 0..14, index into slot->st */

    mgate = m->mm.expected >= 0;
    gate  = (node & 0xFF) | (mgate << 8);
    hz_mixer_begin(&m->mx, gate);

    /* order 0 and order 1, direct tables indexed by the partial byte */
    hz_mixer_add(&m->mx, hz_ctr_p16(m->o0[node]));
    hz_mixer_add(&m->mx, hz_ctr_p16(m->o1[(size_t)(m->c4 & 0xFF) * 256 + node]));

    /* Hashed contexts through the indirect state map.
     *
     * The last two are record-relative.  With no stride detected they would
     * degenerate into a weaker copy of order-1 and merely dilute the mixer,
     * so they are fed a neutral zero instead -- the mixer then learns to
     * ignore them at no cost, and text is unaffected. */
    for (i = 0; i < HZ_NHASH; ++i) {
        if (i >= HZ_NHASH - 3 && !m->rec_stride) {
            hz_mixer_add_st(&m->mx, 0);
        } else {
            uint8_t st = m->cstate[i][ti];
            hz_mixer_add(&m->mx, hz_ctr_p16(m->sm[i].p[st]));
        }
    }

    /* match model */
    if (mgate) {
        int eb = (m->mm.expected | 256) >> (7 - m->bitpos);
        if ((eb >> 1) == node) {
            int predbit = eb & 1;
            int lb = (int)(m->mm.len < 63 ? m->mm.len : 63);
            hz_ctr *c = &m->mm.cm[(size_t)lb * 256 + node];
            int st = hz_stretch(hz_ctr_p16(*c));
            /* The counter tracks "was the prediction right"; convert that
             * into a signed opinion about the actual bit. */
            hz_mixer_add_st(&m->mx, predbit ? st : -st);
        } else {
            m->mm.len = 0;
            m->mm.expected = -1;
            mgate = 0;
            hz_mixer_add_st(&m->mx, 0);
        }
    } else {
        hz_mixer_add_st(&m->mx, 0);
    }

    /* match length as an explicit confidence input, plus a constant bias */
    hz_mixer_add_st(&m->mx, mgate ? hz_clampi((int)m->mm.len * 24, 0, 1200) : 0);
    hz_mixer_add_st(&m->mx, 256);

    p = hz_mixer_predict(&m->mx);

    /* Two SSE stages, combined in the logit domain.
     *
     * Averaging probabilities directly would cap how confident the result
     * can be: one lagging stage at 0.999 drags a 0.99999 consensus down by
     * an order of magnitude in cost.  Averaging logits keeps the shared
     * confidence and costs the same handful of instructions. */
    {
        int p1 = hz_apm_pp(&m->apm1, p, node);
        int c2 = (((m->c4 & 0xF0) >> 4) | ((node & 0x0F) << 4)
                 | (mgate << 8)) & (256 * 16 - 1);
        int p2 = hz_apm_pp(&m->apm2, p, c2);
        int s  = (hz_stretch(p) + hz_stretch(p1) + 2 * hz_stretch(p2)) >> 2;
        p = hz_squash(s);
    }
    return hz_clampi(p, 1, 65535);
}

HZ_INLINE void hz_cm_update(hz_cm *m, int bit)
{
    int i;
    int node = m->c0;
    int ti   = m->nibnode - 1;

    hz_apm_update(&m->apm1, bit);
    hz_apm_update(&m->apm2, bit);
    hz_mixer_update(&m->mx, bit);

    hz_ctr_update(&m->o0[node], bit, 255);
    hz_ctr_update(&m->o1[(size_t)(m->c4 & 0xFF) * 256 + node], bit, 255);

    for (i = 0; i < HZ_NHASH; ++i) {
        uint8_t *st;
        if (i >= HZ_NHASH - 3 && !m->rec_stride) continue;
        st = &m->cstate[i][ti];
        hz_ctr_update(&m->sm[i].p[*st], bit, 255);
        *st = hz_state_next[*st][bit];
    }

    if (m->mm.expected >= 0) {
        int eb = (m->mm.expected | 256) >> (7 - m->bitpos);
        if ((eb >> 1) == node) {
            int lb = (int)(m->mm.len < 63 ? m->mm.len : 63);
            hz_ctr_update(&m->mm.cm[(size_t)lb * 256 + node],
                          ((eb & 1) == bit) ? 1 : 0, 255);
            if ((eb & 1) != bit) { m->mm.len = 0; m->mm.expected = -1; }
        }
    }

    /* advance the partial byte and the nibble tree */
    m->c0 = (m->c0 << 1) | bit;
    m->nibnode = (m->nibnode << 1) | bit;
    ++m->bitpos;

    /* at the halfway point, re-hash every context on the high nibble */
    if (m->bitpos == 4)
        hz_cm_refresh(m, 1, m->c0 & 0x0F);
}

/* Called after a full byte has been coded. */
HZ_INLINE void hz_cm_push_byte(hz_cm *m, int c)
{
    m->c4 = (m->c4 << 8) | (uint32_t)c;
    m->c8 = (m->c8 << 8) | (uint64_t)(unsigned)c;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_') {
        m->wordhash = (m->wordhash + (uint32_t)c + 1) * HZ_PRIME32_1;
    } else {
        m->wordhash = 0;
    }
    m->c0 = 1;
    m->bitpos = 0;
    ++m->hpos;
    if (m->rec_stride) {
        if (++m->rec_pos >= m->rec_stride) {
            m->rec_pos = 0;
            /* A record just completed: fold it into one value that the next
             * record can be predicted from.  Reading it back out of the
             * history costs one pass per record, not per byte. */
            if (m->hpos >= m->rec_stride) {
                /* Fold only the *high* portion of the record.
                 *
                 * In a little endian numeric record the low bytes are the
                 * least significant digits, and on measured data they are
                 * close to noise: hashing them splits one useful context
                 * into thousands of near-duplicates that each see too few
                 * observations to become confident.  Measured on a
                 * quantised sensor series, a context built from the top
                 * three bytes of the previous value predicts exactly as
                 * well as one built from all eight -- and it concentrates
                 * the statistics instead of scattering them. */
                const uint8_t *rp = m->hist + m->hpos - m->rec_stride;
                uint32_t keep = m->rec_stride > 4 ? m->rec_stride / 2 : m->rec_stride;
                uint64_t h = 0;
                uint32_t j;
                for (j = m->rec_stride - keep; j < m->rec_stride; ++j)
                    h = hz_mix64(h ^ ((uint64_t)rp[j] + 0x9E3779B97F4A7C15ull));
                m->rec_hash = h;
            }
        }
    }
    hz_cm_match_update(m);
    hz_cm_refresh(m, 0, 0);
}

/* =========================================================================
 * Public entry points
 * ========================================================================= */
size_t hz_cm_compress(uint8_t *dst, size_t dst_cap,
                      const uint8_t *src, size_t src_size, int level)
{
    hz_cm  m;
    hz_enc e;
    size_t i;
    int    b;

    if (dst_cap < 8) return 0;
    if (hz_cm_init_geom(&m, hz_cm_geometry(level, src_size), src) < 0) return 0;

    dst[0] = (uint8_t)m.memlog;          /* geometry for the decoder */
    /* The stride cannot be re-derived by the decoder -- it has no data yet --
     * so it travels in the header.  One byte, and it pays for itself many
     * times over on any tabular input. */
    m.rec_stride = hz_detect_stride(src, src_size);
    dst[1] = (uint8_t)m.rec_stride;
    hz_enc_init(&e, dst + 2, dst_cap - 2);

    hz_cm_match_update(&m);
    hz_cm_refresh(&m, 0, 0);

    for (i = 0; i < src_size; ++i) {
        int c = src[i];
        for (b = 7; b >= 0; --b) {
            int bit = (c >> b) & 1;
            int p   = hz_cm_predict(&m);
            hz_enc_bit(&e, p, bit);
            hz_cm_update(&m, bit);
        }
        hz_cm_push_byte(&m, c);
        if (HZ_UNLIKELY(e.overflow)) { hz_cm_free(&m); return 0; }
    }

    {
        size_t n = hz_enc_flush(&e);
        hz_cm_free(&m);
        if (e.overflow || n + 2 > dst_cap) return 0;
        return n + 2;
    }
}

int hz_cm_decompress(uint8_t *dst, size_t dst_size,
                     const uint8_t *src, size_t src_size)
{
    hz_cm  m;
    hz_dec d;
    size_t i;
    int    b;
    int    memlog;

    if (src_size < 2) return -1;
    memlog = src[0];
    if (memlog < 16 || memlog > 26) return -1;

    /* Reconstruct the geometry from the header alone.
     *
     * The decoder cannot re-derive it from the level, because the level is
     * not transmitted and the size cap above depends on the input length.
     * So the header's memlog is authoritative: it is passed in directly and
     * the level only selects the match table, which is derived from it. */
    if (hz_cm_init_geom(&m, memlog, dst) < 0) return -1;
    m.rec_stride = src[1];

    hz_dec_init(&d, src + 2, src_size - 2);

    hz_cm_match_update(&m);
    hz_cm_refresh(&m, 0, 0);

    for (i = 0; i < dst_size; ++i) {
        int c = 1;
        for (b = 0; b < 8; ++b) {
            int p   = hz_cm_predict(&m);
            int bit = hz_dec_bit(&d, p);
            hz_cm_update(&m, bit);
            c = (c << 1) | bit;
        }
        c &= 0xFF;
        dst[i] = (uint8_t)c;
        hz_cm_push_byte(&m, c);
    }

    hz_cm_free(&m);
    return 0;
}
