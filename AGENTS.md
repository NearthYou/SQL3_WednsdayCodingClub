# 작업 진행 상황

## 프로젝트 분석 결과

- 기존 SQL 처리기: 현재는 `src/cli/main.c` 가 SQL 파일을 읽고 `src/legacy/parser.c` / `src/legacy/executor.c` 로 분기하는 CLI 중심 구조
- 기존 B+ Tree 위치: `src/legacy/bptree.c`, `src/legacy/bptree.h`
- 기존 DB 엔진 API: legacy 쪽은 `execute_insert`, `execute_select`, `execute_update`, `execute_delete` 중심. 신규 서버 쪽은 `src/db/dbapi.h`
- 빌드 방식: `Makefile`
- 테스트 방식: 기존은 시나리오 SQL/벤치 중심, 현재는 `tests/*.c` + `tests/api_test.sh` 추가
- 현재 지원 SQL: CLI는 `INSERT`, `SELECT`, `UPDATE`, `DELETE`. API도 CRUD만 노출하고 `CREATE` 같은 DDL은 미지원
- 데이터 저장 방식:
  - legacy CLI: CSV + `.delta` log + `.idx` snapshot
  - 신규 API 엔진: CSV persisted table + committed version chain + table-snapshot COW MVCC

## 구현 계획

- API 서버: 구현 완료. `src/api`
- 스레드 풀: 구현 완료. `src/thr/pool.h`, `src/thr/pool.c`
- DB 락: RW Lock 대신 table-snapshot COW MVCC로 대체
- 트랜잭션: 구현 완료. private working copy + conflict detect + rollback discard
- 테스트: 구현 완료. `make test`
- 데모: 구현 완료. `data/*.csv`, `/api/v1/page`, `scripts/load.sh`
- CI/CD: 구현 완료. `.github/workflows/ci.yml`
