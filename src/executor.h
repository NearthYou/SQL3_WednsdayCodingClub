#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "types.h"
#include <stddef.h>

typedef int (*EngineRowSink)(const char **col_names,
                             const char **values,
                             int col_count,
                             void *ctx);

void execute_insert(Statement *stmt);
void execute_select(Statement *stmt);
void execute_update(Statement *stmt);
void execute_delete(Statement *stmt);
int execute_select_result(Statement *stmt,
                          EngineRowSink sink,
                          void *ctx,
                          char *err,
                          size_t err_size);
int execute_insert_result(Statement *stmt, long *inserted_id, char *err, size_t err_size);
int execute_update_result(Statement *stmt, int *affected_rows, char *err, size_t err_size);
int execute_delete_result(Statement *stmt, int *affected_rows, char *err, size_t err_size);
void generate_jungle_dataset(int record_count, const char *filename);
void run_bplus_benchmark(int record_count);
void run_jungle_benchmark(int record_count);
void close_all_tables(void);
void set_executor_quiet(int quiet);

#endif
