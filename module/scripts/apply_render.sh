#!/system/bin/sh
# M54 Tuner module v2 — rendering stack (HWUI / RenderEngine / SurfaceFlinger props).
#
# THE POINT OF THIS FILE IS RESTART SCOPE. A prop is only live for a process that has not read it
# yet, so each lever declares the CHEAPEST restart that makes it real, and we do only that:
#
#   none     — nothing reads it late; already live.
#   apps     — debug.hwui.* / ro.hwui.* are read when an app process starts its HWUI:
#              `cmd activity force-stop <pkg>` on the chosen apps. No blink, ~instant.
#   systemui — same props, but for the shell: restart SystemUI only (status bar flickers).
#   sf       — debug.renderengine.* / debug.sf.* / ro.surface_flinger.* are read once by
#              SurfaceFlinger: `ctl.restart surfaceflinger`, the screen blanks and comes back.
#              NEVER implicit — the app must pass --sf after the user confirms it.
#
# Usage: sh apply_render.sh [--sf] [--force]
#   --sf     the user accepted the screen blink, do the SurfaceFlinger restart now
#   --force  apply every prop and take the full scope even if nothing changed
#   --boot   real-boot context (post-fs-data): nothing has read the props yet, so record the
#            state and restart NOTHING — no force-stop, no SurfaceFlinger, no pending flag

DIR=$(dirname "$0")
. "$DIR/lib.sh"

STATE="$M54_DIR/render_state"
DO_SF=0
FORCE=0
BOOT=0
for a in "$@"; do
  case "$a" in
    --sf) DO_SF=1 ;;
    --force) FORCE=1 ;;
    --boot) BOOT=1 ;;
  esac
done

# Probed against this firmware's own binaries (strings on libhwui.so / surfaceflinger):
#   * libhwui knows exactly two renderer values, "skiagl" and "skiavk" — there is no threaded
#     variant for HWUI, and NO ro.hwui.* string appears in the library at all, which is why the
#     old texture/layer/glyph cache props were pure placebo here and are gone.
#   * SurfaceFlinger refuses non-threaded RenderEngine backends ("Non-threaded RenderEngine not
#     supported"), so only skiaglthreaded / skiavkthreaded are real. Factory is GLES (Ganesh).
#   * debug.sf.latch_unsignaled already ships true, so there was nothing to gain by touching it.
RENDERER=$(read_cfg hwui_renderer skiagl)       # skiagl|skiavk
RE_BACKEND=$(read_cfg re_backend skiaglthreaded)  # skiaglthreaded|skiavkthreaded
APPS=$(read_cfg render_apps "")
SUI=$(read_cfg restart_systemui 0)

# A v1 config says "default" for both; that is not a value either binary accepts, so map it onto
# the factory values instead of writing a string the driver will ignore while we report it as OK.
[ "$RENDERER" = default ] && RENDERER=skiagl
[ "$RE_BACKEND" = default ] && RE_BACKEND=skiaglthreaded

result_begin "render"
if ! begin_apply_lock; then rep module.lock fail busy render; result_end; exit 1; fi
trap 'end_apply_lock' EXIT INT TERM

# Signature of everything that lands in a process at start; scope is derived from what changed.
SIG_APPS="$RENDERER"
SIG_SF="$RE_BACKEND"
OLD_APPS=$(grep -E '^apps=' "$STATE" 2>/dev/null | cut -d= -f2-)
OLD_SF=$(grep -E '^sf=' "$STATE" 2>/dev/null | cut -d= -f2-)
# When (seconds since boot) the current SF-tier props were written, plus the boot they belong to.
OLD_AT=$(grep -E '^sf_at=' "$STATE" 2>/dev/null | cut -d= -f2-)
OLD_BOOT=$(grep -E '^boot=' "$STATE" 2>/dev/null | cut -d= -f2-)
BOOT_NOW=$(boot_id)
case "$OLD_AT" in ''|*[!0-9]*) OLD_AT=0;; esac
# What the RUNNING SurfaceFlinger actually built. This is ground truth and needs no bookkeeping:
# if it already matches what we want, SF has clearly started after those props were written.
sf_live_backend() {
  case "$(dumpsys SurfaceFlinger 2>/dev/null | grep -m1 -oE 'RE (GLES|Vulkan)')" in
    *Vulkan*) echo skiavkthreaded ;;
    *GLES*)   echo skiaglthreaded ;;
    *)        echo "" ;;
  esac
}

sf_drift() {
  [ "$(getprop debug.renderengine.backend)" = "$RE_BACKEND" ] || return 0
  [ "$(sf_live_backend)" = "$RE_BACKEND" ] || return 0
  return 1
}

SF_DRIFT=0
sf_drift && SF_DRIFT=1

# v0.11 stored the retired FPS experiment in the SF signature. It never described the running
# RenderEngine, so accept an old backend|fps record when dumpsys proves that backend is already
# live. This clears the old timestamp/pending latch without restarting a healthy SurfaceFlinger.
case "$OLD_SF" in
  "$RE_BACKEND|"*)
    if [ "$(sf_live_backend)" = "$RE_BACKEND" ]; then OLD_SF="$SIG_SF"; OLD_AT=0; fi
    ;;
esac

if [ "$OLD_BOOT" != "$BOOT_NOW" ]; then
  # No usable record for this boot (previous boot, or the module was just updated). Normally that
  # means the props are not live yet and a restart IS pending — but not if the running SF is
  # already using the backend we want, which proves it started after they were written. Without
  # this check an in-place module update re-raised a banner for props that were already live, and
  # nothing could ever clear it.
  OLD_AT=0
  if [ "$(sf_live_backend)" = "$RE_BACKEND" ]; then
    OLD_SF="$SIG_SF"
  else
    OLD_SF=""
  fi
fi

# ---------------- HWUI renderer ----------------
apply_prop render.hwui_renderer debug.hwui.renderer "$RENDERER"

# ---------------- SurfaceFlinger tier ----------------
apply_prop render.re_backend debug.renderengine.backend "$RE_BACKEND"

# `fps_unlock` is retired: this only restores the three experimental props to their captured
# factory values once. The config key remains an inert tombstone for old identity migration.
FPS_RETIRED="$M54_DIR/fps_unlock_retired_v012"
if [ ! -e "$FPS_RETIRED" ]; then
  FPS_RETIRE_FAILED=0
  apply_prop_managed render.fps_retire_override ro.surface_flinger.game_default_frame_rate_override "" || FPS_RETIRE_FAILED=1
  apply_prop_managed render.fps_retire_enable   ro.surface_flinger.enable_frame_rate_override "" || FPS_RETIRE_FAILED=1
  apply_prop_managed render.fps_retire_feature  debug.graphics.game_default_frame_rate.disabled "" || FPS_RETIRE_FAILED=1
  if [ "$FPS_RETIRE_FAILED" = 0 ] && : > "$FPS_RETIRED"; then
    rep render.fps_retired ok factory factory
  else
    rep render.fps_retired fail restore factory
  fi
fi

# A scope record is evidence about values that actually landed. Never advance it after a rejected
# property write: doing so suppresses the retry and turns live drift into a false "already applied".
if [ "$M54_RESULT_FAILED" != 0 ] || [ "$M54_RESULT_IO_FAILED" != 0 ]; then
  rep render.scope fail write-failed unchanged
  result_end
  exit $?
fi

# ---------------- scope resolution ----------------
NEED_APPS=0
NEED_SF=0
[ "$SIG_APPS" != "$OLD_APPS" ] && NEED_APPS=1
[ "$SIG_SF" != "$OLD_SF" ] && NEED_SF=1
[ "$SF_DRIFT" = 1 ] && NEED_SF=1
[ "$FORCE" = "1" ] && { NEED_APPS=1; NEED_SF=1; }

if [ "$BOOT" = "1" ]; then
  # Real boot: the props land before anything reads them, so no instance predates them.
  rep render.scope ok boot boot
  rm -f "$M54_DIR/pending_sf" || rep render.pending fail remove absent
  { echo "apps=$SIG_APPS"; echo "sf=$SIG_SF"; echo "sf_at=0"; echo "boot=$BOOT_NOW"; } |
    atomic_write "$STATE" 0600 || rep render.state fail write boot
  result_end
  rc=$?
  log "render applied at boot (no restarts needed)"
  exit "$rc"
fi

if [ "$NEED_APPS" = "1" ]; then
  force_stop_apps "$APPS"
  # Killing SystemUI while a SurfaceFlinger restart is also in play is asking for trouble: during
  # testing a burst of both in quick succession took system_server down with it (Android recovered
  # with a runtime restart, but every app closed). Two guards: never do it in the same pass as an SF
  # restart — SF coming back redraws the shell anyway — and never more than once a minute.
  if [ "$SUI" = "1" ] && [ "$NEED_SF" != "1" ] && [ ! -e /data/adb/m54tuner/.sui_cooldown ]; then
    before=$(first_pid com.android.systemui)
    if pkill -f com.android.systemui >/dev/null 2>&1; then
      i=0; after=0
      while [ "$i" -lt 10 ]; do
        sleep 1; after=$(first_pid com.android.systemui)
        [ "$after" -gt 0 ] 2>/dev/null && [ "$after" != "$before" ] && break
        i=$((i + 1))
      done
    fi
    if [ "$after" -gt 0 ] 2>/dev/null && [ "$after" != "$before" ] &&
       : > /data/adb/m54tuner/.sui_cooldown; then
      (sleep 60; rm -f /data/adb/m54tuner/.sui_cooldown) >/dev/null 2>&1 &
      rep render.restart_systemui ok "$after" "$before"
      log "systemui restarted"
    else
      rep render.restart_systemui fail "$after" "$before"
    fi
  elif [ "$SUI" = "1" ]; then
    rep render.restart_systemui skip guarded guarded
  fi
else
  rep render.restart_apps skip - -
fi

# Pendency is a fact about the RUNNING PROCESS: a prop is inert for a SurfaceFlinger that started
# BEFORE it was written, and live for one that started after. Comparing those two timestamps answers
# it directly, so the flag clears no matter who restarted SF — us, a reboot, a runtime restart, or
# the user's own soft reboot. Keying it on a stored signature instead is what left the banner up
# forever after SF had already picked the props up.
if [ "$SIG_SF" != "$OLD_SF" ] || [ "$SF_DRIFT" = 1 ]; then
  OLD_SF="$SIG_SF"
  OLD_AT=$(uptime_s)                 # written now; any SF older than this has not seen it
fi

SF_START=$(proc_start_s "$(first_pid surfaceflinger)")
SF_RESTARTED=0
if [ "$DO_SF" = "1" ] && [ "$SF_START" -lt "$OLD_AT" ] 2>/dev/null; then
  restart_sf
  SF_RESTARTED=1
  SF_START=$(proc_start_s "$(first_pid surfaceflinger)")
fi

# On this Samsung build, ctl.restart may first publish a new SurfaceFlinger PID and only then turn
# into a broader Android-runtime restart. That delayed restart can reassert properties after an
# immediate readback looked correct. Do not write a successful terminal record inside that race.
[ "$SF_RESTARTED" = 1 ] && sleep 12

POST_SF_DRIFT=0
sf_drift && POST_SF_DRIFT=1
if [ "$SF_START" -lt "$OLD_AT" ] 2>/dev/null; then
  mark_pending_sf
elif [ "$POST_SF_DRIFT" = 1 ]; then
  # A Samsung runtime restart can accompany ctl.restart surfaceflinger and reassert properties
  # after the new PID appears. A PID change alone is therefore not proof that the requested state
  # became live. Keep recovery actionable and make the caller see a failed apply.
  mark_pending_sf
  rep render.sf_verify fail drift "$SIG_SF"
else
  rep render.sf_restart ok live live
  rm -f "$M54_DIR/pending_sf" || rep render.pending fail remove absent
fi

{ echo "apps=$SIG_APPS"; echo "sf=$OLD_SF"; echo "sf_at=$OLD_AT"; echo "boot=$BOOT_NOW"; } |
  atomic_write "$STATE" 0600 || rep render.state fail write live
result_end
rc=$?
log "render applied renderer=$RENDERER re=$RE_BACKEND scope_apps=$NEED_APPS scope_sf=$NEED_SF sf_done=$DO_SF"
exit "$rc"
