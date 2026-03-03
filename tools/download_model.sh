#!/usr/bin/env bash
set -euo pipefail

MODEL_ID="${1:-depth-anything-v3-small}"
MODEL_ROOT="${ZSODA_MODEL_ROOT:-models}"
MANIFEST_PATH="${ZSODA_MODEL_MANIFEST:-${MODEL_ROOT}/models.manifest}"
HF_TOKEN_VALUE="${HF_TOKEN:-${HUGGINGFACE_TOKEN:-${HUGGING_FACE_HUB_TOKEN:-}}}"
declare -a asset_urls=()
declare -a asset_paths=()

trim() {
  local input="$1"
  input="${input#"${input%%[![:space:]]*}"}"
  input="${input%"${input##*[![:space:]]}"}"
  printf '%s' "${input}"
}

add_asset() {
  local relative_path="$1"
  local url="$2"
  relative_path="$(trim "${relative_path}")"
  url="$(trim "${url}")"
  if [[ -z "${relative_path}" || -z "${url}" ]]; then
    echo "모델 자산 항목 누락(relative_path/url)" >&2
    return 1
  fi
  asset_paths+=("${relative_path}")
  asset_urls+=("${url}")
}

parse_auxiliary_assets() {
  local raw="$1"
  raw="$(trim "${raw}")"
  [[ -z "${raw}" ]] && return 0

  local IFS=';'
  read -r -a entries <<< "${raw}"
  local entry=""
  for entry in "${entries[@]}"; do
    entry="$(trim "${entry}")"
    [[ -z "${entry}" ]] && continue
    if [[ "${entry}" != *"::"* ]]; then
      echo "매니페스트 auxiliary_assets 형식 오류: ${entry}" >&2
      return 1
    fi
    local rel_path="${entry%%::*}"
    local url="${entry#*::}"
    add_asset "${rel_path}" "${url}" || return 1
  done
}

ensure_ort_external_data_alias() {
  local downloaded_path="$1"
  local leaf
  leaf="$(basename "${downloaded_path}")"
  if [[ "${leaf}" != *.onnx_data ]]; then
    return 0
  fi
  if [[ "${leaf}" == "model.onnx_data" ]]; then
    return 0
  fi

  local directory
  directory="$(dirname "${downloaded_path}")"
  local alias_path="${directory}/model.onnx_data"
  if [[ "${alias_path}" == "${downloaded_path}" ]]; then
    return 0
  fi

  cp -f "${downloaded_path}" "${alias_path}"
  echo "Alias: ${alias_path} <= ${downloaded_path}"
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
    local auxiliary_assets=""
    IFS='|' read -r id display_name rel_path model_url preferred_default auxiliary_assets _ <<< "${line}"

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

    asset_urls=()
    asset_paths=()
    add_asset "${rel_path}" "${model_url}" || return 2
    parse_auxiliary_assets "${auxiliary_assets}" || return 2
    return 0
  done < "${manifest}"
  return 1
}

manifest_status=1
if resolve_from_manifest "${MANIFEST_PATH}" "${MODEL_ID}"; then
  manifest_status=0
else
  manifest_status=$?
fi

if [[ ${manifest_status} -eq 2 ]]; then
  exit 1
fi

if [[ ${manifest_status} -ne 0 ]]; then
  asset_urls=()
  asset_paths=()
  case "${MODEL_ID}" in
    depth-anything-v3-small)
      add_asset "depth-anything-v3/depth_anything_v3_small.onnx" \
        "https://huggingface.co/onnx-community/depth-anything-v3-small/resolve/main/onnx/model.onnx"
      add_asset "depth-anything-v3/depth_anything_v3_small.onnx_data" \
        "https://huggingface.co/onnx-community/depth-anything-v3-small/resolve/main/onnx/model.onnx_data"
      ;;
    depth-anything-v3-base)
      add_asset "depth-anything-v3/depth_anything_v3_base.onnx" \
        "https://huggingface.co/onnx-community/depth-anything-v3-base/resolve/main/onnx/model.onnx"
      add_asset "depth-anything-v3/depth_anything_v3_base.onnx_data" \
        "https://huggingface.co/onnx-community/depth-anything-v3-base/resolve/main/onnx/model.onnx_data"
      ;;
    depth-anything-v3-large)
      add_asset "depth-anything-v3/depth_anything_v3_large.onnx" \
        "https://huggingface.co/onnx-community/depth-anything-v3-large/resolve/main/onnx/model.onnx"
      add_asset "depth-anything-v3/depth_anything_v3_large.onnx_data" \
        "https://huggingface.co/onnx-community/depth-anything-v3-large/resolve/main/onnx/model.onnx_data"
      ;;
    midas-dpt-large)
      add_asset "midas/dpt_large_384.onnx" \
        "https://github.com/isl-org/MiDaS/releases/download/v3_1/dpt_large_384.onnx"
      ;;
    *)
      echo "지원하지 않는 모델 ID: ${MODEL_ID}" >&2
      echo "확인 경로: ${MANIFEST_PATH}" >&2
      echo "기본 지원 목록: depth-anything-v3-small, depth-anything-v3-base, depth-anything-v3-large, midas-dpt-large" >&2
      exit 1
      ;;
  esac
fi

if [[ ${#asset_urls[@]} -eq 0 || ${#asset_urls[@]} -ne ${#asset_paths[@]} ]]; then
  echo "다운로드 자산 목록이 비어 있거나 손상되었습니다." >&2
  exit 1
fi

echo "다운로드 시작: ${MODEL_ID}"
for i in "${!asset_urls[@]}"; do
  url="${asset_urls[$i]}"
  relative_path="${asset_paths[$i]}"
  dest="${MODEL_ROOT}/${relative_path}"
  mkdir -p "$(dirname "${dest}")"

  echo "URL: ${url}"
  echo "저장 경로: ${dest}"
  curl_args=(-fL)
  if [[ -n "${HF_TOKEN_VALUE}" && "${url}" == https://huggingface.co/* ]]; then
    curl_args+=(-H "Authorization: Bearer ${HF_TOKEN_VALUE}")
  fi
  curl "${curl_args[@]}" "${url}" -o "${dest}"
  ensure_ort_external_data_alias "${dest}"
  echo "완료: ${dest}"
done
