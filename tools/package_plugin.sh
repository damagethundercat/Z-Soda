#!/usr/bin/env bash
set -euo pipefail

print_usage() {
  cat <<'EOF'
Usage:
  bash tools/package_plugin.sh --platform <windows|macos> --build-dir <dir> --output-dir <dir> [--include-manifest]

Examples:
  bash tools/package_plugin.sh --platform windows --build-dir build-win --output-dir dist
  bash tools/package_plugin.sh --platform macos --build-dir build-mac --output-dir dist --include-manifest
EOF
}

platform=""
build_dir=""
output_dir=""
include_manifest="0"

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

if [[ "${platform}" == "windows" ]]; then
  cp "${artifact_source}" "${destination}"
else
  cp -R "${artifact_source}" "${destination}"
fi

if [[ "${include_manifest}" == "1" && -f "models/models.manifest" ]]; then
  cp "models/models.manifest" "${output_dir}/models.manifest"
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

echo "Packaged artifact:"
echo "  platform: ${platform}"
echo "  source:   ${artifact_source}"
echo "  output:   ${destination}"
if [[ "${include_manifest}" == "1" ]]; then
  echo "  manifest: ${output_dir}/models.manifest"
fi
