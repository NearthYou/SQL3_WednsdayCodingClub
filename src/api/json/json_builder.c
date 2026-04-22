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
        if (!jb_append(&jb, "{\n  \"status\": \"error\",\n  \"message\": ")) goto fail;
        if (!jb_append_escaped(&jb, result->error_message ? result->error_message : "DB execution failed")) goto fail;
        if (!jb_append(&jb, "\n}")) goto fail;
    } else {
        if (!jb_append(&jb, "{\n  \"status\": \"ok\",\n  \"rows\": [")) goto fail;
        for (i = 0; i < result->row_count; i++) {
            const DbRow *row = &result->rows[i];
            if (i > 0 && !jb_append(&jb, ",")) goto fail;
            if (!jb_append(&jb, "\n    {")) goto fail;
            for (j = 0; j < row->cell_count; j++) {
                if (j > 0 && !jb_append(&jb, ",")) goto fail;
                if (!jb_append(&jb, "\n      ")) goto fail;
                if (!jb_append_escaped(&jb, row->cells[j].name ? row->cells[j].name : "")) goto fail;
                if (!jb_append(&jb, ": ")) goto fail;
                if (!jb_append_escaped(&jb, row->cells[j].value ? row->cells[j].value : "")) goto fail;
            }
            if (row->cell_count > 0 && !jb_append(&jb, "\n    ")) goto fail;
            if (!jb_append(&jb, "}")) goto fail;
        }
        if (result->row_count > 0 && !jb_append(&jb, "\n  ")) goto fail;
        if (!jb_append(&jb, "]\n}")) goto fail;
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
