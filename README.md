# Mini DBMS API Server

기존 C 기반 SQL 처리기를 REST API 서버로 확장한 프로젝트입니다.  

## 한눈에 보기

- API 서버: `src/api`
- 스레드 풀: `src/thr/pool.c`
- DB 엔진(MVCC): `src/db/dbapi.c`, `src/db/mvcc.c`
- 인덱스: `src/legacy/bptree.c`
- 데모 UI: `web/demo.js`, `web/demo.css`

## 빠른 실행

```bash
make build
./bin/dbsrv
```

서버 확인:

```bash
curl http://localhost:8080/api/v1/health
```

Docker 실행:

```bash
docker build -t sqlprocessor:local .
docker run --rm -p 8080:8080 sqlprocessor:local ./bin/dbsrv
```

## API 목록

- `GET /api/v1/health`
- `POST /api/v1/sql`
- `POST /api/v1/batch`
- `POST /api/v1/tx`
- `GET /api/v1/page`
- `GET /api/v1/metrics`

## 프로젝트 특징

- C 기반 SQL 엔진을 REST API 서버로 확장
- API Worker Pool + DB Query Pool 이중 풀 구조
- `/api/v1/page` 병렬 조회 시 trace로 동작 확인 가능
- 트랜잭션 롤백/캐시/부하 시연까지 한 화면 데모 제공
- CSV + B+Tree 기반 엔진을 유지하면서 동시성 처리 강화

## 동시성 정책 (MVCC + Row-level Lock)

### 1) MVCC + Optimistic Commit

- 읽기: snapshot 기준으로 일관성 보장
- 쓰기: private working copy에서 처리 후 commit 시 충돌 검사

### 3) Row-level Write Lock (table + id shard)

- `/api/v1/sql` 단건 쓰기 경로에서 `table + id` 기준 락 샤딩
- 같은 row(id) 쓰기는 직렬화
- 다른 id는 병렬 처리

### 둘을 함께 쓸 때 막는 상황

- MVCC가 막는 것:
  - 읽는 도중 다른 트랜잭션이 커밋해도 “중간 상태가 섞여 보이는 문제”
  - 트랜잭션 단위의 읽기 일관성 붕괴
- Row-level lock이 막는 것:
  - 같은 `table+id`를 동시에 갱신할 때 발생하는 write-write 충돌
  - 동일 row 동시 쓰기에서의 실패 급증

정리하면, **MVCC는 읽기 일관성**, **row-level lock은 동일 row 동시 쓰기 충돌**을 담당합니다.

## 데모 시나리오

### 5단계

- 동시 요청 레벨(8/16/32) 성능 비교

### 6단계 (혼합 부하)

- `SELECT /api/v1/sql`: 50%
- `UPDATE /api/v1/sql`: 30%
- `GET /api/v1/page`: 20%

즉, 읽기 전용이 아니라 **읽기/쓰기 혼합** 상황을 시연합니다.

## 테스트

```bash
make test
```

테스트 파일:

- `tests/test_pool.c`
- `tests/test_mvcc.c`
- `tests/test_dbapi.c`
- `tests/test_tx.c`
- `tests/api_test.sh`
