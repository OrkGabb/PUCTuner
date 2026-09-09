#!/system/bin/sh
# M54 Tuner module v2 — live tuning pass (tier "live": nothing restarts, everything is
# effective the moment it is written). Reads $CONFIG for the user's choices and $FACTORY for
# the true stock values captured at a clean boot. Every lever reports into $RESULT.
# Usage: sh apply_profile.sh

DIR=$(dirname "$0")
. "$DIR/lib.sh"

# Normal operation always starts from the same recorded baseline. Legacy profile config must
# not select I/O/UFS/F2FS/VM presets behind the autonomous learner's back. Only explicit offline
# experiment invocations can override this baseline through their environment.
PROFILE=balanced
case "${M54_PROFILE_OVERRIDE:-}" in game|balanced|powersave|none) PROFILE="$M54_PROFILE_OVERRIDE";; esac
THERMAL=$(read_cfg thermal moderate)
GOS=$(read_cfg gos untouched)
FASRS=$(read_cfg fasrs_companion auto)
case "$PROFILE" in game|balanced|powersave|none) ;; *) PROFILE=none;; esac
case "$THERMAL" in moderate|aggressive) ;; *) THERMAL=moderate;; esac
case "$GOS" in untouched|disabled|enabled) ;; *) GOS=untouched;; esac

COMPANION=0
# A live external DVFS owner is authoritative even if the UI preference was switched off.
has_fasrs && COMPANION=1
ADAPTIVE=0
[ "$(read_cfg adaptive_mode active)" = active ] && [ -x "${DIR%/*}/bin/m54-adaptive" ] && ADAPTIVE=1

result_begin "profile:$PROFILE"
if ! sh "$DIR/adaptive_stop.sh"; then rep adaptive.stop fail busy stopped; result_end; exit 1; fi
if ! begin_apply_lock; then rep module.lock fail busy profile; result_end; exit 1; fi
trap 'end_apply_lock' EXIT INT TERM
log "apply profile=$PROFILE thermal=$THERMAL gos=$GOS companion=$COMPANION"

fget() { grep -E "^$1=" "$FACTORY" 2>/dev/null | tail -1 | cut -d= -f2-; }
# ovr <cfg-key> — user override from the config; empty means the profile preset wins.
ovr() {
  local v
  case "$1" in
    gpu_dvfs_period) [ -n "${M54_BENCH_DVFS:-}" ] && { echo "$M54_BENCH_DVFS"; return; } ;;
    gpu_polling_speed) [ -n "${M54_BENCH_POLLING:-}" ] && { echo "$M54_BENCH_POLLING"; return; } ;;
    gpu_js_period) [ -n "${M54_BENCH_JS:-}" ] && { echo "$M54_BENCH_JS"; return; } ;;
  esac
  v=$(read_cfg "$1" "")
  case "$1" in
    gpu_min|gpu_max|gpu_hs_load|gpu_hs_clock|gpu_hs_delay|gpu_cl_boost|gpu_dvfs_period|gpu_polling_speed|gpu_js_period|mif_min|int_min|ufs_rpm_lvl|disp_min|f2fs_ipu)
      case "$v" in ''|*[!0-9]*) echo ""; return;; esac ;;
    cpu_gov|io_sched|gpu_gov|gpu_power_policy)
      echo "$v" | grep -Eq '^[A-Za-z0-9_-]*$' || { echo ""; return; } ;;
  esac
  echo "$v"
}

# ================= CPU =================
# Companion mode: fas-rs owns the CPU DVFS decisions (frame-aware eBPF beats a static floor), so we
# only restore the factory window and never fight it. The governor stays ours either way — fas-rs
# drives frequencies through the same governor, it does not replace it.
apply_cpu() {
  [ "$COMPANION" = 1 ] && { rep cpu.owner skip fas-rs fas-rs; return; }
  local gov_ovr
  gov_ovr=$(ovr cpu_gov)
  for p in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$p" ] || continue
    local tag hwmin hwmax table min max gov
    tag=$(basename "$p")
    hwmin=$(cat "$p/cpuinfo_min_freq")
    hwmax=$(cat "$p/cpuinfo_max_freq")
    table=$(cat "$p/scaling_available_frequencies" 2>/dev/null)
    if [ "$COMPANION" = 1 ]; then
      min=""; max=""      # fas-rs owns the window — see the skip below
    else
      case "$PROFILE" in
        game)
          max=$hwmax
          min=$(nearest "$(ratio_freq "$hwmin" "$hwmax" 60)" "$table")
          ;;
        powersave)
          # NO frequency cap. Capping the CPU at 55% of its range made the phone feel broken for the
          # sake of a saving that mostly is not there: for a fixed amount of work, finishing at a
          # high clock and going idle usually costs less than crawling at a low one (race to idle).
          # Economia now saves where it actually pays — the GPU ceiling, idle states, I/O and the
          # governor — and leaves the CPU window at factory.
          min=$hwmin
          max=$(fget "cpu_${tag}_max"); [ -z "$max" ] && max=$hwmax
          ;;
        *)
          min=$(fget "cpu_${tag}_min"); [ -z "$min" ] && min=$hwmin
          max=$(fget "cpu_${tag}_max"); [ -z "$max" ] && max=$hwmax
          ;;
      esac
    fi
    gov="$gov_ovr"
    if [ -z "$gov" ]; then
      gov=$(fget "cpu_${tag}_gov"); [ -z "$gov" ] && gov=energy_aware
    fi
    if grep -qw "$gov" "$p/scaling_available_governors" 2>/dev/null; then
      apply_node "cpu.$tag.gov" "$p/scaling_governor" "$gov"
    fi
    if [ "$COMPANION" = 1 ]; then
      # Hand the window BACK to factory before letting fas-rs drive it. Simply skipping the write
      # was wrong: a floor we had written while fas-rs was down stayed behind, so entering companion
      # mode left our old Game floor pinned under a scheduler that thought it owned the window.
      # Verified loosely (ceiling/atleast) because fas-rs re-caps within a second — that is its job,
      # not a failed write.
      local fmin fmax
      fmin=$(fget "cpu_${tag}_min"); [ -z "$fmin" ] && fmin=$hwmin
      fmax=$(fget "cpu_${tag}_max"); [ -z "$fmax" ] && fmax=$hwmax
      write_silent "$p/scaling_min_freq" "$hwmin"
      apply_node "cpu.$tag.max" "$p/scaling_max_freq" "$fmax" ceiling
      apply_node "cpu.$tag.min" "$p/scaling_min_freq" "$fmin" atleast
      rep "cpu.$tag.owner" skip fas-rs "-"
      continue
    fi
    write_silent "$p/scaling_min_freq" "$hwmin"     # drop the floor first so min<=max always holds
    # "ceiling" whenever we are RAISING the window, not only at the hardware maximum. A thermal
    # cooling device can hold the cluster below what we ask (seen live: asked 2112000, got 1824000
    # with thermal-dev-0 at state 2) — the write did not fail, the kernel is simply capping, and
    # reporting that as our failure is wrong. Only a deliberate LOWER cap is checked exactly.
    if [ "$max" -ge "$(cat "$p/scaling_max_freq")" ] 2>/dev/null; then
      apply_node "cpu.$tag.max" "$p/scaling_max_freq" "$max" ceiling
    else
      apply_node "cpu.$tag.max" "$p/scaling_max_freq" "$max"
    fi
    # scaling_min_freq reads back HIGHER than requested while a top-app QoS boost is live (Samsung
    # raises it transiently) — that is not a failed write, so verify it with "atleast".
    apply_node "cpu.$tag.min" "$p/scaling_min_freq" "$min" atleast
  done
}

# ================= GPU (/sys/kernel/gpu + mali0 KMD) =================
# The GPU is where this device has the most unused headroom, so every knob the driver accepts is
# exposed: governor, clock window, the highspeed ramp triple, the power policy and the CL boost.
apply_gpu() {
  [ "$COMPANION" = 1 ] && { rep gpu.owner skip fas-rs fas-rs; return; }
  if [ ! -r /sys/kernel/gpu/gpu_freq_table ]; then rep gpu skip - -; return; fi
  local M=/sys/class/misc/mali0/device
  local table hwmin hwmax min max gov hsl hsc hsd pp clb
  table=$(cat /sys/kernel/gpu/gpu_freq_table)
  hwmin=$(echo "$table" | tr ' ' '\n' | grep . | sort -n | head -1)
  hwmax=$(echo "$table" | tr ' ' '\n' | grep . | sort -n | tail -1)

  case "$PROFILE" in
    game)
      gov=Interactive; hsl=85; hsc=$hwmax; hsd=0; pp=always_on; clb=0
      max=$hwmax
      min=$(nearest "$(ratio_freq "$hwmin" "$hwmax" 55)" "$table")
      ;;
    powersave)
      gov=Default
      hsl=$(fget gpu_hs_load);  [ -z "$hsl" ] && hsl=94
      hsc=$(fget gpu_hs_clock); [ -z "$hsc" ] && hsc=650000
      hsd=$(fget gpu_hs_delay); [ -z "$hsd" ] && hsd=0
      pp=coarse_demand; clb=1
      min=$hwmin
      # 552 MHz (50% of the range) was low enough to make ordinary scrolling stutter. 754 still
      # leaves real headroom off the table without the phone feeling second-hand.
      max=$(nearest "$(ratio_freq "$hwmin" "$hwmax" 72)" "$table")
      ;;
    *)
      # Restore. EVERY field needs a fallback: with no factory snapshot these came back empty, the
      # write was skipped, and "Balanceado" quietly kept Game's ramp (highspeed_load 85) forever.
      # The fallbacks are this driver's own defaults, probed on the device.
      gov=$(fget gpu_gov);      [ -z "$gov" ] && gov=Interactive
      hsl=$(fget gpu_hs_load);  [ -z "$hsl" ] && hsl=94
      hsc=$(fget gpu_hs_clock); [ -z "$hsc" ] && hsc=650000
      hsd=$(fget gpu_hs_delay); [ -z "$hsd" ] && hsd=0
      pp=coarse_demand
      clb=$(fget gpu_cl_boost); [ -z "$clb" ] && clb=1
      min=$(fget gpu_min); [ -z "$min" ] && min=$hwmin
      max=$(fget gpu_max); [ -z "$max" ] && max=$hwmax
      ;;
  esac
  # explicit user overrides beat the profile preset
  [ -n "$(ovr gpu_gov)" ]          && gov=$(ovr gpu_gov)
  [ -n "$(ovr gpu_min)" ]          && min=$(ovr gpu_min)
  [ -n "$(ovr gpu_max)" ]          && max=$(ovr gpu_max)
  [ -n "$(ovr gpu_hs_load)" ]      && hsl=$(ovr gpu_hs_load)
  [ -n "$(ovr gpu_hs_clock)" ]     && hsc=$(ovr gpu_hs_clock)
  [ -n "$(ovr gpu_hs_delay)" ]     && hsd=$(ovr gpu_hs_delay)
  [ -n "$(ovr gpu_power_policy)" ] && pp=$(ovr gpu_power_policy)
  [ -n "$(ovr gpu_cl_boost)" ]     && clb=$(ovr gpu_cl_boost)

  # ORDER IS LOAD-BEARING: switching the Exynos GPU governor resets the min/max locks, so the
  # governor goes first and the clock window is locked after it.
  [ -n "$gov" ] && apply_node gpu.gov /sys/kernel/gpu/gpu_governor "$gov"
  if [ "$PROFILE" = game ] && [ "$COMPANION" = 1 ]; then
    # Companion mode defers the FLOOR to fas-rs — never the ceiling. Skipping the max write left
    # whatever the previous profile had set: coming back to Game from Economia kept a 552 MHz cap on
    # a GPU that can do 949, and nothing in the report said so because we simply never wrote it.
    if [ "$max" = "$hwmax" ]; then
      apply_node gpu.max /sys/kernel/gpu/gpu_max_clock "$max" ceiling
    else
      apply_node gpu.max /sys/kernel/gpu/gpu_max_clock "$max"
    fi
    apply_node gpu.min /sys/kernel/gpu/gpu_min_clock "$hwmin"
    rep gpu.owner skip fas-rs "-"
  else
    write_silent /sys/kernel/gpu/gpu_min_clock "$hwmin"
    if [ "$max" = "$hwmax" ]; then
      apply_node gpu.max /sys/kernel/gpu/gpu_max_clock "$max" ceiling
    else
      apply_node gpu.max /sys/kernel/gpu/gpu_max_clock "$max"
    fi
    apply_node gpu.min /sys/kernel/gpu/gpu_min_clock "$min"
  fi
  [ -n "$hsl" ] && apply_node gpu.hs_load  "$M/highspeed_load"  "$hsl"
  [ -n "$hsc" ] && apply_node gpu.hs_clock "$M/highspeed_clock" "$hsc"
  [ -n "$hsd" ] && apply_node gpu.hs_delay "$M/highspeed_delay" "$hsd"
  # power_policy prints the whole list with the active one in brackets, like a scheduler node.
  [ -n "$pp"  ] && apply_node gpu.power_policy "$M/power_policy" "$pp" sched
  [ -n "$clb" ] && apply_node gpu.cl_boost_disable /sys/kernel/gpu/gpu_cl_boost_disable "$clb"

  # How often the governor re-evaluates load. At the stock 100 ms the clock answers a load spike up
  # to a tenth of a second late, which on a 120 Hz panel is twelve frames. Halving it is the most
  # direct lever there is on "the GPU takes too long to ramp".
  local per
  case "$PROFILE" in
    game) per=50 ;;
    *)    per=$(fget gpu_dvfs_period); [ -z "$per" ] && per=100 ;;
  esac
  [ -n "$(ovr gpu_dvfs_period)" ] && per=$(ovr gpu_dvfs_period)
  [ -e "$M/dvfs_period" ] && apply_node gpu.dvfs_period "$M/dvfs_period" "$per"

  # How often the driver samples GPU utilisation. dvfs_period decides how often the governor RULES;
  # this decides how often it even LOOKS. Both matter for a burst of work that appears between two
  # samples and is gone before the next one.
  local pol
  case "$PROFILE" in
    game) pol=15 ;;
    *)    pol=$(fget gpu_polling_speed); [ -z "$pol" ] && pol=30 ;;
  esac
  [ -n "$(ovr gpu_polling_speed)" ] && pol=$(ovr gpu_polling_speed)
  [ -e "$M/polling_speed" ] && apply_node gpu.polling_speed "$M/polling_speed" "$pol"

  # The job scheduler tick: how often the GPU re-decides which context and job slot run. A combat
  # burst is many jobs arriving at once, and at 100 ms the scheduler re-evaluates them ten times a
  # second — on a 120 Hz panel that is twelve frames between decisions. Safe to lower here because
  # js_timeouts is expressed in ms and kbase recomputes its ticks (verified on this device).
  local jsp
  case "$PROFILE" in
    game) jsp=50 ;;
    *)    jsp=$(fget gpu_js_period); [ -z "$jsp" ] && jsp=100 ;;
  esac
  [ -n "$(ovr gpu_js_period)" ] && jsp=$(ovr gpu_js_period)
  [ -e "$M/js_scheduling_period" ] && apply_node gpu.js_period "$M/js_scheduling_period" "$jsp"

  # down_staycount is per clock level: how many periods the GPU must sit below the threshold before
  # stepping DOWN. Factory keeps the top levels for 5 periods but drops out of the middle almost
  # immediately (650 MHz after 3, 552 MHz after 1), so between two frames it falls further than it
  # needs to and pays the ramp again. Game holds the middle longer; the others restore the factory
  # ladder. Written per level, verified per level — the node prints the whole table.
  set_stay() {
    [ -e "$M/down_staycount" ] || return
    echo "$1 $2" > "$M/down_staycount" 2>/dev/null
    local got
    got=$(grep "Clock $1 " "$M/down_staycount" 2>/dev/null | awk '{print $4}')
    if [ "$got" = "$2" ]; then rep "gpu.stay_$1" ok "$got" "$2"
    else rep "gpu.stay_$1" fail "$got" "$2"; fi
  }
  case "$PROFILE" in
    game)
      set_stay 650000 5
      set_stay 552000 3
      ;;
    *)
      local s650 s552
      s650=$(fget gpu_stay_650000); [ -z "$s650" ] && s650=3
      s552=$(fget gpu_stay_552000); [ -z "$s552" ] && s552=1
      set_stay 650000 "$s650"
      set_stay 552000 "$s552"
      ;;
  esac
}

# ================= MIF (DRAM bus) — floor only; the 2093000 cap is kernel-owned =================
apply_mif() {
  local d=/sys/class/devfreq/17000010.devfreq_mif
  if [ ! -d "$d" ]; then rep mif skip - -; return; fi
  local cap table hwmin min
  cap=$(cat "$d/max_freq")
  table=$(cat "$d/available_frequencies" | tr ' ' '\n' | awk -v c="$cap" '$1<=c' | sort -n)
  hwmin=$(echo "$table" | head -1)
  case "$PROFILE" in
    game)
      # DRAM bandwidth is where sustained frame time is won or lost, and the factory governor is
      # deliberately reluctant: its ladder demands 90% bus load at 1352 MHz and 97% at 1794 MHz
      # before stepping up, so a game rarely climbs. The kernel does ship an aggressive profile
      # (interactive mode 1: 3% load -> max, measured reaching 2093 MHz where mode 0 stopped at
      # 1794), but current_mode falls back to 0 within ten seconds on its own, so it cannot be held.
      # The floor is what actually sticks — verified holding cur_freq for 8 s — so Game takes the
      # second-highest step and simply never goes below it.
      min=$(echo "$table" | tail -2 | head -1)
      ;;
    *)
      min=$(fget mif_min); [ -z "$min" ] && min=$hwmin
      ;;
  esac
  [ -n "$(ovr mif_min)" ] && min=$(ovr mif_min)
  write_silent "$d/min_freq" "$hwmin"
  apply_node mif.max "$d/max_freq" "$cap"
  apply_node mif.min "$d/min_freq" "$min"

  # The internal bus carries display and peripheral traffic alongside the DRAM path; leaving it at
  # its 50 MHz floor while the game streams is a bottleneck nobody looks at.
  local n=/sys/class/devfreq/17000020.devfreq_int imin
  [ -d "$n" ] || return
  case "$PROFILE" in
    game) imin=400000 ;;
    *)    imin=$(fget int_min); [ -z "$imin" ] && imin=50000 ;;
  esac
  [ -n "$(ovr int_min)" ] && imin=$(ovr int_min)
  apply_node int.min "$n/min_freq" "$imin"

  # Display bus. Writable and it holds (66000 -> 333000 stayed for 3 s), but I have no measurement
  # showing a game benefits, and the driver already sits at the floor while compositing at 120 Hz.
  # So there is no profile preset here — only an explicit override, and Auto means factory.
  local dsp=/sys/class/devfreq/17000040.devfreq_disp dmin
  [ -d "$dsp" ] || return
  dmin=$(ovr disp_min)
  if [ -z "$dmin" ]; then
    dmin=$(fget disp_min); [ -z "$dmin" ] && dmin=$(cat "$dsp/available_frequencies" | tr ' ' '
' | grep . | sort -n | head -1)
  fi
  apply_node disp.min "$dsp/min_freq" "$dmin"
}

# ================= I/O =================
apply_io() {
  local target
  target=$(ovr io_sched)
  if [ -z "$target" ]; then
    case "$PROFILE" in
      game) target=mq-deadline ;;
      powersave) target=kyber ;;
      *) target="" ;;
    esac
  fi
  for b in /sys/block/sd* /sys/block/mmcblk* /sys/block/nvme*; do
    [ -e "$b/queue/scheduler" ] || continue
    # sdb..sde are 0-byte UFS boot LUNs: tuning them was four wasted writes (and four report lines)
    # per apply. Only devices that actually hold data matter.
    local size
    size=$(cat "$b/size" 2>/dev/null)
    case "$size" in ''|*[!0-9]*) continue;; esac
    # 4-20 MB boot LUNs vs a 244 GB data LUN: checking for zero never excluded anything, so every
    # apply was also tuning four devices nothing reads from.
    [ "$size" -lt 4194304 ] && continue
    local name t
    name=$(basename "$b")
    t="$target"
    if [ -z "$t" ]; then t=$(fget "io_$name"); [ -z "$t" ] && t=ssg; fi
    if ! grep -q "$t" "$b/queue/scheduler"; then rep "io.$name" skip - "$t"; continue; fi
    apply_node "io.$name" "$b/queue/scheduler" "$t" sched

    # --- queue level: these persist across a scheduler switch, so they are restored by value ---
    # Always written, never only on the way up: Game raised read_ahead_kb to 256 and no other
    # profile lowered it again, so Balanceado silently kept Game's readahead.
    local ranew nrnew rqaff iostats
    case "$PROFILE" in
      game)
        # Benchmarked on this device with a 1.25 GB asset file: 484 MB/s at 128 kB, 509 at 256,
        # 522 at 512, and then flat (508 at 1024, 522 at 2048) — max_sectors_kb caps a request at
        # 1 MB and UFS 2.2 saturates around 520 MB/s, so anything past 512 only occupies page cache.
        ranew=512; nrnew=128
        rqaff=2      # complete the request on the CPU that issued it — cheaper across big.LITTLE
        iostats=0    # stop accounting every request; small saving on the hot path
        ;;
      *)
        ranew=$(fget "io_${name}_ra");      [ -z "$ranew" ] && ranew=128
        nrnew=$(fget "io_${name}_nr");      [ -z "$nrnew" ] && nrnew=62
        rqaff=$(fget "io_${name}_rqaff");   [ -z "$rqaff" ] && rqaff=1
        iostats=$(fget "io_${name}_stats"); [ -z "$iostats" ] && iostats=1
        ;;
    esac
    apply_node "io.$name.nr" "$b/queue/nr_requests" "$nrnew"
    apply_node "io.$name.ra" "$b/queue/read_ahead_kb" "$ranew"
    apply_node "io.$name.rq_affinity" "$b/queue/rq_affinity" "$rqaff"
    apply_node "io.$name.iostats" "$b/queue/iostats" "$iostats"
    # These three ship correct on this firmware and there is nothing to gain by changing them —
    # but another module can change them, so assert the right value instead of assuming it.
    # add_random=0: no entropy accounting per I/O. rotational=0: it is flash, not a platter, and
    # saying otherwise re-enables seek heuristics. nomerges=0: merging fully on (1 and 2 disable it,
    # which only costs throughput).
    apply_node "io.$name.add_random" "$b/queue/add_random" 0
    apply_node "io.$name.rotational" "$b/queue/rotational" 0
    apply_node "io.$name.nomerges"   "$b/queue/nomerges"   0

    # --- elevator level ---
    # These knobs belong to the scheduler instance and DO reset when the scheduler changes. Relying
    # on that was wrong: with a pinned scheduler (io_sched in the config) it never changes, so
    # Game's tight deadlines leaked into Balanceado and stayed there. Every profile writes its own
    # values explicitly; the "restore" numbers are this kernel's own defaults, probed on the device.
    local E="$b/queue/iosched"
    [ -d "$E" ] || continue

    if [ -e "$E/fifo_batch" ]; then          # mq-deadline (ssg also has read_expire, but no fifo_batch)
      case "$PROFILE" in
        game)
          # A game reads constantly and writes almost nothing (saves, logs). Tight deadline for
          # reads, let writes wait, and keep a writeback burst from parking in front of them.
          apply_node "io.$name.read_expire"    "$E/read_expire"    100 near
          apply_node "io.$name.write_expire"   "$E/write_expire"   3000 near
          apply_node "io.$name.writes_starved" "$E/writes_starved" 6
          apply_node "io.$name.fifo_batch"     "$E/fifo_batch"     8
          apply_node "io.$name.front_merges"   "$E/front_merges"   0
          apply_node "io.$name.async_depth"    "$E/async_depth"    16
          # How long a low-priority request waits before it is aged up and allowed to compete with
          # the foreground. Longer means background I/O keeps its distance from the game for longer.
          apply_node "io.$name.prio_aging"     "$E/prio_aging_expire" 20000 near
          ;;
        *)
          apply_node "io.$name.read_expire"    "$E/read_expire"    500 near
          apply_node "io.$name.write_expire"   "$E/write_expire"   5000 near
          apply_node "io.$name.writes_starved" "$E/writes_starved" 2
          apply_node "io.$name.fifo_batch"     "$E/fifo_batch"     16
          apply_node "io.$name.front_merges"   "$E/front_merges"   1
          apply_node "io.$name.async_depth"    "$E/async_depth"    24
          apply_node "io.$name.prio_aging"     "$E/prio_aging_expire" 10000 near
          ;;
      esac
    fi

    if [ -e "$E/max_write_starvation" ]; then # ssg (Samsung's own; shares read_expire/write_expire
                                             # names with deadline, hence the unique marker)
      case "$PROFILE" in
        game)
          apply_node "io.$name.read_expire"  "$E/read_expire"          100 near
          apply_node "io.$name.write_expire" "$E/write_expire"         3000 near
          apply_node "io.$name.write_starv"  "$E/max_write_starvation" 6
          apply_node "io.$name.front_merges" "$E/front_merges"         0
          ;;
        *)
          apply_node "io.$name.read_expire"  "$E/read_expire"          500 near
          apply_node "io.$name.write_expire" "$E/write_expire"         5000 near
          apply_node "io.$name.write_starv"  "$E/max_write_starvation" 2
          apply_node "io.$name.front_merges" "$E/front_merges"         1
          ;;
      esac
      # The wb_* knobs drive Samsung's write booster with values tied to their own heuristics;
      # guessing at them buys nothing measurable, so they are left alone.
    fi

    if [ -e "$E/slice_idle" ]; then          # bfq
      case "$PROFILE" in
        game)
          # Idling waits for the next request from the same process to keep it sequential. On flash
          # there is no seek to protect, so it is pure added latency.
          apply_node "io.$name.slice_idle"   "$E/slice_idle"        0
          apply_node "io.$name.fifo_sync"    "$E/fifo_expire_sync"  62 near
          apply_node "io.$name.low_latency"  "$E/low_latency"       1
          ;;
        *)
          apply_node "io.$name.slice_idle"   "$E/slice_idle"        8
          apply_node "io.$name.fifo_sync"    "$E/fifo_expire_sync"  125 near
          apply_node "io.$name.low_latency"  "$E/low_latency"       1
          ;;
      esac
    fi

    if [ -e "$E/read_lat_nsec" ]; then       # kyber
      case "$PROFILE" in
        powersave)
          # kyber throttles to hit a target latency; relaxing it lets the controller batch more and
          # wake up less often.
          apply_node "io.$name.read_lat"  "$E/read_lat_nsec"  4000000
          apply_node "io.$name.write_lat" "$E/write_lat_nsec" 20000000
          ;;
        *)
          apply_node "io.$name.read_lat"  "$E/read_lat_nsec"  2000000
          apply_node "io.$name.write_lat" "$E/write_lat_nsec" 10000000
          ;;
      esac
    fi
  done
}

# ================= UFS link =================
# Underneath every scheduler decision sits the link itself. Idle, it gates its clock (150 ms) and
# hibernates (2 ms), so the FIRST access after a quiet moment pays a wake-up — exactly the access
# that loads the next asset. Game keeps the link awake and lets the UFS driver ask for a higher big
# cluster floor while it is busy; the other profiles put Samsung's values back, because an always
# clocked link costs battery for nothing when you are not streaming assets.
apply_ufs() {
  local u p
  for u in /sys/devices/platform/*.ufs; do
    [ -d "$u" ] || continue
    local cg cd ah
    case "$PROFILE" in
      game) cg=0; cd=500; ah=10000 ;;
      *)    cg=$(fget ufs_clkgate);       [ -z "$cg" ] && cg=1
            cd=$(fget ufs_clkgate_delay); [ -z "$cd" ] && cd=150
            ah=$(fget ufs_hibern8);       [ -z "$ah" ] && ah=2000 ;;
    esac
    [ -e "$u/clkgate_enable" ]    && apply_node ufs.clkgate       "$u/clkgate_enable"    "$cg"
    [ -e "$u/clkgate_delay_ms" ]  && apply_node ufs.clkgate_delay "$u/clkgate_delay_ms"  "$cd"
    [ -e "$u/auto_hibern8" ]      && apply_node ufs.auto_hibern8  "$u/auto_hibern8"      "$ah"
    # Runtime PM level: 3 (factory) puts the DEVICE to sleep and hibernates the link; 1 keeps the
    # device awake and hibernates only the link, so the first access after an idle gap does not pay
    # a device wake-up. Mechanism, not measurement — the UFS latency monitor on this device never
    # counted a request, so I could not put a number on it. Costs battery; Game only.
    local rpm
    case "$PROFILE" in
      game) rpm=1 ;;
      *)    rpm=$(fget ufs_rpm_lvl); [ -z "$rpm" ] && rpm=3 ;;
    esac
    [ -n "$(ovr ufs_rpm_lvl)" ] && rpm=$(ovr ufs_rpm_lvl)
    [ -e "$u/rpm_lvl" ] && apply_node ufs.rpm_lvl "$u/rpm_lvl" "$rpm"
    break
  done
  p=/sys/kernel/ufs_perf_0
  [ -d "$p" ] || return
  local q1
  case "$PROFILE" in
    game) q1=2016000 ;;
    *)    q1=$(fget ufs_qos_cluster1); [ -z "$q1" ] && q1=1248000 ;;
  esac
  # This is a transient boost the UFS driver requests only while I/O is in flight, not a standing
  # floor, so it complements fas-rs instead of fighting it — fas-rs schedules frames and knows
  # nothing about a texture being pulled off storage.
  [ -e "$p/pm_qos_cluster1" ] && apply_node ufs.qos_big "$p/pm_qos_cluster1" "$q1"
}

# ================= f2fs garbage collection =================
# /data is f2fs, and a log-structured filesystem has to garbage-collect. When it decides to do that
# mid-game the write path stalls, which is felt as a hitch that has nothing to do with the frame.
# GC_URGENT_LOW (2) does not disable it — it tells f2fs to collect only while the device is idle.
# Every other profile restores GC_NORMAL, because permanently suppressed GC eventually fragments the
# filesystem and costs more than it saved.
apply_f2fs() {
  local f
  for f in /sys/fs/f2fs/*; do
    [ -d "$f" ] || continue
    local urgent minsleep
    case "$PROFILE" in
      game) urgent=2; minsleep=120000 ;;
      *)    urgent=0
            minsleep=$(fget f2fs_gc_min_sleep); [ -z "$minsleep" ] && minsleep=30000 ;;
    esac
    # gc_urgent reads back as a NAME (GC_NORMAL / GC_URGENT_LOW), never as the number written.
    if [ -e "$f/gc_urgent" ]; then
      ( echo "$urgent" > "$f/gc_urgent" ) 2>/dev/null
      local got want
      got=$(cat "$f/gc_urgent" 2>/dev/null)
      case "$urgent" in
        2) want=GC_URGENT_LOW ;;
        1) want=GC_URGENT_HIGH ;;
        *) want=GC_NORMAL ;;
      esac
      if [ "$got" = "$want" ]; then rep f2fs.gc_urgent ok "$got" "$want"
      else rep f2fs.gc_urgent fail "$got" "$want"; fi
    fi
    [ -e "$f/gc_min_sleep_time" ] && apply_node f2fs.gc_sleep "$f/gc_min_sleep_time" "$minsleep"

    # ipu_policy is a bitmask deciding WHEN f2fs overwrites a block in place instead of allocating a
    # fresh segment (and creating work for the garbage collector later):
    #   0x01 FORCE  0x02 SSR  0x04 UTIL  0x08 SSR_UTIL  0x10 FSYNC  0x20 ASYNC  0x40 NOCACHE
    # Factory here is 0x10 (fsync writes only). Game adds 0x04 (UTIL), which triggers in-place
    # updates once the filesystem is fuller than min_ipu_util — measured at 70 on this device, with
    # /data already at 83%, so it applies right now. Fewer new segments means less GC to run later,
    # which is the same stall we are chasing with GC_URGENT_LOW. It is one bit with an explicit
    # trigger condition, not the blanket FORCE.
    local ipu
    case "$PROFILE" in
      game) ipu=20 ;;
      *)    ipu=$(fget f2fs_ipu_policy); [ -z "$ipu" ] && ipu=16 ;;
    esac
    [ -n "$(ovr f2fs_ipu)" ] && ipu=$(ovr f2fs_ipu)
    [ -e "$f/ipu_policy" ] && apply_node f2fs.ipu_policy "$f/ipu_policy" "$ipu"
    break
  done
}

# ================= cpuidle =================
# The deep idle state costs 230 us to leave on the LITTLE cluster and 340 us on the BIG one. Inside
# an 8.3 ms frame budget that is up to 4% spent waking a core that a game was about to use anyway.
# Disabling it is a real battery cost, which is why only Game does it. Per-CPU: writing cpu0 and cpu4
# leaves the other six untouched.
apply_cpuidle() {
  local c s want n=0 ok=0
  for c in /sys/devices/system/cpu/cpu[0-9]*; do
    s="$c/cpuidle/state1/disable"
    [ -e "$s" ] || continue
    case "$PROFILE" in
      game) want=1 ;;
      *) want=$(fget "cpuidle_$(basename "$c")"); [ -z "$want" ] && want=0 ;;
    esac
    # Adaptive policies need race-to-idle in all tiers, including a late snapshot inherited
    # from another tuner that disabled deep idle. This is a policy choice, not a stock claim.
    [ "$ADAPTIVE" = 1 ] && [ "$REQUESTED_PROFILE" != none ] && want=0
    n=$((n+1))
    write_node "$s" "$want" >/dev/null 2>&1 && ok=$((ok+1))
  done
  [ "$n" = 0 ] && return
  if [ "$ok" = "$n" ]; then rep cpuidle.deep ok "$want ($ok/$n)" "$want"
  else rep cpuidle.deep fail "$ok/$n" "$want"; fi
}

# ================= PELT (quasi-WALT responsiveness; 1/2/4 only) =================
apply_pelt() {
  [ "$COMPANION" = 1 ] && [ -z "${M54_BENCH_PELT:-}" ] && { rep pelt.owner skip fas-rs fas-rs; return; }
  local P=/proc/sys/kernel/sched_pelt_multiplier v
  if [ ! -w "$P" ]; then rep pelt skip - -; return; fi
  case "${M54_BENCH_PELT:-}" in
    1|2|4) v="$M54_BENCH_PELT" ;;
    *)
      case "$PROFILE" in
        game) v=2 ;;
        *)    v=$(fget pelt); [ -z "$v" ] && v=1 ;;
      esac
      ;;
  esac
  apply_node pelt "$P" "$v"
}

# EAS is deliberately not a profile preset. The M54 kernel exposes a real scheduler switch, but
# disabling it can trade battery/thermals for a better short sample. Only the controlled benchmark
# may touch it; bench_sched.sh restores the exact value it found even when interrupted.
apply_bench_eas() {
  local P=/proc/sys/kernel/sched_energy_aware v="${M54_BENCH_EAS:-}"
  case "$v" in 0|1) ;; *) return;; esac
  [ -w "$P" ] || { rep eas skip - "$v"; return; }
  apply_node eas "$P" "$v"
}

# ================= Thermal (separate axis; battery zone is never disabled) =================
apply_thermal() {
  local ty orig
  for tz in /sys/class/thermal/thermal_zone*; do
    [ -e "$tz/mode" ] || continue
    ty=$(cat "$tz/type" 2>/dev/null | tr 'A-Z' 'a-z')
    case "$ty" in
      *batt*) apply_node "thermal.$ty" "$tz/mode" enabled ;;
      big|little|g3d|*cpu*|*gpu*)
        if [ "$THERMAL" = "aggressive" ] && [ ! -e "$M54_DIR/thermal_guard_forced" ]; then
          apply_node "thermal.$ty" "$tz/mode" disabled
        else
          # Moderate always means protection enabled. A temp-root snapshot can contain
          # disabled modes left by another tuner and must never re-disable protection.
          apply_node "thermal.$ty" "$tz/mode" enabled
        fi
        ;;
    esac
  done
}

# ================= VM (cheap, live) =================
apply_vm() {
  # swappiness is not a user control; the profile picks it. Samsung ships 130, which suits a phone
  # juggling many small apps — but measured on this device a single game holds ~4 GB RSS, and at 130
  # the kernel compresses ITS pages into zram and then stalls faulting them back in. That is what a
  # sudden drop to 20 fps followed by a jump back looks like. Game therefore swaps lazily; the other
  # profiles keep Samsung's behaviour, where caching many background apps is the right trade.
  local sw page cache stat
  case "$PROFILE" in
    game)      sw=80; page=0; cache=50; stat=10 ;;
    powersave) sw=100
               page=$(fget vm_page-cluster); [ -z "$page" ] && page=0
               cache=$(fget vm_vfs_cache_pressure); [ -z "$cache" ] && cache=100
               stat=1 ;;
    *)         sw=$(fget vm_swappiness); [ -z "$sw" ] && sw=130
               page=$(fget vm_page-cluster); [ -z "$page" ] && page=0
               cache=$(fget vm_vfs_cache_pressure); [ -z "$cache" ] && cache=100
               stat=$(fget vm_stat_interval); [ -z "$stat" ] && stat=1 ;;
  esac
  apply_node vm.swappiness /proc/sys/vm/swappiness "$sw"
  apply_node vm.page_cluster /proc/sys/vm/page-cluster "$page"
  apply_node vm.cache_pressure /proc/sys/vm/vfs_cache_pressure "$cache"
  apply_node vm.stat_interval /proc/sys/vm/stat_interval "$stat"
}

# ================= GOS =================
apply_gos() {
  local pkg=com.samsung.android.game.gos st bak="$M54_DIR/gos_backup"
  case "$GOS" in
    disabled|enabled)
      if [ ! -f "$bak" ]; then
        if pm list packages -e --user 0 "$pkg" 2>/dev/null | grep -q "$pkg"; then echo enabled > "$bak"
        else echo disabled > "$bak"; fi
      fi
      [ "$GOS" = disabled ] && pm disable-user --user 0 "$pkg" >/dev/null 2>&1
      [ "$GOS" = enabled ] && pm enable --user 0 "$pkg" >/dev/null 2>&1
      ;;
    *) rep gos skip - untouched; return ;;
  esac
  if pm list packages -e --user 0 "$pkg" 2>/dev/null | grep -q "$pkg"; then st=enabled; else st=disabled; fi
  if [ "$st" = "$GOS" ]; then rep gos ok "$st" "$GOS"; else rep gos fail "$st" "$GOS"; fi
}

# "none" is an explicit stock restore. It must not leave values behind from an earlier profile.
if [ "$PROFILE" = "none" ]; then
  PROFILE=balanced
  RESTORING_STOCK=1
  log "profile=none: restoring the captured stock values"
fi
REQUESTED_PROFILE="$PROFILE"
# Native policy starts from the recorded baseline; Game no longer pins high floors or disables
# deep idle while the learner is waiting for valid frames. Explicit overrides still win.
[ "$ADAPTIVE" = 1 ] && [ "$PROFILE" != none ] && PROFILE=balanced
apply_cpu
apply_gpu
apply_mif
PROFILE="$REQUESTED_PROFILE"
LIVE_PROFILE="$PROFILE"
[ "${M54_STORAGE_COOLDOWN:-}" = 1 ] && PROFILE=balanced
apply_io
apply_ufs
apply_f2fs
PROFILE="$LIVE_PROFILE"
[ "$ADAPTIVE" = 1 ] && [ "$PROFILE" != none ] && PROFILE=balanced
apply_pelt
apply_bench_eas
apply_cpuidle
PROFILE="$REQUESTED_PROFILE"
apply_vm
# Opt-in, one-shot: fires exactly when this script actually runs with profile=game (a real
# selection or the once-per-boot restore), never on a timer or a blind re-apply.
if [ "$PROFILE" = "game" ] && [ "$(read_cfg game_ram_clear 0)" = "1" ] && \
   [ "${M54_SKIP_RAM_CLEAR:-}" != 1 ]; then
  game_ram_clear
fi
[ "${RESTORING_STOCK:-}" = 1 ] && rep profile ok stock none
apply_thermal
apply_gos

# Aggressive thermal mode is never left without an independent fail-safe. The watcher is a
# singleton and re-enables CPU/GPU zones on temperature or time limits.
if [ "$THERMAL" = aggressive ] && [ "$(read_cfg thermal_guard 1)" = 1 ]; then
  if ! pid_record_alive "$M54_DIR/thermal_guard_pid" thermal_guard.sh; then
    rm -f "$M54_DIR/thermal_guard_pid"
    nohup sh "$DIR/thermal_guard.sh" </dev/null >/dev/null 2>&1 &
  fi
else
  sh "$DIR/thermal_guard_stop.sh" >/dev/null 2>&1
fi
# The foreground-game watcher is gone: it rewrote the profile from a hand-written list of package
# names, which is a declaration pretending to be an inference. A watcher already running from an
# older install is stopped here rather than left to keep rewriting the profile underneath us --
# disabling a thing has never been the same as killing what it started, on this device least of all.
if [ "${M54_SESSION_CONTROLLER:-}" != 1 ] && [ -f "$M54_DIR/session_watch_pid" ]; then
  sh "$DIR/session_watch_stop.sh" >/dev/null 2>&1
fi
result_end
log "apply done"
end_apply_lock
trap - EXIT INT TERM
sh "$DIR/adaptive_start.sh"
