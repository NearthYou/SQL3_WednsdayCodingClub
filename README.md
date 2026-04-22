# Mini DBMS API Server

기존 C 기반 SQL 처리기를 REST API 서버로 확장한 프로젝트입니다.

## 핵심 구성

- API 서버: `src/api`
- 스레드 풀: `src/thr/pool.c`
- DB 엔진: `src/db/dbapi.c`, `src/db/mvcc.c`
- 인덱스: `src/legacy/bptree.c`
- 데모 페이지: `web/demo.js`, `web/demo.css`

## 현재 동시성 정책 (중요)

### 1) MVCC + optimistic commit

- 읽기: snapshot 기준으로 일관성 보장
- 쓰기: private working copy에 반영 후 commit 시 충돌 검사

### 2) write retry

- 쓰기 충돌 시 backoff 재시도
- 설정 위치: `src/db/dbapi.c`
  - `WRITE_RETRY_MAX`
  - `WRITE_RETRY_BASE_US`
  - `WRITE_RETRY_CAP_US`

### 3) row-level write lock (table + id shard lock)

- `/api/v1/sql` 단건 쓰기 경로에서 `table + id` 기준으로 락 샤딩
- 같은 row(id) 동시 쓰기는 직렬화
- 다른 id는 병렬 처리
- 목적: 동시 쓰기 성공률 개선 (충돌성 쓰기 테스트에서 100% 성공 확인)

## API

- `GET /api/v1/health`
- `POST /api/v1/sql`
- `POST /api/v1/batch`
- `POST /api/v1/tx`
- `GET /api/v1/page`
- `GET /api/v1/metrics`

## 데모

웹 데모는 단계별 시나리오를 제공합니다.

- 5단계: 동시 요청 레벨(8/16/32) 비교
- 6단계: 읽기/쓰기 혼합 부하
  - `SELECT /api/v1/sql` 50%
  - `UPDATE /api/v1/sql` 30%
  - `GET /api/v1/page` 20%

## 실행

```bash
make build
./bin/dbsrv
```

또는 Docker:

```bash
docker build -t sqlprocessor:local .
docker run --rm -p 8080:8080 sqlprocessor:local ./bin/dbsrv
```

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

