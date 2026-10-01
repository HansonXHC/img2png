#ifndef IMG2PNG_THREADS_H
#define IMG2PNG_THREADS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*task_fn_t)(void *arg);

typedef struct thread_pool thread_pool_t;

/* Fixed-size pool of Win32 worker threads. */
thread_pool_t *pool_create(int nthreads);
void           pool_destroy(thread_pool_t *pool);

/* Submit fn(arg) as an independent task. */
void           pool_submit(thread_pool_t *pool, task_fn_t fn, void *arg);

/* Block until every submitted task has finished. */
void           pool_wait(thread_pool_t *pool);


#ifdef __cplusplus
}
#endif

#endif
