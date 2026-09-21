#!/system/bin/sh
# M54 Tuner — manual RAM clear ("Limpar agora"): one-shot kill of background/
# cached apps via clear_background_ram in lib.sh. This manual trigger and the engine's own
# opt-in trim (adaptive_ram_management) are the two real mechanisms; there is no
# profile-gated automatic clear anymore (static profiles are retired).
# Usage: sh clear_ram.sh

DIR=$(dirname "$0")
. "$DIR/lib.sh"

result_begin "clear_ram"
if ! begin_apply_lock; then rep module.lock fail busy clear_ram; result_end; exit 1; fi
trap 'end_apply_lock' EXIT INT TERM
clear_background_ram
result_end
exit $?
