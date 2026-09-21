#!/system/bin/sh
# Restore Samsung/MARs/GOS state before deleting the ownership ledger. Kernel/sysfs and resetprop
# state disappear on the next reboot, but these framework settings do not.
#
# Stopping what the module started happens here, while its binaries still exist. Restoring the
# settings cannot: this runs at post-fs-data, before the framework, and the module directory is
# deleted as soon as it returns. So the restore is handed to a copy of itself under the ledger
# directory that waits for the framework (scripts/uninstall_finish.sh). A failed stop is logged
# and never a reason to skip that restore: the settings are the part that outlives a reboot.
MODDIR=${0%/*}
. "$MODDIR/scripts/lib.sh"

sh "$MODDIR/scripts/adaptive_stop.sh" || log "uninstall: adaptive_stop failed; continuing"
sh "$MODDIR/scripts/restore_persistent.sh" stop || log "uninstall: daemon stop failed; continuing"

STAGE="$M54_DIR/uninstall"
if ! mkdir -p "$STAGE" ||
   ! cp -f "$MODDIR/scripts/lib.sh" "$MODDIR/scripts/restore_persistent.sh" \
           "$MODDIR/scripts/uninstall_finish.sh" "$STAGE/"; then
  log "uninstall: could not stage the restore; $M54_DIR retained"
  exit 1
fi
nohup sh "$STAGE/uninstall_finish.sh" </dev/null >/dev/null 2>&1 &
exit 0
