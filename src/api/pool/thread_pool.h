#ifndef API_THREAD_POOL_H
#define API_THREAD_POOL_H

#include <pthread.h>

#include "task_queue.h"

typedef struct {
    pthread_t *threads;
    int thread_count;
    TaskQueue *queue;
} ThreadPool;

int thread_pool_start(ThreadPool *pool, int thread_count, TaskQueue *queue);
void thread_pool_stop(ThreadPool *pool);

#endif
