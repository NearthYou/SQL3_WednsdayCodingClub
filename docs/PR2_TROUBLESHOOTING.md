# PR #2 리뷰 코멘트 트러블슈팅

대상 PR: `https://github.com/NearthYou/SQL3_WednsdayCodingClub/pull/2`  
목적: 리뷰 코멘트(High/Medium) 해석, 수정 내역, 남은 과제 정리

## 1) `src/api/srv.c` 고정 버퍼 vs `max_body` 불일치 (High)

- 코멘트 요지:
  - `CONN_BUF_CAP=32KB` 고정 버퍼로 인해, `max_body`(기본 1MB) 설정과 달리 큰 요청이 조기 413 될 수 있음.
- 원인:
  - 연결 버퍼가 정적 배열이고, 요청 헤더+바디 전체를 같은 버퍼에 담는 구조.
- 조치:
  - `HttpConn`을 동적 버퍼(`buf/cap/max_total`)로 변경.
  - `max_total = max_body + MAX_HDR_BYTES + 4` 상한 내에서 필요 시 `realloc` 확장.
  - `req_bytes > max_total`만 413 처리.
- 효과:
  - 실제 허용 요청 크기가 `max_body` 설정과 일치하도록 개선.

## 2) `size_t` -> `int` 캐스팅 비교 리스크 (Medium)

- 코멘트 요지:
  - `body_len`(`size_t`)를 `int`로 캐스팅해 비교하면 overflow/우회 위험 가능.
- 원인:
  - `if ((int)body_len > max_body)` 형태의 타입 불일치 비교.
- 조치:
  - `size_t` 기준으로 비교하도록 변경:
  - `body_cap = (size_t)max_body`를 두고 `if (body_len > body_cap) return 413;`
- 효과:
  - 요청 길이 검증의 타입 안정성 확보.

## 3) `db->mu` 락 범위 과대: clone/save 병목 (High)

- 코멘트 요지:
  - 전역 mutex를 잡은 상태에서 무거운 작업(`tab_clone`, `tab_save`)이 수행되어 병목.
- 원인:
  - write 경로에서 `tx_touch()`가 lock 안에서 `tab_clone()` 수행.
  - commit 경로에서 lock 안에서 `tab_save()` 수행.
- 조치(이번 반영):
  - `tab_clone`은 lock 밖에서 수행되도록 write 경로 분리:
  - lock 안: `tx_view()`로 base 참조 확보
  - lock 밖: `tx_make_work()`에서 clone 생성
- 보류/의도적 미반영:
  - `tab_save`를 lock 밖으로 옮기는 변경은 **파일 최신성 역전 위험**(후행 커밋이 먼저 저장될 수 있음)이 있어 안전성 우선으로 보류.
- 남은 과제:
  - per-table lock 또는 저장 순서 보장 메커니즘(버전 기반 flush 가드) 설계 후 `tab_save` 락 축소.

## 4) 행 추가 시 반복 `realloc`로 O(N^2) 경향 (Medium)

- 코멘트 요지:
  - 로딩/삽입 시 매 row마다 `realloc`하면 대용량에서 비효율.
- 원인:
  - `load_row`, `ins_do`가 `row_cnt+1` 단위 재할당.
- 조치:
  - `TabVer`에 `row_cap` 추가.
  - `ensure_row_cap()` 도입(배수 증가 전략).
  - `load_row`와 `ins_do`가 capacity 기반 확장 사용.
- 효과:
  - 평균 삽입/적재 비용 개선(빈번한 재할당 감소).

## 5) 검증 결과

- 통과:
  - `make build`
  - `./bin/test_mvcc`
  - `./bin/test_dbapi`
  - `./bin/test_tx`
- 제한:
  - `tests/api_test.sh`는 현재 실행 환경에서 소켓 bind 정책 제한으로 실패 가능
  - 로그 예: `srv_run failed (port=...)`

## 6) 요약

- 4개 코멘트 중 3개는 직접 수정 완료:
  - 동적 버퍼/`max_body` 정합성
  - `size_t` 길이 비교 안정성
  - row capacity 확장 전략
- 1개는 안전성 우선으로 부분 대응:
  - clone 병목은 완화, save 락 축소는 후속 설계 과제로 분리
