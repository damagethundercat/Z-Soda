#!/usr/bin/env bash
set -euo pipefail

jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 2)"

run_cmake_suite() {
  local name="$1"
  shift
  local build_dir="/tmp/zsoda_ci_${name}"

  rm -rf "${build_dir}"
  cmake -S . -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DZSODA_BUILD_TESTS=ON \
    -DZSODA_BUILD_TOOLS=OFF \
    "$@"
  cmake --build "${build_dir}" --parallel "${jobs}"
  ctest --test-dir "${build_dir}" --output-on-failure
}

echo "[1/2] build+run default test suite"
run_cmake_suite default

echo "[2/2] build+run ONNX Runtime scaffold test suite"
run_cmake_suite ort_scaffold -DZSODA_WITH_ONNX_RUNTIME=ON

echo "Local CI checks passed."
