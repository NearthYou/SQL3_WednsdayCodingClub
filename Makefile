CC ?= gcc
CFLAGS ?= -O2 -fdiagnostics-color=always -g
TARGET ?= sqlsprocessor
API_TARGET ?= mini_dbms_api

SRC_DIR := src
BENCH_DIR := tools/bench
EXAMPLE_SQL_DIR := examples/sql
API_DIR := $(SRC_DIR)/api

BENCH_GEN ?= bench_workload_generator
BENCH_RUNNER ?= benchmark_runner
BENCH_TEST ?= bench_formula_test

SRC = $(SRC_DIR)/main.c
SRC_DEPS = \
	$(SRC_DIR)/main.c \
	$(SRC_DIR)/lexer.c \
	$(SRC_DIR)/parser.c \
	$(SRC_DIR)/bptree.c \
	$(SRC_DIR)/executor.c \
	$(SRC_DIR)/lexer.h \
	$(SRC_DIR)/parser.h \
	$(SRC_DIR)/bptree.h \
	$(SRC_DIR)/executor.h \
	$(SRC_DIR)/types.h

SQL ?= $(EXAMPLE_SQL_DIR)/demo_bptree.sql
API_PORT ?= 8080
PYTHON ?= python
JUNGLE_DATASET ?= jungle_benchmark_users.csv
JUNGLE_RECORDS ?= 1000000
BENCH_SCORE_UPDATE_ROWS ?= 1000000
BENCH_SCORE_DELETE_ROWS ?= 1000000
BENCH_SCORE_IN_TMP ?= 1

.PHONY: all build api run-api test-api api-load bench-tools bench-test run demo-bptree demo-jungle scenario-jungle-regression scenario-jungle-range-and-replay scenario-jungle-update-constraints generate-jungle generate-jungle-sql benchmark benchmark-jungle bench-smoke bench-score bench-report bench-clean clean

all: build

build: $(TARGET)

$(TARGET): $(SRC_DEPS)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

API_CFLAGS := -Wall -Wextra -O2 -g -pthread
API_SRC = \
	$(SRC_DIR)/api_main.c \
	$(SRC_DIR)/lexer.c \
	$(SRC_DIR)/parser.c \
	$(SRC_DIR)/bptree.c \
	$(SRC_DIR)/executor.c \
	$(API_DIR)/net/listener.c \
	$(API_DIR)/net/http_parser.c \
	$(API_DIR)/pool/task_queue.c \
	$(API_DIR)/pool/thread_pool.c \
	$(API_DIR)/handler/request_handler.c \
	$(API_DIR)/db/db_wrapper.c \
	$(API_DIR)/json/json_builder.c \
	$(API_DIR)/log/log.c

api: $(API_TARGET)

$(API_TARGET): $(API_SRC)
	$(CC) $(API_CFLAGS) $(API_SRC) -o $(API_TARGET)

run-api: $(API_TARGET)
	./$(API_TARGET)

test-api: $(API_TARGET) tests/api/test_http_parser tests/api/test_json_builder tests/api/test_task_queue
	tests/api/test_http_parser
	tests/api/test_json_builder
	tests/api/test_task_queue

api-load:
	$(PYTHON) scripts/run_api_load_test.py --concurrency 50 --requests 1000 --mix read-heavy --report artifacts/api-load/report.json

tests/api/test_http_parser: tests/api/test_http_parser.c $(API_DIR)/net/http_parser.c
	$(CC) $(API_CFLAGS) -Isrc -o $@ tests/api/test_http_parser.c $(API_DIR)/net/http_parser.c

tests/api/test_json_builder: tests/api/test_json_builder.c $(API_DIR)/json/json_builder.c
	$(CC) $(API_CFLAGS) -Isrc -o $@ tests/api/test_json_builder.c $(API_DIR)/json/json_builder.c

tests/api/test_task_queue: tests/api/test_task_queue.c $(API_DIR)/pool/task_queue.c
	$(CC) $(API_CFLAGS) -Isrc -o $@ tests/api/test_task_queue.c $(API_DIR)/pool/task_queue.c

bench-tools: $(BENCH_GEN) $(BENCH_RUNNER)

$(BENCH_GEN): $(BENCH_DIR)/bench_workload_generator.c
	$(CC) $(CFLAGS) $< -o $(BENCH_GEN)

$(BENCH_RUNNER): $(BENCH_DIR)/benchmark_runner.c
	$(CC) $(CFLAGS) $< -o $(BENCH_RUNNER)

$(BENCH_TEST): $(BENCH_DIR)/bench_formula_test.c
	$(CC) $(CFLAGS) $< -o $(BENCH_TEST)

bench-test: $(BENCH_TEST)
	./$(BENCH_TEST)

$(JUNGLE_DATASET): $(TARGET)
	./$(TARGET) --generate-jungle $(JUNGLE_RECORDS) $(JUNGLE_DATASET)

run: $(TARGET)
	./$(TARGET) $(SQL)

demo-bptree: $(TARGET)
	./$(TARGET) $(EXAMPLE_SQL_DIR)/demo_bptree.sql

demo-jungle: $(TARGET) $(JUNGLE_DATASET)
	./$(TARGET) $(EXAMPLE_SQL_DIR)/demo_jungle.sql

scenario-jungle-regression: $(TARGET)
	./$(TARGET) $(EXAMPLE_SQL_DIR)/scenario_jungle_regression.sql

scenario-jungle-range-and-replay: $(TARGET)
	./$(TARGET) $(EXAMPLE_SQL_DIR)/scenario_jungle_range_and_replay.sql

scenario-jungle-update-constraints: $(TARGET)
	./$(TARGET) $(EXAMPLE_SQL_DIR)/scenario_jungle_update_constraints.sql

generate-jungle: $(JUNGLE_DATASET)

generate-jungle-sql: $(JUNGLE_DATASET)
	$(PYTHON) scripts/generate_jungle_sql_workloads.py

benchmark: $(TARGET)
	./$(TARGET) --benchmark 1000000

benchmark-jungle: $(TARGET)
	./$(TARGET) --benchmark-jungle 1000000

bench-smoke: build bench-tools
	./$(BENCH_RUNNER) --profile smoke --seed 20260415 --repeat 3 --memtrack

bench-score: build bench-tools
ifeq ($(BENCH_SCORE_IN_TMP),1)
	tr -d '\r' < scripts/run_bench_score_tmp.sh | sh -s -- "$(CURDIR)" "$(BENCH_SCORE_UPDATE_ROWS)" "$(BENCH_SCORE_DELETE_ROWS)"
else
	./$(BENCH_RUNNER) --profile score --seed 20260415 --repeat 1 --update-rows $(BENCH_SCORE_UPDATE_ROWS) --delete-rows $(BENCH_SCORE_DELETE_ROWS) --memtrack
endif

bench-report: $(BENCH_RUNNER)
	./$(BENCH_RUNNER) --report-only

bench-clean:
	rm -rf artifacts/bench
	rm -f generated_sql/jungle_insert_smoke.sql generated_sql/jungle_update_smoke.sql generated_sql/jungle_delete_smoke.sql
	rm -f generated_sql/jungle_insert_regression.sql generated_sql/jungle_update_regression.sql generated_sql/jungle_delete_regression.sql
	rm -f generated_sql/jungle_insert_score.sql generated_sql/jungle_update_score.sql generated_sql/jungle_delete_score.sql
	rm -f generated_sql/jungle_correctness_success_smoke.sql generated_sql/jungle_correctness_failure_smoke.sql
	rm -f generated_sql/jungle_correctness_success_regression.sql generated_sql/jungle_correctness_failure_regression.sql
	rm -f generated_sql/jungle_correctness_success_score.sql generated_sql/jungle_correctness_failure_score.sql
	rm -f generated_sql/workload_smoke.sql generated_sql/workload_regression.sql generated_sql/workload_score.sql
	rm -f generated_sql/workload_smoke.meta.json generated_sql/workload_regression.meta.json generated_sql/workload_score.meta.json
	rm -f generated_sql/oracle_smoke.json generated_sql/oracle_regression.json generated_sql/oracle_score.json

clean:
	rm -f $(TARGET) $(API_TARGET) $(BENCH_GEN) $(BENCH_RUNNER) $(BENCH_TEST)
	rm -f tests/api/test_http_parser tests/api/test_json_builder tests/api/test_task_queue
