#include "task_queue.h"

#include <stdlib.h>
#include <string.h>

int task_queue_init(TaskQueue *queue, int capacity) {
    if (!queue || capacity <= 0) return 0;
    memset(queue, 0, sizeof(*queue));
    queue->items = (QueueTask *)calloc((size_t)capacity, sizeof(QueueTask));
    if (!queue->items) return 0;
    queue->capacity = capacity;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);
    return 1;
}

void task_queue_destroy(TaskQueue *queue) {
    if (!queue) return;
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond);
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
}

int task_queue_push(TaskQueue *queue, QueueTask task) {
    int ok = 0;
    if (!queue) return 0;
    pthread_mutex_lock(&queue->mutex);
    if (!queue->shutting_down && queue->size < queue->capacity) {
        queue->items[queue->tail] = task;
        queue->tail = (queue->tail + 1) % queue->capacity;
        queue->size++;
        ok = 1;
        pthread_cond_signal(&queue->cond);
    }
    pthread_mutex_unlock(&queue->mutex);
    return ok;
}

int task_queue_pop(TaskQueue *queue, QueueTask *task) {
    if (!queue || !task) return 0;
    pthread_mutex_lock(&queue->mutex);
    while (!queue->shutting_down && queue->size == 0) {
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }
    if (queue->size == 0 && queue->shutting_down) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }
    *task = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->size--;
    pthread_mutex_unlock(&queue->mutex);
    return 1;
}

void task_queue_shutdown(TaskQueue *queue) {
    if (!queue) return;
    pthread_mutex_lock(&queue->mutex);
    queue->shutting_down = 1;
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
}

int task_queue_size(TaskQueue *queue) {
    int size;
    if (!queue) return 0;
    pthread_mutex_lock(&queue->mutex);
    size = queue->size;
    pthread_mutex_unlock(&queue->mutex);
    return size;
}

int task_queue_capacity(TaskQueue *queue) {
    int capacity;
    if (!queue) return 0;
    pthread_mutex_lock(&queue->mutex);
    capacity = queue->capacity;
    pthread_mutex_unlock(&queue->mutex);
    return capacity;
}
