#!/usr/bin/env bash
# Fetch the prebuilt ONNX Runtime used by the camera privacy stage into
# third_party/onnxruntime/ (git-ignored). The YOLOv8n model is vendored at
# models/yolov8n.onnx; only the runtime is downloaded here.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEST="${ROOT}/third_party/onnxruntime"
VERSION=${ONNXRUNTIME_VERSION:-1.20.1}

case "$(uname -m)" in
  aarch64|arm64) ARCH=aarch64 ;;
  x86_64|amd64)  ARCH=x64 ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac

if [[ -f "${DEST}/lib/libonnxruntime.so" && -f "${DEST}/include/onnxruntime_cxx_api.h" ]]; then
  echo "onnxruntime already present at ${DEST}"
  exit 0
fi

PKG="onnxruntime-linux-${ARCH}-${VERSION}"
URL="https://github.com/microsoft/onnxruntime/releases/download/v${VERSION}/${PKG}.tgz"
TMP=$(mktemp -d)
trap 'rm -rf "${TMP}"' EXIT

echo "downloading ${URL}"
curl -sSL --fail -o "${TMP}/ort.tgz" "${URL}"
tar xzf "${TMP}/ort.tgz" -C "${TMP}"
mkdir -p "$(dirname "${DEST}")"
rm -rf "${DEST}"
mv "${TMP}/${PKG}" "${DEST}"
echo "installed onnxruntime ${VERSION} (${ARCH}) at ${DEST}"
