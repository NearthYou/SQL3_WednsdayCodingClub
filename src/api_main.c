#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "api/db/db_wrapper.h"
#include "api/handler/request_handler.h"
#include "api/log/log.h"
#include "api/net/listener.h"
#include "api/pool/task_queue.h"
#include "api/pool/thread_pool.h"
#include "executor.h"

#define API_PORT_DEFAULT 8080
#define API_BACKLOG 128
#define API_QUEUE_CAPACITY 256
#define API_WORKER_COUNT 16

static volatile sig_atomic_t g_shutdown_requested = 0;
static unsigned long long g_trace_seq = 0;

static unsigned long long next_trace_id(void) {
    return __sync_add_and_fetch(&g_trace_seq, 1ULL);
}

static void on_signal(int signo) {
    (void)signo;
    g_shutdown_requested = 1;
}

static int choose_worker_count(void) {
    return API_WORKER_COUNT;
}

static int choose_port(void) {
    const char *env = getenv("PORT");
    char *endptr = NULL;
    long port;

    if (!env || env[0] == '\0') return API_PORT_DEFAULT;
    port = strtol(env, &endptr, 10);
    if (endptr == env || *endptr != '\0' || port <= 0 || port > 65535) {
        return API_PORT_DEFAULT;
    }
    return (int)port;
}

int main(void) {
    int listener_fd = -1;
    TaskQueue queue;
    ThreadPool pool;
    struct sigaction sa;
    int workers;
    int port;

    memset(&queue, 0, sizeof(queue));
    memset(&pool, 0, sizeof(pool));

    log_init();
    if (!db_wrapper_init()) {
        fprintf(stderr, "failed to initialize db wrapper\n");
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (!task_queue_init(&queue, API_QUEUE_CAPACITY)) {
        fprintf(stderr, "failed to initialize queue\n");
        db_wrapper_destroy();
        return 1;
    }

    workers = choose_worker_count();
    port = choose_port();
    if (!thread_pool_start(&pool, workers, &queue)) {
        fprintf(stderr, "failed to start thread pool\n");
        task_queue_destroy(&queue);
        db_wrapper_destroy();
        return 1;
    }

    listener_fd = create_listener_socket(port, API_BACKLOG);
    if (listener_fd < 0) {
        fprintf(stderr, "failed to create listener socket\n");
        thread_pool_stop(&pool);
        task_queue_destroy(&queue);
        db_wrapper_destroy();
        return 1;
    }

    log_write(LOG_INFO, 0, "server started on port %d (workers=%d)", port, workers);

    while (!g_shutdown_requested) {
        int client_fd;
        QueueTask task;
        struct sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);

        client_fd = accept(listener_fd, (struct sockaddr *)&addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR && g_shutdown_requested) break;
            if (errno == EINTR) continue;
            log_write(LOG_WARN, 0, "accept failed: errno=%d", errno);
            continue;
        }

        task.fd = client_fd;
        task.trace_id = next_trace_id();

        if (g_shutdown_requested || !task_queue_push(&queue, task)) {
            send_error_json_and_close(client_fd, 503, "service unavailable");
        }
    }

    if (listener_fd >= 0) close(listener_fd);
    task_queue_shutdown(&queue);
    thread_pool_stop(&pool);
    close_all_tables();
    task_queue_destroy(&queue);
    db_wrapper_destroy();
    log_write(LOG_INFO, 0, "server stopped");
    log_destroy();
    return 0;
}
