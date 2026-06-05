#!/bin/sh
set -eu

if [ "$#" -lt 3 ] || [ "$#" -gt 5 ]; then
    echo "usage: $0 OUT_TSV START COUNT [JOBS] [BUILD_PREFIX]" >&2
    exit 2
fi

out="$1"
start="$2"
count="$3"
jobs="${4:-1}"
build_prefix="${5:-build/churn-seed-sweep}"
cppflags="${CHURN_CPPFLAGS:--Iinclude -Itests/littlefs -Ibenchmarks/churn_model}"
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/fffs-churn-sweep.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT

printf "seed\tstatus\ttotal_written\ttime_ms\tflash_ops\tlog\n" > "$out"

seq "$start" "$((start + count - 1))" | xargs -n1 -P "$jobs" sh -c '
    outdir="$1"
    build_prefix="$2"
    cppflags="$3"
    i="$4"
    seed=$(printf "%08x" "$i")
    dir="${build_prefix}-${seed}"
    log="${dir}/run.log"
    row="${outdir}/${seed}.tsv"

    make BUILD_DIR="$dir" \
        CPPFLAGS="${cppflags} -DFFFS_HOST_CHURN_SEED=0x${seed}u" \
        "$dir/fffs_churn_probe" >/dev/null 2>&1

    if "$dir/fffs_churn_probe" > "$log" 2>&1; then
        status=pass
    else
        status=fail
    fi

    written=$(awk "
        /churn summary/ || /no_space progress/ {
            for (i = 1; i <= NF; i++) {
                if (\$i ~ /^total_written=/) {
                    split(\$i, a, \"=\");
                    print a[2];
                }
            }
        }" "$log" | tail -1)

    time_ms=$(awk "
        /churn total/ || /no_space timing/ {
            for (i = 1; i <= NF; i++) {
                if (\$i ~ /^time=/) {
                    split(\$i, a, \"=\");
                    if (a[2] != \"\") {
                        print a[2];
                    } else if (i < NF) {
                        print \$(i + 1);
                    }
                }
            }
        }" "$log" | tail -1)

    ops=$(awk "
        /churn total/ || /no_space timing/ {
            for (i = 1; i <= NF; i++) {
                if (\$i ~ /^flash_ops=/) {
                    split(\$i, a, \"=\");
                    print a[2];
                }
            }
        }" "$log" | tail -1)

    printf "%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$seed" "$status" "${written:-0}" "${time_ms:-0}" "${ops:-}" \
        "$log" > "$row"
' sh "$tmpdir" "$build_prefix" "$cppflags"

cat "$tmpdir"/*.tsv >> "$out"
