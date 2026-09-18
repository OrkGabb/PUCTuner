#!/system/bin/sh
# Restore Samsung/MARs/GOS state before deleting the ownership ledger. Kernel/sysfs and resetprop
# state disappear on the next reboot, but these framework settings do not.
MODDIR=${0%/*}
[ -f "$MODDIR/scripts/adaptive_stop.sh" ] && sh "$MODDIR/scripts/adaptive_stop.sh"
[ -f "$MODDIR/scripts/restore_persistent.sh" ] && sh "$MODDIR/scripts/restore_persistent.sh"
rm -rf /data/adb/m54tuner
