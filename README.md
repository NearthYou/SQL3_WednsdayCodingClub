# Mini DBMS API Server

> C로 만든 SQL 처리기를 여러 클라이언트가 사용할 수 있는 트랜잭션 API 서버로 확장한 프로젝트

기존 SQL 처리기에 HTTP 요청이 동시에 들어오면 요청 처리와 DB 작업이 서로 막힐 수 있습니다. 트랜잭션 도중 오류가 났을 때 원본 데이터가 일부만 바뀌는 문제도 막아야 합니다. 이 프로젝트는 POSIX socket과 pthread 위에 API 계층, 이중 스레드 풀, snapshot 기반 트랜잭션을 추가해 이 문제를 다룹니다.

## 요청 처리 구조

```text
Client
→ HTTP API
→ API Worker Pool
→ Route / DB Adapter
→ DB Query Pool
→ SQL Engine
→ CSV Table · B+ Tree · WAL
```

API 요청을 받는 풀과 병렬 SQL을 실행하는 풀을 분리했습니다. 무거운 쿼리가 API worker를 모두 점유하거나 같은 풀을 다시 기다리는 상황을 피하기 위한 선택입니다.

## 구현 범위

| 영역 | 구현 내용 |
| --- | --- |
| API | POSIX socket 기반 HTTP server와 JSON 응답 |
| 동시성 | 고정 크기 API pool, DB query pool, row-level write lock |
| 읽기 | table-snapshot copy-on-write MVCC |
| 트랜잭션 | private working copy, rollback, commit conflict detection |
| 저장 | CSV table, PK·UK B+ Tree, WAL recovery |
| 관찰 | health, metrics, 병렬 조회 trace |

## 트랜잭션 기준

트랜잭션은 시작 시점의 table snapshot을 읽습니다. 쓰기는 원본이 아니라 private working copy에 적용합니다. 모든 SQL이 성공하고 충돌 검사까지 통과해야 원본에 반영됩니다. 중간 실패나 충돌이 있으면 working copy를 버리므로 원본은 바뀌지 않습니다.

```text
snapshot 획득
→ private working copy에서 SQL 실행
→ commit 직전 version conflict 검사
→ 성공: WAL 기록 후 반영
→ 실패: working copy 폐기
```

### 현재 MVCC의 범위

이 구현은 row-version chain을 두는 일반적인 MVCC가 아니라 table-snapshot COW 방식입니다. 따라서 충돌도 table 단위로 발생할 수 있습니다.

`mv_gc`는 종료된 snapshot을 active snapshot 목록에서 제거합니다. 오래된 row version을 회수하는 함수가 아니며 현재 구현에는 row-version reclamation이 없습니다. `gc_wait`도 남아 있는 active snapshot과 현재 version의 차이를 나타내는 상태값입니다.

## API

| Method | Endpoint | 역할 |
| --- | --- | --- |
| `GET` | `/api/v1/health` | 서버 상태 확인 |
| `POST` | `/api/v1/sql` | SQL 한 건 실행 |
| `POST` | `/api/v1/batch` | 여러 SQL 일괄 실행 |
| `POST` | `/api/v1/tx` | 여러 SQL을 하나의 트랜잭션으로 실행 |
| `GET` | `/api/v1/page` | 병렬 조회와 trace 반환 |
| `GET` | `/api/v1/metrics` | pool과 MVCC 상태 확인 |

## 실행

POSIX socket과 pthread를 사용하므로 Linux 또는 WSL 환경이 필요합니다.

```bash
make build
./bin/dbsrv
```

```bash
curl http://localhost:8080/api/v1/health

curl -X POST http://localhost:8080/api/v1/sql \
  -H "Content-Type: application/json" \
  -d '{"sql":"SELECT * FROM users"}'
```

Docker로도 실행할 수 있습니다.

```bash
docker build -t mini-dbms .
docker run --rm -p 8080:8080 mini-dbms ./bin/dbsrv
```

## 검증

```bash
make test
```

`make test`는 thread pool, MVCC 상태, DB API, transaction, HTTP API 시나리오를 실행합니다.

부하 테스트와 세부 설계는 다음 문서에 정리되어 있습니다.

- [아키텍처](./docs/ARCH.md)
- [API](./docs/API.md)
- [설계 결정](./docs/DECISIONS.md)
- [테스트](./docs/TEST.md)
