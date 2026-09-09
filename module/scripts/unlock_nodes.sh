#!/system/bin/sh
# Restores the write bit on tuning nodes that something took it away from.
#
# Observed on this device: both CPU governors and four thermal zone modes left at mode 0444 by
# a module that had already been disabled three days earlier. Root can still chmod them back --
# the lock is discretionary permission, not SELinux and not the kernel refusing -- but the
# module reads capability with access(W_OK), so a locked node silently disappears from its
# capabilities and the user is never told why.
#
# Deliberately NOT automatic and never run from a hook. Taking the write bit back is undoing
# another module's decision, and that is the user's call, not a side effect of booting.
#
#   unlock_nodes.sh            list what is locked, change nothing
#   unlock_nodes.sh --apply    restore mode 0644, recording the previous mode
#   unlock_nodes.sh --revert   put the recorded modes back
set -eu
DIR=${0%/*}
. "$DIR/lib.sh" 2>/dev/null || M54_DIR=/data/adb/m54tuner
RECORD="$M54_DIR/locked_modes"

nodes() {
  for pattern in \
    /sys/devices/system/cpu/cpufreq/policy*/scaling_governor \
    /sys/devices/system/cpu/cpufreq/policy*/scaling_min_freq \
    /sys/devices/system/cpu/cpufreq/policy*/scaling_max_freq \
    /sys/kernel/gpu/gpu_governor /sys/kernel/gpu/gpu_min_clock /sys/kernel/gpu/gpu_max_clock \
    /sys/class/devfreq/*mif*/min_freq /sys/class/devfreq/*mif*/max_freq \
    /sys/class/thermal/thermal_zone*/mode \
    /proc/sys/kernel/sched_pelt_multiplier
  do
    [ -e "$pattern" ] || continue
    # Not [ -w ]: that is access(W_OK), and as root it says yes even at mode 0444.
    mode=$(stat -c %a "$pattern" 2>/dev/null) || continue
    case "$mode" in *[2367]??) continue ;; esac
    echo "$pattern"
  done
}

case "${1:-}" in
  --apply)
    : > "$RECORD.new"
    count=0
    for n in $(nodes); do
      mode=$(stat -c %a "$n" 2>/dev/null || echo 444)
      # Only record the first time, so a repeated run cannot overwrite the true original.
      grep -q "^$n " "$RECORD" 2>/dev/null || echo "$n $mode" >> "$RECORD.new"
      if chmod 644 "$n" 2>/dev/null && [ -w "$n" ]; then
        count=$((count + 1))
        echo "unlocked $n (was $mode)"
      else
        echo "could not unlock $n"
      fi
    done
    [ -s "$RECORD.new" ] && cat "$RECORD.new" >> "$RECORD"
    rm -f "$RECORD.new"
    echo "unlocked $count node(s); previous modes recorded in $RECORD"
    ;;
  --revert)
    [ -f "$RECORD" ] || { echo "nothing recorded"; exit 0; }
    while read -r path mode; do
      [ -e "$path" ] || continue
      chmod "$mode" "$path" 2>/dev/null && echo "restored $path to $mode"
    done < "$RECORD"
    rm -f "$RECORD"
    ;;
  *)
    found=0
    for n in $(nodes); do
      echo "LOCKED $(stat -c %a "$n" 2>/dev/null) $n"
      found=$((found + 1))
    done
    [ "$found" = 0 ] && echo "no locked tuning nodes"
    echo "run with --apply to restore the write bit, --revert to undo"
    ;;
esac
