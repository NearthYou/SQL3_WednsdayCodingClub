#ifndef API_REQUEST_HANDLER_H
#define API_REQUEST_HANDLER_H

#include <stddef.h>

void handle_connection(int fd, unsigned long long trace_id);
void send_json_response_and_close(int fd, int status_code, const char *json_body, size_t body_len);
void send_error_json_and_close(int fd, int status_code, const char *message);

#endif
