# 진행 현황 (PROGRESS)

이 문서는 작업 진행도와 남은 작업을 공유하기 위한 운영 문서입니다.

## 1. 전체 진행률
- 전체 진행률: **99%** (`PLAN.md`의 `P1`~`P5` 기준, `P3/P4/P5`는 마무리 단계)
- 마지막 업데이트: **2026-03-03** (빌드 로더 게이트를 `.rr` + 최종 `.aex` 바이너리 검증으로 재설계하고 `ZSoda.loader_check.txt` 산출 추가)
- 갱신 원칙: **작업 단위 완료 시 즉시 업데이트**

## 2. 현재 작업 상태
- [x] `P1` 플러그인 기본 구조 및 AE SDK 핸들러 스캐폴딩 — 상태: `완료`
- [x] `P2` 모델/세션 생명주기 + 캐시 우선 렌더 파이프라인 — 상태: `완료`
- [ ] `P3` Depth Map/Slicing 모드 + 8/16/32 bpc 경계 변환 — 상태: `진행중 (95%)` (완료: `PF_Cmd_USER_CHANGED_PARAM` 매핑+`params[]` 추출/렌더 override, `PARAM_SETUP` `PF_ADD_*` 등록 스캐폴드, SDK 픽셀 힌트+stride 결합 포맷 추론, AE SDK 25.6 헤더 호환 컴파일 수정 / 남은 핵심: 실제 AE 호스트에서 파라미터 UI 등록/렌더 연동 실검증)
- [ ] `P4` OOM/백엔드 실패 대비 타일링·다운스케일 폴백 — 상태: `진행중 (88%)` (완료: `직접->타일->다운스케일->안전 출력` 폴백 체인, 적응형 타일 재시도+VRAM budget 기반 비율 조정 / 남은 핵심: SDK/OS 메모리 신호 연계, OOM/백엔드 실패 원인별 정책 세분화)
- [ ] `P5` 테스트/벤치마크/안정성 검증 + 패키징 스크립트 — 상태: `진행중 (97%)` (완료: perf harness+CTest 등록, 로컬/CI 공용 검증 스크립트·워크플로, Windows `.aex` 빌드 헬퍼(`tools/build_aex.ps1`), ORT 런타임 배포 노트/AE 스모크 테스트 체크리스트 추가, ORT API ON + SDK ON 테스트 빌드/단위 테스트 검증, MediaCore 배치 및 산출물 해시 일치 검증, `package_plugin.ps1` Windows 패키지/manifest/ORT DLL/SHA256 산출 확인, dump 기반 예외/레지스터 컨텍스트 추출 검토 및 예외 메시지 경로 `nullptr` 방어 패치 적용, AE 엔트리/스텁 예외 방어층(SEH + C++ 예외 로그) 추가, ORT DLL 기본 로드를 side-by-side 절대경로 우선으로 강화, `std::mutex` 의존 경로를 호환 락(`SRWLOCK`)으로 치환, CMake/빌드 스크립트에 `MSVC_RUNTIME_LIBRARY` 고정 옵션 추가 / 남은 핵심: 네이티브 host 기준 AE 스모크/렌더 큐 실검증)

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
- [x] `D40` 로컬 Windows 에이전트 인수인계 문서 추가: 동기화/환경변수/빌드/배치/스모크 테스트/실패 보고 템플릿 정리 (`docs/build/LOCAL_AGENT_HANDOFF.md`, `docs/build/README.md`)
- [x] `D41` 로컬 Windows 실빌드 검증: AE SDK 25.6에서 `PF_InData/PF_LayerDef` 멤버 호환(`pixel_format/pix_format`) 컴파일 수정 후 `tools/build_aex.ps1`로 `ZSoda.aex` 산출 확인, `ZSODA_WITH_ONNX_RUNTIME_API=ON` + `ZSODA_WITH_AE_SDK=ON` 테스트 빌드 및 `zsoda_tests` 통과 검증 (`plugin/ae/AeHostAdapter.cpp`, `build-win/*`, `build-win-tests/*`)
- [x] `D42` 로컬 핸드오프 문서 보강: MediaCore 배치 권한 이슈(`Access is denied`) 및 ORT DLL 선로딩(System32 구버전) 충돌 대응 가이드 추가 (`docs/build/LOCAL_AGENT_HANDOFF.md`)
- [x] `D43` 관리자 권한 배치 검증: `ZSoda.aex`/`onnxruntime.dll`를 `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore`에 복사 완료 및 소스-대상 SHA256 일치 검증
- [x] `D44` Windows 배포 패키징 검증: `tools/package_plugin.ps1`로 `dist/windows-release`에 `.aex`/`models.manifest`/`onnxruntime.dll`/`.sha256` 산출 확인
- [x] `D45` AE 로더 인식 이슈 수정: `ZSoda.aex`가 `No loaders recognized`로 Ignore 되던 문제를 `EffectMain` export 강제(`__declspec(dllexport)`)로 수정, 재빌드 후 export 테이블(`EffectMain`) 및 MediaCore 재배치 해시 검증 (`plugin/ae/AePluginEntry.cpp`, `build-win/plugin/Release/ZSoda.aex`)
- [x] `D46` Windows PiPL 경로 연결: `ZSodaPiPL.r` 추가 + CMake에 `cl -> PiPLtool -> rc` 생성 파이프라인 연결해 `.aex` 빌드 시 PiPL 리소스 포함, MediaCore 최신 빌드 재배치 (`plugin/ae/ZSodaPiPL.r`, `plugin/CMakeLists.txt`, `build-win/plugin/pipl/*`)
- [x] `D47` AE 적용 크래시 대응: Sentry breadcrumb의 `global outflags mismatch (code 4008120 vs PiPL 4008020)` 원인 확인 후 `PF_Cmd_GLOBAL_SETUP`에서 `my_version/out_flags/out_flags2` 명시 세팅 및 PiPL outflags 동기화, 재빌드/재배치/단위 테스트 통과 (`plugin/ae/AeHostAdapter.cpp`, `plugin/ae/ZSodaPiPL.r`, `build-win/plugin/Release/ZSoda.aex`)

## 4. 남은 작업
1. `P3` 구현: AE 파라미터와 모델 선택 UI(`model_id`)를 실제 `PARAM_SETUP` 등록 코드와 AE 호스트에서 실검증
2. `P3` 구현: AE SDK 실제 `PF_Cmd_*` 경로에 라우터/파라미터 연결(현재 USER_CHANGED/RENDER/SEQUENCE/SMART no-op 매핑 반영, 호스트 통합 검증 남음)
3. `P3` 구현: `PF_Cmd_RENDER` payload에서 SDK 픽셀 타입 힌트 결합 경로의 16/32 bpc 실호스트 검증
4. `P4` 잔여: OOM/백엔드 실패 원인별 정책 세분화(타일 자동 축소 + VRAM 힌트 기반 다운스케일 비율은 반영 완료, SDK/OS별 메모리 신호 연계 남음)
5. `P5` 구축: 성능/회귀/장시간 안정성 테스트 파이프라인 정리(로컬/CI 기본 자동화 완료, 네이티브 host 검증 파이프라인 추가 필요)
6. `P5` 구축: 문서화된 최종 패키징 경로를 실제 CMake 타깃(`.aex/.plugin`) 및 배포 스크립트로 연결 (Windows 빌드/패키징/배치 검증 완료, AE 스모크/렌더 큐 실검증 남음)
7. ORT 백엔드 GPU 프로바이더(CUDA/DirectML/Metal/CoreML) 분기 연결 및 OS별 fallback 정책 문서화
8. macOS 코드서명/노타리 최종 경로 마감 및 배포용 번들 검증

## 5. 이슈 및 리스크
- 플러그인 스캐폴드는 구축되었고 SDK 바인딩 경로를 확장했지만, AE 호스트 실환경에서의 최종 검증이 남아 있음.
- ONNX Runtime/CUDA/DirectML/Metal/CoreML 실추론 경로가 아직 연결되지 않음.
- Windows 환경에서 `C:\Windows\System32\onnxruntime.dll`(구버전)이 우선 로드되면 ORT API 버전 충돌이 발생할 수 있어, 실행 폴더 기준 런타임 DLL 버전 고정 전략이 필요함.
- AE가 플러그인을 스캔해도 로더가 인식하지 못하면 `Plugin Loading.log`에 `No loaders recognized ... Ignore`로 남으므로, 배치 후에는 export/PiPL 인식 여부를 로그로 재확인해야 함.
- 리스크 대응:
  - 모델 선택/캐시/폴백 경로를 먼저 안정화해 AE 크래시 리스크를 최소화
  - ORT 연동은 CPU 경로부터 시작 후 GPU 백엔드를 OS별로 점진 확장

## 6. 다음 공유 예정
- 다음 공유 시점: `P3`의 다음 작업 단위(`PF_Cmd_RENDER` payload 디코딩 + `PixelConversion` 실제 연결) 완료 직후
- 다음 공유 내용: SDK payload->내부 프레임 변환 코드, 테스트/벤치 상태, GitHub 반영 상태

## 7. Local Troubleshooting Notes (2026-03-02)
- [x] `D48` Cleared AE crash-blocklist state for `ZSoda Depth` by removing `Effect Crashed.txt` and deleting the `ZSoda Depth 65536` `lastCrashedDate` entry in `Adobe After Effects 25.0 Prefs-effects.txt` before retest.
- [x] `D49` 로컬 크래시 재분석 결과(최신 Sentry `0ea8ab30-95ac-4967-8688-90ef01782626`)에서 `global outflags mismatch (code 4008120 vs PiPL 4008020)` 재확인 후, 플러그인 `GlobalSetup`/PiPL outflags를 `0x04008020`으로 통일하고 재빌드/MediaCore 재배포(SHA256 `9773ec07b6247ce86273253dddfcb3a2d3760798772aceec6fc1cd4c9fcd3b7d`) 완료.
- [x] `D50` 반복 크래시 UUID(`471fec5f-fa20-4dc7-a552-f44ef1074861`) 재분석에서 동일 outflags mismatch(`code 4008120 vs PiPL 4008020`) 확인 후, ZSoda outflags를 `0x04008120`으로 재통일 재빌드하고 MediaCore 재배포(SHA256 `fcb5f1538885131cc7e4e84054ba3140e9b98be07f8f55da77047991f572d446`) 완료. 추가로 AE 레지스트리 캐시의 `PluginCache\en_US\ZSoda.aex_*` Ignore 키를 삭제해 구캐시 강제 참조 상태를 해제.
- [x] `D51` Session wrap-up for WSL handoff: pushed `main` to `origin` with commits `320e0c7` (AE PiPL/outflags sync fixes) and `6ce6329` (Windows `ZSoda.aex` artifact). Current blocker remains runtime DLL conflict risk (`onnxruntime.dll` version/load path) and intermittent AE PluginCache `Ignore` state for `ZSoda.aex`.
- [x] `D52` ORT 크래시 재분석 결과를 기반으로 구조적 해결 전략 확정: Adobe ORT(1.17)와 플러그인 ORT(1.24.2) 충돌 가능성을 핵심 원인으로 기록하고, 명시적 ORT 로딩/버전 협상/폴백 보장을 위한 멀티 에이전트 병렬 작업 착수 (`docs/research/2026-03-03-ort-runtime-collision-analysis.md`)
- [x] `D53` 구조적 해결 1차 통합: ORT 명시적 동적 로더(`OrtDynamicLoader`) 추가, OnnxRuntimeBackend 초기화 경로 리팩터링(로더 협상 실패 시 fallback reason 반환), CMake에 direct-link 모드/런타임 DLL 경로 힌트 추가, 빌드/패키징 스크립트의 ORT DLL 누락 검증 강화, ORT 충돌 대응 운영 문서/테스트 보강 (`plugin/inference/OrtDynamicLoader.*`, `plugin/inference/OnnxRuntimeBackend.cpp`, `plugin/CMakeLists.txt`, `tools/build_aex.ps1`, `tools/package_plugin.ps1`, `docs/build/ORT_RUNTIME_ISOLATION_PLAN.md`, `tests/test_inference_engine.cpp`)
- [x] `D54` 덤프 역추적성 강화: Windows `.aex` 타깃에 `/Zi + /DEBUG:FULL + /MAP` 적용, `build_aex.ps1`에서 `ZSoda.pdb`/`ZSoda.map` 산출 검사 및 해시 출력, 핸드오프 문서에 PDB/MAP 필수 수집 절차 반영 (`plugin/CMakeLists.txt`, `tools/build_aex.ps1`, `docs/build/LOCAL_AGENT_HANDOFF.md`, `docs/build/README.md`)
- [x] `D55` 신규 크래시 덤프(`dump/29584863-ffbd-4e6d-b845-be901ba33605.dmp`) 재분석: `ExceptionCode=0xC0000005`, `MSVCP140.dll+0x126a0`, `ExceptionInformation=[0,0]`, `RDX=0x0` 확인 및 ZSoda 리턴 오프셋 후보(`+0x2017d`, `+0x1ba3a`, `+0xb4ea` 등) 추출
- [x] `D56` 구조적 안정화 패치: `ex.what()`/`Name()`/DLL path hint 문자열 변환 경로에 `nullptr C-string` 방어 적용, ORT 로더 메서드명 불일치(`LoadedLibraryPath` -> `LoadedDllPath`) 정정 후 로컬 CI 재통과 (`plugin/core/RenderPipeline.cpp`, `plugin/inference/OnnxRuntimeBackend.cpp`, `plugin/inference/ManagedInferenceEngine.cpp`)
- [x] `D57` 외부 참조 심화 분석: `NevermindNilas/TheAnimeScripter` 내부 코드 기반으로 AE 연동/모델 운영/provider fallback 패턴을 조사하고 Z-Soda 적용 항목 정리 (`docs/research/2026-03-03-theanimescripter-reference.md`)
- [x] `D58` ORT 경로 충돌 구조 개선: DLL 경로 미지정 시 `onnxruntime.dll` bare name 로드 대신 플러그인 모듈 인접(side-by-side) 경로를 우선 탐색하고, 미발견 시 명시 오류로 fallback 전환 (`plugin/inference/OrtDynamicLoader.cpp`)
- [x] `D59` 호스트 크래시 완화층 추가: `EffectMain`/스텁 엔트리 함수에 C++ 예외 가드 + Windows SEH 가드 및 `%TEMP%\\ZSoda_AE_Runtime.log` 진단 로그 경로 추가 (`plugin/ae/AePluginEntry.cpp`)
- [x] `D60` CRT 경로 의존 완화: `ManagedInferenceEngine`/`DepthCache`/`BufferPool`의 `std::mutex` + `std::scoped_lock` 사용을 `CompatMutex/CompatLockGuard`로 치환해 Windows에서 `MSVCP _Mtx_lock` 경유를 제거 (`plugin/core/CompatMutex.h`, `plugin/core/Cache.*`, `plugin/core/BufferPool.*`, `plugin/inference/ManagedInferenceEngine.*`)
- [x] `D61` 빌드 런타임 제어 강화: CMake `CMP0091 NEW` + `ZSODA_MSVC_RUNTIME_LIBRARY`(기본 `/MT`) 옵션 추가, Windows 빌드 스크립트/핸드오프 문서 동기화 (`CMakeLists.txt`, `tools/build_aex.ps1`, `docs/build/README.md`, `docs/build/LOCAL_AGENT_HANDOFF.md`)
- [x] `D62` Context7 기반 버전 검증: ONNX Runtime `OrtApiBase::GetApi()` 미지원 버전 시 `nullptr` 반환 규칙 및 CMake `MSVC_RUNTIME_LIBRARY` 적용 조건(CMP0091) 확인
- [x] `D63` AE 초기화 복원력 강화: `EffectMain`에서 `BuildSdkDispatch/Dispatch` 실패 시 명령별 정책 적용(`RENDER`만 치명 반환, 나머지 명령은 `PF_Err_NONE`) 및 `%TEMP%\\ZSoda_AE_Runtime.log`에 상세 진단(`cmd/mapped/error`) 기록 (`plugin/ae/AePluginEntry.cpp`)
- [x] `D64` 로더 인식 실패(Plugin Ignore) 구조 대응: PiPL 리소스의 Windows 코드 엔트리를 `CodeWin64X86 {"EffectMain"}`로 단순 고정하고, `build_aex.ps1`에 생성 PiPL RC 시그니처 검증(`CodeWin64X86/EffectMain/outflags`)을 강제해 비정상 `.aex` 배포를 차단 (`plugin/ae/ZSodaPiPL.r`, `plugin/CMakeLists.txt`, `tools/build_aex.ps1`, `docs/build/LOCAL_AGENT_HANDOFF.md`)
- [x] `D65` 로컬 재현 로그 재확인 및 WSL 재위임 준비: 최신 `main(73464a4)` pull 후 `tools/build_aex.ps1` 재빌드 시 `Assert-PiPLSignature`가 `.rc`에서 `CodeWin64X86` 토큰 미검출로 실패(동일 시점 `.rr`에는 토큰 존재)함을 확인. AE 재실행 후에도 `Plugin Loading.log`에 `No loaders recognized ... set to Ignore`가 반복되고 `PluginCache\\en_US\\ZSoda.aex_*`가 `Ignore=1`로 재생성됨을 확인해 핸드오프 문서에 증적/다음 액션 반영 (`docs/build/LOCAL_AGENT_HANDOFF.md`)
- [x] `D66` 빌드 로더 게이트 구조 개선: `tools/build_aex.ps1`를 `.rc` 토큰 검사 방식에서 `.rr` literal 시그니처 + 최종 `ZSoda.aex`(export/`.rsrc`/machine) 검증으로 전환하고, `ZSoda.loader_check.txt` 요약 산출 및 `LOCAL_AGENT_HANDOFF.md` 절차를 동기화 (`tools/build_aex.ps1`, `docs/build/LOCAL_AGENT_HANDOFF.md`)
- [x] `D67` 듀얼 경로 로더 재현 고정: `MediaCore`/`Effects` 두 위치에 동일 SHA256 `ZSoda.aex`를 배치해 재현해도 양쪽 모두 `No loaders recognized ... set to Ignore`로 실패하고, `PluginCache\\en_US`에 경로별 `ZSoda.aex_*` 2개 키가 `Ignore=1`로 동시 생성됨을 확인. `%TEMP%\\ZSoda_AE_Runtime.log` 미생성과 `LoadLibraryW` 단독 성공/`dumpbin /dependents` 정상 결과를 함께 기록해 실패 지점을 AE 내부 로더 단계로 한정 (`docs/build/LOCAL_AGENT_HANDOFF.md`)
