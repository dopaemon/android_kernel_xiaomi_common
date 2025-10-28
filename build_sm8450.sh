#!/bin/bash
#
# Compile script for Xiaomi 8450 kernel, dts and modules with AOSPA
# Copyright (C) 2024 Adithya R.

SECONDS=0 # start builtin bash timer
KP_ROOT="$HOME/Rebase"
SRC_ROOT="$KP_ROOT/sm8450"
TC_DIR="$KP_ROOT/prebuilts/clang/kernel/linux-x86/clang-r416183b"
PREBUILTS_DIR="$KP_ROOT/prebuilts/kernel-build-tools/linux-x86"
MODULES_REPO="sm8450-modules"
DT_REPO="sm8450-devicetrees"
HDR_STAGING="kernel-headers"

DO_CLEAN=false
NO_LTO=false
ONLY_CONFIG=false
TARGET=
DTB_WILDCARD="*"
DTBO_WILDCARD="*"

while [ "${#}" -gt 0 ]; do
    case "${1}" in
        -c | --clean )
                DO_CLEAN=true
                ;;
        -n | --no-lto )
                NO_LTO=true
                ;;
        -o | --only-config )
                ONLY_CONFIG=true
                ;;
        * )
                TARGET="${1}"
                ;;
    esac
    shift
done

if [ -z "$TARGET" ]; then
    echo "Target (device) not specified!"
    exit 1
fi

KERNEL_DIR="$KP_ROOT/$TARGET-kernel"

if [ ! -d "$KERNEL_DIR" ]; then
    echo "$KERNEL_DIR does not exist!"
    mkdir -p $KERNEL_DIR
fi

KERNEL_COPY_TO="$KERNEL_DIR"
DTB_COPY_TO="$KERNEL_DIR/dtbs"
DTBO_COPY_TO="$DTB_COPY_TO/dtbo.img"
VBOOT_DIR="$KERNEL_DIR/vendor_ramdisk"
VDLKM_DIR="$KERNEL_DIR/vendor_dlkm"
HDR_DEST="$KERNEL_DIR/kernel-headers"

if [ ! -d "$DTB_COPY_TO" ]; then
    echo "$DTB_COPY_TO does not exist!"
    mkdir -p $DTB_COPY_TO
fi

if [ ! -d "$VBOOT_DIR" ]; then
    echo "$VBOOT_DIR does not exist!"
    mkdir -p $VBOOT_DIR
fi

if [ ! -d "$VDLKM_DIR" ]; then
    echo "$VDLKM_DIR does not exist!"
    mkdir -p $VDLKM_DIR
fi

if [ ! -d "$HDR_DEST" ]; then
    echo "$HDR_DEST does not exist!"
    mkdir -p $HDR_DEST
fi

DEFCONFIG="gki_defconfig"
DEFCONFIGS="vendor/waipio_GKI.config \
vendor/xiaomi_GKI.config \
vendor/${TARGET}_GKI.config \
vendor/debugfs.config"

MODULES_SRC="../$MODULES_REPO/qcom/opensource"
MODULES="mmrm-driver \
audio-kernel \
camera-kernel \
cvp-kernel \
dataipa/drivers/platform/msm \
datarmnet/core \
datarmnet-ext/aps \
datarmnet-ext/offload \
datarmnet-ext/shs \
datarmnet-ext/perf \
datarmnet-ext/perf_tether \
datarmnet-ext/sch \
datarmnet-ext/wlan \
display-drivers/msm \
eva-kernel \
video-driver \
wlan/qcacld-3.0/.qca6490"

case "$TARGET" in
    "marble" )
        DTB_WILDCARD="ukee"
        DTBO_WILDCARD="marble-sm7475-pm8008-overlay"
        ;;
    "cupid" )
        DTB_WILDCARD="waipio"
        DTBO_WILDCARD="cupid-sm8450-pm8008-overlay"
        ;;
    "mayfly" )
        DTB_WILDCARD="cape"
        DTBO_WILDCARD="mayfly-sm8475-pm8008-overlay"
        ;;
esac

export PATH="$TC_DIR/bin:$PREBUILTS_DIR/bin:$PATH"

function m() {
    make -j$(nproc --all) O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 \
        DTC_EXT="$PREBUILTS_DIR/bin/dtc" \
        DTC_OVERLAY_TEST_EXT="$PREBUILTS_DIR/bin/ufdt_apply_overlay" \
        TARGET_PRODUCT=$TARGET $@ || exit $?
}

$DO_CLEAN && (
    rm -rf out $MODULES_REPO
    echo "Cleaned output directories."
)

mkdir -p out

echo -e "Generating config...\n"
m $DEFCONFIG
m ./scripts/kconfig/merge_config.sh $DEFCONFIGS vendor/${TARGET}_GKI.config
$NO_LTO && (
    scripts/config --file out/.config \
        -d LTO_CLANG_FULL -e LTO_NONE
    echo -e "\nDisabled LTO!"
)

$ONLY_CONFIG && exit

echo -e "\nBuilding kernel...\n"
m Image modules dtbs
rm -rf out/modules out/*.ko
m INSTALL_MOD_PATH=modules INSTALL_MOD_STRIP=1 modules_install headers_install INSTALL_HDR_PATH="$HDR_STAGING"

mkdir -p "$HDR_DEST"

echo -e "\nBuilding techpack modules..."
for module in $MODULES; do
    echo -e "\nBuilding $module..."
    m -C $MODULES_SRC/$module M=$MODULES_SRC/$module KERNEL_SRC="$(pwd)" OUT_DIR="$(pwd)/out"
    m -C $MODULES_SRC/$module M=$MODULES_SRC/$module KERNEL_SRC="$(pwd)" OUT_DIR="$(pwd)/out" \
        INSTALL_MOD_PATH=modules INSTALL_MOD_STRIP=1 modules_install
done

echo -e "\nKernel compiled succesfully!\nMerging dtb's...\n"

rm -rf out/dtbs{,-base}
mkdir out/dtbs{,-base}
mv  out/arch/arm64/boot/dts/vendor/qcom/$DTB_WILDCARD.dtb \
    out/arch/arm64/boot/dts/vendor/qcom/$DTBO_WILDCARD.dtbo \
    out/dtbs-base
rm -f out/arch/arm64/boot/dts/vendor/qcom/*.dtbo
$KP_ROOT/build/android/merge_dtbs.py out/dtbs-base out/arch/arm64/boot/dts/vendor/qcom/ out/dtbs || exit $?

echo -e "\nCopying files...\n"

cp out/arch/arm64/boot/Image $KERNEL_COPY_TO
echo "Copied kernel to $KERNEL_COPY_TO."

if [ -d "$DTB_COPY_TO" ]; then
    rm -f $DTB_COPY_TO/*.dtb
    cp out/dtbs/*.dtb $DTB_COPY_TO/
else
    rm -f $DTB_COPY_TO
    cat out/dtbs/*.dtb >> $DTB_COPY_TO/
fi
echo "Copied dtb(s) to $DTB_COPY_TO."

mkdtboimg.py create $DTBO_COPY_TO --page_size=4096 out/dtbs/*.dtbo
echo "Generated dtbo.img to $DTBO_COPY_TO".

first_stage_modules="$(cat modules.list.msm.waipio)"
second_stage_modules="$(cat modules.list.second_stage modules.list.second_stage.$TARGET)"
vendor_dlkm_modules="$(cat modules.list.vendor_dlkm modules.list.vendor_dlkm.$TARGET)"
modules_out="out/modules/lib/modules/$(ls -t out/modules/lib/modules/ | head -n1)"

rm -rf $VBOOT_DIR && mkdir -p $VBOOT_DIR
rm -rf $VDLKM_DIR && mkdir -p $VDLKM_DIR

echo -e "\nCopying first stage modules..."
for module in $first_stage_modules; do
    mod_path=$(find $modules_out -name "$module" -print -quit)
    if [ -z "$mod_path" ]; then
        echo "Could not locate $module, skipping!"
        continue
    fi
    cp $mod_path $VBOOT_DIR
    echo $module >> $VBOOT_DIR/modules.load
    echo $module >> $VBOOT_DIR/modules.load.recovery
done

echo -e "\nCopying second stage modules..."
for module in $second_stage_modules; do
    mod_path=$(find $modules_out -name "$module" -print -quit)
    if [ -z "$mod_path" ]; then
        echo "Could not locate $module, skipping!"
        continue
    fi
    cp $mod_path $VBOOT_DIR
    cp $mod_path $VDLKM_DIR
    echo $module >> $VBOOT_DIR/modules.load.recovery
    echo $module >> $VDLKM_DIR/modules.load
done

echo -e "\nCopying vendor_dlkm modules..."
for module in $vendor_dlkm_modules; do
    mod_path=$(find $modules_out -name "$module" -print -quit)
    if [ -z "$mod_path" ]; then
        echo "Could not locate $module, skipping!"
        continue
    fi
    cp $mod_path $VDLKM_DIR
    echo $module >> $VDLKM_DIR/modules.load
done

for dest_dir in $VBOOT_DIR $VDLKM_DIR; do
    cp modules.vendor_blocklist.msm.waipio $dest_dir/modules.blocklist
    cp $modules_out/modules.{alias,dep,softdep} $dest_dir
done

sed -E -i 's|([^: ]*/)([^/]*\.ko)([:]?)([ ]\|$)|/lib/modules/\2\3\4|g' $VBOOT_DIR/modules.dep
sed -E -i 's|([^: ]*/)([^/]*\.ko)([:]?)([ ]\|$)|/vendor_dlkm/lib/modules/\2\3\4|g' $VDLKM_DIR/modules.dep

echo -e "\nCopying kernel headers..."
cp -a "out/$HDR_STAGING/include/*" "$HDR_DEST/"

echo -e "\nCompleted in $((SECONDS / 60)) minute(s) and $((SECONDS % 60)) second(s) !"
