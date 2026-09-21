#!/system/bin/sh
# M54 Tuner module v2 — memory tier (zram algorithm + swappiness). Split out of service.sh so
# changing the zram algorithm no longer drags dexopt and a SurfaceFlinger restart along with it.
# Live, but destructive to swap contents, so it is guarded and never runs on the fast path.

DIR=$(dirname "$0")
. "$DIR/lib.sh"

result_begin "mem"
if ! begin_apply_lock; then rep module.lock fail busy mem; result_end; exit 1; fi
trap 'end_apply_lock' EXIT INT TERM

algo=$(read_cfg zram_algo "")
sys=/sys/block/zram0
dev=/dev/block/zram0

if [ -z "$algo" ] || [ ! -d "$sys" ]; then
  rep zram.algo skip - "$algo"
  result_end
  exit $?
fi

cur=$(cat "$sys/comp_algorithm" 2>/dev/null | tr ' ' '\n' | grep '^\[' | tr -d '[]')
if ! cat "$sys/comp_algorithm" 2>/dev/null | tr -d '[]' | grep -qw "$algo"; then
  rep zram.algo fail unsupported "$algo"
  result_end
  exit $?
fi
if [ "$cur" = "$algo" ]; then
  rep zram.algo ok "$cur" "$algo"
  result_end
  exit $?
fi

# ---------------------------------------------------------------------------
# Anti-OOM guard. Rebuilding zram means swapping everything back into RAM first, so the check is
# "does free RAM cover what is currently in swap". When it does not, refusing outright just leaves
# the user stuck — the shortfall is almost always cached pages and idle background apps, i.e. the
# cheapest memory on the device. So we MAKE the room instead, in escalating steps, re-measuring
# after each one, and only give up if even a cleared device cannot take it.
# ---------------------------------------------------------------------------
swap_used() {
  local u
  u=$(grep -E "^$dev[[:space:]]" /proc/swaps | awk '{print $4}')
  [ -z "$u" ] && u=0
  echo "$u"
}
# 20% headroom over the swapped-out pages, plus 300 MB so the rebuild itself has somewhere to run.
need_now() { echo $(( $(swap_used) * 12 / 10 + 307200 )); }
fits() { [ "$(mem_avail)" -ge "$(need_now)" ]; }

free_ram() {
  # 1. Background apps. `kill-all` targets BACKGROUND processes only: whatever is on screen —
  #    M54 Tuner itself, when the user triggered this from the UI — is not touched, and neither are
  #    persistent/system services. Killed apps simply relaunch cold; nothing is lost.
  cmd activity kill-all >/dev/null 2>&1 || am kill-all >/dev/null 2>&1
  sleep 2
  fits && { rep mem.freed ok kill_bg kill_bg; log "mem: freed by kill-all"; return 0; }

  # 2. Last-resort page-cache reclaim is reserved for this destructive maintenance operation. It
  # is intentionally NOT used by Game activation: cold UFS reads there hurt frame pacing.
  sync
  echo 1 > /proc/sys/vm/drop_caches 2>/dev/null
  echo 1 > /proc/sys/vm/compact_memory 2>/dev/null
  sleep 1
  fits && { rep mem.freed ok compact compact; log "mem: freed by compaction"; return 0; }
  return 1
}

if ! fits; then
  before=$(mem_avail)
  log "zram: not enough free RAM (avail=$before need=$(need_now)) — clearing"
  if ! free_ram; then
    rep mem.freed fail "$(mem_avail)" "$(need_now)"
    rep zram.algo fail "lowmem:$(mem_avail)<$(need_now)" "$algo"
    log "zram: giving up, still short after clearing (avail=$(mem_avail))"
    result_end
    exit $?
  fi
  log "zram: cleared, avail $before -> $(mem_avail)"
fi

disk=$(cat "$sys/disksize")                          # keep Samsung's 4GB, never resize
old_algo="$cur"
old_prio=$(grep -E "^$dev[[:space:]]" /proc/swaps 2>/dev/null | awk '{print $5}')
case "$old_prio" in ''|*[!0-9-]*) old_prio=-1;; esac

restore_old_swap() {
  log "zram: rolling back to algo=$old_algo size=$disk priority=$old_prio"
  ( echo 1 > "$sys/reset" ) 2>/dev/null || return 1
  ( echo "$old_algo" > "$sys/comp_algorithm" ) 2>/dev/null || return 1
  ( echo "$disk" > "$sys/disksize" ) 2>/dev/null || return 1
  mkswap "$dev" >/dev/null 2>&1 || return 1
  if [ "$old_prio" -ge 0 ] 2>/dev/null; then swapon -p "$old_prio" "$dev" 2>/dev/null
  else swapon "$dev" 2>/dev/null; fi
}

abort_rebuild() {
  local stage="$1"
  if restore_old_swap; then rep zram.rollback warn "$old_algo" "$stage"
  else rep zram.rollback fail no-swap "$old_algo"; fi
  rep zram.algo fail "$stage" "$algo"
  result_end
  exit $?
}

swapoff "$dev" 2>/dev/null || {
  rep zram.algo fail swapoff "$algo"
  log "zram: swapoff failed"
  result_end
  exit $?
}
( echo 1 > "$sys/reset" ) 2>/dev/null || abort_rebuild reset
( echo "$algo" > "$sys/comp_algorithm" ) 2>/dev/null || abort_rebuild algorithm
( echo "$disk" > "$sys/disksize" ) 2>/dev/null || abort_rebuild disksize
mkswap "$dev" >/dev/null 2>&1 || abort_rebuild mkswap
if { [ "$old_prio" -ge 0 ] 2>/dev/null && swapon -p "$old_prio" "$dev" 2>/dev/null; } || \
   { [ "$old_prio" -lt 0 ] 2>/dev/null && swapon "$dev" 2>/dev/null; }; then
  now=$(cat "$sys/comp_algorithm" | tr ' ' '\n' | grep '^\[' | tr -d '[]')
  if [ "$now" = "$algo" ]; then rep zram.algo ok "$now" "$algo"; else rep zram.algo fail "$now" "$algo"; fi
  log "zram: algo=$now disksize=$disk priority=$old_prio"
else
  abort_rebuild swapon
fi
result_end
exit $?
