# 진행 현황 (PROGRESS)

이 문서는 작업 진행도와 남은 작업을 공유하기 위한 운영 문서입니다.

## 1. 전체 진행률
- 전체 진행률: **99%** (`PLAN.md`의 `P1`~`P5` 기준, `P3/P4/P5`는 마무리 단계)
- 마지막 업데이트: **2026-03-02** (`tools/build_aex.ps1` PowerShell 문자열 보간 파서 오류(`$Label:`) 수정)
- 갱신 원칙: **작업 단위 완료 시 즉시 업데이트**

## 2. 현재 작업 상태
- [x] `P1` 플러그인 기본 구조 및 AE SDK 핸들러 스캐폴딩 — 상태: `완료`
- [x] `P2` 모델/세션 생명주기 + 캐시 우선 렌더 파이프라인 — 상태: `완료`
- [ ] `P3` Depth Map/Slicing 모드 + 8/16/32 bpc 경계 변환 — 상태: `진행중 (93%)` (완료: `PF_Cmd_USER_CHANGED_PARAM` 매핑+`params[]` 추출/렌더 override, `PARAM_SETUP` `PF_ADD_*` 등록 스캐폴드, SDK 픽셀 힌트+stride 결합 포맷 추론 / 남은 핵심: 실제 AE SDK 환경에서 파라미터 UI 등록/렌더 연동 실검증)
- [ ] `P4` OOM/백엔드 실패 대비 타일링·다운스케일 폴백 — 상태: `진행중 (88%)` (완료: `직접->타일->다운스케일->안전 출력` 폴백 체인, 적응형 타일 재시도+VRAM budget 기반 비율 조정 / 남은 핵심: SDK/OS 메모리 신호 연계, OOM/백엔드 실패 원인별 정책 세분화)
- [ ] `P5` 테스트/벤치마크/안정성 검증 + 패키징 스크립트 — 상태: `진행중 (86%)` (완료: perf harness+CTest 등록, 로컬/CI 공용 검증 스크립트·워크플로, Windows `.aex` 빌드 헬퍼(`tools/build_aex.ps1`), ORT 런타임 배포 노트/AE 스모크 테스트 체크리스트 추가 / 남은 핵심: 네이티브 host 기준 최종 `.aex/.plugin` 실검증, ORT API ON 경로 실빌드 검증)

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
- [x] `D21` `.aex/.plugin` 최종 패키징 경로 문서화 (사전 요구사항, CMake 옵션, Windows/macOS 명령 예시, 환경 제약 정리) (`docs/build/README.md`, `README.md`)
- [x] `D22` AE SDK 조건부 엔트리 어댑터 추가 (`AeHostAdapter`, `EffectMain` 스켈레톤) 및 라우팅 테스트 보강
- [x] `D23` 8/16/32 bpc 경계 변환 유틸 추가 (`PixelConversion`) 및 변환/유효성 테스트 보강
- [x] `D24` ORT 옵션 경로 2차 통합 검증 (`ZSODA_WITH_ONNX_RUNTIME=1` 테스트 빌드 통과)
- [x] `D25` 리더 리뷰 노트 추가: 이번 라운드 수용 기준 요약 + Windows/macOS AE SDK 환경의 실 `.aex/.plugin` 산출 전 필수 잔여 조건 정리 (`docs/build/2026-03-02-leader-review-note.md`)
- [x] `D26` 호스트 버퍼 렌더 브리지(입출력 픽셀 변환 경로), 적응형 타일 재시도 폴백, 추론 백엔드 상태 진단 추가 + 관련 테스트 확장 후 재검증 완료 (`plugin/ae/*`, `plugin/core/RenderPipeline.cpp`, `plugin/inference/ManagedInferenceEngine*`, `tests/*`)
- [x] `D27` ONNX Runtime 스캐폴드의 모델 경로 검증/전처리 준비/진단 문자열 확장 + `PF_Cmd_RENDER` 추출 스캐폴드(안전 프레임 해시/픽셀 포맷 후보 계산) 고도화 및 회귀 테스트 통과 (`plugin/inference/OnnxRuntimeBackend.cpp`, `plugin/inference/ManagedInferenceEngine.cpp`, `plugin/ae/AeHostAdapter*`, `tests/test_inference_engine.cpp`, `tests/test_ae_router.cpp`)
- [x] `D28` ORT API 옵션 경로 추가(`ZSODA_WITH_ONNX_RUNTIME_API`) 및 실제 세션 생성/실행 파이프라인(전처리 NCHW + 출력 정규화/리사이즈) 통합, CMake 옵션/문서 확장, 비-API 회귀 테스트 재통과 (`plugin/inference/OnnxRuntimeBackend.cpp`, `plugin/CMakeLists.txt`, `README.md`, `docs/build/README.md`, `tests/test_inference_engine.cpp`)
- [x] `D29` CMake에 AE SDK 기반 패키징 타깃 정의 추가(Windows `zsoda_aex`, macOS `zsoda_plugin_bundle`) 및 빌드 문서 동기화 (`plugin/CMakeLists.txt`, `README.md`, `docs/build/README.md`)
- [x] `D30` macOS `zsoda_plugin_bundle`에 `.plugin` 번들 속성/`Info.plist` 템플릿 연결 및 패키징 문서 반영 (`plugin/CMakeLists.txt`, `plugin/ae/Info.plist.in`, `README.md`, `docs/build/README.md`)
- [x] `D31` `PF_Cmd_RENDER` 스캐폴드의 호스트 픽셀 포맷 선택 로직 고도화(Stride 기반 8/16/32 추론 + source/output 힌트 충돌 검증) 및 단위 테스트 추가 (`plugin/ae/AeHostAdapter.*`, `tests/test_ae_router.cpp`)
- [x] `D32` `RenderParams.vram_budget_mb` 도입 + 다운스케일 fallback에서 VRAM budget 기반 축소 비율 선택 로직 추가(캐시 키 반영 포함) 및 회귀 테스트 통과 (`plugin/core/RenderPipeline.*`, `plugin/core/Cache.*`, `plugin/ae/AeParams.*`, `tests/test_render_pipeline.cpp`, `tests/test_ae_params.cpp`)
- [x] `D33` 네이티브 빌드 산출물 수집용 패키징 스크립트 추가(`tools/package_plugin.sh`, `tools/package_plugin.ps1`) 및 빌드 문서/README 동기화 (`docs/build/README.md`, `README.md`)
- [x] `D34` 로컬/CI 공용 검증 스크립트(`tools/run_local_ci.sh`) 및 GitHub Actions 워크플로(`.github/workflows/ci.yml`) 추가, 로컬 실행 통과
- [x] `D35` AE SDK 경로에 `PF_Cmd_USER_CHANGED_PARAM` -> `AeCommand::kUpdateParams` 매핑 추가, `params[]` 기반 `AeParamValues` 추출/렌더 override 연결, 스텁 파라미터 설정 API(`ZSodaSetParamsStub`) 및 회귀 테스트 보강 (`plugin/ae/AeHostAdapter.*`, `plugin/ae/AePluginEntry.cpp`, `tests/test_ae_router.cpp`)
- [x] `D36` AE SDK `PARAM_SETUP`에 `PF_ADD_*` 기반 파라미터 등록 스캐폴드 추가, `SEQUENCE_*`/`SMART_*` 안전 no-op 매핑 확장, `PF_Cmd_RENDER` 포맷 선택 시 SDK 픽셀 힌트(in_data/world/accessor) 결합 반영 (`plugin/ae/AeHostAdapter.cpp`)
- [x] `D37` 문서 동기화: 문서 인덱스에 새 문서/스크립트 참조 링크 추가 + `사용자 테스트 가능 시점` 섹션 반영 (`README.md`, `docs/build/README.md`, `PROGRESS.md`)
- [x] `D38` Windows 빌드/검증 고도화: `tools/build_aex.ps1` 추가(설정/빌드/산출물 SHA256/MediaCore 복사 옵션), `tools/package_plugin.ps1`에 `-OrtRuntimeDllPath` 옵션 추가, ORT 런타임 배포 노트/AE 스모크 테스트 체크리스트 추가 (`tools/build_aex.ps1`, `tools/package_plugin.ps1`, `docs/build/ORT_RUNTIME_DEPLOY.md`, `docs/build/AE_SMOKE_TEST.md`, `README.md`, `docs/build/README.md`)
- [x] `D39` Windows 실제 실행 이슈 수정: `tools/build_aex.ps1`의 `Write-Host "$Label:"`를 `Write-Host "$($Label):"`로 변경해 PowerShell 파서 오류 제거 (`tools/build_aex.ps1`)

## 4. 남은 작업
1. `P3` 구현: AE 파라미터와 모델 선택 UI(`model_id`)를 실제 `PARAM_SETUP` 등록 코드와 AE 호스트에서 실검증
2. `P3` 구현: AE SDK 실제 `PF_Cmd_*` 경로에 라우터/파라미터 연결(현재 USER_CHANGED/RENDER/SEQUENCE/SMART no-op 매핑 반영, 호스트 통합 검증 남음)
3. `P3` 구현: `PF_Cmd_RENDER` payload에서 SDK 픽셀 타입 힌트 결합 경로의 16/32 bpc 실호스트 검증
4. `P4` 잔여: OOM/백엔드 실패 원인별 정책 세분화(타일 자동 축소 + VRAM 힌트 기반 다운스케일 비율은 반영 완료, SDK/OS별 메모리 신호 연계 남음)
5. `P5` 구축: 성능/회귀/장시간 안정성 테스트 파이프라인 정리(로컬/CI 기본 자동화 완료, 네이티브 host 검증 파이프라인 추가 필요)
6. `P5` 구축: 문서화된 최종 패키징 경로를 실제 CMake 타깃(`.aex/.plugin`) 및 배포 스크립트로 연결 (Windows 빌드/패키징 스크립트는 추가 완료, 네이티브 호스트 실검증 남음)
7. ORT API 경로의 네이티브 실검증 (현재 환경은 ORT 헤더/라이브러리 부재로 API ON 빌드 미검증)
8. CMake 기반 빌드/테스트 루트 검증 (`cmake` 도구 설치 후 재검증)
9. ORT 백엔드 GPU 프로바이더(CUDA/DirectML/Metal/CoreML) 분기 연결 및 OS별 fallback 정책 문서화
10. macOS 코드서명/노타리 최종 경로 마감 및 배포용 번들 검증

## 5. 이슈 및 리스크
- 플러그인 스캐폴드는 구축되었고 SDK 바인딩 경로를 확장했지만, AE 호스트 실환경에서의 최종 검증이 남아 있음.
- ONNX Runtime/CUDA/DirectML/Metal/CoreML 실추론 경로가 아직 연결되지 않음.
- 리스크 대응:
  - 모델 선택/캐시/폴백 경로를 먼저 안정화해 AE 크래시 리스크를 최소화
  - ORT 연동은 CPU 경로부터 시작 후 GPU 백엔드를 OS별로 점진 확장

## 6. 다음 공유 예정
- 다음 공유 시점: `P3`의 다음 작업 단위(`PF_Cmd_RENDER` payload 디코딩 + `PixelConversion` 실제 연결) 완료 직후
- 다음 공유 내용: SDK payload->내부 프레임 변환 코드, 테스트/벤치 상태, GitHub 반영 상태
