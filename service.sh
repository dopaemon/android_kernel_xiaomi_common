#!/system/bin/sh

MODDIR="${0%/*}"
PROFILE_FILE="$MODDIR/profile.conf"

read_profile() {
  [ -f "$PROFILE_FILE" ] && cat "$PROFILE_FILE" || echo "SM8450"
}

w() {
  path="$1"
  value="$2"
  [ -e "$path" ] || return 0
  echo "$value" > "$path" 2>/dev/null
}

set_governor() {
  gov="$1"
  for p in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -e "$p/scaling_governor" ] || continue
    grep -qw "$gov" "$p/scaling_available_governors" 2>/dev/null || continue
    w "$p/scaling_governor" "$gov"
  done
}

set_adios() {
  for q in /sys/block/sd*/queue/scheduler /sys/block/dm-*/queue/scheduler; do
    [ -e "$q" ] || continue
    grep -qw "adios" "$q" 2>/dev/null || continue
    w "$q" "adios"
  done
}

zram_size_bytes() {
  mem_kb="$(awk '/MemTotal:/ {print $2}' /proc/meminfo 2>/dev/null)"
  [ -n "$mem_kb" ] || mem_kb=0

  case "$(read_profile | tr -d '\r')" in
    SM8475) pct=75 ;;
    *) pct=65 ;;
  esac

  echo $((mem_kb * 1024 * pct / 100))
}

set_zram() {
  [ -e /sys/block/zram0/comp_algorithm ] || return 0

  size="$(zram_size_bytes)"
  prio="$(awk '$1=="/dev/block/zram0"{print $5}' /proc/swaps 2>/dev/null)"
  [ -n "$prio" ] || prio=32758

  swapoff /dev/block/zram0 2>/dev/null || true
  w /sys/block/zram0/reset 1
  w /sys/block/zram0/comp_algorithm zstd
  [ "$size" -gt 0 ] && w /sys/block/zram0/disksize "$size"
  mkswap /dev/block/zram0 >/dev/null 2>&1 || return 0
  swapon /dev/block/zram0 -p "$prio" 2>/dev/null || true
}

set_walt_common() {
  w /proc/sys/kernel/sched_util_clamp_min 224
  w /proc/sys/kernel/sched_util_clamp_max 1024
  w /proc/sys/kernel/sched_util_clamp_min_rt_default 0

  w /proc/sys/kernel/sched_min_task_util_for_boost 16
  w /proc/sys/kernel/sched_min_task_util_for_uclamp 16
  w /proc/sys/kernel/sched_min_task_util_for_colocation 16
  w /proc/sys/kernel/sched_asymcap_boost 1
  w /proc/sys/kernel/sched_asymcap_booster 1
  w /proc/sys/kernel/walt_low_latency_task_threshold 1024
  w /proc/sys/kernel/walt_rtg_cfs_boost_prio 110
  w /proc/sys/kernel/sched_coloc_downmigrate_ns 100000000
  w /proc/sys/kernel/sched_group_upmigrate 100
  w /proc/sys/kernel/sched_group_downmigrate 95
  w /proc/sys/kernel/sched_wake_up_idle "1 1"
  w /proc/sys/kernel/sched_conservative_pl 0
  w /proc/sys/kernel/sched_sync_hint_enable 1

  w /proc/sys/kernel/sched_upmigrate "88 88"
  w /proc/sys/kernel/sched_downmigrate "75 75"

  w /proc/sys/kernel/sched_schedstats 0
}

set_input_boost() {
  freqs="1209600 1209600 1209600 1209600 1574400 1574400 1574400 1766400"
  w /proc/sys/kernel/input_boost_ms 120
  w /proc/sys/kernel/input_boost_freq "$freqs"
  w /proc/sys/kernel/sched_boost_on_input 3
  w /proc/sys/kernel/powerkey_input_boost_ms 500
  w /proc/sys/kernel/powerkey_input_boost_freq "$freqs"
  w /proc/sys/kernel/powerkey_sched_boost_on_input 3
}

set_vm() {
  w /proc/sys/vm/swappiness 120
  w /proc/sys/vm/page-cluster 0
  w /proc/sys/vm/watermark_scale_factor 125
  w /proc/sys/vm/watermark_boost_factor 12000
  w /proc/sys/vm/vfs_cache_pressure 70
  w /proc/sys/vm/compaction_proactiveness 20
  w /sys/kernel/mm/lru_gen/enabled 0x0003
  w /sys/kernel/mm/lru_gen/min_ttl_ms 1000
  w /sys/kernel/mm/transparent_hugepage/enabled madvise
  w /sys/kernel/mm/transparent_hugepage/defrag defer+madvise
}

apply_profile() {
  set_walt_common
  set_input_boost
  set_vm
  set_zram
  set_governor schedutil
  set_governor reflex
  set_governor walt
  set_adios
}

while [ "$(getprop sys.boot_completed)" != "1" ]; do
  sleep 2
done

sleep 20

i=0
while [ "$i" -lt 12 ]; do
  apply_profile
  i=$((i + 1))
  sleep 10
done
