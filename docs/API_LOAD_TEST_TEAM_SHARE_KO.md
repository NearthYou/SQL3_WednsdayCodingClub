# API 서버 및 부하테스트 팀 공유 문서

## 1. 요약

이번 작업은 기존 C 기반 SQL 처리기 위에 HTTP API 서버를 얹고, 팀원이 같은 기준으로 기능/부하 테스트를 재현할 수 있도록 테스트 runner와 결과 문서를 정리한 것이다.

핵심 결과:

- 기존 CLI 실행 파일 `sqlsprocessor` 빌드 성공
- API 서버 실행 파일 `mini_dbms_api` 빌드 성공
- API 유닛 테스트 3종 통과
- API smoke 통과
- API 동시성 stress 50/100/200 요청 통과
- API load test 1,000 요청, 동시성 50, read-heavy mix 통과
- CLI `bench-smoke` 통과 및 `artifacts/bench/report.md` 생성

## 2. 테스트 실행 환경

테스트는 Windows 호스트에서 WSL Ubuntu 24.04를 사용해 실행했다.

설치한 주요 도구:

- `build-essential`
- `make`
- `gcc`
- `curl`
- `valgrind`
- `python3`

확인한 실행 경로:

```bash
cd /mnt/c/Users/KJ/Workspace/WEEK8-wend
make
make api
make test-api
bash tests/api/smoke.sh
CONCURRENCY=50 bash tests/api/stress.sh
CONCURRENCY=100 bash tests/api/stress.sh
CONCURRENCY=200 bash tests/api/stress.sh
CONCURRENCY=50 REQUESTS=1000 MIX=read-heavy bash tests/api/load_report.sh
make bench-smoke
```

## 3. 프로그램 구조

### 기존 CLI 엔진

기존 SQL 처리기는 그대로 유지한다.

- `src/main.c`: SQL 파일 입력, benchmark 옵션, CLI 진입점
- `src/parser.c`, `src/lexer.c`: SQL 문자열을 `Statement`로 변환
- `src/executor.c`: CSV 테이블 캐시, B+Tree 인덱스, delta log, CRUD 실행
- `src/bptree.c`: PK/UK 인덱스용 B+Tree 구현

### API 서버

API 서버는 기존 CLI와 별도 실행 파일로 분리했다.

- `src/api_main.c`: server lifecycle, signal, listener, worker pool orchestration
- `src/api/net`: listener socket, HTTP/1.1 parser
- `src/api/pool`: bounded task queue, thread pool
- `src/api/handler`: connection 단위 request/response 처리
- `src/api/db`: DB wrapper, 전역 DB lock, `DbResult` 생성
- `src/api/json`: JSON response builder
- `src/api/log`: mutex 기반 공용 로그

### 테스트/부하 도구

- `tests/api/test_http_parser.c`: HTTP parser 유닛 테스트
- `tests/api/test_json_builder.c`: JSON builder 유닛 테스트
- `tests/api/test_task_queue.c`: queue 유닛 테스트
- `tests/api/smoke.sh`: API 기본 정상/오류 흐름 확인
- `tests/api/stress.sh`: curl 기반 동시 요청 확인
- `tests/api/load_report.sh`: 임시 CSV 복사본으로 서버 실행 후 부하 리포트 생성
- `scripts/run_api_load_test.py`: latency/status/RPS를 수집하는 Python 부하 runner

## 4. 구현 특징

### HTTP API

- 엔드포인트는 `POST /query` 하나만 제공한다.
- request body에는 SQL 1문장만 허용한다.
- `Content-Type: text/plain`만 허용한다.
- response는 항상 JSON이며 `Connection: close`를 사용한다.
- keep-alive, chunked transfer, pipelining은 v1 범위에서 제외했다.

### 동시성 모델

- listener thread가 accept 후 bounded queue에 fd를 넣는다.
- worker thread pool이 queue에서 fd를 꺼내 요청을 처리한다.
- queue capacity는 256이다.
- worker 수는 현재 16개로 고정했다.
- queue full 또는 shutdown 중 enqueue 실패 시 listener가 직접 `503`을 반환한다.

### DB 보호 정책

- 기존 executor는 `open_tables`, row/page cache, shared `FILE *` 등 mutable 전역 상태를 사용한다.
- 따라서 v1 API 서버에서는 모든 SQL 실행을 전역 `pthread_rwlock_wrlock`으로 보호한다.
- DB lock 안에서는 SQL 실행과 `DbResult` deep copy만 수행한다.
- JSON 직렬화와 network write는 DB lock 밖에서 수행한다.

### SELECT 결과 반환

- stdout 캡처를 쓰지 않고 executor에 `EngineRowSink` callback을 추가했다.
- `SELECT *`는 테이블 전체 컬럼명을 JSON key로 사용한다.
- `SELECT col1, col2`는 선택된 컬럼명만 JSON key로 사용한다.
- 모든 값은 JSON 문자열로 직렬화한다.

## 5. 테스트 결과

### 빌드

| 항목 | 결과 |
| --- | --- |
| `make` | PASS |
| `make api` | PASS |
| 컴파일 경고 | 있음. 기존 parser/executor/bptree 경고 중심 |

API 빌드 중 경고는 있었지만 실행 파일은 생성됐다. 주요 경고는 기존 코드의 `strncpy` truncation 가능성, unused static function, `MAX_TABLES=1` 관련 array bounds 경고다.

### API 유닛 테스트

| 테스트 | 결과 |
| --- | --- |
| `test_http_parser` | PASS |
| `test_json_builder` | PASS |
| `test_task_queue` | PASS |

Valgrind 결과:

| 테스트 | leak | error |
| --- | ---: | ---: |
| `test_http_parser` | 0 bytes | 0 |
| `test_json_builder` | 0 bytes | 0 |
| `test_task_queue` | 0 bytes | 0 |

### API Smoke

명령:

```bash
bash tests/api/smoke.sh
```

결과:

```text
smoke.sh: OK
```

확인한 항목:

- `POST /query` SELECT 정상 응답
- INSERT 정상 응답
- `GET /query`는 `405`
- `/wrong`은 `404`
- response body는 JSON 형식

### API Stress

명령:

```bash
CONCURRENCY=50 bash tests/api/stress.sh
CONCURRENCY=100 bash tests/api/stress.sh
CONCURRENCY=200 bash tests/api/stress.sh
```

결과:

```text
stress.sh: OK (concurrency=50)
stress.sh: OK (concurrency=100)
stress.sh: OK (concurrency=200)
```

요약:

| 동시성 | 요청 mix | 결과 | 비고 |
| ---: | --- | --- | --- |
| 50 | SELECT / INSERT / UPDATE / DELETE 혼합 | PASS | 기본 기준 |
| 100 | SELECT / INSERT / UPDATE / DELETE 혼합 | PASS | 추가 stress |
| 200 | SELECT / INSERT / UPDATE / DELETE 혼합 | PASS | 추가 stress |
| 100/200 동시 실행 | 서로 다른 stress script 동시 실행 | FAIL | 둘 다 8080 포트를 직접 bind하므로 순차 실행 필요 |

stress script는 임시 디렉터리에 `case_basic_users.csv` 복사본을 만들고 서버를 실행하므로 tracked sample CSV를 오염시키지 않는다.

주의: stress script는 내부에서 API 서버를 직접 띄우므로 50/100/200을 동시에 실행하면 8080 포트 충돌이 난다. 순차 실행해야 한다.

### API Load Test

명령:

```bash
CONCURRENCY=50 REQUESTS=1000 MIX=read-heavy bash tests/api/load_report.sh
```

결과 파일:

- `artifacts/api-load/report.json`
- `artifacts/api-load/report.md`
- `artifacts/api-load/server.log`

주요 수치:

| 지표 | 값 |
| --- | ---: |
| profile | api-regression |
| mix | read-heavy |
| concurrency | 50 |
| requests | 1000 |
| success | 1000 |
| errors | 0 |
| status 200 | 1000 |
| requests/sec | 1331.76 |
| p50 latency | 32.01 ms |
| p95 latency | 50.20 ms |
| p99 latency | 58.68 ms |
| max latency | 84.98 ms |

초기 기준과 비교:

| 기준 | 목표 | 결과 |
| --- | ---: | ---: |
| success rate | >= 99% | 100% |
| 503 rate | <= 1% | 0% |
| p95 latency | <= 250 ms | 50.20 ms |

### API Load Test - 16 Worker 재측정

worker thread 수를 CPU 기반 `sysconf(_SC_NPROCESSORS_ONLN) * 2`에서 16개 고정으로 바꾼 뒤 같은 read-heavy 조건으로 다시 측정했다.

명령:

```bash
PORT=18086 CONCURRENCY=50 REQUESTS=1000 MIX=read-heavy REPORT=artifacts/api-load/report-workers16-c50-r1000.json bash tests/api/load_report.sh
PORT=18087 CONCURRENCY=100 REQUESTS=1000 MIX=read-heavy REPORT=artifacts/api-load/report-workers16-c100-r1000.json bash tests/api/load_report.sh
PORT=18088 CONCURRENCY=200 REQUESTS=1000 MIX=read-heavy REPORT=artifacts/api-load/report-workers16-c200-r1000.json bash tests/api/load_report.sh
```

결과 파일:

- `artifacts/api-load/report-workers16-c50-r1000.json`
- `artifacts/api-load/report-workers16-c50-r1000.md`
- `artifacts/api-load/report-workers16-c100-r1000.json`
- `artifacts/api-load/report-workers16-c100-r1000.md`
- `artifacts/api-load/report-workers16-c200-r1000.json`
- `artifacts/api-load/report-workers16-c200-r1000.md`

기존 기준과 16 worker의 같은 조건 비교:

| 지표 | 기존 기준 | 16 worker | 변화 |
| --- | ---: | ---: | ---: |
| concurrency | 50 | 50 | - |
| requests | 1000 | 1000 | - |
| success | 1000 | 1000 | 동일 |
| errors | 0 | 0 | 동일 |
| status 200 | 1000 | 1000 | 동일 |
| requests/sec | 1331.76 | 1196.92 | -10.1% |
| p50 latency | 32.01 ms | 33.72 ms | +5.3% |
| p95 latency | 50.20 ms | 57.31 ms | +14.2% |
| p99 latency | 58.68 ms | 64.51 ms | +9.9% |
| max latency | 84.98 ms | 81.52 ms | -4.1% |

16 worker 동시성별 결과:

| concurrency | requests | success | errors | requests/sec | p50 latency | p95 latency | p99 latency | max latency |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 50 | 1000 | 1000 | 0 | 1196.92 | 33.72 ms | 57.31 ms | 64.51 ms | 81.52 ms |
| 100 | 1000 | 1000 | 0 | 1252.86 | 44.28 ms | 85.52 ms | 102.72 ms | 136.45 ms |
| 200 | 1000 | 1000 | 0 | 1231.94 | 18.30 ms | 65.70 ms | 80.40 ms | 102.66 ms |

해석:

- 16 worker로 줄인 뒤에도 세 조건 모두 `1000/1000` 성공, HTTP 503 및 runner error는 0건이었다.
- 같은 concurrency 50 기준 RPS는 약 10.1% 낮아졌지만 p95는 57.31 ms로 목표 기준 250 ms 대비 여유가 크다.
- concurrency 100/200에서도 p95가 100 ms 이하로 유지되어 현재 read-heavy mix에서는 worker 16개가 병목으로 드러나지 않았다.
- concurrency 200의 p50이 concurrency 100보다 낮게 나온 것은 1000 요청 규모의 짧은 측정에서 스케줄링과 요청 분포 영향이 섞인 결과로 보고, 장시간 soak test에서는 더 큰 request 수로 재측정하는 편이 좋다.

### CLI Bench Smoke

명령:

```bash
make bench-smoke
```

결과 파일:

- `artifacts/bench/report.json`
- `artifacts/bench/report.md`

주요 수치:

| 지표 | 값 |
| --- | ---: |
| profile | smoke |
| seed | 20260415 |
| repeat | 3 |
| correctness | pass |
| score | 83.96 / 100 |
| SELECT id | 14667.06 kops |
| SELECT UK email | 5757.05 kops |
| SELECT scan | 47.62 kops |
| INSERT | 49.94 kops |
| UPDATE | 38.68 kops |
| DELETE | 154.83 kops |
| peak process memory | 105807872 bytes |
| peak heap requested | 7389011 bytes |

## 6. 비교 시 잘 살릴 만한 부분

다른 팀원 생성물과 비교할 때 아래 부분은 유지 가치가 높다.

- API 서버와 CLI 엔진을 실행 파일 수준에서 분리해 기존 데모/벤치를 깨지 않는다.
- API stress/load 테스트가 tracked CSV를 직접 수정하지 않고 임시 CSV 복사본으로 실행된다.
- shell pass/fail stress와 Python metric runner를 분리해 빠른 확인과 수치 측정을 모두 지원한다.
- `artifacts/api-load/report.json`과 `report.md`를 동시에 생성해 자동 처리와 사람이 읽는 리뷰가 모두 가능하다.
- SELECT 결과 반환을 stdout 캡처가 아니라 executor callback으로 처리해 API 응답 구조가 명확하다.
- DB lock 범위를 보수적으로 잡아 기존 executor의 전역 상태와 shared file pointer 위험을 피했다.

## 7. 남은 리스크와 개선 후보

- API 서버의 DB 실행은 v1에서 global write lock이므로 DB 작업 자체는 병렬화되지 않는다.
- compiler warning은 남아 있다. 특히 기존 parser의 `strncpy` 경고와 executor의 unused variable/function 경고는 발표 전 정리하면 좋다.
- API runner는 서버를 직접 시작하지 않는 기본 모드와, `tests/api/load_report.sh`를 통한 서버 시작 모드가 나뉘어 있다.
- 장시간 soak test와 서버 프로세스 전체 valgrind는 아직 별도 실행하지 않았다.
- `affected_rows`, `inserted_id`는 내부에서 받을 수 있지만 v1 JSON 응답에는 포함하지 않는다.

## 8. 재현 명령 모음

```bash
# environment
wsl -d Ubuntu -u root -- bash -lc "cd /mnt/c/Users/KJ/Workspace/WEEK8-wend && make --version && gcc --version"

# build
wsl -d Ubuntu -u root -- bash -lc "cd /mnt/c/Users/KJ/Workspace/WEEK8-wend && make && make api"

# api unit
wsl -d Ubuntu -u root -- bash -lc "cd /mnt/c/Users/KJ/Workspace/WEEK8-wend && make test-api"

# api smoke/stress/load
wsl -d Ubuntu -u root -- bash -lc "cd /mnt/c/Users/KJ/Workspace/WEEK8-wend && bash tests/api/smoke.sh"
wsl -d Ubuntu -u root -- bash -lc "cd /mnt/c/Users/KJ/Workspace/WEEK8-wend && CONCURRENCY=50 bash tests/api/stress.sh"
wsl -d Ubuntu -u root -- bash -lc "cd /mnt/c/Users/KJ/Workspace/WEEK8-wend && CONCURRENCY=100 bash tests/api/stress.sh"
wsl -d Ubuntu -u root -- bash -lc "cd /mnt/c/Users/KJ/Workspace/WEEK8-wend && CONCURRENCY=200 bash tests/api/stress.sh"
wsl -d Ubuntu -u root -- bash -lc "cd /mnt/c/Users/KJ/Workspace/WEEK8-wend && CONCURRENCY=50 REQUESTS=1000 MIX=read-heavy bash tests/api/load_report.sh"

# cli bench
wsl -d Ubuntu -u root -- bash -lc "cd /mnt/c/Users/KJ/Workspace/WEEK8-wend && make bench-smoke"
```
