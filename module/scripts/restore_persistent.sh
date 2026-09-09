#!/system/bin/sh
# Restores every persistent Android/Samsung setting owned by M54 Tuner. Safe to run repeatedly.
DIR=$(dirname "$0")
. "$DIR/lib.sh"

EXCL=content://com.samsung.android.sm.mars/MARs_ExcludeTarget
POL=content://com.samsung.android.sm.mars/MARs_Policy
SET=content://com.samsung.android.sm/settings

sh "$DIR/keepalive_stop.sh" >/dev/null 2>&1
sh "$DIR/bench_stop.sh" >/dev/null 2>&1
[ -f "$M54_DIR/session_watch_pid" ] && sh "$DIR/session_watch_stop.sh" >/dev/null 2>&1
[ -f "$M54_DIR/thermal_guard_pid" ] && sh "$DIR/thermal_guard_stop.sh" >/dev/null 2>&1

if [ -f "$M54_DIR/protect_owned" ]; then
  while IFS= read -r pkg; do
    valid_pkg "$pkg" || continue
    content delete --uri "$EXCL" --where "packageName='$pkg'" >/dev/null 2>&1
  done < "$M54_DIR/protect_owned"
fi

if [ -f "$M54_DIR/protect_backup" ]; then
  spcm=$(grep '^spcm=' "$M54_DIR/protect_backup" | tail -1 | cut -d= -f2-)
  [ -n "$spcm" ] && [ "$spcm" != __ABSENT__ ] && \
    content update --uri "$SET" --bind value:s:"$spcm" --where "key='spcm_switch'" >/dev/null 2>&1
  for p in 1 8; do
    v=$(grep "^policy$p=" "$M54_DIR/protect_backup" | tail -1 | cut -d= -f2-)
    [ -n "$v" ] && [ "$v" != __ABSENT__ ] && \
      content update --uri "$POL" --bind isPolicyEnabled:i:"$v" --where "policyNum=$p" >/dev/null 2>&1
  done
fi

if [ -f "$M54_DIR/samsung_backup" ]; then
  while IFS= read -r line; do
    table=${line%%|*}; rest=${line#*|}; key=${rest%%=*}; value=${rest#*=}
    [ -n "$table" ] && [ -n "$key" ] || continue
    if [ "$value" = __ABSENT__ ]; then settings delete "$table" "$key" >/dev/null 2>&1
    else settings put "$table" "$key" "$value" >/dev/null 2>&1; fi
  done < "$M54_DIR/samsung_backup"
fi

if [ -f "$M54_DIR/gos_backup" ]; then
  gos=$(cat "$M54_DIR/gos_backup")
  [ "$gos" = enabled ] && pm enable --user 0 com.samsung.android.game.gos >/dev/null 2>&1
  [ "$gos" = disabled ] && pm disable-user --user 0 com.samsung.android.game.gos >/dev/null 2>&1
fi

log "persistent settings restored"
