#include "threads.h"
#include <stdlib.h>

#if defined(_WIN32) && !defined(IMG2PNG_FORCE_POSIX)

#include <windows.h>

typedef struct task_node {
    task_fn_t fn;
    void *arg;
    struct task_node *next;
} task_node_t;

struct thread_pool {
    HANDLE *threads;
    int nthreads;

    CRITICAL_SECTION lock;
    CONDITION_VARIABLE work_avail;
    CONDITION_VARIABLE idle;

    task_node_t *head, *tail;
    int queued;
    int running;
    int shutdown;
};

static task_node_t *take_task(thread_pool_t *p)
{
    task_node_t *t = p->head;
    if (t) {
        p->head = t->next;
        if (!p->head)
            p->tail = NULL;
    }
    return t;
}

static DWORD WINAPI worker(LPVOID arg)
{
    thread_pool_t *p = (thread_pool_t *)arg;
    for (;;) {
        EnterCriticalSection(&p->lock);
        while (!p->head && !p->shutdown)
            SleepConditionVariableCS(&p->work_avail, &p->lock, INFINITE);
        if (p->shutdown && !p->head) {
            LeaveCriticalSection(&p->lock);
            return 0;
        }
        task_node_t *t = take_task(p);
        p->queued--;
        p->running++;
        LeaveCriticalSection(&p->lock);

        t->fn(t->arg);
        free(t);

        EnterCriticalSection(&p->lock);
        p->running--;
        if (p->queued == 0 && p->running == 0)
            WakeAllConditionVariable(&p->idle);
        LeaveCriticalSection(&p->lock);
    }
}

thread_pool_t *pool_create(int nthreads)
{
    if (nthreads < 1)
        nthreads = 1;
    thread_pool_t *p = (thread_pool_t *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->nthreads = nthreads;
    InitializeCriticalSection(&p->lock);
    InitializeConditionVariable(&p->work_avail);
    InitializeConditionVariable(&p->idle);
    p->threads = (HANDLE *)calloc((size_t)nthreads, sizeof(HANDLE));
    if (!p->threads) {
        DeleteCriticalSection(&p->lock);
        free(p);
        return NULL;
    }
    for (int i = 0; i < nthreads; i++) {
        p->threads[i] = CreateThread(NULL, 0, worker, p, 0, NULL);
        if (!p->threads[i]) {
            p->shutdown = 1;
            WakeAllConditionVariable(&p->work_avail);
            for (int j = 0; j < i; j++) {
                WaitForSingleObject(p->threads[j], INFINITE);
                CloseHandle(p->threads[j]);
            }
            free(p->threads);
            DeleteCriticalSection(&p->lock);
            free(p);
            return NULL;
        }
    }
    return p;
}

void pool_submit(thread_pool_t *p, task_fn_t fn, void *arg)
{
    task_node_t *t = (task_node_t *)malloc(sizeof(*t));
    if (!t)
        return;     /* allocation failure: caller's wait() would hang, so
                       the CLI pre-allocates one node per file instead */
    t->fn = fn;
    t->arg = arg;
    t->next = NULL;
    EnterCriticalSection(&p->lock);
    if (p->tail)
        p->tail->next = t;
    else
        p->head = t;
    p->tail = t;
    p->queued++;
    LeaveCriticalSection(&p->lock);
    WakeConditionVariable(&p->work_avail);
}

void pool_wait(thread_pool_t *p)
{
    EnterCriticalSection(&p->lock);
    while (p->queued > 0 || p->running > 0)
        SleepConditionVariableCS(&p->idle, &p->lock, INFINITE);
    LeaveCriticalSection(&p->lock);
}

void pool_destroy(thread_pool_t *p)
{
    if (!p)
        return;
    EnterCriticalSection(&p->lock);
    p->shutdown = 1;
    LeaveCriticalSection(&p->lock);
    WakeAllConditionVariable(&p->work_avail);
    for (int i = 0; i < p->nthreads; i++) {
        WaitForSingleObject(p->threads[i], INFINITE);
        CloseHandle(p->threads[i]);
    }
    free(p->threads);
    DeleteCriticalSection(&p->lock);
    free(p);
}

#else /* POSIX (Linux / macOS): pthreads */

#include <pthread.h>

typedef struct task_node {
    task_fn_t fn;
    void *arg;
    struct task_node *next;
} task_node_t;

struct thread_pool {
    pthread_t *threads;
    int nthreads;

    pthread_mutex_t lock;
    pthread_cond_t work_avail;
    pthread_cond_t idle;

    task_node_t *head, *tail;
    int queued;
    int running;
    int shutdown;
};

static task_node_t *take_task(thread_pool_t *p)
{
    task_node_t *t = p->head;
    if (t) {
        p->head = t->next;
        if (!p->head)
            p->tail = NULL;
    }
    return t;
}

static void *worker(void *arg)
{
    thread_pool_t *p = (thread_pool_t *)arg;
    for (;;) {
        pthread_mutex_lock(&p->lock);
        while (!p->head && !p->shutdown)
            pthread_cond_wait(&p->work_avail, &p->lock);
        if (p->shutdown && !p->head) {
            pthread_mutex_unlock(&p->lock);
            return NULL;
        }
        task_node_t *t = take_task(p);
        p->queued--;
        p->running++;
        pthread_mutex_unlock(&p->lock);

        t->fn(t->arg);
        free(t);

        pthread_mutex_lock(&p->lock);
        p->running--;
        if (p->queued == 0 && p->running == 0)
            pthread_cond_broadcast(&p->idle);
        pthread_mutex_unlock(&p->lock);
    }
}

thread_pool_t *pool_create(int nthreads)
{
    if (nthreads < 1)
        nthreads = 1;
    thread_pool_t *p = (thread_pool_t *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->nthreads = nthreads;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->work_avail, NULL);
    pthread_cond_init(&p->idle, NULL);
    p->threads = (pthread_t *)calloc((size_t)nthreads, sizeof(pthread_t));
    if (!p->threads) {
        pthread_mutex_destroy(&p->lock);
        pthread_cond_destroy(&p->work_avail);
        pthread_cond_destroy(&p->idle);
        free(p);
        return NULL;
    }
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&p->threads[i], NULL, worker, p) != 0) {
            pthread_mutex_lock(&p->lock);
            p->shutdown = 1;
            pthread_mutex_unlock(&p->lock);
            pthread_cond_broadcast(&p->work_avail);
            for (int j = 0; j < i; j++)
                pthread_join(p->threads[j], NULL);
            free(p->threads);
            pthread_mutex_destroy(&p->lock);
            pthread_cond_destroy(&p->work_avail);
            pthread_cond_destroy(&p->idle);
            free(p);
            return NULL;
        }
    }
    return p;
}

void pool_submit(thread_pool_t *p, task_fn_t fn, void *arg)
{
    task_node_t *t = (task_node_t *)malloc(sizeof(*t));
    if (!t)
        return;
    t->fn = fn;
    t->arg = arg;
    t->next = NULL;
    pthread_mutex_lock(&p->lock);
    if (p->tail)
        p->tail->next = t;
    else
        p->head = t;
    p->tail = t;
    p->queued++;
    pthread_mutex_unlock(&p->lock);
    pthread_cond_signal(&p->work_avail);
}

void pool_wait(thread_pool_t *p)
{
    pthread_mutex_lock(&p->lock);
    while (p->queued > 0 || p->running > 0)
        pthread_cond_wait(&p->idle, &p->lock);
    pthread_mutex_unlock(&p->lock);
}

void pool_destroy(thread_pool_t *p)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->lock);
    p->shutdown = 1;
    pthread_mutex_unlock(&p->lock);
    pthread_cond_broadcast(&p->work_avail);
    for (int i = 0; i < p->nthreads; i++)
        pthread_join(p->threads[i], NULL);
    free(p->threads);
    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->work_avail);
    pthread_cond_destroy(&p->idle);
    free(p);
}

#endif
