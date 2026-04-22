#ifndef API_HTTP_PARSER_H
#define API_HTTP_PARSER_H

#include <stddef.h>

#define HTTP_CONNECTION_BUFFER_CAPACITY 32768

typedef struct {
    int fd;
    char buffer[HTTP_CONNECTION_BUFFER_CAPACITY];
    size_t used;
} HttpConnection;

typedef struct {
    char method[16];
    char path[256];
    char version[16];
    char content_type[128];
    size_t content_length;
    char *body;
    int connection_close;
} ParsedHttpRequest;

void http_connection_init(HttpConnection *connection, int fd);
int parse_http_request(HttpConnection *connection, ParsedHttpRequest *request, int *status_code, char **error_message);
void free_http_request(ParsedHttpRequest *request);

#endif
