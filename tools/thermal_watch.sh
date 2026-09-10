# Who takes the frames away over a long session? Passive sampler -- writes nothing, owns nothing.
#
# The A/B session of 2026-09-10 showed p95 going 8 -> 149 ms and 15 -> 9 fps over 38 minutes while
# the DIE COOLED 78 -> 62 C, energy fell, and battery temp rose 27.5 -> 40.2 C monotonically. That
# signature is a power budget being withdrawn against the chassis, not the die -- the die never
# came near the 85/88 C kernel trips. Two candidates predicted it identically:
#
#   (a) an external throttle indexed to skin/battery temperature
#   (b) the engine's own allowance draining and lowering its action ceiling
#
# (b) is already refuted from the history: `budget` read 1.000 in all 297 game windows, because
# heat = clip((temp - (high - 18)) / 18, 0, 1) is zero below 67 C die. This sampler is aimed at
# (a), and at one question the same data settles for free.
#
# Three questions, three groups of columns, and they are NOT interchangeable:
#
#   CEILINGS  scaling_max_freq, gpu_max_clock, MIF max_freq, and every cooling_device cur_state.
#             The engine never writes a ceiling -- it only ever raises floors -- so any ceiling
#             that moves was moved by somebody else. This is the direct test of (a).
#
#   POWER     current_now x voltage_now, actually multiplied out. "Energy fell" was a proxy in the
#             history; here it is milliwatts. A withdrawn budget must show falling power at
#             constant requested effort, and if it does not, the theory is wrong.
#
#   REACH     what the engine ASKED for (the `last` field in adaptive_journal, its write-ahead
#             record of every node it owns) next to what sysfs ACTUALLY READS BACK. Those two
#             disagreeing is the bug class that already cost this project two dead axes, and the
#             session that prompted this run had cpu_floor at level 0 in 84% of windows -- which
#             is either a controller correctly holding still, or an axis with no reach again.
#             A missing journal line means the engine owns nothing there: also an answer, and a
#             different one from "asked for zero", so it is logged as -1 and never as 0.
#
# Cost matters here, because this samples a device whose frame delivery IS the measurement. The
# first draft forked one `cat` per column and took 10 s per sample -- 33 processes competing with
# the thing under observation. Everything readable is now read by a SINGLE `cat` over a fixed file
# list, so a sample is three forks. That fixed list is why the script refuses to start when any
# path is unreadable: a `cat` that silently skips a file shifts every column after it, and a
# quietly misaligned log is worse than no log.
#
# Do not name a helper `r` here. /system/bin/sh is mksh, which has a builtin alias r="fc -e -";
# a shell function of that name is silently shadowed and every column comes back empty.
SWITCH=/data/local/tmp/thermal_on
LOG=/data/local/tmp/thermal_watch.log
STATUS=/data/adb/m54tuner/adaptive_status
JOURNAL=/data/adb/m54tuner/adaptive_journal
P0=/sys/devices/system/cpu/cpufreq/policy0
P4=/sys/devices/system/cpu/cpufreq/policy4
G=/sys/kernel/gpu
MIF=/sys/class/devfreq/17000010.devfreq_mif
BAT=/sys/class/power_supply/battery
TZ=/sys/class/thermal

# Fixed order. The header below must match this list exactly, and both must stay in this order.
FILES="$BAT/temp $BAT/current_now $BAT/voltage_now $BAT/capacity \
$TZ/thermal_zone0/temp $TZ/thermal_zone1/temp $TZ/thermal_zone2/temp $TZ/thermal_zone6/temp \
$TZ/cooling_device0/cur_state $TZ/cooling_device1/cur_state $TZ/cooling_device2/cur_state \
$TZ/cooling_device3/cur_state $TZ/cooling_device4/cur_state \
$P0/scaling_cur_freq $P0/scaling_min_freq $P0/scaling_max_freq \
$P4/scaling_cur_freq $P4/scaling_min_freq $P4/scaling_max_freq \
$G/gpu_clock $G/gpu_min_clock $G/gpu_max_clock $G/gpu_busy $G/gpu_tmu \
$MIF/cur_freq $MIF/min_freq $MIF/max_freq"

missing=""
for f in $FILES; do [ -r "$f" ] || missing="$missing $f"; done
if [ -n "$missing" ]; then
  echo "refusing to start -- unreadable, and a short cat would misalign every column:$missing"
  exit 1
fi

# Singleton. Removing the switch file does not stop the previous instance until it wakes from
# its sleep, so a stop-push-start cycle briefly leaves two samplers appending to one log -- which
# reads as a doubled sample rate and silently interleaves two clocks. mkdir is the atomic test.
LOCK=/data/local/tmp/thermal_watch.lock
if ! mkdir "$LOCK" 2>/dev/null; then
  echo "another sampler holds $LOCK -- stop it first, or rmdir the lock if it is stale"
  exit 1
fi
trap 'rmdir "$LOCK" 2>/dev/null; echo "# stopped" >> $LOG; exit 0' EXIT INT TERM

echo "# passive thermal/reach watch. no writes. period=2s" > $LOG
echo "at_boot at_mono batt_mC batt_mA batt_uV cap z_BIG z_LITTLE z_G3D z_ac cd_isp cd_lit cd_big cd_dev cd_gpu p0_cur p0_min p0_max p4_cur p4_min p4_max g_cur g_min g_max g_busy g_tmu mif_cur mif_min mif_max own_p0min own_p4min own_gmin owned_n" >> $LOG

while [ -e "$SWITCH" ]; do
  # Three forks: the bulk read, the journal pass, the status grep. gpu_busy carries a '%' and
  # leading spaces; stripping the sign here keeps every column a bare number for the reader.
  bulk=$(cat $FILES 2>/dev/null | tr -d '%' | tr '\n' ' ')
  # One awk pass emits all three owned values plus the line count, so ownership costs one fork
  # rather than four. -1, not 0, for a node the engine does not own.
  own=$(awk -v a="$P0/scaling_min_freq" -v b="$P4/scaling_min_freq" -v c="$G/gpu_min_clock" '
    { n++ ; if ($1==a) x=$3; if ($1==b) y=$3; if ($1==c) z=$3 }
    END { printf "%s %s %s %d", (x?x:-1), (y?y:-1), (z?z:-1), n }' $JOURNAL 2>/dev/null)
  [ -z "$own" ] && own="-1 -1 -1 0"
  am=$(grep "^at=" $STATUS 2>/dev/null | cut -d= -f2)
  [ -z "$am" ] && am=-1
  echo "$(cut -d' ' -f1 /proc/uptime) $am $bulk$own" >> $LOG
  sleep 2
done
