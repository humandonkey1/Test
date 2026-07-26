#!/usr/bin/env bash
# ============================================================================
# HYDRA benchmark: ratio and speed against the standard tools, on a corpus of
# deliberately different data types.
#
# Every hydra number below is verified by a full decompress-and-compare before
# it is printed.  A ratio that does not round trip is reported as FAIL.
# ============================================================================
set -u

CORPUS=${CORPUS:-/tmp/corpus}
HYDRA=${HYDRA:-./bin/hydra}
LEVELS=${LEVELS:-"1 5 9"}

if [ ! -d "$CORPUS" ]; then
    echo "building corpus..."
    python3 bench/make_corpus.py "$CORPUS" || exit 1
fi

command -v "$HYDRA" >/dev/null 2>&1 || [ -x "$HYDRA" ] || { echo "no hydra binary; run make"; exit 1; }

hr() { printf '%.0s-' $(seq 1 96); echo; }

# time a command, echo seconds
timeit() {
    local s e
    s=$(date +%s.%N)
    "$@" >/dev/null 2>&1
    e=$(date +%s.%N)
    echo "$e $s" | awk '{printf "%.4f", $1-$2}'
}

printf "%-18s %10s  %-12s %10s %8s %9s %9s\n" \
       FILE SIZE TOOL OUT RATIO "ENC MB/s" "DEC MB/s"
hr

total_in=0
declare -A tool_out

for f in "$CORPUS"/*; do
    [ -f "$f" ] || continue
    name=$(basename "$f")
    size=$(stat -c%s "$f")
    total_in=$((total_in + size))

    for lvl in $LEVELS; do
        out="/tmp/.hb.hydra"
        back="/tmp/.hb.back"
        t_enc=$(timeit "$HYDRA" c -"$lvl" --no-checksum "$f" "$out")
        csize=$(stat -c%s "$out" 2>/dev/null || echo 0)
        t_dec=$(timeit "$HYDRA" d "$out" "$back")
        if ! cmp -s "$f" "$back"; then
            printf "%-18s %10s  %-12s %10s %8s %9s %9s\n" \
                   "$name" "$size" "hydra -$lvl" "FAIL" "-" "-" "-"
            continue
        fi
        ratio=$(awk -v a="$size" -v b="$csize" 'BEGIN{printf "%.3f", a/b}')
        emb=$(awk -v s="$size" -v t="$t_enc" 'BEGIN{printf "%.1f", s/1e6/t}')
        dmb=$(awk -v s="$size" -v t="$t_dec" 'BEGIN{printf "%.1f", s/1e6/t}')
        printf "%-18s %10s  %-12s %10s %8s %9s %9s\n" \
               "$name" "$size" "hydra -$lvl" "$csize" "$ratio" "$emb" "$dmb"
        tool_out["hydra-$lvl"]=$(( ${tool_out["hydra-$lvl"]:-0} + csize ))
    done

    for spec in "gzip -9:gzip" "bzip2 -9:bzip2" "xz -9e:xz" "zstd -19:zstd"; do
        cmd=${spec%%:*}; bin=${spec##*:}
        command -v "$bin" >/dev/null 2>&1 || continue
        t_enc=$(timeit sh -c "$cmd -c '$f' > /tmp/.hb.other")
        csize=$(stat -c%s /tmp/.hb.other 2>/dev/null || echo 0)
        [ "$csize" -gt 0 ] || continue
        t_dec=$(timeit sh -c "$bin -dc /tmp/.hb.other > /dev/null")
        ratio=$(awk -v a="$size" -v b="$csize" 'BEGIN{printf "%.3f", a/b}')
        emb=$(awk -v s="$size" -v t="$t_enc" 'BEGIN{printf "%.1f", s/1e6/t}')
        dmb=$(awk -v s="$size" -v t="$t_dec" 'BEGIN{printf "%.1f", s/1e6/t}')
        printf "%-18s %10s  %-12s %10s %8s %9s %9s\n" \
               "$name" "$size" "$cmd" "$csize" "$ratio" "$emb" "$dmb"
        tool_out["$bin"]=$(( ${tool_out["$bin"]:-0} + csize ))
    done
    hr
done

echo
echo "corpus totals ($total_in bytes in):"
for k in "${!tool_out[@]}"; do
    v=${tool_out[$k]}
    awk -v k="$k" -v a="$total_in" -v b="$v" \
        'BEGIN{printf "  %-12s %12d  %.3fx\n", k, b, a/b}'
done | sort -k3 -n -r
