#include "pool.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct Job {
    JobFn fn;
    void *arg;
    struct Job *next;
} Job;

struct Pool {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t *thr;
    int nthr;
    int qmax;
    int stop;
    int queued;
    int active;
    unsigned long done;
    Job *head;
    Job *tail;
};

static pthread_key_t g_tid;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void make_key(void) {
    pthread_key_create(&g_tid, NULL);
}

static void *wrk_run(void *arg) {
    Pool *pool = (Pool *)arg;
    int tid = -1;

    pthread_once(&g_once, make_key);
    pthread_mutex_lock(&pool->mu);
    for (tid = 0; tid < pool->nthr; tid++) {
        if (pthread_equal(pool->thr[tid], pthread_self())) break;
    }
    pthread_setspecific(g_tid, (void *)(long)(tid + 1));
    while (1) {
        Job *job;

        while (!pool->stop && !pool->head) pthread_cond_wait(&pool->cv, &pool->mu);
        if (pool->stop && !pool->head) break;
        job = pool->head;
        pool->head = job->next;
        if (!pool->head) pool->tail = NULL;
        pool->queued--;
        pool->active++;
        pthread_mutex_unlock(&pool->mu);
        job->fn(job->arg);
        free(job);
        pthread_mutex_lock(&pool->mu);
        pool->active--;
        pool->done++;
        pthread_cond_broadcast(&pool->cv);
    }
    pthread_mutex_unlock(&pool->mu);
    return NULL;
}

Pool *pool_new(int nthr, int qmax) {
    Pool *pool;
    int i;

    if (nthr <= 0 || qmax <= 0) return NULL;
    pool = (Pool *)calloc(1, sizeof(Pool));
    if (!pool) return NULL;
    pthread_mutex_init(&pool->mu, NULL);
    pthread_cond_init(&pool->cv, NULL);
    pool->thr = (pthread_t *)calloc((size_t)nthr, sizeof(pthread_t));
    if (!pool->thr) {
        pool_del(pool);
        return NULL;
    }
    pool->nthr = nthr;
    pool->qmax = qmax;
    for (i = 0; i < nthr; i++) {
        if (pthread_create(&pool->thr[i], NULL, wrk_run, pool) != 0) {
            pool->nthr = i;
            pool_stop(pool);
            pool_del(pool);
            return NULL;
        }
    }
    return pool;
}

void pool_stop(Pool *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->mu);
    pool->stop = 1;
    pthread_cond_broadcast(&pool->cv);
    pthread_mutex_unlock(&pool->mu);
}

void pool_del(Pool *pool) {
    int i;
    Job *job;

    if (!pool) return;
    pool_stop(pool);
    for (i = 0; i < pool->nthr; i++) {
        if (pool->thr && pool->thr[i]) pthread_join(pool->thr[i], NULL);
    }
    while (pool->head) {
        job = pool->head;
        pool->head = job->next;
        free(job);
    }
    free(pool->thr);
    pthread_cond_destroy(&pool->cv);
    pthread_mutex_destroy(&pool->mu);
    free(pool);
}

int pool_add(Pool *pool, JobFn fn, void *arg) {
    Job *job;

    if (!pool || !fn) return -2;
    job = (Job *)calloc(1, sizeof(Job));
    if (!job) return -3;
    job->fn = fn;
    job->arg = arg;
    pthread_mutex_lock(&pool->mu);
    if (pool->stop) {
        pthread_mutex_unlock(&pool->mu);
        free(job);
        return -2;
    }
    if (pool->queued >= pool->qmax) {
        pthread_mutex_unlock(&pool->mu);
        free(job);
        return -1;
    }
    if (pool->tail) pool->tail->next = job;
    else pool->head = job;
    pool->tail = job;
    pool->queued++;
    pthread_cond_signal(&pool->cv);
    pthread_mutex_unlock(&pool->mu);
    return 0;
}

void pool_stat(Pool *pool, PoolSt *out) {
    if (!pool || !out) return;
    pthread_mutex_lock(&pool->mu);
    out->size = pool->nthr;
    out->active = pool->active;
    out->queued = pool->queued;
    out->done = pool->done;
    pthread_mutex_unlock(&pool->mu);
}

int pool_tid(void) {
    pthread_once(&g_once, make_key);
    return (int)(long)pthread_getspecific(g_tid);
}
