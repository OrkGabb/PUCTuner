#!/system/bin/sh
# M54 Tuner module v2 — post-fs-data (runs EARLY, before SurfaceFlinger/zygote on a permanently
# rooted kernel). Two jobs belong here and nowhere else:
#   1. snapshot the TRUE factory state, while nothing has touched it yet — this is what "Balanced"
#      restores, and capturing it later would restore a tuned value as if it were stock;
#   2. set the boot props, so on a real boot SurfaceFlinger and every app read them naturally and
#      NOTHING has to be restarted. (On this device's temp-root KSU this script runs late, so
#      service.sh does the one restart per boot session instead.)

MODDIR=${0%/*}
. "$MODDIR/scripts/lib.sh"
mkdir -p "$M54_DIR"

capture_factory() {
  local dest="$FACTORY" tmp="$FACTORY.tmp.$$"
  local FACTORY="$tmp"
  : > "$FACTORY"
  echo "schema=3" >> "$FACTORY"
  if [ "$(getprop sys.boot_completed)" = 1 ]; then echo 'source=session' >> "$FACTORY"
  else echo 'source=boot' >> "$FACTORY"; fi
  echo "fingerprint=$(getprop ro.build.fingerprint)" >> "$FACTORY"
  echo "kernel=$(uname -r)" >> "$FACTORY"
  echo "captured_boot=$(boot_id)" >> "$FACTORY"
  echo "captured_uptime=$(uptime_s)" >> "$FACTORY"
  for p in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$p" ] || continue
    tag=$(basename "$p")
    echo "cpu_${tag}_min=$(cat "$p/scaling_min_freq" 2>/dev/null)" >> "$FACTORY"
    echo "cpu_${tag}_max=$(cat "$p/scaling_max_freq" 2>/dev/null)" >> "$FACTORY"
    echo "cpu_${tag}_gov=$(cat "$p/scaling_governor" 2>/dev/null)" >> "$FACTORY"
  done
  if [ -r /sys/kernel/gpu/gpu_min_clock ]; then
    M=/sys/class/misc/mali0/device
    echo "gpu_min=$(cat /sys/kernel/gpu/gpu_min_clock)" >> "$FACTORY"
    echo "gpu_max=$(cat /sys/kernel/gpu/gpu_max_clock)" >> "$FACTORY"
    echo "gpu_gov=$(cat /sys/kernel/gpu/gpu_governor | tr -d ' ')" >> "$FACTORY"
    echo "gpu_hs_load=$(cat "$M/highspeed_load" 2>/dev/null)" >> "$FACTORY"
    echo "gpu_hs_clock=$(cat "$M/highspeed_clock" 2>/dev/null)" >> "$FACTORY"
    echo "gpu_hs_delay=$(cat "$M/highspeed_delay" 2>/dev/null)" >> "$FACTORY"
    # power_policy prints the whole list with the active one in brackets; store just the active one.
    echo "gpu_power_policy=$(cat "$M/power_policy" 2>/dev/null | tr ' ' '
' | grep '^\[' | tr -d '[]')" >> "$FACTORY"
    echo "gpu_cl_boost=$(cat /sys/kernel/gpu/gpu_cl_boost_disable 2>/dev/null)" >> "$FACTORY"
    echo "gpu_dvfs_period=$(cat "$M/dvfs_period" 2>/dev/null)" >> "$FACTORY"
    echo "gpu_polling_speed=$(cat "$M/polling_speed" 2>/dev/null)" >> "$FACTORY"
    echo "gpu_js_period=$(cat "$M/js_scheduling_period" 2>/dev/null)" >> "$FACTORY"
    # down_staycount prints one line per clock level ("Clock 650000 - 3"); store the two the
    # profiles touch so a restore puts the exact factory ladder back.
    for lvl in 650000 552000; do
      echo "gpu_stay_$lvl=$(grep "Clock $lvl " "$M/down_staycount" 2>/dev/null | awk '{print $4}')" >> "$FACTORY"
    done
  fi
  d=/sys/class/devfreq/17000010.devfreq_mif
  [ -d "$d" ] && echo "mif_min=$(cat "$d/min_freq")" >> "$FACTORY"
  n=/sys/class/devfreq/17000020.devfreq_int
  [ -d "$n" ] && echo "int_min=$(cat "$n/min_freq")" >> "$FACTORY"
  dsp=/sys/class/devfreq/17000040.devfreq_disp
  [ -d "$dsp" ] && echo "disp_min=$(cat "$dsp/min_freq")" >> "$FACTORY"
  for b in /sys/block/sd* /sys/block/mmcblk* /sys/block/nvme*; do
    [ -e "$b/queue/scheduler" ] || continue
    name=$(basename "$b")
    cur=$(cat "$b/queue/scheduler" | tr ' ' '\n' | grep '^\[' | tr -d '[]')
    [ -n "$cur" ] && echo "io_$name=$cur" >> "$FACTORY"
    echo "io_${name}_nr=$(cat "$b/queue/nr_requests" 2>/dev/null)" >> "$FACTORY"
    echo "io_${name}_ra=$(cat "$b/queue/read_ahead_kb" 2>/dev/null)" >> "$FACTORY"
    # These two live on the queue and survive a scheduler change, so they need restoring by value.
    echo "io_${name}_rqaff=$(cat "$b/queue/rq_affinity" 2>/dev/null)" >> "$FACTORY"
    echo "io_${name}_stats=$(cat "$b/queue/iostats" 2>/dev/null)" >> "$FACTORY"
  done
  for u in /sys/devices/platform/*.ufs; do
    [ -d "$u" ] || continue
    echo "ufs_clkgate=$(cat "$u/clkgate_enable" 2>/dev/null)" >> "$FACTORY"
    echo "ufs_clkgate_delay=$(cat "$u/clkgate_delay_ms" 2>/dev/null)" >> "$FACTORY"
    echo "ufs_hibern8=$(cat "$u/auto_hibern8" 2>/dev/null)" >> "$FACTORY"
    echo "ufs_rpm_lvl=$(cat "$u/rpm_lvl" 2>/dev/null)" >> "$FACTORY"
    break
  done
  [ -e /sys/kernel/ufs_perf_0/pm_qos_cluster1 ] && \
    echo "ufs_qos_cluster1=$(cat /sys/kernel/ufs_perf_0/pm_qos_cluster1)" >> "$FACTORY"
  for f in /sys/fs/f2fs/*; do
    [ -d "$f" ] || continue
    echo "f2fs_gc_min_sleep=$(cat "$f/gc_min_sleep_time" 2>/dev/null)" >> "$FACTORY"
    echo "f2fs_ipu_policy=$(cat "$f/ipu_policy" 2>/dev/null)" >> "$FACTORY"
    break
  done
  [ -r /proc/sys/kernel/sched_pelt_multiplier ] && \
    echo "pelt=$(cat /proc/sys/kernel/sched_pelt_multiplier)" >> "$FACTORY"
  for key in swappiness page-cluster vfs_cache_pressure stat_interval; do
    [ -r "/proc/sys/vm/$key" ] && echo "vm_${key}=$(cat "/proc/sys/vm/$key")" >> "$FACTORY"
  done
  for c in /sys/devices/system/cpu/cpu[0-9]*; do
    [ -r "$c/cpuidle/state1/disable" ] || continue
    echo "cpuidle_$(basename "$c")=$(cat "$c/cpuidle/state1/disable")" >> "$FACTORY"
  done
  for z in /sys/class/thermal/thermal_zone*; do
    [ -r "$z/mode" ] || continue
    echo "thermal_$(basename "$z")=$(cat "$z/mode")" >> "$FACTORY"
  done
  echo "complete=1" >> "$FACTORY"
  chmod 0600 "$FACTORY" 2>/dev/null
  mv -f "$FACTORY" "$dest"
  log "factory snapshot captured"
}

# Capture ONCE (factory values are constant per device), and refuse an obviously-tuned capture:
# re-running the module mid-session on an already-tuned device would poison the snapshot, which is
# exactly how "Balanced" once ended up restoring modified values. First activation must be on a
# clean boot.
factory_valid() {
  [ "$(grep '^schema=' "$FACTORY" 2>/dev/null | cut -d= -f2)" = 3 ] &&
  [ "$(grep '^complete=' "$FACTORY" 2>/dev/null | cut -d= -f2)" = 1 ] &&
  [ "$(grep '^fingerprint=' "$FACTORY" 2>/dev/null | cut -d= -f2-)" = "$(getprop ro.build.fingerprint)" ] &&
  [ "$(grep '^kernel=' "$FACTORY" 2>/dev/null | cut -d= -f2-)" = "$(uname -r)" ]
}

if ! factory_valid; then
  p0=/sys/devices/system/cpu/cpufreq/policy0
  cur=$(cat "$p0/scaling_min_freq" 2>/dev/null)
  hw=$(cat "$p0/cpuinfo_min_freq" 2>/dev/null)
  # The CPU floor alone was not enough: a device with a stock CPU window but a Game-tuned GPU would
  # record highspeed_load 85 as "factory", and Balanceado would then restore the tuned value.
  gcur=$(cat /sys/kernel/gpu/gpu_min_clock 2>/dev/null)
  ghw=$(cat /sys/kernel/gpu/gpu_freq_table 2>/dev/null | tr ' ' '
' | grep . | sort -n | head -1)
  # The floor was still not enough. Another module capped the big cluster's CEILING at
  # 2208000 of 2400000 while leaving every floor stock, so this guard passed and recorded the
  # cap as factory -- after which Balanceado restored that cap on every apply, for as long as
  # the snapshot lived. A ceiling below what cpuinfo reports is never a factory value here.
  capped=
  for p in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$p" ] || continue
    pmax=$(cat "$p/scaling_max_freq" 2>/dev/null)
    phw=$(cat "$p/cpuinfo_max_freq" 2>/dev/null)
    [ -n "$pmax" ] && [ -n "$phw" ] && [ "$pmax" -lt "$phw" ] 2>/dev/null &&
      capped="$capped $(basename "$p")($pmax<$phw)"
  done
  if [ "$cur" = "$hw" ] && { [ -z "$gcur" ] || [ "$gcur" = "$ghw" ]; } && [ -z "$capped" ]; then
    capture_factory
  elif [ -n "$capped" ]; then
    log "factory NOT captured: a CPU ceiling is below hardware:$capped; keeping prior snapshot"
  else
    log "factory NOT captured/refreshed: device looks tuned (min=$cur hw=$hw); keeping prior snapshot"
  fi
fi

# "--boot" claims nothing has read these props yet. That is only true at a REAL boot: with LKM
# temp-root this script runs after the UI is already up, and sys.boot_completed is the honest
# discriminator. Getting this wrong silently cleared the pending-soft-reboot flag.
[ "${1:-}" = --capture-only ] && exit 0
BOOTARG=--boot
[ "$(getprop sys.boot_completed)" = "1" ] && BOOTARG=""

# Props only — no restarts here when we really are at boot.
sh "$MODDIR/scripts/apply_render.sh" $BOOTARG >/dev/null 2>&1
sh "$MODDIR/scripts/apply_art.sh" $BOOTARG >/dev/null 2>&1
