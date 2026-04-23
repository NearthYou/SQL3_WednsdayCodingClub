# 의사결정 기록

## 1. 외부 라이브러리를 넣지 않은 이유

과제용 미니 DBMS 서버에서 의존성보다 코드 가시성과 통제 범위가 더 중요하다고 판단했습니다.

- HTTP: POSIX socket + pthread
- JSON: 요청 스키마 전용 최소 파서

## 2. API Pool과 DB Pool을 분리한 이유

API worker가 내부 SQL 작업을 같은 풀에 넣고 기다리면 deadlock이 생길 수 있습니다. 그래서 HTTP 요청 처리용 풀과 병렬 SQL 실행용 풀을 분리했습니다.

## 3. RW Lock 대신 table-snapshot COW MVCC를 선택한 이유

기존 CSV 엔진 구조에서 row-chain MVCC까지 가면 구현량이 크게 늘고, 발표 설명도 복잡해집니다. 반면 table-snapshot COW MVCC는 다음 장점이 있습니다.

- snapshot consistency 설명이 쉬움
- rollback 구현이 단순함
- 현재 구조를 크게 뒤엎지 않아도 됨

대신 단점은 table-level conflict가 발생한다는 점입니다.

## 4. API에서 CRUD만 노출한 이유

v1 목표는 안정적인 API/동시성/트랜잭션 시연입니다. DDL까지 열면 parser, file init, schema mutation까지 범위가 급격히 넓어지므로 `CREATE` 는 API에서 의도적으로 막았습니다.

## 5. rollback을 file backup이 아니라 working copy discard로 처리한 이유

현재 서버 엔진은 commit 전까지 private copy에서만 수정합니다. 따라서 실패 시에는 메모리 working copy만 버리면 원상복구가 가능합니다. 이 방식이 snapshot/file backup보다 현재 구조에 더 잘 맞습니다.

## 6. 기본 데이터 루트를 `data/` 로 둔 이유

데모용 fixture를 저장소 루트 CSV와 섞지 않기 위해서입니다. 테스트는 `DB_ROOT` 로 임시 디렉터리를 따로 사용합니다.
