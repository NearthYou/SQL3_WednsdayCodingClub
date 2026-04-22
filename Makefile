CC ?= gcc
CFLAGS ?= -O2 -fdiagnostics-color=always -g
TARGET ?= sqlsprocessor

SRC_DIR := src
BENCH_DIR := tools/bench
EXAMPLE_SQL_DIR := examples/sql

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
PYTHON ?= python
JUNGLE_DATASET ?= jungle_benchmark_users.csv
JUNGLE_RECORDS ?= 1000000
BENCH_SCORE_UPDATE_ROWS ?= 1000000
BENCH_SCORE_DELETE_ROWS ?= 1000000
BENCH_SCORE_IN_TMP ?= 1

.PHONY: all build bench-tools bench-test run demo-bptree demo-jungle scenario-jungle-regression scenario-jungle-range-and-replay scenario-jungle-update-constraints generate-jungle generate-jungle-sql benchmark benchmark-jungle bench-smoke bench-score bench-report bench-clean clean

all: build

build: $(TARGET)

$(TARGET): $(SRC_DEPS)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

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
	rm -f $(TARGET) $(BENCH_GEN) $(BENCH_RUNNER) $(BENCH_TEST)
