#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=${ROOT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
KERNEL_DIR=${KERNEL_DIR:-common}
BUILD_CONFIG=${BUILD_CONFIG:-common/build.config.gki.aarch64}

ensure_host_tool() {
  local tool="$1"
  local src
  local candidate

  src=""
  for candidate in "${tool}" "${tool}-18" "${tool}-17" "${tool}-16" "${tool}-15" "${tool}-14" "${tool}-13" "${tool}-12"; do
    src="$(command -v "${candidate}" || true)"
    if [[ -n "${src}" ]]; then
      break
    fi
  done

  if [[ -z "${src}" ]]; then
    echo "ERROR: required host tool '${tool}' not found"
    exit 127
  fi

  mkdir -p "${ROOT_DIR}/build/build-tools/path/linux-x86" \
           "${ROOT_DIR}/build-tools/path/linux-x86"
  ln -sf "${src}" "${ROOT_DIR}/build/build-tools/path/linux-x86/${tool}"
  ln -sf "${src}" "${ROOT_DIR}/build-tools/path/linux-x86/${tool}"
}

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

# build/build_abi.sh may replace PATH with hermetic tool dirs. Ensure
# essential host tools exist there to avoid exit 127 failures.
for t in \
  find mktemp readlink realpath dirname basename \
  grep sed awk sort uniq xargs \
  nproc rm cp mv ln mkdir rmdir cat echo printf tee cut tr \
  head tail wc test env uname date pwd sh bash \
  make clang ld.lld ar nm objcopy objdump strip \
  python3 perl git rsync; do
  ensure_host_tool "${t}"
done

cd "${ROOT_DIR}"

echo "[kmi-check] ROOT_DIR=${ROOT_DIR}"
echo "[kmi-check] BUILD_CONFIG=${BUILD_CONFIG}"
echo "[kmi-check] KERNEL_DIR=${KERNEL_DIR}"

"${ROOT_DIR}/build/build_abi.sh" --print-report
