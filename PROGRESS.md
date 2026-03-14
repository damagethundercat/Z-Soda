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

### D234 (2026-03-13)
- mac bundle 기준 asset discovery를 정리했다.
- `plugin/inference/RuntimePathResolver.cpp`에 `dladdr` 기반 non-Windows
  module directory 해석을 넣고 `Contents/MacOS -> Contents/Resources`
  레이아웃에서 `models`, `zsoda_py`, `zsoda_ort`를 찾도록 확장했다.
- `plugin/inference/EngineFactory.cpp`와
  `plugin/inference/RemoteInferenceBackend.cpp`의 service script 탐색도
  같은 search root 규칙으로 맞췄다.
- `tools/package_plugin.sh`와 `tools/package_plugin.ps1`를 갱신해
  Windows는 패키지 루트, macOS는 `ZSoda.plugin/Contents/Resources/`
  아래에 `models/`, `zsoda_py/`, `zsoda_ort/`를 stage하도록 정리했다.
- `tools/build_plugin_macos.sh`를 추가해 configure/build/package/verify
  그리고 선택적 MediaCore copy까지 한 번에 처리할 수 있게 했다.
- mac remote helper의 device 선택에 `mps` auto/validation을 추가했고,
  non-Windows remote service autostart 경로도 붙였다.
- 검증:
  - `bash tools/run_local_ci.sh`
  - `cmake --build build-mac --config Release --target zsoda_plugin_bundle`
  - `bash tools/package_plugin.sh --platform macos --build-dir build-mac --output-dir dist-mac --include-manifest`
  - `file build-mac/plugin/Release/ZSoda.plugin/Contents/MacOS/ZSoda`
  - `nm -gU build-mac/plugin/Release/ZSoda.plugin/Contents/MacOS/ZSoda | rg _EffectMain`
  - `otool -l dist-mac/ZSoda.plugin/Contents/MacOS/ZSoda | rg -A4 "LC_BUILD_VERSION|LC_VERSION_MIN_MACOSX"`

### D235 (2026-03-13)
- `plugin/inference/RemoteInferenceBackend.cpp`의 non-Windows 경로에
  localhost HTTP client를 추가해 mac에서도 helper autostart health check와
  remote inference transport가 실제로 컴파일/실행 가능한 형태가 되도록 정리했다.
- `tools/package_plugin.sh`와 `tools/package_plugin.ps1`에 배포용 archive
  생성(`ZSoda-macos.zip`, `ZSoda-windows.zip`)과 archive sha256 출력을 추가했다.
- mac shell packager의 archive 생성에서 AppleDouble `._*` 메타파일이 섞이지
  않도록 `ditto --norsrc`/`COPYFILE_DISABLE=1` 경로로 다듬었다.
- `bash tools/build_plugin_macos.sh --ae-sdk-root "...AfterEffectsSDK..." --build-dir build-mac --output-dir dist-mac`
  를 다시 끝까지 돌려 `dist-mac/ZSoda.plugin`, `dist-mac/ZSoda-macos.zip`,
  `dist-mac/ZSoda-macos.zip.sha256` 산출과 `arm64`, `minos 12.0`,
  `_EffectMain`, ad-hoc codesign을 재확인했다.
- 확인 결과 현재 mac 배포 blocker는 bundle payload staging이 아니라
  `PiPL/resource wiring`, `AE 실제 로드 smoke`, `embedded Python/runtime`,
  `first-run model preseed 정책` 쪽으로 좁혀졌다.

### D236 (2026-03-13)
- `dist-mac/ZSoda.plugin`을 실제 mac MediaCore 경로
  `/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/ZSoda.plugin`
  에 설치했다.
- 설치 경로는 root 권한이 필요했고, macOS TCC 때문에 `Documents` 아래
  원본을 직접 root가 읽지 못해 `/tmp/zsoda_mediacore_stage/ZSoda.plugin`
  으로 staging 후 관리자 권한 복사를 수행했다.
- 설치 후 대상 번들의 `Contents/MacOS/ZSoda`가 `Mach-O 64-bit bundle arm64`
  인 것과 `Contents/Resources/models`, `Contents/Resources/zsoda_py`가
  MediaCore 아래에도 그대로 들어간 것을 다시 확인했다.

### D237 (2026-03-13)
- mac에서 effect 검색이 되지 않던 원인을 `PiPL` resource 누락으로 좁혔다.
- `plugin/ae/ZSodaPiPL.r`에서 `AE_Effect.h` include를 제거해 Adobe SDK 예제와
  같은 PiPL 전처리 표면으로 맞췄고, `plugin/ae/ZSodaAeFlags.h`에는 Rez 경로용
  literal fallback outflags를 추가했다.
- `plugin/CMakeLists.txt`의 mac build에 `xcrun Rez` post-build 단계를 넣어
  `ZSoda.plugin/Contents/Resources/ZSoda.rsrc`를 직접 생성하도록 정리했다.
- 검증:
  - `xcrun DeRez -useDF build-mac/plugin/Release/ZSoda.plugin/Contents/Resources/ZSoda.rsrc`
  - `xcrun DeRez -useDF dist-mac/ZSoda.plugin/Contents/Resources/ZSoda.rsrc`
  - `codesign --verify --deep --strict build-mac/plugin/Release/ZSoda.plugin`

### D238 (2026-03-13)
- mac effect bundle 검색 실패 원인을 추가로 `Info.plist`/`PkgInfo` 메타데이터
  불일치로 좁혔다. 빌드 산출물은 일반 `BNDL` 번들처럼 보였고, Adobe SDK 예제는
  `CFBundlePackageType=eFKT`, `CFBundleSignature=FXTC`를 사용하고 있었다.
- `plugin/ae/Info.plist.in`에 AE effect용 bundle metadata를 반영했고,
  `plugin/CMakeLists.txt`에는 Xcode target build setting
  (`PRODUCT_BUNDLE_PACKAGE_TYPE`, `INFOPLIST_KEY_CFBundlePackageType`,
  `INFOPLIST_KEY_CFBundleSignature`, `INFOPLIST_KEY_LSRequiresCarbon`)을 추가해
  최종 `ProcessInfoPlist` 단계에서도 값이 유지되도록 정리했다.
- `cmake -S . -B build-mac -G Xcode ...`, `cmake --build build-mac --config Release --target zsoda_plugin_bundle`,
  `bash tools/package_plugin.sh --platform macos --build-dir build-mac --output-dir dist-mac --include-manifest`
  를 다시 수행해 `build-mac/plugin/Release/ZSoda.plugin`과
  `dist-mac/ZSoda.plugin` 모두 `eFKT/FXTC`, `LSRequiresCarbon=1`,
  `PkgInfo=eFKTFXTC`를 확인했다.
- 최신 번들을 앱 내부
  `/Applications/Adobe After Effects 2026/Plug-ins/Effects/ZSoda.plugin`
  와 공용
  `/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/ZSoda.plugin`
  두 경로에 모두 재설치했다.

### D239 (2026-03-13)
- mac에서 effect는 정상 로드되지만 최초 실행 후 지연 끝에 더미 엔진으로
  떨어지는 현상을 helper runtime 쪽으로 좁혔다.
- 현재 배포는 `zsoda_py/distill_any_depth_remote_service.py` 스크립트만 포함하고
  Python 의존성은 번들하지 않기 때문에, host Python 환경이 실제 추론 경로를
  결정한다는 점을 다시 확인했다.
- `/usr/bin/python3`로 bundled helper를 직접 실행해 본 결과 모델 다운로드 전에
  `transformers` import 단계에서
  `tokenizers>=0.22.0,<=0.23.0 required, found tokenizers==0.20.3`
  ImportError가 발생해 서비스가 기동하지 못하는 것을 재현했다.
- `plugin/inference/RemoteInferenceBackend.cpp`의 Python probe를 `torch` 단독이 아니라
  `torch + transformers + PIL + depth classes`까지 확인하도록 강화했고,
  service autostart health timeout 시 Python/runtime probe 결과와
  `ZSoda_RemoteService.log` tail을 fallback reason에 포함하도록 보강했다.
- `cmake --build build-mac --config Release --target zsoda_plugin_bundle`,
  `bash tools/package_plugin.sh --platform macos --build-dir build-mac --output-dir dist-mac --include-manifest`,
  `python3 -m py_compile tools/distill_any_depth_remote_service.py`
  를 다시 통과시켰다.

### D240 (2026-03-13)
- 배포 목표를 `zip 해제 -> .plugin/.aex 복사 -> AE 즉시 사용` 기준으로 다시 고정하고,
  core render/UI가 아니라 inference/bootstrap/package 레이어를 중심으로
  single-file 배포 기반을 추가했다.
- `plugin/inference/EmbeddedPayload.{h,cpp}`를 추가해 Windows `.aex` 끝에
  붙은 payload footer를 읽고, `%LOCALAPPDATA%\\ZSoda\\PayloadCache\\<sha256>`
  아래로 `models/`, `zsoda_py/`, `zsoda_ort/`를 추출할 수 있게 했다.
- `plugin/inference/RuntimePathResolver.*`, `plugin/inference/EngineFactory.cpp`,
  `plugin/inference/RemoteInferenceBackend.cpp`, `plugin/inference/RuntimeOptions.h`
  를 갱신해 추출된 asset root를 우선 search root로 사용하도록 정리했다.
- `tools/build_embedded_payload.py`를 추가했고,
  `tools/package_plugin.sh`/`tools/package_plugin.ps1`는 Windows 패키징 시
  sidecar 폴더 대신 `ZSoda.aex` 하나에 payload를 append하도록 바꿨다.
- 문서 기준도 현재 release 목표에 맞게 조정했다.
- 검증:
  - `cmake -S . -B build-mac`
  - `cmake --build build-mac --config Release --target zsoda_plugin_bundle`
  - `bash tools/package_plugin.sh --platform macos --build-dir build-mac --output-dir dist-mac --include-manifest`
  - `python3 -m py_compile tools/build_embedded_payload.py tools/distill_any_depth_remote_service.py`
- 확인 메모:
  - mac 번들은 다시 빌드/패키징까지 통과했다.
  - `zsoda_tests` 실행은 이 변경과 별개로 mac에서 `RunAeRouterTests()` 중
    `EXC_BAD_ACCESS`로 죽는 기존 테스트 안정성 이슈가 남아 있다.

### D241 (2026-03-13)
- self-contained release payload 기준을 더 구체화했다.
- `tools/distill_any_depth_remote_service.py`는 이제 `models/hf/<model_id>/`
  로컬 HF snapshot 을 먼저 찾고, 있으면 `from_pretrained(local_path)`로
  로드한다. `models/hf-cache/`가 있으면 Hugging Face cache root로도 사용한다.
- Windows `RemoteInferenceBackend`의 helper autostart도 embedded payload에서
  추출한 `zsoda_py/python.exe` 류 후보를 먼저 탐색하도록 보강했다.
- `tools/package_plugin.sh`, `tools/package_plugin.ps1`,
  `tools/build_plugin_macos.sh`에
  `--python-runtime-dir`, `--model-repo-dir`, `--hf-cache-dir`,
  `--require-self-contained`를 추가해 shipping용 로컬 자산을 명시적으로
  주입할 수 있게 했다.
- 검증:
  - `python3 -m py_compile tools/distill_any_depth_remote_service.py tools/build_embedded_payload.py`
  - helper import smoke로 로컬 `models/hf/distill-any-depth-base` 및
    `models/hf-cache` 해석 확인
  - `cmake --build build-mac --config Release --target zsoda_plugin_bundle`
  - fake portable runtime/model repo를 사용한 mac self-contained package smoke
  - fake portable runtime/model repo를 사용한 windows single-file payload smoke

### D242 (2026-03-13)
- canonical `release-assets/` 준비 경로를 추가했다.
- `tools/prepare_release_assets.py`를 만들어
  `python-macos/`, `python-win/`, `models/`, `hf-cache/`를 한 디렉터리 아래로
  정리하고 `asset-manifest.json`을 남기도록 했다.
- `tools/package_plugin.sh`, `tools/package_plugin.ps1`는 이제 인자를 따로 주지 않아도
  `release-assets/`가 있으면 자동으로 찾아 self-contained packaging 입력으로 사용한다.
- `docs/build/RELEASE_ASSETS.md`에 canonical layout, staged output, 검증 규칙,
  auto-detect packaging 흐름을 정리했다.
- 검증:
  - fake runtime/model/cache로 `prepare_release_assets.py` 실행
  - 생성된 `release-assets/asset-manifest.json` 확인
  - auto-detect 상태에서 mac self-contained package smoke
  - auto-detect 상태에서 windows embedded single-file package smoke

### D243 (2026-03-13)
- 실제 mac self-contained release asset 을 이 작업 디렉터리에 준비했다.
- `astral-sh/python-build-standalone`의 arm64 macOS Python 3.12.13 install-only
  배포본을 내려받아 `release-assets/python-macos`로 정리했고,
  그 안에 `torch==2.10.0`, `transformers==5.3.0`, `Pillow==12.1.1`,
  `huggingface_hub==1.6.0`을 설치했다.
- Hugging Face `lc700x/Distill-Any-Depth-Base-hf` snapshot 을
  `release-assets/models/distill-any-depth-base`로 준비했다.
- `python3 tools/prepare_release_assets.py --output-dir release-assets --macos-python-runtime-dir ... --model-repo-dir ... --clean`
  후 `python3 tools/check_release_readiness.py` 기준 readiness 는 `5/8`이 됐다.
  남은 미완료는 Windows runtime, Windows build, Windows package 뿐이다.
- `bash tools/build_plugin_macos.sh --ae-sdk-root ... --build-dir build-mac --output-dir dist-mac --require-self-contained`
  로 mac 번들을 다시 빌드/패키징했다.
- 결과물:
  - `dist-mac/ZSoda.plugin`
  - `dist-mac/ZSoda-macos.zip`
  - `dist-mac/ZSoda-macos.zip.sha256`
- 패키지 크기 메모:
  - `ZSoda.plugin`: 약 1.1G
  - `ZSoda-macos.zip`: 약 560M
- bundled helper/runtime 실기 smoke:
  - `dist-mac/ZSoda.plugin/Contents/Resources/zsoda_py/python/bin/python3`
    로 helper 를 직접 실행
  - `/status` 기준 `loaded=true`, `loaded_model_id=distill-any-depth-base`,
    `model_repo_is_local=true`, `device=mps`
  - `.cache/test-input.ppm`를 `/zsoda/depth`에 보내 실제 depth 응답까지 확인
  - 첫 실제 depth 요청은 약 2.07s, output 은 `512x384` float depth map 으로 반환됨

### D244 (2026-03-15)
- Windows agent handoff 기준을 `docs/build/LOCAL_AGENT_HANDOFF.md`에 다시 정리했다.
- 새 handoff 문서에는 다음을 반영했다.
  - 현재 배포 계약:
    `zip 해제 -> .plugin/.aex 단일 파일 복사 -> AE 즉시 사용`
  - mac self-contained 경로에서 이미 검증된 범위
  - Windows 쪽 즉시 blocker 3개:
    `release-assets/python-win`,
    `build-win/plugin/Release/ZSoda.aex`,
    `dist/ZSoda-windows.zip`
  - Windows agent가 만들어야 할 runtime/model/build/package/smoke 절차
- 커밋에 포함하면 안 되는 로컬 생성물을 분명히 하기 위해
  `.gitignore`에 `.DS_Store`, `.cache/`, `dist-mac/`, `release-assets/`
  를 추가했다.
