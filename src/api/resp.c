#include "resp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int buf_grow(Buf *buf, size_t need) {
    char *nbuf;
    size_t cap;

    if (buf->len + need + 1 <= buf->cap) return 1;
    cap = buf->cap == 0 ? 256 : buf->cap;
    while (buf->len + need + 1 > cap) cap *= 2;
    nbuf = (char *)realloc(buf->buf, cap);
    if (!nbuf) return 0;
    buf->buf = nbuf;
    buf->cap = cap;
    return 1;
}

void buf_init(Buf *buf) {
    memset(buf, 0, sizeof(*buf));
}

void buf_free(Buf *buf) {
    if (!buf) return;
    free(buf->buf);
    memset(buf, 0, sizeof(*buf));
}

int buf_chr(Buf *buf, char ch) {
    if (!buf_grow(buf, 1)) return 0;
    buf->buf[buf->len++] = ch;
    buf->buf[buf->len] = '\0';
    return 1;
}

int buf_put(Buf *buf, const char *text) {
    size_t len;

    if (!text) text = "";
    len = strlen(text);
    if (!buf_grow(buf, len)) return 0;
    memcpy(buf->buf + buf->len, text, len);
    buf->len += len;
    buf->buf[buf->len] = '\0';
    return 1;
}

int buf_fmt(Buf *buf, const char *fmt, ...) {
    va_list ap;
    va_list cp;
    int need;

    va_start(ap, fmt);
    va_copy(cp, ap);
    need = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (need < 0 || !buf_grow(buf, (size_t)need)) {
        va_end(ap);
        return 0;
    }
    vsnprintf(buf->buf + buf->len, buf->cap - buf->len, fmt, ap);
    buf->len += (size_t)need;
    va_end(ap);
    return 1;
}

int buf_jsn(Buf *buf, const char *text) {
    const unsigned char *ptr = (const unsigned char *)(text ? text : "");

    if (!buf_chr(buf, '"')) return 0;
    while (*ptr) {
        if (*ptr == '"' || *ptr == '\\') {
            if (!buf_chr(buf, '\\') || !buf_chr(buf, (char)*ptr)) return 0;
        } else if (*ptr == '\n') {
            if (!buf_put(buf, "\\n")) return 0;
        } else if (*ptr == '\r') {
            if (!buf_put(buf, "\\r")) return 0;
        } else if (*ptr == '\t') {
            if (!buf_put(buf, "\\t")) return 0;
        } else if (*ptr < 0x20) {
            if (!buf_fmt(buf, "\\u%04x", (unsigned int)*ptr)) return 0;
        } else if (!buf_chr(buf, (char)*ptr)) {
            return 0;
        }
        ptr++;
    }
    return buf_chr(buf, '"');
}

int res_set(HttpRes *res, int code, Buf *buf) {
    if (!res || !buf) return 0;
    res->code = code;
    res->body = buf->buf;
    res->len = buf->len;
    buf->buf = NULL;
    buf->len = 0;
    buf->cap = 0;
    return 1;
}

void res_free(HttpRes *res) {
    if (!res) return;
    free(res->body);
    memset(res, 0, sizeof(*res));
}
