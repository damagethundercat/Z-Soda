#!/usr/bin/env bash
set -euo pipefail

MODEL_ID="${1:-depth-anything-v3-small}"
MODEL_ROOT="${ZSODA_MODEL_ROOT:-models}"

url=""
relative_path=""

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
    echo "지원 목록: depth-anything-v3-small, depth-anything-v3-base, depth-anything-v3-large, midas-dpt-large" >&2
    exit 1
    ;;
esac

dest="${MODEL_ROOT}/${relative_path}"
mkdir -p "$(dirname "${dest}")"

echo "다운로드 시작: ${MODEL_ID}"
echo "URL: ${url}"
echo "저장 경로: ${dest}"
curl -fL "${url}" -o "${dest}"
echo "완료: ${dest}"
