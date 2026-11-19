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

# vol_key: wait up to 10 s for a volume key press, print "up" or "down"
# (nothing on timeout)
vol_key() {
  local ev=$AKHOME/vol_key.ev i=0 pid key=;
  : > $ev;
  getevent -lq > $ev 2>/dev/null &
  pid=$!;
  while [ $i -lt 50 ]; do
    if grep -q 'KEY_VOLUMEUP *DOWN' $ev; then key=up; break; fi;
    if grep -q 'KEY_VOLUMEDOWN *DOWN' $ev; then key=down; break; fi;
    sleep 0.2 2>/dev/null || sleep 1;
    i=$((i + 1));
  done;
  kill $pid 2>/dev/null;
  rm -f $ev;
  echo $key;
}

# display refresh rate: the default dtbo.img or one of the overclocked
# variants the CI put in dtbo-oc/ (only built for some devices)
oc_label() {
  case $1 in
    1) echo "Default (60/120 Hz)";;
    2) echo "144 Hz (overclock)";;
    3) echo "All: 144 Hz + 156 Hz (overclock)";;
  esac;
}
if [ -f $AKHOME/dtbo-oc/dtbo-oc144.img -a -f $AKHOME/dtbo-oc/dtbo-ocall.img ]; then
  oc=1;
  ui_print " " "Display refresh rate:";
  if command -v getevent >/dev/null 2>&1; then
    ui_print "  Vol+ = next option, Vol- = select" \
      "  (the shown option is used after 10 s without a key)" \
      "> $(oc_label $oc)";
    while true; do
      case $(vol_key) in
        up) oc=$((oc % 3 + 1)); ui_print "> $(oc_label $oc)";;
        *) break;;
      esac;
    done;
  else
    ui_print "  No key input in this recovery, using the default";
  fi;
  case $oc in
    2) cp -f $AKHOME/dtbo-oc/dtbo-oc144.img $AKHOME/dtbo.img || abort "Selecting dtbo failed. Aborting...";;
    3) cp -f $AKHOME/dtbo-oc/dtbo-ocall.img $AKHOME/dtbo.img || abort "Selecting dtbo failed. Aborting...";;
  esac;
  ui_print "Selected: $(oc_label $oc)";
  [ $oc = 1 ] || ui_print "Note: the panel is tuned for 120 Hz, colours may shift above it";
fi;
rm -rf $AKHOME/dtbo-oc;

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
