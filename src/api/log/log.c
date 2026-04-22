#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_log_initialized = 0;

static const char *level_name(LogLevel level) {
    switch (level) {
        case LOG_INFO: return "INFO";
        case LOG_WARN: return "WARN";
        case LOG_ERROR: return "ERROR";
        default: return "INFO";
    }
}

void log_init(void) {
    if (!g_log_initialized) {
        pthread_mutex_init(&g_log_mutex, NULL);
        g_log_initialized = 1;
    }
}

void log_destroy(void) {
    if (g_log_initialized) {
        pthread_mutex_destroy(&g_log_mutex);
        g_log_initialized = 0;
    }
}

void log_vwrite(LogLevel level, unsigned long long trace_id, const char *fmt, va_list args) {
    time_t now;
    struct tm tm_now;
    char ts[32];
    unsigned long tid;

    if (!g_log_initialized) log_init();

    now = time(NULL);
#if defined(_POSIX_THREAD_SAFE_FUNCTIONS)
    localtime_r(&now, &tm_now);
#else
    tm_now = *localtime(&now);
#endif
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
    tid = (unsigned long)pthread_self();

    pthread_mutex_lock(&g_log_mutex);
    fprintf(stdout, "[%s] [%s] [%lu] [%llu] ", level_name(level), ts, tid, trace_id);
    vfprintf(stdout, fmt, args);
    fputc('\n', stdout);
    fflush(stdout);
    pthread_mutex_unlock(&g_log_mutex);
}

void log_write(LogLevel level, unsigned long long trace_id, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_vwrite(level, trace_id, fmt, args);
    va_end(args);
}
