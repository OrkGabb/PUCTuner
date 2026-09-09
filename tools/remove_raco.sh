#!/system/bin/sh
set -eu
# Reviewed replacement for upstream uninstall: no unbounded parent-directory deletion.
PKG=com.kanagawa.yamada.project.raco
am force-stop "$PKG" 2>/dev/null || true
for proc in /proc/[0-9]*; do
  [ -r "$proc/cmdline" ] || continue
  cmd=$(tr '\000' ' ' < "$proc/cmdline" 2>/dev/null) || continue
  case "$cmd" in
    *'/data/adb/modules/ProjectRaco/'*|*'/data/adb/modules_update/ProjectRaco/'*|*'/data/adb/ksu/modules/ProjectRaco/'*)
      pid=${proc##*/}
      [ "$pid" != "$$" ] && kill -TERM "$pid" 2>/dev/null || true
      ;;
  esac
done
sleep 2
for proc in /proc/[0-9]*; do
  [ -r "$proc/cmdline" ] || continue
  cmd=$(tr '\000' ' ' < "$proc/cmdline" 2>/dev/null) || continue
  case "$cmd" in *'/data/adb/modules/ProjectRaco/'*) kill -KILL "${proc##*/}" 2>/dev/null || true;; esac
done
if grep -q 'ProjectRaco/RSWAP' /proc/swaps; then
  avail=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
  used=$(awk '/ProjectRaco\/RSWAP/ {print $4}' /proc/swaps)
  [ "$avail" -gt $((used + 524288)) ] || { echo 'Not enough available RAM to remove Raco swap'; exit 1; }
  swapoff /data/ProjectRaco/RSWAP
fi
! grep -q 'ProjectRaco' /proc/swaps || { echo 'Raco swap still active; preserving files'; exit 1; }
pm uninstall "$PKG"
for d in /data/adb/modules/ProjectRaco /data/adb/modules_update/ProjectRaco /data/adb/ksu/modules/ProjectRaco /data/ProjectRaco; do
  [ -d "$d" ] || continue
  resolved=$(readlink -f "$d")
  [ "$resolved" = "$d" ] || { echo "Unexpected target: $d -> $resolved"; exit 1; }
  rm -rf "$d"
done
# Identified by upstream uninstall as its own notification image assets.
rm -f /data/local/tmp/logo.png /data/local/tmp/Anya.png
echo 'Raco package, four daemons, module and inactive swap data removed'
cat /proc/swaps
ps -A -o PID,NAME,ARGS | grep -Ei 'raco|anya|kobo|zetamin|rswap' || true
pm list packages | grep -i raco || true
id
