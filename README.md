# Mini DBMS API Server

## 실행 방법

```bash
make
./bin/dbsrv
```

기본 환경변수:

```bash
DBSV_PORT=8080
API_THR=4
DB_THR=4
QUE_MAX=128
MAX_BODY=1048576
MAX_SQL=4096
MAX_QRY=32
DB_ROOT=data
```

## API 목록

- `GET /api/v1/health`
- `POST /api/v1/sql`
- `POST /api/v1/batch`
- `POST /api/v1/tx`
- `GET /api/v1/page`
- `GET /api/v1/metrics`

간단한 확인 예시:

```bash
curl http://localhost:8080/api/v1/health

curl -X POST http://localhost:8080/api/v1/sql \
  -H "Content-Type: application/json" \
  -d '{"query":"SELECT * FROM restaurants WHERE zone = '\''seoul_east'\''"}'

curl "http://localhost:8080/api/v1/page?user_id=1&lat=37.5&lng=127.0"

curl -X POST http://localhost:8080/api/v1/tx \
  -H "Content-Type: application/json" \
  -d '{"queries":["INSERT INTO cart VALUES (9, 9, 1, 7000)","INVALID SQL"]}'

curl http://localhost:8080/api/v1/metrics
```

자세한 명세는 [docs/API.md](/Users/jungilyou/Documents/GitHub/SQL3_WednsdayCodingClub/docs/API.md) 에 정리했습니다.

## Thread Pool 구조

서버는 요청마다 새 스레드를 만들지 않고, 두 개의 고정 크기 풀을 사용합니다.

- API Worker Pool: HTTP 요청 수신 후 라우팅과 응답 생성 담당
- DB Query Pool: `/page`, all-read `/batch` 같은 병렬 조회 SQL 처리 담당

이렇게 분리한 이유는 API worker가 같은 풀에 SQL 작업을 넣고 기다리다가 교착되는 구조를 피하기 위해서입니다.

## MVCC 동시성 제어

table-snapshot copy-on-write MVCC

- 읽기 요청은 시작 시 snapshot id를 잡고 그 시점의 committed version만 봅니다.
- 쓰기 요청은 처음 write가 발생할 때 해당 table의 visible version을 private copy로 복제합니다.
- commit 시점에는 시작 시점의 base version과 현재 head가 같은지 검사합니다.
- 같으면 새 committed version으로 install하고, 다르면 write conflict로 abort합니다.
- rollback은 private working copy 폐기로 처리합니다.

이 방식은 row-chain full MVCC보다 단순하지만, 현재 CSV 엔진 구조와 발표 설명에는 훨씬 잘 맞습니다.

## Transaction rollback 방식

트랜잭션 요청은 private working copy 기반으로 동작합니다.

- 성공: 새 committed table version 설치
- 실패: working copy 폐기
- partial write rollback: 파일 백업 없이 메모리 working copy 폐기로 해결

Durability는 전체 DBMS/WAL 수준이 아니라, 현재 CSV flush가 끝난 시점까지로 제한됩니다.

## 테스트 방법

```bash
make test
```

포함된 테스트:

- `tests/test_pool.c`
- `tests/test_mvcc.c`
- `tests/test_dbapi.c`
- `tests/test_tx.c`
- `tests/api_test.sh`

보조 스크립트:

```bash
./scripts/run.sh
./scripts/load.sh
```

## 차별점

- table-snapshot copy-on-write MVCC로 snapshot consistency와 rollback을 구현한 점
- API Worker Pool과 DB Query Pool을 분리해 deadlock 위험을 낮춘 점
- `/api/v1/page` 와 `/api/v1/metrics` 로 병렬 실행과 내부 상태를 눈으로 보여줄 수 있는 점
