#ifndef API_TASK_QUEUE_H
#define API_TASK_QUEUE_H

#include <pthread.h>

typedef struct {
    int fd;
    unsigned long long trace_id;
} QueueTask;

typedef struct {
    QueueTask *items;
    int capacity;
    int head;
    int tail;
    int size;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} TaskQueue;

int task_queue_init(TaskQueue *queue, int capacity);
void task_queue_destroy(TaskQueue *queue);
int task_queue_push(TaskQueue *queue, QueueTask task); /* 1=ok,0=full/shutdown */
int task_queue_pop(TaskQueue *queue, QueueTask *task); /* 1=ok,0=shutdown+empty */
void task_queue_shutdown(TaskQueue *queue);

#endif
