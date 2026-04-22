# 부하테스트 개발 문서

## 1. 목적

이 문서는 SQL 처리기와 API 서버의 부하테스트를 개발, 실행, 해석하기 위한 기준서입니다.

기존 `docs/BENCHMARK_DEVELOPMENT_PLAN_KO.md`가 점수식과 벤치마크 철학을 정리한다면, 이 문서는 실제 개발자가 다음 작업을 할 때 필요한 실행 절차와 확장 지점을 정리합니다.

## 2. 테스트 대상

### 2.1 CLI SQL 처리기

- 실행 파일: `sqlsprocessor`
- 주요 소스: `src/main.c`, `src/executor.c`, `src/bptree.c`
- 벤치 도구:
  - `tools/bench/bench_workload_generator.c`
  - `tools/bench/benchmark_runner.c`
  - `tools/bench/bench_formula_test.c`
- 생성 데이터:
  - `jungle_benchmark_users.csv`
  - `jungle_workload_users.csv`
  - `generated_sql/`
  - `artifacts/bench/`

CLI 부하테스트는 DB 엔진 자체의 처리량을 측정합니다. HTTP, socket, thread pool 비용을 제외하고 B+ Tree 인덱스, CSV append, delta log, row cache 성능을 확인하는 용도입니다.

### 2.2 API 서버

- 실행 파일: `mini_dbms_api`
- 진입점: `src/api_main.c`
- 서버 코드: `src/api/`
- 테스트 스크립트:
  - `tests/api/smoke.sh`
  - `tests/api/stress.sh`

API 부하테스트는 HTTP 요청 처리, bounded queue, worker thread pool, DB wrapper lock, JSON 직렬화까지 포함한 end-to-end 성능과 안정성을 확인합니다.

## 3. 기본 원칙

- 같은 CSV 테이블을 여러 프로세스가 동시에 수정하지 않는다.
- CLI 벤치와 API stress는 동시에 실행하지 않는다.
- 성능 측정 전에 correctness smoke를 먼저 통과시킨다.
- 오류 유도 테스트와 처리량 측정 테스트를 분리한다.
- generated file은 재현 가능해야 하므로 seed와 profile을 기록한다.
- 부하테스트는 성공/실패뿐 아니라 raw metric을 남긴다.

## 4. 결정 사항

| 항목 | 결정 |
| --- | --- |
| E1 기본 테스트 | 수동 curl 대신 `tests/api/smoke.sh`, `tests/api/stress.sh` bash 스크립트를 기본으로 사용한다. |
| E2 동시성 테스트 | 의존성 없는 백그라운드 curl 병렬 실행을 기본으로 한다. 여유가 있으면 `ab`를 보조 측정 도구로 추가하고, `wrk`는 필수로 두지 않는다. |
| E3 유닛 테스트 | 핵심 모듈만 작성한다. 1차 대상은 `http_parser`, `json_builder`, `task_queue`다. |
| E4 Valgrind / 메모리 검증 | 마지막 안정화 단계에서 `valgrind --leak-check=full`을 1회 실행해 leak 중심으로 확인한다. |
| E5 부하 테스트 기준 | 동시 50 요청을 기준 측정값으로 사용한다. 동시 100~200 요청은 stress용으로 사용한다. |

## 5. 사전 준비

Linux, WSL, devcontainer 중 하나를 권장합니다. API 서버는 `pthread`와 POSIX socket을 사용하므로 Windows PowerShell 단독 환경에서는 빌드가 제한될 수 있습니다.

```bash
make clean
make build
make api
make test-api
```

대용량 정글 데이터셋이 없으면 생성합니다.

```bash
make generate-jungle
```

생성된 부하 SQL을 초기화하고 싶으면 아래를 실행합니다.

```bash
make bench-clean
make generate-jungle-sql
```

## 6. CLI 부하테스트

### 6.1 빠른 확인

```bash
make bench-smoke
```

목적:

- 빌드가 되는지 확인
- workload generator가 동작하는지 확인
- correctness success/failure SQL이 기대대로 실행되는지 확인
- 작은 데이터에서 metric 출력 포맷이 깨지지 않는지 확인

### 6.2 점수용 실행

```bash
make bench-score
```

기본 동작:

- `sqlsprocessor` 빌드
- `bench_workload_generator` 빌드
- score profile SQL 생성
- correctness gate 실행
- 반복 측정
- `artifacts/bench/report.md` 생성

### 6.3 리포트만 다시 생성

```bash
make bench-report
```

이미 raw report가 있을 때 Markdown 리포트만 다시 만들 때 사용합니다.

## 7. API 부하테스트

### 7.1 Smoke

```bash
make api
tests/api/smoke.sh
```

확인 항목:

- `POST /query` 정상 SELECT
- INSERT 성공 응답
- `GET /query`가 `405`를 반환
- 잘못된 path가 `404`를 반환
- 모든 응답이 JSON 형태인지 확인

### 7.2 기본 Stress

```bash
make api
CONCURRENCY=50 tests/api/stress.sh
```

`tests/api/stress.sh`는 서버를 직접 띄운 뒤 병렬 curl 요청을 보냅니다. 기본 요청은 SELECT, INSERT, UPDATE, DELETE를 섞습니다.

부하를 키울 때:

```bash
CONCURRENCY=100 tests/api/stress.sh
CONCURRENCY=200 tests/api/stress.sh
```

주의:

- 현재 DB wrapper는 모든 SQL 실행을 global write lock으로 보호합니다.
- 따라서 API stress의 목표는 DB 병렬 실행이 아니라 queue, worker, HTTP 처리 안정성 확인입니다.
- DB 처리량 자체는 CLI 벤치로 따로 봅니다.

## 8. 권장 프로필

| Profile | 대상 | 데이터 크기 | 동시성 | 목적 |
| --- | --- | ---: | ---: | --- |
| smoke | CLI/API | 10,000 rows | 1~50 | 빠른 개발 확인 |
| regression | CLI | 100,000 rows | 1 | 기능 회귀와 성능 급락 확인 |
| api-regression | API | sample CSV | 50~100 | worker/queue/JSON 안정성 |
| score | CLI | 1,000,000 rows | 1 | 발표용 DB 엔진 성능 측정 |
| soak | API | sample or generated CSV | 50 | 장시간 안정성 확인 |

## 9. 측정 지표

### 9.1 CLI

반드시 기록할 지표:

- insert ops/sec
- id SELECT ops/sec
- email UK SELECT ops/sec
- phone UK SELECT ops/sec
- scan SELECT ops/sec
- update ops/sec
- delete ops/sec 또는 estimated 여부
- peak heap requested bytes
- 최종 score
- seed
- profile
- commit hash

### 9.2 API

최소 기록 지표:

- total requests
- success count
- error count
- HTTP status 분포
- total elapsed seconds
- requests/sec
- p50 latency
- p95 latency
- p99 latency
- max latency
- queue full로 인한 `503` 발생 수
- 서버 crash 여부

현재 `tests/api/stress.sh`는 간단한 pass/fail 수준입니다. 다음 단계에서는 latency와 status 분포를 남기는 runner로 확장합니다.

## 10. API Stress Runner 확장안

현재 shell script를 유지하되, 아래 기능을 추가하는 전용 스크립트를 권장합니다.

권장 파일:

```text
scripts/run_api_load_test.py
```

입력 옵션:

```text
--url http://127.0.0.1:8080/query
--concurrency 50
--requests 1000
--mix read-heavy
--seed 20260415
--timeout-sec 5
--report artifacts/api-load/report.json
```

지원 mix:

| Mix | SELECT | INSERT | UPDATE | DELETE | 용도 |
| --- | ---: | ---: | ---: | ---: | --- |
| read-heavy | 80 | 10 | 8 | 2 | 일반 조회 중심 |
| balanced | 55 | 20 | 20 | 5 | CRUD 혼합 |
| write-heavy | 30 | 35 | 25 | 10 | lock/queue 압박 |
| select-only | 100 | 0 | 0 | 0 | HTTP read path 기준선 |

출력 JSON 예시:

```json
{
  "profile": "api-regression",
  "seed": 20260415,
  "concurrency": 50,
  "requests": 1000,
  "success": 1000,
  "errors": 0,
  "requests_per_sec": 320.5,
  "latency_ms": {
    "p50": 12.1,
    "p95": 41.7,
    "p99": 88.4,
    "max": 132.0
  },
  "status_counts": {
    "200": 1000
  }
}
```

## 11. 실패 기준

아래 중 하나라도 발생하면 부하테스트 실패로 본다.

- 서버 프로세스 crash
- curl 또는 runner connection error
- JSON이 아닌 응답
- API smoke에서 `404`, `405`, `400` 기대 케이스 불일치
- correctness gate 실패
- 부하테스트 후 같은 SQL이 재실행 불가능한 데이터 오염
- `503` 비율이 목표 기준을 초과
- p95 latency가 이전 기준 대비 2배 이상 증가

초기 기준은 완화해서 잡고, 팀 기준선이 생기면 수치화합니다.

예시 초기 기준:

```text
api-regression:
  success_rate >= 99%
  503_rate <= 1%
  p95_latency_ms <= 250

score:
  correctness == pass
  report.md generated
  no process crash
```

## 12. 산출물 위치

권장 디렉터리:

```text
artifacts/
├── bench/
│   ├── report.raw
│   └── report.md
└── api-load/
    ├── report.json
    ├── report.md
    └── server.log
```

`artifacts/`는 결과물이므로 Git에 커밋하지 않습니다.

## 13. 개발 순서

1. `make test-api`를 안정화한다.
2. `tests/api/stress.sh`의 SQL shape를 현재 CSV schema와 맞춘다.
3. API stress 결과에 elapsed time과 status count를 추가한다.
4. `scripts/run_api_load_test.py`를 만든다.
5. `artifacts/api-load/report.json`과 `report.md`를 생성한다.
6. CI 또는 수동 체크리스트에 smoke/regression profile을 추가한다.
7. score profile은 발표/최종 측정 때만 실행한다.

## 14. 구현 시 주의할 점

- DB lock 안에서 network write를 하지 않는다.
- queue mutex를 잡은 상태에서 DB lock을 잡지 않는다.
- worker thread가 처리한 fd는 반드시 close한다.
- response write는 partial write를 고려한다.
- stress script가 INSERT한 sample row는 재실행 가능한 id range를 사용한다.
- 부하테스트 전후로 `.delta`, `.idx`, `.tmp` 잔여물을 확인한다.
- sample CSV를 수정하는 테스트는 끝난 뒤 원상복구되도록 설계한다.

## 15. 완료 기준

부하테스트 개발은 아래 상태를 만족하면 1차 완료로 봅니다.

- `make bench-smoke`가 성공한다.
- `make bench-score`가 `artifacts/bench/report.md`를 만든다.
- `make api`가 성공한다.
- `tests/api/smoke.sh`가 성공한다.
- `CONCURRENCY=50 tests/api/stress.sh`가 성공한다.
- API stress report가 `artifacts/api-load/report.json`으로 저장된다.
- README 또는 docs에서 실행 방법을 찾을 수 있다.
