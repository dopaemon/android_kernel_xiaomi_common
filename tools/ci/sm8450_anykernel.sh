### AnyKernel3 Ramdisk Mod Script
## osm0sis @ xda-developers
## DoraCore SM8450 6.6: boot + vendor_boot (hdr v4) + dtbo + vendor_dlkm + system_dlkm

### AnyKernel setup
# global properties
properties() { '
kernel.string=@KERNEL_STRING@
do.devicecheck=1
do.modules=0
do.systemless=1
do.cleanup=1
do.cleanuponabort=0
device.name1=@DEVICE@
device.name2=
device.name3=
device.name4=
device.name5=
supported.versions=
supported.patchlevels=
supported.vendorpatchlevels=
'; } # end properties


### AnyKernel install
## boot shell variables
BLOCK=boot;
IS_SLOT_DEVICE=1;
RAMDISK_COMPRESSION=auto;
PATCH_VBMETA_FLAG=auto;

# import functions/variables and setup patching - see for reference (DO NOT REMOVE)
. tools/ak3-core.sh;

# recovery only: this zip remaps vendor_dlkm/system_dlkm in super and
# replaces every kernel module, which must not happen under a running
# Android (Kernel Flasher, KernelSU/Magisk manager, FKM, ...).
# update-binary does not export BOOTMODE, so look for Android ourselves.
for proc in zygote zygote64 system_server; do
  if pidof $proc >/dev/null 2>&1 \
    || ps 2>/dev/null | grep -v grep | grep -qw $proc \
    || ps -A 2>/dev/null | grep -v grep | grep -qw $proc; then
    abort "DoraCore must be flashed from recovery (OrangeFox/TWRP)." \
      "Flashing from Android apps is not supported. Aborting...";
  fi;
done;

# vol_key: wait for a volume key press and print "up" or "down"; prints
# "none" only if getevent stops (no input device to read)
vol_key() {
  local ev=$AKHOME/vol_key.ev pid key=;
  : > $ev;
  getevent -lq > $ev 2>/dev/null &
  pid=$!;
  while [ ! "$key" ]; do
    sleep 0.2 2>/dev/null || sleep 1;
    if grep -q 'KEY_VOLUMEUP *DOWN' $ev; then
      key=up;
    elif grep -q 'KEY_VOLUMEDOWN *DOWN' $ev; then
      key=down;
    elif ! kill -0 $pid 2>/dev/null; then
      key=none;
    fi;
  done;
  kill $pid 2>/dev/null;
  rm -f $ev;
  echo $key;
}

# menu <title> <option>...: Vol+ steps through the options, Vol- picks the
# shown one. Sets $choice (1 = first option, also without key input) and
# $key ("none" when there is no key input).
menu() {
  local title=$1 n label;
  shift;
  n=$#;
  choice=1;
  eval "label=\"\$$choice\"";
  ui_print " " "$title" "  Vol+ = next option, Vol- = select" "> $label";
  while true; do
    key=$(vol_key);
    case $key in
      up) choice=$((choice % n + 1));
        eval "label=\"\$$choice\"";
        ui_print "> $label";;
      none) choice=1;
        ui_print "  No key input, using the default";
        break;;
      *) break;;
    esac;
  done;
}

# confirm <warning line>...: show the warning and disclaimer, return 0 when
# Vol- accepts it; on Vol+ or without key input return 1
confirm() {
  ui_print " " "$@" \
    "You use it entirely at your own risk: DoraCore" \
    "and its developers take NO responsibility for" \
    "any damage, data loss or voided warranty." \
    " " \
    "  Vol- = I accept the risk, use it" \
    "  Vol+ = go back to the menu";
  key=$(vol_key);
  case $key in
    down) ui_print " " "Accepted.";
      return 0;;
    up) ui_print " " "Back to the menu.";;
  esac;
  return 1;
}

# pick <title> <warning function> <option>...: menu, then confirm every
# option but the first (default) one; sets $choice
pick() {
  local title=$1 warn=$2;
  shift 2;
  while true; do
    menu "$title" "$@";
    [ $choice = 1 ] && return;
    eval "$warn" && return;
    [ "$key" = none ] && { choice=1; return; };
  done;
}

has_keys() {
  command -v getevent >/dev/null 2>&1 && return 0;
  ui_print " " "No key input in this recovery, keeping the default $1";
  return 1;
}

# display refresh rate: the default dtbo.img or one of the overclocked
# variants the CI put in dtbo-oc/ (only built for some devices)
oc_warning() {
  case $choice in
    2) label="146 Hz";;
    *) label="146 Hz and 155 Hz";;
  esac;
  confirm "!!!!!!!!!! WARNING: DISPLAY OVERCLOCK !!!!!!!!!!" \
    "$label drives the panel beyond its rated" \
    "120 Hz with a higher DSI clock (998.4 MHz at" \
    "146 Hz, 1331.2 MHz at 155 Hz). It can cause" \
    "colour shift, flicker, artifacts, more heat and" \
    "battery drain or a black screen, and lasting" \
    "damage to the panel cannot be ruled out." \
    "If the screen stays black, flash this zip again" \
    "from recovery and choose Default.";
}
if [ -f $AKHOME/dtbo-oc/dtbo-oc144.img -a -f $AKHOME/dtbo-oc/dtbo-ocall.img ] && has_keys "refresh rate"; then
  pick "Display refresh rate:" oc_warning \
    "Default (60/120 Hz)" \
    "146 Hz (overclock)" \
    "All: 146 Hz + 155 Hz (overclock)";
  case $choice in
    2) cp -f $AKHOME/dtbo-oc/dtbo-oc144.img $AKHOME/dtbo.img || abort "Selecting dtbo failed. Aborting...";
      ui_print "Display refresh rate: 146 Hz (overclock)";;
    3) cp -f $AKHOME/dtbo-oc/dtbo-ocall.img $AKHOME/dtbo.img || abort "Selecting dtbo failed. Aborting...";
      ui_print "Display refresh rate: 146 Hz + 155 Hz (overclock)";;
    *) ui_print "Display refresh rate: Default (60/120 Hz)";;
  esac;
fi;
rm -rf $AKHOME/dtbo-oc;

# GPU: the default dtb or one of the undervolt/overclock variants the CI
# put in dtb-gpu/ (only built for some SoCs; dtb-gpu/soc names the SoC)
gpu_soc=$(cat $AKHOME/dtb-gpu/soc 2>/dev/null);
case $gpu_soc in
  waipio) gpu_name="Adreno 730"; gpu_stock=818;;
  cape) gpu_name="Adreno 730"; gpu_stock=900;;
  ukee) gpu_name="Adreno 725"; gpu_stock=580;;
  *) gpu_name="GPU"; gpu_stock=;;
esac;
gpu_label() {
  case $gpu_soc-$1 in
    *-uv) echo "Undervolt (stock $gpu_stock MHz, lower voltage)";;
    cape-oc) echo "Overclock (912 MHz max)";;
    cape-uvoc) echo "Undervolt + overclock (912 MHz max)";;
    waipio-uvoc) echo "Undervolt + overclock (831 MHz max)";;
    ukee-uvoc) echo "Balance: undervolt + overclock (591 MHz max)";;
    ukee-perf) echo "Performance: UV + OC, higher DDR (591 MHz max)";;
    ukee-save) echo "Battery save: UV + underclock (547 MHz max)";;
    *) echo "$1";;
  esac;
}
gpus=;
for v in uv oc uvoc perf save; do
  [ -f $AKHOME/dtb-gpu/dtb-gpu-$v ] && gpus="${gpus:+$gpus }$v";
done;
# gpu_at <menu choice>: the variant shown at that position (2 = first one)
gpu_at() {
  local n=$(($1 - 1));
  set -- $gpus;
  eval echo \$$n;
}
gpu_warning() {
  label=$(gpu_label $(gpu_at $choice));
  confirm "!!!!! WARNING: GPU UNDERVOLT / OVERCLOCK !!!!!" \
    "\"$label\"" \
    "runs the $gpu_name outside Qualcomm's stock" \
    "voltage/frequency table." \
    "Undervolting can cause graphics glitches, app" \
    "crashes, GPU hangs, reboots or a bootloop;" \
    "overclocking adds heat, battery drain and wear." \
    "Every chip is different: a table that is stable" \
    "on one phone may fail on another." \
    "If the phone crashes or bootloops, flash this" \
    "zip again from recovery and choose Default.";
}
gpu_pick() {
  local v;
  set -- "Default (stock, $gpu_stock MHz)";
  for v in $gpus; do
    set -- "$@" "$(gpu_label $v)";
  done;
  pick "GPU ($gpu_name):" gpu_warning "$@";
}
if [ "$gpus" ] && has_keys "GPU table"; then
  gpu_pick;
  gpu=; label=Default;
  if [ $choice -gt 1 ]; then
    gpu=$(gpu_at $choice);
    label=$(gpu_label $gpu);
    cp -f $AKHOME/dtb-gpu/dtb-gpu-$gpu $AKHOME/vendor_boot-files/dtb || abort "Selecting dtb failed. Aborting...";
  fi;
  ui_print "GPU: $label";
fi;
rm -rf $AKHOME/dtb-gpu;

# boot install: only the kernel (boot-files/Image) changes, the generic
# ramdisk (and Magisk/KernelSU in it) is kept as is
split_boot;
flash_boot;
## end boot install


# vendor_boot shell variables
BLOCK=vendor_boot;
IS_SLOT_DEVICE=1;
RAMDISK_COMPRESSION=auto;
PATCH_VBMETA_FLAG=auto;

# reset for vendor_boot patching
reset_ak;

# vendor_boot install: new dtb (vendor_boot-files/dtb) and new first stage
# modules in the vendor ramdisk that carries lib/modules ("dlkm" on LineageOS)
dump_boot;

dlkm_dir=;
for i in $VENDORRD/*; do
  if ls $i/lib/modules/*.ko >/dev/null 2>&1; then
    [ "$dlkm_dir" ] && abort "Kernel modules found in several vendor ramdisks. Aborting...";
    dlkm_dir=$i/lib/modules;
  fi;
done;
[ "$dlkm_dir" ] || abort "No vendor ramdisk with kernel modules found. Aborting...";
ui_print " " "Replacing vendor ramdisk modules in $(basename $(dirname $(dirname $dlkm_dir)))...";
rm -f $dlkm_dir/*.ko $dlkm_dir/modules.dep $dlkm_dir/modules.alias $dlkm_dir/modules.softdep $dlkm_dir/modules.load;
cp -f $AKHOME/vendor_boot_modules/* $dlkm_dir/;
set_perm_recursive 0 0 755 644 $dlkm_dir;

# etc/build.prop of vendor_dlkm/system_dlkm identifies the installed ROM
# build, so copy the device's current one into the 4 KiB block the CI
# reserved for it (byte offset in <part>.img.prop_offset)
fill_dlkm_prop() {
  local part=$1 img=$AKHOME/$1.img off src mnt blk size;

  [ -f $img -a -f $img.prop_offset ] || return 0;
  off=$(cat $img.prop_offset);
  src=/$part/etc/build.prop;
  if [ ! -f $src ]; then
    mnt=$AKHOME/mnt_$part;
    mkdir -p $mnt;
    for blk in /dev/block/mapper/$part$SLOT /dev/block/mapper/$part; do
      [ -e $blk ] && mount -o ro -t ext4 $blk $mnt 2>/dev/null && break;
    done;
    src=$mnt/etc/build.prop;
  fi;
  if [ -f $src ]; then
    size=$(wc -c < $src);
    if [ "$size" -le 4096 ]; then
      (cat $src; head -c $((4096 - size)) /dev/zero | tr '\0' '\n') > $AKHOME/$part.build.prop;
      dd if=$AKHOME/$part.build.prop of=$img bs=4096 seek=$((off / 4096)) count=1 conv=notrunc 2>/dev/null \
        || abort "Writing $part build.prop failed. Aborting...";
      ui_print " " "Copied $part/etc/build.prop from the installed ROM";
    else
      ui_print " " "Warning: $part/etc/build.prop is larger than 4 KiB, left empty";
    fi;
  else
    ui_print " " "Warning: no $part/etc/build.prop found on the device, left empty";
  fi;
  [ "$mnt" ] && { umount $mnt 2>/dev/null; rmdir $mnt 2>/dev/null; };
  return 0;
}
fill_dlkm_prop vendor_dlkm;
fill_dlkm_prop system_dlkm;

# repack, flash vendor_boot, then vendor_dlkm, system_dlkm and dtbo from the zip root
write_boot;
## end vendor_boot install
