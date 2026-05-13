#!/sbin/sh

SKIPUNZIP=0
REPLACE=""

ui_print "-----------------------------"
ui_print "      DoraCore Tuner         "
ui_print "-----------------------------"
ui_print " Auto-detect profile mode    "
ui_print "-----------------------------"

auto_detect_profile() {
  SOC_RAW="$(getprop ro.soc.model) $(getprop ro.board.platform) $(getprop ro.hardware)"
  case "$SOC_RAW" in
    *SM8475*|*sm8475*|*kalama*|*Kalama*) echo "SM8475" ;;
    *SM8450*|*sm8450*|*taro*|*Taro*) echo "SM8450" ;;
    *) echo "SM8450" ;;
  esac
}

PROFILE="$(auto_detect_profile | tail -n 1 | tr -d '\r')"
case "$PROFILE" in
  SM8450|SM8475) ;;
  *) PROFILE="SM8450" ;;
esac

ui_print "[+] Detected profile: $PROFILE"

mkdir -p "$MODPATH"
printf '%s\n' "$PROFILE" > "$MODPATH/profile.conf"

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/profile.conf" 0 0 0644
