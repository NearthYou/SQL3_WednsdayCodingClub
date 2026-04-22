#include <assert.h>
#include <stdio.h>

#include "../../src/api/pool/task_queue.h"

int main(void) {
    TaskQueue q;
    QueueTask t;

    assert(task_queue_init(&q, 2) == 1);
    assert(task_queue_push(&q, (QueueTask){.fd = 1, .trace_id = 10}) == 1);
    assert(task_queue_push(&q, (QueueTask){.fd = 2, .trace_id = 11}) == 1);
    assert(task_queue_push(&q, (QueueTask){.fd = 3, .trace_id = 12}) == 0);

    assert(task_queue_pop(&q, &t) == 1);
    assert(t.fd == 1);
    assert(task_queue_pop(&q, &t) == 1);
    assert(t.fd == 2);

    task_queue_shutdown(&q);
    assert(task_queue_pop(&q, &t) == 0);
    task_queue_destroy(&q);

    printf("test_task_queue: OK\n");
    return 0;
}
