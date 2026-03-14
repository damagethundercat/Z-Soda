#!/usr/bin/env bash
set -euo pipefail

print_usage() {
  cat <<'EOF'
Usage:
  bash tools/build_plugin_macos.sh --ae-sdk-root <dir> [options]

Options:
  --ae-sdk-root <dir>         Adobe After Effects SDK root (required if AE_SDK_ROOT is unset)
  --build-dir <dir>           CMake build directory (default: build-mac)
  --config <name>             Build configuration (default: Release)
  --output-dir <dir>          Packaged output directory (default: dist-mac)
  --arch <arch>               CMAKE_OSX_ARCHITECTURES value (default: arm64)
  --deployment-target <ver>   CMAKE_OSX_DEPLOYMENT_TARGET value (default: 12.0)
  --python-runtime-dir <dir>  Portable Python runtime to stage under zsoda_py/python
  --model-repo-dir <dir>      Directory whose children are local HF repos named by model id
  --hf-cache-dir <dir>        Optional Hugging Face cache directory to stage under models/hf-cache
  --require-self-contained    Fail packaging unless bundled runtime and local model repos are present
  --copy-to-mediacore         Copy packaged bundle into Adobe MediaCore after packaging
  --mediacore-dir <dir>       MediaCore destination (default: /Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore)
  --no-include-manifest       Skip copying models/ into the packaged bundle
  -h, --help                  Show this help

Examples:
  bash tools/build_plugin_macos.sh --ae-sdk-root "/path/to/AfterEffectsSDK"
  bash tools/build_plugin_macos.sh --ae-sdk-root "/path/to/AfterEffectsSDK" --copy-to-mediacore
EOF
}

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
ae_sdk_root="${AE_SDK_ROOT:-}"
build_dir="build-mac"
config="Release"
output_dir="dist-mac"
arch="arm64"
deployment_target="12.0"
copy_to_mediacore="0"
include_manifest="1"
mediacore_dir="/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore"
python_runtime_dir=""
model_repo_dir=""
hf_cache_dir=""
require_self_contained="0"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ae-sdk-root)
      ae_sdk_root="${2:-}"
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
    --python-runtime-dir)
      python_runtime_dir="${2:-}"
      shift 2
      ;;
    --model-repo-dir)
      model_repo_dir="${2:-}"
      shift 2
      ;;
    --hf-cache-dir)
      hf_cache_dir="${2:-}"
      shift 2
      ;;
    --require-self-contained)
      require_self_contained="1"
      shift
      ;;
    --copy-to-mediacore)
      copy_to_mediacore="1"
      shift
      ;;
    --mediacore-dir)
      mediacore_dir="${2:-}"
      shift 2
      ;;
    --no-include-manifest)
      include_manifest="0"
      shift
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

cd "${repo_root}"

echo "[1/4] Configure mac build"
cmake -S . -B "${build_dir}" -G Xcode \
  -DZSODA_WITH_AE_SDK=ON \
  -DAE_SDK_ROOT="${ae_sdk_root}" \
  -DCMAKE_OSX_ARCHITECTURES="${arch}" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${deployment_target}"

echo "[2/4] Build zsoda_plugin_bundle (${config})"
cmake --build "${build_dir}" --config "${config}" --target zsoda_plugin_bundle

echo "[3/4] Package plugin bundle"
package_args=(
  bash tools/package_plugin.sh
  --platform macos
  --build-dir "${build_dir}"
  --output-dir "${output_dir}"
)
if [[ "${include_manifest}" == "1" ]]; then
  package_args+=(--include-manifest)
fi
if [[ -n "${python_runtime_dir}" ]]; then
  package_args+=(--python-runtime-dir "${python_runtime_dir}")
fi
if [[ -n "${model_repo_dir}" ]]; then
  package_args+=(--model-repo-dir "${model_repo_dir}")
fi
if [[ -n "${hf_cache_dir}" ]]; then
  package_args+=(--hf-cache-dir "${hf_cache_dir}")
fi
if [[ "${require_self_contained}" == "1" ]]; then
  package_args+=(--require-self-contained)
fi
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
