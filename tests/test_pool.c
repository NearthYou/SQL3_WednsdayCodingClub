#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "thr/pool.h"
#include "tutil.h"

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int go;
    int hits;
    int run;
    int max;
    int tids[16];
    int tid_n;
} State;

static void add_tid(State *st, int tid) {
    int i;

    for (i = 0; i < st->tid_n; i++) {
        if (st->tids[i] == tid) return;
    }
    st->tids[st->tid_n++] = tid;
}

static void hold_job(void *arg) {
    State *st = (State *)arg;

    pthread_mutex_lock(&st->mu);
    add_tid(st, pool_tid());
    st->run++;
    if (st->run > st->max) st->max = st->run;
    pthread_cond_broadcast(&st->cv);
    while (!st->go) pthread_cond_wait(&st->cv, &st->mu);
    st->run--;
    st->hits++;
    pthread_cond_broadcast(&st->cv);
    pthread_mutex_unlock(&st->mu);
}

static void wait_run(State *st, int want) {
    pthread_mutex_lock(&st->mu);
    while (st->max < want) pthread_cond_wait(&st->cv, &st->mu);
    pthread_mutex_unlock(&st->mu);
}

static void wait_hit(State *st, int want) {
    pthread_mutex_lock(&st->mu);
    while (st->hits < want) pthread_cond_wait(&st->cv, &st->mu);
    pthread_mutex_unlock(&st->mu);
}

static void let_go(State *st) {
    pthread_mutex_lock(&st->mu);
    st->go = 1;
    pthread_cond_broadcast(&st->cv);
    pthread_mutex_unlock(&st->mu);
}

static void init_st(State *st) {
    memset(st, 0, sizeof(*st));
    pthread_mutex_init(&st->mu, NULL);
    pthread_cond_init(&st->cv, NULL);
}

static void free_st(State *st) {
    pthread_cond_destroy(&st->cv);
    pthread_mutex_destroy(&st->mu);
}

int main(void) {
    Pool *pool;
    PoolSt pst;
    State st;
    int rc;

    init_st(&st);
    pool = pool_new(3, 8);
    T_OK(pool != NULL);
    T_OK(pool_add(pool, hold_job, &st) == 0);
    T_OK(pool_add(pool, hold_job, &st) == 0);
    T_OK(pool_add(pool, hold_job, &st) == 0);
    wait_run(&st, 2);
    let_go(&st);
    wait_hit(&st, 3);
    pool_stat(pool, &pst);
    T_OK(pst.done == 3);
    T_OK(st.max >= 2);
    T_OK(st.tid_n >= 2);
    pool_del(pool);
    free_st(&st);

    init_st(&st);
    pool = pool_new(1, 1);
    T_OK(pool != NULL);
    T_OK(pool_add(pool, hold_job, &st) == 0);
    wait_run(&st, 1);
    T_OK(pool_add(pool, hold_job, &st) == 0);
    rc = pool_add(pool, hold_job, &st);
    T_OK(rc == -1);
    usleep(20000);
    pool_stat(pool, &pst);
    T_OK(pst.queued == 1);
    pool_stop(pool);
    let_go(&st);
    wait_hit(&st, 2);
    pool_del(pool);
    free_st(&st);

    printf("test_pool: ok\n");
    return 0;
}
