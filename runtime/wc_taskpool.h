/* wc_taskpool.h — the worker pool Box2D and Box3D hand their tasks to.
 *
 * Both libraries expose the same contract: you give the world an
 * `enqueueTask` and a `finishTask`, and it forks its solver across them.
 * The callbacks are identical in shape between the two, so one pool serves
 * both.
 *
 * Threads are a build-time capability, not a runtime guess:
 *
 *   native (pthreads)  real worker threads.
 *   wasm + -pthread    real workers, but only on hosts that can give the
 *                      module SharedArrayBuffer.
 *   wasm, default      no workers. enqueue runs the task inline and returns
 *                      NULL, which both libraries explicitly define as "ran
 *                      it serially, don't call finish". Correct, just not
 *                      parallel — carts keep working on every host.
 *
 * The pool never blocks a worker on another task, so it cannot deadlock the
 * way a job system can when a step waits on its own sub-jobs: the thread
 * that called finish drains pending work itself while it waits.
 */
#ifndef WC_TASKPOOL_H
#define WC_TASKPOOL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Same prototype both physics libraries use for a task body. */
typedef void wc_task_fn(void *task_context);

/* Start the pool. `workers` <= 1, or a build without threads, means serial.
 * Safe to call repeatedly; the first call wins. Returns workers actually
 * started (0 = serial). */
int  wc_taskpool_init(int workers);
void wc_taskpool_shutdown(void);

/* How many workers are live (0 = serial). Carts can read this to size their
 * own work, and the tests assert on it. */
int  wc_taskpool_workers(void);

/* The two callbacks handed to b2WorldDef / b3WorldDef. `user_context` is
 * ignored; the pool is a process-wide singleton. */
void *wc_taskpool_enqueue(wc_task_fn *task, void *task_context, void *user_context);
void  wc_taskpool_finish(void *user_task, void *user_context);

/* Hardware thread count, clamped to something sane, for a default. */
int  wc_taskpool_hw_threads(void);

#ifdef __cplusplus
}
#endif

#endif /* WC_TASKPOOL_H */
