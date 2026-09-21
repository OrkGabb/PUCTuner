#!/system/bin/sh
# M54 Tuner module v2 — shared helpers. POSIX sh (mksh/toybox on Android).
# Sourced by post-fs-data.sh, service.sh, apply_profile.sh, apply_render.sh, status.sh.

umask 077

M54_DIR=${M54_DIR:-/data/adb/m54tuner}
CONFIG="$M54_DIR/config"
FACTORY="$M54_DIR/factory"
case "${M54_OP_ID:-}" in *[!A-Za-z0-9._-]*) M54_OP_ID="" ;; esac
if [ -n "${M54_OP_ID:-}" ]; then RESULT="$M54_DIR/result.$M54_OP_ID"; else RESULT="$M54_DIR/result"; fi
FPROPS="$M54_DIR/factory_props"
LOG="$M54_DIR/m54tuner.log"

# Every script must be able to run standalone (the app calls them one at a time, and they are also
# usable from adb), so the state directory is guaranteed here rather than by whoever ran first.
mkdir -p "$M54_DIR" 2>/dev/null
chmod 0700 "$M54_DIR" 2>/dev/null

log() {
  local size
  size=0
  [ -f "$LOG" ] && size=$(wc -c < "$LOG" 2>/dev/null)
  case "$size" in ''|*[!0-9]*) size=0;; esac
  if [ "$size" -gt 131072 ]; then mv -f "$LOG" "$LOG.1" 2>/dev/null; fi
  echo "$(date '+%H:%M:%S') $*" >> "$LOG" 2>/dev/null
}

# Atomic state/config writer. Call as: producer | atomic_write /path/file [mode].
atomic_write() {
  local dst="$1" mode="${2:-0600}" tmp="$1.tmp.$$"
  cat > "$tmp" || { rm -f "$tmp"; return 1; }
  chmod "$mode" "$tmp" 2>/dev/null || { rm -f "$tmp"; return 1; }
  mv -f "$tmp" "$dst" || { rm -f "$tmp"; return 1; }
}

valid_pkg() { echo "$1" | grep -Eq '^[A-Za-z0-9_]+([.][A-Za-z0-9_]+)+$'; }

clamp_int() {
  local v="$1" lo="$2" hi="$3" def="$4"
  case "$v" in ''|*[!0-9-]*) v="$def";; esac
  [ "$v" -lt "$lo" ] 2>/dev/null && v="$lo"
  [ "$v" -gt "$hi" ] 2>/dev/null && v="$hi"
  echo "$v"
}

# Serializes state-changing scripts. mkdir is atomic and works in KSU BusyBox without relying on
# flock being compiled in. A stale owner is reclaimed only after PID/start-time/boot verification.
APPLY_LOCK="$M54_DIR/.apply_lock"
begin_apply_lock() {
  [ "${M54_LOCK_HELD:-}" = "1" ] && return 0
  local i=0 owner oboot ostart live_start
  while [ "$i" -lt 30 ]; do
    if mkdir "$APPLY_LOCK" 2>/dev/null; then
      if ! { echo "pid=$$"; echo "boot=$(boot_id)"; echo "start=$(proc_start_s $$)"; } |
        atomic_write "$APPLY_LOCK/owner" 0600; then
        # A lock without a durable owner record is not owned. Publishing failure used to be
        # ignored, allowing another process to reclaim the directory while this writer proceeded.
        rmdir "$APPLY_LOCK" 2>/dev/null
        return 1
      fi
      M54_OWNS_LOCK=1
      return 0
    fi
    owner=$(grep '^pid=' "$APPLY_LOCK/owner" 2>/dev/null | cut -d= -f2)
    # mkdir precedes publication of owner. Do not reclaim a lock during that window.
    if [ -z "$owner" ]; then
      sleep 1
      i=$((i + 1))
      [ "$i" -lt 3 ] && continue
      [ -f "$APPLY_LOCK/owner" ] && continue
      rmdir "$APPLY_LOCK" 2>/dev/null
      continue
    fi
    oboot=$(grep '^boot=' "$APPLY_LOCK/owner" 2>/dev/null | cut -d= -f2-)
    ostart=$(grep '^start=' "$APPLY_LOCK/owner" 2>/dev/null | cut -d= -f2)
    live_start=$(proc_start_s "$owner")
    if [ "$oboot" != "$(boot_id)" ] || [ "$live_start" = -1 ] || [ "$live_start" != "$ostart" ]; then
      rm -rf "$APPLY_LOCK" 2>/dev/null
      continue
    fi
    sleep 1
    i=$((i + 1))
  done
  return 1
}

end_apply_lock() {
  [ "${M54_OWNS_LOCK:-}" = "1" ] || return 0
  local owner owner_start owner_boot
  owner=$(grep '^pid=' "$APPLY_LOCK/owner" 2>/dev/null | cut -d= -f2)
  owner_start=$(grep '^start=' "$APPLY_LOCK/owner" 2>/dev/null | cut -d= -f2)
  owner_boot=$(grep '^boot=' "$APPLY_LOCK/owner" 2>/dev/null | cut -d= -f2-)
  if [ "$owner" = "$$" ] && [ "$owner_start" = "$(proc_start_s $$)" ] && [ "$owner_boot" = "$(boot_id)" ]; then
    rm -rf "$APPLY_LOCK" 2>/dev/null
  fi
  M54_OWNS_LOCK=0
}

# ---------------------------------------------------------------------------
# RESULT REPORTING — the fix for "sometimes it applies everything, sometimes not".
# Every lever appends one line to $RESULT; the app reads the file after each apply and
# renders per-lever OK/FAIL with the value that actually stuck. Nothing is reported as
# applied unless it was read back from the kernel.
#   R|<key>|<ok|fail|skip|warn>|<got>|<want>
# ---------------------------------------------------------------------------
# Set M54_RESULT_APPEND=1 to keep the previous sections (the boot sequence runs several scripts
# and the app wants one combined report instead of only the last one).
M54_RESULT_FAILED=0
M54_RESULT_IO_FAILED=0
result_begin() {
  M54_RESULT_FAILED=0
  M54_RESULT_IO_FAILED=0
  if [ "${M54_RESULT_APPEND:-0}" != "1" ]; then
    : > "$RESULT" 2>/dev/null || M54_RESULT_IO_FAILED=1
  fi
  echo "T|$(date '+%s')|$1" >> "$RESULT" 2>/dev/null || M54_RESULT_IO_FAILED=1
  [ "$M54_RESULT_IO_FAILED" = 0 ]
}
rep() {
  [ "$2" = fail ] && M54_RESULT_FAILED=1
  echo "R|$1|$2|$3|$4" >> "$RESULT" 2>/dev/null || M54_RESULT_IO_FAILED=1
  return 0
}
result_end() {
  local state=ok
  [ "$M54_RESULT_FAILED" = 0 ] && [ "$M54_RESULT_IO_FAILED" = 0 ] || state=fail
  echo "E|$(date '+%s')|$state" >> "$RESULT" 2>/dev/null || M54_RESULT_IO_FAILED=1
  [ "$M54_RESULT_FAILED" = 0 ] && [ "$M54_RESULT_IO_FAILED" = 0 ]
}

# read_cfg <key> <default>
read_cfg() {
  local k="$1" def="$2" v
  [ -f "$CONFIG" ] || { echo "$def"; return; }
  v=$(grep -E "^$k=" "$CONFIG" 2>/dev/null | tail -1 | cut -d= -f2-)
  [ -n "$v" ] && echo "$v" || echo "$def"
}

# write_node <path> <value> [exact|sched|atleast] — write, read back, retry 3x. Silent; rc only.
# Return codes: 0 stuck, 1 written but did not stick, 2 node absent, 3 node refuses writes.
# The redirection is wrapped in a subshell because `echo x > file 2>/dev/null` does NOT hide a
# failure to OPEN the file — that error comes from the shell itself, and it leaked to stderr for
# every locked node. Its exit status is also the only reliable way to tell "another module made
# this read-only" from "the kernel ignored my value".
write_node() {
  local path="$1" value="$2" mode="${3:-exact}" i now denied=0 clamped=0
  [ -e "$path" ] || return 2
  for i in 1 2 3; do
    if ( echo "$value" > "$path" ) 2>/dev/null; then denied=0; else denied=1; fi
    now=$(cat "$path" 2>/dev/null)
    case "$mode" in
      sched)   echo "$now" | grep -q "\[$value\]" && return 0 ;;
      atleast) [ -n "$now" ] && [ "$now" -ge "$value" ] 2>/dev/null && return 0 ;;
      # "give me the top": the kernel may hold a ceiling below the hardware maximum (policy4 clamps
      # to 2112000 even with every cooling device at 0, exactly like the MIF cap at 2093000).
      # Landing on that ceiling is success, not a failed write.
      ceiling)
        [ "$now" = "$value" ] && return 0
        [ -n "$now" ] && [ "$now" -le "$value" ] 2>/dev/null && clamped=1
        ;;
      # Millisecond knobs are stored in jiffies, so the read-back is rounded (150 -> 152 at HZ=250).
      # Demanding an exact match there would report a failure for a write that landed perfectly.
      near)
        [ -n "$now" ] || return 1
        local d tol
        d=$(( now > value ? now - value : value - now ))
        tol=$(( value / 20 )); [ "$tol" -lt 2 ] && tol=2
        [ "$d" -le "$tol" ] 2>/dev/null && return 0
        ;;
      *)       [ "$now" = "$value" ] && return 0 ;;
    esac
  done
  [ "$denied" = 1 ] && return 3
  [ "$clamped" = 1 ] && return 4
  return 1
}

# apply_node <key> <path> <value> [mode] — write_node + report. This is what every lever uses.
apply_node() {
  local key="$1" path="$2" value="$3" mode="${4:-exact}" rc got
  write_node "$path" "$value" "$mode"; rc=$?
  got=$(cat "$path" 2>/dev/null)
  case "$mode" in sched) got=$(echo "$got" | tr ' ' '\n' | grep '^\[' | tr -d '[]') ;; esac
  case "$rc" in
    0) rep "$key" ok "$got" "$value" ;;
    2) rep "$key" skip "-" "$value" ;;
    # Another module made the node read-only: foreign tuners chmod what they mean to keep.
    # Calling that our failure would be wrong; calling it applied would be worse.
    3) rep "$key" skip "bloqueado" "$value"; log "LOCKED $key want=$value" ;;
    4) rep "$key" warn "$got" "$value"; log "CLAMPED $key want=$value got=$got" ;;
    *) rep "$key" fail "$got" "$value"; log "FAIL $key want=$value got=$got" ;;
  esac
  return $rc
}

write_silent() { ( echo "$2" > "$1" ) 2>/dev/null; }

# nearest <target> <list...>
nearest() {
  local target="$1"; shift
  local best="" bestd="" d x
  for x in $*; do
    d=$(( x > target ? x - target : target - x ))
    if [ -z "$bestd" ] || [ "$d" -lt "$bestd" ]; then bestd="$d"; best="$x"; fi
  done
  echo "$best"
}

# ratio_freq <min> <max> <pct>
ratio_freq() { echo $(( $1 + $3 * ($2 - $1) / 100 )); }

# ---------------------------------------------------------------------------
# PROPS — resetprop wrapper. `pset` sets, `pdel` deletes (back to firmware default).
# ---------------------------------------------------------------------------
RP=""
resetprop_bin() {
  [ -n "$RP" ] && { echo "$RP"; return; }
  # Probed live: `resetprop` is NOT on PATH in a plain `su -c` shell on this device — it sits in
  # KernelSU's private bin, and `ksud` is at /data/adb/ksud. Try the real paths before PATH.
  if [ -x /data/adb/ksu/bin/resetprop ]; then RP="/data/adb/ksu/bin/resetprop"
  elif command -v resetprop >/dev/null 2>&1; then RP="resetprop"
  elif [ -x /data/adb/ksud ]; then RP="/data/adb/ksud resetprop"
  else RP="ksud resetprop"; fi
  echo "$RP"
}
pset() {
  local rp; rp=$(resetprop_bin)
  $rp -n "$1" "$2" 2>/dev/null || $rp "$1" "$2" 2>/dev/null
}
pdel() { local rp; rp=$(resetprop_bin); $rp --delete "$1" 2>/dev/null; }

# apply_prop <key> <prop> <value>  — set + read back + report ("" value deletes the prop).
apply_prop() {
  local key="$1" prop="$2" value="$3" got
  if [ -z "$value" ]; then
    # A delete is a write like any other: read back, do not assume. An absent prop reads
    # empty, which is also the success state, so a delete of a prop that was never there
    # is honestly reported ok -- but a surviving value is a failure, not a success.
    pdel "$prop"
    got=$(getprop "$prop")
    if [ -z "$got" ]; then rep "$key" ok "-" "-"; return 0
    else rep "$key" fail "$got" "-"; log "FAIL prop-delete $prop still=$got"; return 1; fi
  fi
  pset "$prop" "$value"
  got=$(getprop "$prop")
  if [ "$got" = "$value" ]; then rep "$key" ok "$got" "$value"; return 0
  else rep "$key" fail "$got" "$value"; log "FAIL prop $prop want=$value got=$got"; return 1; fi
}

# ---------------------------------------------------------------------------
# RESTART SCOPE — the fix for "soft-reboot for everything".
# Three tiers, always use the cheapest one that actually makes the change live:
#   live  : sysfs / procfs writes, effective immediately, nothing to restart.
#   apps  : debug.hwui.* / ro.hwui.* are read when a process starts its HWUI ->
#           `am force-stop <pkg>` on the affected apps only. No screen blink.
#   sf    : debug.renderengine.* / debug.sf.* / ro.surface_flinger.* are read once by
#           SurfaceFlinger -> needs `ctl.restart surfaceflinger` (screen blinks).
#           NEVER done implicitly: the app must pass --sf after the user confirms.
# ---------------------------------------------------------------------------
force_stop_apps() {
  local list="$1" n=0 g
  [ -z "$list" ] && return 0
  local IFS=,
  for g in $list; do
    g=$(echo "$g" | tr -d ' ')
    [ -z "$g" ] && continue
    pm path "$g" >/dev/null 2>&1 || continue
    if cmd activity force-stop "$g" >/dev/null 2>&1 && [ -z "$(pidof "$g" 2>/dev/null)" ]; then
      n=$((n+1))
    else
      rep "render.restart_apps.$g" fail running stopped
    fi
  done
  unset IFS
  rep "render.restart_apps" ok "$n" "$n"
  log "force-stop: $n app(s)"
}

restart_sf() {
  local before after i=0
  before=$(first_pid surfaceflinger)
  log "restarting surfaceflinger pid=$before"
  setprop ctl.restart surfaceflinger 2>/dev/null
  while [ "$i" -lt 12 ]; do
    sleep 1; after=$(first_pid surfaceflinger)
    [ "$after" -gt 0 ] 2>/dev/null && [ "$after" != "$before" ] && break
    i=$((i + 1))
  done
  if [ "$after" -gt 0 ] 2>/dev/null && [ "$after" != "$before" ]; then
    if touch /dev/.m54tuner_sf_done && rm -f "$M54_DIR/pending_sf"; then
      rep "render.sf_restart" ok "$after" "$before"
    else
      rep "render.sf_restart" fail marker restart
      return 1
    fi
  else
    mark_pending_sf
    rep "render.sf_restart" fail "$after" restart
    log "surfaceflinger restart not verified before=$before after=$after"
    return 1
  fi
}

mark_pending_sf() {
  if touch "$M54_DIR/pending_sf"; then rep "render.sf_pending" warn pending pending
  else rep "render.sf_pending" fail marker pending; return 1; fi
}

# first_pid <name...> — the pid of a running service, or 0 (pidof can return several, e.g. zygote
# and zygote64).
first_pid() {
  local n p
  for n in "$@"; do
    p=$(pidof "$n" 2>/dev/null | tr ' ' '\n' | head -1)
    [ -n "$p" ] && { echo "$p"; return; }
  done
  echo 0
}

# uptime_s — whole seconds since boot. The clock for "when did we write this prop".
uptime_s() { cut -d' ' -f1 /proc/uptime 2>/dev/null | cut -d. -f1; }

# proc_start_s <pid> — seconds since boot at which that process started (field 22 of
# /proc/PID/stat, in clock ticks; Android runs at 100 Hz). -1 when the process is gone.
proc_start_s() {
  local pid="$1" ticks
  [ -r "/proc/$pid/stat" ] || { echo -1; return; }
  ticks=$(awk '{print $22}' "/proc/$pid/stat" 2>/dev/null)
  case "$ticks" in ''|*[!0-9]*) echo -1; return;; esac
  echo $(( ticks / 100 ))
}

pid_record_write() {
  local file="$1" tag="$2"
  { echo "pid=$$"; echo "boot=$(boot_id)"; echo "start=$(proc_start_s $$)"; echo "tag=$tag"; } |
    atomic_write "$file" 0600
}

pid_record_pid() { grep '^pid=' "$1" 2>/dev/null | cut -d= -f2; }

pid_record_alive() {
  local file="$1" tag="$2" pid rec_boot rec_start live_start cmd
  [ -f "$file" ] || return 1
  pid=$(pid_record_pid "$file")
  rec_boot=$(grep '^boot=' "$file" 2>/dev/null | cut -d= -f2-)
  rec_start=$(grep '^start=' "$file" 2>/dev/null | cut -d= -f2)
  [ "$rec_boot" = "$(boot_id)" ] || return 1
  live_start=$(proc_start_s "$pid"); [ "$live_start" != -1 ] && [ "$live_start" = "$rec_start" ] || return 1
  cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null)
  echo "$cmd" | grep -Fq "$tag"
}

# pending_for <state file> <at-key> <process names...> — 0 (true) when the props recorded in that
# state file were written AFTER the named process started, i.e. it has not read them yet.
#
# This exists because the pending_sf / pending_soft FILES are only a cache: they are written when an
# apply runs and nothing re-evaluates them afterwards. Measured live — the props were written at
# uptime 95117, SurfaceFlinger restarted on its own at 97671, and the stale flag sat there claiming a
# restart was pending for the next 42 minutes because no apply had run since. The UI must read the
# fact, not the cache.
pending_for() {
  local st="$1" atkey="$2"
  shift 2
  [ -f "$st" ] || return 1
  local boot at start
  boot=$(grep -E '^boot=' "$st" 2>/dev/null | cut -d= -f2-)
  [ "$boot" = "$(boot_id)" ] || return 1      # a record from another boot describes props that are gone
  at=$(grep -E "^$atkey=" "$st" 2>/dev/null | cut -d= -f2-)
  case "$at" in ''|*[!0-9]*) return 1;; esac
  [ "$at" = "0" ] && return 1                 # written at boot, before anything could read them
  start=$(proc_start_s "$(first_pid "$@")")
  [ "$start" -lt "$at" ] 2>/dev/null
}

# boot_id — changes on every real reboot. resetprop values do NOT survive one, so a record carrying
# a different boot_id says nothing about this session and must be discarded.
boot_id() { cat /proc/sys/kernel/random/boot_id 2>/dev/null; }

# ---------------------------------------------------------------------------
# RAM RECLAIM — cached/background process termination, run proactively instead of waiting for
# the kernel or
# Samsung's AL_Kill to do it reactively at the worst possible moment. Shared by the zram anti-OOM
# guard (apply_mem.sh's free_ram) and the Game-activation / manual RAM clear (apply_profile.sh,
# clear_ram.sh). Always ONE pass per trigger — this must never become a loop, that is exactly the
# generic "RAM booster" pattern this is meant to not be.
# ---------------------------------------------------------------------------
mem_avail() { grep MemAvailable /proc/meminfo | awk '{print $2}'; }

# clear_background_ram — kill background/cached apps only. `cmd activity kill-all`
# never touches the foreground app (M54 Tuner itself, or whatever triggered this) nor persistent /
# system services — it targets exactly the cached processes the kernel would pick first anyway.
# Reports the before/after MemAvailable (kB) so the app can show what it actually bought.
clear_background_ram() {
  local before after delta rc=0
  before=$(mem_avail)
  cmd activity kill-all >/dev/null 2>&1 || am kill-all >/dev/null 2>&1 || rc=1
  after=$(mem_avail)
  delta=$(( after - before ))
  if [ "$rc" = 0 ] && [ "$delta" -gt 0 ]; then
    rep mem.game_clear ok "$after (+$delta kB)" "$before"
  else
    rep mem.game_clear warn "$after (${delta} kB)" "$before"
  fi
  log "game ram clear (cached/background only): avail $before -> $after kB"
}

# ---------------------------------------------------------------------------
# FACTORY PROP MEMORY. `resetprop --delete` removes a property outright — it does NOT put back the
# value build.prop shipped. For a prop that exists from the factory (debug.sf.latch_unsignaled=true,
# ro.surface_flinger.game_default_frame_rate_override=60, the whole dalvik.vm.* set) deleting is a
# silent regression, so every managed prop's original value is recorded the first time we touch it
# and reverting writes that value back instead of deleting.
# ---------------------------------------------------------------------------
prop_remember() {
  grep -qE "^$1=" "$FPROPS" 2>/dev/null && return
  local original
  original=$(getprop "$1")
  { [ ! -f "$FPROPS" ] || cat "$FPROPS"; echo "$1=$original"; } | atomic_write "$FPROPS" 0600
}
prop_orig() { grep -E "^$1=" "$FPROPS" 2>/dev/null | tail -1 | cut -d= -f2-; }

# apply_prop_managed <key> <prop> <value>  — "" reverts to the remembered factory value
# (deleting only when the prop did not exist in the first place).
apply_prop_managed() {
  local key="$1" prop="$2" value="$3" orig
  prop_remember "$prop" || { rep "$key" fail backup "$value"; return 1; }
  if [ -n "$value" ]; then apply_prop "$key" "$prop" "$value"; return; fi
  orig=$(prop_orig "$prop")
  if [ -n "$orig" ]; then apply_prop "$key" "$prop" "$orig"
  else apply_prop "$key" "$prop" ""; fi
}

# apply_prop_tri <key> <prop> <state> <on-value>
# Three states, because "off" and "don't touch" are NOT the same thing: several of these props ship
# ENABLED from the factory (debug.sf.latch_unsignaled is `true` on this firmware), so deleting them
# on "off" is a regression, not a reset. `auto` leaves the firmware value exactly as it is.
apply_prop_tri() {
  local key="$1" prop="$2" state="$3" onval="$4"
  case "$state" in
    on)  apply_prop_managed "$key" "$prop" "$onval" ;;
    off) apply_prop_managed "$key" "$prop" "" ;;   # back to the factory value, not deleted
    *)   rep "$key" skip "$(getprop "$prop")" auto ;;
  esac
}

# ---------------------------------------------------------------------------
# SOFT REBOOT — the tier between "restart one process" and "reboot the phone".
#
# Restarting zygote re-execs the whole Java framework: system_server, SystemUI and every app come
# back fresh, so anything read once at zygote/system_server start (the whole dalvik.vm.* set) goes
# live, and Zygisk re-injects cleanly. It is NOT a full boot: init, the kernel, the KSU LKM and
# every sysfs tune we applied survive it, which is why it is cheap enough to be worth having — and
# also why it must stay deliberate: it closes every open app.
#
# Same rule as the SurfaceFlinger restart: never implicit. A tier that needs it writes
# $M54_DIR/pending_soft, the UI asks, and only then does soft_reboot() run.
# ---------------------------------------------------------------------------
mark_pending_soft() {
  if touch "$M54_DIR/pending_soft"; then rep "art.soft_pending" warn pending pending
  else rep "art.soft_pending" fail marker pending; return 1; fi
}

soft_reboot() {
  local before after i=0
  before=$(first_pid zygote64 zygote)
  log "soft reboot: restarting zygote pid=$before"
  sync
  setprop ctl.restart zygote 2>/dev/null
  while [ "$i" -lt 30 ]; do
    sleep 1; after=$(first_pid zygote64 zygote)
    [ "$after" -gt 0 ] 2>/dev/null && [ "$after" != "$before" ] && break
    i=$((i + 1))
  done
  if [ "$after" -gt 0 ] 2>/dev/null && [ "$after" != "$before" ]; then
    rm -f "$M54_DIR/pending_soft"
    rep "art.soft_reboot" ok "$after" "$before"
  else
    mark_pending_soft
    rep "art.soft_reboot" fail "$after" restart
    log "zygote restart not verified before=$before after=$after"
    return 1
  fi
}
