#!/usr/bin/env bash
set -euo pipefail

MODEL_ID="${1:-distill-any-depth-base}"
MODEL_ROOT="${ZSODA_MODEL_ROOT:-models}"
MANIFEST_PATH="${ZSODA_MODEL_MANIFEST:-${MODEL_ROOT}/models.manifest}"

case "${MODEL_ID}" in
  distill-any-depth|distill-any-depth-base|distill-any-depth-large)
    ;;
  *)
    echo "unsupported model id: ${MODEL_ID}" >&2
    exit 1
    ;;
esac

mkdir -p "${MODEL_ROOT}"

cat <<EOF
ZSoda production path is DistillAnyDepth via remote service.
Model: ${MODEL_ID}
Model root: ${MODEL_ROOT}
Manifest: ${MANIFEST_PATH}

No local ONNX asset is downloaded by this helper.
The runtime service resolves the Hugging Face model on demand:
  tools/distill_any_depth_remote_service.py
EOF
