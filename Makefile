CC ?= gcc
CFLAGS ?= -O2 -fdiagnostics-color=always -g
CPPFLAGS ?= -I. -Isrc -Isrc/legacy
LDLIBS ?= -lpthread

TARGET ?= sqlsprocessor
BIN_DIR ?= bin
OBJ_DIR ?= build
PYTHON ?= python3

BENCH_GEN ?= bench_workload_generator
BENCH_RUNNER ?= benchmark_runner
BENCH_TEST ?= bench_formula_test
BENCH_GEN_SRC := tools/bench/bench_workload_generator.c
BENCH_RUNNER_SRC := tools/bench/benchmark_runner.c
BENCH_TEST_SRC := tools/bench/bench_formula_test.c

SQL ?= sql/legacy/demo_bptree.sql
JUNGLE_DATASET ?= data/legacy/jungle_benchmark_users.csv
JUNGLE_RECORDS ?= 1000000
BENCH_SCORE_UPDATE_ROWS ?= 1000000
BENCH_SCORE_DELETE_ROWS ?= 1000000
BENCH_SCORE_IN_TMP ?= 1

LEGACY_SRCS = \
	src/legacy/lexer.c \
	src/legacy/parser.c \
	src/legacy/bptree.c

SRV_SRCS = \
	src/dbsrv.c \
	src/api/resp.c \
	src/api/route.c \
	src/api/srv.c \
	src/db/dbapi.c \
	src/db/mvcc.c \
	src/thr/pool.c \
	$(LEGACY_SRCS)

TEST_POOL_SRCS = tests/test_pool.c tests/tutil.c src/thr/pool.c
TEST_MVCC_SRCS = tests/test_mvcc.c tests/tutil.c src/db/dbapi.c src/db/mvcc.c $(LEGACY_SRCS)
TEST_DBAPI_SRCS = tests/test_dbapi.c tests/tutil.c src/db/dbapi.c src/db/mvcc.c $(LEGACY_SRCS)
TEST_TX_SRCS = tests/test_tx.c tests/tutil.c src/db/dbapi.c src/db/mvcc.c $(LEGACY_SRCS)

SRV_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(SRV_SRCS))
TEST_POOL_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(TEST_POOL_SRCS))
TEST_MVCC_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(TEST_MVCC_SRCS))
TEST_DBAPI_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(TEST_DBAPI_SRCS))
TEST_TX_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(TEST_TX_SRCS))

TEST_BINS = \
	$(BIN_DIR)/test_pool \
	$(BIN_DIR)/test_mvcc \
	$(BIN_DIR)/test_dbapi \
	$(BIN_DIR)/test_tx

.PHONY: all build build-tests test run demo-bptree demo-jungle scenario-jungle-regression scenario-jungle-range-and-replay scenario-jungle-update-constraints generate-jungle generate-jungle-sql benchmark benchmark-jungle bench-tools bench-test bench-smoke bench-score bench-report bench-clean clean

all: build

build: $(TARGET) $(BIN_DIR)/dbsrv

build-tests: $(TEST_BINS) $(BIN_DIR)/dbsrv

test: build-tests
	./$(BIN_DIR)/test_pool
	./$(BIN_DIR)/test_mvcc
	./$(BIN_DIR)/test_dbapi
	./$(BIN_DIR)/test_tx
	sh tests/api_test.sh

$(TARGET): src/cli/main.c src/legacy/lexer.c src/legacy/parser.c src/legacy/bptree.c src/legacy/executor.c src/legacy/lexer.h src/legacy/parser.h src/legacy/bptree.h src/legacy/executor.h src/legacy/types.h
	$(CC) $(CPPFLAGS) $(CFLAGS) src/cli/main.c -o $(TARGET)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/dbsrv: $(BIN_DIR) $(SRV_OBJS)
	$(CC) $(CFLAGS) $(SRV_OBJS) -o $@ $(LDLIBS)

$(BIN_DIR)/test_pool: $(BIN_DIR) $(TEST_POOL_OBJS)
	$(CC) $(CFLAGS) $(TEST_POOL_OBJS) -o $@ $(LDLIBS)

$(BIN_DIR)/test_mvcc: $(BIN_DIR) $(TEST_MVCC_OBJS)
	$(CC) $(CFLAGS) $(TEST_MVCC_OBJS) -o $@ $(LDLIBS)

$(BIN_DIR)/test_dbapi: $(BIN_DIR) $(TEST_DBAPI_OBJS)
	$(CC) $(CFLAGS) $(TEST_DBAPI_OBJS) -o $@ $(LDLIBS)

$(BIN_DIR)/test_tx: $(BIN_DIR) $(TEST_TX_OBJS)
	$(CC) $(CFLAGS) $(TEST_TX_OBJS) -o $@ $(LDLIBS)

bench-tools: $(BENCH_GEN) $(BENCH_RUNNER)

$(BENCH_GEN): $(BENCH_GEN_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BENCH_GEN_SRC) -o $(BENCH_GEN)

$(BENCH_RUNNER): $(BENCH_RUNNER_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BENCH_RUNNER_SRC) -o $(BENCH_RUNNER)

$(BENCH_TEST): $(BENCH_TEST_SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BENCH_TEST_SRC) -o $(BENCH_TEST)

bench-test: $(BENCH_TEST)
	./$(BENCH_TEST)

$(JUNGLE_DATASET): $(TARGET)
	./$(TARGET) --generate-jungle $(JUNGLE_RECORDS)

run: $(TARGET)
	./$(TARGET) $(SQL)

demo-bptree: $(TARGET)
	./$(TARGET) sql/legacy/demo_bptree.sql

demo-jungle: $(TARGET) $(JUNGLE_DATASET)
	./$(TARGET) sql/legacy/demo_jungle.sql

scenario-jungle-regression: $(TARGET)
	./$(TARGET) sql/legacy/scenario_jungle_regression.sql

scenario-jungle-range-and-replay: $(TARGET)
	./$(TARGET) sql/legacy/scenario_jungle_range_and_replay.sql

scenario-jungle-update-constraints: $(TARGET)
	./$(TARGET) sql/legacy/scenario_jungle_update_constraints.sql

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
	rm -rf $(OBJ_DIR) $(BIN_DIR)
	rm -f $(TARGET) $(BENCH_GEN) $(BENCH_RUNNER) $(BENCH_TEST)
