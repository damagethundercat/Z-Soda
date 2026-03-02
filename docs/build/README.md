# AE Final-Mile Packaging Guide (.aex/.plugin)

This document records the packaging path from CMake build output to After Effects installable artifacts.

## Related Docs/Scripts

- Leader acceptance/remaining gates note:
  - `docs/build/2026-03-02-leader-review-note.md`
- Local Windows agent handoff:
  - `docs/build/LOCAL_AGENT_HANDOFF.md`
- AE smoke test guide:
  - `docs/build/AE_SMOKE_TEST.md`
- ORT runtime deploy note:
  - `docs/build/ORT_RUNTIME_DEPLOY.md`
- ORT runtime isolation plan (DLL collision prevention):
  - `docs/build/ORT_RUNTIME_ISOLATION_PLAN.md`
- Packaging helper scripts:
  - `tools/build_aex.ps1` (Windows CMake configure/build + `.aex` 해시 출력)
  - `tools/package_plugin.sh`
  - `tools/package_plugin.ps1`
- Local CI/scripted verification:
  - `tools/run_local_ci.sh`

## Scope

- Target artifacts:
  - Windows: `.aex`
  - macOS: `.plugin` bundle
- Current branch status:
  - `cmake` always builds `zsoda_plugin` (static library).
  - with `ZSODA_WITH_AE_SDK=ON` on native host:
    - Windows target `zsoda_aex` is generated (`ZSoda.aex`)
    - macOS target `zsoda_plugin_bundle` is generated (`ZSoda.plugin` bundle target with `Info.plist`)
  - Commands below include both:
    - what you can run now (scaffold verification)
    - final-mile packaging steps to apply when AE SDK-linked targets are added

## Prerequisites

- Adobe After Effects SDK extracted on local machine
  - Windows example: `C:\SDKs\AdobeAfterEffectsSDK`
  - macOS example: `$HOME/SDKs/AdobeAfterEffectsSDK`
  - Expected inside SDK root: headers + examples/skeleton directories from Adobe SDK package
- CMake `>= 3.21`
- Compiler toolchain:
  - Windows: Visual Studio 2022 (MSVC v143, x64)
  - macOS: Xcode (AppleClang)
- Optional model package root for runtime testing: `ZSODA_MODEL_ROOT`

## 명시적 ORT 로딩 전략 (DLL 충돌 방지)

배경 문제:
- AE 프로세스에 Adobe ORT 1.17.x가 먼저 로드된 상태에서, 플러그인이 ORT 1.24.x 기준으로 초기화하면 API/ABI 충돌이 발생할 수 있다.
- 대표 증상: `The requested API version [24] is not available ... [1, 17]`

원칙:
- `onnxruntime.dll` implicit link 제거(Import Table 의존 제거)
- 플러그인 내부에서 ORT DLL 절대경로를 계산해 `LoadLibraryExW`로 명시적 로드
- `OrtGetApiBase()->GetApi(ORT_API_VERSION)` API 버전 협상 실패 시 안전 fallback(패스스루/빈 출력)으로 전환

운영 시 확인할 로그:
- ORT 모듈 실제 경로
- ORT 파일 버전
- 요청 API 버전 및 협상 결과
- fallback 원인

단계별 적용:
1. Phase 0: 경로/버전/협상 로깅 추가(기준선 수집)
2. Phase 1: explicit loader opt-in 도입
3. Phase 2: explicit loader 기본값 전환
4. Phase 3: implicit 링크 경로 제거

상세 설계/검증/롤아웃은 `docs/build/ORT_RUNTIME_ISOLATION_PLAN.md`를 기준으로 한다.

## CMake Options (Packaging-Oriented)

- `-DZSODA_BUILD_TESTS=OFF`
  - keep packaging build minimal
- `-DCMAKE_BUILD_TYPE=Release` (single-config generators) or `--config Release` (multi-config)
- `-DAE_SDK_ROOT=<absolute-path>`
  - canonical SDK root path flag
  - if `AE_SDK_INCLUDE_DIR` is not set, build tries:
    - `${AE_SDK_ROOT}/Examples/Headers`
    - `${AE_SDK_ROOT}/Headers`
- `-DAE_SDK_INCLUDE_DIR=<absolute-path>`
  - directory that directly contains `AE_Effect.h`
- `-DZSODA_WITH_AE_SDK=ON`
  - enables conditional `EffectMain` entrypoint scaffold
- `-DZSODA_WITH_ONNX_RUNTIME=ON`
  - enables ONNX backend module in plugin build
- `-DZSODA_WITH_ONNX_RUNTIME_API=ON`
  - enables real ONNX Runtime C++ API execution path (default is scaffold/off)
  - requires:
    - `-DONNXRUNTIME_INCLUDE_DIR=<absolute-path-containing-onnxruntime_cxx_api.h>`
    - `-DONNXRUNTIME_LIBRARY=<absolute-path-to-onnxruntime-binary>`
  - if any required path is missing, configure step stops with `FATAL_ERROR`

## Windows Commands (PowerShell)

```powershell
$env:AE_SDK_ROOT = "C:\SDKs\AdobeAfterEffectsSDK"

cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64 `
  -DZSODA_BUILD_TESTS=OFF `
  -DZSODA_WITH_AE_SDK=ON `
  -DZSODA_WITH_ONNX_RUNTIME=ON `
  -DZSODA_WITH_ONNX_RUNTIME_API=ON `
  -DONNXRUNTIME_INCLUDE_DIR="C:\onnxruntime\include" `
  -DONNXRUNTIME_LIBRARY="C:\onnxruntime\lib\onnxruntime.lib" `
  -DCMAKE_BUILD_TYPE=Release `
  -DAE_SDK_ROOT="$env:AE_SDK_ROOT"

cmake --build build-win --config Release --target zsoda_plugin
cmake --build build-win --config Release --target zsoda_aex
```

Current expected artifacts:
- `build-win/plugin/Release/zsoda_plugin.lib`
- `build-win/plugin/Release/ZSoda.aex` (`zsoda_aex` target)

Final-mile packaging path (after AE SDK target wiring lands):

```powershell
# Example target name for final packaging stage
cmake --build build-win --config Release --target zsoda_aex

# Example deploy path used by Adobe shared plug-ins
Copy-Item "build-win/plugin/Release/ZSoda.aex" `
  "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\ZSoda.aex"
```

## Windows 빠른 시작 10단계

1. PowerShell(권장: "x64 Native Tools Command Prompt for VS 2022"에서 연 PowerShell)를 실행합니다.
2. AE SDK/ONNX Runtime 경로를 환경 변수로 지정합니다.

   ```powershell
   $env:AE_SDK_ROOT = "C:\SDKs\AdobeAfterEffectsSDK"
   $env:ORT_INCLUDE = "C:\onnxruntime\include"
   $env:ORT_LIB = "C:\onnxruntime\lib\onnxruntime.lib"
   ```

3. 필수 파일 존재를 먼저 확인합니다.

   ```powershell
   Test-Path "$env:AE_SDK_ROOT\Examples\Headers\AE_Effect.h"
   Test-Path "$env:ORT_INCLUDE\onnxruntime_cxx_api.h"
   Test-Path "$env:ORT_LIB"
   ```

4. CMake configure를 수행합니다.

   ```powershell
   cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64 `
     -DZSODA_BUILD_TESTS=OFF `
     -DZSODA_WITH_AE_SDK=ON `
     -DZSODA_WITH_ONNX_RUNTIME=ON `
     -DZSODA_WITH_ONNX_RUNTIME_API=ON `
     -DONNXRUNTIME_INCLUDE_DIR="$env:ORT_INCLUDE" `
     -DONNXRUNTIME_LIBRARY="$env:ORT_LIB" `
     -DCMAKE_BUILD_TYPE=Release `
     -DAE_SDK_ROOT="$env:AE_SDK_ROOT"
   ```

   ```powershell
   # Shortcut: 위 configure/build 과정을 한 번에 실행하려면
   .\tools\build_aex.ps1 `
     -AeSdkIncludeDir "$env:AE_SDK_ROOT\Examples\Headers" `
     -OrtIncludeDir "$env:ORT_INCLUDE" `
     -OrtLibrary "$env:ORT_LIB" `
     -BuildDir "build-win" `
     -Config Release
   ```

5. 공통 코어 타깃(`zsoda_plugin`)을 빌드합니다.

   ```powershell
   cmake --build build-win --config Release --target zsoda_plugin
   ```

6. 최종 `.aex` 타깃(`zsoda_aex`)을 빌드합니다.

   ```powershell
   cmake --build build-win --config Release --target zsoda_aex
   ```

7. 생성 산출물(`build-win/plugin/Release/ZSoda.aex`)을 확인합니다.
8. After Effects를 종료한 상태에서 공유 플러그인 경로에 복사합니다.

   ```powershell
   Copy-Item "build-win/plugin/Release/ZSoda.aex" `
     "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\ZSoda.aex" -Force
   ```

9. After Effects를 재실행하고 플러그인 목록에서 `ZSoda` 로드 여부를 확인합니다.
10. 배포 폴더가 필요하면 패키징 스크립트로 `.aex`를 수집합니다.

   ```powershell
   .\tools\package_plugin.ps1 -Platform windows -BuildDir build-win -OutputDir dist -IncludeManifest
   ```

   ```powershell
   # Optional: onnxruntime.dll까지 함께 복사
   .\tools\package_plugin.ps1 -Platform windows -BuildDir build-win -OutputDir dist -IncludeManifest `
     -OrtRuntimeDllPath "C:\onnxruntime\lib\onnxruntime.dll"
   ```

## 사용자 테스트 가능 시점

- **Windows 환경 준비 즉시** 사용자 테스트를 시작할 수 있습니다.
- 기준 조건:
  - AE SDK/VS2022/CMake/ONNX Runtime 경로 검증 완료
  - `zsoda_aex` 빌드 성공
  - `MediaCore` 경로 복사 후 AE에서 플러그인 로드 확인

## 실패 시 점검 6항목

1. `AE_Effect.h` 미검출:
   - 증상: configure 단계에서 AE SDK include 경로 관련 오류
   - 점검: `Test-Path "$env:AE_SDK_ROOT\Examples\Headers\AE_Effect.h"`
2. ONNX Runtime 헤더/라이브러리 불일치:
   - 증상: configure 또는 link 단계 실패
   - 점검: `Test-Path "$env:ORT_INCLUDE\onnxruntime_cxx_api.h"` / `Test-Path "$env:ORT_LIB"`
3. 아키텍처 불일치(x86 vs x64):
   - 증상: 링크 오류 또는 `.aex` 로드 실패
   - 점검: configure에 `-A x64`가 포함되었는지 확인
4. `zsoda_aex` 타깃 미생성:
   - 증상: `cmake --build ... --target zsoda_aex` 실패
   - 점검: configure 로그에서 `-DZSODA_WITH_AE_SDK=ON` 반영 여부 확인
5. MediaCore 복사 실패(권한/경로):
   - 증상: `Copy-Item` 접근 거부 또는 경로 오류
   - 점검: 관리자 권한 PowerShell 사용, `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore` 존재 확인
6. ORT DLL 충돌(Adobe 1.17.x vs Plugin 1.24.x):
   - 증상: `The requested API version [24] is not available ... [1, 17]`, 초기화 실패/비정상 종료
   - 점검: 플러그인 로그의 ORT 모듈 경로/파일 버전/API 협상 결과 확인
   - 기대 동작: 충돌 시 crash 대신 fallback 출력

## 산출물 확인 명령

```powershell
$AexPath = "build-win/plugin/Release/ZSoda.aex"
$MediaCorePath = "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\ZSoda.aex"

# 1) 빌드 산출물 존재 여부
Test-Path $AexPath

# 2) 파일 메타데이터(크기/수정시각)
Get-Item $AexPath | Format-List FullName, Length, LastWriteTime

# 3) 무결성 확인용 해시
Get-FileHash $AexPath -Algorithm SHA256

# 4) MediaCore 배포본 존재 여부 및 메타데이터
Test-Path $MediaCorePath
if (Test-Path $MediaCorePath) {
  Get-Item $MediaCorePath | Format-List FullName, Length, LastWriteTime
}
```

## macOS Commands (zsh/bash)

```bash
export AE_SDK_ROOT="$HOME/SDKs/AdobeAfterEffectsSDK"

cmake -S . -B build-mac -G Xcode \
  -DZSODA_BUILD_TESTS=OFF \
  -DZSODA_WITH_AE_SDK=ON \
  -DZSODA_WITH_ONNX_RUNTIME=ON \
  -DZSODA_WITH_ONNX_RUNTIME_API=ON \
  -DONNXRUNTIME_INCLUDE_DIR="$HOME/onnxruntime/include" \
  -DONNXRUNTIME_LIBRARY="$HOME/onnxruntime/lib/libonnxruntime.dylib" \
  -DCMAKE_BUILD_TYPE=Release \
  -DAE_SDK_ROOT="$AE_SDK_ROOT"

cmake --build build-mac --config Release --target zsoda_plugin
cmake --build build-mac --config Release --target zsoda_plugin_bundle
```

Current expected artifacts:
- `build-mac/plugin/Release/libzsoda_plugin.a` (or generator-specific static library output)
- `build-mac/plugin/Release/ZSoda.plugin` (`zsoda_plugin_bundle` target)

Final-mile packaging path (after AE SDK target wiring lands):

```bash
# Example target name for final packaging stage
cmake --build build-mac --config Release --target zsoda_plugin_bundle

# Example deploy path used by Adobe shared plug-ins
cp -R "build-mac/plugin/Release/ZSoda.plugin" \
  "/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/"
```

## Packaging Helper Scripts

After a successful native build, use helper scripts to collect artifacts into a deployable folder.

macOS/Linux shell:

```bash
bash tools/package_plugin.sh --platform windows --build-dir build-win --output-dir dist
bash tools/package_plugin.sh --platform macos --build-dir build-mac --output-dir dist --include-manifest
```

Windows PowerShell:

```powershell
.\tools\package_plugin.ps1 -Platform windows -BuildDir build-win -OutputDir dist -IncludeManifest
.\tools\package_plugin.ps1 -Platform macos -BuildDir build-mac -OutputDir dist
```

## Expected Artifact Checklist

When packaging integration is complete, verify:

- Windows:
  - `ZSoda.aex`
  - any required runtime sidecars (if backend-specific runtime DLLs are needed)
- macOS:
  - `ZSoda.plugin/Contents/MacOS/ZSoda`
  - `Info.plist`, code-signing state (if distribution policy requires signing)
- Models:
  - model weights are delivered separately (not committed into git)

## Current Environment Limitations (as of 2026-03-02)

- Working environment is Linux/WSL2, not native Windows/macOS.
- `cmake` is not installed in this environment.
- `xcodebuild`/MSBuild are unavailable here.
- Adobe AE SDK is not present in this workspace.
- Therefore `.aex/.plugin` end-to-end packaging commands are documented but not executable in the current environment until prerequisites are installed on target OS hosts.
