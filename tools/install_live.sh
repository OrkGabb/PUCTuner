#!/system/bin/sh
set -eu
BASE=/data/adb/modules
[ ! -d /data/adb/ksu/modules ] || BASE=/data/adb/ksu/modules
MODDIR="$BASE/m54tuner"
DATA=/data/adb/m54tuner
# A stop that fails must abort the install, not race it: proceeding past a live
# daemon risks the new process reading the model file mid-save and starting empty.
if [ -f "$MODDIR/scripts/adaptive_stop.sh" ]; then
  sh "$MODDIR/scripts/adaptive_stop.sh" || { echo "install_live: adaptive stop failed, aborting" >&2; exit 1; }
fi
mkdir -p "$MODDIR" "$DATA"
chmod 0700 "$DATA"
unzip -oq /data/local/tmp/m54tuner-module.zip -d "$MODDIR"
# unzip updates files but never deletes payloads removed from a newer release.
rm -rf "$MODDIR/fas"
chmod 0755 "$MODDIR" "$MODDIR"/*.sh "$MODDIR/scripts" "$MODDIR/scripts"/*.sh "$MODDIR/bin" "$MODDIR/bin/m54-adaptive"
if [ ! -f "$DATA/config" ]; then
  cat > "$DATA/config" <<'CFG'
profile=balanced
adaptive_mode=active
adaptive_learning=1
adaptive_target_fps=60
adaptive_thermal_limit=82
thermal=moderate
thermal_guard=1
gos=untouched
auto_game=0
CFG
  algo=$(cat /sys/block/zram0/comp_algorithm | tr ' ' '\n' | grep '^\[' | tr -d '[]')
  echo "zram_algo=$algo" >> "$DATA/config"
fi
chmod 0600 "$DATA/config"
touch "$MODDIR/skip_mount"
sh "$MODDIR/post-fs-data.sh" --capture-only
sh "$MODDIR/scripts/apply_profile.sh"
# Live installation has completed; opening the APK must not trigger unrelated zram/ART/render work.
touch /dev/.m54tuner_session
chmod 0600 /dev/.m54tuner_session
cat "$MODDIR/module.prop"
cat "$DATA/result"
id
