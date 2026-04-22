# Mini DBMS API Server 개발 문서

## 0. 검토 결과

제안된 구현 명세의 큰 방향은 현재 프로젝트에 적용 가능하다. HTTP/1.1, `POST /query`, thread pool, bounded queue, JSON 응답, graceful shutdown이라는 범위도 이번 일정에 맞다.

다만 현재 저장소 구조와 엔진 구현을 기준으로 아래 사항은 보정해서 개발한다.

- 기존 엔진은 `engine/`이 아니라 `src/` 루트에 있다. 이번 개발에서는 파일 이동을 최소화하고, API 서버 코드를 `src/api/` 아래에 새로 추가한다.
- 현재 CLI 진입점인 `src/main.c`는 유지한다. API 서버는 별도 실행 파일 `mini_dbms_api`로 만든다.
- 현재 executor는 결과를 구조체로 반환하지 않고 stdout에 출력한다. API 서버에서는 stdout 캡처가 아니라 `DbResult`로 결과를 받는 엔진 wrapper API를 추가한다.
- 현재 SELECT 경로도 테이블 캐시, LRU, lazy row cache, shared `FILE *` offset 등을 변경할 수 있다. 따라서 1차 구현에서는 모든 SQL 실행을 전역 DB write lock 아래에서 처리하고, SELECT read lock 병렬화는 executor read path가 안전해진 뒤 켠다.
- `pthread`, POSIX socket, `valgrind`를 기준으로 하므로 API 서버 빌드/테스트 대상은 Linux, WSL, devcontainer 중 하나로 둔다.

## 1. 목표

기존 C 기반 SQL 처리기와 B+Tree 인덱스 엔진을 HTTP 클라이언트가 사용할 수 있도록 API 서버로 감싼다.

핵심 목표는 다음과 같다.

- 기존 CSV 기반 미니 DBMS 엔진을 API 서버 내부에 임베디드 방식으로 연결한다.
- HTTP 요청 body에 담긴 SQL 1문장을 실행한다.
- 결과를 JSON으로 반환한다.
- Thread pool과 bounded request queue로 동시 연결을 처리한다.
- 전역 DB lock으로 기존 엔진의 전역 상태와 파일 접근을 보호한다.
- 기본 기능 안정성과 테스트 가능성을 우선한다.

## 2. 이번 범위

### 포함

- HTTP/1.1 서버
- `POST /query` 단일 엔드포인트
- `Content-Type: text/plain` SQL body 처리
- Blocking socket + thread pool
- 배열 기반 순환 task queue
- DB engine wrapper
- JSON 응답 builder
- 공용 logging
- SIGINT/SIGTERM graceful shutdown
- 기본 API 테스트, 동시성 테스트, 핵심 모듈 단위 테스트

### 제외

- Keep-Alive
- chunked transfer encoding
- HTTP pipelining
- HTTP/2 이상
- query-level parallelism
- sharding
- query cache
- prepared statement
- transaction
- connection pool
- `/stats` 엔드포인트
- adaptive thread pool
- 별도 차별화 기능

## 3. 저장소 적용 구조

현재 `src/`의 엔진 파일은 그대로 유지하고 API 서버를 옆에 추가한다.

```text
src/
├── main.c                  # 기존 CLI 진입점, 유지
├── lexer.c
├── lexer.h
├── parser.c
├── parser.h
├── executor.c
├── executor.h
├── bptree.c
├── bptree.h
├── types.h
├── api_main.c              # 새 API 서버 진입점
└── api/
    ├── net/
    │   ├── listener.c
    │   ├── listener.h
    │   ├── http_parser.c
    │   └── http_parser.h
    ├── pool/
    │   ├── thread_pool.c
    │   ├── thread_pool.h
    │   ├── task_queue.c
    │   └── task_queue.h
    ├── handler/
    │   ├── request_handler.c
    │   └── request_handler.h
    ├── db/
    │   ├── db_wrapper.c
    │   └── db_wrapper.h
    ├── json/
    │   ├── json_builder.c
    │   └── json_builder.h
    └── log/
        ├── log.c
        └── log.h
tests/
└── api/
    ├── test_http_parser.c
    ├── test_json_builder.c
    ├── test_task_queue.c
    ├── smoke.sh
    └── stress.sh
```

추후 구조 정리가 필요하면 엔진 파일을 `src/engine/`으로 옮길 수 있지만, 이번 구현의 1차 목표는 기존 CLI 기능을 깨지 않고 API 서버를 추가하는 것이다.

## 4. 빌드 정책

기존 CLI 타깃은 유지한다.

- `make` 또는 `make build`: 기존 `sqlsprocessor`
- `make api`: API 서버 `mini_dbms_api`
- `make run-api`: API 서버 실행
- `make test-api`: API 단위/통합 테스트 실행
- `make clean`: 기존 산출물과 API 산출물 정리

API 서버 컴파일 플래그는 아래를 기본값으로 한다.

```text
-Wall -Wextra -O2 -g -pthread
```

API 서버 타깃은 `src/main.c`를 컴파일하지 않는다. `src/main.c`는 파일 끝에서 `lexer.c`, `parser.c`, `bptree.c`, `executor.c`를 직접 include하므로 API 타깃에 섞으면 중복 심볼이 생길 수 있다.

## 5. HTTP API

### Endpoint

```text
POST /query
```

### Request

```http
POST /query HTTP/1.1
Host: localhost:8080
Content-Type: text/plain
Content-Length: 43
Connection: close

SELECT * FROM case_basic_users WHERE id = 1;
```

규칙:

- body에는 SQL 1문장만 담는다.
- `Content-Length`는 필수다.
- `Content-Type`은 `text/plain`만 정상 요청으로 본다.
- SQL 최대 길이는 우선 기존 `MAX_SQL_LEN`에 맞춘다.
- 세미콜론은 있어도 되고 없어도 되지만, 여러 문장은 받지 않는다.

### Success Response

```http
HTTP/1.1 200 OK
Content-Type: application/json
Connection: close
Content-Length: ...

{"status":"ok","rows":[{"id":"1","email":"admin@test.com"}]}
```

DML 성공 시에도 같은 형태를 유지한다.

```json
{"status":"ok","rows":[]}
```

### Error Response

```http
HTTP/1.1 400 Bad Request
Content-Type: application/json
Connection: close
Content-Length: ...

{"status":"error","message":"invalid request"}
```

### HTTP Status

- `200 OK`: SQL 정상 실행
- `400 Bad Request`: 잘못된 HTTP 요청, 빈 SQL, 잘못된 body, 지원하지 않는 포맷
- `404 Not Found`: `/query` 외 경로
- `405 Method Not Allowed`: `POST` 외 메서드
- `413 Payload Too Large`: 요청 body 또는 응답 JSON 한도 초과
- `500 Internal Server Error`: 서버 내부 오류, DB 엔진 오류, 메모리 할당 실패
- `503 Service Unavailable`: 작업 큐가 가득 참, shutdown 중 새 요청 거절

## 6. 네트워크 설계

- Protocol: HTTP/1.1
- Port: `8080`
- I/O model: blocking socket + thread pool
- `listen()` backlog: `128`
- `SO_REUSEADDR`: on
- Keep-Alive: 미지원
- 응답 헤더: 항상 `Connection: close`
- 연결 모델: 1 request = 1 connection

HTTP parser는 이번 범위에 필요한 최소 기능만 구현한다.

- request line 파싱
- method/path/version 검증
- header 파싱
- `Content-Length` 파싱
- header와 함께 이미 읽힌 body fragment 처리
- 남은 body는 `Content-Length`만큼 루프 read
- chunked, gzip, multipart, pipelining은 거절

요청 body가 `MAX_SQL_LEN`을 넘으면 DB wrapper까지 넘기지 않고 `413`을 반환한다.

## 7. Thread Pool과 Queue

### Thread Pool

- 서버 시작 시 worker를 일괄 생성한다.
- 기본 스레드 수는 `cores * 2`로 한다.
- 로컬 8코어 기준 기본값은 16개다.
- worker는 queue에서 connection fd를 꺼내 request handler에 넘긴다.
- 종료 시 shutdown flag를 세우고 condition broadcast 후 join한다.

### Task Queue

- 배열 기반 순환 큐
- queue capacity: 256
- 동기화: `pthread_mutex_t`, `pthread_cond_t`
- enqueue 성공 시 `pthread_cond_signal`
- shutdown 시 `pthread_cond_broadcast`
- FIFO 외 별도 fairness 보장은 하지 않는다.

큐가 가득 찬 경우 listener thread가 직접 `503 Service Unavailable` 응답을 쓰고 fd를 닫는다. 큐에 넣지 못한 fd를 worker가 처리할 수 없기 때문이다.

## 8. DB 동시성 제어

### 1차 구현 정책

전역 `pthread_rwlock_t`를 사용하되, 모든 SQL 실행은 write lock으로 보호한다.

이유:

- 현재 `open_tables`, `open_table_count`, table LRU가 전역 mutable 상태다.
- `SELECT`도 lazy table load, row cache, page cache, LRU, shared `FILE *` 위치를 변경할 수 있다.
- shared `FILE *`에 대한 동시 `fseek`/read는 read lock 병렬 실행에 안전하지 않다.

따라서 1차 안정화 기준은 다음과 같다.

```text
HTTP read/parse       lock 밖
SQL parse/classify    lock 밖 가능
DB execute            DB write lock 안
DbResult deep copy    DB write lock 안
JSON build            lock 밖
network write         lock 밖
```

### 2차 개선 정책

SELECT read lock은 아래 조건이 충족된 뒤 켠다.

- SELECT 결과 row를 callback 또는 result sink로 deep copy할 수 있다.
- SELECT 중 shared `FILE *` 위치를 공유하지 않거나 별도 동기화한다.
- row/page cache mutation이 read lock 병렬 실행에 안전하거나 write lock 영역으로 분리된다.
- table open/lazy load는 write lock으로 선행 처리된다.

`pthread_rwlock_t` 자체는 writer preference를 보장하지 않을 수 있다. writer starvation을 반드시 막아야 하면 mutex와 condition variable 기반의 RW lock wrapper를 별도로 구현한다.

## 9. DB Wrapper와 Result Model

API 서버는 executor 내부 구조를 직접 만지지 않고 `db_wrapper`를 통해서만 SQL을 실행한다.

### Wrapper 책임

- SQL 문자열 trim, BOM 제거, 단일 문장 확인
- `parse_statement()` 호출
- statement type 확인
- DB lock 획득/해제
- executor API 호출
- executor 결과를 `DbResult`로 변환
- 오류 메시지를 API 응답용 문자열로 정리

### DbResult 개념

실제 필드명은 구현 편의에 맞춰 조정하되 JSON builder가 순회하기 쉬운 구조여야 한다.

```c
typedef struct {
    char *name;
    char *value;
} DbCell;

typedef struct {
    size_t cell_count;
    DbCell *cells;
} DbRow;

typedef struct {
    int ok;
    int http_status;
    char *error_message;
    size_t row_count;
    DbRow *rows;
} DbResult;
```

모든 값은 문자열로 직렬화한다. 숫자 타입과 문자열 타입은 JSON 응답에서 구분하지 않는다.

### Executor 보완 지시

현재 `execute_select()`는 결과를 stdout에 출력한다. API 서버용으로는 stdout 캡처 대신 아래 방식 중 하나로 보완한다.

권장 방식:

- `executor.c` 내부 SELECT 경로에 result sink callback을 추가한다.
- 기존 CLI 출력은 print sink callback으로 유지한다.
- API wrapper는 collect sink callback을 넘겨 `DbResult`에 row를 복사한다.

예시 개념:

```c
typedef int (*EngineRowSink)(const char **col_names,
                             const char **values,
                             int col_count,
                             void *ctx);

int execute_select_result(Statement *stmt, EngineRowSink sink, void *ctx, char *err, size_t err_size);
```

INSERT, UPDATE, DELETE도 API용 성공/실패 반환 API를 추가한다. 기존 CLI용 `void execute_*()` 함수는 유지해도 된다.

## 10. JSON Builder

외부 JSON 라이브러리는 사용하지 않는다.

지원 escape:

- `"`
- `\`
- `\n`
- `\r`
- `\t`

응답 버퍼 정책:

- 초기 64KB
- 필요 시 2배씩 확장
- 최대 4MB
- 초과 시 `413 Payload Too Large` 또는 `500` 중 원인에 맞게 반환

JSON builder는 DB lock 밖에서 실행한다. 이를 위해 `DbResult`는 lock 해제 전에 모든 문자열을 deep copy해야 한다.

## 11. Logging

로그 출력은 stdout으로 통일한다.

로그 레벨:

- `INFO`
- `WARN`
- `ERROR`

로그 포맷:

```text
[LEVEL] [timestamp] [tid] [trace_id] message
```

규칙:

- 로그 출력은 mutex로 보호한다.
- request마다 trace id를 부여한다.
- trace id는 atomic counter 또는 mutex 보호 counter로 만든다.
- executor 기존 출력은 API 응답과 섞이지 않게 점진적으로 log API 또는 error buffer로 이동한다.

최소 로그:

- server start/stop
- listen start
- accept success/failure
- enqueue/dequeue
- worker start/stop
- request parse success/failure
- DB execute start/end
- response write success/failure
- shutdown start/end

## 12. Graceful Shutdown

트리거:

- SIGINT
- SIGTERM

권장 구현:

- signal handler에서는 `volatile sig_atomic_t` flag만 변경한다.
- `sigaction` 사용 시 `SA_RESTART`를 피해서 `accept()`가 `EINTR`로 깨어날 수 있게 한다.
- accept loop는 shutdown flag를 확인하고 새 연결 수락을 중단한다.
- listener fd close, queue shutdown, worker broadcast, worker join, DB 자원 해제 순서로 정리한다.

순서:

1. shutdown flag 설정
2. accept loop 중단
3. listening socket 닫기
4. 새 요청은 `503` 또는 즉시 close
5. queue에 남은 작업 처리
6. worker broadcast
7. worker join
8. `close_all_tables()`
9. mutex, cond, lock 등 자원 해제

## 13. Request 처리 흐름

```text
listener accept()
-> trace id 발급
-> task queue enqueue
-> worker dequeue
-> HTTP request 읽기
-> HTTP parser 검증
-> SQL body 추출
-> db_wrapper 실행
-> DbResult 생성
-> JSON builder 실행
-> HTTP response write loop
-> close(fd)
```

주의:

- queue mutex를 잡은 채 DB lock을 잡지 않는다.
- DB lock을 잡은 채 network write를 하지 않는다.
- response write는 partial write를 고려해 루프로 처리한다.
- 모든 error path에서 fd와 heap memory를 해제한다.

## 14. 테스트 계획

### Unit Test

대상:

- `http_parser`
- `json_builder`
- `task_queue`

확인:

- 정상 request line/header/body
- 잘못된 method/path/version
- 누락된 `Content-Length`
- body 길이 초과
- JSON escape
- queue full
- queue shutdown

### Smoke Test

`tests/api/smoke.sh`에서 curl 기반으로 확인한다.

- `SELECT * FROM case_basic_users WHERE id = 1;`
- 정상 `INSERT`
- 잘못된 SQL
- 빈 body
- `GET /query`
- `POST /wrong`
- 큰 body

### Concurrency Test

`tests/api/stress.sh`에서 background curl을 병렬 실행한다.

- 기준 부하: 동시 50 요청
- 추가 부하: 100~200 요청
- SELECT/INSERT/UPDATE/DELETE 혼합
- 모든 응답이 JSON 형태인지 확인
- 서버가 죽지 않는지 확인

### Memory Check

마지막 단계에서 Linux, WSL, devcontainer 중 하나에서 실행한다.

```bash
valgrind --leak-check=full ./mini_dbms_api
```

## 15. 구현 순서

### Stage 0. 골격

- `src/api/` 폴더 생성
- `src/api_main.c` 추가
- Makefile에 `api`, `run-api`, `test-api` 추가
- log 모듈 추가
- 기존 CLI 빌드가 계속 되는지 확인

### Stage 1. Listener

- server socket 생성
- `SO_REUSEADDR`
- `bind`
- `listen`
- accept loop
- SIGINT/SIGTERM shutdown flag
- 단일 연결을 받아 고정 응답을 보내는 smoke 동작 확인

### Stage 2. HTTP Parser와 Response

- request line/header parser
- `POST /query` 검증
- `Content-Length` 기반 body read
- error response builder
- `curl` 정상/오류 케이스 확인

### Stage 3. Thread Pool

- bounded task queue
- worker lifecycle
- listener enqueue
- queue full 시 listener가 503 응답
- 동시 50 요청으로 안정성 확인

### Stage 4. DB Wrapper

- SQL trim/BOM 제거
- `parse_statement()` 연동
- global DB write lock 적용
- SELECT result sink 추가
- INSERT/UPDATE/DELETE API용 반환 함수 추가
- `DbResult` 생성
- JSON 응답 완성

### Stage 5. 안정화

- 모든 error path 자원 해제 확인
- partial read/write 점검
- shutdown 중 새 요청 거절
- `make test-api`
- stress script
- valgrind 1회
- README에 API 서버 사용법 링크 추가

## 16. 완료 기준

아래를 만족하면 이번 API 서버 범위는 완료로 본다.

- `make`로 기존 CLI가 빌드된다.
- `make api`로 `mini_dbms_api`가 빌드된다.
- `make run-api` 후 `POST /query`가 JSON을 반환한다.
- `/query` 외 경로와 잘못된 method가 적절한 HTTP status를 반환한다.
- 동시 50 요청에서 서버가 crash 없이 응답한다.
- shutdown 시 worker join과 `close_all_tables()`가 실행된다.
- README 또는 docs에서 빌드/실행/테스트 방법을 찾을 수 있다.

