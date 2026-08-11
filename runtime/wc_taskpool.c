/* wc_taskpool.c — see wc_taskpool.h for the contract and why threads are a
 * build-time capability rather than a runtime guess.
 */
#include "wc_taskpool.h"

#include <stdlib.h>
#include <string.h>

/* Threads exist when we are not a plain single-threaded wasm build.
 * Emscripten only defines __EMSCRIPTEN_PTHREADS__ when built with -pthread,
 * which is also the only case where a host can actually give us workers. */
#if defined(__wasm__) && !defined(__EMSCRIPTEN_PTHREADS__)
  #define WC_POOL_SERIAL 1
#else
  #define WC_POOL_SERIAL 0
  #include <pthread.h>
  #include <unistd.h>
#endif

#define WC_POOL_MAX_WORKERS 16
#define WC_POOL_QUEUE       256

#if WC_POOL_SERIAL

int  wc_taskpool_init(int workers) { (void)workers; return 0; }
void wc_taskpool_shutdown(void) {}
int  wc_taskpool_workers(void) { return 0; }
int  wc_taskpool_hw_threads(void) { return 1; }

void *wc_taskpool_enqueue(wc_task_fn *task, void *task_context, void *user_context) {
    (void)user_context;
    task(task_context);      /* ran it here; NULL tells the caller not to wait */
    return NULL;
}
void wc_taskpool_finish(void *user_task, void *user_context) {
    (void)user_task; (void)user_context;
}

#else

typedef struct {
    wc_task_fn *fn;
    void       *ctx;
    int         done;
} pool_job;

static pool_job        g_jobs[WC_POOL_QUEUE];
static int             g_head, g_tail;          /* ring: [head,tail) pending */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_work = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_done = PTHREAD_COND_INITIALIZER;
static pthread_t       g_threads[WC_POOL_MAX_WORKERS];
static int             g_worker_count;
static int             g_running;
static int             g_started;

/* Take one pending job. Caller holds the lock. */
static pool_job *take_job_locked(void) {
    if (g_head == g_tail) return NULL;
    pool_job *j = &g_jobs[g_head % WC_POOL_QUEUE];
    g_head++;
    return j;
}

static void run_job(pool_job *j) {
    j->fn(j->ctx);
    pthread_mutex_lock(&g_lock);
    j->done = 1;
    pthread_cond_broadcast(&g_done);
    pthread_mutex_unlock(&g_lock);
}

static void *worker_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        while (g_running && g_head == g_tail)
            pthread_cond_wait(&g_work, &g_lock);
        if (!g_running) break;
        pool_job *j = take_job_locked();
        pthread_mutex_unlock(&g_lock);
        if (j) run_job(j);
        pthread_mutex_lock(&g_lock);
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int wc_taskpool_hw_threads(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > WC_POOL_MAX_WORKERS) n = WC_POOL_MAX_WORKERS;
    return (int)n;
}

int wc_taskpool_init(int workers) {
    if (g_started) return g_worker_count;
    if (workers <= 1) { g_started = 1; g_worker_count = 0; return 0; }
    if (workers > WC_POOL_MAX_WORKERS) workers = WC_POOL_MAX_WORKERS;

    g_running = 1;
    g_head = g_tail = 0;
    for (int i = 0; i < workers; i++) {
        if (pthread_create(&g_threads[i], NULL, worker_main, NULL) != 0) break;
        g_worker_count++;
    }
    g_started = 1;
    return g_worker_count;
}

void wc_taskpool_shutdown(void) {
    if (!g_started) return;
    pthread_mutex_lock(&g_lock);
    g_running = 0;
    pthread_cond_broadcast(&g_work);
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < g_worker_count; i++) pthread_join(g_threads[i], NULL);
    g_worker_count = 0;
    g_started = 0;
}

int wc_taskpool_workers(void) { return g_worker_count; }

void *wc_taskpool_enqueue(wc_task_fn *task, void *task_context, void *user_context) {
    (void)user_context;
    if (g_worker_count == 0) { task(task_context); return NULL; }

    pthread_mutex_lock(&g_lock);
    /* A full ring is not an error: run it here rather than drop physics work
     * on the floor or grow without bound mid-step. */
    if (g_tail - g_head >= WC_POOL_QUEUE) {
        pthread_mutex_unlock(&g_lock);
        task(task_context);
        return NULL;
    }
    pool_job *j = &g_jobs[g_tail % WC_POOL_QUEUE];
    j->fn = task; j->ctx = task_context; j->done = 0;
    g_tail++;
    pthread_cond_signal(&g_work);
    pthread_mutex_unlock(&g_lock);
    return j;
}

void wc_taskpool_finish(void *user_task, void *user_context) {
    (void)user_context;
    pool_job *j = (pool_job *)user_task;
    if (!j) return;

    for (;;) {
        pthread_mutex_lock(&g_lock);
        if (j->done) { pthread_mutex_unlock(&g_lock); return; }
        /* Drain other pending work on THIS thread rather than idling on a
         * condvar. A step that waits on its own sub-tasks would otherwise be
         * one worker short for the whole wait, and in the pathological case
         * (every worker blocked in finish) nothing would make progress. */
        pool_job *other = take_job_locked();
        if (other) {
            pthread_mutex_unlock(&g_lock);
            run_job(other);
            continue;
        }
        pthread_cond_wait(&g_done, &g_lock);
        pthread_mutex_unlock(&g_lock);
    }
}

#endif /* WC_POOL_SERIAL */
