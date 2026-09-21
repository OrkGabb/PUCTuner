#!/system/bin/sh
# M54 Tuner — Samsung's own limiter switches, which live in the settings tables rather than sysfs.
#
# These are not kernel knobs: they are OneUI policy flags, and several of them quietly cap what the
# hardware is allowed to do. Found by walking `settings list global` and `settings list secure`:
#
#   sem_low_heat_mode              1  Samsung's low-heat profile — limits sustained performance.
#   BG_AD_RESTRICTION_BY_AI        1  restricts background apps "by AI"; a prime suspect for games
#                                     dying seconds after you switch away.
#   restricted_device_performance  1,1  reads exactly as it is named.
#   sem_enhanced_cpu_responsiveness  0  Samsung's own responsiveness boost, shipped OFF.
#   adaptive_power_saving_setting  1  adaptive power saving, which throttles on its own schedule.
#
# Every original is captured once into $M54_DIR/samsung_backup BEFORE the first change, and OFF
# restores exactly those values — never a guessed default. Nothing here is applied unless the user
# turns it on: these are visible OneUI settings and silently flipping them would be rude.
#
# Usage: sh apply_samsung.sh

DIR=$(dirname "$0")
. "$DIR/lib.sh"

BAK="$M54_DIR/samsung_backup"
WANT=$(read_cfg samsung_perf 0)

result_begin "samsung"
if ! begin_apply_lock; then rep module.lock fail busy samsung; result_end; exit 1; fi
trap 'end_apply_lock' EXIT INT TERM

# save <table> <key> — records the CURRENT value once, before we ever touch it.
save() {
  local v
  grep -qE "^$1\|$2=" "$BAK" 2>/dev/null && return 0
  v=$(settings get "$1" "$2" 2>/dev/null)
  [ -n "$v" ] || return 1
  [ "$v" = "null" ] && v=__ABSENT__
  { [ ! -f "$BAK" ] || cat "$BAK"; echo "$1|$2=$v"; } | atomic_write "$BAK" 0600
}

restore_one() {
  local line v
  line=$(grep -E "^$1\|$2=" "$BAK" 2>/dev/null | tail -1)
  [ -z "$line" ] && { rep "sam.$2" skip sem-backup -; return; }
  v=${line#*=}
  if [ "$v" = "__ABSENT__" ]; then settings delete "$1" "$2" >/dev/null 2>&1
  else settings put "$1" "$2" "$v" 2>/dev/null; fi
  local got; got=$(settings get "$1" "$2" 2>/dev/null)
  if { [ "$v" = "__ABSENT__" ] && [ "$got" = "null" ]; } || [ "$got" = "$v" ]; then
    rep "sam.$2" ok "$got" "$v"
  else rep "sam.$2" fail "$got" "$v"; fi
}

set_one() {
  save "$1" "$2" || { rep "sam.$2" fail backup "$3"; return 1; }
  settings put "$1" "$2" "$3" 2>/dev/null
  local got; got=$(settings get "$1" "$2" 2>/dev/null)
  if [ "$got" = "$3" ]; then rep "sam.$2" ok "$got" "$3"; else rep "sam.$2" fail "$got" "$3"; fi
}

if [ "$WANT" = "1" ]; then
  set_one global sem_low_heat_mode 0
  set_one secure BG_AD_RESTRICTION_BY_AI 0
  set_one global restricted_device_performance "0, 0"
  set_one global sem_enhanced_cpu_responsiveness 1
  set_one global adaptive_power_saving_setting 0
  log "samsung: modos limitadores desligados"
else
  restore_one global sem_low_heat_mode
  restore_one secure BG_AD_RESTRICTION_BY_AI
  restore_one global restricted_device_performance
  restore_one global sem_enhanced_cpu_responsiveness
  restore_one global adaptive_power_saving_setting
  log "samsung: valores originais restaurados"
fi

result_end
exit $?
