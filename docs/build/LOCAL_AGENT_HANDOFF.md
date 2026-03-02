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
  -BuildDir "build-win" ^
  -Config Release ^
  -CopyToMediaCore
```

ORT DLL 복사:
```cmd
copy /Y "%ORT_DLL_DIR%\onnxruntime.dll" "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\onnxruntime.dll"
```

주의:
- `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore` 쓰기 시 `Access is denied`가 나오면 **관리자 권한 PowerShell/CMD**로 재실행해야 한다.
- 테스트/실행 중 `The requested API version [24] is not available ... [1, 17]`가 나오면 Adobe ORT 1.17.x와 plugin ORT 1.24.x 충돌 가능성이 높다. 임시 복사로 우회하지 말고 아래 `명시적 ORT 로딩 전략` 기준으로 점검한다.

결과 확인:
```cmd
if exist "build-win\plugin\Release\ZSoda.aex" (echo BUILD_AEX: OK) else (echo BUILD_AEX: MISSING)
if exist "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\ZSoda.aex" (echo MEDIA_AEX: OK) else (echo MEDIA_AEX: MISSING)
if exist "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\onnxruntime.dll" (echo MEDIA_ORT_DLL: OK) else (echo MEDIA_ORT_DLL: MISSING)
if exist "build-win\plugin\Release\ZSoda.pdb" (echo PDB: OK) else (echo PDB: MISSING)
if exist "build-win\plugin\Release\ZSoda.map" (echo MAP: OK) else (echo MAP: MISSING)
```

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
