#!/system/bin/sh
# M54 Tuner — profile-guided AOT for chosen games. Android 14+ ART Service prefers speed-profile;
# full speed remains an expert override because it compiles every method and consumes more storage.
# along with a profile switch or a boot. Reversible with `pm compile --reset <pkg>`.
# Usage: sh apply_dexopt.sh [--reset]

DIR=$(dirname "$0")
. "$DIR/lib.sh"

MODE=$(read_cfg dexopt_mode speed-profile)
case "$MODE" in speed-profile|speed) ;; *) MODE=speed-profile;; esac
[ "$1" = "--reset" ] && MODE=reset

result_begin "dexopt:$MODE"
if ! begin_apply_lock; then rep module.lock fail busy dexopt; result_end; exit 1; fi
trap 'end_apply_lock' EXIT INT TERM
games=$(read_cfg games "")
if [ -z "$games" ]; then
  rep dexopt skip - -
  result_end
  exit $?
fi

IFS=,
for g in $games; do
  g=$(echo "$g" | tr -d ' ')
  [ -z "$g" ] && continue
  if ! valid_pkg "$g"; then rep dexopt.invalid skip invalid-package "$MODE"; continue; fi
  if ! pm path "$g" >/dev/null 2>&1; then rep "dexopt.$g" skip absent "$MODE"; continue; fi
  if [ "$MODE" = reset ]; then
    # No -f on the speed path: `compile -m speed` skips what is already speed-compiled, so
    # re-running after another change costs seconds instead of recompiling every game.
    cmd package compile --reset "$g" >/dev/null 2>&1
  else
    cmd package compile -m "$MODE" "$g" >/dev/null 2>&1
  fi
  if [ $? -eq 0 ]; then
    live=$(pm art dump "$g" 2>/dev/null | grep -m1 -o 'status=[^ ]*' | cut -d= -f2 | tr -d '[],')
    [ -z "$live" ] && live="$MODE"
    rep "dexopt.$g" ok "$live" "$MODE"; log "dexopt $MODE: $g status=$live"
  else rep "dexopt.$g" fail - "$MODE"; fi
done
unset IFS
result_end
exit $?
