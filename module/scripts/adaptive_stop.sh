#!/system/bin/sh
DIR=${0%/*}
. "$DIR/lib.sh"
BIN="${DIR%/*}/bin/m54-adaptive"
PIDF="$M54_DIR/adaptive_pid"
if pid_record_alive "$PIDF" m54-adaptive; then
  kill -TERM "$(pid_record_pid "$PIDF")" 2>/dev/null
  i=0
  while pid_record_alive "$PIDF" m54-adaptive && [ "$i" -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
  done
  if pid_record_alive "$PIDF" m54-adaptive; then
    log 'adaptive: stop pending, refusing a concurrent profile writer'
    exit 1
  fi
fi
# SIGKILL/crash recovery runs before any static profile can overwrite the journal's values.
if [ -x "$BIN" ] && [ -f "$M54_DIR/adaptive_journal" ]; then
  "$BIN" --restore "$M54_DIR" || exit 1
fi
