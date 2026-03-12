# 진행 현황 (PROGRESS)

이 문서는 현재 shipping 기준과 최근 작업 단위를 빠르게 공유하기 위한
한국어 상태판이다. 과거 상세 실험 이력은 git history와 `docs/research/`
기준으로 확인한다.

## 1. 현재 제품 기준
- 공개 effect 이름: `Z-Soda`
- internal match name: `Z-Soda Depth Slice`
- 고정 모델: `distill-any-depth-base`
- 기본 런타임: local Python remote service + binary localhost transport
- 공개 UI:
  - `Quality`
  - `Preserve Ratio`
  - `Output`
  - `Color Map`
  - `Slice Mode`
  - `Position (%)`
  - `Range (%)`
  - `Soft Border (%)`
- `Color Map`은 `Depth Map`에서만 의미가 있으며 현재 `Gray`, `Turbo`,
  `Viridis`, `Inferno`, `Magma`를 제공한다.

## 2. 현재 작업 상태
- AE slice slider arrow 입력 크래시는 해결된 상태다.
- depth render / depth slice render는 현재 기준으로 정상 동작한다.
- `Depth Map` 전용 컬러맵 옵션(`Gray`, `Turbo`) 추가를 완료했다.
- Debug 테스트 바이너리(`zsoda_tests.exe`)는 현재 통과한다.
- Release AEX 빌드(`zsoda_aex`)도 현재 통과한다.

## 3. 현재 남은 확인 포인트
1. AE 실기에서 `Color Map` 전환 시 `Depth Map` 표시가 의도대로 보이는지 최종 확인
2. 필요 시 `Turbo` 외 추가 palette를 넣을지 결정
3. 문서/스모크 테스트 기준을 새 8-control UI 기준으로 유지

## 4. 최근 작업 로그

### D224 (2026-03-12)
- 깨진 `PROGRESS.md` 상단 상태판을 버리고 현재 shipping baseline 기준으로 다시 세우는 정리를 시작했다.
- 상단에는 현재 기준, known issue, 다음 우선순위만 남기고 과거 상세는 git history 기준으로 보기로 정리했다.

### D225 (2026-03-12)
- AE crash 조사 과정에서 남아 있던 진단용 dead branch를 정리했다.
- `plugin/ae/AeDiagnostics.h`를 추가하고 `%TEMP%\\ZSoda_AE_Runtime.log` 파일 로그는
  `ZSODA_AE_DIAGNOSTICS=1` 또는 `ZSODA_AE_TRACE=1`일 때만 켜지도록 묶었다.
- 비활성화돼 있던 render/commit bypass 분기를 제거했다.

### D226 (2026-03-12)
- build/handoff/smoke 문서를 현재 shipping slice UX 기준으로 다시 맞췄다.
- `README.md`, `PLAN.md`, `docs/build/README.md`,
  `docs/build/LOCAL_AGENT_HANDOFF.md`, `docs/build/AE_SMOKE_TEST.md`를
  `Quality / Preserve Ratio / Output / Slice Mode / Position (%) / Range (%) / Soft Border (%)`
  기준으로 갱신했다.

### D227 (2026-03-12)
- cleanup pass 이후 Debug/Release 빌드를 다시 확인했다.
- `cmake --build build-cleanup --config Debug --target zsoda_tests`와
  `cmake --build build-win --config Release --target zsoda_aex`가 통과했다.

### D228 (2026-03-12)
- `main` CI red 원인을 stale test expectation으로 정리했다.
- `tests/test_inference_engine.cpp`와 `tests/test_render_pipeline.cpp`의
  현재 fallback/temporal 계약과 맞지 않던 expectation을 수정했다.
- `bash tools/run_local_ci.sh`를 끝까지 통과시켜 local CI green을 복구했다.

### D229 (2026-03-12)
- `Depth Map` 전용 `Color Map` popup(`Gray`, `Turbo`)을 추가했다.
- core render path에 false-color 출력 경로를 넣고, `Depth Slice`에서는
  컬러맵이 no-op이 되도록 cache key와 rerender 판단도 함께 정리했다.
- AE 파라미터 표면, SDK param extraction, router trace, build/smoke 문서를
  새 8-control UI 기준으로 갱신했다.
- 검증:
  - `cmake --build build-cleanup --config Debug --target zsoda_tests -- /m:1 /p:UseMultiToolTask=false /p:CL_MPCount=1`
  - `build-cleanup\\tests\\Debug\\zsoda_tests.exe`
  - `cmake --build build-win --config Release --target zsoda_aex -- /m:1 /p:UseMultiToolTask=false /p:CL_MPCount=1`

### D230 (2026-03-12)
- 최신 Release 빌드 `build-win\\plugin\\Release\\ZSoda.aex`를 MediaCore에 배포했다.
- 기존 MediaCore 플러그인은 `C:\\Program Files\\Adobe\\Common\\Plug-ins\\7.0\\MediaCore\\ZSoda.aex.bak-20260312-232017`로 백업했다.
- 배포 후 대상 `ZSoda.aex`의 SHA256은 빌드 산출물과 동일한
  `87B1DCF4CE63DEBF8D4C4F684E6D33710A700D31CACC63919E9C3C7C1E209D92`로 확인했다.

### D231 (2026-03-12)
- `Color Map` preset을 `Gray`, `Turbo`, `Viridis`, `Inferno`, `Magma`까지 확장했다.
- core false-color palette lookup과 AE popup label/enum clamp를 같은 순서로 맞췄다.
- false-color preset 전체가 색 출력으로 나오는지 테스트를 확장해 다시 확인했다.

### D232 (2026-03-12)
- preset 확장본 Release 빌드 `build-win\\plugin\\Release\\ZSoda.aex`를 MediaCore에 다시 배포했다.
- 기존 MediaCore 플러그인은 `C:\\Program Files\\Adobe\\Common\\Plug-ins\\7.0\\MediaCore\\ZSoda.aex.bak-20260312-232516`로 백업했다.
- 배포 후 대상 `ZSoda.aex`의 SHA256은
  `F3F9E3C30D7530D409EFAAD97E1DC908558C3060885CDA98896E5E31CF923DD8`로 확인했다.

### D233 (2026-03-13)
- Apple Silicon macOS용 AE 빌드/배포 준비를 위한 handoff 문서
  `docs/build/MAC_SILICON_HANDOFF.md`를 추가했다.
- 현재 repo 기준으로 이미 준비된 항목, 실제 blocker, 권장 구현 순서,
  패키징/서명/노타리제이션, 성능 최적화 포인트를 한 문서에 묶었다.
- Adobe/Apple/ONNX Runtime/PyTorch 공식 참고 링크와 함께 mac bring-up에
  바로 필요한 `PiPL`, `EffectMain` export, MediaCore 경로, `mps`/CoreML
  검토 지점을 정리했다.
