# 데모 시나리오

## 1. 서버 실행

```bash
make
./bin/dbsrv
```

시작 로그:

```text
dbsrv listening on :8080 (api=4 db=4 q=128)
```

## 2. Health Check

```bash
curl http://localhost:8080/api/v1/health
```

보여줄 포인트:

- 서버가 올라와 있음
- 외부 클라이언트가 접근 가능함

## 3. 단일 SQL

```bash
curl -X POST http://localhost:8080/api/v1/sql \
  -H "Content-Type: application/json" \
  -d '{"query":"SELECT * FROM restaurants WHERE zone = '\''seoul_east'\''"}'
```

보여줄 포인트:

- REST API가 기존 SQL 처리 경로를 호출
- PK/UK/B+Tree 기반 조회 결과를 JSON으로 반환

## 4. 음식 배달 앱 페이지 요청

```bash
curl "http://localhost:8080/api/v1/page?user_id=1&lat=37.5&lng=127.0"
```

보여줄 포인트:

- 하나의 페이지 요청이 4개 SQL로 분해됨
- DB Query Pool에서 병렬 실행됨
- `trace` 필드로 thread id 와 latency 확인 가능

## 5. Metrics 확인

```bash
curl http://localhost:8080/api/v1/metrics
```

보여줄 포인트:

- API pool / DB pool 처리량
- 현재 MVCC 버전과 snapshot 상태

## 6. Rollback 시연

```bash
curl -X POST http://localhost:8080/api/v1/tx \
  -H "Content-Type: application/json" \
  -d '{"queries":["INSERT INTO cart VALUES (9, 9, 1, 7000)","INVALID SQL"]}'
```

보여줄 포인트:

- 첫 쿼리는 working copy 에 반영
- 두 번째 쿼리 실패
- 전체 트랜잭션 abort
- 응답에서 `done` 과 `fail_at` 확인

## 7. 부하 스모크

```bash
./scripts/load.sh
```

출력 예시:

```text
Total requests: 100
Concurrency: 10
Success: 100
Fail: 0
Avg latency: 8ms
```
