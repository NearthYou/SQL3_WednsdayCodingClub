#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../../src/api/net/http_parser.h"

static void test_valid_request(void) {
    int fds[2];
    ParsedHttpRequest req;
    int status = 0;
    char *err = NULL;
    const char *raw =
        "POST /query HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 33\r\n"
        "\r\n"
        "SELECT * FROM users WHERE id = 1;";

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(write(fds[0], raw, strlen(raw)) == (ssize_t)strlen(raw));
    assert(parse_http_request(fds[1], &req, &status, &err) == 1);
    assert(strcmp(req.method, "POST") == 0);
    assert(strcmp(req.path, "/query") == 0);
    assert(strcmp(req.body, "SELECT * FROM users WHERE id = 1;") == 0);
    free_http_request(&req);
    close(fds[0]);
    close(fds[1]);
}

static void test_invalid_method(void) {
    int fds[2];
    ParsedHttpRequest req;
    int status = 0;
    char *err = NULL;
    const char *raw =
        "GET /query HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 1\r\n"
        "\r\n"
        "x";

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(write(fds[0], raw, strlen(raw)) == (ssize_t)strlen(raw));
    assert(parse_http_request(fds[1], &req, &status, &err) == 0);
    assert(status == 405);
    free(err);
    close(fds[0]);
    close(fds[1]);
}

int main(void) {
    test_valid_request();
    test_invalid_method();
    printf("test_http_parser: OK\n");
    return 0;
}
