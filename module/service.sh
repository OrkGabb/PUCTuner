#!/system/bin/sh
# Once-per-boot orchestration. service.sh and boot-completed.sh may race on modern KernelSU;
# /dev is tmpfs, so the lock and completion marker naturally reset on a real reboot.

MODDIR=${0%/*}
. "$MODDIR/scripts/lib.sh"

SESSION=/dev/.m54tuner_session
BOOT_LOCK=/dev/.m54tuner_boot_lock
# Failed boot sequences this boot. The app replays the sequence on launch while SESSION is
# missing; a failure that repeats identically (a prop the firmware reasserts, a read-only node)
# would otherwise re-run the whole boot on every launch, forever. The app stops replaying once
# this count reaches its limit (ModuleBridge.consumeFirstOfSession); a manual apply still runs.
BOOT_FAILURES=/dev/.m54tuner_boot_failures
count_failure() {
  local n
  n=$(cat "$BOOT_FAILURES" 2>/dev/null)
  case "$n" in ''|*[!0-9]*) n=0;; esac
  echo $((n + 1)) > "$BOOT_FAILURES"
}
ensure_daemons() {
  sh "$MODDIR/scripts/adaptive_start.sh" || return 1
  if [ "$(read_cfg protect_games 0)" = 1 ]; then
    if ! pid_record_alive "$M54_DIR/keepalive_pid" keepalive.sh; then
      rm -f "$M54_DIR/keepalive_pid"
      nohup sh "$MODDIR/scripts/keepalive.sh" </dev/null >/dev/null 2>&1 &
      i=0
      while [ "$i" -lt 20 ] && ! pid_record_alive "$M54_DIR/keepalive_pid" keepalive.sh; do
        sleep 1; i=$((i + 1))
      done
    fi
    pid_record_alive "$M54_DIR/keepalive_pid" keepalive.sh || return 1
  fi
}
if [ -e "$SESSION" ]; then
  ensure_daemons
  exit $?
fi

if ! mkdir "$BOOT_LOCK" 2>/dev/null; then
  i=0
  while [ "$i" -lt 30 ] && [ ! -e "$SESSION" ]; do sleep 1; i=$((i + 1)); done
  [ -e "$SESSION" ] && { ensure_daemons; exit $?; }
  # A killed service must not make the boot sequence impossible for the rest of this boot.
  if ! pid_record_alive "$BOOT_LOCK/owner" service.sh; then
    rm -rf "$BOOT_LOCK"
    mkdir "$BOOT_LOCK" 2>/dev/null || exit 1
  else
    # Another live owner still holds the boot transaction. Timing out is not success: the caller
    # has no evidence that owner completed, and must be allowed to retry.
    exit 1
  fi
fi
pid_record_write "$BOOT_LOCK/owner" service.sh || { rmdir "$BOOT_LOCK" 2>/dev/null; exit 1; }
# Only remove what we still own: a racer that reclaimed the lock after us must not
# lose it when we exit, and we must not drop a lock we already lost.
boot_unlock() {
  local owner ostart
  owner=$(pid_record_pid "$BOOT_LOCK/owner" 2>/dev/null)
  ostart=$(grep '^start=' "$BOOT_LOCK/owner" 2>/dev/null | cut -d= -f2)
  if [ "$owner" = "$$" ] && [ "$ostart" = "$(proc_start_s $$)" ]; then rm -rf "$BOOT_LOCK"; fi
}
trap 'boot_unlock' EXIT INT TERM
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
# The completion marker is earned, not assumed: every apply is counted, and a boot where
# anything failed leaves no SESSION so the next trigger (follower service.sh, or the first
# app launch, which replays this same sequence) retries instead of skipping this boot.
fail=0
sh "$MODDIR/scripts/apply_render.sh" || fail=1
sh "$MODDIR/scripts/apply_art.sh" || fail=1
sh "$MODDIR/scripts/apply_mem.sh" || fail=1
sh "$MODDIR/scripts/apply_profile.sh" || fail=1

if [ "$fail" = 0 ] && ensure_daemons; then
  if ! touch "$SESSION" || ! chmod 0600 "$SESSION"; then
    rm -f "$SESSION"
    log "=== boot sequence INCOMPLETE (session marker write failed) ==="
    count_failure
    exit 1
  fi
  log "=== boot sequence done ==="
else
  log "=== boot sequence INCOMPLETE (see FAIL lines above) ==="
  count_failure
  exit 1
fi
