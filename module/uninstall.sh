#!/system/bin/sh
# Restore Samsung/MARs/GOS state before deleting the ownership ledger. Kernel/sysfs and resetprop
# state disappear on the next reboot, but these framework settings do not.
MODDIR=${0%/*}
[ -f "$MODDIR/scripts/adaptive_stop.sh" ] && sh "$MODDIR/scripts/adaptive_stop.sh" || exit 1
[ -f "$MODDIR/scripts/restore_persistent.sh" ] || exit 1
if sh "$MODDIR/scripts/restore_persistent.sh"; then
  rm -rf /data/adb/m54tuner
else
  echo "M54 Tuner: restore failed; /data/adb/m54tuner retained for recovery" >&2
  exit 1
fi
