# 진행 현황 (PROGRESS)

이 문서는 작업 진행도와 남은 작업을 공유하기 위한 운영 문서입니다.

## 1. 전체 진행률
- 전체 진행률: **75%** (`PLAN.md`의 `P1`~`P5` 기준, 완료 2/5 + `P3`/`P4`/`P5` 병행 진전)
- 마지막 업데이트: **2026-03-02** (멀티 에이전트 통합 검증 반영)
- 갱신 원칙: **작업 단위 완료 시 즉시 업데이트**

## 2. 현재 작업 상태
- [x] `P1` 플러그인 기본 구조 및 AE SDK 핸들러 스캐폴딩 — 상태: `완료`
- [x] `P2` 모델/세션 생명주기 + 캐시 우선 렌더 파이프라인 — 상태: `완료`
- [ ] `P3` Depth Map/Slicing 모드 + 8/16/32 bpc 경계 변환 — 상태: `진행중` (AE 라우터 바인딩 2차 완료)
- [ ] `P4` OOM/백엔드 실패 대비 타일링·다운스케일 폴백 — 상태: `진행중` (명시 체인/안전 출력/회귀 테스트 1차 완료)
- [ ] `P5` 테스트/벤치마크/안정성 검증 + 패키징 스크립트 — 상태: `진행중` (perf harness/ctest 등록 완료)

## 3. 최근 완료 작업
- [x] `D1` 문서 역할 분리 완료 (`AGENTS.md` 필수 지침, `PLAN.md` 실행 계획, `PROGRESS.md` 진행 현황)
- [x] `D2` 진행 추적 규칙 확정 (작업 단위마다 갱신, 체크리스트+퍼센트 형식)
- [x] `D3` `plugin/` 스캐폴드 생성 (AE 라우터, 코어 파이프라인, 추론 엔진 인터페이스)
- [x] `D4` 기본 단위 테스트 추가 및 `g++` 기준 컴파일/실행 검증 완료
- [x] `D5` 모델 카탈로그/모델 선택 엔진 추가 (기본: `depth-anything-v3-small`)
- [x] `D6` 모델별 캐시 분리(`model_id` 기반 키 해시) 및 전환 테스트 추가
- [x] `D7` 모델 다운로드 보조 스크립트/가이드 추가 (`tools/download_model.sh`, `models/README.md`)
- [x] `D8` AE 파라미터 스키마/매핑 추가 (`model_id`, quality, output mode, depth range, tiling)
- [x] `D9` 파라미터/렌더 파이프라인/모델 전환 테스트 추가 및 `g++` 통합 검증 재완료
- [x] `D10` `AeCommandRouter`에 파라미터 업데이트/모델 메뉴/렌더 바인딩 연결
- [x] `D11` AE 스텁 엔트리에 모델 설정/그레이 프레임 렌더 테스트 API 추가
- [x] `D12` 팀 운영 문서(`TEAM.md`) 및 리서치 노트(`docs/research/*`) 추가
- [x] `D13` 체크포인트 커밋/원격 푸시 완료 (`main` -> `origin/main`, commit: `348fec7`)
- [x] `D14` 리서치 실무 노트 추가 (`docs/research/2026-03-02-ae-pfcmd-depthanything-notes.md`)
- [x] `D15` `RenderPipeline` 단계별 폴백 체인 구현 (`직접 -> 타일 -> 다운스케일 -> 안전 출력`) 및 캐시/폴백/예외 경로 테스트 추가
- [x] `D16` 추론 런타임 백엔드 선택 옵션/모델 매니페스트 로더/카탈로그 등록 확장 및 관련 테스트 추가 (`plugin/inference/*`, `models/models.manifest`, `tools/download_model.sh`, `tests/test_inference_engine.cpp`)
- [x] `D17` AE 커맨드 브리지 컨텍스트(`AeCommandContext`) 도입 및 엔트리 스텁 경유 업데이트/렌더 경로 검증
- [x] `D18` 성능/안정성 하네스 추가 (`tests/perf_harness.cpp`, `docs/perf/README.md`, CTest 등록)
- [x] `D19` 멀티 에이전트 통합 체크리스트 문서화 (`docs/research/2026-03-02-integration-checklist.md`)
- [x] `D20` 멀티 에이전트 결과 리드 통합 검증 완료 (`/tmp/zsoda_tests`, perf benchmark/stability 통과)

## 4. 남은 작업
1. `P3` 구현: AE 파라미터와 모델 선택 UI(`model_id`)를 실제 핸들러에 연결
2. `P3` 구현: AE SDK 실제 `PF_Cmd_*` 경로에 라우터/파라미터 연결
3. `P3` 구현: 8/16/32 bpc 입출력 변환 유틸 추가 및 렌더 경계 테스트
4. `P4` 잔여: OOM/백엔드 실패 원인별 정책 세분화(예: 타일 크기 자동 축소, VRAM 힌트 기반 다운스케일 비율)
5. `P5` 구축: 성능/회귀/장시간 안정성 테스트 파이프라인 정리(현재 하네스는 1차 완료)
6. 실제 ONNX Runtime 백엔드 연결 (현재는 백엔드 선택 옵션 + 매니페스트 기반 카탈로그 + 안전 폴백 스켈레톤)
7. CMake 기반 빌드/테스트 루트 검증 (`cmake` 도구 설치 후 재검증)

## 5. 이슈 및 리스크
- 플러그인 스캐폴드는 구축되었지만 AE SDK 실제 엔트리/파라미터 바인딩은 아직 스텁 단계임.
- ONNX Runtime/CUDA/DirectML/Metal/CoreML 실추론 경로가 아직 연결되지 않음.
- 리스크 대응:
  - 모델 선택/캐시/폴백 경로를 먼저 안정화해 AE 크래시 리스크를 최소화
  - ORT 연동은 CPU 경로부터 시작 후 GPU 백엔드를 OS별로 점진 확장

## 6. 다음 공유 예정
- 다음 공유 시점: `P3`의 다음 작업 단위(AE SDK 실제 `PF_Cmd_RENDER` 경로 연결) 완료 직후
- 다음 공유 내용: SDK 엔트리 변경점, 픽셀 변환 경로, 테스트/벤치 상태, GitHub 반영 상태
