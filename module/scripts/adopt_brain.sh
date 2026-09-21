#!/system/bin/sh
# Takes a brain that was set aside back into use, under this firmware's identity.
#
# Every OTA changes the identity a brain is saved under, and the engine then sets the stored brain
# aside as adaptive_model.rejected.<time> and starts empty. That is the right default: an update
# can change what a CPU or GPU level does on this silicon. Whether the old brain is still worth
# more than starting over is the user's call, so this only ever runs when the app asks.
#
# Usage: sh adopt_brain.sh <adaptive_model.rejected.NAME>
# The brain in use, if any, is set aside as adaptive_model.replaced.<time>, never overwritten.
DIR=${0%/*}
. "$DIR/lib.sh"
BIN="${DIR%/*}/bin/m54-adaptive"
NAME="$1"

result_begin "adopt"
case "$NAME" in
  adaptive_model.rejected.*) ;;
  *) rep adopt.name fail "$NAME" rejected; result_end; exit 1 ;;
esac
case "$NAME" in */*) rep adopt.name fail "$NAME" rejected; result_end; exit 1 ;; esac
[ -f "$M54_DIR/$NAME" ] || { rep adopt.file fail absent present; result_end; exit 1; }
[ -x "$BIN" ] || { rep adopt.engine fail missing present; result_end; exit 1; }

# The engine's singleton lock is what keeps a running daemon from saving over the file being
# replaced; --adopt refuses (exit 3) while one is alive, so stop it first.
was_running=0
pid_record_alive "$M54_DIR/adaptive_pid" m54-adaptive && was_running=1
if ! sh "$DIR/adaptive_stop.sh"; then
  rep adopt.stop fail running stopped
  result_end
  exit 1
fi

out=$("$BIN" --adopt "$M54_DIR" "$NAME" 2>&1); rc=$?
if [ "$rc" = 0 ]; then
  rep adopt.brain ok "$NAME" adopted
  log "adopt: $NAME taken into use"
else
  rep adopt.brain fail "${out:-exit$rc}" adopted
  log "adopt: $NAME refused rc=$rc ${out}"
fi

if [ "$was_running" = 1 ] && ! sh "$DIR/adaptive_start.sh"; then
  rep adopt.restart fail stopped running
fi
result_end
