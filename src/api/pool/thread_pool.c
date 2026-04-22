#include "thread_pool.h"

#include <stdlib.h>
#include <string.h>

#include "../handler/request_handler.h"
#include "../log/log.h"

static void *worker_main(void *arg) {
    ThreadPool *pool = (ThreadPool *)arg;
    QueueTask task;

    log_write(LOG_INFO, 0, "worker started");
    while (task_queue_pop(pool->queue, &task)) {
        handle_connection(task.fd, task.trace_id);
    }
    log_write(LOG_INFO, 0, "worker stopped");
    return NULL;
}

int thread_pool_start(ThreadPool *pool, int thread_count, TaskQueue *queue) {
    int i;
    if (!pool || !queue || thread_count <= 0) return 0;
    memset(pool, 0, sizeof(*pool));
    pool->threads = (pthread_t *)calloc((size_t)thread_count, sizeof(pthread_t));
    if (!pool->threads) return 0;
    pool->thread_count = thread_count;
    pool->queue = queue;

    for (i = 0; i < thread_count; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_main, pool) != 0) {
            pool->thread_count = i;
            thread_pool_stop(pool);
            return 0;
        }
    }
    return 1;
}

void thread_pool_stop(ThreadPool *pool) {
    int i;
    if (!pool) return;
    if (pool->queue) task_queue_shutdown(pool->queue);
    for (i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    free(pool->threads);
    memset(pool, 0, sizeof(*pool));
}
