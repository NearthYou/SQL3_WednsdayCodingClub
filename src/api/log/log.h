#ifndef API_LOG_H
#define API_LOG_H

#include <stdarg.h>

typedef enum {
    LOG_INFO = 0,
    LOG_WARN = 1,
    LOG_ERROR = 2
} LogLevel;

void log_init(void);
void log_destroy(void);
void log_set_level(LogLevel level);
LogLevel log_get_level(void);
void log_write(LogLevel level, unsigned long long trace_id, const char *fmt, ...);
void log_vwrite(LogLevel level, unsigned long long trace_id, const char *fmt, va_list args);

#endif
