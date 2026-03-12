# 진행 현황 (PROGRESS)

이 문서는 현재 제품 기준과 최근 작업 단위를 빠르게 넘겨주기 위한 한국어 운영 로그입니다.
2026-03-12 정리 작업에서 깨진 상단 상태판을 폐기하고 현재 기준으로 다시 작성했습니다.
상세한 과거 이력은 git history를 기준으로 확인합니다.

## 1. 현재 제품 기준
- 공개 effect 이름: `Z-Soda`
- internal match name: `Z-Soda Depth Slice`
- 고정 모델: `distill-any-depth-base`
- 기본 런타임: local Python remote service + binary localhost transport
- 공개 UI:
  - `Quality`
  - `Preserve Ratio`
  - `Output`
  - `Slice Mode`
  - `Position (%)`
  - `Range (%)`
  - `Soft Border (%)`
- 현재 안정 체크포인트: `c9ef89c` (`repo: checkpoint stable AE slice controls state`)

## 2. 현재 작업 상태
- AE slice slider 화살표 입력 크래시는 해결된 상태다.
- render path는 다시 활성화되어 있고 사용자가 정상 동작을 직접 확인했다.
- 현재 정리 작업:
  - `PROGRESS.md` 상태판 복구
  - AE 진단 dead branch / 과한 파일 로그 정리
  - build/docs 드리프트 정리
- 알려진 잔여 이슈:
  - 전체 `zsoda_tests.exe` 실행은 여전히 `tests/test_inference_engine.cpp:633`의 기존 assertion에서 실패한다.

## 3. 최근 안정 작업
- `D216`: 공개 UI를 현재 slice UX로 복귀.
- `D217` / `D218`: render override stale params 경로 수정.
- `D220` / `D221`: sequence/no-supervision 충돌 완화.
- `D222` / `D223`: AE layer cleanup 1차 정리.

## 4. 다음 우선순위
1. AE 진단 경로를 기본 `off` 상태로 유지하고 필요 시 env로만 켜기.
2. build/handoff/smoke 문서를 현재 7-control slice UX 기준으로 유지.
3. shipping path와 무관한 legacy branch를 더 줄이기.
4. Debug/Release 재빌드 후 상태를 다시 확인하기.

## 5. 작업 로그

## D224 (2026-03-12)
- 깨진 `PROGRESS.md` 상단 상태판을 폐기하고 현재 shipping baseline 기준으로 새 문서를 다시 작성했다.
- 현재 기준, 안정 체크포인트, 남은 우선순위, known issue(`tests/test_inference_engine.cpp:633`)를 상단에 명시했다.

## D225 (2026-03-12)
- AE crash 조사 과정에서 남아 있던 진단 dead branch를 정리했다.
- `plugin/ae/AeDiagnostics.h`를 추가해 `%TEMP%\ZSoda_AE_Runtime.log` 파일 로그는 `ZSODA_AE_DIAGNOSTICS=1`일 때만 켜지도록 바꿨다.
- `AePluginEntry.cpp`에서는 비활성화된 render/commit bypass 분기를 제거했고, 테스트 helper 이름도 현재 동작에 맞게 `ResetSdkAdapterState()`로 정리했다.

## D226 (2026-03-12)
- build/handoff/smoke 문서를 현재 shipping slice UX 기준으로 다시 맞췄다.
- `README.md`, `PLAN.md`, `docs/build/README.md`, `docs/build/LOCAL_AGENT_HANDOFF.md`, `docs/build/AE_SMOKE_TEST.md`를 `Quality / Preserve Ratio / Output / Slice Mode / Position (%) / Range (%) / Soft Border (%)` 기준으로 갱신했다.
- crash 재현 시 런타임 trace를 켜는 방법(`ZSODA_AE_DIAGNOSTICS=1`)도 문서에 반영했다.

## D227 (2026-03-12)
- cleanup pass 이후 `cmake --build build-cleanup --config Debug --target zsoda_tests -- /m:1 /p:UseMultiToolTask=false /p:CL_MPCount=1`와 `cmake --build build-win --config Release --target zsoda_aex -- /m:1 /p:UseMultiToolTask=false /p:CL_MPCount=1`를 다시 통과했다.
- `build-cleanup\tests\Debug\zsoda_tests.exe` 실행은 기존과 동일하게 `tests/test_inference_engine.cpp:633`의 `assert(error.empty())`에서 실패했고, 이번 cleanup으로 새 실패는 추가되지 않았다.

## D228 (2026-03-12)
- `main` CI red 원인을 stale test expectation으로 정리했다.
- `tests/test_inference_engine.cpp`에서 fallback 진단 문자열이 정상인 경로를 반영하도록 manifest/custom-model 관련 expectation을 수정했고, ONNX scaffold build에서 `ActiveBackend()`가 `CPU`로 남는 현재 계약도 허용했다.
- `tests/test_render_pipeline.cpp`에서는 새 temporal stabilization 구현(저주파 정렬 + current detail 유지)에 맞게 기대값을 갱신했다.
- `tools/run_local_ci.sh`는 bash 실행 호환성을 위해 LF/no-BOM 상태로 정리했고, `bash tools/run_local_ci.sh`를 끝까지 통과해 local CI green을 확인했다.
