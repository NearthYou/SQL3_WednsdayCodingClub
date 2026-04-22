# SQL-B-Tree

C 기반 SQL 처리기입니다. CSV 파일을 테이블처럼 사용하며 `INSERT`, `SELECT`, `UPDATE`, `DELETE`를 실행합니다.

이번 버전은 메모리 기반 `B+ Tree` 인덱스를 추가했습니다. `id(PK)` 컬럼은 자동 증가 ID로 사용할 수 있고, `WHERE id = ...` 형태의 조회는 B+ Tree를 사용합니다. `UK` 컬럼은 문자열 B+ Tree 인덱스로 관리하며, 인덱스가 없는 컬럼 조건은 선형 탐색으로 비교합니다.

현재 메모리 기반 구현의 안전 상한은 2,000,000건입니다. CSV가 이 상한을 넘으면 앞쪽 2,000,000건만 `TableCache`에 캐시하고, 이후 row는 CSV에 남겨 둔 채 tail scan fallback으로 처리합니다.

## 폴더 구조

```text
src/              SQL 처리기 C 소스와 헤더
tools/bench/      벤치마크 생성기/러너/검증 도구
examples/sql/     데모와 회귀 시나리오 SQL
examples/data/    데모와 회귀 시나리오용 작은 CSV 샘플
scripts/          워크로드 생성/벤치 실행 보조 스크립트
docs/             설계와 발표용 문서
generated_sql/    생성되는 대용량 SQL 출력물(.gitignore)
artifacts/        벤치마크 리포트 출력물(.gitignore)
```

엔진은 먼저 현재 폴더의 `<테이블명>.csv`를 찾고, 없으면 `examples/data/<테이블명>.csv`를 자동으로 찾아 예제 SQL을 실행합니다. 새로 생성되는 대용량/런타임 CSV는 기존처럼 프로젝트 루트에 만들어집니다.

## 빌드

```powershell
gcc -O2 src/main.c -o sqlsprocessor.exe
```

`make`가 있는 환경에서는 기존 Makefile도 사용할 수 있습니다.

```bash
make
```

## API 서버 (v1)

`POST /query` 단일 엔드포인트를 제공하는 HTTP/1.1 API 서버를 별도 실행 파일로 빌드할 수 있습니다.

상세 설계/제약은 `docs/API_SERVER_DEVELOPMENT_SPEC_KO.md` 문서를 따릅니다.

```bash
make api
./mini_dbms_api
```

기본 포트는 `8080`이고, 요청은 `Content-Type: text/plain`의 SQL 1문장을 body로 보냅니다.

```bash
curl -X POST http://127.0.0.1:8080/query \
  -H 'Content-Type: text/plain' \
  --data "SELECT * FROM case_basic_users WHERE id = 1;"
```

API 테스트:

```bash
make test-api
bash tests/api/smoke.sh
bash tests/api/stress.sh
```

API 부하 테스트 결과를 JSON/Markdown으로 남길 수 있습니다. 서버를 먼저 실행한 뒤 별도 터미널에서 실행합니다.

```bash
make run-api
python scripts/run_api_load_test.py --concurrency 50 --requests 1000 --mix read-heavy --report artifacts/api-load/report.json
```

## 기본 실행

```powershell
.\sqlsprocessor.exe examples/sql/demo_bptree.sql
```

SQL 파일은 세미콜론(`;`)으로 문장을 구분합니다.

## 기존 B+ Tree 데모

기본 샘플은 `examples/data/case_basic_users.csv` 와 `examples/sql/demo_bptree.sql` 입니다.

```sql
SELECT * FROM case_basic_users WHERE id = 4;
SELECT * FROM case_basic_users WHERE name = 'AutoUser';
```

실행 결과에 아래 표시가 나오면 각각 인덱스와 선형 탐색 경로를 뜻합니다.

```text
[index] B+ tree id lookup
[scan] linear scan on column 'name'
```

범위 조건도 지원합니다.

```sql
SELECT * FROM case_basic_users WHERE id BETWEEN 2 AND 4;
SELECT * FROM case_basic_users WHERE email BETWEEN 'a@test.com' AND 'm@test.com';
```

```text
[index] B+ tree id range lookup
[index] UK B+ tree range lookup on column 'email'
```

## 기존 벤치마크

기존 `--benchmark` 는 현재 브랜치의 기존 방식 그대로 유지합니다. 이 경로는 `bptree_benchmark_users.csv` 기반의 legacy benchmark이며, bulk-build 중심 실험을 사용합니다.

```powershell
.\sqlsprocessor.exe --benchmark 1000000
```

## 정글 지원자 데모

과제 발표용 대규모 데이터셋은 `정글 지원자 마감 직전, 지원자 100만 건이 몰린 접수 시스템` 콘셉트로 구성했습니다.

- 기본 파일: `jungle_benchmark_users.csv`
- 기본 테이블명: `jungle_benchmark_users`
- 헤더:

```csv
id(PK),email(UK),phone(UK),name,track(NN),background,history,pretest,github,status,round
```

- 예시 row:

```csv
777777,jungle0777777@apply.kr,010-0077-7777,한류진,game_tech_lab,incumbent,frontend_3y,61,gh_0777777,submitted,2026_spring
```

상세 생성 규칙은 `docs/JUNGLE_DATASET_DESIGN_KO.md` 에 정리했습니다.

### 정글 데이터셋 생성

```powershell
.\sqlsprocessor.exe --generate-jungle 1000000
```

다른 파일명으로도 만들 수 있습니다.

```powershell
.\sqlsprocessor.exe --generate-jungle 1000000 my_jungle_demo.csv
```

기본 출력 파일이 이미 있으면 덮어쓰지 않고 안전하게 중단합니다.

```text
[safe-stop] dataset file already exists: ...
```

### 정글 발표 데모

```powershell
.\sqlsprocessor.exe examples/sql/demo_jungle.sql
```

또는

```bash
make demo-jungle
```

`examples/sql/demo_jungle.sql` 은 100만 건 테이블에서 아래 조회 경로를 보여줍니다.

- `id`: PK B+ Tree
- `email`: UK B+ Tree
- `phone`: UK B+ Tree
- `name`: 선형 탐색

## 정글 추가 시나리오

작은 확인용 테이블 `examples/data/jungle_test_users.csv` 로 반복 가능한 추가 시나리오를 제공합니다.

```powershell
.\sqlsprocessor.exe examples/sql/scenario_jungle_regression.sql
.\sqlsprocessor.exe examples/sql/scenario_jungle_range_and_replay.sql
.\sqlsprocessor.exe examples/sql/scenario_jungle_update_constraints.sql
```

각 시나리오의 확인 항목:

- `examples/sql/scenario_jungle_regression.sql`
  - PK/UK exact lookup
  - 비인덱스 컬럼 선형 탐색
  - UPDATE/DELETE 기초 동작
  - duplicate PK / duplicate email / duplicate phone / NN 제약
- `examples/sql/scenario_jungle_range_and_replay.sql`
  - PK/UK range lookup
  - UK 기준 UPDATE/DELETE
  - 다른 테이블 재오픈 후 delta replay
- `examples/sql/scenario_jungle_update_constraints.sql`
  - duplicate UK UPDATE 거부
  - NN 컬럼 빈 문자열 UPDATE 거부
  - PK UPDATE 거부
  - 실패 후 정상 UPDATE 가능 여부

## 정글 SQL 워크로드 생성

정글 데이터셋을 기반으로 `INSERT / UPDATE / DELETE` SQL 파일을 생성할 수 있습니다.

```bash
python scripts/generate_jungle_sql_workloads.py
```

또는

```bash
make generate-jungle-sql
```

생성 결과물은 `generated_sql/` 아래에 만들어집니다.

- `jungle_insert_1000000.sql`
- `jungle_update_1000000.sql`
- `jungle_delete_1000000.sql`

체크용 SQL도 함께 제공합니다.

권장 순서는 아래와 같습니다.

```powershell
.\sqlsprocessor.exe generated_sql/jungle_insert_1000000.sql
.\sqlsprocessor.exe examples/sql/scenario_jungle_workload_after_insert.sql
.\sqlsprocessor.exe generated_sql/jungle_update_1000000.sql
.\sqlsprocessor.exe examples/sql/scenario_jungle_workload_after_update.sql
.\sqlsprocessor.exe generated_sql/jungle_delete_1000000.sql
.\sqlsprocessor.exe examples/sql/scenario_jungle_workload_after_delete.sql
```

## 정글 SQL 경로 벤치마크

`--benchmark-jungle` 은 과제 요구사항에 맞춰 실제 `INSERT` 경로로 100만 건 이상을 넣고, 같은 `SELECT` 실행 경로로 성능을 비교합니다.

```powershell
.\sqlsprocessor.exe --benchmark-jungle 1000000
```

또는

```bash
make benchmark-jungle
```

측정 항목은 아래와 같습니다.

- `id SELECT via SQL path using B+ tree`
- `email(UK) SELECT via SQL path using B+ tree`
- `phone(UK) SELECT via SQL path using B+ tree`
- `name SELECT via SQL path using linear scan`

비율 출력으로 인덱스 조회가 선형 탐색보다 얼마나 빠른지 비교할 수 있습니다.

```text
linear/id-index average speed ratio: ...
linear/email-index average speed ratio: ...
linear/phone-index average speed ratio: ...
```

`--benchmark-jungle` 은 전용 테이블만 사용하며, 아래 파일 중 하나라도 이미 있으면 삭제하지 않고 안전하게 중단합니다.

- `jungle_benchmark_users.csv`
- `jungle_benchmark_users.delta`
- `jungle_benchmark_users.idx`

## Make Targets

```bash
make demo-bptree
make generate-jungle
make demo-jungle
make scenario-jungle-regression
make scenario-jungle-range-and-replay
make scenario-jungle-update-constraints
make generate-jungle-sql
make benchmark
make benchmark-jungle
```

## Memory Cache Limit

This implementation keeps the first 2,000,000 rows in `TableCache.records` and builds B+ Tree indexes for that cached region. If a CSV has more rows, the first uncached row offset is remembered and tail PK offsets are indexed in memory.

- PK/UK SELECT first checks the B+ Tree index. If a PK key is not cached, exact lookup uses the tail PK offset index before falling back to scan.
- Non-indexed SELECT scans cached rows in memory first, then scans only the uncached CSV tail.
- INSERT beyond the memory limit appends to CSV only and keeps the uncached tail scan path active.
- UPDATE/DELETE on over-limit tables rewrites the CSV through a full-file fallback, then reloads the cache and recomputes the uncached offset.
- UPDATE on cached tables updates only the changed UK B+ Tree entry when the SET column is a UK column.
- UPDATE/DELETE with `WHERE id = ...` uses the PK B+ Tree to locate the target row before applying the change.
- Requests above 2,000,000 rows are capped to 2,000,000 rows to avoid memory blowups.

## Append-Only Delta Log

Cached tables with a PK no longer rewrite the whole CSV for every UPDATE or DELETE. The executor keeps B+ Tree indexes in memory and persists row changes to `<table>.delta`.

- UPDATE writes committed `U` records to the delta log after memory and B+ Tree checks succeed.
- DELETE writes committed `D` tombstone records to the delta log after removing only the deleted PK/UK entries from B+ Tree indexes.
- On table open, the CSV is loaded first, then committed delta batches are replayed, then PK/UK B+ Tree indexes are bulk-built from the current rows.
- Incomplete delta batches are ignored because only records between `B` and `E` markers are replayed.
- If the table later crosses the memory cache limit, pending delta changes are compacted back into the CSV before new tail rows are appended.
- Large delta logs are compacted back into the CSV once they cross `DELTA_COMPACT_BYTES`.

## Stable Slot IDs

The cached prefix no longer treats `record_count` as the number of live rows. It is now the number of allocated slots. Live rows are tracked by `record_active[slot_id]`, and free deleted slots are kept in `free_slots`.
