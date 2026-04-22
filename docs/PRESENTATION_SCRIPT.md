# 발표 대본

## 0. 오프닝

안녕하세요. 저희 프로젝트는 기존 C 기반 미니 SQL 처리기를 REST API 서버 형태로 확장한 Mini DBMS API Server입니다.

기존 프로젝트는 SQL 파일을 입력으로 받아 lexer, parser, executor를 거쳐 CSV와 B+Tree 인덱스를 사용하는 CLI 중심 구조였습니다. 이번에는 여기에 API 서버, thread pool, MVCC transaction, rollback, metrics, 테스트와 CI를 추가해서 외부 클라이언트가 실제로 요청을 보낼 수 있는 서버형 구조로 발전시켰습니다.

## 1. Before -> After

먼저 전체 변화입니다.

Before는 SQL 파일을 읽고 내부 실행기가 바로 CSV를 조작하는 구조였습니다. 엔진 구현을 보기에는 좋지만, 외부 시스템이 접근하거나 동시 요청을 처리하는 구조는 아니었습니다.

After는 HTTP client가 API server로 요청을 보내고, API worker pool이 요청을 처리한 뒤 DB API와 MVCC engine으로 내려가는 구조입니다. 기존 B+Tree와 CSV 저장 방식은 살리면서, 서버로 사용할 수 있도록 계층을 추가했습니다.

핵심 변화는 다섯 가지입니다. CLI에서 REST API로 확장했고, 단일 실행 흐름에서 thread pool 기반으로 바뀌었습니다. 또 `/sql`, `/batch`, `/tx`, `/page`, `/metrics` endpoint를 만들었고, table-snapshot copy-on-write 방식의 MVCC를 추가했습니다. 마지막으로 `make test`와 GitHub Actions로 검증 흐름을 만들었습니다.

## 2. 전체 구조

전체 구조를 보면 위쪽에는 client와 HTTP API server가 있습니다. API server는 request를 API worker pool에 넣고, worker가 route layer로 넘깁니다.

route layer는 요청 종류에 따라 DB API를 호출합니다. 일반 SQL은 바로 DB API로 내려가고, `/page`처럼 여러 조회가 필요한 요청은 DB Query Pool을 사용해서 병렬로 SELECT를 실행합니다.

DB API 아래에는 MVCC engine이 있고, 실제 데이터는 `data/*.csv`에 저장됩니다. 인덱스는 기존 legacy B+Tree 구현을 재사용했습니다. 즉, 새로 만든 서버 계층과 기존 SQL 엔진 자산을 연결한 구조입니다.

## 3. 핵심 메시지

이번 확장의 핵심 키워드는 API Server, Thread Pool, MVCC, Demo, Quality입니다.

API Server는 기존 엔진을 외부에서 호출 가능하게 만든 부분입니다. Thread Pool은 요청마다 thread를 새로 만들지 않고 고정 worker와 queue로 처리하는 구조입니다.

MVCC는 읽기 일관성과 rollback을 보여주기 위한 핵심입니다. read는 snapshot을 보고, write는 private working copy에 반영한 뒤 commit 시점에 충돌을 확인합니다.

Demo 측면에서는 `/page`, `/metrics`, `/tx`를 준비했습니다. Quality 측면에서는 unit test, API smoke test, CI를 추가했습니다.

## 4. 요청 처리 흐름

요청 하나가 들어오면 먼저 HTTP API server가 request를 받습니다. 이 request는 API pool에 job으로 들어가고, worker가 route dispatch를 수행합니다.

route는 SQL 실행, batch 실행, transaction 실행 같은 작업을 DB API에 요청합니다. DB API는 snapshot이나 transaction context를 만들고 MVCC engine에 접근합니다.

MVCC engine은 현재 snapshot에서 볼 수 있는 table version을 읽거나, commit 시 CSV에 flush합니다. 결과는 `DbRes` 형태로 올라오고, route layer가 JSON response로 변환해서 client에게 돌려줍니다.

이 흐름의 장점은 HTTP 처리, route 처리, DB 처리의 책임이 분리되어 있다는 점입니다.

## 5. Thread Pool

Thread pool은 두 종류로 나눴습니다.

첫 번째는 API Worker Pool입니다. 이 pool은 HTTP 요청을 받아 route handler를 실행합니다. 요청마다 thread를 만들지 않고 queue에 넣어서 고정 worker들이 처리합니다.

두 번째는 DB Query Pool입니다. 이 pool은 `/page`나 read batch처럼 여러 조회를 병렬로 실행할 수 있는 경우에 사용합니다.

특히 `/api/v1/page`는 하나의 페이지 요청을 여러 SQL로 분해합니다. 예를 들어 음식점 목록, 주문 정보, 쿠폰, 장바구니 같은 데이터를 같은 snapshot에서 병렬로 조회합니다. 응답에는 thread id와 latency trace가 포함되어서 병렬 실행을 발표 중에 확인할 수 있습니다.

## 6. MVCC Transaction

이번 프로젝트의 동시성 제어는 row-level MVCC가 아니라 table-snapshot copy-on-write MVCC입니다.

Transaction이 시작되면 snapshot id를 잡습니다. read는 그 시점에 보이는 committed version만 읽습니다.

write가 발생하면 바로 원본 table을 수정하지 않습니다. first write 시점에 현재 table version을 private working copy로 복사하고, 모든 변경은 그 copy에 반영합니다.

commit 시점에는 transaction이 시작할 때의 base version과 현재 head version이 같은지 확인합니다. 같으면 새 committed version으로 install하고, 다르면 write conflict로 abort합니다.

rollback은 단순합니다. 원본을 건드리지 않았기 때문에 private working copy만 버리면 됩니다.

## 7. API Demo Flow

데모는 일곱 단계로 진행하면 됩니다.

첫 번째, `make` 후 `./bin/dbsrv`로 서버를 실행합니다.

두 번째, `/api/v1/health`로 서버가 살아있는지 확인합니다.

세 번째, `/api/v1/sql`로 단일 SQL이 REST API를 통해 실행되는 것을 보여줍니다.

네 번째, `/api/v1/page`를 호출해서 하나의 페이지 요청이 여러 SQL로 분해되고 DB Query Pool에서 병렬 실행되는 것을 보여줍니다.

다섯 번째, `/api/v1/metrics`로 API pool, DB pool, MVCC 상태를 확인합니다.

여섯 번째, `/api/v1/tx`에 정상 INSERT와 잘못된 SQL을 같이 넣어서 rollback을 시연합니다.

마지막으로 `make test`로 구현이 테스트되고 있다는 점을 보여줍니다.

## 8. Rollback 시연

Rollback 시연은 이 프로젝트에서 가장 보여주기 쉬운 포인트입니다.

client가 transaction으로 INSERT를 보냅니다. 이 INSERT는 committed table에 바로 반영되지 않고 working copy에만 적용됩니다.

그 다음 두 번째 query로 잘못된 SQL을 보냅니다. transaction은 실패 지점을 `fail_at`으로 응답하고 abort됩니다.

abort되면 working copy를 버립니다. 그래서 앞에서 성공한 것처럼 보였던 INSERT도 최종 committed data에는 남지 않습니다.

이 방식은 파일 백업을 해두고 되돌리는 방식이 아니라, 애초에 commit 전까지 원본을 수정하지 않는 방식이라 구조가 단순합니다.

## 9. Test & CI

테스트는 thread pool, MVCC, DB API, transaction을 각각 나눠서 검증합니다.

`tests/test_pool.c`는 queue와 worker 동작을 확인합니다. `tests/test_mvcc.c`는 snapshot과 version 동작을 확인합니다. `tests/test_dbapi.c`는 DB API가 SQL 결과를 잘 반환하는지 확인합니다. `tests/test_tx.c`는 commit과 rollback을 검증합니다.

추가로 `tests/api_test.sh`는 실제 서버를 띄운 뒤 API endpoint를 호출하는 smoke test입니다.

이 테스트들은 `make test`로 실행되고, GitHub Actions에도 연결되어 있습니다.

## 10. 마무리

정리하면, 기존 프로젝트는 미니 SQL 엔진의 내부 구현에 강점이 있었습니다. 이번 확장에서는 그 엔진을 서버로 감싸고, thread pool과 MVCC transaction을 추가해서 시스템 설계 관점의 프로젝트로 확장했습니다.

발표의 핵심 흐름은 네 가지입니다.

첫째, CLI 엔진을 REST API 서버로 확장했습니다.

둘째, API Worker Pool과 DB Query Pool로 병렬 처리 구조를 만들었습니다.

셋째, table-snapshot copy-on-write MVCC로 snapshot read, conflict detect, rollback을 구현했습니다.

넷째, `/page`, `/metrics`, `/tx`와 테스트/CI로 시연성과 검증 가능성을 확보했습니다.

한 줄로 말하면, 이 프로젝트는 C 기반 미니 SQL 처리기를 외부에서 접근 가능한 동시성 DBMS API 서버로 확장한 작업입니다.
