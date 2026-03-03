# Local Windows Agent Handoff Guide

이 문서는 로컬(Windows) 에이전트가 `.aex` 빌드와 AE 스모크 테스트를 이어서 수행하기 위한 실행 지침이다.

## 1) 작업 루트

- 저장소 루트(예시): `C:\Users\Yongkyu\code\Z-Soda`
- 필수 확인:
  - `tools\build_aex.ps1`
  - `tools\package_plugin.ps1`
  - `docs\build\AE_SMOKE_TEST.md`
  - `docs\build\ORT_RUNTIME_ISOLATION_PLAN.md`

## 2) 시작 전 동기화

CMD:
```cmd
cd /d C:\Users\Yongkyu\code\Z-Soda
git fetch origin
git checkout main
git pull --ff-only origin main
git rev-parse --short HEAD
```

## 3) 환경 변수 설정 (CMD)

```cmd
set "AE_SDK_ROOT=C:\SDKs\AdobeAfterEffectsSDK\AfterEffectsSDK_25.6_61_win\ae25.6_61.64bit.AfterEffectsSDK"
set "AE_HEADERS=%AE_SDK_ROOT%\Examples\Headers"
set "ORT_ROOT=C:\onnxruntime-win-x64-1.24.2"
set "ORT_INCLUDE=%ORT_ROOT%\include"
set "ORT_LIB=%ORT_ROOT%\lib\onnxruntime.lib"
set "ORT_DLL_DIR=%ORT_ROOT%\lib"
```

검증:
```cmd
if exist "%AE_HEADERS%\AE_Effect.h" (echo AE_HEADER: OK) else (echo AE_HEADER: MISSING)
if exist "%ORT_INCLUDE%\onnxruntime_cxx_api.h" (echo ORT_HEADER: OK) else (echo ORT_HEADER: MISSING)
if exist "%ORT_LIB%" (echo ORT_LIB: OK) else (echo ORT_LIB: MISSING)
if exist "%ORT_DLL_DIR%\onnxruntime.dll" (echo ORT_DLL: OK) else (echo ORT_DLL: MISSING)
```

## 4) 빌드 및 배치

CMD:
```cmd
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_aex.ps1 ^
  -AeSdkIncludeDir "%AE_HEADERS%" ^
  -OrtIncludeDir "%ORT_INCLUDE%" ^
  -OrtLibrary "%ORT_LIB%" ^
  -MsvcRuntime "MultiThreaded$<$<CONFIG:Debug>:Debug>" ^
  -BuildDir "build-win" ^
  -Config Release ^
  -CopyToMediaCore
```

로더 분리 진단용(최소 probe 플러그인 동시 빌드):
```cmd
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_aex.ps1 ^
  -AeSdkIncludeDir "%AE_HEADERS%" ^
  -OrtIncludeDir "%ORT_INCLUDE%" ^
  -OrtLibrary "%ORT_LIB%" ^
  -MsvcRuntime "MultiThreaded$<$<CONFIG:Debug>:Debug>" ^
  -BuildDir "build-win" ^
  -Config Release ^
  -BuildLoaderProbe ^
  -CopyToMediaCore
```

ORT DLL 복사:
```cmd
copy /Y "%ORT_DLL_DIR%\onnxruntime.dll" "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\onnxruntime.dll"
```

주의:
- `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore` 쓰기 시 `Access is denied`가 나오면 **관리자 권한 PowerShell/CMD**로 재실행해야 한다.
- `tools\build_aex.ps1`는 기본값으로 ORT API를 활성화한다. 구조 안전 모드가 필요할 때만 `-DisableOrtApi`를 사용한다.
- 테스트/실행 중 `The requested API version [24] is not available ... [1, 17]`가 나오면 Adobe ORT 1.17.x와 plugin ORT 1.24.x 충돌 가능성이 높다. 임시 복사로 우회하지 말고 아래 `명시적 ORT 로딩 전략` 기준으로 점검한다.
- CRT 버전 불일치 이슈를 줄이기 위해 기본 런타임은 정적 CRT(`/MT`)다. 강제로 `/MD`가 필요하지 않다면 `-MsvcRuntime` 기본값을 유지한다.

결과 확인:
```cmd
if exist "build-win\plugin\Release\ZSoda.aex" (echo BUILD_AEX: OK) else (echo BUILD_AEX: MISSING)
if exist "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\ZSoda.aex" (echo MEDIA_AEX: OK) else (echo MEDIA_AEX: MISSING)
if exist "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\onnxruntime.dll" (echo MEDIA_ORT_DLL: OK) else (echo MEDIA_ORT_DLL: MISSING)
if exist "build-win\plugin\Release\ZSoda.pdb" (echo PDB: OK) else (echo PDB: MISSING)
if exist "build-win\plugin\Release\ZSoda.map" (echo MAP: OK) else (echo MAP: MISSING)
if exist "build-win\plugin\pipl\ZSodaPiPL.rr" (echo PIPL_RR: OK) else (echo PIPL_RR: MISSING)
if exist "build-win\plugin\Release\ZSoda.loader_check.txt" (echo LOADER_CHECK: OK) else (echo LOADER_CHECK: MISSING)
if exist "build-win\plugin\Release\ZSodaLoaderProbe.aex" (echo PROBE_AEX: OK) else (echo PROBE_AEX: MISSING)
if exist "build-win\plugin\Release\ZSodaLoaderProbe.loader_check.txt" (echo PROBE_LOADER_CHECK: OK) else (echo PROBE_LOADER_CHECK: MISSING)
```

`No loaders recognized this plugin`가 나오면 우선 `build-win\plugin\pipl\ZSodaPiPL.rr`에 아래 토큰이 있는지 확인:
- `CodeWin64X86`
- `EffectMain`
- `AE_Effect_Global_OutFlags`
- `0x04008120`

그리고 `build-win\plugin\Release\ZSoda.loader_check.txt`가 생성되었는지 확인한다.
- 이 파일은 빌드 스크립트가 수행한 최종 로더 게이트 결과(필수 export/section 점검 대상)를 요약한다.
- 추가 증적은 빌드 콘솔의 `loader_export:` / `loader_header:` 줄(`dumpbin` 기반)을 사용한다.
- `-BuildLoaderProbe`를 사용했다면 `ZSodaLoaderProbe.aex`도 동일 방식으로 검사된다.
  - `ZSodaLoaderProbe.aex`만 AE에서 로드되면: 현재 본 플러그인(`ZSoda.aex`) 내부 구현/의존성 이슈 가능성 우세
  - 둘 다 `No loaders recognized`면: AE 로더 정책/캐시/설치 경로/호스트 환경 축 우선 점검

덤프 역추적 필수 산출물:
- `build-win\plugin\Release\ZSoda.pdb`
- `build-win\plugin\Release\ZSoda.map`
- 둘 중 하나라도 누락이면 WinDbg/VS에서 `ZSoda.aex+RVA`를 함수 단위로 확정하기 어려워진다.

## 4-1) 명시적 ORT 로딩 전략 (필수)

목표:
- Adobe 번들 ORT(1.17.x)와 플러그인 ORT(1.24.x) 충돌을 구조적으로 차단한다.

핵심 원칙:
- `onnxruntime.dll` implicit link(import table 의존) 제거
- 플러그인에서 ORT DLL 절대경로를 계산해 `LoadLibraryExW`로 명시적 로드
- `OrtGetApiBase()->GetApi(ORT_API_VERSION)` 협상 실패 시 추론 비활성화 + 안전 fallback

운영 체크:
- 로그에 아래 4개가 반드시 남아야 함
  - 로드된 ORT 모듈 절대경로
  - ORT 파일 버전(예: 1.24.x)
  - 요청/협상 API 버전(예: request 24)
  - fallback 사유(로드/심볼/협상 실패)

PowerShell 확인 예시(관리자 권한 권장):
```powershell
$p = Get-Process AfterFX -ErrorAction SilentlyContinue
if ($p) {
  $mods = Get-Process -Id $p.Id -Module | Where-Object { $_.ModuleName -ieq "onnxruntime.dll" }
  $mods | Select-Object ModuleName, FileName,
    @{N="FileVersion";E={$_.FileVersionInfo.FileVersion}}
}
```

판정 기준:
- 정상: ORT 모듈 경로가 플러그인 배포 경로(예: `...\ZSoda\...\runtime\onnxruntime.dll`)이고 API 협상 성공
- 비정상: `System32`/Adobe 경로 ORT가 잡히거나 API 협상 실패. 이 경우 crash 대신 fallback 출력이어야 함

추가(2026-03-03):
- 최신 코드에서는 ORT DLL 경로가 비어 있으면 bare-name 로딩(`onnxruntime.dll`)을 허용하지 않고, **플러그인 모듈 인접(side-by-side) DLL**을 우선 찾는다.
- 찾지 못하면 ORT 초기화는 실패하고 fallback 경로로 전환된다(하드 크래시 대신 안전 출력).
- 적용 중 오류 분석은 `%TEMP%\ZSoda_AE_Runtime.log`를 먼저 확인한다(SEH/C++ 예외 로그).

## 5) AE 스모크 테스트

- AE 재시작 후 `ZSoda` 이펙트 로드 확인
- `docs\build\AE_SMOKE_TEST.md`의 ST-01 ~ ST-07 순서대로 실행
- 최소 필수 보고:
  - ST-01 (로드)
  - ST-03 (DepthMap)
  - ST-04 (Slicing)
  - ST-06 (Render Queue)

## 6) 실패 시 보고 템플릿

- 커밋: `git rev-parse --short HEAD`
- 실패 단계: (예: build/configure/AE load/ST-04)
- 명령/절차:
- 에러 원문 (전체):
- 재현 여부: 항상/간헐
- 스크린샷/로그 경로:

## 7) 운영 규칙

- 모델 가중치/대용량 바이너리는 git에 커밋하지 않는다.
- 스크립트 수정 시 `module: summary` 커밋 규칙을 따른다.
- 작업 단위 완료 후 `PROGRESS.md`를 갱신한다.

## 8) 원클릭 진단 수집 (Windows)

로더 단계 이슈(`No loaders recognized`, `Ignore=1`) 재현 시 아래 스크립트로 증거를 한 번에 수집한다.

CMD:
```cmd
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\collect_ae_loader_diagnostics.ps1 ^
  -AfterEffectsVersion "25.0" ^
  -OutputRoot ".\artifacts\diagnostics" ^
  -ContextLines 8
```

주요 수집 항목:
- PluginCache: `HKCU\Software\Adobe\After Effects\25.0\PluginCache\en_US\ZSoda.aex_*` 키/값 덤프(JSON + TXT)
- Plugin Loading.log: `ZSoda`/`No loaders recognized` 매치 라인 ±N 컨텍스트
- `.aex` 증거: `dumpbin /exports /headers /dependents /rawdata:.rsrc`, SHA256, `LoadLibraryW` probe 결과

출력 구조:
```text
artifacts/diagnostics/ae_loader_diag_YYYYMMDD_HHMMSS/
  summary.txt
  plugin_cache/
    zsoda_plugin_cache.json
    zsoda_plugin_cache.txt
  logs/
    Plugin Loading.log
    plugin_loading_zsoda_context.txt
  aex/
    C_Program_Files_Adobe_Common_Plug-ins_7.0_MediaCore_ZSoda.aex.meta.txt
    *.exports.txt
    *.headers.txt
    *.dependents.txt
    *.rsrc.txt
```

## Session Handoff (2026-03-03, for next WSL agent)

### Branch / Remote
- Branch: `main`
- Remote: `origin` (`https://github.com/damagethundercat/Z-Soda.git`)
- Latest pushed commits:
  - `6ce6329` - add windows aex artifact
  - `320e0c7` - AE PiPL pipeline + outflags sync fixes

### What is pushed and ready
- Source changes for AE entrypoint/PiPL/outflags are pushed.
- Windows artifact is pushed at:
  - `artifacts/windows/ZSoda.aex`
  - `artifacts/windows/ZSoda.aex.sha256`
- Expected SHA256 for current artifact:
  - `fcb5f1538885131cc7e4e84054ba3140e9b98be07f8f55da77047991f572d446`

### Latest crash evidence
- Latest reported crash dump in this session:
  - `471fec5f-fa20-4dc7-a552-f44ef1074861.dmp`
- Sentry breadcrumb still showed:
  - `global outflags mismatch. Code flags are 4008120 and PiPL flags are 4008020`
- Plugin loading log repeatedly showed `plugin is marked as Ignore` for `ZSoda.aex`.

### High-probability root cause to continue
- ORT runtime DLL conflict is highly likely:
  - AE process loaded Adobe-bundled `onnxruntime.dll` (v1.17.x)
  - Intended local runtime (`onnxruntime-win-x64-1.24.2`) did not appear loaded
- This can trigger ABI/API mismatch and access violations (`C0000005`).

### Immediate next actions for next agent
1. In WSL/Windows test flow, enforce deterministic ORT DLL resolution (plugin-local/runtime-local), and avoid accidental reuse of Adobe/other ORT modules already loaded by process.
2. Re-check AE PluginCache key for ZSoda `Ignore` state after each crash and ensure fresh load path when validating fixes.
3. Reproduce with a clean startup and collect:
   - `Plugin Loading.log`
   - Sentry `__sentry-breadcrumb1`
   - loaded module list (`AfterFX.exe` + exact `onnxruntime.dll` path/version)
4. Follow phased rollout in `docs/build/ORT_RUNTIME_ISOLATION_PLAN.md` (Phase 0 -> 3) and keep fallback behavior enabled until Phase 2 validation completes.

### Notes
- Local build/output folders (`build-win*`, `dist/`, `Microsoft/`) are intentionally not committed.
- Artifact commit is for handoff convenience only; production packaging strategy can be revised later.

### Session update (2026-03-03 16:16, local Windows retest)
- Synced latest `main` to `73464a4` and rebuilt with `tools/build_aex.ps1`.
- Build reached link stage and produced `build-win\plugin\Release\ZSoda.aex`, but script exited fail in `Assert-PiPLSignature`:
  - `Missing token 'CodeWin64X86' in build-win\plugin\pipl\ZSodaPiPL.rc`
- Verified PiPL artifacts:
  - `build-win\plugin\pipl\ZSodaPiPL.rc` exists and contains encoded Win64 code token `"4668"` + `"EffectMain"` + outflags numeric value.
  - `build-win\plugin\pipl\ZSodaPiPL.rr` contains literal tokens `CodeWin64X86`, `EffectMain`, `AE_Effect_Global_OutFlags`, `0x04008120`.
- Because script aborted before `-CopyToMediaCore` final step, local run manually copied latest `ZSoda.aex` to `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\ZSoda.aex`.

#### Repro result after rebuild
- User retest still shows the same AE init error (25::3).
- `Plugin Loading.log` (`2026-03-03 16:16:05`) still shows:
  - `Loading ...\MediaCore\ZSoda.aex`
  - `Loading from disk...`
  - `No loaders recognized this plugin, so the plugin is set to Ignore.`
- `%TEMP%\ZSoda_AE_Runtime.log` is still not created (likely `EffectMain` not reached).
- `HKCU\Software\Adobe\After Effects\25.0\PluginCache\en_US\ZSoda.aex_*` key is recreated with `Ignore=1` after AE launch even after manual deletion.

#### Ask for next WSL pass
1. Keep investigating why AE loader still rejects this binary despite `EffectMain` export + PiPL resource presence.
2. Include exact evidence in next report:
   - `Plugin Loading.log` snippet around ZSoda load lines
   - current `PluginCache\...\ZSoda.aex_*` values (`Ignore`, `DateLow/DateHigh`)
   - PiPL dump evidence from built `.aex` (`ZSodaPiPL.rr` and PE resource inspection).
   - `build-win\plugin\Release\ZSoda.loader_check.txt` 내용 전문.

### Session update (2026-03-03 16:34, WSL script hardening)
- `tools/build_aex.ps1`의 로더 게이트를 `.rc` 텍스트 기반 검사에서 `.rr` + 최종 `.aex` 바이너리 검사로 재설계:
  - `Assert-RrSignature`: `ZSodaPiPL.rr`의 literal 토큰(`CodeWin64X86`, `EffectMain`, `AE_Effect_Global_OutFlags`, `0x04008120`) 검증
  - `Assert-AexLoaderSignature`: 최종 `ZSoda.aex`의 `EffectMain export`, `.rsrc` 섹션, 플랫폼 machine 타입 검증
  - 빌드 산출물 옆에 `ZSoda.loader_check.txt`를 생성해 게이트 요약 기록
- 목적: `.rc` 인코딩/토큰 표현 차이로 인한 false fail을 제거하고, 실제 AE 로더 관점(최종 바이너리 기준)으로 실패를 조기에 차단.

### Session update (2026-03-03 16:45, dual-path loader repro)
- User retest still reproduces same init failure after loader-gate-enabled build.
- `Plugin Loading.log` now confirms **both install paths** fail loader recognition:
  - `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\ZSoda.aex` -> `No loaders recognized this plugin, so the plugin is set to Ignore.`
  - `C:\Program Files\Adobe\Adobe After Effects 2025\Support Files\Plug-ins\Effects\ZSoda.aex` -> `No loaders recognized this plugin, so the plugin is set to Ignore.`
- `PluginCache\en_US` contains two ZSoda keys (path-hash variants), both `Ignore=1`:
  - `ZSoda.aex_74697f7d-6eaa-731e-5ba9-290933586ec3`
  - `ZSoda.aex_00f48907-5c13-bfaa-3f5b-d4f4b7658605`
- `%TEMP%\ZSoda_AE_Runtime.log` remains absent, supporting that failure occurs before `EffectMain` dispatch.

#### Extra evidence added in this session
- `dumpbin /dependents` for `ZSoda.aex`: only `KERNEL32.dll` (static CRT build), no missing import signal.
- Manual `LoadLibraryW()` probe on `ZSoda.aex` succeeds outside AE.
- PiPL binary extraction from `ZSoda.aex` shows expected property keys including Win64 code token (`8664`/`EffectMain`) and effect metadata.
- AE SDK sample PiPL transform (`CheckoutPiPL.r -> PiPLtool -> .rc`) shows the same Windows encoded pattern (`"4668"` in `.rc`), so this encoding itself is not abnormal.

#### Next WSL focus
1. Investigate why AE’s internal loader rejects this specific AEX despite valid export/resource signatures and successful raw DLL load.
2. Correlate `PluginCache` hash entries to physical paths and verify whether one failing path can poison the other via shared cache logic.
3. Attempt a minimal AE SDK sample effect build/load on the same machine to isolate whether issue is plugin-specific vs host policy/environment.

### Session update (2026-03-03 17:34, native diagnostics run with new script)
- Ran:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\collect_ae_loader_diagnostics.ps1 -AfterEffectsVersion "25.0" -OutputRoot ".\artifacts\diagnostics" -ContextLines 8`
- Full diagnostics output:
  - `artifacts\diagnostics\ae_loader_diag_20260303_173409`
  - `summary.txt` reports `plugin_cache_keys=2` and valid `dumpbin` path.
- Key observations from collected artifacts:
  - `logs\plugin_loading_zsoda_context.txt` contains repeated rejects on both paths:
    - `...\MediaCore\ZSoda.aex` -> `No loaders recognized this plugin, so the plugin is set to Ignore.`
    - `...\Effects\ZSoda.aex` -> `No loaders recognized this plugin, so the plugin is set to Ignore.`
  - `plugin_cache\zsoda_plugin_cache.json` shows two keys with `Ignore=1`:
    - `ZSoda.aex_00f48907-5c13-bfaa-3f5b-d4f4b7658605`
    - `ZSoda.aex_74697f7d-6eaa-731e-5ba9-290933586ec3`
  - `aex\...MediaCore_ZSoda.aex.meta.txt` confirms:
    - `sha256=92e75bbaa480f8cba677b627d0d4a0415f5ff01eb87c02610bcfabbef03d97c2`
    - `loadlibrary_success=True`
  - `aex\...MediaCore_ZSoda.aex.exports.txt` confirms `EffectMain` export is present.

#### Hand-off hint
- New script is now the preferred way to hand over reproducible loader evidence.
- If rerunning, share at minimum:
  1. `summary.txt`
  2. `logs\plugin_loading_zsoda_context.txt`
  3. `plugin_cache\zsoda_plugin_cache.json`

### Session update (2026-03-03 17:50, local rebuild after latest pull)
- Pulled latest `main` to `21678b1` and rebuilt from local Windows environment.
- Normal build path (without probe) succeeds:
  - `tools\build_aex.ps1 ... -Config Release -CopyToMediaCore`
  - `build-win\plugin\Release\ZSoda.aex` generated and copied to `MediaCore`.
- User retest still reproduces the same AE error.

#### Current blocker 1: AE loader reject persists
- `Plugin Loading.log` still reports:
  - `Loading ...\ZSoda.aex`
  - `No loaders recognized this plugin, so the plugin is set to Ignore.`
- `PluginCache\en_US\ZSoda.aex_*` keys keep returning with `Ignore=1`.
- `%TEMP%\ZSoda_AE_Runtime.log` is still not generated, indicating `EffectMain` is likely never reached.

#### Current blocker 2: Loader probe build is broken
- Running handoff-recommended probe build:
  - `tools\build_aex.ps1 ... -BuildLoaderProbe -CopyToMediaCore`
- Fails at `plugin/ae/LoaderProbeEntry.cpp` compile step:
  - `C2065`: `in_data` undeclared in `DoAbout` (line ~27, `PF_SPRINTF` usage context)
  - `C2440`: invalid cast from `PF_PixelPtr` to `uint8_t*` / `const uint8_t*` (lines ~70-71)
  - `C3536`: `dst`/`src` used before initialization (derived errors, line ~73)
- Result: `zsoda_loader_probe_aex` target currently cannot be used for isolation test.

#### Immediate request for next WSL pass
1. Fix `plugin/ae/LoaderProbeEntry.cpp` so `-BuildLoaderProbe` completes successfully.
2. Rebuild with probe enabled and verify both:
   - `build-win\plugin\Release\ZSodaLoaderProbe.aex`
   - `build-win\plugin\Release\ZSodaLoaderProbe.loader_check.txt`
3. Run AE load test with probe and compare outcomes:
   - if probe loads but ZSoda fails: issue likely inside ZSoda implementation/deps.
   - if probe also fails with same loader message: issue likely PiPL/loader contract or host-side policy.

### Session update (2026-03-03 18:20, WSL follow-up)
- `LoaderProbeEntry.cpp` compile blockers fixed:
  - `PF_SPRINTF` 제거 후 `std::snprintf` 사용(`in_data` undeclared 오류 제거)
  - `PF_PixelPtr` -> byte pointer 변환을 `reinterpret_cast`로 변경(`C2440`/파생 오류 제거)
- 다음 네이티브 재검증은 아래 명령으로 진행:
  - `tools\build_aex.ps1 ... -BuildLoaderProbe -CopyToMediaCore`
- 외부 리서치(Adobe community) 기반 체크포인트 반영:
  1. PiPL/entrypoint 불일치나 stale resource가 있으면 `No loaders recognized`가 발생할 수 있으므로, probe/본 플러그인 모두 `-Clean`으로 완전 재생성 후 비교 권장.
  2. 리소스 트리 구조(PiPL -> 16000 -> locale 1033)와 export `EffectMain` 정합성 재확인 필요.
  3. probe까지 동일 오류면 구현 문제가 아니라 AE 로더 정책/호스트 환경 축 우선으로 판단.

### Session update (2026-03-03 18:35, version mismatch fix)
- 네이티브 보고된 Probe 경고 `25::16 version mismatch` 대응:
  - 코드(`my_version`)와 PiPL(`AE_Effect_Version`)을 공용 상수 `ZSODA_EFFECT_VERSION_HEX`로 통일.
  - 적용 파일:
    - `plugin/ae/ZSodaVersion.h`
    - `plugin/ae/AeHostAdapter.cpp`
    - `plugin/ae/LoaderProbeEntry.cpp`
    - `plugin/ae/ZSodaPiPL.r`
    - `plugin/ae/ZSodaLoaderProbePiPL.r`
- Probe의 이펙트 컨트롤에서 펼칠 파라미터가 없는 것은 정상:
  - 현재 Probe는 로더 인식 분리 진단용 최소 엔트리이며 사용자 파라미터를 등록하지 않는다.
  - 목적은 UI 효과가 아니라 `로더 인식 여부`를 확인하는 것이다.

### Session update (2026-03-03 19:00, functional runtime defaults)
- `tools\build_aex.ps1` 기본 동작을 ORT API 활성화로 전환:
  - 별도 플래그 없이도 `ZSODA_WITH_ONNX_RUNTIME_API=ON`으로 구성됨.
  - 구조 안전 모드가 필요할 때만 `-DisableOrtApi` 사용.
- MFR 경고 최소화를 위한 outflags 동기화:
  - 코드(`AeHostAdapter`/`LoaderProbeEntry`)와 PiPL(`ZSodaPiPL.r`/`ZSodaLoaderProbePiPL.r`)에서
    `ZSODA_AE_GLOBAL_OUTFLAGS`, `ZSODA_AE_GLOBAL_OUTFLAGS2` 공용 매크로 사용.
  - `PF_OutFlag2_SUPPORTS_THREADED_RENDERING`가 SDK에서 정의되면 자동 반영.
- 런타임 엔진 상태 로깅 추가:
  - `EffectMain` 최초 진입 시 `%TEMP%\ZSoda_AE_Runtime.log`에 `EngineStatus` 기록.
  - 예: `requested=..., active=..., engine=OnnxRuntimeBackend[...]` 또는 fallback reason.

### Session update (2026-03-03 19:15, init-failure hardening)
- `ZSoda`에서 `25::3 (cannot be initialized)`가 반복되는 경로를 완화하기 위해
  `EffectMain` 예외 정책을 명령별로 분기:
  - `PF_Cmd_RENDER`: 기존과 동일하게 치명 오류 반환(`PF_Err_INTERNAL_STRUCT_DAMAGED`)
  - 그 외 초기화/설정 명령(`GLOBAL_SETUP`, `PARAMS_SETUP` 등): 예외 발생 시 `PF_Err_NONE`로 비치명 처리
- 목적:
  - 로더 통과 후 초기화 예외 때문에 전체 이펙트 적용이 막히는 현상을 방지
  - 적용은 유지하고, 실제 원인은 `%TEMP%\ZSoda_AE_Runtime.log`의 `EffectMain`/`EngineStatus` 로그로 추적

### Session update (2026-03-03 19:25, full non-fatal return policy)
- 네이티브 보고 기준으로 `ZSoda`는 여전히 `25::3`가 발생하므로, `EffectMain`에서의 실패 반환 정책을 추가 완화:
  - `BuildSdkDispatch` 실패 시 `PF_Err_NONE`
  - `Dispatch` 실패 시 `PF_Err_NONE`
  - C++ 예외/SEH 예외 catch 시 `PF_Err_NONE`
- 의도:
  - AE가 이펙트 적용 자체를 거부하지 않도록 하여 본체(`ZSoda`)를 Probe와 동일하게 “적용 가능한 상태”로 맞춤
  - 실패 원인 분석은 로그(`%TEMP%\ZSoda_AE_Runtime.log`) 기반으로 지속

### Session update (2026-03-03 19:23, runtime log triage after same error)
- User retest still reports the same AE apply error.
- Checked `%TEMP%\ZSoda_AE_Runtime.log`:
  - `LastWrite`: `2026-03-03 19:23:34.260`
  - `Size`: `1518` bytes
- Latest `EngineStatus` lines (`18:57:32.794`, `19:11:28.601`, `19:23:34.260`) all show identical fallback:
  - `requested=auto, active=cpu, engine=DummyDepthEngine`
  - `configured_fallback=true, last_run_fallback=true`
  - `fallback_reason=onnx runtime backend initialization failed`
  - `requested_path=attempted_load_path=C:\onnxruntime-win-x64-1.24.2\lib\onnxruntime.dll`
  - `loaded_path=<none>, negotiated_api_version=0, runtime_version=<unknown>`
  - `error=LoadLibraryW failed: DLL 초기화 루틴을 실행할 수 없습니다.`
- `EffectMain` keyword search in the same runtime log returns `NONE` (no matching line).

#### Implication for next WSL pass
1. Failure is repeating at ORT DLL initialization with the same path/signature, so prioritize dependency/runtime initialization diagnostics over random crash triage.
2. Add immediate logging of Win32 error code (`GetLastError`) right after failed `LoadLibraryW` and include decoded message text.
3. Verify transitive dependency loadability of `C:\onnxruntime-win-x64-1.24.2\lib\onnxruntime.dll` under the same user/session context (AE-launched context preferred).
