# 테스트 문서

## 실행 방법

```bash
make test
```

## 단위 테스트

### `tests/test_pool.c`

검증 내용:

- pool 생성/삭제
- 다중 worker 병렬 처리
- queue full 처리
- done 통계 증가
- graceful stop 경로

### `tests/test_mvcc.c`

검증 내용:

- snapshot read consistency
- uncommitted write 비가시성
- commit 후 신규 snapshot 가시성
- write-write conflict abort
- snapshot release 후 GC wait 해소

### `tests/test_dbapi.c`

검증 내용:

- 구조화된 SELECT 결과
- INSERT/UPDATE/DELETE autocommit
- batch read 결과
- unsupported SQL 에러

### `tests/test_tx.c`

검증 내용:

- 성공 tx commit
- 실패 tx rollback
- rollback 후 상태 보존
- 빈 tx 처리
- query 수 제한 초과
- `TX_ABORT` 코드 반환

## API 테스트

```bash
sh tests/api_test.sh
```

검증 항목:

- `/health`
- `/sql`
- `/batch`
- `/tx`
- `/page`
- `/metrics`
- empty body
- bad json
- missing field
- too many batch queries
- too big body
- bad method
- no route

## 부하 테스트

```bash
./scripts/load.sh
```

기본값:

- 요청 수: 100
- 동시성: 10
- 엔드포인트: `/api/v1/page`

## 테스트한 엣지 케이스

- 빈 HTTP body
- 잘못된 JSON
- `query` 누락
- 너무 많은 batch query
- 너무 큰 body
- 존재하지 않는 route
- 지원하지 않는 method
- tx 중간 실패
- rollback 후 상태 유지
- queue full
- snapshot 유지 중 새 commit 발생
