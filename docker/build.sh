#!/usr/bin/env bash
# Local build of the DoraCore GKI 6.6 kernel.
# Mirrors the "Build Selected Variants" step of .github/workflows/GKI-6.6.yml,
# minus everything that only makes sense on a GitHub runner (release upload,
# Telegram, swap tuning). Settings come from docker-compose.yml.
set -euo pipefail

# ---- settings (defaults match the workflow) ---------------------------------
BUILD_TARGET="${BUILD_TARGET:-KSU}"          # KSU | NonKSU | Both
LTO_MODE="${LTO_MODE:-Thin}"                 # Thin | Full | None
DEVICE="${DEVICE:-gki}"                      # only used in the zip name
TOOLCHAIN="${TOOLCHAIN:-aosp}"               # aosp | system
CLANG_BRANCH="${CLANG_BRANCH:-main-kernel-build-2024}"
CLANG_FALLBACK_VERSION="${CLANG_FALLBACK_VERSION:-r510928}"
BUILD_JOBS="${BUILD_JOBS:-}"                 # empty = all CPUs
USE_CCACHE="${USE_CCACHE:-1}"
CCACHE_SIZE="${CCACHE_SIZE:-5G}"
CLEAN_BUILD="${CLEAN_BUILD:-0}"              # 1 = wipe the out dir first
STRIP_PROTECTED_EXPORTS="${STRIP_PROTECTED_EXPORTS:-1}"
EXTRA_CONFIG="${EXTRA_CONFIG:-}"             # extra scripts/config arguments
MAKE_TARGET="${MAKE_TARGET:-Image}"
PACK_ZIP="${PACK_ZIP:-1}"
ANYKERNEL_GIT="${ANYKERNEL_GIT:-https://github.com/dopaemon/Anykernel3.git}"

# ---- paths inside the container ---------------------------------------------
KERNEL_SRC="${KERNEL_SRC:-/work/common}"
OUT_ROOT="${OUT_ROOT:-/work/out}"
CACHE_ROOT="${CACHE_ROOT:-/work/cache}"
DIST_DIR="${DIST_DIR:-/work/dist}"

log()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# ---- sanity checks ----------------------------------------------------------
[ -f "$KERNEL_SRC/Makefile" ] && [ -d "$KERNEL_SRC/arch/arm64" ] || die \
  "no kernel source at $KERNEL_SRC. Clone it on the host first and point KERNEL_DIR in docker-compose.yml (or .env) at it."

if [ "$(git -C "$KERNEL_SRC" config --get core.ignorecase 2>/dev/null || true)" = "true" ]; then
  die "the kernel was cloned on a case-insensitive filesystem (macOS default).
       The Linux tree has files that differ only by case, so that checkout is broken.
       Create a case-sensitive APFS volume and clone the kernel there, e.g.:
         hdiutil create -type SPARSEBUNDLE -fs 'Case-sensitive APFS' -size 60g -volname kernel ~/kernel.sparsebundle
         hdiutil attach ~/kernel.sparsebundle      # mounted at /Volumes/kernel"
fi

case "$BUILD_TARGET" in
  KSU) VARIANTS="KSU" ;;
  NonKSU) VARIANTS="NonKSU" ;;
  Both) VARIANTS="KSU NonKSU" ;;
  *) die "unsupported BUILD_TARGET: $BUILD_TARGET (KSU | NonKSU | Both)" ;;
esac

case "$LTO_MODE" in
  Thin) LTO_ARGS="-d LTO_NONE -d LTO_CLANG_FULL -e LTO_CLANG_THIN" ;;
  Full) LTO_ARGS="-d LTO_NONE -d LTO_CLANG_THIN -e LTO_CLANG_FULL" ;;
  None) LTO_ARGS="-d LTO_CLANG_THIN -d LTO_CLANG_FULL -e LTO_NONE" ;;
  *) die "unsupported LTO_MODE: $LTO_MODE (Thin | Full | None)" ;;
esac

[ -n "$BUILD_JOBS" ] || BUILD_JOBS="$(nproc --all)"
BUILD_DATE="$(date +'%Y-%m%d-%H%M')"
mkdir -p "$OUT_ROOT" "$CACHE_ROOT" "$DIST_DIR"

# ---- toolchain --------------------------------------------------------------
setup_toolchain() {
  case "$TOOLCHAIN" in
    aosp)
      [ "$(uname -m)" = "x86_64" ] || die \
        "TOOLCHAIN=aosp needs an x86_64 container (AOSP clang has no arm64 host build).
       Use DOCKER_PLATFORM=linux/amd64, or DOCKER_PLATFORM=linux/arm64 with TOOLCHAIN=system."
      local version clang_dir tmp
      version="$(sed -n 's/^CLANG_VERSION=//p' "$KERNEL_SRC/build.config.constants" 2>/dev/null | head -n 1)"
      version="${version:-$CLANG_FALLBACK_VERSION}"
      clang_dir="$CACHE_ROOT/clang/clang-$version"
      if [ ! -x "$clang_dir/bin/clang" ]; then
        log "Downloading AOSP clang-$version ($CLANG_BRANCH), one time only"
        tmp="$CACHE_ROOT/clang/.download"
        rm -rf "$tmp" "$clang_dir"
        mkdir -p "$CACHE_ROOT/clang"
        # Only the pinned toolchain directory, not the whole prebuilts branch.
        git clone -b "$CLANG_BRANCH" --single-branch --depth=1 --no-tags \
                  --filter=blob:none --sparse \
                  https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86 "$tmp"
        git -C "$tmp" sparse-checkout set "clang-$version"
        [ -x "$tmp/clang-$version/bin/clang" ] || die "clang-$version not found in $CLANG_BRANCH"
        mv "$tmp/clang-$version" "$clang_dir"
        rm -rf "$tmp"
      fi
      export PATH="$clang_dir/bin:$PATH"
      ;;
    system)
      # Distro clang from the image.
      ;;
    *) die "unsupported TOOLCHAIN: $TOOLCHAIN (aosp | system)" ;;
  esac
  command -v clang >/dev/null && command -v ld.lld >/dev/null || die "clang/ld.lld not found"
}

# ---- protected exports ------------------------------------------------------
# This build cannot sign modules with the GKI key, so the stock system_dlkm
# modules would be refused the protected exports. The workflow deletes the
# list from its throwaway clone; here the tree is yours, so it is put back
# when the build ends.
PROTECTED_RESTORE=()
strip_protected_exports() {
  [ "$STRIP_PROTECTED_EXPORTS" = "1" ] || return 0
  local f rel
  for f in "$KERNEL_SRC"/android/abi_gki_protected_exports_*; do
    [ -f "$f" ] || continue
    rel="${f#"$KERNEL_SRC"/}"
    if git -C "$KERNEL_SRC" ls-files --error-unmatch "$rel" >/dev/null 2>&1 \
       && git -C "$KERNEL_SRC" diff --quiet HEAD -- "$rel"; then
      PROTECTED_RESTORE+=("$rel")
      rm -f "$f"
    else
      echo "WARNING: $rel is untracked or modified, leaving it in place" >&2
    fi
  done
}
restore_protected_exports() {
  [ "${#PROTECTED_RESTORE[@]}" -gt 0 ] || return 0
  git -C "$KERNEL_SRC" checkout -q HEAD -- "${PROTECTED_RESTORE[@]}" \
    || echo "WARNING: could not restore ${PROTECTED_RESTORE[*]}; run: git checkout -- android/" >&2
  PROTECTED_RESTORE=()
}

MAKE_PID=""
cleanup() {
  [ -z "$MAKE_PID" ] || kill "$MAKE_PID" 2>/dev/null || true
  restore_protected_exports
}
trap cleanup EXIT
trap 'echo; echo "Interrupted."; exit 130' INT TERM

# Run in the background and wait, so a stop signal is handled right away
# instead of after make returns.
run() {
  "$@" &
  MAKE_PID=$!
  wait "$MAKE_PID"
  MAKE_PID=""
}

# ---- AnyKernel3 -------------------------------------------------------------
prepare_anykernel() {
  local branch="$1" dest="$2" cache="$CACHE_ROOT/anykernel/$1"
  if [ ! -f "$cache/anykernel.sh" ]; then
    rm -rf "$cache"
    mkdir -p "$(dirname "$cache")"
    git clone -b "$branch" --depth=1 --single-branch "$ANYKERNEL_GIT" "$cache"
  fi
  rm -rf "$dest"
  mkdir -p "$dest"
  tar -C "$cache" --exclude=.git -cf - . | tar -C "$dest" -xf -
  # The AnyKernel3 template is shared with the 5.10 builds.
  sed -i 's/GKI 5\.10/GKI 6.6/g' "$dest/anykernel.sh"
  [ ! -f "$dest/banner" ] || sed -i 's/5\.10\.y/6.6.y/g' "$dest/banner"
}

# ---- build ------------------------------------------------------------------
build_variant() {
  local variant="$1" ksu_switch ksu_expect image_name
  local out_dir="$OUT_ROOT/$variant"

  case "$variant" in
    KSU)    ksu_switch="-e"; ksu_expect="CONFIG_KSU=y";            image_name=Image-kernelsu ;;
    NonKSU) ksu_switch="-d"; ksu_expect="# CONFIG_KSU is not set"; image_name=Image-standard ;;
  esac

  log "Variant $variant (LTO $LTO_MODE, -j$BUILD_JOBS, out: $out_dir)"
  [ "$CLEAN_BUILD" != "1" ] || rm -rf "$out_dir"
  mkdir -p "$out_dir"

  local make_args=(
    -C "$KERNEL_SRC"
    -j"$BUILD_JOBS"
    LLVM=1
    LLVM_IAS=1
    ARCH=arm64
    CROSS_COMPILE=aarch64-linux-gnu-
    LD=ld.lld
    HOSTLD=ld.lld
    # Set (even empty) so setlocalversion does not append "+" or "-dirty".
    LOCALVERSION=
    O="$out_dir"
  )
  if [ "$USE_CCACHE" = "1" ]; then
    make_args+=("CC=ccache clang")
  else
    make_args+=("CC=clang")
  fi

  run make "${make_args[@]}" gki_defconfig
  # UAPI_HEADER_TEST turns itself on whenever the compiler can link against
  # an arm64 libc (native arm64 container, or a cross libc on the host) and
  # then fails on the Xiaomi media headers. It is off in CI, where clang
  # cannot link, and it has no effect on the kernel image.
  # shellcheck disable=SC2086
  "$KERNEL_SRC/scripts/config" --file "$out_dir/.config" \
    "$ksu_switch" KSU $LTO_ARGS -d UAPI_HEADER_TEST $EXTRA_CONFIG
  run make "${make_args[@]}" olddefconfig

  grep -qxF "$ksu_expect" "$out_dir/.config" \
    || die "variant $variant: expected '$ksu_expect' in .config"
  grep -E '^(# )?CONFIG_(KSU|KSU_SUSFS|LTO_NONE|LTO_CLANG_THIN|LTO_CLANG_FULL|LOCALVERSION)[= ]' "$out_dir/.config" || true

  # shellcheck disable=SC2086
  run make "${make_args[@]}" $MAKE_TARGET

  local image="$out_dir/arch/arm64/boot/Image"
  [ -f "$image" ] || { echo "No Image produced (MAKE_TARGET=$MAKE_TARGET), skipping packaging."; return 0; }
  echo "Kernel Release: $(cat "$out_dir/include/config/kernel.release")"

  cp "$image" "$DIST_DIR/Image-$variant"
  [ "$PACK_ZIP" = "1" ] || return 0

  if [ "$BUILD_TARGET" = "Both" ]; then
    cp "$image" "$AK_DIR/$image_name"
  else
    local zip_name="DoraCore-${DEVICE}-${variant}-6.6-A15-${BUILD_DATE}.zip"
    cp "$image" "$AK_DIR/Image"
    ( cd "$AK_DIR" && zip -qr9 "$DIST_DIR/$zip_name" . )
    echo "Package: dist/$zip_name"
  fi
}

setup_toolchain

log "Environment"
echo "Kernel:    $(make -s -C "$KERNEL_SRC" kernelversion) @ $(git -C "$KERNEL_SRC" rev-parse --short=12 HEAD 2>/dev/null || echo 'no git')"
echo "Clang:     $(clang --version | head -n 1)"
echo "LLD:       $(ld.lld --version | head -n 1)"
echo "Pahole:    $(pahole --version)"
echo "Host arch: $(uname -m), CPUs: $(nproc --all)"

if [ "$USE_CCACHE" = "1" ]; then
  # One cache per toolchain so they never serve each other's objects.
  export CCACHE_DIR="$CACHE_ROOT/ccache/$TOOLCHAIN"
  export CCACHE_BASEDIR="$KERNEL_SRC"
  export CCACHE_NOHASHDIR="true"
  export CCACHE_COMPILERCHECK="%compiler% --version"
  mkdir -p "$CCACHE_DIR"
  ccache --set-config=max_size="$CCACHE_SIZE"
  ccache --set-config=sloppiness=time_macros,include_file_ctime,include_file_mtime,file_stat_matches
  ccache -z >/dev/null
fi

AK_DIR=""
if [ "$PACK_ZIP" = "1" ]; then
  AK_DIR="$(mktemp -d)/AnyKernel3"
  if [ "$BUILD_TARGET" = "Both" ]; then
    prepare_anykernel volume "$AK_DIR"
  else
    prepare_anykernel master "$AK_DIR"
  fi
fi

strip_protected_exports

for variant in $VARIANTS; do
  build_variant "$variant"
done

if [ "$PACK_ZIP" = "1" ] && [ "$BUILD_TARGET" = "Both" ] && [ -f "$AK_DIR/Image-kernelsu" ]; then
  zip_name="DoraCore-${DEVICE}-6.6-A15-${BUILD_DATE}.zip"
  ( cd "$AK_DIR" && zip -qr9 "$DIST_DIR/$zip_name" . )
  echo "Package: dist/$zip_name"
fi

[ "$USE_CCACHE" != "1" ] || { log "ccache"; ccache -s; }
log "Done. Output is in the dist folder."
ls -lh "$DIST_DIR"
