#ifndef POOL_H
#define POOL_H

typedef struct Pool Pool;
typedef void (*JobFn)(void *arg);

typedef struct {
    int size;
    int active;
    int queued;
    unsigned long done;
} PoolSt;

Pool *pool_new(int nthr, int qmax);
void pool_del(Pool *pool);
int pool_add(Pool *pool, JobFn fn, void *arg);
void pool_stop(Pool *pool);
void pool_stat(Pool *pool, PoolSt *out);
int pool_tid(void);

#endif
