#include "http_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "../../types.h"

#define MAX_HEADER_BYTES 8192

static char *dup_msg(const char *msg) {
    size_t n = strlen(msg) + 1;
    char *out = (char *)malloc(n);
    if (!out) return NULL;
    memcpy(out, msg, n);
    return out;
}

static int starts_with_ci(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
        s++;
        prefix++;
    }
    return 1;
}

static int is_text_plain(const char *content_type) {
    if (!content_type || content_type[0] == '\0') return 0;
    if (!starts_with_ci(content_type, "text/plain")) return 0;
    if (content_type[10] == '\0') return 1;
    return content_type[10] == ';';
}

static void trim(char *s) {
    char *start = s;
    char *end;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
}

static int set_error(int *status_code, char **error_message, int status, const char *msg) {
    if (status_code) *status_code = status;
    if (error_message) *error_message = dup_msg(msg);
    return 0;
}

static char *find_header_end(const char *buffer, size_t used) {
    size_t i;
    if (!buffer || used < 4) return NULL;
    for (i = 0; i + 3 < used; i++) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n' &&
            buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
            return (char *)(buffer + i);
        }
    }
    return NULL;
}

static int ensure_buffered(HttpConnection *connection,
                           size_t target_bytes,
                           int *status_code,
                           char **error_message,
                           const char *eof_message) {
    while (connection->used < target_bytes) {
        ssize_t nread;
        if (connection->used >= sizeof(connection->buffer)) {
            return set_error(status_code, error_message, 400, "request exceeds connection buffer");
        }
        nread = recv(connection->fd,
                     connection->buffer + connection->used,
                     sizeof(connection->buffer) - connection->used,
                     0);
        if (nread == 0) {
            if (connection->used == 0) {
                if (status_code) *status_code = 0;
                if (error_message) *error_message = NULL;
                return 0;
            }
            return set_error(status_code, error_message, 400, eof_message);
        }
        if (nread < 0) return set_error(status_code, error_message, 400, "failed to read request");
        connection->used += (size_t)nread;
    }
    return 1;
}

void http_connection_init(HttpConnection *connection, int fd) {
    if (!connection) return;
    memset(connection, 0, sizeof(*connection));
    connection->fd = fd;
}

void free_http_request(ParsedHttpRequest *request) {
    if (!request) return;
    free(request->body);
    memset(request, 0, sizeof(*request));
}

int parse_http_request(HttpConnection *connection,
                       ParsedHttpRequest *request,
                       int *status_code,
                       char **error_message) {
    char header_copy[MAX_HEADER_BYTES + 1];
    char *header_end;
    size_t header_len;
    size_t request_bytes;
    char *line;
    char *saveptr = NULL;

    if (!connection || !request) return set_error(status_code, error_message, 500, "internal parser error");
    memset(request, 0, sizeof(*request));
    if (status_code) *status_code = 0;
    if (error_message) *error_message = NULL;

    while ((header_end = find_header_end(connection->buffer, connection->used)) == NULL) {
        if (connection->used >= MAX_HEADER_BYTES) {
            return set_error(status_code, error_message, 400, "request header too large");
        }
        if (!ensure_buffered(connection,
                             connection->used + 1,
                             status_code,
                             error_message,
                             "failed to read request header")) {
            return 0;
        }
    }

    header_len = (size_t)(header_end - connection->buffer);
    if (header_len > MAX_HEADER_BYTES) {
        return set_error(status_code, error_message, 400, "request header too large");
    }

    memcpy(header_copy, connection->buffer, header_len);
    header_copy[header_len] = '\0';

    line = strtok_r(header_copy, "\r\n", &saveptr);
    if (!line) return set_error(status_code, error_message, 400, "invalid request line");
    if (sscanf(line, "%15s %255s %15s", request->method, request->path, request->version) != 3) {
        return set_error(status_code, error_message, 400, "invalid request line");
    }
    if (strcmp(request->version, "HTTP/1.1") != 0) {
        return set_error(status_code, error_message, 400, "only HTTP/1.1 is supported");
    }

    while ((line = strtok_r(NULL, "\r\n", &saveptr)) != NULL) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        colon++;
        trim(line);
        trim(colon);
        if (starts_with_ci(line, "Content-Length")) {
            char *endptr = NULL;
            unsigned long len = strtoul(colon, &endptr, 10);
            if (!endptr || *endptr != '\0') {
                return set_error(status_code, error_message, 400, "invalid Content-Length");
            }
            request->content_length = (size_t)len;
        } else if (starts_with_ci(line, "Content-Type")) {
            strncpy(request->content_type, colon, sizeof(request->content_type) - 1);
            request->content_type[sizeof(request->content_type) - 1] = '\0';
        } else if (starts_with_ci(line, "Connection")) {
            if (starts_with_ci(colon, "close")) request->connection_close = 1;
        } else if (starts_with_ci(line, "Transfer-Encoding")) {
            return set_error(status_code, error_message, 400, "chunked transfer is not supported");
        }
    }

    if (strcmp(request->path, "/query") == 0) {
        if (strcmp(request->method, "POST") != 0) {
            return set_error(status_code, error_message, 405, "method not allowed");
        }
        if (request->content_length == 0) return set_error(status_code, error_message, 400, "empty request body");
        if (request->content_length > (size_t)(MAX_SQL_LEN - 1)) {
            return set_error(status_code, error_message, 413, "request body too large");
        }
        if (!is_text_plain(request->content_type)) {
            return set_error(status_code, error_message, 400, "Content-Type must be text/plain");
        }
    } else if (strcmp(request->path, "/stats") == 0) {
        if (strcmp(request->method, "GET") != 0) {
            return set_error(status_code, error_message, 405, "method not allowed");
        }
        if (request->content_length != 0) {
            return set_error(status_code, error_message, 400, "GET /stats does not accept a request body");
        }
    } else {
        return set_error(status_code, error_message, 404, "not found");
    }

    request_bytes = header_len + 4 + request->content_length;
    if (request_bytes > sizeof(connection->buffer)) {
        return set_error(status_code, error_message, 413, "request body too large");
    }

    if (!ensure_buffered(connection,
                         request_bytes,
                         status_code,
                         error_message,
                         "failed to read request body")) {
        return 0;
    }

    if (request->content_length > 0) {
        request->body = (char *)calloc(request->content_length + 1, 1);
        if (!request->body) return set_error(status_code, error_message, 500, "out of memory");
        memcpy(request->body, connection->buffer + header_len + 4, request->content_length);
        request->body[request->content_length] = '\0';
    }

    if (connection->used > request_bytes) {
        memmove(connection->buffer, connection->buffer + request_bytes, connection->used - request_bytes);
    }
    connection->used -= request_bytes;
    return 1;
}
