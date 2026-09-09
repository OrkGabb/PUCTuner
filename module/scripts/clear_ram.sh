#!/system/bin/sh
# M54 Tuner — manual RAM clear ("Limpar agora"): one-shot kill of background/
# cached apps. Same mechanism as the automatic Game-activation clear (game_ram_clear in lib.sh),
# just triggered on demand instead of tied to a profile switch.
# Usage: sh clear_ram.sh

DIR=$(dirname "$0")
. "$DIR/lib.sh"

result_begin "clear_ram"
if ! begin_apply_lock; then rep module.lock fail busy clear_ram; result_end; exit 1; fi
trap 'end_apply_lock' EXIT INT TERM
game_ram_clear
result_end
