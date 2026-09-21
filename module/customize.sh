#!/system/bin/sh
# M54 Tuner module — install-time setup (sourced by the KSU/Magisk installer).

SKIPUNZIP=0
umask 077
if ! type abort >/dev/null 2>&1; then
  abort() { ui_print "$1"; exit 1; }
fi

ui_print "- PUCTuner (module): init-context tuning daemon"

DEVICE=$(getprop ro.product.device 2>/dev/null)
SOC=$(getprop ro.soc.model 2>/dev/null)
case "$DEVICE" in m54x|m54xxx|SM-M546B) ;;
  *) abort "! Unsupported device: $DEVICE (M54 / SM-M546B required)" ;;
esac
case "$SOC" in s5e8835|S5E8835|Exynos1380|"") ;;
  *) abort "! Unsupported SoC: $SOC (s5e8835 required)" ;;
esac

M54_DIR=/data/adb/m54tuner
mkdir -p "$M54_DIR"
chmod 0700 "$M54_DIR"

# Seed a safe default config only if the user (via the app) hasn't written one yet.
if [ ! -f "$M54_DIR/config" ]; then
  cat > "$M54_DIR/config" <<'CFG'
# M54 Tuner config v3 — the app writes this; the module reads it.
# ---- live tier (apply_profile.sh) ----
profile=none
adaptive_mode=active
adaptive_learning=1
adaptive_target_fps=60
adaptive_thermal_limit=82
thermal=moderate
gos=untouched
# GPU overrides ("" = use the profile preset)
protect_games=0
samsung_perf=0
samsung_protect=0
samsung_spcm=0
samsung_mars_off=0
protect_list=
protect_adj=-700
protect_interval=1
adaptive_ram_management=0
pelt=2
thermal_guard=1
thermal_guard_high=78000
thermal_guard_low=70000
thermal_guard_interval=2
thermal_guard_timeout=900
loading_boost_seconds=45
# ---- memory tier (apply_mem.sh) ----
zram_algo=lz4
# ---- render tier (apply_render.sh) ----
hwui_renderer=skiagl
re_backend=skiaglthreaded
render_apps=
restart_systemui=0
# ---- ART/zygote tier (apply_art.sh) — needs a soft reboot to take effect ----
art_usap=auto
art_dex2oat_little=auto
art_heap=auto
# ---- dexopt tier (apply_dexopt.sh) ----
games=
dexopt_mode=speed-profile
bench_min_spread_pct=5
CFG
  chmod 0600 "$M54_DIR/config"
  ui_print "- Default config written to $M54_DIR/config"
else
  # Older config: add keys introduced through v3 without touching existing choices.
  # the user already made.
  add_key() { grep -qE "^$1=" "$M54_DIR/config" || echo "$1=$2" >> "$M54_DIR/config"; }
  add_key protect_games 0
  add_key samsung_perf 0
  add_key samsung_protect 0
  add_key samsung_spcm 0
  add_key samsung_mars_off 0
  add_key protect_list ""
  add_key protect_adj -700
  add_key protect_interval 1
  add_key adaptive_ram_management 0
  add_key pelt 2
  add_key thermal_guard 1
  add_key thermal_guard_high 78000
  add_key thermal_guard_low 70000
  add_key thermal_guard_interval 2
  add_key thermal_guard_timeout 900
  add_key adaptive_mode active
  add_key adaptive_learning 1
  add_key adaptive_target_fps 60
  add_key adaptive_thermal_limit 82
  add_key loading_boost_seconds 45
  add_key hwui_renderer skiagl
  add_key re_backend skiaglthreaded
  add_key art_usap auto
  add_key art_dex2oat_little auto
  add_key art_heap auto
  add_key dexopt_mode speed-profile
  add_key bench_min_spread_pct 5
  add_key render_apps ""
  add_key restart_systemui 0
  ui_print "- Existing config kept (new v3 keys added)"
fi

chmod 0600 "$M54_DIR/config" 2>/dev/null
touch "$MODPATH/skip_mount"

# Retired payloads must not survive across updates: disabling a module never kills what it
# started, and neither does flashing over it. fas-rs companion mode was removed in v0.11.0
# (the engine stands down from foreign tuners instead of yielding axes to one); a stale
# fas/ directory from an older install would otherwise sit on the device implying a mode
# that no longer exists.
rm -rf "$MODPATH/fas" 2>/dev/null

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm_recursive "$MODPATH/scripts" 0 0 0755 0755
set_perm_recursive "$MODPATH/bin" 0 0 0755 0755
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/boot-completed.sh" 0 0 0755
set_perm "$MODPATH/late-load.sh" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755
