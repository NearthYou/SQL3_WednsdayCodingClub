#include "request_handler.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../db/db_wrapper.h"
#include "../json/json_builder.h"
#include "../log/log.h"
#include "../net/http_parser.h"

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

void send_json_response_and_close(int fd, int status_code, const char *json_body, size_t body_len) {
    char header[256];
    int header_len;
    if (!json_body) {
        close(fd);
        return;
    }
    header_len = snprintf(header, sizeof(header),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: application/json\r\n"
                          "Connection: close\r\n"
                          "Content-Length: %zu\r\n"
                          "\r\n",
                          status_code, status_text(status_code), body_len);
    if (header_len > 0) {
        (void)send_all(fd, header, (size_t)header_len);
        (void)send_all(fd, json_body, body_len);
    }
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
    ParsedHttpRequest request;
    DbResult result;
    int parse_status = 0;
    char *parse_err = NULL;
    char *json = NULL;
    size_t json_len = 0;
    int status_code = 500;

    log_write(LOG_INFO, trace_id, "request started");

    if (!parse_http_request(fd, &request, &parse_status, &parse_err)) {
        log_write(LOG_WARN, trace_id, "request parse failed: %s", parse_err ? parse_err : "unknown");
        send_error_json_and_close(fd, parse_status ? parse_status : 400, parse_err ? parse_err : "invalid request");
        free(parse_err);
        return;
    }

    result = db_execute_sql(request.body);
    status_code = result.ok ? 200 : (result.http_status ? result.http_status : 500);

    if (!json_build_response(&result, &json, &json_len)) {
        db_result_free(&result);
        free_http_request(&request);
        send_error_json_and_close(fd, 500, "failed to serialize response");
        return;
    }

    send_json_response_and_close(fd, status_code, json, json_len);
    free(json);
    db_result_free(&result);
    free_http_request(&request);
    log_write(LOG_INFO, trace_id, "request finished with status=%d", status_code);
}
