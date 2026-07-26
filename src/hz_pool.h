/* ===========================================================================
 * HYDRA: a minimal fork-join worker pool.
 *
 * Blocks are already independent -- each one carries its own filters, its
 * own model state and its own entropy stream -- so the only thing standing
 * between this codec and using every core is a way to run N jobs and wait.
 * That is all this is: no work stealing, no dynamic scheduling, no
 * lock-free cleverness.  Each worker claims the next index with one atomic
 * increment and runs it to completion.
 *
 * Output ordering is not the pool's problem: every job writes into its own
 * scratch buffer and the caller concatenates them in index order afterwards,
 * so the result is byte identical to the single threaded encoder.  That is
 * a property worth keeping -- it means the test suite covers both paths at
 * once, and a stream does not record how many cores produced it.
 *
 * When built without HZ_THREADS, or when only one job is queued, everything
 * below collapses into a direct call.
 * ========================================================================= */
#ifndef HZ_POOL_H
#define HZ_POOL_H

#include "hz_int.h"

typedef void (*hz_job_fn)(void *ctx, int index);

#if defined(HZ_THREADS)

#include <pthread.h>

typedef struct {
    hz_job_fn       fn;
    void           *ctx;
    int             njobs;
    int             next;
    pthread_mutex_t lock;
} hz_pool_state;

static void *hz_pool_worker(void *arg)
{
    hz_pool_state *st = (hz_pool_state *)arg;
    for (;;) {
        int idx;
        pthread_mutex_lock(&st->lock);
        idx = st->next < st->njobs ? st->next++ : -1;
        pthread_mutex_unlock(&st->lock);
        if (idx < 0) break;
        st->fn(st->ctx, idx);
    }
    return NULL;
}

/* Run `njobs` invocations of fn(ctx, i) across at most `nthreads` threads
 * and return once every one has finished. */
static void hz_pool_run(hz_job_fn fn, void *ctx, int njobs, int nthreads)
{
    hz_pool_state st;
    pthread_t     tid[HZ_MAX_THREADS];
    int i, spawned = 0;

    if (njobs <= 0) return;
    if (nthreads > njobs) nthreads = njobs;
    if (nthreads > HZ_MAX_THREADS) nthreads = HZ_MAX_THREADS;

    if (nthreads <= 1) {
        for (i = 0; i < njobs; ++i) fn(ctx, i);
        return;
    }

    st.fn = fn; st.ctx = ctx; st.njobs = njobs; st.next = 0;
    if (pthread_mutex_init(&st.lock, NULL) != 0) {
        for (i = 0; i < njobs; ++i) fn(ctx, i);
        return;
    }

    for (i = 0; i < nthreads; ++i)
        if (pthread_create(&tid[i], NULL, hz_pool_worker, &st) == 0)
            ++spawned;

    /* If some threads failed to start the remaining work still gets done:
     * this thread joins in as a worker rather than leaving jobs stranded. */
    hz_pool_worker(&st);

    for (i = 0; i < spawned; ++i) pthread_join(tid[i], NULL);
    pthread_mutex_destroy(&st.lock);
}

int hz_cpu_count(void);

#else /* !HZ_THREADS */

static void hz_pool_run(hz_job_fn fn, void *ctx, int njobs, int nthreads)
{
    int i;
    (void)nthreads;
    for (i = 0; i < njobs; ++i) fn(ctx, i);
}

HZ_INLINE int hz_cpu_count(void) { return 1; }

#endif

#endif /* HZ_POOL_H */
