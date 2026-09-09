#!/system/bin/sh
# Replace only the native payload; retain config, learned memory and the running game.
set -eu
MOD=/data/adb/modules/m54tuner
DATA=/data/adb/m54tuner
BACKUP=/data/local/tmp/m54-resume-before-$(date +%Y%m%d-%H%M%S)
[ -x "$MOD/scripts/adaptive_stop.sh" ]
[ -s /data/local/tmp/m54-adaptive-resume ]
[ -s /data/local/tmp/runqueue-resume.bpf.o ]
[ ! -e "$BACKUP" ]
mkdir -m 700 "$BACKUP"
cp "$MOD/bin/m54-adaptive" "$BACKUP/m54-adaptive"
cp "$MOD/bin/runqueue.bpf.o" "$BACKUP/runqueue.bpf.o"
cp "$DATA/config" "$BACKUP/config"
sh "$MOD/scripts/adaptive_stop.sh"
cat "$DATA/adaptive_status"
echo "restored_pelt=$(cat /proc/sys/kernel/sched_pelt_multiplier)"
cp /data/local/tmp/m54-adaptive-resume "$MOD/bin/m54-adaptive.next"
cp /data/local/tmp/runqueue-resume.bpf.o "$MOD/bin/runqueue.bpf.o.next"
chmod 755 "$MOD/bin/m54-adaptive.next"
chmod 644 "$MOD/bin/runqueue.bpf.o.next"
mv "$MOD/bin/m54-adaptive.next" "$MOD/bin/m54-adaptive"
mv "$MOD/bin/runqueue.bpf.o.next" "$MOD/bin/runqueue.bpf.o"
sha256sum "$MOD/bin/m54-adaptive" "$MOD/bin/runqueue.bpf.o"
sh "$MOD/scripts/adaptive_start.sh"
