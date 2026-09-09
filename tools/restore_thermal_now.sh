#!/system/bin/sh
# Actual inherited state found during installation: compute thermal zones were disabled.
# Restore the configured moderate policy immediately; no restart or module unload.
for z in /sys/class/thermal/thermal_zone*; do
  type=$(cat "$z/type" 2>/dev/null)
  case "$type" in BIG|LITTLE|G3D|battery)
    chmod 0644 "$z/mode"
    echo enabled > "$z/mode"
    echo "$type=$(cat "$z/mode")"
    ;;
  esac
done
id
