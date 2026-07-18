# 발표 스크립트 (최신 버전)

## 0. 오프닝

안녕하세요. 저희는 기존 C 기반 SQL 처리기를 실제 요청을 받는 API 서버 형태로 확장했습니다.  
오늘 발표는 다음 3가지를 중심으로 보시면 됩니다.

1. 구조를 어떻게 바꿨는지
2. 동시성에서 무엇을 막았는지
3. 데모에서 어떤 결과를 확인할 수 있는지

[여기에 프로젝트 한 줄 요약이 들어간 타이틀 슬라이드 그림]

---

## 1. 문제 정의

기존 코드는 CLI 중심이라 SQL 파일 실행에는 적합했지만, 동시에 들어오는 웹 요청을 처리하기에는 한계가 있었습니다.  
핵심 과제는 2가지였습니다.

- 읽기 일관성을 깨지 않기
- 동시 쓰기 충돌에서 실패율을 낮추기

[여기에 Before(단일 실행) vs After(동시 요청) 비교 그림]

---

## 2. 최종 구조

현재 구조는 `API 서버 + 스레드풀 + MVCC 엔진`입니다.

- API 계층: `/sql`, `/batch`, `/tx`, `/page`, `/metrics`
- 실행 계층: API Worker Pool, DB Query Pool
- 저장/엔진: CSV + B+Tree, MVCC

요청은 API Worker가 받고, 병렬 조회가 필요한 경우 DB Query Pool에서 분기 실행합니다.

[여기에 전체 아키텍처 다이어그램 그림]

---

## 3. 프로젝트 특징

- C 기반 SQL 엔진을 REST API 서버로 확장
- API Worker Pool + DB Query Pool 이중 풀 구조
- `/api/v1/page`에서 병렬 trace 확인 가능
- 트랜잭션 롤백, 캐시, 부하 테스트를 데모 페이지에서 통합 시연
- 기존 CSV + B+Tree 자산을 유지한 채 동시성 강화

[여기에 핵심 특징 5개 아이콘형 요약 그림]

---

## 4. 동시성 정책: table-snapshot COW MVCC와 충돌 제어

정리하면:
- **snapshot read = 한 요청·트랜잭션 안의 읽기 일관성**
- **table head/base 검사 = 명시적 transaction의 write-write conflict detection**
- **autocommit mutex shard = 단건 write의 제한된 직렬화**

[여기에 snapshot read, table conflict, autocommit serialization 역할 분리 그림]

---

## 5. 구현 포인트

### 5-1) MVCC

- read는 snapshot 기준으로 수행
- write는 private working copy에 반영
- commit 시 충돌 검사 후 install

### 5-2) Autocommit Write Mutex Shard

- `/api/v1/sql` 단건 쓰기에서 `table + id` hash로 mutex shard 선택
- 같은 shard의 쓰기는 직렬화되지만 hash 충돌이 가능함
- 명시적 transaction은 table head/base conflict detection 사용
- 따라서 row-level lock이 아니라 제한된 autocommit serialization로 설명

[여기에 mutex shard 충돌과 table-level conflict 범위 그림]

---

## 6. 데모 시나리오

### 5단계: 동시 요청 레벨 비교

- concurrency 8 / 16 / 32 비교
- `total_ms`, `p95`, `rps`, `fail_rate`로 확인

### 6단계: 혼합 부하

- `SELECT /api/v1/sql` 50%
- `UPDATE /api/v1/sql` 30%
- `GET /api/v1/page` 20%

즉, 읽기 전용이 아니라 실제에 가까운 읽기/쓰기 혼합 시나리오입니다.

[여기에 데모 페이지 5,6단계 스크린샷 그림]

---

## 7. 성능/안정성 결과 (발표 핵심 수치)

동일 조건(`UPDATE only`, 32 VU, 20초) 기준:

- 재시도/락 적용 전: 실패율이 매우 높게 발생(충돌 상황에서 대량 실패)
- 재시도 + autocommit mutex shard 정책을 적용한 당시 문서에는 **성공률 100%**로 기록되어 있음

이 수치는 해당 조건의 역사적 관측값이며 일반적인 성공률 보장이 아닙니다. 원시 k6 결과가 저장소에 함께 보존되어 있지 않으므로 외부 제출 자료에서 인용하려면 같은 프로필을 다시 실행해 결과 artifact와 함께 제시해야 합니다.

[여기에 적용 전/후 성공률 비교 막대 그래프 그림]

---

## 8. 트랜잭션 정합성

`/api/v1/tx`에서 중간 SQL 실패 시 전체 롤백을 시연할 수 있습니다.

- 성공 전까지 원본 반영 없음
- 실패 시 working copy 폐기
- 결과적으로 부분 반영 없이 정합성 유지

[여기에 트랜잭션 성공/실패 분기 그림]

---

## 9. 테스트/검증

- `make test`
- `tests/test_pool.c`
- `tests/test_mvcc.c`
- `tests/test_dbapi.c`
- `tests/test_tx.c`
- `tests/api_test.sh`

코드 레벨 테스트와 API 스모크 테스트를 함께 운영합니다.

[여기에 테스트 실행 결과 요약 화면 그림]

---

## 10. 마무리

이번 프로젝트의 결론은 명확합니다.

1. CLI 엔진을 실제 서비스형 API 구조로 확장했다.
2. MVCC로 읽기 일관성을 확보했다.
3. table-level conflict detection과 autocommit mutex shard의 범위를 코드 기준으로 검증했다.
4. 데모와 부하 테스트에서 결과를 수치로 검증했다.

감사합니다.

[여기에 최종 요약 4줄 카드형 그림]

