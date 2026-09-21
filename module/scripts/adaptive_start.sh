#!/system/bin/sh
DIR=${0%/*}
. "$DIR/lib.sh"
BIN="${DIR%/*}/bin/m54-adaptive"
[ -x "$BIN" ] || { log 'adaptive: native engine missing'; exit 1; }
[ "$(read_cfg adaptive_mode active)" != off ] || exit 0
# Starting the learner never requires selecting an activity/profile.
pid_record_alive "$M54_DIR/adaptive_pid" m54-adaptive && exit 0
pid_record_alive "$M54_DIR/bench_pid" bench.sh && exit 0
pid_record_alive "$M54_DIR/bench_sched_pid" bench_sched.sh && exit 0
nohup "$BIN" --run "$M54_DIR" </dev/null >/dev/null 2>&1 &
i=0
# Up to 5 s: the daemon restores its journal under the apply lock before it publishes a PID.
while [ "$i" -lt 50 ]; do
  pid_record_alive "$M54_DIR/adaptive_pid" m54-adaptive && exit 0
  sleep 0.1
  i=$((i + 1))
done
log 'adaptive: launch returned but daemon did not publish a live PID'
exit 1
