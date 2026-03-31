# Z-Soda

Local depth maps for After Effects.

![Z-Soda hero](docs/assets/z-soda-hero.svg)

Z-Soda is an After Effects effect plug-in built around a single production-minded path:

- local inference only
- native ONNX Runtime sidecar packaging
- one shipping model: `distill-any-depth-base`
- two outputs: `Depth Map` and `Depth Slice`

## Why This Repo Exists

Z-Soda focuses on a simple install story and a predictable runtime shape.
The current public build avoids giant embedded binaries and keeps the plug-in
layout explicit:

- small `.aex` / `.plugin`
- bundled model folder
- bundled ORT runtime sidecar
- safe render fallbacks instead of host crashes

## Supported Platforms

| Platform | Status | Runtime path | Notes |
| --- | --- | --- | --- |
| Windows | Beta-tested | ONNX Runtime + DirectML sidecar | Recommended |
| macOS Apple Silicon | Beta-tested | ONNX Runtime + CoreML/CPU sidecar | Recommended |
| macOS Intel | Not supported yet | None | Not part of the current shipping target |

Beta validation was done against the current After Effects 2026 cycle.

## Installation

![Install shape](docs/assets/z-soda-install.svg)

### Windows

1. Quit After Effects.
2. Download the latest Windows package from [Releases](https://github.com/damagethundercat/Z-Soda/releases).
3. Unzip it.
4. Copy the `Z-Soda` folder into:
   `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\`
5. Launch After Effects and apply `Z-Soda` to a footage layer.

### macOS

1. Quit After Effects.
2. Download the latest macOS package from [Releases](https://github.com/damagethundercat/Z-Soda/releases).
3. Unzip it.
4. Copy `ZSoda.plugin` into your After Effects `Plug-ins` folder.
5. Launch After Effects and apply `Z-Soda`.

If macOS blocks a downloaded build, clear the quarantine flag:

```bash
xattr -dr com.apple.quarantine ZSoda.plugin
```

## What You Get

### Depth Map

Generates a normalized per-pixel depth result for grading, fog, focus, or 2.5D compositing.

### Depth Slice

Turns a depth range into a matte so you can isolate foreground, background, or a narrow depth band.

## Controls

| Control | What it does |
| --- | --- |
| `Quality` | Sets the real inference resolution. Higher values improve detail but take longer. |
| `Preserve Ratio` | Keeps the source aspect ratio during inference. |
| `Output` | Switches between `Depth Map` and `Depth Slice`. |
| `Color Map` | Changes how the depth map is visualized. |
| `Slice Mode` | Chooses whether the slice targets `Near`, `Far`, or a `Band`. |
| `Position (%)` | Moves the slice center or threshold along the depth range. |
| `Range (%)` | Widens or narrows the active slice band. |
| `Soft Border (%)` | Softens the edge of the slice matte. |

## Package Layout

### Windows

```text
Z-Soda/
  ZSoda.aex
  models/
    distill-any-depth/
      distill_any_depth_base.onnx
  zsoda_ort/
    onnxruntime.dll
    onnxruntime_providers_shared.dll
    DirectML.dll
```

### macOS

```text
ZSoda.plugin/
  Contents/
    MacOS/
      ZSoda
    Resources/
      models/
      zsoda_ort/
```

## Repository Layout

- [plugin/ae](plugin/ae): AE entry point, parameter wiring, host bridge
- [plugin/core](plugin/core): render pipeline, cache, frame transforms
- [plugin/inference](plugin/inference): ORT backend, model/runtime resolution
- [tests](tests): unit and integration coverage
- [tools](tools): build, packaging, and staging helpers
- [models/models.manifest](models/models.manifest): shipped model manifest

## Building From Source

You need:

- Adobe After Effects SDK
- CMake
- a C++ toolchain for your platform
- ONNX Runtime SDK/runtime files for the target platform

Primary entry points:

- Windows: `tools/build_aex.ps1`
- macOS: `tools/build_plugin_macos.sh`
- Shared packaging helpers:
  - `tools/prepare_ort_sidecar_release.py`
  - `tools/package_plugin.ps1`
  - `tools/package_plugin.sh`

## Current Scope

Z-Soda intentionally ships one narrow product path:

- one primary model family
- no cloud dependency
- ORT-first runtime
- sidecar packaging instead of giant embedded payloads

That constraint is deliberate. It keeps the plug-in easier to install, debug, and ship.

---

## 한국어

Z-Soda는 After Effects에서 로컬로 깊이맵을 생성하기 위한 플러그인입니다.

현재 제품 방향은 하나로 고정되어 있습니다.

- 로컬 추론만 사용
- 네이티브 ONNX Runtime sidecar 패키징
- 기본 모델: `distill-any-depth-base`
- 출력: `Depth Map`, `Depth Slice`

### 왜 이런 구조인가요?

Z-Soda는 설치와 배포를 단순하게 유지하는 것을 우선합니다.
현재 공개 빌드는 거대한 embedded 바이너리 대신 다음 구조를 사용합니다.

- 작은 `.aex` / `.plugin`
- 번들된 모델 폴더
- 번들된 ORT 런타임 sidecar
- 호스트 크래시 대신 안전한 렌더 fallback

### 지원 환경

| 플랫폼 | 상태 | 런타임 경로 | 비고 |
| --- | --- | --- | --- |
| Windows | 베타 테스트 완료 | ONNX Runtime + DirectML sidecar | 권장 |
| macOS Apple Silicon | 베타 테스트 완료 | ONNX Runtime + CoreML/CPU sidecar | 권장 |
| macOS Intel | 아직 미지원 | 없음 | 현재 배포 타깃 아님 |

현재 베타 검증은 After Effects 2026 기준으로 진행되었습니다.

### 설치

#### Windows

1. After Effects를 종료합니다.
2. [Releases](https://github.com/damagethundercat/Z-Soda/releases)에서 최신 Windows 패키지를 받습니다.
3. 압축을 풉니다.
4. `Z-Soda` 폴더를 아래 경로에 복사합니다.
   `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\`
5. After Effects를 실행하고 레이어에 `Z-Soda`를 적용합니다.

#### macOS

1. After Effects를 종료합니다.
2. [Releases](https://github.com/damagethundercat/Z-Soda/releases)에서 최신 macOS 패키지를 받습니다.
3. 압축을 풉니다.
4. `ZSoda.plugin`을 After Effects `Plug-ins` 폴더에 복사합니다.
5. After Effects를 실행하고 `Z-Soda`를 적용합니다.

macOS가 다운로드한 플러그인을 막는다면:

```bash
xattr -dr com.apple.quarantine ZSoda.plugin
```

### 출력

#### Depth Map

장면 전체의 깊이를 정규화된 맵으로 출력합니다. 안개, 포커스, 2.5D 합성 등에 사용할 수 있습니다.

#### Depth Slice

특정 깊이 구간만 매트처럼 잘라냅니다. 전경, 배경, 혹은 좁은 깊이 밴드를 분리할 때 사용합니다.

### 컨트롤

| 컨트롤 | 설명 |
| --- | --- |
| `Quality` | 실제 추론 해상도를 설정합니다. 높을수록 디테일은 좋아지고 속도는 느려집니다. |
| `Preserve Ratio` | 추론 시 원본 화면 비율을 유지합니다. |
| `Output` | `Depth Map`과 `Depth Slice` 사이를 전환합니다. |
| `Color Map` | 깊이맵의 시각화 색상을 바꿉니다. |
| `Slice Mode` | 슬라이스를 `Near`, `Far`, `Band` 중 어떤 방식으로 잡을지 정합니다. |
| `Position (%)` | 슬라이스 중심 또는 기준 위치를 옮깁니다. |
| `Range (%)` | 슬라이스 폭을 넓히거나 좁힙니다. |
| `Soft Border (%)` | 슬라이스 경계를 얼마나 부드럽게 할지 정합니다. |

### 패키지 구조

#### Windows

```text
Z-Soda/
  ZSoda.aex
  models/
    distill-any-depth/
      distill_any_depth_base.onnx
  zsoda_ort/
    onnxruntime.dll
    onnxruntime_providers_shared.dll
    DirectML.dll
```

#### macOS

```text
ZSoda.plugin/
  Contents/
    MacOS/
      ZSoda
    Resources/
      models/
      zsoda_ort/
```

### 소스 빌드

필요한 것:

- Adobe After Effects SDK
- CMake
- 플랫폼별 C++ toolchain
- 대상 플랫폼용 ONNX Runtime SDK/runtime 파일

주요 진입 스크립트:

- Windows: `tools/build_aex.ps1`
- macOS: `tools/build_plugin_macos.sh`
- 공용 패키징:
  - `tools/prepare_ort_sidecar_release.py`
  - `tools/package_plugin.ps1`
  - `tools/package_plugin.sh`

### 현재 방향

Z-Soda는 의도적으로 범위를 좁게 유지합니다.

- 하나의 주력 모델 계열
- 클라우드 의존성 없음
- ORT-first 런타임
- 거대한 embedded payload 대신 sidecar 패키징

이 제한은 의도된 선택입니다. 설치, 디버깅, 배포를 더 단순하게 유지하기 위한 방향입니다.
