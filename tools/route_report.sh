#!/system/bin/sh
# Compares runs recorded by route.sh, one line per label.
#
# Reports the median beside the mean because a single loading hitch skews a mean over a few
# dozen windows, and prints the starting temperature of every run: two runs that began 10 C
# apart are not comparable no matter how repeatable the route was.
awk '
/^# run label=/ {
  for (i = 1; i <= NF; i++) {
    if ($i ~ /^label=/)      { split($i, kv, "="); cur = kv[2] }
    if ($i ~ /^start_temp=/) { split($i, kv, "="); gsub("C", "", kv[2]); begin[cur] = kv[2] }
  }
  next
}
/^# majfaults=/ {
  for (i = 1; i <= NF; i++) {
    split($i, kv, "=")
    if (kv[1] == "majfaults") faults[cur] = kv[2]
    if (kv[1] == "read_mib")  reads[cur] = kv[2]
  }
  next
}
/^#/ { next }
{
  label = $1; at = ""; p = 0; j = 0; t = 0; w = 0; f = 0; a = 0
  for (i = 2; i <= NF; i++) {
    split($i, kv, "=")
    if (kv[1] == "at")            at = kv[2]
    else if (kv[1] == "p95_ms")   p = kv[2] + 0
    else if (kv[1] == "jank")     j = kv[2] + 0
    else if (kv[1] == "temp")     t = kv[2] + 0
    else if (kv[1] == "watts")    w = kv[2] + 0
    else if (kv[1] == "frames")   f = kv[2] + 0
    else if (kv[1] == "action")   a = kv[2] + 0
  }
  if (at == "" || seen[label "/" at]++) next
  if (f < 24 || p <= 0) { blind[label]++; next }
  n[label]++
  P[label] += p; J[label] += j; T[label] += t; W[label] += w; F[label] += f
  if (a > 0) acted[label]++
  if (t > peak[label]) peak[label] = t
  store[label "," n[label]] = p
  order[label] = 1
}
function median(count, key,   list, i, m, tmp) {
  for (i = 1; i <= count; i++) list[i] = store[key "," i]
  for (i = 1; i <= count; i++) for (m = i + 1; m <= count; m++)
    if (list[m] < list[i]) { tmp = list[i]; list[i] = list[m]; list[m] = tmp }
  return count % 2 ? list[(count + 1) / 2] : (list[count / 2] + list[count / 2 + 1]) / 2
}
END {
  printf "%-14s %5s %7s %7s %8s %6s %6s %6s %6s %7s %8s\n",
         "run", "wins", "p95mean", "p95med", "jank", "fps", "t0C", "peakC", "watts", "acted", "majflt"
  for (k in order) {
    printf "%-14s %5d %7.1f %7.1f %8.3f %6.1f %6s %6.0f %6.2f %6.0f%% %8s\n",
           k, n[k], P[k]/n[k], median(n[k], k), J[k]/n[k], F[k]/n[k]/6,
           (k in begin ? begin[k] : "?"), peak[k], W[k]/n[k],
           100*acted[k]/n[k], (k in faults ? faults[k] : "?")
  }
  print ""
  for (k in blind) printf "%s: %d windows with no usable frame measurement\n", k, blind[k]
  print ""
  print "Only compare runs whose t0 is within a few degrees. A route is repeatable; a thermal"
  print "state is not, and the second run of any pair starts from the heat the first one left."
}
' "${1:-/data/local/tmp/route.log}"
