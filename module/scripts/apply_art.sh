#!/system/bin/sh
# M54 Tuner module v2 — ART / zygote tier.
#
# Everything here is read ONCE, by zygote, when it starts. Setting these props changes nothing for
# a running system: they only take effect on the next zygote start. That is precisely the case the
# soft reboot exists for — restarting zygote re-execs the framework (system_server, SystemUI, every
# app) while init, the kernel, the KSU LKM and all our sysfs tuning survive.
#
# So this script never restarts anything by itself: it writes the props, and if what it wrote
# differs from what the running zygote was started with, it marks $M54_DIR/pending_soft. The UI
# asks once, the user answers, and only then does soft_reboot() run. Passing --soft here does it
# immediately (used at boot, and when the user confirms).
#
# Usage: sh apply_art.sh [--soft] [--force] [--boot]
#   --boot  real boot: zygote has not started yet, so the props land naturally — record the state
#           and mark nothing as pending.

DIR=$(dirname "$0")
. "$DIR/lib.sh"

STATE="$M54_DIR/art_state"
DO_SOFT=0
FORCE=0
BOOT=0
for a in "$@"; do
  case "$a" in
    --soft) DO_SOFT=1 ;;
    --force) FORCE=1 ;;
    --boot) BOOT=1 ;;
  esac
done

USAP=$(read_cfg art_usap auto)          # auto|on|off
DEXCPU=$(read_cfg art_dex2oat_little auto)
HEAP=$(read_cfg art_heap auto)

result_begin "art"
if ! begin_apply_lock; then rep module.lock fail busy art; result_end; exit 1; fi
trap 'end_apply_lock' EXIT INT TERM

# Capture drift before rewriting the properties. A matching state-file signature only says what a
# previous run requested; it cannot prove another root tool did not change the live property later.
PROP_DRIFT=0
art_differs() { [ "$(getprop "$1")" != "$2" ] && PROP_DRIFT=1; }
if [ "$USAP" = on ]; then
  art_differs dalvik.vm.usap_pool_enabled true
  art_differs dalvik.vm.usap_pool_size_max 4
  art_differs dalvik.vm.usap_pool_size_min 2
  art_differs dalvik.vm.usap_refill_threshold 2
elif [ "$USAP" = off ] && [ -f "$FPROPS" ]; then
  for p in dalvik.vm.usap_pool_enabled dalvik.vm.usap_pool_size_max dalvik.vm.usap_pool_size_min dalvik.vm.usap_refill_threshold; do
    art_differs "$p" "$(prop_orig "$p")"
  done
fi
if [ "$DEXCPU" = on ]; then
  art_differs dalvik.vm.dex2oat-threads 4
  art_differs dalvik.vm.dex2oat-cpu-set 0,1,2,3
elif [ "$DEXCPU" = off ] && [ -f "$FPROPS" ]; then
  for p in dalvik.vm.dex2oat-threads dalvik.vm.dex2oat-cpu-set; do art_differs "$p" "$(prop_orig "$p")"; done
fi
if [ "$HEAP" = on ]; then art_differs dalvik.vm.heapgrowthlimit 288m
elif [ "$HEAP" = off ] && [ -f "$FPROPS" ]; then art_differs dalvik.vm.heapgrowthlimit "$(prop_orig dalvik.vm.heapgrowthlimit)"; fi

# ---------------------------------------------------------------------------
# 1. USAP — Unspecialized App Process pool. Zygote keeps N pre-forked, pre-warmed processes ready,
#    so a cold app start skips fork+preload. Samsung ships it DISABLED (probed: usap_pool_enabled
#    =false, max=3, min=1). The cost is a couple of idle processes; on 8 GB that is a good trade,
#    and it is orthogonal to anything that schedules threads which already exist: none of that
#    helps with the part of a cold start that happens before the process exists.
# ---------------------------------------------------------------------------
case "$USAP" in
  on)
    apply_prop_managed art.usap            dalvik.vm.usap_pool_enabled       true
    apply_prop_managed art.usap_max        dalvik.vm.usap_pool_size_max      4
    apply_prop_managed art.usap_min        dalvik.vm.usap_pool_size_min      2
    apply_prop_managed art.usap_threshold  dalvik.vm.usap_refill_threshold   2
    ;;
  off)
    apply_prop_managed art.usap            dalvik.vm.usap_pool_enabled       ""
    apply_prop_managed art.usap_max        dalvik.vm.usap_pool_size_max      ""
    apply_prop_managed art.usap_min        dalvik.vm.usap_pool_size_min      ""
    apply_prop_managed art.usap_threshold  dalvik.vm.usap_refill_threshold   ""
    ;;
  *) rep art.usap skip "$(getprop dalvik.vm.usap_pool_enabled)" auto ;;
esac

# ---------------------------------------------------------------------------
# 2. dex2oat on the little cluster. Background compilation (Play Store updates, the ART service's
#    idle dexopt, our own AOT pass) is a CPU hog that lands on whatever core the scheduler likes —
#    including the A78s, in the middle of a game. Pinning it to cpu0-3 with 4 threads keeps the big
#    cluster for the foreground. This is the ART tweak that matters most next to the DVFS floors:
#    they raise what the game gets, this stops the loudest competitor sitting on the big cores.
#    Probed factory values: dex2oat-threads and dex2oat-cpu-set are both EMPTY (no restriction).
# ---------------------------------------------------------------------------
case "$DEXCPU" in
  on)
    apply_prop_managed art.dex2oat_threads dalvik.vm.dex2oat-threads  4
    apply_prop_managed art.dex2oat_cpuset  dalvik.vm.dex2oat-cpu-set  0,1,2,3
    ;;
  off)
    apply_prop_managed art.dex2oat_threads dalvik.vm.dex2oat-threads  ""
    apply_prop_managed art.dex2oat_cpuset  dalvik.vm.dex2oat-cpu-set  ""
    ;;
  *) rep art.dex2oat skip "$(getprop dalvik.vm.dex2oat-cpu-set)" auto ;;
esac

# ---------------------------------------------------------------------------
# 3. Heap ceiling. heapgrowthlimit is the per-app cap before ART forces a GC; Samsung ships 256m
#    (heapsize, the hard ceiling for largeHeap apps, is 512m). Raising the growth limit to 288m
#    gives heavy games one fewer forced GC pause per scene load. Deliberately modest: this is RAM
#    per foreground app, and lmkd (ro.slmk.*, untouched here on purpose) is what pays for it.
# ---------------------------------------------------------------------------
case "$HEAP" in
  on)  apply_prop_managed art.heap_growth dalvik.vm.heapgrowthlimit 288m ;;
  off) apply_prop_managed art.heap_growth dalvik.vm.heapgrowthlimit "" ;;
  *)   rep art.heap skip "$(getprop dalvik.vm.heapgrowthlimit)" auto ;;
esac

if [ "$M54_RESULT_FAILED" != 0 ] || [ "$M54_RESULT_IO_FAILED" != 0 ]; then
  rep art.scope fail write-failed unchanged
  result_end
  exit $?
fi

# ---------------------------------------------------------------------------
# Scope: did anything change relative to the zygote that is currently running?
# ---------------------------------------------------------------------------
SIG="$USAP|$DEXCPU|$HEAP"
OLD_SIG=$(grep -vE '^(at|boot)=' "$STATE" 2>/dev/null | head -1)
OLD_AT=$(grep -E '^at=' "$STATE" 2>/dev/null | cut -d= -f2-)
OLD_BOOT=$(grep -E '^boot=' "$STATE" 2>/dev/null | cut -d= -f2-)
BOOT_NOW=$(boot_id)
case "$OLD_AT" in ''|*[!0-9]*) OLD_AT=0;; esac
if [ "$OLD_BOOT" != "$BOOT_NOW" ]; then OLD_SIG=""; OLD_AT=0; fi

if [ "$BOOT" = "1" ]; then
  { echo "$SIG"; echo "at=0"; echo "boot=$BOOT_NOW"; } | atomic_write "$STATE" 0600 || rep art.state fail write boot
  rm -f "$M54_DIR/pending_soft" || rep art.pending fail remove absent
  rep art.scope ok boot boot
  result_end
  rc=$?
  log "art applied at boot (zygote reads it naturally)"
  exit "$rc"
fi
NEED=0
[ "$SIG" != "$OLD_SIG" ] && NEED=1
[ "$PROP_DRIFT" = 1 ] && NEED=1
# All three on `auto` means nothing here is managed — no prop was written, so there is nothing for
# a soft reboot to make live. Never ask for one in that case.
if [ "$USAP" = auto ] && [ "$DEXCPU" = auto ] && [ "$HEAP" = auto ]; then NEED=0; fi
[ "$FORCE" = "1" ] && NEED=1

# Same start-time rule as the render tier: these props are inert for a zygote that started before
# they were written, and live for one that started after — whoever restarted it.
if [ "$NEED" = "1" ]; then
  OLD_SIG="$SIG"
  OLD_AT=$(uptime_s)
fi

ZYG_START=$(proc_start_s "$(first_pid zygote64 zygote)")
if [ "$DO_SOFT" = "1" ] && [ "$ZYG_START" -lt "$OLD_AT" ] 2>/dev/null; then
  { echo "$OLD_SIG"; echo "at=$OLD_AT"; echo "boot=$BOOT_NOW"; } | atomic_write "$STATE" 0600 || {
    rep art.state fail write pre-restart
    result_end
    exit $?
  }
  soft_reboot
  result_end
  exit $?
fi

if [ "$ZYG_START" -lt "$OLD_AT" ] 2>/dev/null; then
  mark_pending_soft
else
  rep art.soft_reboot ok live live
  rm -f "$M54_DIR/pending_soft" || rep art.pending fail remove absent
fi

{ echo "$OLD_SIG"; echo "at=$OLD_AT"; echo "boot=$BOOT_NOW"; } | atomic_write "$STATE" 0600 || rep art.state fail write live
result_end
rc=$?
log "art applied usap=$USAP dex2oat_little=$DEXCPU heap=$HEAP need_soft=$NEED soft_done=$DO_SOFT"
exit "$rc"
