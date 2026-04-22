#ifndef API_JSON_BUILDER_H
#define API_JSON_BUILDER_H

#include <stddef.h>

#include "../db/db_wrapper.h"
#include "../stats/stats.h"

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    size_t row_count;
    int started;
    int failed;
} JsonRowsWriter;

int json_build_response(const DbResult *result, char **out_json, size_t *out_len);
char *json_build_error(const char *message, size_t *out_len);
int json_build_ok_response(char **out_json, size_t *out_len);
int json_rows_writer_init(JsonRowsWriter *writer);
int json_rows_writer_add_row(JsonRowsWriter *writer,
                             const char **col_names,
                             const char **values,
                             int col_count);
int json_rows_writer_finish(JsonRowsWriter *writer, char **out_json, size_t *out_len);
void json_rows_writer_destroy(JsonRowsWriter *writer);
int json_build_stats_response(const ApiStatsSnapshot *snapshot, char **out_json, size_t *out_len);

#endif
