#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=${ROOT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
KERNEL_DIR=${KERNEL_DIR:-common}
BUILD_CONFIG=${BUILD_CONFIG:-common/build.config.gki.aarch64}

if [[ ! -x "${ROOT_DIR}/build/build_abi.sh" ]]; then
  echo "ERROR: missing ${ROOT_DIR}/build/build_abi.sh"
  exit 1
fi

if [[ ! -f "${ROOT_DIR}/${BUILD_CONFIG}" ]]; then
  echo "ERROR: missing ${ROOT_DIR}/${BUILD_CONFIG}"
  exit 1
fi

export KERNEL_DIR
export BUILD_CONFIG
export KMI_SYMBOL_LIST_STRICT_MODE=${KMI_SYMBOL_LIST_STRICT_MODE:-1}
export TRIM_NONLISTED_KMI=${TRIM_NONLISTED_KMI:-1}
export LTO=${LTO:-none}
export SKIP_MRPROPER=${SKIP_MRPROPER:-1}

cd "${ROOT_DIR}"

echo "[kmi-check] ROOT_DIR=${ROOT_DIR}"
echo "[kmi-check] BUILD_CONFIG=${BUILD_CONFIG}"
echo "[kmi-check] KERNEL_DIR=${KERNEL_DIR}"

"${ROOT_DIR}/build/build_abi.sh" --print-report
