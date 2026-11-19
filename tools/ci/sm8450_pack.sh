#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Turn the Kleaf dist of //vendor/xiaomi/sm8450-build:sm8450_dist into an
# AnyKernel3 payload: boot Image, vendor_boot dtb and ramdisk modules,
# dtbo.img, vendor_dlkm.img and system_dlkm.img.
#
# Module staging mirrors vendor/lineage/build/tasks/kernel.mk
# (build-image-kernel-modules-lineage) for TARGET_KERNEL_PLATFORM_TARGET
# with TARGET_KERNEL_MIXED_MODE, using the module lists of
# device/xiaomi/sm8450-common.  boot and vendor_boot are patched on the
# device by AnyKernel3, so their ramdisks never leave the phone.
#
# Required environment:
#   DIST          Kleaf dist directory (Image, *.ko, *.dtb, *.dtbo, ...)
#   DEVICE_COMMON device/xiaomi/sm8450-common checkout
#   KP            kernel/platform/kernel-6.6 root (prebuilts)
#   OUT           AnyKernel3 staging directory
# Optional:
#   KERNEL_IMAGE  Image for boot (default: $DIST/Image)

set -euo pipefail

: "${DIST:?}" "${DEVICE_COMMON:?}" "${KP:?}" "${OUT:?}"
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
  for p in "$KP/prebuilts/kernel-build-tools/linux-x86/bin/$1" \
           "$KP/prebuilts/build-tools/linux-x86/bin/$1"; do
    [ -x "$p" ] && { echo "$p"; return; }
  done
  command -v "$1" 2>/dev/null && return
  return 1
}

AVBTOOL=$(find_tool avbtool || true)
if [ -z "$AVBTOOL" ]; then
  fetch_googlesource platform/external/avb avbtool.py "$WORK/avbtool.py"
  AVBTOOL="python3 $WORK/avbtool.py"
fi
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
    [ -f "$DIST/$name" ] || { echo "ERROR: $name not in dist" >&2; missing=1; continue; }
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
SYSTEM_LOAD=$DIST/system_dlkm.modules.load
[ -f "$SYSTEM_LOAD" ] || { echo "missing $SYSTEM_LOAD (mixed build)" >&2; exit 1; }
xargs -rn1 basename < "$SYSTEM_LOAD" | sort -u > "$WORK/system.list"
find "$DIST" -maxdepth 1 -type f -name '*.ko' -printf '%f\n' | sort > "$WORK/all.list"
comm -23 "$WORK/all.list" "$WORK/system.list" > "$WORK/vendor.list"
read_list "$DEVICE_COMMON/modules.load.vendor_boot" > "$WORK/vendor_boot.list"
echo "dist: $(wc -l < "$WORK/all.list") modules, system_dlkm: $(wc -l < "$WORK/system.list")," \
     "vendor_dlkm: $(wc -l < "$WORK/vendor.list"), vendor_boot: $(wc -l < "$WORK/vendor_boot.list")"

log "system_dlkm modules"
build_modules "$WORK/system_dlkm" system_dlkm/ "$WORK/depmod_system_dlkm" \
  "$WORK/system.list" "$WORK/system.list"
cp "$DEVICE_COMMON/modules.blocklist.system_dlkm" "$WORK/system_dlkm/lib/modules/modules.blocklist"

log "vendor_boot (dlkm fragment) modules"
build_modules "$WORK/vendor_ramdisk" "" "$WORK/depmod_vendor_ramdisk" \
  "$WORK/vendor_boot.list" "$DEVICE_COMMON/modules.load.vendor_boot"
# The vendor ramdisk blocklist stays wherever the ROM put it; only the
# modules and their depmod files of the dlkm fragment are replaced.

log "vendor_dlkm modules"
build_modules "$WORK/vendor_dlkm" vendor_dlkm/ "$WORK/depmod_vendor" \
  "$WORK/vendor.list" "$DEVICE_COMMON/modules.load" \
  "$WORK/depmod_system_dlkm/lib/modules/0.0/system_dlkm"
cp "$DEVICE_COMMON/modules.blocklist" "$WORK/vendor_dlkm/lib/modules/modules.blocklist"

log "dtb / dtbo"
if [ -f "$DIST/dtb.img" ]; then
  cp "$DIST/dtb.img" "$WORK/dtb.img"
else
  find "$DIST" -maxdepth 1 -type f -name '*.dtb' | sort | xargs cat > "$WORK/dtb.img"
fi
[ -s "$WORK/dtb.img" ] || { echo "no dtb produced" >&2; exit 1; }
if [ -f "$DIST/dtbo.img" ]; then
  cp "$DIST/dtbo.img" "$OUT/dtbo.img"
else
  mapfile -t DTBOS < <(find "$DIST" -maxdepth 1 -type f -name '*.dtbo' | sort)
  [ "${#DTBOS[@]}" -gt 0 ] || { echo "no dtbo produced" >&2; exit 1; }
  printf '  %s\n' "${DTBOS[@]##*/}"
  $MKDTBOIMG create "$OUT/dtbo.img" --page_size=4096 "${DTBOS[@]}"
fi
$AVBTOOL add_hash_footer --partition_name dtbo --partition_size "$DTBO_SIZE" --image "$OUT/dtbo.img"

log "vendor_dlkm.img / system_dlkm.img"
make_ext4 vendor_dlkm u:object_r:vendor_file:s0 u:object_r:vendor_configs_file:s0 "$WORK/vendor_dlkm/lib/modules" "$OUT/vendor_dlkm.img"
make_ext4 system_dlkm u:object_r:system_dlkm_file:s0 u:object_r:system_dlkm_file:s0 "$WORK/system_dlkm/lib/modules" "$OUT/system_dlkm.img"

log "AnyKernel3 payload"
# boot-files / vendor_boot-files are picked up by AK3 setup_ak for the
# matching BLOCK; vendor_boot_modules replaces the vendor ramdisk modules.
mkdir -p "$OUT/boot-files" "$OUT/vendor_boot-files" "$OUT/vendor_boot_modules"
cp "$KERNEL_IMAGE" "$OUT/boot-files/Image"
cp "$WORK/dtb.img" "$OUT/vendor_boot-files/dtb"
cp "$WORK/vendor_ramdisk/lib/modules/"* "$OUT/vendor_boot_modules/"
du -ah "$OUT" | sort -k2 | grep -v '\.ko$'
