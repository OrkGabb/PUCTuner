#!/system/bin/sh
# Stops the keepalive watcher at its recorded PID. Signalling by name would also hit the `sleep`
# subprocess and leave the loop running — the same mistake that cost us the bench restore.
DIR=$(dirname "$0")
. "$DIR/lib.sh"

PIDF="$M54_DIR/keepalive_pid"
[ -f "$PIDF" ] || { echo "keepalive nao esta rodando"; exit 0; }
if pid_record_alive "$PIDF" keepalive.sh; then PID=$(pid_record_pid "$PIDF")
else
  # v2 migration: accept a legacy numeric PID only when its cmdline still proves ownership.
  PID=$(cat "$PIDF" 2>/dev/null)
  case "$PID" in ''|*[!0-9]*) rm -f "$PIDF"; echo "registro antigo removido"; exit 0;; esac
  tr '\0' ' ' < "/proc/$PID/cmdline" 2>/dev/null | grep -Fq keepalive.sh || {
    rm -f "$PIDF"; echo "registro obsoleto removido"; exit 0; }
fi
kill -TERM "$PID" 2>/dev/null
i=0
while [ "$i" -lt 8 ]; do
  kill -0 "$PID" 2>/dev/null || break
  sleep 1
  i=$((i + 1))
done
kill -0 "$PID" 2>/dev/null && kill -9 "$PID" 2>/dev/null
rm -f "$PIDF"
log "keepalive: parado"
echo "keepalive parado"
