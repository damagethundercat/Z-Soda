#!/usr/bin/env bash
set -euo pipefail

MODEL_ID="${1:-depth-anything-v3-small}"
MODEL_ROOT="${ZSODA_MODEL_ROOT:-models}"
MANIFEST_PATH="${ZSODA_MODEL_MANIFEST:-${MODEL_ROOT}/models.manifest}"

url=""
relative_path=""

trim() {
  local input="$1"
  input="${input#"${input%%[![:space:]]*}"}"
  input="${input%"${input##*[![:space:]]}"}"
  printf '%s' "${input}"
}

resolve_from_manifest() {
  local manifest="$1"
  local target_id="$2"
  [[ -f "${manifest}" ]] || return 1

  while IFS= read -r raw_line || [[ -n "${raw_line}" ]]; do
    local line="${raw_line%%#*}"
    line="$(trim "${line}")"
    [[ -z "${line}" ]] && continue

    local id=""
    local display_name=""
    local rel_path=""
    local model_url=""
    local preferred_default=""
    IFS='|' read -r id display_name rel_path model_url preferred_default <<< "${line}"

    id="$(trim "${id:-}")"
    rel_path="$(trim "${rel_path:-}")"
    model_url="$(trim "${model_url:-}")"
    if [[ "${id}" != "${target_id}" ]]; then
      continue
    fi

    if [[ -z "${rel_path}" || -z "${model_url}" ]]; then
      echo "매니페스트 항목 누락: ${id} (relative_path/download_url 필요)" >&2
      return 2
    fi

    echo "${model_url}|${rel_path}"
    return 0
  done < "${manifest}"
  return 1
}

resolved=""
manifest_status=1
if resolved="$(resolve_from_manifest "${MANIFEST_PATH}" "${MODEL_ID}")"; then
  manifest_status=0
else
  manifest_status=$?
fi

if [[ ${manifest_status} -eq 2 ]]; then
  exit 1
fi

if [[ ${manifest_status} -eq 0 ]]; then
  IFS='|' read -r url relative_path <<< "${resolved}"
fi

if [[ -z "${url}" || -z "${relative_path}" ]]; then
  case "${MODEL_ID}" in
    depth-anything-v3-small)
      url="https://huggingface.co/depth-anything/Depth-Anything-V3/resolve/main/depth_anything_v3_small.onnx"
      relative_path="depth-anything-v3/depth_anything_v3_small.onnx"
      ;;
    depth-anything-v3-base)
      url="https://huggingface.co/depth-anything/Depth-Anything-V3/resolve/main/depth_anything_v3_base.onnx"
      relative_path="depth-anything-v3/depth_anything_v3_base.onnx"
      ;;
    depth-anything-v3-large)
      url="https://huggingface.co/depth-anything/Depth-Anything-V3/resolve/main/depth_anything_v3_large.onnx"
      relative_path="depth-anything-v3/depth_anything_v3_large.onnx"
      ;;
    midas-dpt-large)
      url="https://github.com/isl-org/MiDaS/releases/download/v3_1/dpt_large_384.onnx"
      relative_path="midas/dpt_large_384.onnx"
      ;;
    *)
      echo "지원하지 않는 모델 ID: ${MODEL_ID}" >&2
      echo "확인 경로: ${MANIFEST_PATH}" >&2
      echo "기본 지원 목록: depth-anything-v3-small, depth-anything-v3-base, depth-anything-v3-large, midas-dpt-large" >&2
      exit 1
      ;;
  esac
fi

dest="${MODEL_ROOT}/${relative_path}"
mkdir -p "$(dirname "${dest}")"

echo "다운로드 시작: ${MODEL_ID}"
echo "URL: ${url}"
echo "저장 경로: ${dest}"
curl -fL "${url}" -o "${dest}"
echo "완료: ${dest}"
