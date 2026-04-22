#include "request_handler.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../db/db_wrapper.h"
#include "../json/json_builder.h"
#include "../log/log.h"
#include "../net/http_parser.h"
#include "../stats/stats.h"

static const char *status_text(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Internal Server Error";
    }
}

static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        sent += (size_t)n;
    }
    return 1;
}

static char *dup_body(const char *s) {
    size_t len;
    char *out;
    if (!s) return NULL;
    len = strlen(s);
    out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len + 1);
    return out;
}

static unsigned long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

static int send_json_response(int fd,
                              int status_code,
                              const char *json_body,
                              size_t body_len,
                              int connection_close) {
    char header[256];
    int header_len;
    if (!json_body) {
        return 0;
    }
    header_len = snprintf(header, sizeof(header),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: application/json\r\n"
                          "Connection: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "\r\n",
                          status_code, status_text(status_code),
                          connection_close ? "close" : "keep-alive",
                          body_len);
    if (header_len > 0) {
        if (!send_all(fd, header, (size_t)header_len)) return 0;
        if (!send_all(fd, json_body, body_len)) return 0;
    }
    return 1;
}

void send_json_response_and_close(int fd, int status_code, const char *json_body, size_t body_len) {
    (void)send_json_response(fd, status_code, json_body, body_len, 1);
    close(fd);
}

void send_error_json_and_close(int fd, int status_code, const char *message) {
    size_t body_len = 0;
    char *json = json_build_error(message, &body_len);
    if (!json) {
        static const char *fallback = "{\"status\":\"error\",\"message\":\"internal error\"}";
        send_json_response_and_close(fd, 500, fallback, strlen(fallback));
        return;
    }
    send_json_response_and_close(fd, status_code, json, body_len);
    free(json);
}

void handle_connection(int fd, unsigned long long trace_id) {
    HttpConnection connection;
    int request_count = 0;

    http_connection_init(&connection, fd);

    for (;;) {
        ParsedHttpRequest request;
        DbJsonResponse db_response;
        ApiStatsSnapshot snapshot;
        int parse_status = 0;
        int status_code = 500;
        int should_close = 0;
        char *parse_err = NULL;
        char *json = NULL;
        size_t json_len = 0;
        unsigned long long parse_start;
        unsigned long long send_start;
        int send_ok;

        parse_start = now_ns();
        if (!parse_http_request(&connection, &request, &parse_status, &parse_err)) {
            api_stats_add_parse_ns(now_ns() - parse_start);
            if (parse_status == 0) break;
            api_stats_note_request_started();
            status_code = parse_status ? parse_status : 400;
            log_write(LOG_WARN, trace_id, "request parse failed: %s", parse_err ? parse_err : "unknown");
            json = json_build_error(parse_err ? parse_err : "invalid request", &json_len);
            if (!json) {
                static const char *fallback = "{\"status\":\"error\",\"message\":\"internal error\"}";
                json = dup_body(fallback);
                json_len = strlen(fallback);
            }
            send_start = now_ns();
            send_ok = send_json_response(fd, status_code, json, json_len, 1);
            api_stats_add_send_ns(now_ns() - send_start);
            api_stats_note_request_finished(status_code);
            free(json);
            free(parse_err);
            if (!send_ok) log_write(LOG_WARN, trace_id, "response send failed after parse error");
            break;
        }

        api_stats_add_parse_ns(now_ns() - parse_start);
        api_stats_note_request_started();
        if (request_count > 0) api_stats_note_keep_alive_reuse();
        request_count++;
        should_close = request.connection_close;

        if (strcmp(request.path, "/stats") == 0) {
            api_stats_snapshot(&snapshot);
            if (!json_build_stats_response(&snapshot, &json, &json_len)) {
                json = json_build_error("failed to serialize stats", &json_len);
                status_code = 500;
            } else {
                status_code = 200;
            }
            if (!json) {
                static const char *fallback = "{\"status\":\"error\",\"message\":\"internal error\"}";
                json = dup_body(fallback);
                json_len = strlen(fallback);
                status_code = 500;
            }
        } else {
            memset(&db_response, 0, sizeof(db_response));
            if (!db_execute_sql_json(request.body, &db_response)) {
                json = json_build_error("failed to execute SQL", &json_len);
                status_code = 500;
                if (!json) {
                    static const char *fallback = "{\"status\":\"error\",\"message\":\"internal error\"}";
                    json = dup_body(fallback);
                    json_len = strlen(fallback);
                }
            } else {
                json = db_response.json_body;
                json_len = db_response.json_len;
                status_code = db_response.http_status ? db_response.http_status : (db_response.ok ? 200 : 500);
                db_response.json_body = NULL;
                db_json_response_free(&db_response);
            }
        }

        send_start = now_ns();
        send_ok = send_json_response(fd, status_code, json, json_len, should_close);
        api_stats_add_send_ns(now_ns() - send_start);
        api_stats_note_request_finished(status_code);
        if (!send_ok) {
            log_write(LOG_WARN, trace_id, "response send failed with status=%d", status_code);
            should_close = 1;
        }

        free(json);
        free_http_request(&request);
        if (should_close) break;
    }

    close(fd);
}
