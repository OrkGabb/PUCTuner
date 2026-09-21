#!/system/bin/sh
# Restore every persistent Android/Samsung setting owned by M54 Tuner. Ownership ledgers are
# retained until every mutation is verified so a failed uninstall remains recoverable.
#
# Two phases, because uninstall runs them at different moments:
#   stop      stop the daemons this module started; needs the module's own stop scripts, so it
#             runs from uninstall.sh while the module directory still exists
#   settings  MARs / SPCM / Samsung settings / GOS; needs `content`, `settings` and `pm`, so it
#             runs only once the framework is up (uninstall_finish.sh waits for that)
# No argument runs both.
DIR=$(dirname "$0")
. "$DIR/lib.sh"
PHASE=${1:-all}

EXCL=content://com.samsung.android.sm.mars/MARs_ExcludeTarget
POL=content://com.samsung.android.sm.mars/MARs_Policy
SET=content://com.samsung.android.sm/settings
fail=0

if [ "$PHASE" != settings ]; then
  stop_if_present() {
    [ -f "$M54_DIR/$1" ] || return 0
    sh "$DIR/$2" >/dev/null 2>&1 || { log "restore: failed to stop $2"; fail=1; }
  }
  stop_if_present keepalive_pid keepalive_stop.sh
  stop_if_present bench_pid bench_stop.sh
  [ -f "$M54_DIR/bench_sched_pid" ] && sh "$DIR/bench_stop.sh" sched >/dev/null 2>&1 || {
    [ -f "$M54_DIR/bench_sched_pid" ] && fail=1
  }
  stop_if_present session_watch_pid session_watch_stop.sh
  stop_if_present thermal_guard_pid thermal_guard_stop.sh
fi
if [ "$PHASE" = stop ]; then
  [ "$fail" = 0 ] && exit 0
  log "restore: daemon stop INCOMPLETE"
  exit 1
fi

pkg_absent() {
  local out rc
  out=$(content query --uri "$EXCL" --where "packageName='$1'" 2>/dev/null); rc=$?
  [ "$rc" = 0 ] || return 1
  ! echo "$out" | grep -Fq "packageName=$1"
}
if [ -f "$M54_DIR/protect_owned" ]; then
  while IFS= read -r pkg; do
    valid_pkg "$pkg" || continue
    if ! content delete --uri "$EXCL" --where "packageName='$pkg'" >/dev/null 2>&1 || ! pkg_absent "$pkg"; then
      log "restore: MARs row still present or unreadable: $pkg"
      fail=1
    fi
  done < "$M54_DIR/protect_owned"
fi

spcm_now() { content query --uri "$SET" --where "key='spcm_switch'" 2>/dev/null | grep -o 'value=[01]' | head -1 | cut -d= -f2; }
policy_now() { content query --uri "$POL" --where "policyNum=$1" 2>/dev/null | grep -o 'isPolicyEnabled=[01]' | head -1 | cut -d= -f2; }
if [ -f "$M54_DIR/protect_backup" ]; then
  spcm=$(grep '^spcm=' "$M54_DIR/protect_backup" | tail -1 | cut -d= -f2-)
  if [ -n "$spcm" ] && [ "$spcm" != __ABSENT__ ]; then
    content update --uri "$SET" --bind value:s:"$spcm" --where "key='spcm_switch'" >/dev/null 2>&1 || fail=1
    [ "$(spcm_now)" = "$spcm" ] || fail=1
  fi
  for p in 1 8; do
    v=$(grep "^policy$p=" "$M54_DIR/protect_backup" | tail -1 | cut -d= -f2-)
    if [ -n "$v" ] && [ "$v" != __ABSENT__ ]; then
      content update --uri "$POL" --bind isPolicyEnabled:i:"$v" --where "policyNum=$p" >/dev/null 2>&1 || fail=1
      [ "$(policy_now "$p")" = "$v" ] || fail=1
    fi
  done
fi

if [ -f "$M54_DIR/samsung_backup" ]; then
  while IFS= read -r line; do
    # The pipe is escaped on purpose. Unescaped, mksh reads `${line%%|*}` as the alternation
    # "" | "*", so every table came out empty, every line was skipped, and this script logged
    # "restored and verified" having restored nothing (seen on the device, 2026-09-21).
    [ -n "$line" ] || continue
    table=${line%%\|*}; rest=${line#*\|}; key=${rest%%=*}; value=${rest#*=}
    # A ledger line that does not parse is a setting that cannot be restored, not one to skip.
    if [ -z "$table" ] || [ -z "$key" ] || [ "$rest" = "$line" ]; then
      log "restore: unparseable samsung_backup line: $line"
      fail=1
      continue
    fi
    # stdin inside this loop is the ledger, and `settings` is `cmd`, which hands its stdin to
    # system_server over binder. SELinux refuses an adb_data_file fd there, so every call failed
    # with "Failed transaction" and no key was ever restored (seen on the device, 2026-09-21).
    if [ "$value" = __ABSENT__ ]; then
      settings delete "$table" "$key" </dev/null >/dev/null 2>&1 || fail=1
      [ "$(settings get "$table" "$key" </dev/null 2>/dev/null)" = null ] || fail=1
    else
      settings put "$table" "$key" "$value" </dev/null >/dev/null 2>&1 || fail=1
      [ "$(settings get "$table" "$key" </dev/null 2>/dev/null)" = "$value" ] || fail=1
    fi
  done < "$M54_DIR/samsung_backup"
fi

if [ -f "$M54_DIR/gos_backup" ]; then
  gos=$(cat "$M54_DIR/gos_backup")
  case "$gos" in
    enabled)
      pm enable --user 0 com.samsung.android.game.gos >/dev/null 2>&1 || fail=1
      pm list packages -e --user 0 com.samsung.android.game.gos 2>/dev/null | grep -q com.samsung.android.game.gos || fail=1 ;;
    disabled)
      pm disable-user --user 0 com.samsung.android.game.gos >/dev/null 2>&1 || fail=1
      pm list packages -d --user 0 com.samsung.android.game.gos 2>/dev/null | grep -q com.samsung.android.game.gos || fail=1 ;;
    *) fail=1 ;;
  esac
fi

if [ "$fail" = 0 ]; then
  log "persistent settings restored and verified"
  exit 0
fi
log "persistent restore INCOMPLETE; ownership ledgers retained"
exit 1
