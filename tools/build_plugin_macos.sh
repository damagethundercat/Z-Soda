#!/usr/bin/env bash
set -euo pipefail

print_usage() {
  cat <<'EOF'
Usage:
  bash tools/build_plugin_macos.sh --ae-sdk-root <dir> --ort-include-dir <dir> --model-root-dir <dir> --ort-runtime-dir <dir> [options]

Options:
  --ae-sdk-root <dir>         Adobe After Effects SDK root (required if AE_SDK_ROOT is unset)
  --ort-sdk-root <dir>        Extracted ONNX Runtime macOS SDK root; defaults include/lib from here
  --ort-include-dir <dir>     Directory containing onnxruntime_cxx_api.h
  --ort-runtime-dir <dir>     Directory staged into Contents/Resources/zsoda_ort
  --sidecar-assets-dir <dir>  Prepared asset root containing models/ and zsoda_ort/
  --model-root-dir <dir>      Native ONNX model root containing models.manifest and *.onnx
  --build-dir <dir>           CMake build directory (default: build-mac)
  --config <name>             Build configuration (default: Release)
  --output-dir <dir>          Packaged output directory (default: dist-mac)
  --arch <arch>               CMAKE_OSX_ARCHITECTURES value (default: arm64)
  --deployment-target <ver>   CMAKE_OSX_DEPLOYMENT_TARGET value (default: 12.0)
  --copy-to-mediacore         Copy packaged bundle into Adobe MediaCore after packaging
  --mediacore-dir <dir>       MediaCore destination (default: /Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore)
  -h, --help                  Show this help

Examples:
  bash tools/build_plugin_macos.sh \
    --ae-sdk-root "/path/to/AfterEffectsSDK" \
    --ort-sdk-root "/path/to/onnxruntime-osx-arm64-1.23.2" \
    --sidecar-assets-dir "/path/to/mac-ort-assets"
EOF
}

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
ae_sdk_root="${AE_SDK_ROOT:-}"
ort_sdk_root=""
ort_include_dir=""
ort_runtime_dir=""
sidecar_assets_dir=""
model_root_dir=""
build_dir="build-mac"
config="Release"
output_dir="dist-mac"
arch="arm64"
deployment_target="12.0"
copy_to_mediacore="0"
mediacore_dir="/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ae-sdk-root)
      ae_sdk_root="${2:-}"
      shift 2
      ;;
    --ort-sdk-root)
      ort_sdk_root="${2:-}"
      shift 2
      ;;
    --ort-include-dir)
      ort_include_dir="${2:-}"
      shift 2
      ;;
    --ort-runtime-dir)
      ort_runtime_dir="${2:-}"
      shift 2
      ;;
    --sidecar-assets-dir)
      sidecar_assets_dir="${2:-}"
      shift 2
      ;;
    --model-root-dir)
      model_root_dir="${2:-}"
      shift 2
      ;;
    --build-dir)
      build_dir="${2:-}"
      shift 2
      ;;
    --config)
      config="${2:-}"
      shift 2
      ;;
    --output-dir)
      output_dir="${2:-}"
      shift 2
      ;;
    --arch)
      arch="${2:-}"
      shift 2
      ;;
    --deployment-target)
      deployment_target="${2:-}"
      shift 2
      ;;
    --copy-to-mediacore)
      copy_to_mediacore="1"
      shift
      ;;
    --mediacore-dir)
      mediacore_dir="${2:-}"
      shift 2
      ;;
    -h|--help)
      print_usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      print_usage
      exit 1
      ;;
  esac
done

if [[ -z "${ae_sdk_root}" ]]; then
  echo "AE SDK root is required. Pass --ae-sdk-root or set AE_SDK_ROOT." >&2
  exit 1
fi

if [[ ! -f "${ae_sdk_root}/Examples/Headers/AE_Effect.h" && ! -f "${ae_sdk_root}/Headers/AE_Effect.h" ]]; then
  echo "AE_Effect.h was not found under AE SDK root: ${ae_sdk_root}" >&2
  exit 1
fi

if [[ -n "${sidecar_assets_dir}" ]]; then
  if [[ -z "${model_root_dir}" ]]; then
    model_root_dir="${sidecar_assets_dir}/models"
  fi
  if [[ -z "${ort_runtime_dir}" ]]; then
    ort_runtime_dir="${sidecar_assets_dir}/zsoda_ort"
  fi
fi

if [[ -n "${ort_sdk_root}" ]]; then
  if [[ -z "${ort_include_dir}" ]]; then
    ort_include_dir="${ort_sdk_root}/include"
  fi
  if [[ -z "${ort_runtime_dir}" ]]; then
    ort_runtime_dir="${ort_sdk_root}/lib"
  fi
fi

if [[ -z "${ort_include_dir}" ]]; then
  echo "ORT include dir is required. Pass --ort-include-dir or --ort-sdk-root." >&2
  exit 1
fi
if [[ -z "${ort_runtime_dir}" ]]; then
  echo "ORT runtime dir is required. Pass --ort-runtime-dir, --sidecar-assets-dir, or --ort-sdk-root." >&2
  exit 1
fi
if [[ -z "${model_root_dir}" ]]; then
  echo "Native model root is required. Pass --model-root-dir or --sidecar-assets-dir." >&2
  exit 1
fi

if [[ ! -f "${ort_include_dir}/onnxruntime_cxx_api.h" && ! -f "${ort_include_dir}/onnxruntime/core/session/onnxruntime_cxx_api.h" ]]; then
  echo "onnxruntime_cxx_api.h was not found under ORT include dir: ${ort_include_dir}" >&2
  exit 1
fi
if [[ ! -f "${ort_runtime_dir}/libonnxruntime.dylib" ]]; then
  echo "libonnxruntime.dylib was not found under ORT runtime dir: ${ort_runtime_dir}" >&2
  exit 1
fi
if [[ ! -f "${model_root_dir}/models.manifest" ]]; then
  echo "models.manifest was not found under native model root: ${model_root_dir}" >&2
  exit 1
fi
if ! find "${model_root_dir}" -type f -name '*.onnx' | grep -q .; then
  echo "No ONNX model files were found under native model root: ${model_root_dir}" >&2
  exit 1
fi

cd "${repo_root}"

echo "[1/4] Configure mac build"
cmake -S . -B "${build_dir}" -G Xcode \
  -DZSODA_WITH_AE_SDK=ON \
  -DZSODA_WITH_ONNX_RUNTIME=ON \
  -DZSODA_WITH_ONNX_RUNTIME_API=ON \
  -DZSODA_ONNXRUNTIME_DIRECT_LINK_MODE=OFF \
  -DAE_SDK_ROOT="${ae_sdk_root}" \
  -DONNXRUNTIME_INCLUDE_DIR="${ort_include_dir}" \
  -DCMAKE_OSX_ARCHITECTURES="${arch}" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${deployment_target}"

echo "[2/4] Build zsoda_plugin_bundle (${config})"
cmake --build "${build_dir}" --config "${config}" --target zsoda_plugin_bundle

echo "[3/4] Package plugin bundle"
package_args=(
  bash tools/package_plugin.sh
  --platform macos
  --package-mode sidecar-ort
  --build-dir "${build_dir}"
  --output-dir "${output_dir}"
  --model-root-dir "${model_root_dir}"
  --ort-runtime-dir "${ort_runtime_dir}"
  --require-self-contained
)
"${package_args[@]}"

packaged_bundle="${output_dir}/ZSoda.plugin"
if [[ ! -d "${packaged_bundle}" ]]; then
  echo "Packaged bundle was not created: ${packaged_bundle}" >&2
  exit 1
fi

echo "[4/4] Verify packaged bundle"
file "${packaged_bundle}/Contents/MacOS/ZSoda"
nm -gU "${packaged_bundle}/Contents/MacOS/ZSoda" | rg "_EffectMain" >/dev/null
otool -l "${packaged_bundle}/Contents/MacOS/ZSoda" | rg -A4 "LC_BUILD_VERSION|LC_VERSION_MIN_MACOSX"
codesign -dv --verbose=2 "${packaged_bundle}" 2>&1 | sed -n '1,12p'

if [[ "${copy_to_mediacore}" == "1" ]]; then
  echo "[deploy] Copy bundle into MediaCore"
  if [[ ! -d "${mediacore_dir}" ]]; then
    echo "MediaCore directory not found: ${mediacore_dir}" >&2
    exit 1
  fi

  target_bundle="${mediacore_dir}/ZSoda.plugin"
  timestamp="$(date +%Y%m%d-%H%M%S)"
  if [[ -e "${target_bundle}" ]]; then
    backup_path="${target_bundle}.bak-${timestamp}"
    if [[ -w "${mediacore_dir}" ]]; then
      mv "${target_bundle}" "${backup_path}"
    else
      sudo mv "${target_bundle}" "${backup_path}"
    fi
    echo "Backed up existing bundle to ${backup_path}"
  fi

  if [[ -w "${mediacore_dir}" ]]; then
    cp -R "${packaged_bundle}" "${target_bundle}"
  else
    sudo cp -R "${packaged_bundle}" "${target_bundle}"
  fi
  echo "Installed ${target_bundle}"
fi
