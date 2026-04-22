#include "json_builder.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_INITIAL_CAPACITY (64 * 1024)
#define JSON_MAX_CAPACITY (4 * 1024 * 1024)

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} JsonBuf;

static const char *log_level_name(LogLevel level) {
    switch (level) {
        case LOG_INFO: return "INFO";
        case LOG_WARN: return "WARN";
        case LOG_ERROR: return "ERROR";
        default: return "INFO";
    }
}

static int jb_ensure(JsonBuf *jb, size_t extra) {
    size_t need;
    size_t next_cap;
    char *next;

    if (!jb) return 0;
    need = jb->len + extra + 1;
    if (need <= jb->cap) return 1;
    next_cap = jb->cap ? jb->cap : JSON_INITIAL_CAPACITY;
    while (next_cap < need) {
        if (next_cap >= JSON_MAX_CAPACITY / 2) {
            next_cap = JSON_MAX_CAPACITY;
            break;
        }
        next_cap *= 2;
    }
    if (next_cap < need || next_cap > JSON_MAX_CAPACITY) return 0;
    next = (char *)realloc(jb->buf, next_cap);
    if (!next) return 0;
    jb->buf = next;
    jb->cap = next_cap;
    return 1;
}

static int jb_append(JsonBuf *jb, const char *s) {
    size_t n;
    if (!jb || !s) return 0;
    n = strlen(s);
    if (!jb_ensure(jb, n)) return 0;
    memcpy(jb->buf + jb->len, s, n);
    jb->len += n;
    jb->buf[jb->len] = '\0';
    return 1;
}

static int jb_appendf(JsonBuf *jb, const char *fmt, ...) {
    va_list args;
    va_list copy;
    int need;
    if (!jb || !fmt) return 0;
    va_start(args, fmt);
    va_copy(copy, args);
    need = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (need < 0 || !jb_ensure(jb, (size_t)need)) {
        va_end(args);
        return 0;
    }
    vsnprintf(jb->buf + jb->len, jb->cap - jb->len, fmt, args);
    jb->len += (size_t)need;
    va_end(args);
    return 1;
}

static int jb_append_escaped(JsonBuf *jb, const char *s) {
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    if (!jb_append(jb, "\"")) return 0;
    while (*p) {
        if (*p == '"') {
            if (!jb_append(jb, "\\\"")) return 0;
        } else if (*p == '\\') {
            if (!jb_append(jb, "\\\\")) return 0;
        } else if (*p == '\n') {
            if (!jb_append(jb, "\\n")) return 0;
        } else if (*p == '\r') {
            if (!jb_append(jb, "\\r")) return 0;
        } else if (*p == '\t') {
            if (!jb_append(jb, "\\t")) return 0;
        } else {
            if (!jb_ensure(jb, 1)) return 0;
            jb->buf[jb->len++] = (char)*p;
            jb->buf[jb->len] = '\0';
        }
        p++;
    }
    return jb_append(jb, "\"");
}

int json_build_response(const DbResult *result, char **out_json, size_t *out_len) {
    JsonBuf jb;
    size_t i;
    size_t j;

    if (!result || !out_json) return 0;
    memset(&jb, 0, sizeof(jb));
    if (!jb_ensure(&jb, 128)) return 0;
    jb.buf[0] = '\0';

    if (!result->ok) {
        if (!jb_append(&jb, "{\"status\":\"error\",\"message\":")) goto fail;
        if (!jb_append_escaped(&jb, result->error_message ? result->error_message : "DB execution failed")) goto fail;
        if (!jb_append(&jb, "}")) goto fail;
    } else {
        if (!jb_append(&jb, "{\"status\":\"ok\",\"rows\":[")) goto fail;
        for (i = 0; i < result->row_count; i++) {
            const DbRow *row = &result->rows[i];
            if (i > 0 && !jb_append(&jb, ",")) goto fail;
            if (!jb_append(&jb, "{")) goto fail;
            for (j = 0; j < row->cell_count; j++) {
                if (j > 0 && !jb_append(&jb, ",")) goto fail;
                if (!jb_append_escaped(&jb, row->cells[j].name ? row->cells[j].name : "")) goto fail;
                if (!jb_append(&jb, ":")) goto fail;
                if (!jb_append_escaped(&jb, row->cells[j].value ? row->cells[j].value : "")) goto fail;
            }
            if (!jb_append(&jb, "}")) goto fail;
        }
        if (!jb_append(&jb, "]}")) goto fail;
    }

    *out_json = jb.buf;
    if (out_len) *out_len = jb.len;
    return 1;

fail:
    free(jb.buf);
    return 0;
}

char *json_build_error(const char *message, size_t *out_len) {
    DbResult result;
    char *json = NULL;
    memset(&result, 0, sizeof(result));
    result.ok = 0;
    result.error_message = (char *)(message ? message : "error");
    if (!json_build_response(&result, &json, out_len)) return NULL;
    return json;
}

int json_build_ok_response(char **out_json, size_t *out_len) {
    DbResult result;
    memset(&result, 0, sizeof(result));
    result.ok = 1;
    result.http_status = 200;
    return json_build_response(&result, out_json, out_len);
}

int json_rows_writer_init(JsonRowsWriter *writer) {
    if (!writer) return 0;
    memset(writer, 0, sizeof(*writer));
    if (!jb_ensure((JsonBuf *)writer, 128)) return 0;
    writer->buf[0] = '\0';
    if (!jb_append((JsonBuf *)writer, "{\"status\":\"ok\",\"rows\":[")) {
        free(writer->buf);
        memset(writer, 0, sizeof(*writer));
        return 0;
    }
    writer->started = 1;
    return 1;
}

int json_rows_writer_add_row(JsonRowsWriter *writer,
                             const char **col_names,
                             const char **values,
                             int col_count) {
    int i;
    JsonBuf *jb = (JsonBuf *)writer;

    if (!writer || !writer->started || writer->failed || col_count < 0) return 0;
    if (writer->row_count > 0 && !jb_append(jb, ",")) goto fail;
    if (!jb_append(jb, "{")) goto fail;
    for (i = 0; i < col_count; i++) {
        if (i > 0 && !jb_append(jb, ",")) goto fail;
        if (!jb_append_escaped(jb, col_names ? col_names[i] : "")) goto fail;
        if (!jb_append(jb, ":")) goto fail;
        if (!jb_append_escaped(jb, values ? values[i] : "")) goto fail;
    }
    if (!jb_append(jb, "}")) goto fail;
    writer->row_count++;
    return 1;

fail:
    writer->failed = 1;
    return 0;
}

int json_rows_writer_finish(JsonRowsWriter *writer, char **out_json, size_t *out_len) {
    JsonBuf *jb = (JsonBuf *)writer;

    if (!writer || !out_json || !writer->started || writer->failed) return 0;
    if (!jb_append(jb, "]}")) {
        writer->failed = 1;
        return 0;
    }
    writer->started = 0;
    *out_json = writer->buf;
    if (out_len) *out_len = writer->len;
    writer->buf = NULL;
    writer->len = 0;
    writer->cap = 0;
    return 1;
}

void json_rows_writer_destroy(JsonRowsWriter *writer) {
    if (!writer) return;
    free(writer->buf);
    memset(writer, 0, sizeof(*writer));
}

int json_build_stats_response(const ApiStatsSnapshot *snapshot, char **out_json, size_t *out_len) {
    JsonBuf jb;

    if (!snapshot || !out_json) return 0;
    memset(&jb, 0, sizeof(jb));
    if (!jb_ensure(&jb, 256)) return 0;
    jb.buf[0] = '\0';

    if (!jb_append(&jb, "{\"status\":\"ok\"")) goto fail;
    if (!jb_appendf(&jb, ",\"uptime_ms\":%llu", snapshot->uptime_ms)) goto fail;
    if (!jb_appendf(&jb, ",\"total_requests\":%llu", snapshot->total_requests)) goto fail;
    if (!jb_appendf(&jb, ",\"inflight_requests\":%llu", snapshot->inflight_requests)) goto fail;
    if (!jb_appendf(&jb, ",\"ok_responses\":%llu", snapshot->ok_responses)) goto fail;
    if (!jb_appendf(&jb, ",\"error_responses\":%llu", snapshot->error_responses)) goto fail;
    if (!jb_appendf(&jb, ",\"status_503\":%llu", snapshot->status_503)) goto fail;
    if (!jb_appendf(&jb, ",\"queue_depth\":%llu", snapshot->queue_depth)) goto fail;
    if (!jb_appendf(&jb, ",\"queue_capacity\":%llu", snapshot->queue_capacity)) goto fail;
    if (!jb_appendf(&jb, ",\"configured_workers\":%llu", snapshot->configured_workers)) goto fail;
    if (!jb_appendf(&jb, ",\"keep_alive_reuse\":%llu", snapshot->keep_alive_reuse)) goto fail;
    if (!jb_appendf(&jb, ",\"cache_hits\":%llu", snapshot->cache_hits)) goto fail;
    if (!jb_appendf(&jb, ",\"cache_misses\":%llu", snapshot->cache_misses)) goto fail;
    if (!jb_append(&jb, ",\"log_level\":")) goto fail;
    if (!jb_append_escaped(&jb, log_level_name(snapshot->current_log_level))) goto fail;
    if (!jb_appendf(&jb, ",\"parse_ms\":%.3f", (double)snapshot->parse_ns / 1000000.0)) goto fail;
    if (!jb_appendf(&jb, ",\"lock_wait_ms\":%.3f", (double)snapshot->lock_wait_ns / 1000000.0)) goto fail;
    if (!jb_appendf(&jb, ",\"engine_ms\":%.3f", (double)snapshot->engine_ns / 1000000.0)) goto fail;
    if (!jb_appendf(&jb, ",\"json_ms\":%.3f", (double)snapshot->json_ns / 1000000.0)) goto fail;
    if (!jb_appendf(&jb, ",\"send_ms\":%.3f", (double)snapshot->send_ns / 1000000.0)) goto fail;
    if (!jb_append(&jb, "}")) goto fail;

    *out_json = jb.buf;
    if (out_len) *out_len = jb.len;
    return 1;

fail:
    free(jb.buf);
    return 0;
}
