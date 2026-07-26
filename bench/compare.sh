#!/usr/bin/env bash
# ============================================================================
# Ratio comparison against the standard tools.
#
# Sizes only -- speed is measured separately by bench_mem, which times the
# codec in memory rather than a process plus file I/O.
#
# Every hydra size below is produced by a run that was verified to decompress
# back to the exact input.  Anything that fails to round trip is printed as
# FAIL rather than being silently reported as a good ratio.
# ============================================================================
set -u
CORPUS=${CORPUS:-/tmp/corpus}
HYDRA=${HYDRA:-./bin/hydra}
LVL=${LVL:-7}

printf "%-18s %10s | %9s %9s %9s | %9s\n" \
       FILE SIZE "gzip -9" "bzip2 -9" "xz -9e" "hydra -$LVL"
printf '%.0s-' $(seq 1 82); echo

declare -A tot
tot[in]=0; tot[gzip]=0; tot[bzip2]=0; tot[xz]=0; tot[hydra]=0

for f in "$CORPUS"/*; do
    [ -f "$f" ] || continue
    name=$(basename "$f")
    size=$(stat -c%s "$f")

    g=$(gzip -9 -c "$f" | wc -c)
    b=$(bzip2 -9 -c "$f" | wc -c)
    x=$(xz -9e -c "$f" | wc -c)

    "$HYDRA" c -"$LVL" "$f" /tmp/.cmp.hz >/dev/null 2>&1
    h=$(stat -c%s /tmp/.cmp.hz 2>/dev/null || echo 0)
    "$HYDRA" d /tmp/.cmp.hz /tmp/.cmp.back >/dev/null 2>&1
    if ! cmp -s "$f" /tmp/.cmp.back; then
        printf "%-18s %10s | %9s %9s %9s | %9s\n" "$name" "$size" "$g" "$b" "$x" "FAIL"
        continue
    fi

    printf "%-18s %10s | %9s %9s %9s | %9s\n" "$name" "$size" "$g" "$b" "$x" "$h"
    tot[in]=$((tot[in]+size)); tot[gzip]=$((tot[gzip]+g))
    tot[bzip2]=$((tot[bzip2]+b)); tot[xz]=$((tot[xz]+x)); tot[hydra]=$((tot[hydra]+h))
done

printf '%.0s-' $(seq 1 82); echo
printf "%-18s %10s | %9s %9s %9s | %9s\n" TOTAL "${tot[in]}" \
       "${tot[gzip]}" "${tot[bzip2]}" "${tot[xz]}" "${tot[hydra]}"
awk -v i="${tot[in]}" -v g="${tot[gzip]}" -v b="${tot[bzip2]}" \
    -v x="${tot[xz]}" -v h="${tot[hydra]}" 'BEGIN{
  printf "%-18s %10s | %9.3f %9.3f %9.3f | %9.3f\n","RATIO","",i/g,i/b,i/x,i/h;
  printf "\nhydra vs xz: %+.2f%% smaller output\n", 100*(x-h)/x;
}'
