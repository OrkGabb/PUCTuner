# Freeze the whole machine state in one file, so a bug that is FIXED BY AN ACTION can be caught
# by taking a snapshot on each side of that action and diffing them.
#
# The bug this exists for: sometimes, right after a boot, the whole device drags -- the UI feels
# like a third of the frame rate the panel can do -- and toggling the screen off and on twice
# clears it. Intermittent, so there is no point staring at a boot log hoping to be there when it
# happens; and it PERSISTS until the toggle, which is the property that makes this approach work.
# Whatever differs between "bad" and "good" is the bug, and everything identical in both is not.
#
#   sh stutter_snapshot.sh bad     <- while it is dragging, BEFORE touching the screen
#   ...toggle the screen off/on until it is smooth again...
#   sh stutter_snapshot.sh good
#   python tools/stutter_diff.py   <- pulls both and prints only what changed
#
# Take the "bad" one FIRST and do not fix it first: a snapshot of the healthy state alone proves
# nothing, because every value in it will look perfectly reasonable.
#
# The leading hypothesis when this was written is the render stack, not the CPU. This device runs
# debug.hwui.renderer=skiavk and debug.renderengine.backend=skiavkthreaded, both non-factory --
# stock here is GLES/Ganesh -- and a screen toggle forces SurfaceFlinger to tear down and rebuild
# the display and its swapchain, which is exactly the shape of a fix for a Vulkan surface that
# came up degraded. That is a hypothesis, so this file does NOT only sample the render stack: a
# snapshot that records only what you already suspect can never surprise you, and the CPU window,
# the cooling devices and the engine's ownership are all cheap to include and all capable of
# refuting it.
LABEL=${1:-snap}
OUT=/data/local/tmp/snap_$LABEL.txt
D=/data/adb/m54tuner
M=/sys/class/misc/mali0/device

exec > "$OUT" 2>&1
echo "## label=$LABEL uptime=$(cut -d' ' -f1 /proc/uptime) at=$(date)"

echo "## render props"
for p in debug.hwui.renderer debug.renderengine.backend \
         ro.surface_flinger.game_default_frame_rate_override \
         ro.surface_flinger.enable_frame_rate_override \
         debug.graphics.game_default_frame_rate.disabled \
         debug.sf.latch_unsignaled ro.surface_flinger.use_content_detection_for_refresh_rate; do
  echo "prop $p = $(getprop $p)"
done

echo "## surfaceflinger"
sf=$(pidof surfaceflinger)
echo "sf_pid = $sf"
# Process start time in jiffies since boot: proves whether SF predates the props, and whether the
# screen toggle restarted it (it should NOT -- if it did, that is itself the finding).
[ -n "$sf" ] && echo "sf_starttime_jiffies = $(awk '{print $22}' /proc/$sf/stat 2>/dev/null)"
echo "sf_backend = $(dumpsys SurfaceFlinger 2>/dev/null | grep -m1 -oE 'RE (GLES|Vulkan)')"
# The refresh rate the compositor is actually driving, and the mode the display is in. "Feels like
# 30 Hz" on a 120 Hz panel is either a real mode change or missed deadlines, and these separate them.
dumpsys SurfaceFlinger 2>/dev/null | grep -iE "refresh|activeMode|DisplayMode|FrameRate" | head -20
echo "## display modes"
dumpsys display 2>/dev/null | grep -iE "mActiveModeId|mBaseDisplayInfo|refreshRate|mUserPreferredMode" | head -20
echo "## frame timing (missed deadlines are the measurement, not the feeling)"
# NOT --timestats: that counter is off by default and must be armed ahead of time, so a snapshot
# taken the moment the bug appears returns an empty section -- which is what the first version of
# this file did. Worse, arming it only before the "good" capture would make the two sides
# incomparable. gfxinfo needs no arming and reports exactly the quantity in question: what share
# of frames missed their deadline, and which stage was late.
#
# SystemUI is the right subject. It is always running, it is drawn by the same HWUI and composited
# by the same RenderEngine as everything else, and the complaint is that the WHOLE UI drags -- so
# if the shell is janking, the problem is the stack and not one app.
#
# The counters gfxinfo prints are CUMULATIVE SINCE THE PROCESS STARTED, which makes them useless
# for comparing two moments: a "bad" capture and a "good" one taken minutes apart on one boot both
# report the same lifetime average, and the few seconds that actually differ are diluted into
# hours. So reset first, let a few seconds of real frames accumulate, and read the WINDOW. That
# turns the section into a measurement of now instead of a measurement of the whole boot.
#
# HISTOGRAM is deliberately not captured: it is 150 buckets on one line and would bury every other
# difference in the diff. The percentiles and the four "why was it late" counters carry the same
# information in a form a human can read.
#
# Both readings are kept, because each fails where the other works. The lifetime block is always
# populated but averages the whole boot; the windowed one measures NOW but reads zero when nothing
# happens to be drawing -- SystemUI only redraws on change, and a game with its own swapchain is
# nearly invisible to gfxinfo either way. When a block reports 0 frames its percentiles print as
# the 4950ms sentinel; that is "no data", not a five-second frame.
PKGS="com.android.systemui $(dumpsys activity activities 2>/dev/null | grep -m1 -oE 'topResumedActivity.*\{[^ ]* [^ ]* ([a-zA-Z0-9_.]+)/' | grep -oE '[a-zA-Z0-9_.]+/$' | tr -d /)"
gfx() { dumpsys gfxinfo "$1" 2>/dev/null | grep -iE "Total frames|Janky frames|percentile|Missed Vsync|High input|Slow UI|Slow bitmap|Slow issue" | grep -v HISTOGRAM | head -13; }
for pkg in $PKGS; do echo "-- gfxinfo $pkg (since process start)"; gfx "$pkg"; done
echo "-- KEEP THE UI MOVING FOR THE NEXT 5 SECONDS (swipe the shade) or the window below reads 0"
for pkg in $PKGS; do dumpsys gfxinfo "$pkg" reset >/dev/null 2>&1; done
sleep 5
for pkg in $PKGS; do echo "-- gfxinfo $pkg (5 s window)"; gfx "$pkg"; done

echo "## cpufreq"
# One key per field on purpose. Emitting the whole policy as a single "cur=.. min=.. gov=.." line
# makes every field differ whenever the frequency moved, which hides a changed GOVERNOR or a
# lowered CEILING inside a string that was going to differ anyway.
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  [ -d "$p" ] || continue
  t=$(basename $p)
  echo "$t.cur = $(cat $p/scaling_cur_freq)"
  echo "$t.min = $(cat $p/scaling_min_freq)"
  echo "$t.max = $(cat $p/scaling_max_freq)"
  echo "$t.hwmax = $(cat $p/cpuinfo_max_freq)"
  echo "$t.gov = $(cat $p/scaling_governor)"
done
echo "online = $(cat /sys/devices/system/cpu/online)"
echo "pelt = $(cat /proc/sys/kernel/sched_pelt_multiplier 2>/dev/null)"

echo "## thermal -- a stuck cooling device caps the cluster without failing any write"
for z in /sys/class/thermal/thermal_zone*; do
  echo "zone $(cat $z/type 2>/dev/null) = $(cat $z/temp 2>/dev/null)"
done
for c in /sys/class/thermal/cooling_device*; do
  echo "cool $(cat $c/type 2>/dev/null) cur=$(cat $c/cur_state 2>/dev/null) max=$(cat $c/max_state 2>/dev/null)"
done

echo "## gpu"
for f in gpu_clock gpu_min_clock gpu_max_clock gpu_busy gpu_governor gpu_tmu; do
  echo "$f = $(cat /sys/kernel/gpu/$f 2>/dev/null)"
done
echo "power_policy = $(cat $M/power_policy 2>/dev/null)"

echo "## devfreq"
for d in /sys/class/devfreq/*; do
  echo "$(basename $d) cur=$(cat $d/cur_freq 2>/dev/null) min=$(cat $d/min_freq 2>/dev/null) max=$(cat $d/max_freq 2>/dev/null)"
done

echo "## engine -- is it even acting, and does it own anything right now"
grep -E "^(mode|reason|regime|samples|windows|confidence|probe|locked_nodes|owner|ceiling|budget|cadence)=" $D/adaptive_status 2>/dev/null
echo "journal:"
cat $D/adaptive_journal 2>/dev/null

echo "## module's own verdict on its last apply"
grep -iE "warn|fail|clamp" $D/result 2>/dev/null || echo "(no warn/fail/clamped)"
tail -15 $D/m54tuner.log 2>/dev/null

echo "## graphics driver complaints since boot"
# Aggregated, not raw. Every raw line carries its own timestamp and pid, so a diff of two captures
# treats each as a unique key and drowns the real findings in hundreds of one-sided lines -- which
# is exactly what the first version did. Stripping the prefix and counting occurrences turns the
# section into "which complaints, and how many", which is both comparable and shorter.
logcat -d -t 600 2>/dev/null |
  grep -iE "vulkan|mali|gralloc|swapchain|VK_ERROR|composer|surfaceflinger" |
  sed -E 's/^[0-9-]+ +[0-9:.]+ +[0-9]+ +[0-9]+ +//; s/[0-9]{4,}/N/g' |
  sort | uniq -c | sort -rn | head -25 |
  sed -E 's/^ *([0-9]+) (.*)$/ = /' 

echo "## done"
