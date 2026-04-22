#ifndef API_JSON_BUILDER_H
#define API_JSON_BUILDER_H

#include <stddef.h>

#include "../db/db_wrapper.h"

int json_build_response(const DbResult *result, char **out_json, size_t *out_len);
char *json_build_error(const char *message, size_t *out_len);

#endif
