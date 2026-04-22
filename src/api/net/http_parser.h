#ifndef API_HTTP_PARSER_H
#define API_HTTP_PARSER_H

#include <stddef.h>

typedef struct {
    char method[16];
    char path[256];
    char version[16];
    char content_type[128];
    size_t content_length;
    char *body;
} ParsedHttpRequest;

int parse_http_request(int fd, ParsedHttpRequest *request, int *status_code, char **error_message);
void free_http_request(ParsedHttpRequest *request);

#endif
