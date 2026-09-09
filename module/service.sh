#!/system/bin/sh
# Once-per-boot orchestration. service.sh and boot-completed.sh may race on modern KernelSU;
# /dev is tmpfs, so the lock and completion marker naturally reset on a real reboot.

MODDIR=${0%/*}
. "$MODDIR/scripts/lib.sh"

# API v4 observer. Copying/creating a Lua file in this directory is the official hot-reload path.
# Keep it observer-only until the callbacks are validated on-device; no DVFS node has two owners.
install_fas_extension() {
  local ext=/dev/fas_rs/extensions/m54tuner.lua i=0
  [ -f "$MODDIR/fas/m54tuner.lua" ] || return 0
  while [ ! -d /dev/fas_rs/extensions ] && [ "$i" -lt 90 ]; do sleep 1; i=$((i + 1)); done
  [ -d /dev/fas_rs/extensions ] || { log "fas-rs extension directory unavailable"; return 0; }
  cp -f "$MODDIR/fas/m54tuner.lua" "$ext" 2>/dev/null && chmod 0644 "$ext" 2>/dev/null
}
install_fas_extension &

SESSION=/dev/.m54tuner_session
BOOT_LOCK=/dev/.m54tuner_boot_lock
if [ -e "$SESSION" ]; then
  sh "$MODDIR/scripts/adaptive_start.sh"
  exit 0
fi

if ! mkdir "$BOOT_LOCK" 2>/dev/null; then
  i=0
  while [ "$i" -lt 30 ] && [ ! -e "$SESSION" ]; do sleep 1; i=$((i + 1)); done
  [ -e "$SESSION" ] && exit 0
  # A killed service must not make the boot sequence impossible for the rest of this boot.
  if ! pid_record_alive "$BOOT_LOCK/owner" service.sh; then
    rm -rf "$BOOT_LOCK"
    mkdir "$BOOT_LOCK" 2>/dev/null || exit 1
  else
    exit 0
  fi
fi
pid_record_write "$BOOT_LOCK/owner" service.sh
trap 'rm -rf "$BOOT_LOCK"' EXIT INT TERM
[ -e "$SESSION" ] && exit 0

if [ "${M54_BOOT_READY:-}" != 1 ]; then
  i=0
  while [ "$(getprop sys.boot_completed)" != 1 ] && [ "$i" -lt 120 ]; do sleep 1; i=$((i + 1)); done
fi
[ "$(getprop sys.boot_completed)" = 1 ] || { log "boot sequence skipped: boot not completed"; exit 1; }
sleep 2

: > "$RESULT"
export M54_RESULT_APPEND=1
log "=== boot sequence ==="

# Late load/temp-root records pending renderer changes; compositor restart stays explicit.
sh "$MODDIR/scripts/apply_render.sh"
sh "$MODDIR/scripts/apply_art.sh"
sh "$MODDIR/scripts/apply_mem.sh"
sh "$MODDIR/scripts/apply_profile.sh"

touch "$SESSION"
chmod 0600 "$SESSION" 2>/dev/null
log "=== boot sequence done ==="
