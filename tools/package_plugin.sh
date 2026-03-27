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
  bash tools/package_plugin.sh --platform windows --build-dir build-win --output-dir dist \
    --ort-runtime-dir build-win/plugin/Release/zsoda_ort \
    --require-self-contained --golden-macos-fixture /path/to/ZSoda.plugin.zip
EOF
}

platform=""
build_dir=""
output_dir=""
include_manifest="0"
package_mode=""
ort_dir=""
python_dir=""
package_resource_root=""
package_root_dir_name=""
package_output_root=""
ort_output_dir=""
python_output_dir=""
models_output_dir=""
archive_path=""
archive_sha_path=""
payload_stage_dir=""
embedded_payload_status=""
artifact_hash_input=""
artifact_hash_label=""
python_runtime_dir=""
model_repo_dir=""
model_root_dir=""
hf_cache_dir=""
require_self_contained="0"
golden_macos_fixture=""
ort_runtime_dir=""
declare -a embedded_payload_roots=()
declare -a archive_entries=()

copy_dir_contents() {
  local source_dir="$1"
  local destination_dir="$2"
  mkdir -p "${destination_dir}"
  cp -R "${source_dir}/." "${destination_dir}/"
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

read_stage_plan_scalar() {
  local python_command="$1"
  local plan_path="$2"
  local field_name="$3"
  "${python_command}" - "${plan_path}" "${field_name}" <<'PY'
import json
import sys
from pathlib import Path

plan = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
value = plan.get(sys.argv[2], "")
if value is None:
    value = ""
print(value)
PY
}

emit_stage_plan_warnings() {
  local python_command="$1"
  local plan_path="$2"
  "${python_command}" - "${plan_path}" <<'PY'
import json
import sys
from pathlib import Path

plan = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
for warning in plan.get("warnings", []):
    if warning:
        print(warning)
PY
}

emit_stage_plan_roots() {
  local python_command="$1"
  local plan_path="$2"
  "${python_command}" - "${plan_path}" <<'PY'
import json
import sys
from pathlib import Path

plan = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
paths = plan.get("staged_root_paths", {})
for root_name in plan.get("staged_roots", []):
    staged_path = paths.get(root_name, "")
    if staged_path:
        print(f"{root_name}\t{staged_path}")
PY
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
    --package-mode)
      package_mode="${2:-}"
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
    --model-root-dir)
      model_root_dir="${2:-}"
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
    --golden-macos-fixture)
      golden_macos_fixture="${2:-}"
      shift 2
      ;;
    --ort-runtime-dir)
      ort_runtime_dir="${2:-}"
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

if [[ -z "${platform}" || -z "${build_dir}" || -z "${output_dir}" ]]; then
  echo "Missing required arguments." >&2
  print_usage
  exit 1
fi

if [[ -z "${package_mode}" ]]; then
  if [[ "${platform}" == "macos" ]]; then
    package_mode="sidecar-ort"
  else
    package_mode="embedded-windows"
  fi
fi

payload_python="$(resolve_payload_python)" || {
  echo "Python is required to prepare package staging layout." >&2
  exit 1
}

mkdir -p "${output_dir}"
stage_plan_path="${output_dir}/.package-stage.json"
stage_args=(
  "${payload_python}" "tools/prepare_package_stage.py"
  "--platform" "${platform}"
  "--package-mode" "${package_mode}"
  "--build-dir" "${build_dir}"
  "--output-dir" "${output_dir}"
  "--plan-out" "${stage_plan_path}"
  "--quiet"
)
if [[ "${include_manifest}" == "1" ]]; then
  stage_args+=("--include-manifest")
fi
if [[ -n "${python_runtime_dir}" ]]; then
  stage_args+=("--python-runtime-dir" "${python_runtime_dir}")
fi
if [[ -n "${model_repo_dir}" ]]; then
  stage_args+=("--model-repo-dir" "${model_repo_dir}")
fi
if [[ -n "${model_root_dir}" ]]; then
  stage_args+=("--model-root-dir" "${model_root_dir}")
fi
if [[ -n "${hf_cache_dir}" ]]; then
  stage_args+=("--hf-cache-dir" "${hf_cache_dir}")
fi
if [[ -n "${ort_runtime_dir}" ]]; then
  stage_args+=("--ort-runtime-dir" "${ort_runtime_dir}")
fi
if [[ "${require_self_contained}" == "1" ]]; then
  stage_args+=("--require-self-contained")
fi
"${stage_args[@]}"

artifact_source="$(read_stage_plan_scalar "${payload_python}" "${stage_plan_path}" "artifact_source")"
artifact_name="$(read_stage_plan_scalar "${payload_python}" "${stage_plan_path}" "artifact_name")"
payload_stage_dir="$(read_stage_plan_scalar "${payload_python}" "${stage_plan_path}" "payload_stage_dir")"
package_resource_subdir="$(read_stage_plan_scalar "${payload_python}" "${stage_plan_path}" "package_resource_subdir")"
package_root_dir_name="$(read_stage_plan_scalar "${payload_python}" "${stage_plan_path}" "package_root_dir_name")"
archive_name="$(read_stage_plan_scalar "${payload_python}" "${stage_plan_path}" "archive_name")"
if [[ -z "${package_root_dir_name}" ]]; then
  package_output_root="${output_dir}"
  artifact_hash_input="${artifact_name}"
  artifact_hash_label="${artifact_name}"
  archive_entries=("${artifact_name}")
else
  package_output_root="${output_dir}/${package_root_dir_name}"
  mkdir -p "${package_output_root}"
  artifact_hash_input="${package_root_dir_name}/${artifact_name}"
  artifact_hash_label="${artifact_hash_input}"
  archive_entries=("${package_root_dir_name}")
fi

if [[ "${require_self_contained}" == "1" && "${package_mode}" != "sidecar-ort" ]]; then
  if [[ -d "${payload_stage_dir}/zsoda_py" && -d "${payload_stage_dir}/models" ]]; then
    "${payload_python}" "tools/validate_self_contained_runtime.py" \
      --stage-root "${payload_stage_dir}" \
      --platform "${platform}" \
      --model-id "distill-any-depth-base" \
      --validate-device "cpu"
  fi
fi

while IFS= read -r warning; do
  if [[ -n "${warning}" ]]; then
    echo "warning: ${warning}"
  fi
done < <(emit_stage_plan_warnings "${payload_python}" "${stage_plan_path}")

if [[ -z "${artifact_source}" || -z "${artifact_name}" ]]; then
  echo "Package stage preparation did not resolve an artifact." >&2
  exit 1
fi

destination="${package_output_root}/${artifact_name}"
rm -rf "${destination}"
if [[ "${platform}" == "windows" ]]; then
  cp "${artifact_source}" "${destination}"
else
  cp -R "${artifact_source}" "${destination}"
fi

if [[ -z "${package_resource_subdir}" ]]; then
  package_resource_root="${package_output_root}"
else
  package_resource_root="${destination}/${package_resource_subdir}"
  mkdir -p "${package_resource_root}"
fi

while IFS=$'\t' read -r staged_root staged_path; do
  if [[ -z "${staged_root}" || -z "${staged_path}" ]]; then
    continue
  fi
  if [[ "${platform}" == "windows" && "${package_mode}" == "embedded-windows" ]]; then
    embedded_payload_roots+=("${staged_path}")
    case "${staged_root}" in
      models)
        models_output_dir="${staged_path}"
        ;;
      zsoda_ort)
        ort_output_dir="${staged_path}"
        ;;
      zsoda_py)
        python_output_dir="${staged_path}"
        ;;
    esac
  else
    if [[ "${platform}" == "windows" ]]; then
      resource_destination="${package_output_root}/${staged_root}"
      if [[ -z "${package_root_dir_name}" ]]; then
        archive_entries+=("${staged_root}")
      fi
    else
      resource_destination="${package_resource_root}/${staged_root}"
    fi
    rm -rf "${resource_destination}"
    copy_dir_contents "${staged_path}" "${resource_destination}"
    case "${staged_root}" in
      models)
        models_output_dir="${resource_destination}"
        ;;
      zsoda_ort)
        ort_output_dir="${resource_destination}"
        ;;
      zsoda_py)
        python_output_dir="${resource_destination}"
        ;;
    esac
  fi
done < <(emit_stage_plan_roots "${payload_python}" "${stage_plan_path}")

if [[ "${platform}" == "windows" && "${package_mode}" == "embedded-windows" && "${#embedded_payload_roots[@]}" -gt 0 ]]; then
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

  validation_args=("${payload_python}" "tools/check_release_readiness.py" "--inspect-windows-artifact" "${destination}" "--require-self-contained")
  if [[ -n "${golden_macos_fixture}" ]]; then
    if [[ ! -e "${golden_macos_fixture}" ]]; then
      echo "Golden macOS fixture was not found: ${golden_macos_fixture}" >&2
      exit 1
    fi
    validation_args+=("--compare-windows-artifact-to-macos-fixture" "${golden_macos_fixture}")
  fi
  "${validation_args[@]}"
  embedded_payload_status="${embedded_payload_status}; validation passed"
fi

if [[ "${platform}" == "macos" ]] && command -v codesign >/dev/null 2>&1; then
  codesign --force --sign - --timestamp=none --deep "${destination}"
fi

if [[ "${platform}" == "macos" ]]; then
  archive_path="${output_dir}/${archive_name}"
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
  archive_path="${output_dir}/${archive_name}"
  rm -f "${archive_path}"
  if command -v zip >/dev/null 2>&1; then
    (
      cd "${output_dir}"
      archive_name="$(basename "${archive_path}")"
      COPYFILE_DISABLE=1 zip -qry "${archive_name}" "${archive_entries[@]}"
    )
  else
    archive_path=""
  fi
fi

if command -v sha256sum >/dev/null 2>&1; then
  (
    cd "${output_dir}"
    if [[ "${platform}" == "windows" ]]; then
      sha256sum "${artifact_hash_input}" | sed "s#  ${artifact_hash_input}\$#  ${artifact_hash_label}#" > "${artifact_name}.sha256"
    else
      tar -cf - "${artifact_name}" | sha256sum | sed "s#  -\$#  ${artifact_name}#" > "${artifact_name}.sha256"
    fi
  )
elif command -v shasum >/dev/null 2>&1; then
  (
    cd "${output_dir}"
    if [[ "${platform}" == "windows" ]]; then
      shasum -a 256 "${artifact_hash_input}" | sed "s#  ${artifact_hash_input}\$#  ${artifact_hash_label}#" > "${artifact_name}.sha256"
    else
      tar -cf - "${artifact_name}" | shasum -a 256 | sed "s#  -\$#  ${artifact_name}#" > "${artifact_name}.sha256"
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
  if [[ "${platform}" == "windows" && "${package_mode}" == "embedded-windows" ]]; then
    echo "  models:   embedded"
  else
    echo "  models:   ${models_output_dir}"
  fi
fi
if [[ -n "${ort_output_dir}" ]]; then
  if [[ "${platform}" == "windows" && "${package_mode}" == "embedded-windows" ]]; then
    echo "  ort dir:  embedded"
  else
    echo "  ort dir:  ${ort_output_dir}"
  fi
fi
if [[ -n "${python_output_dir}" ]]; then
  if [[ "${platform}" == "windows" && "${package_mode}" == "embedded-windows" ]]; then
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
if [[ -n "${stage_plan_path}" ]]; then
  rm -f "${stage_plan_path}"
fi
