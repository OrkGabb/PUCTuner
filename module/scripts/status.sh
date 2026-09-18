#!/system/bin/sh
# M54 Tuner module v2 — one-shot live state dump for the app (single root round trip).
# Everything the UI shows about the device comes from here, so the panel reflects the KERNEL,
# not what the app believes it wrote. Output is flat key=value.

DIR=$(dirname "$0")
. "$DIR/lib.sh"

# --full pulls in the Samsung/MARs section below, which is 9 `content query`/`settings get`
# subprocess spawns (each forks a JVM — hundreds of ms apiece). That is fine once, but doing it on
# every 3-second poll (and on every app launch) is what made the app feel slow for information that
# barely ever changes. Default is fast; the app asks for --full only right after the screen is
# already up, and on an explicit refresh or a Samsung/MARs toggle.
FULL=0
[ "$1" = "--full" ] && FULL=1

echo "module=1"
pid_record_alive "$M54_DIR/adaptive_pid" m54-adaptive && echo 'adaptive.running=1' || echo 'adaptive.running=0'
if [ -f "$M54_DIR/adaptive_status" ]; then
  sed 's/^/adaptive./' "$M54_DIR/adaptive_status"
fi
[ -f "$FACTORY" ] && echo "factory=1" || echo "factory=0"
# Computed, never read from the flag files: those are a cache that only an apply refreshes, so a
# SurfaceFlinger that restarted on its own left the banner up forever.
pending_for "$M54_DIR/render_state" sf_at surfaceflinger && echo "pending_sf=1" || echo "pending_sf=0"
pending_for "$M54_DIR/art_state" at zygote64 zygote && echo "pending_soft=1" || echo "pending_soft=0"

# ---- CPU ----
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  [ -d "$p" ] || continue
  t=$(basename "$p")
  echo "cpu.$t.cur=$(cat "$p/scaling_cur_freq" 2>/dev/null)"
  echo "cpu.$t.min=$(cat "$p/scaling_min_freq" 2>/dev/null)"
  echo "cpu.$t.max=$(cat "$p/scaling_max_freq" 2>/dev/null)"
  echo "cpu.$t.gov=$(cat "$p/scaling_governor" 2>/dev/null)"
  echo "cpu.$t.hwmin=$(cat "$p/cpuinfo_min_freq" 2>/dev/null)"
  echo "cpu.$t.hwmax=$(cat "$p/cpuinfo_max_freq" 2>/dev/null)"
  echo "cpu.$t.govs=$(cat "$p/scaling_available_governors" 2>/dev/null)"
done

# ---- GPU ----
if [ -r /sys/kernel/gpu/gpu_freq_table ]; then
  M=/sys/class/misc/mali0/device
  echo "gpu.table=$(cat /sys/kernel/gpu/gpu_freq_table)"
  echo "gpu.min=$(cat /sys/kernel/gpu/gpu_min_clock 2>/dev/null)"
  echo "gpu.max=$(cat /sys/kernel/gpu/gpu_max_clock 2>/dev/null)"
  # gpu_governor prints ONLY the active one; the choices live in gpu_available_governor. Emit them
  # the way the driver formats power_policy — full list, active one in brackets — so the app can
  # offer every governor instead of just the one already selected.
  gov=$(cat /sys/kernel/gpu/gpu_governor 2>/dev/null | tr -d ' \n')
  avail=$(cat /sys/kernel/gpu/gpu_available_governor 2>/dev/null | tr '\n' ' ')
  govline=""
  for g in $avail; do
    if [ "$g" = "$gov" ]; then govline="$govline [$g]"; else govline="$govline $g"; fi
  done
  [ -z "$avail" ] && govline="[$gov]"
  echo "gpu.gov=$govline"
  echo "gpu.cur=$(cat /sys/kernel/gpu/gpu_clock 2>/dev/null)"
  echo "gpu.busy=$(cat /sys/kernel/gpu/gpu_busy 2>/dev/null | tr -d ' %')"
  echo "gpu.hs_load=$(cat "$M/highspeed_load" 2>/dev/null)"
  echo "gpu.hs_clock=$(cat "$M/highspeed_clock" 2>/dev/null)"
  echo "gpu.hs_delay=$(cat "$M/highspeed_delay" 2>/dev/null)"
  echo "gpu.power_policy=$(cat "$M/power_policy" 2>/dev/null | tr '\n' ' ')"
  echo "gpu.cl_boost_disable=$(cat /sys/kernel/gpu/gpu_cl_boost_disable 2>/dev/null)"
  echo "gpu.dvfs_period=$(cat "$M/dvfs_period" 2>/dev/null)"
  echo "gpu.polling_speed=$(cat "$M/polling_speed" 2>/dev/null)"
  echo "gpu.js_period=$(cat "$M/js_scheduling_period" 2>/dev/null)"
  echo "gpu.stay_650=$(grep 'Clock 650000 ' "$M/down_staycount" 2>/dev/null | awk '{print $4}')"
  echo "gpu.stay_552=$(grep 'Clock 552000 ' "$M/down_staycount" 2>/dev/null | awk '{print $4}')"
fi

# ---- MIF / IO / sched ----
d=/sys/class/devfreq/17000010.devfreq_mif
if [ -d "$d" ]; then
  echo "mif.cur=$(cat "$d/cur_freq" 2>/dev/null)"
  echo "mif.min=$(cat "$d/min_freq" 2>/dev/null)"
  echo "mif.max=$(cat "$d/max_freq" 2>/dev/null)"
  echo "mif.table=$(cat "$d/available_frequencies" 2>/dev/null)"
  echo "mif.gov=$(cat "$d/governor" 2>/dev/null)"
  echo "mif.load=$(cat "$d/interactive/current_target_load" 2>/dev/null)"
fi
n=/sys/class/devfreq/17000020.devfreq_int
if [ -d "$n" ]; then
  echo "int.cur=$(cat "$n/cur_freq" 2>/dev/null)"
  echo "int.min=$(cat "$n/min_freq" 2>/dev/null)"
  echo "int.max=$(cat "$n/max_freq" 2>/dev/null)"
fi
dsp=/sys/class/devfreq/17000040.devfreq_disp
if [ -d "$dsp" ]; then
  echo "disp.cur=$(cat "$dsp/cur_freq" 2>/dev/null)"
  echo "disp.min=$(cat "$dsp/min_freq" 2>/dev/null)"
fi
for b in /sys/block/sd* /sys/block/mmcblk* /sys/block/nvme*; do
  [ -e "$b/queue/scheduler" ] || continue
  # Skip the 0-byte UFS boot LUNs: the app was listing five identical rows for
  # devices nothing uses.
  # The UFS boot LUNs are 4-20 MB while the data LUN is 244 GB, so "size is not zero" never
  # filtered anything — they are small, not empty. 2 GB in 512-byte sectors separates them cleanly.
  sz=$(cat "$b/size" 2>/dev/null)
  case "$sz" in ''|*[!0-9]*) continue;; esac
  [ "$sz" -lt 4194304 ] && continue
  n=$(basename "$b")
  echo "io.$n=$(cat "$b/queue/scheduler" | tr ' ' '\n' | grep '^\[' | tr -d '[]')"
done
# Elevator tunables of the data LUN, so the app can show what the profile actually did instead of
# leaving the I/O tuning invisible.
for b in /sys/block/sd* /sys/block/mmcblk* /sys/block/nvme*; do
  [ -e "$b/queue/scheduler" ] || continue
  # The UFS boot LUNs are 4-20 MB while the data LUN is 244 GB, so "size is not zero" never
  # filtered anything — they are small, not empty. 2 GB in 512-byte sectors separates them cleanly.
  sz=$(cat "$b/size" 2>/dev/null)
  case "$sz" in ''|*[!0-9]*) continue;; esac
  [ "$sz" -lt 4194304 ] && continue
  echo "ioq.dev=$(basename "$b")"
  echo "ioq.nr=$(cat "$b/queue/nr_requests" 2>/dev/null)"
  echo "ioq.ra=$(cat "$b/queue/read_ahead_kb" 2>/dev/null)"
  echo "ioq.rq_affinity=$(cat "$b/queue/rq_affinity" 2>/dev/null)"
  echo "ioq.iostats=$(cat "$b/queue/iostats" 2>/dev/null)"
  for k in read_expire write_expire writes_starved fifo_batch front_merges async_depth            prio_aging_expire max_write_starvation slice_idle read_lat_nsec write_lat_nsec; do
    [ -f "$b/queue/iosched/$k" ] && echo "ioq.$k=$(cat "$b/queue/iosched/$k" 2>/dev/null)"
  done
  break
done
for u in /sys/devices/platform/*.ufs; do
  [ -d "$u" ] || continue
  echo "ufs.clkgate=$(cat "$u/clkgate_enable" 2>/dev/null)"
  echo "ufs.clkgate_delay=$(cat "$u/clkgate_delay_ms" 2>/dev/null)"
  echo "ufs.hibern8=$(cat "$u/auto_hibern8" 2>/dev/null)"
  echo "ufs.rpm_lvl=$(cat "$u/rpm_lvl" 2>/dev/null)"
  echo "ufs.wb_on=$(cat "$u/wb_on" 2>/dev/null)"
  break
done
echo "ufs.qos_big=$(cat /sys/kernel/ufs_perf_0/pm_qos_cluster1 2>/dev/null)"
for f in /sys/fs/f2fs/*; do
  [ -d "$f" ] || continue
  echo "f2fs.dev=$(basename "$f")"
  echo "f2fs.gc_urgent=$(cat "$f/gc_urgent" 2>/dev/null)"
  echo "f2fs.gc_min_sleep=$(cat "$f/gc_min_sleep_time" 2>/dev/null)"
  echo "f2fs.ipu_policy=$(cat "$f/ipu_policy" 2>/dev/null)"
  echo "f2fs.min_ipu_util=$(cat "$f/min_ipu_util" 2>/dev/null)"
  break
done
# Is the background-protection watcher alive? Reported from the PID, not from the config, so a
# watcher that died shows as off instead of as "enabled".
ka=0
pid_record_alive "$M54_DIR/keepalive_pid" keepalive.sh && ka=1
echo "keepalive.running=$ka"
pid_record_alive "$M54_DIR/thermal_guard_pid" thermal_guard.sh && echo "thermal.guard=1" || echo "thermal.guard=0"
echo "thermal.guard_state=$(cat "$M54_DIR/thermal_guard_state" 2>/dev/null)"
echo "psi.memory=$(tr '\n' ';' < /proc/pressure/memory 2>/dev/null)"
if [ "$FULL" = 1 ]; then
  # Samsung's own limiter switches, read straight from the settings tables.
  echo "sam.low_heat=$(settings get global sem_low_heat_mode 2>/dev/null)"
  echo "sam.bg_ai=$(settings get secure BG_AD_RESTRICTION_BY_AI 2>/dev/null)"
  echo "sam.restricted_perf=$(settings get global restricted_device_performance 2>/dev/null)"
  echo "sam.cpu_resp=$(settings get global sem_enhanced_cpu_responsiveness 2>/dev/null)"
  # Samsung's MARs background-restriction system, read from its own providers.
  echo "sam.spcm=$(content query --uri content://com.samsung.android.sm/settings --where "key='spcm_switch'" 2>/dev/null | grep -o 'value=[01]' | cut -d= -f2)"
  echo "sam.mars_p1=$(content query --uri content://com.samsung.android.sm.mars/MARs_Policy --where 'policyNum=1' 2>/dev/null | grep -o 'isPolicyEnabled=[01]' | cut -d= -f2)"
  echo "sam.mars_p8=$(content query --uri content://com.samsung.android.sm.mars/MARs_Policy --where 'policyNum=8' 2>/dev/null | grep -o 'isPolicyEnabled=[01]' | cut -d= -f2)"
  echo "sam.excluded=$(content query --uri content://com.samsung.android.sm.mars/MARs_ExcludeTarget 2>/dev/null | grep -c 'packageName=')"
  echo "sam.adaptive_ps=$(settings get global adaptive_power_saving_setting 2>/dev/null)"
fi
echo "cpuidle.deep_disabled=$(cat /sys/devices/system/cpu/cpu0/cpuidle/state1/disable 2>/dev/null)"
echo "cpuidle.gov=$(cat /sys/devices/system/cpu/cpuidle/current_governor 2>/dev/null)"
echo "vm.cache_pressure=$(cat /proc/sys/vm/vfs_cache_pressure 2>/dev/null)"
echo "pelt=$(cat /proc/sys/kernel/sched_pelt_multiplier 2>/dev/null)"
echo "eas=$(cat /proc/sys/kernel/sched_energy_aware 2>/dev/null)"
echo "bench.sched=$(cat "$M54_DIR/bench_sched_state" 2>/dev/null)"

# ---- memory ----
echo "zram.algo=$(cat /sys/block/zram0/comp_algorithm 2>/dev/null | tr ' ' '\n' | grep '^\[' | tr -d '[]')"
echo "zram.algos=$(cat /sys/block/zram0/comp_algorithm 2>/dev/null | tr -d '[]')"
echo "zram.disksize=$(cat /sys/block/zram0/disksize 2>/dev/null)"
echo "swappiness=$(cat /proc/sys/vm/swappiness 2>/dev/null)"
echo "mem.avail=$(grep MemAvailable /proc/meminfo | awk '{print $2}')"

# ---- thermal: compute zones only (BIG/LITTLE/G3D). Taking the max across ALL zones caught the
# modem and charger sensors and reported absurd peaks while the phone was cool. ----
max=0
for z in /sys/class/thermal/thermal_zone*; do
  ty=$(cat "$z/type" 2>/dev/null)
  case "$ty" in BIG|LITTLE|G3D) ;; *) continue;; esac
  v=$(cat "$z/temp" 2>/dev/null)
  case "$v" in ''|*[!0-9]*) continue;; esac
  echo "temp.$ty=$v"
  [ "$v" -gt "$max" ] && max=$v
done
echo "temp=$max"
tzmode=moderate
for z in /sys/class/thermal/thermal_zone*; do
  ty=$(cat "$z/type" 2>/dev/null)
  case "$ty" in BIG|LITTLE|G3D) ;; *) continue;; esac
  [ "$(cat "$z/mode" 2>/dev/null)" = "disabled" ] && tzmode=aggressive
done
echo "thermal.mode=$tzmode"

# ---- render props actually live right now ----
echo "prop.hwui_renderer=$(getprop debug.hwui.renderer)"
echo "prop.re_backend=$(getprop debug.renderengine.backend)"
echo "prop.fps_override=$(getprop ro.surface_flinger.game_default_frame_rate_override)"
# Which RenderEngine SurfaceFlinger actually built — the prop can be empty while the backend is
# very much decided, so read the truth from the running service.
echo "prop.re_live=$(dumpsys SurfaceFlinger 2>/dev/null | grep -m1 -oE 'RE (GLES|Vulkan)[^-]*' | tr -d '\n')"
echo "prop.fps_feature_disabled=$(getprop debug.graphics.game_default_frame_rate.disabled)"
echo "prop.usap=$(getprop dalvik.vm.usap_pool_enabled)"
echo "prop.dex2oat_cpuset=$(getprop dalvik.vm.dex2oat-cpu-set)"
echo "prop.dex2oat_threads=$(getprop dalvik.vm.dex2oat-threads)"
echo "prop.heap_growth=$(getprop dalvik.vm.heapgrowthlimit)"
# Uptime of the zygote process: how long ago the last soft reboot (or boot) happened.
echo "zygote.pid=$(pidof zygote64 2>/dev/null || pidof zygote 2>/dev/null)"

# ---- GOS ----
PKG=com.samsung.android.game.gos
if pm list packages --user 0 "$PKG" 2>/dev/null | grep -q "$PKG"; then
  if pm list packages -e --user 0 "$PKG" 2>/dev/null | grep -q "$PKG"; then echo "gos=enabled"; else echo "gos=disabled"; fi
else
  echo "gos=absent"
fi
