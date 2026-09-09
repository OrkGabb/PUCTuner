#!/system/bin/sh
# Reads crossover.log and reports both arms. De-duplicates by the engine's own window
# timestamp, because the recorder samples faster than the decision window.
#
# Reports the median as well as the mean: one teleport into a menu produces a window three
# times faster than anything around it, and a mean over a few dozen windows is not robust to
# that. It also prints how many windows each arm actually contributed -- an imbalance is
# itself a finding, usually meaning one arm spent its time in `waiting_frames`.
awk '
/^#/ { next }
{
  arm=$1; at=""; p=""; j=""; t=""; w="unavailable"; f=""; a=""; c=""; span=0
  for (i = 2; i <= NF; i++) {
    split($i, kv, "=")
    if (kv[1]=="at") at=kv[2]
    else if (kv[1]=="p95_ms") p=kv[2]+0
    else if (kv[1]=="jank") j=kv[2]+0
    else if (kv[1]=="temp") t=kv[2]+0
    else if (kv[1]=="watts") w=kv[2]
    else if (kv[1]=="frame_time_ms") span=kv[2]+0
    else if (kv[1]=="frames") f=kv[2]+0
    else if (kv[1]=="action") a=kv[2]+0
    else if (kv[1]=="cadence") c=kv[2]+0
  }
  if (at=="" || seen[at]++) next          # one row per engine window
  if (f < 24 || p <= 0) { idle[arm]++; next }   # no usable frame measurement
  n[arm]++
  P[arm] += p; J[arm] += j; T[arm] += t
  if (w != "unavailable") { W[arm] += w+0; powerN[arm]++ }
  if (span > 0) { F[arm] += f; duration[arm] += span }
  if (a > 0) acted[arm]++
  cad[arm "/" c]++
  samples[arm "," n[arm]] = p
  jank[arm "," n[arm]] = j
}
function median(a, count,   list, k, i, tmp, m) {
  for (i = 1; i <= count; i++) list[i] = a[k = i]
  for (i = 1; i <= count; i++) for (m = i + 1; m <= count; m++)
    if (list[m] < list[i]) { tmp = list[i]; list[i] = list[m]; list[m] = tmp }
  return count % 2 ? list[(count + 1) / 2] : (list[count / 2] + list[count / 2 + 1]) / 2
}
END {
  split("observe active", arms, " ")
  printf "%-8s %6s %8s %8s %8s %8s %7s %7s %7s %7s\n",
         "arm", "wins", "p95mean", "p95med", "jankmean", "jankmed", "fps", "tempC", "watts", "acted"
  for (x = 1; x <= 2; x++) {
    arm = arms[x]
    if (!n[arm]) { printf "%-8s %6d  (no window with a usable frame measurement)\n", arm, 0; continue }
    for (i = 1; i <= n[arm]; i++) { ps[i] = samples[arm "," i]; js[i] = jank[arm "," i] }
    fps = duration[arm] > 0 ? sprintf("%.1f", 1000*F[arm]/duration[arm]) : "N/A"
    power = powerN[arm] > 0 ? sprintf("%.2f", W[arm]/powerN[arm]) : "N/A"
    printf "%-8s %6d %8.1f %8.1f %8.3f %8.3f %7s %7.1f %7s %6.0f%%\n",
           arm, n[arm], P[arm]/n[arm], median(ps, n[arm]), J[arm]/n[arm], median(js, n[arm]),
           fps, T[arm]/n[arm], power, 100*acted[arm]/n[arm]
  }
  print ""
  for (k in idle) printf "%s: %d windows with no usable frames (not counted)\n", k, idle[k]
  for (k in cad) printf "cadence %s: %d windows\n", k, cad[k]
  print ""
  print "Scene variation and carryover remain possible; compare per-quad effects too."
  print "Cadence-relative jank is descriptive, not a fixed threshold across treatments."
  print "Legacy logs without frame_time_ms cannot establish FPS. Unavailable power is not zero."
}
' "${1:-/data/local/tmp/crossover.log}"
