#!/system/bin/sh
# M54 Tuner — keeps the chosen games alive in the background WITHOUT the Game Optimizing Service.
#
# Why a loop and not a single write: oom_score_adj is owned by the ActivityManager. Measured on this
# device — a foreground app sits at 0, pinning it to -700 holds while nothing changes, and the
# instant the app is backgrounded the AM rewrites it to 700. That is precisely the moment Samsung's
# `AL_Kill : over 3 sec` starts counting, so the value has to be put back within a second or two of
# every transition.
#
# This IS a race against that 3-second timer, and it is honest to say so. Measured with a 2 s loop:
# 1 s after backgrounding the process was already back at 700, and only at ~3 s did the watcher
# restore -700 — landing right on the deadline. Hence a 1 s default, which halves that window.
#
# And a caveat that matters more than the interval: oom_score_adj is what the KERNEL's low-memory
# killer reads. Samsung's `AL_Kill` is a framework policy kill based on process state, and it may
# ignore the score entirely. This reliably helps against reason=3 (LOW_MEMORY) — which is what
# killed ZZZ earlier with 4 GB RSS. Whether it stops AL_Kill is unproven.
#
# The value matters. Lower = more protected. system_server sits near -900 and persistent services
# near -800; a cached app is 700-999. -700 puts the game well above ordinary apps but still BELOW
# the system, so under real pressure the kernel still has somewhere to go that is not system_server.
# Pinning a 4 GB game at -1000 would make everything else the victim, including the framework.
#
# Usage: sh keepalive.sh [interval_seconds]     Stop: sh keepalive_stop.sh

DIR=$(dirname "$0")
. "$DIR/lib.sh"

INTERVAL=${1:-$(read_cfg protect_interval 1)}
ADJ=$(read_cfg protect_adj -700)
PIDF="$M54_DIR/keepalive_pid"

INTERVAL=$(clamp_int "$INTERVAL" 1 60 1)
ADJ=$(clamp_int "$ADJ" -850 500 -700)

if pid_record_alive "$PIDF" keepalive.sh; then exit 0; fi
rm -f "$PIDF"
pid_record_write "$PIDF" keepalive.sh || { log "keepalive: PID publication failed"; exit 1; }
cleanup_keepalive() {
  trap - EXIT INT TERM
  [ "$(pid_record_pid "$PIDF")" = "$$" ] && rm -f "$PIDF"
  exit 0
}
trap cleanup_keepalive EXIT INT TERM

log "keepalive: iniciado (intervalo ${INTERVAL}s, adj $ADJ)"

while :; do
  # Re-read the list every pass: the user can change it in the app without restarting the watcher.
  LIST=$(read_cfg protect_list "")
  [ -z "$LIST" ] && LIST=$(read_cfg games "")
  # Under genuine memory distress, do not make a multi-gigabyte game harder to kill than the UI.
  # ActivityManager will naturally restore its own adj on the next process-state transition.
  AVAIL=$(mem_avail)
  if [ -n "$LIST" ] && [ "$AVAIL" -ge 716800 ] 2>/dev/null; then
    IFS=,
    for g in $LIST; do
      g=$(echo "$g" | tr -d ' ')
      valid_pkg "$g" || continue
      for pid in $(pidof "$g" 2>/dev/null); do
        cur=$(cat "/proc/$pid/oom_score_adj" 2>/dev/null)
        case "$cur" in ''|*[!0-9-]*) continue;; esac
        # Only ever lower it. Raising something the AM deliberately protected would be worse than
        # doing nothing, and rewriting an already-good value burns wakeups for free.
        [ "$cur" -gt "$ADJ" ] 2>/dev/null && echo "$ADJ" > "/proc/$pid/oom_score_adj" 2>/dev/null
      done
    done
    unset IFS
  fi
  sleep "$INTERVAL"
done
