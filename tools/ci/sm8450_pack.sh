#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Turn the Kleaf dist of //vendor/xiaomi/sm8450-build:sm8450_dist into an
# AnyKernel3 payload: boot Image, vendor_boot dtb and ramdisk modules,
# dtbo.img (plus the dtbo-oc/ display overclock and dtb-gpu/ GPU
# undervolt/overclock variants), vendor_dlkm.img and system_dlkm.img.
#
# Module staging mirrors vendor/lineage/build/tasks/kernel.mk
# (build-image-kernel-modules-lineage) for TARGET_KERNEL_PLATFORM_TARGET
# using the module lists the DoraCore tree carries in
# vendor/xiaomi/sm8450-common.  The Image and every module come from the
# one standalone waipio_perf build, so they always match each other.
# boot and vendor_boot are patched on the device by AnyKernel3, so their
# ramdisks never leave the phone.
#
# Required environment:
#   DIST          Kleaf dist directory (Image, *.ko, *.dtb, *.dtbo, ...)
#   MODULE_LISTS  directory with modules.load{,.vendor_boot} and modules.blocklist*
#   KP            kernel/platform/kernel-6.6 root (prebuilts)
#   OUT           AnyKernel3 staging directory
#   DEVICE        device the zip is for (picks its overclock variants)
# Optional:
#   KERNEL_IMAGE  Image for boot (default: $DIST/Image)

set -euo pipefail

: "${DIST:?}" "${MODULE_LISTS:?}" "${OUT:?}"
KP=${KP:-}
ALLOW_MISSING_MODULES=${ALLOW_MISSING_MODULES:-0}
KERNEL_IMAGE=${KERNEL_IMAGE:-$DIST/Image}
WORK=$(mktemp -d "${RUNNER_TEMP:-/tmp}/sm8450-pack.XXXXXX")

# From device/xiaomi/sm8450-common/BoardConfigCommon.mk
DTBO_SIZE=$((0x01800000))

mkdir -p "$OUT"

log() { printf '\n===== %s =====\n' "$*"; }

fetch_googlesource() {
  # $1 project, $2 path, $3 dest
  curl -fsSL --retry 5 \
    "https://android.googlesource.com/$1/+/refs/heads/main/$2?format=TEXT" | base64 -d > "$3"
}

find_tool() {
  # $1 name; prints a command usable as "$cmd args"
  local p
  if [ -n "$KP" ]; then
    for p in "$KP/prebuilts/kernel-build-tools/linux-x86/bin/$1" \
             "$KP/prebuilts/build-tools/linux-x86/bin/$1" \
             "$KP/prebuilts/build-tools/bin/$1"; do
      [ -x "$p" ] && { echo "$p"; return; }
    done
  fi
  command -v "$1" 2>/dev/null && return
  return 1
}

AVBTOOL=$(find_tool avbtool || true)
if [ -z "$AVBTOOL" ]; then
  fetch_googlesource platform/external/avb avbtool.py "$WORK/avbtool.py"
  AVBTOOL="python3 $WORK/avbtool.py"
fi
# avbtool add_hashtree_footer runs "fec" from PATH for the FEC data.  Expose
# only the prebuilt fec, so host e2fsprogs/kmod/lz4 stay the ones in use.
FEC=$(find_tool fec || true)
[ -n "$FEC" ] || { echo "fec not found (needed by avbtool add_hashtree_footer)" >&2; exit 1; }
mkdir -p "$WORK/bin"
ln -sf "$FEC" "$WORK/bin/fec"
export PATH="$WORK/bin:$PATH"
MKDTBOIMG=$(find_tool mkdtboimg || find_tool mkdtimg || true)
if [ -z "$MKDTBOIMG" ]; then
  fetch_googlesource platform/system/libufdt utils/src/mkdtboimg.py "$WORK/mkdtboimg.py"
  MKDTBOIMG="python3 $WORK/mkdtboimg.py"
fi

SYSTEM_MAP=$DIST/System.map
[ -f "$SYSTEM_MAP" ] || { echo "missing $SYSTEM_MAP" >&2; exit 1; }
[ -f "$KERNEL_IMAGE" ] || { echo "missing $KERNEL_IMAGE" >&2; exit 1; }

# Read a module list (one .ko basename per line, comments allowed).
read_list() { sed -e 's/#.*//' -e 's/[[:space:]]//g' -e '/^$/d' "$1" | xargs -rn1 basename; }

# build_modules <outdir> <mountpoint/> <staging> <ko-list-file> <load-list-file|""> [ext-dep-dir]
#
# Same steps as build-image-kernel-modules-lineage: copy, depmod against
# System.map with "0.0" as version, rewrite modules.dep to absolute paths
# and write modules.load as bare module names.
build_modules() {
  local out=$1 mnt=$2 stg=$3 kos=$4 load=$5 ext=${6:-}
  local name missing=0

  rm -rf "$out" "$stg"
  mkdir -p "$out/lib/modules" "$stg/lib/modules/0.0/${mnt}lib/modules"
  while read -r name; do
    if [ ! -f "$DIST/$name" ]; then
      if [ "$ALLOW_MISSING_MODULES" = 1 ]; then
        echo "WARN: $name not in dist (skipping)" >&2
        continue
      fi
      echo "ERROR: $name not in dist" >&2; missing=1; continue
    fi
    cp "$DIST/$name" "$out/lib/modules/"
    cp "$DIST/$name" "$stg/lib/modules/0.0/${mnt}lib/modules/"
  done < "$kos"
  [ "$missing" = 0 ] || exit 1
  [ -z "$ext" ] || cp -r "$ext" "$stg/lib/modules/0.0/"

  depmod -ae -F "$SYSTEM_MAP" -b "$stg" 0.0 2> "$stg/depmod_stderr" || true
  cat "$stg/depmod_stderr" >&2
  if grep -q "needs unknown symbol" "$stg/depmod_stderr"; then
    echo "ERROR: kernel module(s) need unknown symbol(s) ($mnt)" >&2
    exit 1
  fi

  sed -e 's/\(.*modules.*\):/\/\1:/g' -e 's/ \([^ ]*modules[^ ]*\)/ \/\1/g' \
    "$stg/lib/modules/0.0/modules.dep" > "$out/lib/modules/modules.dep"
  cp "$stg/lib/modules/0.0/modules.softdep" "$stg/lib/modules/0.0/modules.alias" "$out/lib/modules/"

  : > "$out/lib/modules/modules.load"
  if [ -n "$load" ]; then
    while read -r name; do
      name=$(basename "$name" .ko)
      if [ -f "$out/lib/modules/$name.ko" ]; then
        echo "$name" >> "$out/lib/modules/modules.load"
      elif [ "$ALLOW_MISSING_MODULES" = 1 ]; then
        echo "WARN: $name.ko from the load list was not built (skipping)" >&2
      else
        echo "ERROR: $name.ko from the load list was not built" >&2
        missing=1
      fi
    done < <(read_list "$load")
    [ "$missing" = 0 ] || exit 1
  fi
}

# make_ext4 <partition> <selinux label> <etc label> <lib/modules dir> <out.img>
#
# Build a read-only dlkm image holding lib/modules, labelled like AOSP
# system/sepolicy private/file_contexts does for that mount point.
#
# etc/build.prop identifies the ROM build (fingerprint, date, product), so
# it can only come from the installed ROM: reserve one 4 KiB block for it
# and record its byte offset in <out.img>.prop_offset; anykernel.sh copies
# the device's current build.prop into that block before flashing.
PROP_BLOCK=4096
make_ext4() {
  local part=$1 label=$2 etc_label=$3 mods=$4 img=$5 mnt="$WORK/mnt-$1"
  local size rc blocks bsize pblk

  size=$(du -sb "$mods" | cut -f1)
  size=$(( (size * 12 / 10 + 32 * 1024 * 1024) / 4096 * 4096 ))
  rm -f "$img"
  mkfs.ext4 -q -F -t ext4 -b 4096 -m 0 -L "$part" -E root_owner=0:0 \
    -O ^has_journal,^orphan_file,^metadata_csum_seed,^inline_data "$img" $((size / 4096))

  { echo "# $part build.prop: filled from the installed ROM by AnyKernel3"
    head -c "$PROP_BLOCK" /dev/zero | tr '\0' '\n'
  } | head -c "$PROP_BLOCK" > "$WORK/$part.build.prop"
  [ "$(wc -c < "$WORK/$part.build.prop")" = "$PROP_BLOCK" ]

  mkdir -p "$mnt"
  sudo mount -o loop,rw -t ext4 "$img" "$mnt"
  sudo rm -rf "$mnt/lost+found"
  sudo install -d -m 0755 -o 0 -g 0 "$mnt/etc" "$mnt/lib" "$mnt/lib/modules"
  sudo install -m 0644 -o 0 -g 0 "$WORK/$part.build.prop" "$mnt/etc/build.prop"
  sudo install -m 0644 -o 0 -g 0 "$mods"/* "$mnt/lib/modules/"
  sudo find "$mnt" -exec setfattr -n security.selinux -v "$label" {} +
  sudo find "$mnt/etc" -exec setfattr -n security.selinux -v "$etc_label" {} +
  sync
  sudo umount "$mnt"

  rc=0; e2fsck -fy "$img" > /dev/null || rc=$?
  [ "$rc" -le 1 ] || { echo "e2fsck failed on $img ($rc)" >&2; exit 1; }
  resize2fs -M "$img"
  bsize=$(dumpe2fs -h "$img" 2>/dev/null | awk -F: '/^Block size/ {gsub(/ /,"",$2); print $2}')
  blocks=$(dumpe2fs -h "$img" 2>/dev/null | awk -F: '/^Block count/ {gsub(/ /,"",$2); print $2}')
  truncate -s $((bsize * blocks)) "$img"

  # Physical block of etc/build.prop, after resize2fs may have moved it.
  pblk=$(debugfs -R "bmap /etc/build.prop 0" "$img" 2>/dev/null | tr -dc '0-9')
  [ -n "$pblk" ] && [ "$pblk" -gt 0 ] || { echo "cannot locate etc/build.prop in $img" >&2; exit 1; }
  cmp -n "$PROP_BLOCK" -i "0:$((pblk * bsize))" "$WORK/$part.build.prop" "$img" ||
    { echo "etc/build.prop offset check failed in $img" >&2; exit 1; }
  echo $((pblk * bsize)) > "$img.prop_offset"
  echo "$part: etc/build.prop at byte $((pblk * bsize))"

  $AVBTOOL add_hashtree_footer --partition_name "$part" --hash_algorithm sha256 --image "$img"
}

log "Module lists"
# Standalone build: the GKI module list names the system_dlkm modules.
# (A mixed build ships system_dlkm.modules.load from kernel_aarch64;
#  Linux 5.10 GKI has no system_dlkm partition.)
SYSTEM_LOAD=$DIST/gki_system_dlkm_modules_arm64
[ -f "$SYSTEM_LOAD" ] || SYSTEM_LOAD=$DIST/system_dlkm.modules.load
HAS_SYSTEM_DLKM=0
if [ -f "$SYSTEM_LOAD" ] && [ -f "$MODULE_LISTS/modules.blocklist.system_dlkm" ]; then
  HAS_SYSTEM_DLKM=1
  read_list "$SYSTEM_LOAD" | sort -u > "$WORK/system.list"
else
  : > "$WORK/system.list"
fi
find "$DIST" -maxdepth 1 -type f -name '*.ko' -printf '%f\n' | sort > "$WORK/all.list"
comm -23 "$WORK/all.list" "$WORK/system.list" > "$WORK/vendor.list"
if [ -f "$MODULE_LISTS/modules.boot" ]; then
  read_list "$MODULE_LISTS/modules.boot" | sort -u > "$WORK/vendor_boot.list"
else
  read_list "$MODULE_LISTS/modules.load.vendor_boot" > "$WORK/vendor_boot.list"
fi
echo "dist: $(wc -l < "$WORK/all.list") modules, system_dlkm: $(wc -l < "$WORK/system.list")," \
     "vendor_dlkm: $(wc -l < "$WORK/vendor.list"), vendor_boot: $(wc -l < "$WORK/vendor_boot.list")"

SYSTEM_DEPMOD_EXT=""
if [ "$HAS_SYSTEM_DLKM" = 1 ]; then
  log "system_dlkm modules"
  build_modules "$WORK/system_dlkm" system_dlkm/ "$WORK/depmod_system_dlkm" \
    "$WORK/system.list" "$WORK/system.list"
  cp "$MODULE_LISTS/modules.blocklist.system_dlkm" "$WORK/system_dlkm/lib/modules/modules.blocklist"
  SYSTEM_DEPMOD_EXT="$WORK/depmod_system_dlkm/lib/modules/0.0/system_dlkm"
fi

log "vendor_boot (dlkm fragment) modules"
build_modules "$WORK/vendor_ramdisk" "" "$WORK/depmod_vendor_ramdisk" \
  "$WORK/vendor_boot.list" "$MODULE_LISTS/modules.load.vendor_boot"
# The vendor ramdisk blocklist stays wherever the ROM put it; only the
# modules and their depmod files of the dlkm fragment are replaced.

log "vendor_dlkm modules"
build_modules "$WORK/vendor_dlkm" vendor_dlkm/ "$WORK/depmod_vendor" \
  "$WORK/vendor.list" "$MODULE_LISTS/modules.load" \
  "$SYSTEM_DEPMOD_EXT"
cp "$MODULE_LISTS/modules.blocklist" "$WORK/vendor_dlkm/lib/modules/modules.blocklist"

log "dtb / dtbo"
if [ -f "$DIST/dtb.img" ]; then
  cp "$DIST/dtb.img" "$WORK/dtb.img"
else
  # <base>-gpu-<name>.dtb are optional GPU undervolt/overclock variants of
  # <base>.dtb: the default dtb leaves them out and dtb-gpu/dtb-gpu-<name>
  # swaps them in, for the installer to offer.
  mapfile -t DTBS < <(find "$DIST" -maxdepth 1 -type f -name '*.dtb' ! -name '*-gpu-*.dtb' | sort)
  [ "${#DTBS[@]}" -gt 0 ] || { echo "no dtb produced" >&2; exit 1; }
  printf '  %s\n' "${DTBS[@]##*/}"
  cat "${DTBS[@]}" > "$WORK/dtb.img"

  # Only offer the tables of the SoC this zip's device runs on, named by
  # its board overlay (<device>-sm8450-* waipio, sm8475 cape, sm7475 ukee).
  case $(basename "$(find "$DIST" -maxdepth 1 -name "${DEVICE:-none}-*-overlay.dtbo" | head -1)") in
    *-sm8450-*) DEVICE_SOC=waipio;;
    *-sm8475-*) DEVICE_SOC=cape;;
    *-sm7475-*) DEVICE_SOC=ukee;;
    *) DEVICE_SOC=;;
  esac
  echo "device ${DEVICE:-?}: SoC ${DEVICE_SOC:-unknown}"
  GPU_DTBS=()
  # <soc>-gpu-<name>.dtb, or <soc>-v2-gpu-<name>.dtb for a chip revision
  # (waipio-v2: retail SM8450)
  [ -z "$DEVICE_SOC" ] || mapfile -t GPU_DTBS < <(find "$DIST" -maxdepth 1 -type f \
    \( -name "$DEVICE_SOC-gpu-*.dtb" -o -name "$DEVICE_SOC-v*-gpu-*.dtb" \) | sort)
  for gpu in "${GPU_DTBS[@]}"; do
    variant=${gpu##*-gpu-}; variant=${variant%.dtb}
    base=${gpu%-gpu-*}.dtb
    [ -f "$base" ] || { echo "no $(basename "$base") for $(basename "$gpu")" >&2; exit 1; }
    mkdir -p "$OUT/dtb-gpu"
    echo "dtb-gpu-$variant: $(basename "$gpu") instead of $(basename "$base")"
    cat "${DTBS[@]/#$base/$gpu}" > "$OUT/dtb-gpu/dtb-gpu-$variant"
  done
  # the installer names the GPU and its tables by SoC
  [ "${#GPU_DTBS[@]}" = 0 ] || echo "$DEVICE_SOC" > "$OUT/dtb-gpu/soc"
fi
[ -s "$WORK/dtb.img" ] || { echo "no dtb produced" >&2; exit 1; }
# make_dtbo <out.img> <dtbo>...
make_dtbo() {
  local img=$1; shift
  printf '  %s\n' "${@##*/}"
  $MKDTBOIMG create "$img" --page_size=4096 "$@"
  $AVBTOOL add_hash_footer --partition_name dtbo --partition_size "$DTBO_SIZE" --image "$img"
}

if [ -f "$DIST/dtbo.img" ]; then
  cp "$DIST/dtbo.img" "$OUT/dtbo.img"
  $AVBTOOL add_hash_footer --partition_name dtbo --partition_size "$DTBO_SIZE" --image "$OUT/dtbo.img"
else
  # <board>-overlay-oc<name>.dtbo are optional display overclock variants
  # of <board>-overlay.dtbo: the default dtbo.img leaves them out and
  # dtbo-oc/dtbo-oc<name>.img swaps them in, for the installer to offer.
  mapfile -t DTBOS < <(find "$DIST" -maxdepth 1 -type f -name '*.dtbo' ! -name '*-overlay-oc*.dtbo' | sort)
  [ "${#DTBOS[@]}" -gt 0 ] || { echo "no dtbo produced" >&2; exit 1; }
  echo "dtbo.img:"
  make_dtbo "$OUT/dtbo.img" "${DTBOS[@]}"

  # Only this zip's device gets its variants, so the installer offers
  # them on that device alone.
  OC_DTBOS=()
  [ -z "${DEVICE:-}" ] || mapfile -t OC_DTBOS < <(find "$DIST" -maxdepth 1 -type f -name "$DEVICE-*-overlay-oc*.dtbo" | sort)
  for oc in "${OC_DTBOS[@]}"; do
    variant=${oc##*-overlay-}; variant=${variant%.dtbo}
    base=${oc%-overlay-*}-overlay.dtbo
    [ -f "$base" ] || { echo "no $(basename "$base") for $(basename "$oc")" >&2; exit 1; }
    mkdir -p "$OUT/dtbo-oc"
    echo "dtbo-$variant.img:"
    make_dtbo "$OUT/dtbo-oc/dtbo-$variant.img" "${DTBOS[@]/#$base/$oc}"
  done
  [ "${#OC_DTBOS[@]}" -gt 0 ] || echo "no display overclock variants for ${DEVICE:-?}"
fi

log "vendor_dlkm.img / system_dlkm.img"
make_ext4 vendor_dlkm u:object_r:vendor_file:s0 u:object_r:vendor_configs_file:s0 "$WORK/vendor_dlkm/lib/modules" "$OUT/vendor_dlkm.img"
if [ "$HAS_SYSTEM_DLKM" = 1 ]; then
  make_ext4 system_dlkm u:object_r:system_dlkm_file:s0 u:object_r:system_dlkm_file:s0 "$WORK/system_dlkm/lib/modules" "$OUT/system_dlkm.img"
fi

log "AnyKernel3 payload"
# boot-files / vendor_boot-files are picked up by AK3 setup_ak for the
# matching BLOCK; vendor_boot_modules replaces the vendor ramdisk modules.
mkdir -p "$OUT/boot-files" "$OUT/vendor_boot-files" "$OUT/vendor_boot_modules"
cp "$KERNEL_IMAGE" "$OUT/boot-files/Image"
cp "$WORK/dtb.img" "$OUT/vendor_boot-files/dtb"
cp "$WORK/vendor_ramdisk/lib/modules/"* "$OUT/vendor_boot_modules/"
du -ah "$OUT" | sort -k2 | grep -v '\.ko$'
