#!/system/bin/sh
MODDIR=${0%/*}
ui_print "- Gerando diagnóstico sem listas de pacotes..."
OUT=$(sh "$MODDIR/scripts/diagnostics.sh")
ui_print "- Salvo em $OUT"
