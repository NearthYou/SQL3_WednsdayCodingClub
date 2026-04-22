#ifndef RESP_H
#define RESP_H

#include <stddef.h>

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} Buf;

typedef struct HttpRes {
    int code;
    char *body;
    size_t len;
    const char *content_type;
} HttpRes;

void buf_init(Buf *buf);
void buf_free(Buf *buf);
int buf_chr(Buf *buf, char ch);
int buf_put(Buf *buf, const char *text);
int buf_fmt(Buf *buf, const char *fmt, ...);
int buf_jsn(Buf *buf, const char *text);
int res_set(HttpRes *res, int code, Buf *buf);
int res_set_ct(HttpRes *res, int code, Buf *buf, const char *content_type);
void res_free(HttpRes *res);

#endif
