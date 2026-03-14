#!/usr/bin/env bash
set -euo pipefail

print_usage() {
  cat <<'EOF'
Usage:
  bash tools/package_plugin.sh --platform <windows|macos> --build-dir <dir> --output-dir <dir> [options]

Examples:
  bash tools/package_plugin.sh --platform windows --build-dir build-win --output-dir dist
  bash tools/package_plugin.sh --platform macos --build-dir build-mac --output-dir dist --include-manifest
  bash tools/package_plugin.sh --platform macos --build-dir build-mac --output-dir dist \
    --include-manifest --python-runtime-dir release-assets/python-macos \
    --model-repo-dir release-assets/models
EOF
}

platform=""
build_dir=""
output_dir=""
include_manifest="0"
ort_dir=""
python_dir=""
package_resource_root=""
ort_output_dir=""
python_output_dir=""
models_output_dir=""
archive_path=""
archive_sha_path=""
payload_stage_dir=""
embedded_payload_status=""
python_runtime_dir=""
model_repo_dir=""
hf_cache_dir=""
require_self_contained="0"
declare -a embedded_payload_roots=()

copy_dir_contents() {
  local source_dir="$1"
  local destination_dir="$2"
  mkdir -p "${destination_dir}"
  cp -R "${source_dir}/." "${destination_dir}/"
}

stage_model_repos() {
  local source_root="$1"
  local destination_root="$2"
  if [[ ! -d "${source_root}" ]]; then
    return 0
  fi
  mkdir -p "${destination_root}"
  local found_repo="0"
  local entry=""
  for entry in "${source_root}"/*; do
    if [[ ! -d "${entry}" ]]; then
      continue
    fi
    found_repo="1"
    local model_id
    model_id="$(basename "${entry}")"
    rm -rf "${destination_root}/${model_id}"
    copy_dir_contents "${entry}" "${destination_root}/${model_id}"
  done
  if [[ "${found_repo}" != "1" ]]; then
    echo "Model repo directory does not contain any model subdirectories: ${source_root}" >&2
    exit 1
  fi
}

assert_self_contained_payload() {
  local stage_root="$1"
  local platform_name="$2"
  local stage_python_dir="${stage_root}/zsoda_py"
  local stage_models_hf_dir="${stage_root}/models/hf"
  local service_script="${stage_python_dir}/distill_any_depth_remote_service.py"
  local python_ok="0"

  if [[ ! -f "${service_script}" ]]; then
    echo "Self-contained packaging requires bundled service script: ${service_script}" >&2
    exit 1
  fi

  if [[ "${platform_name}" == "windows" ]]; then
    if [[ -f "${stage_python_dir}/python.exe" || -f "${stage_python_dir}/python/python.exe" || -f "${stage_python_dir}/runtime/python.exe" ]]; then
      python_ok="1"
    fi
  else
    if [[ -x "${stage_python_dir}/bin/python3" || -x "${stage_python_dir}/python/bin/python3" || -x "${stage_python_dir}/runtime/bin/python3" ]]; then
      python_ok="1"
    fi
  fi

  if [[ "${python_ok}" != "1" ]]; then
    echo "Self-contained packaging requires a bundled Python runtime under zsoda_py/." >&2
    exit 1
  fi

  if [[ ! -d "${stage_models_hf_dir}" ]]; then
    echo "Self-contained packaging requires bundled local model repos under models/hf/." >&2
    exit 1
  fi

  local model_repo_found="0"
  local entry=""
  for entry in "${stage_models_hf_dir}"/*; do
    if [[ -d "${entry}" ]]; then
      model_repo_found="1"
      break
    fi
  done
  if [[ "${model_repo_found}" != "1" ]]; then
    echo "Self-contained packaging requires at least one local model repo under models/hf/." >&2
    exit 1
  fi
}

resolve_payload_python() {
  if command -v python3 >/dev/null 2>&1; then
    command -v python3
    return 0
  fi
  if command -v python >/dev/null 2>&1; then
    command -v python
    return 0
  fi
  return 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --platform)
      platform="${2:-}"
      shift 2
      ;;
    --build-dir)
      build_dir="${2:-}"
      shift 2
      ;;
    --output-dir)
      output_dir="${2:-}"
      shift 2
      ;;
    --include-manifest)
      include_manifest="1"
      shift
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

if [[ -z "${platform}" || -z "${build_dir}" || -z "${output_dir}" ]]; then
  echo "Missing required arguments." >&2
  print_usage
  exit 1
fi

if [[ -z "${model_repo_dir}" && -d "release-assets/models" ]]; then
  model_repo_dir="release-assets/models"
fi
if [[ -z "${hf_cache_dir}" && -d "release-assets/hf-cache" ]]; then
  hf_cache_dir="release-assets/hf-cache"
fi
if [[ -z "${python_runtime_dir}" ]]; then
  case "${platform}" in
    windows)
      if [[ -d "release-assets/python-win" ]]; then
        python_runtime_dir="release-assets/python-win"
      fi
      ;;
    macos)
      if [[ -d "release-assets/python-macos" ]]; then
        python_runtime_dir="release-assets/python-macos"
      fi
      ;;
  esac
fi

artifact_source=""
artifact_name=""

case "${platform}" in
  windows)
    artifact_name="ZSoda.aex"
    for candidate in \
      "${build_dir}/plugin/Release/${artifact_name}" \
      "${build_dir}/plugin/${artifact_name}"; do
      if [[ -f "${candidate}" ]]; then
        artifact_source="${candidate}"
        break
      fi
    done
    ;;
  macos)
    artifact_name="ZSoda.plugin"
    for candidate in \
      "${build_dir}/plugin/Release/${artifact_name}" \
      "${build_dir}/plugin/${artifact_name}"; do
      if [[ -d "${candidate}" ]]; then
        artifact_source="${candidate}"
        break
      fi
    done
    ;;
  *)
    echo "Unsupported platform: ${platform}. Use windows or macos." >&2
    exit 1
    ;;
esac

if [[ -z "${artifact_source}" ]]; then
  echo "Artifact not found for platform '${platform}' under build dir '${build_dir}'." >&2
  exit 1
fi

mkdir -p "${output_dir}"
destination="${output_dir}/${artifact_name}"
rm -rf "${destination}"
payload_stage_dir="${output_dir}/.payload-stage"
rm -rf "${payload_stage_dir}"
mkdir -p "${payload_stage_dir}"

if [[ "${platform}" == "windows" ]]; then
  cp "${artifact_source}" "${destination}"
else
  cp -R "${artifact_source}" "${destination}"
fi

if [[ "${platform}" == "windows" ]]; then
  package_resource_root="${output_dir}"
else
  package_resource_root="${destination}/Contents/Resources"
  mkdir -p "${package_resource_root}"
fi

if [[ "${include_manifest}" == "1" && -d "models" ]]; then
  models_output_dir="${payload_stage_dir}/models"
  rm -rf "${models_output_dir}"
  copy_dir_contents "models" "${models_output_dir}"
fi

if [[ -n "${model_repo_dir}" ]]; then
  if [[ ! -d "${model_repo_dir}" ]]; then
    echo "Model repo directory was not found: ${model_repo_dir}" >&2
    exit 1
  fi
  models_output_dir="${payload_stage_dir}/models"
  stage_model_repos "${model_repo_dir}" "${models_output_dir}/hf"
fi

if [[ -n "${hf_cache_dir}" ]]; then
  if [[ ! -d "${hf_cache_dir}" ]]; then
    echo "HF cache directory was not found: ${hf_cache_dir}" >&2
    exit 1
  fi
  models_output_dir="${payload_stage_dir}/models"
  rm -rf "${models_output_dir}/hf-cache"
  copy_dir_contents "${hf_cache_dir}" "${models_output_dir}/hf-cache"
fi

for candidate in \
  "${build_dir}/plugin/Release/zsoda_ort" \
  "${build_dir}/plugin/zsoda_ort"; do
  if [[ -d "${candidate}" ]]; then
    ort_dir="${candidate}"
    break
  fi
done
if [[ -n "${ort_dir}" ]]; then
  ort_output_dir="${payload_stage_dir}/zsoda_ort"
  rm -rf "${ort_output_dir}"
  copy_dir_contents "${ort_dir}" "${ort_output_dir}"
fi

for candidate in \
  "${build_dir}/plugin/Release/zsoda_py" \
  "${build_dir}/plugin/zsoda_py"; do
  if [[ -d "${candidate}" ]]; then
    python_dir="${candidate}"
    break
  fi
done
if [[ -n "${python_dir}" ]]; then
  python_output_dir="${payload_stage_dir}/zsoda_py"
  rm -rf "${python_output_dir}"
  copy_dir_contents "${python_dir}" "${python_output_dir}"
elif [[ -f "tools/distill_any_depth_remote_service.py" ]]; then
  python_output_dir="${payload_stage_dir}/zsoda_py"
  mkdir -p "${python_output_dir}"
  cp "tools/distill_any_depth_remote_service.py" "${python_output_dir}/distill_any_depth_remote_service.py"
fi

if [[ -n "${python_runtime_dir}" ]]; then
  if [[ ! -d "${python_runtime_dir}" ]]; then
    echo "Python runtime directory was not found: ${python_runtime_dir}" >&2
    exit 1
  fi
  python_output_dir="${payload_stage_dir}/zsoda_py"
  mkdir -p "${python_output_dir}"
  rm -rf "${python_output_dir}/python"
  copy_dir_contents "${python_runtime_dir}" "${python_output_dir}/python"
fi

if [[ "${require_self_contained}" == "1" ]]; then
  assert_self_contained_payload "${payload_stage_dir}" "${platform}"
fi

for staged_root in models zsoda_ort zsoda_py; do
  if [[ -d "${payload_stage_dir}/${staged_root}" ]]; then
    if [[ "${platform}" == "windows" ]]; then
      embedded_payload_roots+=("${payload_stage_dir}/${staged_root}")
    else
      rm -rf "${package_resource_root}/${staged_root}"
      copy_dir_contents "${payload_stage_dir}/${staged_root}" "${package_resource_root}/${staged_root}"
      case "${staged_root}" in
        models)
          models_output_dir="${package_resource_root}/${staged_root}"
          ;;
        zsoda_ort)
          ort_output_dir="${package_resource_root}/${staged_root}"
          ;;
        zsoda_py)
          python_output_dir="${package_resource_root}/${staged_root}"
          ;;
      esac
    fi
  fi
done

if [[ "${platform}" == "windows" && "${#embedded_payload_roots[@]}" -gt 0 ]]; then
  payload_python="$(resolve_payload_python)" || {
    echo "Python is required to build embedded Windows payloads." >&2
    exit 1
  }
  payload_args=("${payload_python}" "tools/build_embedded_payload.py" "--artifact" "${destination}")
  for payload_root in "${embedded_payload_roots[@]}"; do
    payload_args+=("--root" "${payload_root}")
  done
  "${payload_args[@]}"
  embedded_payload_status="embedded ${#embedded_payload_roots[@]} root(s) into ${destination}"
fi

if [[ "${platform}" == "macos" ]] && command -v codesign >/dev/null 2>&1; then
  codesign --force --sign - --timestamp=none --deep "${destination}"
fi

if [[ "${platform}" == "macos" ]]; then
  archive_path="${output_dir}/ZSoda-macos.zip"
  rm -f "${archive_path}"
  if command -v ditto >/dev/null 2>&1; then
    (
      cd "${output_dir}"
      COPYFILE_DISABLE=1 ditto -c -k --norsrc --keepParent "${artifact_name}" "$(basename "${archive_path}")"
    )
  elif command -v zip >/dev/null 2>&1; then
    (
      cd "${output_dir}"
      COPYFILE_DISABLE=1 zip -qry "$(basename "${archive_path}")" "${artifact_name}"
    )
  else
    archive_path=""
  fi
else
  archive_path="${output_dir}/ZSoda-windows.zip"
  rm -f "${archive_path}"
  if command -v zip >/dev/null 2>&1; then
    (
      cd "${output_dir}"
      archive_name="$(basename "${archive_path}")"
      COPYFILE_DISABLE=1 zip -qry "${archive_name}" "${artifact_name}"
    )
  else
    archive_path=""
  fi
fi

if command -v sha256sum >/dev/null 2>&1; then
  (
    cd "${output_dir}"
    if [[ "${platform}" == "windows" ]]; then
      sha256sum "${artifact_name}" > "${artifact_name}.sha256"
    else
      tar -cf - "${artifact_name}" | sha256sum > "${artifact_name}.sha256"
    fi
  )
elif command -v shasum >/dev/null 2>&1; then
  (
    cd "${output_dir}"
    if [[ "${platform}" == "windows" ]]; then
      shasum -a 256 "${artifact_name}" > "${artifact_name}.sha256"
    else
      tar -cf - "${artifact_name}" | shasum -a 256 > "${artifact_name}.sha256"
    fi
  )
fi

if [[ -n "${archive_path}" && -f "${archive_path}" ]]; then
  archive_sha_path="${archive_path}.sha256"
  if command -v sha256sum >/dev/null 2>&1; then
    (
      cd "${output_dir}"
      sha256sum "$(basename "${archive_path}")" > "$(basename "${archive_sha_path}")"
    )
  elif command -v shasum >/dev/null 2>&1; then
    (
      cd "${output_dir}"
      shasum -a 256 "$(basename "${archive_path}")" > "$(basename "${archive_sha_path}")"
    )
  else
    archive_sha_path=""
  fi
fi

echo "Packaged artifact:"
echo "  platform: ${platform}"
echo "  source:   ${artifact_source}"
echo "  output:   ${destination}"
if [[ -n "${models_output_dir}" ]]; then
  if [[ "${platform}" == "windows" ]]; then
    echo "  models:   embedded"
  else
    echo "  models:   ${models_output_dir}"
  fi
fi
if [[ -n "${ort_output_dir}" ]]; then
  if [[ "${platform}" == "windows" ]]; then
    echo "  ort dir:  embedded"
  else
    echo "  ort dir:  ${ort_output_dir}"
  fi
fi
if [[ -n "${python_output_dir}" ]]; then
  if [[ "${platform}" == "windows" ]]; then
    echo "  python:   embedded"
  else
    echo "  python:   ${python_output_dir}"
  fi
fi
if [[ -n "${embedded_payload_status}" ]]; then
  echo "  embedded: ${embedded_payload_status}"
fi
if [[ -n "${archive_path}" && -f "${archive_path}" ]]; then
  echo "  archive:  ${archive_path}"
fi
if [[ -n "${archive_sha_path}" && -f "${archive_sha_path}" ]]; then
  echo "  archive sha256: ${archive_sha_path}"
fi

if [[ -n "${payload_stage_dir}" ]]; then
  rm -rf "${payload_stage_dir}"
fi
