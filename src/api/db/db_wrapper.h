#ifndef API_DB_WRAPPER_H
#define API_DB_WRAPPER_H

#include <stddef.h>

typedef struct {
    char *name;
    char *value;
} DbCell;

typedef struct {
    size_t cell_count;
    DbCell *cells;
} DbRow;

typedef struct DbResult {
    int ok;
    int http_status;
    char *error_message;
    size_t row_count;
    DbRow *rows;
} DbResult;

int db_wrapper_init(void);
void db_wrapper_destroy(void);
DbResult db_execute_sql(const char *sql);
void db_result_free(DbResult *result);

#endif
