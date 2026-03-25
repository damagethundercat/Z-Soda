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

### D245 (2026-03-15)
- Windows release lane을 실제로 다시 돌리면서 build/package helper의
  stale blocker를 정리했다.
- `tools/build_aex.ps1`는 현재 `plugin/ae/ZSodaAeFlags.h`의
  symbolic outflags 형식도 읽도록 파서를 보강했다.
- `plugin/CMakeLists.txt`는 mac Rez/xcrun 탐색이 Windows configure에
  새지 않도록 `APPLE` 분기 안으로 옮겼다.
- `plugin/ae/ZSodaAeFlags.h`, `plugin/ae/ZSodaPiPL.r`,
  `plugin/ae/ZSodaLoaderProbePiPL.r`는 Windows `PiPLtool`이 괄호식
  outflags 표현을 거부하지 않도록 literal PiPL outflags 경로를 추가했다.

### D246 (2026-03-15)
- self-contained Windows release asset을 로컬에서 다시 준비했다.
- `C:\\Python313` 설치를 portable runtime source로 검증해
  `release-assets/python-win`에 stage했고,
  Hugging Face `lc700x/Distill-Any-Depth-Base-hf` snapshot 을
  `release-assets/models/distill-any-depth-base`로 준비했다.
- `tools/package_plugin.ps1`와 `tools/package_plugin.sh`는
  `--include-manifest`가 repo `models/` 전체를 중복 embedding 하지 않고
  `models.manifest`/`README.md` metadata만 복사하도록 수정했다.
- `tools/package_plugin.ps1`는 대형 `.aex`에서도 zip 생성이 가능하도록
  Windows archive path를 `Compress-Archive` 대신 `tar -a` 경로로 정리했다.
- 검증:
  - `tools/build_aex.ps1 -AeSdkIncludeDir ... -BuildDir build-win -Config Release`
  - `tools/package_plugin.ps1 -Platform windows -BuildDir build-win -OutputDir dist -IncludeManifest -RequireSelfContained`
  - 결과물:
    - `dist/ZSoda.aex`
    - `dist/ZSoda.aex.sha256`
    - `dist/ZSoda-windows.zip`
    - `dist/ZSoda-windows.zip.sha256`
  - 최종 크기:
    - `ZSoda.aex`: `595,048,992` bytes
    - `ZSoda-windows.zip`: `411,013,971` bytes
  - 최종 SHA256:
    - `ZSoda.aex`: `0ba2738a27b64dace75af911c363f508a97464a20f849f289da74b204f640172`
    - `ZSoda-windows.zip`: `12bc5fd6fe30391d0c3a2b24354ef195132a7aed079cd41e43f9a2ad59406f61`

### D247 (2026-03-15)
- `main`에 Windows release lane fix를 `63b8406`
  (`release: fix windows self-contained packaging`)로 커밋하고 푸시했다.
- 이 머신에 `GitHub CLI`를 설치하고, git credential helper(`manager`)를
  통해 GitHub release API 접근을 확인했다.
- draft release `v0.1.0`을 다시 조회해 기존 `ZSoda-macos.zip` 외에
  `ZSoda-windows.zip`, `ZSoda-windows.zip.sha256`를 asset으로 업로드했다.
- 현재 draft asset 상태:
  - `ZSoda-macos.zip`
  - `ZSoda-windows.zip`
  - `ZSoda-windows.zip.sha256`
- `ZSoda-windows.zip`의 release asset digest는
  `sha256:12bc5fd6fe30391d0c3a2b24354ef195132a7aed079cd41e43f9a2ad59406f61`
  로 로컬 `dist/ZSoda-windows.zip.sha256`와 일치한다.

### D248 (2026-03-16)
- self-contained release zip 크기와 현재 사용자층을 다시 검토한 뒤,
  기본 배포 계약을 `thin plug-in + first-run setup` 방향으로 전환하기로 했다.
- `docs/build/THIN_SETUP_DESIGN.md`에 다음 설계를 정리했다.
  - 첫 실제 effect 사용 시 setup 시작
  - runtime/model bundle manifest + per-user cache layout
  - setup progress를 보여주는 setup slate
  - release path에서 dummy depth fallback 대신 explicit setup state 사용
- `docs/build/README.md`에는 새 설계 문서를 연결했고,
  `PLAN.md`에는 thin bootstrap용 `RF-05` workstream과
  release bootstrap 요구사항을 추가했다.
- 후속 구현 우선순위:
  - native bootstrap manager / manifest / checksum / extract flow
  - `RenderPipeline` setup pending/status card path
  - downloaded cache root를 기준으로 한 remote runtime discovery

### D249 (2026-03-21)
- self-contained cold-start 안정화를 위해 `bundled_asset_root`만 있어도 runtime/script/python을 찾도록 보강했다.
- `RuntimePathResolver`는 `plugin_directory`가 비어도 extracted payload root에서 `models`, `models.manifest`, `onnxruntime.dll`을 탐색한다.
- `EngineFactory`와 `RemoteInferenceBackend`는 `plugin_directory`가 없어도 `zsoda_py/distill_any_depth_remote_service.py`와 bundled Python runtime을 탐색한다.
- `tests/test_runtime_path_resolver.cpp`에 `bundled_asset_root` 단독 케이스를 추가했다.
- `cmake --build`는 다시 통과했지만, 전체 `zsoda_tests.exe`는 기존 `EmbeddedPayload` footer mismatch로 AV가 남아 있어 추가 안정화가 필요하다.

### D250 (2026-03-21)
- self-contained cold-start blocker였던 embedded payload footer/header contract mismatch를 수정했다.
- `plugin/inference/EmbeddedPayload.cpp`는 16-byte padded magic field를 기준으로 footer/header를 읽도록 고쳤다.
- `tools/check_release_readiness.py`에 packaged `ZSoda.aex` embedded payload inspector를 추가했고, `tools/package_plugin.ps1`가 Windows self-contained packaging 중 이를 자동으로 실행하도록 묶었다.
- `tests/test_embedded_payload.cpp`, `tests/test_embedded_payload_main.cpp`, `tests/CMakeLists.txt`로 dedicated embedded payload regression을 추가했고 `zsoda_embedded_payload_tests`는 통과했다.
- 최신 self-contained 산출물은 `artifacts/self-contained-release-fixed-v2/package/ZSoda-windows.zip`에 재생성했다.
- 남은 리스크는 monolithic `zsoda_tests`의 기존 SEGFAULT 1건이며, self-contained payload regression과 package validation gate는 현재 통과한다.

### D251 (2026-03-21)
- self-contained 더미 엔진 fallback의 본질 원인을 `embedded payload overlay`가 아니라 `Windows 경로 길이`로 좁혔다.
- `AfterFX.exe` manifest에 `longPathAware`가 없고, 현재 self-contained payload는 기존 `%LOCALAPPDATA%\\ZSoda\\PayloadCache\\<sha256>` 기준 절대 경로가 최대 273자까지 늘어나는 것을 확인했다.
- `plugin/inference/EmbeddedPayload.cpp`의 Windows 기본 payload cache root를 `%LOCALAPPDATA%\\ZS\\<sha256>`로 줄이고, legacy cache 재사용 fallback과 `PayloadTrace` 진단 로그를 추가했다.
- `tests/test_embedded_payload.cpp`에 Windows short cache root 회귀를 추가했고, `build-origin-main-tests`에서 `ctest -C Release -R zsoda_embedded_payload_tests --output-on-failure`가 통과했다.
- `tools/check_release_readiness.py`가 Windows embedded payload의 최대 상대 경로 길이와 sample cache root 기준 최대 절대 경로 길이를 리포트하도록 보강했다.
- 같은 수정으로 다시 패키징한 self-contained 산출물은 `artifacts/origin-main-self-contained-longpath-fixed/package/ZSoda-windows.zip`이며, packaging gate 기준 `max absolute path length: 257`로 AE host path limit 안쪽까지 내려왔다.
- 추가 조사 결과 self-contained packaging 입력인 `release-assets/**`는 git 밖 자산이며, 현재 rebuild 결과는 source commit보다 이 runtime/model snapshot 상태에 더 크게 좌우된다는 점을 확인했다.

### D252 (2026-03-24)
- self-contained 기준선에서 더미 엔진으로 바뀌는 최신 원인을 `payload extract`가 아니라 `localhost:8345` 고정 포트 충돌로 확인했다.
- `UnicornProService.exe`가 `8345`를 점유한 상태에서 helper가 bind하지 못했고, `ZSoda_AE_Runtime.log`에는 `remote inference service did not become healthy at http://127.0.0.1:8345/status`가 남았다.
- `plugin/inference/RuntimeOptions.h`, `plugin/inference/EngineFactory.cpp`, `plugin/inference/RemoteInferenceBackend.h`, `plugin/inference/RemoteInferenceBackend.cpp`를 수정해 autostart가 고정 `8345` 대신 빈 loopback 포트를 동적으로 배정받고, 성공한 포트를 backend 인스턴스 간에 재사용하도록 바꿨다.
- `tools/distill_any_depth_remote_service.py`는 `--port-file`을 받아 실제 bind된 포트를 기록하도록 보강했고, plugin은 그 파일을 읽어 status/depth endpoint를 동적으로 구성한다.
- 검증:
  - `cmake --build Y:\\build-origin-main-ae --config Release --target zsoda_aex`
  - `cmake --build Y:\\build-origin-main-tests --config Release --target zsoda_embedded_payload_tests`
  - `ctest -C Release -R zsoda_embedded_payload_tests --output-on-failure`
  - `powershell -ExecutionPolicy Bypass -File Y:\\tools\\package_plugin.ps1 ... -RequireSelfContained`
- 최신 self-contained 산출물은 `artifacts/origin-main-self-contained-dynamic-port/package/ZSoda-windows.zip`이며, long-path gate와 embedded payload validation을 모두 통과했다.

### D253 (2026-03-24)
- 맥 정상본 `ZSoda.plugin.zip`을 self-contained golden fixture로 삼고, Windows self-contained payload가 같은 계약을 지키는지 비교 검증하는 gate를 추가했다.
- `tools/check_release_readiness.py`가 이제 macOS fixture `.zip` 또는 `.plugin` 번들을 읽어 `models/**`, `zsoda_py/distill_any_depth_remote_service.py`, `zsoda_py/python/**` 계약을 수집하고 Windows embedded payload와 비교할 수 있다.
- `tools/package_plugin.ps1`, `tools/package_plugin.sh`에 golden fixture 인자를 추가해서, Windows 패키징 직후 같은 명령에서 맥 기준선 비교까지 같이 수행할 수 있게 했다.
- 모델 subtree(`models/hf/distill-any-depth-base/**`)는 hash까지 엄격 비교하고, helper script는 Windows 전용 수정이 있을 수 있어 mismatch를 note로만 남기도록 정리했다.

### D254 (2026-03-24)
- cleanup-first 안정화의 첫 slice로 `RemoteInferenceBackend.cpp`에서 Python runtime candidate 수집, capability probe, autostart selection 로직을 `plugin/inference/PythonAutostart.{h,cpp}`로 분리했다.
- Windows와 macOS/Linux가 각각 유지하던 Python 탐색 우선순위는 그대로 두고, `RemoteInferenceBackend` 쪽에서는 새 helper API만 사용하도록 경계를 좁혔다.
- 같은 Python을 선택한 뒤 launch 직전에 다시 probe하던 중복을 없애기 위해 `PythonAutostartSelection`을 도입했고, 선택 결과와 `probe_output`을 함께 넘기도록 정리했다.
- monolithic `zsoda_tests`만으로는 이 slice를 신뢰하기 어려워서 `tests/test_python_autostart.cpp`와 `zsoda_python_autostart_tests` 타깃을 추가했다.
- 검증:
  - `cmake --build build-origin-main-tests --config Release --target zsoda_tests zsoda_embedded_payload_tests`
  - `cmake --build build-origin-main-tests --config Release --target zsoda_python_autostart_tests zsoda_embedded_payload_tests`
  - `ctest -C Release -R "zsoda_python_autostart_tests|zsoda_embedded_payload_tests" --output-on-failure`
  - `zsoda_embedded_payload_tests`: 통과
  - `zsoda_python_autostart_tests`: 통과
  - `zsoda_tests`: 기존과 동일하게 SEGFAULT 1건이 남아 있어 별도 정리 대상으로 유지
- 다음 cleanup slice는 `RemoteInferenceBackend.cpp`에 아직 남아 있는 Windows/non-Windows별 process launch와 health-check 경로를 별도 launcher helper로 분리하는 것이다.

### D255 (2026-03-24)
- cleanup-first 안정화의 두 번째 slice로 detached remote-service launch, `--port-file` handshake, log-tail 기반 실패 리포트, readiness polling 경로를 `plugin/inference/PythonServiceAutostart.{h,cpp}`로 분리했다.
- `RemoteInferenceBackend.cpp`는 이제 autostart를 직접 실행하지 않고, `PythonServiceLaunchResult`만 받아 endpoint/state를 갱신하도록 줄였다.
- 이 과정에서 `ResolveRemoteServiceScriptPath`, preload model 결정, port-file 대기, service log path/log tail 유틸도 launcher helper 쪽으로 옮겨서 backend 책임을 endpoint/state policy 쪽에 더 가깝게 맞췄다.
- `tests/test_python_autostart.cpp`에 launch helper 조기 실패 경로(스크립트 누락, explicit python unavailable)를 추가해서, 실제 프로세스를 띄우지 않는 범위의 launcher 회귀도 같은 타깃에서 커버하도록 보강했다.
- 검증:
  - `cmake --build build-origin-main-tests --config Release --target zsoda_python_autostart_tests zsoda_embedded_payload_tests`
  - `ctest -C Release -R "zsoda_python_autostart_tests|zsoda_embedded_payload_tests" --output-on-failure`
  - `cmake --build build-origin-main-tests --config Release --target zsoda_tests`
  - `ctest -C Release -R "^zsoda_tests$" --output-on-failure`
  - `cmake --build Y:\\build-origin-main-ae --config Release --target zsoda_aex`
  - `zsoda_python_autostart_tests`: 통과
  - `zsoda_embedded_payload_tests`: 통과
  - `zsoda_aex`: `Y:` 경로 기준 빌드 통과
  - `zsoda_tests`: 기존과 동일하게 SEGFAULT 1건 유지
- 다음 cleanup slice는 monolithic `zsoda_tests`를 쪼개거나 crash 원인을 고정해서, cleanup 회귀를 더 작은 타깃으로 신뢰할 수 있게 만드는 것이다.

### D256 (2026-03-24)
- test 안정화 slice로 monolithic `zsoda_tests`를 `zsoda_core_tests`, `zsoda_ae_params_tests`, `zsoda_ae_router_tests`, `zsoda_inference_tests`, `zsoda_render_tests`로 분리하고, 공통 초기화 코드는 `tests/TestSupport.{h,cpp}`로 모았다.
- 각 테스트 타깃의 Release 빌드에서 `NDEBUG`를 강제로 끄도록 바꿔, cleanup 중 계약 실패가 assert에서 바로 드러나고 Release-only UB/SEGFAULT로 흘러가지 않게 정리했다.
- `ZSODA_TEST_TRACE=1`로 `tests/test_ae_router.cpp`를 추적해 `TestParamSetupAndModelMenu()` 구간을 먼저 좁혔고, 실제 원인이 "assert가 사라진 상태에서 계속 진행되며 잘못된 상태를 역참조하는 패턴"임을 확인했다. `zsoda_ae_router_tests`는 현재 통과한다.
- `plugin/inference/EmbeddedPayload.cpp`에는 Windows에서 1차 cache root 추출이 실패하면 더 짧은 2차 per-user root(`%USERPROFILE%\\ZS`)로 한 번 더 추출을 시도하는 fallback을 넣었다. `tests/test_embedded_payload.cpp`도 긴 `LOCALAPPDATA` + 짧은 `USERPROFILE` 조합을 재현하도록 갱신했다.
- 검증:
  - `cmake -S . -B build-origin-main-tests`
  - `cmake --build build-origin-main-tests --config Release --target zsoda_core_tests zsoda_ae_params_tests zsoda_ae_router_tests zsoda_inference_tests zsoda_render_tests zsoda_python_autostart_tests zsoda_embedded_payload_tests`
  - `ctest -C Release -R "zsoda_(core|ae_params|ae_router|inference|render|python_autostart|embedded_payload)_tests" --output-on-failure`
  - `cmake --build Y:\build-origin-main-ae --config Release --target zsoda_aex`
  - split unit suites: 전부 통과
  - `zsoda_embedded_payload_tests`: 통과
  - `zsoda_python_autostart_tests`: 통과
  - `zsoda_aex`: 통과
- 다음 cleanup slice는 Windows/macOS 패키징 스크립트와 release validation 규약을 하나의 staging spec으로 더 가깝게 모으는 것이다.

### D257 (2026-03-24)
- packaging cleanup slice로 Windows/macOS packager가 공통으로 쓰는 stage 규약을 `tools/package_layout.py`로 끌어올리고, `tools/prepare_package_stage.py`가 `.payload-stage`를 만들고 JSON plan을 내보내도록 정리했다.
- `tools/package_plugin.ps1`와 `tools/package_plugin.sh`는 이제 artifact/source root를 각자 다시 추측하지 않고, shared stage helper가 만든 `artifact_source`, `staged_roots`, `staged_root_paths`를 그대로 사용한다. 즉 `models`, `zsoda_py`, `zsoda_ort`를 무엇을 언제 stage할지에 대한 기준이 한 군데로 모였다.
- `tools/check_release_readiness.py`도 self-contained root 상수와 bundled Python runtime 후보 규약을 shared helper에서 가져오도록 맞췄다. readiness checker와 packager가 서로 다른 root 계약을 따로 들고 있지 않게 한 것이다.
- 검증:
  - `python -m py_compile tools\package_layout.py tools\prepare_package_stage.py tools\check_release_readiness.py`
  - `python tools\prepare_package_stage.py --platform windows --build-dir Y:\build-origin-main-ae --output-dir artifacts\package-stage-smoke --include-manifest --plan-out artifacts\package-stage-smoke\plan.json`
  - `python tools\prepare_package_stage.py --platform macos --build-dir artifacts\fake-mac-stage\build-mac --output-dir artifacts\fake-mac-stage\out --python-runtime-dir artifacts\fake-mac-stage\python-macos --model-repo-dir artifacts\fake-mac-stage\models --require-self-contained --plan-out artifacts\fake-mac-stage\out\plan.json`
  - `powershell -ExecutionPolicy Bypass -File .\tools\package_plugin.ps1 -Platform windows -BuildDir build-origin-main-ae -OutputDir artifacts\package-plugin-smoke -IncludeManifest -PythonRuntimeDir artifacts\fake-package-assets\python-win -ModelRepoDir artifacts\fake-package-assets\models -RequireSelfContained`
  - shared helper(py_compile): 통과
  - Windows stage helper smoke: 통과
  - macOS stage helper smoke(fake bundle 기준): 통과
  - Windows packager smoke(fake self-contained assets 기준): embedded payload validation까지 통과
- 다음 cleanup slice는 아직 남아 있는 PowerShell/shell wrapper 중 dead helper를 더 걷어내고, 가능하면 패키징 smoke를 CI성 전용 스크립트로 묶는 것이다.

### D258 (2026-03-25)
- wrapper cleanup slice로 `tools/package_plugin.ps1`, `tools/package_plugin.sh`에 남아 있던 예전 artifact/stage helper를 걷어내고, 지금은 shared stage plan을 읽는 얇은 wrapper만 남기도록 정리했다.
- 새 smoke runner `tools/run_packaging_smoke.py`를 추가했다. 이 스크립트는 shared helper py_compile, Windows stage helper smoke, fake mac bundle 기준 stage helper smoke, Windows packager smoke를 한 번에 확인한다.
- 검증:
  - `python -m py_compile tools\package_layout.py tools\prepare_package_stage.py tools\check_release_readiness.py tools\run_packaging_smoke.py`
  - `python tools\run_packaging_smoke.py --windows-build-dir build-origin-main-ae --output-dir artifacts\packaging-smoke`
  - `run_packaging_smoke.py`: 통과
  - smoke 산출물: `artifacts\packaging-smoke\windows-package\ZSoda-windows.zip`
- 현재 기준으로 repo-local 패키징 정리는 여기까지로 보고, 다음은 실제 AE에서 smoke를 보는 단계로 넘길 수 있다. 이 머신에는 `bash`가 없어 `package_plugin.sh` 직접 실행은 못 했지만, shared mac stage helper smoke는 통과했다.
### D259 (2026-03-25)
- cleanup 정리 이후 실제 self-contained AE 수동 테스트용 패키지를 다시 생성했다. 기준 빌드는 `build-origin-main-ae`의 최신 `zsoda_aex`이고, 실제 release asset은 `C:\Users\ikidk\Documents\Code\01 Z-Soda\.git-history\release-assets\python-win`, `...\models`를 사용했다.
- `tools/package_plugin.ps1`를 real asset과 golden mac fixture(`C:\Users\ikidk\Downloads\Telegram Desktop\ZSoda.plugin.zip`) 기준으로 다시 실행해 embedded payload validation까지 통과한 Windows 패키지를 만들었다.
- 결과물:
  - `artifacts\ae-manual-test\ZSoda-windows.zip`
  - `artifacts\ae-manual-test\ZSoda.aex`
  - 동일 파일을 사용자 작업 레포 `C:\Users\ikidk\Documents\Code\01 Z-Soda\artifacts\ae-manual-test`에도 복사했다.
- 검증:
  - `cmake --build build-origin-main-ae --config Release --target zsoda_aex`
  - `powershell -ExecutionPolicy Bypass -File .\tools\package_plugin.ps1 -Platform windows -BuildDir .\build-origin-main-ae -OutputDir .\artifacts\ae-manual-test -PythonRuntimeDir C:\Users\ikidk\Documents\Code\01 Z-Soda\.git-history\release-assets\python-win -ModelRepoDir C:\Users\ikidk\Documents\Code\01 Z-Soda\.git-history\release-assets\models -RequireSelfContained -GoldenMacFixture C:\Users\ikidk\Downloads\Telegram Desktop\ZSoda.plugin.zip`
  - `ctest -C Release --output-on-failure` (`build-origin-main-tests`, 9/9 pass)
- 현재 상태는 AE에서 직접 설치/실행 smoke를 시작해도 되는 handoff 지점이다.
### D260 (2026-03-25)
- Windows self-contained가 CPU-only로 돌던 원인을 분리했다. 코어 로직은 이미 `auto -> CUDA if available` 경로였고, 실제 원인은 self-contained에 묶인 `release-assets/python-win`이 `torch==2.10.0+cpu` portable runtime이었다.
- `tools/build_embedded_payload.py`를 스트리밍 쓰기 방식으로 바꿔, CUDA runtime처럼 훨씬 큰 payload도 메모리 한 번에 올리지 않고 `.aex`에 append하도록 보강했다.
- `tools/prepare_release_assets.py`에 runtime probe를 추가해 `asset-manifest.json`에 `torch_version`, `cuda_version`, `cuda_available`, `cuda_device_count`를 남기도록 했고, `tools/check_release_readiness.py`도 그 정보를 출력하도록 맞췄다.
- `tools/distill_any_depth_remote_service.py`는 `/status`와 startup JSON에 runtime 정보를 포함하고, binary depth 응답 헤더에도 `X-ZSoda-Resolved-Device`를 싣도록 보강했다.
- `C:\Users\ikidk\Documents\Code\01 Z-Soda\.git-history\artifacts\runtime-win-cuda-work`에 official PyTorch CUDA wheel(`torch==2.10.0+cu128`)을 설치해 검증했다.
  - 검증 결과: `torch.cuda.is_available() == true`, `device_name = NVIDIA GeForce RTX 4070 Ti`, `resolve_device('auto') == 'cuda'`
- 그 runtime으로 `C:\Users\ikidk\Documents\Code\01 Z-Soda\.git-history\artifacts\release-assets-cuda`를 다시 만들었고, manifest에 `python-win runtime probe: torch 2.10.0+cu128 / cuda 12.8 / cuda_available=true`가 남는 것까지 확인했다.
- 새 CUDA self-contained 패키지를 생성했다.
  - `artifacts\ae-manual-test-cuda\ZSoda.aex`
  - `artifacts\ae-manual-test-cuda\ZSoda-windows.zip`
  - zip 크기: `3,350,164,293` bytes
  - `ZSoda.aex` 크기: `5,321,542,432` bytes
- 같은 패키지를 사용자 작업 레포 `C:\Users\ikidk\Documents\Code\01 Z-Soda\artifacts\ae-manual-test-cuda`에도 복사했고, `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\ZSoda.aex`도 이 CUDA 패키지로 교체했다.
- 설치 전 정리:
  - `HKCU\Software\Adobe\After Effects\26.0\PluginCache\en_US\ZSoda*` 삭제
  - `C:\Users\ikidk\AppData\Local\ZS` 삭제
  - `C:\Users\ikidk\AppData\Local\Temp\ZSoda_*.log` 삭제
- 검증:
  - `python -m py_compile tools\build_embedded_payload.py tools\prepare_release_assets.py tools\distill_any_depth_remote_service.py tools\check_release_readiness.py`
  - `python tools\prepare_release_assets.py --output-dir C:\Users\ikidk\Documents\Code\01 Z-Soda\.git-history\artifacts\release-assets-cuda --windows-python-runtime-dir C:\Users\ikidk\Documents\Code\01 Z-Soda\.git-history\artifacts\runtime-win-cuda-work --model-repo-dir C:\Users\ikidk\Documents\Code\01 Z-Soda\.git-history\release-assets\models`
  - `powershell -ExecutionPolicy Bypass -File .\tools\package_plugin.ps1 -Platform windows -BuildDir .\build-origin-main-ae -OutputDir .\artifacts\ae-manual-test-cuda -PythonRuntimeDir C:\Users\ikidk\Documents\Code\01 Z-Soda\.git-history\artifacts\release-assets-cuda\python-win -ModelRepoDir C:\Users\ikidk\Documents\Code\01 Z-Soda\.git-history\artifacts\release-assets-cuda\models -RequireSelfContained -GoldenMacFixture C:\Users\ikidk\Downloads\Telegram Desktop\ZSoda.plugin.zip`
- 현재 상태는 CUDA self-contained 설치본으로 AE 재확인 가능한 handoff 지점이다.

### D261 (2026-03-25)
- Windows release baseline을 다시 `self-contained CPU`로 고정했다. giant CUDA payload는 실험/검증용으로만 남기고, 실제 shipping 후보는 size와 신뢰도를 둘 다 맞추는 쪽으로 되돌렸다.
- `plugin/inference/EngineFactory.cpp`에서 초기화 실패 시 더 이상 `DummyInferenceEngine`로 갈아타지 않도록 바꿨다. 이제 release build는 backend/setup 실패를 숨기지 않고 `ManagedInferenceEngine` 상태를 그대로 유지한다.
- `plugin/inference/RuntimeOptions.h`, `plugin/inference/ManagedInferenceEngine.cpp`에 `allow_dummy_fallback=false` 기본값을 두고, backend unavailable 시 dummy gradient를 성공처럼 반환하지 않도록 바꿨다. 상태 문자열도 `BackendUnavailable ...`로 바꿔, 로그와 smoke 결과가 실제 runtime 상태를 더 정직하게 반영한다.
- `tools/distill_any_depth_remote_service.py`에 `--validate-bundle` / `--validate-device`를 추가했다. 이 경로는 bundled Python으로 실제 local model repo를 한 번 로드하고 JSON status를 출력한 뒤 종료한다.
- 새 helper `tools/validate_self_contained_runtime.py`를 추가했고, `tools/package_plugin.ps1`, `tools/package_plugin.sh`가 self-contained 패키징 전에 staged runtime을 실제로 검증하도록 묶었다. 이제 packager는 helper script/python/model repo가 “존재하는지”만 보지 않고, `torch + transformers + model load`가 되는지도 확인한다.
- 이 semantic validation으로 2026-03-15 draft release가 왜 실패했는지도 다시 정리됐다. draft는 embedded payload 구조는 맞았지만, runtime payload에 필요한 Python 패키지가 빠진 상태여서 실제 helper/model load 계약을 만족하지 못했다.
- 새 release candidate를 CPU self-contained 기준으로 다시 생성했다.
  - `artifacts\release-self-contained-hardened\ZSoda.aex`
  - `artifacts\release-self-contained-hardened\ZSoda-windows.zip`
  - 동일 산출물을 사용자 작업 레포 `C:\Users\ikidk\Documents\Code\01 Z-Soda\artifacts\release-self-contained-hardened\`에도 복사했다.
- 산출물 크기
  - `ZSoda.aex`: `1,220,119,372` bytes
  - `ZSoda-windows.zip`: `596,877,506` bytes
- 산출물 SHA256
  - `ZSoda.aex`: `9d44a478bc82b9af3bd84753d612b4a60a430198c40d2730cded6e38783ded13`
  - `ZSoda-windows.zip`: `34c1be1fce21db344b8f37ded71ebc379e5bd832e74ddf8a1214e084a31c7967`
- 검증
  - `python -m py_compile tools\distill_any_depth_remote_service.py tools\validate_self_contained_runtime.py tools\package_layout.py tools\prepare_package_stage.py tools\check_release_readiness.py tools\build_embedded_payload.py tools\prepare_release_assets.py`
  - `cmake --build build-origin-main-tests --config Release --target zsoda_inference_tests zsoda_embedded_payload_tests zsoda_python_autostart_tests`
  - `ctest -C Release -R "zsoda_inference_tests|zsoda_embedded_payload_tests|zsoda_python_autostart_tests" --output-on-failure`
  - `cmake --build build-origin-main-ae --config Release --target zsoda_aex`
  - `powershell -ExecutionPolicy Bypass -File .\tools\package_plugin.ps1 -Platform windows -BuildDir .\build-origin-main-ae -OutputDir .\artifacts\release-self-contained-hardened -PythonRuntimeDir C:\Users\ikidk\Documents\Code\01 Z-Soda\.git-history\release-assets\python-win -ModelRepoDir C:\Users\ikidk\Documents\Code\01 Z-Soda\.git-history\release-assets\models -RequireSelfContained -GoldenMacFixture C:\Users\ikidk\Downloads\Telegram Desktop\ZSoda.plugin.zip`
  - `ctest -C Release --output-on-failure` (`build-origin-main-tests`, 9/9 pass)
- 현재 상태는 “배포 가능한 self-contained baseline”을 다시 확보한 지점이다. 다만 GPU + 1GB 안팎 요구를 동시에 만족하는 최종 Windows shipping 경로는 여전히 native ORT/DirectML(or ORT sidecar) 복원 쪽이 더 유망하다.
### D262 (2026-03-25)
- Windows 최종 방향을 `native ORT sidecar + GPU`로 다시 고정하고, `distill-any-depth*` 모델이라는 이유만으로 remote backend를 기본값으로 강제하던 정책을 걷어냈다.
- `plugin/inference/EngineFactory.cpp`는 이제 remote를 명시적으로 요청했을 때만 `preferred_backend=remote`로 잠그고, remote endpoint/script/autostart가 있더라도 기본은 `auto -> ORT 우선`으로 유지한다.
- `plugin/inference/ManagedInferenceEngine.{h,cpp}`는 ORT 우선/remote fallback 순서로 backend를 구성하도록 정리했다. `remote_inference_enabled`는 primary 선택자가 아니라 fallback/dev 허용 신호로만 쓰고, local model asset 누락 진단은 explicit remote가 아닐 때 계속 surface되도록 맞췄다.
- inference 회귀 테스트를 보강했다.
  - `tests/test_inference_engine.cpp`
  - `CreateDefaultEngine()`가 remote fallback이 켜져 있어도 `requested_backend=auto`를 유지하는지
  - explicit remote가 아닐 때 missing local ONNX asset 진단이 계속 보이는지
  - explicit remote일 때만 local asset 누락을 무시하는지
- Windows packager에 `sidecar-ort` package mode를 연결했다.
  - shared stage helper가 `models/` 전체와 `zsoda_ort/`를 sidecar root로 stage
  - `tools/package_plugin.ps1`, `tools/package_plugin.sh`는 Windows에서 `.aex` 옆에 `models/`, `zsoda_ort/`를 복사하고 zip에도 같이 넣는다
  - `.aex` embed는 `embedded-windows` mode에서만 유지된다
- `tools/run_packaging_smoke.py`를 확장해서 `sidecar-ort` smoke를 실제로 돌리게 했다.
  - fake ONNX model + fake `onnxruntime.dll`로 `windows-sidecar-package/ZSoda-windows.zip` 생성
  - zip 안에 `ZSoda.aex`, `models/models.manifest`, `models/distill-any-depth/distill_any_depth_base.onnx`, `zsoda_ort/onnxruntime.dll`가 들어가는 것까지 검사한다
- 실측 결과를 정리했다.
  - official `onnxruntime-gpu==1.24.2` wheel: 약 `207.1 MB`
  - official `onnxruntime-directml==1.24.2` wheel: 약 `25.0 MB`
  - `onnxruntime_providers_cuda.dll` 자체는 unzip 기준 `312,606,240` bytes
  - CUDA EP가 직접 요구하는 DLL은 `cublasLt64_12.dll`, `cublas64_12.dll`, `cufft64_11.dll`, `cudart64_12.dll`, `cudnn64_9.dll`
  - 이전 CUDA PyTorch runtime에서 같은 이름 subset만 봐도 `cublasLt64_12.dll` 674MB, `cublas64_12.dll` 113MB, `cufft64_11.dll` 276MB, `cudnn` 계열 추가 DLL들이 수백 MB라서, NVIDIA용 self-contained sidecar도 자칫하면 다시 1GB를 크게 넘길 수 있다는 점을 확인했다
- 검증
  - `cmake --build build-origin-main-tests --config Release --target zsoda_inference_tests zsoda_core_tests`
  - `ctest -C Release -R "zsoda_inference_tests|zsoda_core_tests" --output-on-failure`
  - `cmake --build build-origin-main-ae --config Release --target zsoda_aex`
  - `python -m py_compile tools\run_packaging_smoke.py tools\package_layout.py tools\prepare_package_stage.py`
  - `python tools\run_packaging_smoke.py --windows-build-dir build-origin-main-ae --output-dir artifacts\packaging-smoke-sidecar`
- 현재 결론
  - ORT-first + sidecar packaging 레일은 코드/테스트/smoke 기준으로 정리됐다
  - 남은 실제 blocker는 `real ORT GPU runtime asset`과 `real ONNX model asset` 확보
  - 특히 NVIDIA self-contained 크기가 다시 커질 가능성이 높아서, 다음 단계는 ONNX export 실현 가능성과 CUDA vs DirectML shipping split를 실제 자산 기준으로 판단하는 것이다
### D263 (2026-03-25)
- 실자산 blocker 중 하나였던 `real ONNX model asset` 쪽을 실제로 뚫었다. 현재 workspace에 남아 있던 local HF snapshot(`C:\Users\ikidk\Documents\Code\01 Z-Soda\.git-history\release-assets\models\distill-any-depth-base`)을 portable Python runtime으로 로드해 `DepthAnythingForDepthEstimation` 모델이 정상 생성되는 것을 먼저 확인했다.
- ONNX export를 일회성 실험이 아니라 반복 가능한 도구로 고정하기 위해 `tools/export_depth_model_onnx.py`를 추가했다.
  - 입력: local HF snapshot directory
  - 출력: `.onnx`
  - export: `torch.onnx.export(..., dynamo=False)`
  - 선택적 검증: `onnx.checker` + `onnxruntime` CPU smoke session
- portable runtime에 export용 최소 의존성도 보강했다.
  - `onnx`
  - `onnxscript`
  - `onnxruntime` (CPU smoke validation용)
- 실제 export/검증 결과
  - 출력 파일: `artifacts\ort-export-probe\distill_any_depth_base.from-script.onnx`
  - 크기: `388,981,138` bytes
  - ORT CPU smoke:
    - input: `pixel_values ['batch', 3, 'height', 'width']`
    - output: `predicted_depth [1, 378, 378] float32`
    - range: 약 `3.37 .. 7.77`
- 동시에 NVIDIA runtime budget도 실측했다.
  - official `onnxruntime-gpu==1.24.2` wheel 자체는 약 `207.1 MB`
  - unzip 기준 `onnxruntime_providers_cuda.dll`는 `312,606,240` bytes
  - import chain상 직접 필요한 DLL은 `cublasLt64_12.dll`, `cublas64_12.dll`, `cufft64_11.dll`, `cudart64_12.dll`, `cudnn64_9.dll`
  - 기존 PyTorch CUDA runtime에서 같은 이름 subset만 봐도 이미 1GB를 크게 넘기기 때문에, “CUDA self-contained sidecar도 자연스럽게 1GB 안쪽일 것”이라는 가정은 성립하지 않는다는 점을 확인했다
- 검증
  - `python -m py_compile tools\export_depth_model_onnx.py`
  - `C:\Users\ikidk\Documents\Code\01 Z-Soda\.git-history\release-assets\python-win\python.exe tools\export_depth_model_onnx.py --model-dir C:\Users\ikidk\Documents\Code\01 Z-Soda\.git-history\release-assets\models\distill-any-depth-base --output-path artifacts\ort-export-probe\distill_any_depth_base.from-script.onnx --input-size 384 --opset 17 --validate-ort --overwrite`
- 현재 상태 요약
  - `real ONNX model asset`: 확보 가능성이 확인됐고, 자동화 스크립트까지 생겼다
  - `real ORT GPU runtime asset`: 여전히 남아 있는 blocker
  - 따라서 다음 단계는 exported ONNX를 repo-sidecar contract에 맞게 정식 staging하고, Windows GPU shipping은 `CUDA 유지 vs DirectML split`를 크기/UX 기준으로 결정하는 것이다
### D264 (2026-03-25)
- Windows 최종 방향을 `native ONNX Runtime sidecar`로 고정하고, 실제 DirectML 자산으로 돌아가는 첫 패키지까지 만들었다.
- `tools/prepare_ort_sidecar_release.py`를 추가해서 실제 ONNX 모델과 실제 ORT/DirectML DLL을 슬림 sidecar 자산 디렉터리로 재구성할 수 있게 했다.
  - 입력 ONNX: `artifacts/ort-export-probe/distill_any_depth_base.from-script.onnx`
  - 입력 런타임: `artifacts/ort-wheel-probe/extracted-directml-wheel`
  - 출력 자산: `artifacts/ort-sidecar-directml-assets`
- sidecar ORT 패키징 검증도 강화했다.
  - `tools/package_layout.py`: `zsoda_ort/onnxruntime_providers_shared.dll`까지 필수로 검증
  - `tools/run_packaging_smoke.py`: Windows sidecar zip smoke가 `onnxruntime_providers_shared.dll`까지 확인
  - `tools/build_aex.ps1`: ORT 번들 DLL 수집에 `DirectML.dll`을 포함
- 공식 ORT 헤더는 `artifacts/ort-source-v1.24.2`에 shallow clone으로 가져와서, Windows AE 빌드를 `ORT API enabled + direct-link OFF`로 실제 성공시켰다.
  - 빌드 산출물: `build-origin-main-ae-ort-dml/plugin/Release/ZSoda.aex`
  - staged runtime: `build-origin-main-ae-ort-dml/plugin/Release/zsoda_ort/{onnxruntime.dll, onnxruntime_providers_shared.dll, DirectML.dll}`
- 실제 Windows DirectML sidecar 패키지를 생성했다.
  - baseline output: `artifacts/ort-sidecar-directml-package/ZSoda-windows.zip`
  - 사용자용 복사본: `C:\Users\ikidk\Documents\Code\01 Z-Soda\artifacts\12_ort-sidecar-directml-manual-test\ZSoda-windows.zip`
  - 함께 들어가는 구성:
    - `ZSoda.aex`
    - `models/distill-any-depth/distill_any_depth_base.onnx`
    - `zsoda_ort/onnxruntime.dll`
    - `zsoda_ort/onnxruntime_providers_shared.dll`
    - `zsoda_ort/DirectML.dll`
- 현재 패키지 크기
  - `ZSoda.aex`: `1,938,432` bytes
  - `ZSoda-windows.zip`: `377,689,897` bytes
  - runtime DLL 총합(DirectML sidecar): 약 `39.6 MB`
  - ONNX 모델: 약 `389.0 MB`
- 검증
  - `python -m py_compile tools\prepare_ort_sidecar_release.py tools\run_packaging_smoke.py tools\package_layout.py`
  - `python tools\run_packaging_smoke.py --windows-build-dir build-origin-main-ae --output-dir artifacts\packaging-smoke-sidecar-v2`
  - `powershell -ExecutionPolicy Bypass -File .\tools\build_aex.ps1 -AeSdkIncludeDir C:\Users\ikidk\Downloads\AfterEffectsSDK_25.6_61_win\ae25.6_61.64bit.AfterEffectsSDK\Examples\Headers -OrtIncludeDir Y:\artifacts\ort-source-v1.24.2\include -OrtLibrary Y:\artifacts\ort-wheel-probe\extracted-directml-wheel\onnxruntime.dll -OrtRuntimeDllPath Y:\artifacts\ort-wheel-probe\extracted-directml-wheel\onnxruntime.dll -EnableOrtApi -RequireOrtRuntimeDll -OrtDirectLinkMode OFF -BuildDir Y:\build-origin-main-ae-ort-dml -Config Release`
  - `powershell -ExecutionPolicy Bypass -File .\tools\package_plugin.ps1 -BuildDir Y:\build-origin-main-ae-ort-dml -Platform windows -PackageMode sidecar-ort -ModelRootDir Y:\artifacts\ort-sidecar-directml-assets\models -OrtRuntimeDllPath Y:\artifacts\ort-sidecar-directml-assets\zsoda_ort\onnxruntime.dll -OutputDir Y:\artifacts\ort-sidecar-directml-package -RequireSelfContained -RequireOrtRuntimeDll`
- 추가 메모
  - 현재 sidecar 패키지는 `distill-any-depth-base` ONNX만 포함한다. UI 상 small/large 항목은 아직 남아 있을 수 있으므로, 다음 단계에서는 ONNX export 범위를 늘리거나 manifest/모델 노출 정책을 정리해야 한다.
  - `tools/build_aex.ps1`는 direct-link OFF에서도 `OrtLibrary` 인자를 같이 요구한다. 실제 CMake는 헤더만으로도 빌드 가능하므로, 다음 정리에서는 wrapper 조건도 간결하게 줄이는 것이 맞다.
### D265 (2026-03-26)
- Windows ORT sidecar 패키징 계약을 `MediaCore\\Z-Soda\\...` 폴더형으로 고정했다.
  - `tools/package_layout.py`에 Windows sidecar 전용 `package_root_dir_name = "Z-Soda"`를 추가했다.
  - `tools/package_plugin.ps1`, `tools/package_plugin.sh`가 이제 `ZSoda.aex`, `models/`, `zsoda_ort/`를 zip 루트가 아니라 `Z-Soda/` 아래에 배치한다.
  - `tests/test_runtime_path_resolver.cpp`에 `MediaCore\\Z-Soda` 인접 자산이 `MediaCore\\models`보다 우선되는 회귀 테스트를 추가했다.
- sidecar packaging smoke를 강화했다.
  - `tools/run_packaging_smoke.py`가 `package_root_dir_name == "Z-Soda"` 계약을 직접 확인한다.
  - zip 안에 `Z-Soda/...`가 존재해야 하고, 예전 flat root(`ZSoda.aex`, `models/...`, `zsoda_ort/...`)가 남아 있으면 실패하도록 바꿨다.
  - optional runtime 파일 `DirectML.dll`도 smoke에서 같이 검증한다.
- `distill-any-depth-base` ONNX export의 본질 원인을 잡고 export 경로를 수정했다.
  - 원인: legacy ONNX export 중 `DepthAnythingDepthEstimationHead.forward()`의 `int(patch_height * patch_size)` / `int(patch_width * patch_size)`가 상수로 굳어져 출력 shape가 고정됐다.
  - 대응: `tools/export_depth_model_onnx.py`에서 export 시점에만 head를 monkeypatch해서 `size=(patch_height * patch_size, patch_width * patch_size)`를 그대로 trace하게 만들었다.
  - export 검증을 다중 shape(정사각/비정사각) ORT smoke로 강화해서, 출력 shape가 계속 같으면 export 단계에서 바로 실패하도록 했다.
- `plugin/inference/OnnxRuntimeBackend.cpp`도 `distill-any-depth*`를 518 기반 dynamic profile로 다루도록 수정했다.
  - `use_upper_bound_dynamic_aspect = true`
  - `patch_multiple = 14`
  - ImageNet mean/std 사용
  - quality 값이 실제 `process_res`로 연결되도록 `ResolveDynamicProcessResolution()` 경로로 일반화했다.
- 새 dynamic ONNX와 새 폴더형 패키지를 다시 생성했다.
  - ONNX: `artifacts/ort-export-probe/distill_any_depth_base.from-script.onnx`
  - baseline package: `artifacts/ort-sidecar-directml-folder-package-dynamic/ZSoda-windows.zip`
  - user-facing copy: `C:\\Users\\ikidk\\Documents\\Code\\01 Z-Soda\\artifacts\\14_ort-sidecar-dynamic-quality-manual-test\\ZSoda-windows.zip`
  - zip SHA256: `0e96bf8baf29be54f294bea68accceae34faaf5d3195bbb4cc1281af8469f694`
- 검증
  - `python -m py_compile tools\\package_layout.py tools\\prepare_package_stage.py tools\\run_packaging_smoke.py tools\\export_depth_model_onnx.py`
  - `cmake --build build-origin-main-tests --config Release --target zsoda_core_tests zsoda_inference_tests`
  - `ctest -C Release -R "zsoda_core_tests|zsoda_inference_tests" --output-on-failure`
  - `python tools\\run_packaging_smoke.py --windows-build-dir build-origin-main-ae --output-dir artifacts\\packaging-smoke-sidecar-folder`
  - `C:\\Users\\ikidk\\Documents\\Code\\01 Z-Soda\\.git-history\\release-assets\\python-win\\python.exe tools\\export_depth_model_onnx.py --model-dir C:\\Users\\ikidk\\Documents\\Code\\01 Z-Soda\\.git-history\\release-assets\\models\\distill-any-depth-base --output-path artifacts\\ort-export-probe\\distill_any_depth_base.from-script.onnx --input-size 518 --opset 17 --validate-ort --overwrite`
  - `cmake --build build-origin-main-ae-ort-dml --config Release --target zsoda_aex`
  - `python tools\\prepare_ort_sidecar_release.py --onnx-model-path artifacts\\ort-export-probe\\distill_any_depth_base.from-script.onnx --ort-runtime-dir Y:\\artifacts\\ort-sidecar-directml-assets\\zsoda_ort --output-dir artifacts\\ort-sidecar-directml-assets-dynamic --overwrite`
  - `powershell -ExecutionPolicy Bypass -File .\\tools\\package_plugin.ps1 -BuildDir Y:\\build-origin-main-ae-ort-dml -Platform windows -PackageMode sidecar-ort -ModelRootDir artifacts\\ort-sidecar-directml-assets-dynamic\\models -OrtRuntimeDllPath artifacts\\ort-sidecar-directml-assets-dynamic\\zsoda_ort\\onnxruntime.dll -OutputDir artifacts\\ort-sidecar-directml-folder-package-dynamic -RequireSelfContained -RequireOrtRuntimeDll`

### D266 (2026-03-26)
- 커밋 준비용 hygiene 정리를 진행했다.
  - `.gitignore`에 `artifacts/*` 기본 무시 규칙을 추가해서 생성 산출물이 작업트리를 계속 더럽히지 않게 했다.
  - `artifacts/windows/README.md`만 예외로 유지해 기존 추적 파일은 계속 살아 있게 했다.
- 현재 방향과 맞지 않던 문서를 ORT sidecar 기준으로 정리했다.
  - `docs/build/LOCAL_AGENT_HANDOFF.md`를 Windows `Z-Soda/` 폴더형 배포 기준으로 전면 갱신했다.
  - `docs/build/AE_SMOKE_TEST.md`의 설치 전제 조건을 `Z-Soda/ZSoda.aex + models + zsoda_ort` 기준으로 수정했다.
  - `tools/collect_ae_loader_diagnostics.ps1`의 기본 검사 경로도 `MediaCore\\Z-Soda\\ZSoda.aex`를 우선 보도록 바꿨다.
- 결과적으로 현재 `git status`에는 코드/테스트/문서/도구 변경만 남고, 생성된 `artifacts/` 디렉터리들은 ignored 상태로 빠진다.

### D267 (2026-03-26)
- macOS 쪽 작업 이관을 위해 [docs/build/MAC_AGENT_HANDOFF.md](docs/build/MAC_AGENT_HANDOFF.md)를 추가했다.
  - 공용 코어는 그대로 유지하고
  - macOS에서는 `.plugin` bundle + `Contents/Resources/models` + `Contents/Resources/zsoda_ort` 기준으로 이어받도록 정리했다.
  - 우선순위를 `ORT CPU bring-up -> CoreML provider 검증 -> AE smoke` 순서로 고정했다.
- [docs/build/README.md](docs/build/README.md)에 mac handoff 링크를 추가해서 build docs에서 바로 찾을 수 있게 했다.
- 현재 브랜치는 커밋/푸시 준비를 마친 상태이며, 다음 단계는 staged 변경을 하나의 브랜치 업데이트로 커밋하고 origin에 푸시하는 것이다.

### D268 (2026-03-26)
- 최종 shipping 기준에 맞춘 cleanup/refactor 1차를 진행했다.
  - `DummyInferenceEngine`와 `ZSODA_ALLOW_DUMMY_FALLBACK` 경로를 제거하고, hard failure는 `RenderPipeline`의 safe output 계약으로만 처리하도록 정리했다.
  - `RuntimeBackend::kMetal`과 미사용 `plugin/backends/BackendConfig.h`를 제거해 런타임 분기 노이즈를 줄였다.
  - `EngineFactory.cpp`는 이제 plugin-adjacent ORT sidecar 자산을 먼저 해석하고, embedded payload 추출은 native sidecar가 없을 때만 legacy fallback으로 시도한다.
- 문서 계약도 현재 기준으로 다시 맞췄다.
  - 루트 `README.md`, `models/README.md`, `docs/build/README.md`를 ORT sidecar shipping 기준으로 다시 작성했다.
  - `docs/build/RELEASE_ASSETS.md`, `docs/build/THIN_SETUP_DESIGN.md`, `docs/build/MAC_SILICON_HANDOFF.md`에는 legacy/superseded note를 추가했다.
  - `docs/build/LOCAL_AGENT_HANDOFF.md`는 Python remote service를 명시적 debug/fallback 경로로만 설명하도록 정리했다.
- 검증:
  - `cmake --build build-origin-main-tests --config Release --target zsoda_core_tests zsoda_inference_tests zsoda_render_tests`
  - `ctest -C Release -R "zsoda_core_tests|zsoda_inference_tests|zsoda_render_tests" --output-on-failure`
  - `python tools\\run_packaging_smoke.py --windows-build-dir build-origin-main-ae --output-dir artifacts\\packaging-smoke-refactor`

### D269 (2026-03-26)
- Windows ORT sidecar release candidate를 다시 빌드하고 handoff 문서를 최신화했다.
  - 최신 패키지: `artifacts/15_ort-sidecar-release-candidate/ZSoda-windows.zip`
  - 설치 폴더: `Z-Soda/ZSoda.aex + models + zsoda_ort`
  - MediaCore 설치 경로도 최신 RC 패키지 기준으로 교체했다.
- 사용자 최종 smoke 결과:
  - AE 로드 정상
  - `Quality` 반영 정상
  - depth/slice 출력 정상
  - Windows 쪽은 제품 기준 release-candidate 상태로 판단
- macOS handoff 문서도 이 상태를 반영했다.
  - `docs/build/LOCAL_AGENT_HANDOFF.md`: 최신 Windows RC 패키지와 smoke pass 상태 반영
  - `docs/build/MAC_AGENT_HANDOFF.md`: Windows를 검증된 reference lane으로 명시하고, mac agent 시작 지시문 추가
