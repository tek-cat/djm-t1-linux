#!/usr/bin/env bash
# Summarize an `amidi -d` capture into distinct controls: status + data byte,
# how many messages, and the value range. A continuous control (fader/knob)
# shows one CC with a wide range; a button shows a Note with values 00/7F.
#
# Usage:  tools/decode-midi.sh /tmp/djm-capture.txt
set -euo pipefail
F="${1:?usage: decode-midi.sh <amidi-capture.txt>}"

awk '
NF>=2 {
  key=$1" "$2
  if (!(key in seen)) { seen[key]=1; order[++n]=key }
  cnt[key]++
  if (NF>=3) {
    v=strtonum("0x" $3)
    if (!(key in mn) || v<mn[key]) mn[key]=v
    if (!(key in mx) || v>mx[key]) mx[key]=v
  }
}
END {
  printf "%-9s %-6s %-6s %-13s %s\n", "status", "data1", "count", "value-range", "kind"
  printf "%-9s %-6s %-6s %-13s %s\n", "------", "-----", "-----", "-----------", "----"
  for (i=1;i<=n;i++) {
    key=order[i]; split(key,a," "); st=a[1]; d1=a[2]
    s=toupper(substr(st,1,1))
    kind = (s=="B") ? "CC (continuous)" : (s=="9") ? "Note (button)" : "other"
    lo = (key in mn) ? mn[key] : 0; hi = (key in mx) ? mx[key] : 0
    printf "%-9s %-6s %-6d 0x%02x..0x%02x   %s\n", st, d1, cnt[key], lo, hi, kind
  }
}' "$F"
