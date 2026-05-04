# 64DOS GitHub 알림 기반 점검 체크리스트 & 수정 계획

작성일: 2026-05-04 (UTC)
대상: `fix-kernel-compilation-errors` 관련 실패 PR (#31/#34/#35/#36)

## 1) 실패 원인 파악 체크리스트 (로그 트리아지)

### A. 컴파일/링크 단계 공통 에러 수집
- [ ] 각 PR CI에서 실패한 **첫 번째 red job** 이름 기록
- [ ] `undefined reference` 발생 심볼 목록 정리
- [ ] `implicit declaration`/`missing include`/`unknown type name` 목록 정리
- [ ] 동일 에러가 PR #31/#34/#35/#36에 반복되는지 교집합 확인

#### 우선 확인 파일
- `kernel/kernel.c`
- `kernel/fs_fat12.c`
- `kernel/include/executable.h`
- `executable.h` (repo root, 중복 헤더 여부 확인)

### B. FS 초기화/마운트 배선(mount wiring) 점검
- [ ] 부팅 초기화 함수에서 FS 등록/초기화/마운트 호출 순서 캡처
- [ ] `fs_iface` 구현체 등록 시점과 사용 시점 역전 여부 확인
- [ ] 마운트 실패 시 반환 코드/로그 메시지 표준화 여부 확인
- [ ] 링크 대상(오브젝트/섹션) 누락 여부 확인

### C. 런타임/명령어(RFS) 변경 추적
- [ ] 최근 커밋에서 RFS read/write 경로 변경점 목록화
- [ ] 명령어 인터페이스 변경(인자/에러코드/출력 형식) 유무 확인
- [ ] 코드 변경과 문서 변경의 불일치 항목 정리

---

## 2) 통합 수정 계획 (중복 리뷰 제안 정리)

## Track A: 컴파일 에러 최소 패치
목표: 빌드 실패를 빠르게 green으로 복구

1. 헤더 정합성 정리
   - 단일 canonical 헤더 경로 결정 (`kernel/include/executable.h` 기준 권장)
   - `#include` 경로를 프로젝트 전반에서 일관화
2. include 순서/전방 선언 정리
   - 상호참조 타입은 전방 선언으로 분리
   - 구현 의존은 `.c`로 제한
3. 링크 누락 보정
   - Makefile 오브젝트 목록 점검
   - 순환 의존 심볼 재배치(필요 시 함수 분리)

완료 기준:
- `make all` 성공
- 링크 에러 0건

## Track B: FS mount wiring 복구
목표: 부팅 후 FS 사용 경로 정상화

1. 초기화 순서 고정
   - 디바이스 준비 → FS 드라이버 등록 → 마운트 → 검증 I/O
2. 등록 테이블 점검
   - FS 인터페이스 구조체/함수 포인터 null 여부 검증
3. 에러 처리 가시화
   - 마운트 실패 시 에러코드 + 단계명 출력

완료 기준:
- 부팅 후 RFS read/write 스모크 통과
- 마운트 실패 재현 시 로그로 원인 식별 가능

## Track C: 문서 통합 커밋
목표: 봇 코멘트 분산으로 인한 리뷰 노이즈 제거

1. `docs/`에 런타임/명령어 변경사항 일괄 반영
2. 변경 이력 섹션 추가(무엇/왜/호환성 영향)
3. 예제 명령 2~3개 포함(정상/실패 케이스)

완료 기준:
- 코드/문서 불일치 항목 0건
- 리뷰어가 PR 본문+docs만으로 변경 영향 파악 가능

---

## 3) 실행 순서 (권장)

1. CI 공통 실패 로그 교집합 작성 (20~30분)
2. Track A 최소 수정으로 빌드 복구
3. Track B 마운트/초기화 순서 복구
4. 테스트 매트릭스 실행
5. Track C 문서 통합 커밋
6. PR 정리(합본 PR + superseded 처리)

---

## 4) 테스트 매트릭스

### Build
- [ ] `make all`
- [ ] 핵심 모듈 단위 빌드(가능 시 kernel/fs/runtime 타겟 분리)

### Smoke
- [ ] 부팅 성공
- [ ] RFS read 테스트
- [ ] RFS write 테스트
- [ ] 간단 명령 실행(예: 데모 배치/샘플 실행)

### 회귀 방지
- [ ] 기존 FAT12 이미지 생성/검증 스크립트 통과
- [ ] 파일시스템 계약 테스트 통과

---

## 5) PR 운영안

- 통합 브랜치에서 중복 제안 반영 후 **합본 PR 1개** 생성
- 기존 #31/#34/#35/#36에는 아래 코멘트로 정리:
  - "This PR is superseded by #<new_pr>. Closing to keep review context in one place."
- 실패 PR 이력은 rebase + force-push로 정돈하되, 최종 리뷰는 합본 PR에 집중

---

## 6) 커밋/PR 템플릿 초안

### Commit template
- `fix(kernel): restore executable header wiring and resolve linker errors`
- `fix(fs): restore mount initialization order and registration checks`
- `docs(rfs): consolidate runtime and command updates`

### PR title (예시)
- `fix: consolidate kernel compile + FS mount wiring + RFS docs`

### PR body (요약)
1. CI 공통 실패 원인(링커/헤더/마운트 순서) 요약
2. 코드 수정 포인트(파일별)
3. 테스트 결과(`make all`, 부팅, RFS R/W)
4. 기존 PR superseded 정리 링크

