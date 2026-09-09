#!/system/bin/sh
# KernelSU late-load replacement for post-fs-data. The regular late-load flow invokes service.sh
# and boot-completed.sh after this script; this pass only captures/restores early state and props.
MODDIR=${0%/*}
KSU_LATE_LOAD=1 sh "$MODDIR/post-fs-data.sh"
