#!/system/bin/sh
# Samsung MARs/SPCM protection with ownership tracking. The module only removes rows it inserted
# itself and restores systemic switches to the exact values captured before the first change.

DIR=$(dirname "$0")
. "$DIR/lib.sh"

EXCL=content://com.samsung.android.sm.mars/MARs_ExcludeTarget
POL=content://com.samsung.android.sm.mars/MARs_Policy
SET=content://com.samsung.android.sm/settings
OWNED="$M54_DIR/protect_owned"
BAK="$M54_DIR/protect_backup"

WANT=$(read_cfg samsung_protect 0)
SPCM=$(read_cfg samsung_spcm 0)
POLOFF=$(read_cfg samsung_mars_off 0)
LIST=$(read_cfg protect_list "")
[ -z "$LIST" ] && LIST=$(read_cfg games "")

result_begin "protect"
if ! begin_apply_lock; then rep module.lock fail busy protect; result_end; exit 1; fi
trap 'end_apply_lock' EXIT INT TERM

in_list() {
  valid_pkg "$1" || return 1
  content query --uri "$EXCL" --where "packageName='$1'" 2>/dev/null | grep -Fq "packageName=$1"
}

add_pkg() {
  valid_pkg "$1" || { rep "protect.$1" skip invalid-package exclude; return 1; }
  content insert --uri "$EXCL" --bind policyNum:i:0 --bind condition:i:24 \
    --bind matchType:s:equals --bind packageName:s:"$1" >/dev/null 2>&1
  in_list "$1"
}

del_pkg() {
  valid_pkg "$1" || return 1
  content delete --uri "$EXCL" --where "packageName='$1'" >/dev/null 2>&1
  ! in_list "$1"
}

backup_once() {
  local key="$1" value="$2"
  [ -f "$BAK" ] || : > "$BAK"
  grep -qE "^$key=" "$BAK" 2>/dev/null && return 0
  [ -n "$value" ] || value=__ABSENT__
  echo "$key=$value" >> "$BAK"
}

backup_get() { grep -E "^$1=" "$BAK" 2>/dev/null | tail -1 | cut -d= -f2-; }
spcm_now() { content query --uri "$SET" --where "key='spcm_switch'" 2>/dev/null | grep -o 'value=[01]' | head -1 | cut -d= -f2; }
policy_now() { content query --uri "$POL" --where "policyNum=$1" 2>/dev/null | grep -o 'isPolicyEnabled=[01]' | head -1 | cut -d= -f2; }

desired="$M54_DIR/protect_desired.$$"
new_owned="$M54_DIR/protect_owned.$$"
: > "$desired"; : > "$new_owned"
if [ "$WANT" = "1" ]; then
  oldifs=$IFS; IFS=,
  for g in $LIST; do
    g=$(echo "$g" | tr -d ' ')
    valid_pkg "$g" && pm path "$g" >/dev/null 2>&1 && echo "$g" >> "$desired"
  done
  IFS=$oldifs
fi
sort -u "$desired" > "$desired.sorted" 2>/dev/null && mv -f "$desired.sorted" "$desired"

if [ -f "$OWNED" ]; then
  while IFS= read -r g; do
    valid_pkg "$g" || continue
    if grep -Fxq "$g" "$desired" 2>/dev/null; then echo "$g" >> "$new_owned"
    elif del_pkg "$g"; then rep "protect.$g" ok removido owned
    else rep "protect.$g" fail ainda-esta remove; echo "$g" >> "$new_owned"; fi
  done < "$OWNED"
fi

while IFS= read -r g; do
  [ -n "$g" ] || continue
  if grep -Fxq "$g" "$new_owned" 2>/dev/null; then
    rep "protect.$g" ok gerenciado exclude
  elif in_list "$g"; then
    rep "protect.$g" ok preexistente preserve
  elif add_pkg "$g"; then
    echo "$g" >> "$new_owned"
    rep "protect.$g" ok inserido owned
  else rep "protect.$g" fail nao-inseriu exclude; fi
done < "$desired"
sort -u "$new_owned" > "$new_owned.sorted" 2>/dev/null && mv -f "$new_owned.sorted" "$new_owned"
chmod 0600 "$new_owned" 2>/dev/null
mv -f "$new_owned" "$OWNED"
rm -f "$desired"

if [ "$SPCM" = "1" ]; then
  now=$(spcm_now); backup_once spcm "$now"
  content update --uri "$SET" --bind value:s:0 --where "key='spcm_switch'" >/dev/null 2>&1
  now=$(spcm_now); [ "$now" = 0 ] && rep protect.spcm ok desligado 0 || rep protect.spcm fail "$now" 0
else
  orig=$(backup_get spcm)
  if [ -n "$orig" ] && [ "$orig" != __ABSENT__ ]; then
    content update --uri "$SET" --bind value:s:"$orig" --where "key='spcm_switch'" >/dev/null 2>&1
    now=$(spcm_now); [ "$now" = "$orig" ] && rep protect.spcm ok "$now" "$orig" || rep protect.spcm fail "$now" "$orig"
  else rep protect.spcm skip sem-backup factory; fi
fi

for p in 1 8; do
  if [ "$POLOFF" = "1" ]; then
    st=$(policy_now "$p"); backup_once "policy$p" "$st"
    content update --uri "$POL" --bind isPolicyEnabled:i:0 --where "policyNum=$p" >/dev/null 2>&1
    st=$(policy_now "$p"); [ "$st" = 0 ] && rep "protect.policy$p" ok 0 0 || rep "protect.policy$p" fail "$st" 0
  else
    orig=$(backup_get "policy$p")
    if [ -n "$orig" ] && [ "$orig" != __ABSENT__ ]; then
      content update --uri "$POL" --bind isPolicyEnabled:i:"$orig" --where "policyNum=$p" >/dev/null 2>&1
      st=$(policy_now "$p"); [ "$st" = "$orig" ] && rep "protect.policy$p" ok "$st" "$orig" || rep "protect.policy$p" fail "$st" "$orig"
    else rep "protect.policy$p" skip sem-backup factory; fi
  fi
done

result_end
