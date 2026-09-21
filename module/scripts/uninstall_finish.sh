#!/system/bin/sh
# Finishes an uninstall after the module directory is gone. uninstall.sh copies this file,
# lib.sh and restore_persistent.sh into $M54_DIR/uninstall and starts it detached.
#
# Magisk and KernelSU run uninstall.sh at post-fs-data of the boot after removal, before
# system_server exists. `content`, `settings` and `pm` all fail there, so a restore run inline
# could never succeed: the old uninstall deleted the ledger anyway and left MARs, SPCM, the
# Samsung settings and GOS as the module had set them, for good. Wait for the framework, retry,
# and delete the ledger only once every setting reads back as restored.
#
# If every attempt fails the ledger stays, together with this script. Run it again by hand:
#   sh /data/adb/m54tuner/uninstall/uninstall_finish.sh
DIR=$(dirname "$0")
. "$DIR/lib.sh"

# A module installed again while this waited owns the ledger now: restoring would undo its
# settings and deleting would take its config. Leave both to it.
reinstalled() {
  [ -d /data/adb/modules/m54tuner ] && [ ! -e /data/adb/modules/m54tuner/remove ]
}

i=0
while [ "$(getprop sys.boot_completed)" != 1 ]; do
  i=$((i + 1))
  if [ "$i" -gt 360 ]; then
    log "uninstall: framework not up after 30 min; $M54_DIR retained, run $DIR/uninstall_finish.sh"
    exit 1
  fi
  sleep 5
done

n=0
while [ "$n" -lt 10 ]; do
  if reinstalled; then
    log "uninstall: module installed again; leaving $M54_DIR to it"
    rm -rf "$DIR"
    exit 0
  fi
  if sh "$DIR/restore_persistent.sh" settings; then
    cd / || exit 1
    rm -rf "$M54_DIR"
    exit 0
  fi
  n=$((n + 1))
  sleep 30
done
log "uninstall: persistent restore failed $n times; $M54_DIR retained, run $DIR/uninstall_finish.sh"
exit 1
