# API 문서

API prefix는 `/api/v1` 입니다.

현재 API는 CRUD 범위만 노출합니다.

- 지원: `SELECT`, `INSERT`, `UPDATE`, `DELETE`
- 미지원: `CREATE`, `DROP`, `ALTER` 등 DDL

## 1. Health

`GET /api/v1/health`

응답:

```json
{
  "ok": true,
  "data": {
    "status": "up"
  },
  "meta": {
    "req_id": "1"
  }
}
```

## 2. 단일 SQL

`POST /api/v1/sql`

요청:

```json
{
  "query": "SELECT * FROM restaurants WHERE zone = 'seoul_east'"
}
```

성공 응답:

```json
{
  "ok": true,
  "data": {
    "rows": [],
    "count": 0
  },
  "meta": {
    "req_id": "2",
    "thr_id": 1,
    "lat_ms": 1
  }
}
```

## 3. Batch

`POST /api/v1/batch`

요청:

```json
{
  "tx": false,
  "queries": [
    "SELECT * FROM restaurants WHERE zone = 'seoul_east'",
    "SELECT * FROM coupons WHERE user_id = 1"
  ]
}
```

정책:

- `tx=false` 이고 전부 `SELECT` 면 shared snapshot으로 병렬 실행
- 쓰기 SQL이 섞이면 순차 실행

## 4. Transaction

`POST /api/v1/tx`

요청:

```json
{
  "queries": [
    "INSERT INTO cart VALUES (9, 9, 1, 7000)",
    "INVALID SQL"
  ]
}
```

실패 응답:

```json
{
  "ok": false,
  "err": {
    "code": "TX_ABORT",
    "msg": "query failed, rollback completed"
  },
  "data": {
    "done": 1,
    "fail_at": 2
  }
}
```

## 5. Page Demo

`GET /api/v1/page?user_id=1&lat=37.5&lng=127.0`

내부 분해:

- restaurants
- order
- coupons
- cart

각 SQL은 DB Query Pool에 들어가고, 응답의 `trace` 에 worker thread id 와 latency가 기록됩니다.

## 6. Metrics

`GET /api/v1/metrics`

응답 필드:

- `api_pool`
- `db_pool`
- `mvcc`

`mvcc` 포함 값:

- `tx_live`
- `snap_min`
- `ver_now`
- `gc_wait`

## 에러 코드

- `BAD_REQ`: 잘못된 요청 형식, 빈 body, 필수 필드 누락
- `BAD_JSON`: 최소 JSON 형식 오류
- `BAD_SQL`: 미지원 SQL 또는 파싱/실행 실패
- `TX_ABORT`: 트랜잭션 실패 후 rollback 완료
- `NO_ROUTE`: 없는 endpoint
- `BAD_METH`: 허용되지 않은 method
- `TOO_BIG`: body 또는 SQL 길이 초과
- `TOO_MANY`: batch query 개수 초과
- `Q_FULL`: API 또는 DB queue 가 가득 참
- `OOM`: 메모리 부족
- `INT_ERR`: 내부 오류

## curl 예시

```bash
curl http://localhost:8080/api/v1/health

curl -X POST http://localhost:8080/api/v1/sql \
  -H "Content-Type: application/json" \
  -d '{"query":"SELECT * FROM restaurants WHERE zone = '\''seoul_east'\''"}'

curl -X POST http://localhost:8080/api/v1/batch \
  -H "Content-Type: application/json" \
  -d '{"tx":false,"queries":["SELECT * FROM restaurants WHERE zone = '\''seoul_east'\''","SELECT * FROM coupons WHERE user_id = 1"]}'

curl -X POST http://localhost:8080/api/v1/tx \
  -H "Content-Type: application/json" \
  -d '{"queries":["INSERT INTO cart VALUES (9, 9, 1, 7000)","INVALID SQL"]}'

curl "http://localhost:8080/api/v1/page?user_id=1&lat=37.5&lng=127.0"
```
