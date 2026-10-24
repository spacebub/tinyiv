#!/usr/bin/env bash
# Usage: tools/harness.sh perf|sanitize|coverage [options] [-- tiv_harness arguments]
# Builds tiv_harness for the mode in a separate tree and runs it. Results go to reports/ in that
# tree. See harness/README.md.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

usage() {
    cat >&2 <<'EOF'
Usage: tools/harness.sh perf|sanitize|coverage [options] [-- tiv_harness arguments]

  perf       Release build with symbols and frame pointers, recorded by perf, with a report
  sanitize   address and undefined behaviour sanitizers over the small corpus
  coverage   clang source coverage, with a report of what the sittings never reach

Options:
  --tree DIR      build tree, build-perf, build-asan or build-cov by default
  --sandbox DIR   where the harness writes, harness-sandbox by default
  --size SIZE     small or full corpus, full for perf and small otherwise
  --freq HZ       perf samples a second, 999 by default
  --dwarf         perf unwinds by DWARF instead of frame pointers, slower and larger
  --stat          perf also counts cycles, instructions and misses in a second run
  --memory SIZE   most memory a run may take before it is killed, half of the system by default,
                  in the units systemd takes such as 12G
EOF
    exit 2
}

say() {
    printf '== %s\n' "$*"
}

MODE=${1:-}
[ -n "$MODE" ] || usage
shift

TREE=
SANDBOX=$ROOT/harness-sandbox
SIZE=
FREQ=999
DWARF=0
STAT=0
MEMORY=$(awk '/^MemTotal:/ { printf "%dK", $2 / 2 }' /proc/meminfo)
HARNESS_ARGS=()

while [ $# -gt 0 ]; do
    case $1 in
        --tree) TREE=$2; shift 2 ;;
        --sandbox) SANDBOX=$2; shift 2 ;;
        --size) SIZE=$2; shift 2 ;;
        --freq) FREQ=$2; shift 2 ;;
        --dwarf) DWARF=1; shift ;;
        --stat) STAT=1; shift ;;
        --memory) MEMORY=$2; shift 2 ;;
        --) shift; HARNESS_ARGS=("$@"); break ;;
        *) usage ;;
    esac
done

# Every mode uses clang. Coverage needs it, and the trees stay comparable.
build() {
    local tree=$1
    shift

    say "configuring $tree"
    mkdir -p "$tree"
    cmake -S "$ROOT" -B "$tree" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DTIV_HARNESS=ON "$@" > "$tree/configure.log" 2>&1 \
        || { cat "$tree/configure.log"; exit 1; }
    say "building tiv_harness in $tree"
    cmake --build "$tree" --target tiv_harness
}

# A run that takes more memory than allowed is killed alone, not the desktop session with it.
contain() {
    systemd-run --user --scope --quiet -p MemoryMax="$MEMORY" -p MemorySwapMax=0 -- "$@"
}

report_dir() {
    local dir
    dir=$1/reports/$(date +%Y%m%d-%H%M%S)
    mkdir -p "$dir"
    printf '%s' "$dir"
}

run_perf() {
    TREE=${TREE:-$ROOT/build-perf}
    SIZE=${SIZE:-full}

    command -v perf > /dev/null || { echo "perf is not installed" >&2; exit 1; }

    # SDL is built from C, so the C flags give it frame pointers.
    build "$TREE" -DCMAKE_BUILD_TYPE=Release -DTIV_SYMBOLS=ON \
        "-DCMAKE_C_FLAGS=-g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer"

    local out graph status
    out=$(report_dir "$TREE")

    # Kernel call chains need perf_event_paranoid below 2, and the harness spends its time in user space.
    if [ "$DWARF" = 1 ]; then
        graph=(--call-graph dwarf,32768 --user-callchains)
    else
        graph=(--call-graph fp --user-callchains)
    fi

    local harness=("$TREE/bin/tiv_harness" --sandbox "$SANDBOX" --size "$SIZE")

    # The corpus is written first, so the recording leaves it out.
    say "corpus"
    contain "${harness[@]}" --corpus || say "the corpus check found problems, listed above"

    say "recording into $out"
    set +e
    contain perf record -F "$FREQ" "${graph[@]}" -k CLOCK_MONOTONIC -o "$out/perf.data" -- \
        "${harness[@]}" --marks "$out/marks.tsv" "${HARNESS_ARGS[@]}" 2>&1 | tee "$out/harness.txt"
    status=${PIPESTATUS[0]}
    set -e

    if [ ! -s "$out/perf.data" ]; then
        say "perf recorded nothing, see the messages above"
        exit 1
    fi

    if [ "$STAT" = 1 ]; then
        say "counting"
        contain perf stat -e task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,cpu-migrations,page-faults \
            -o "$out/stat.txt" -- "${harness[@]}" "${HARNESS_ARGS[@]}" > /dev/null 2>&1 || true
    fi

    say "writing the report"
    python3 -I "$ROOT/tools/perf-report.py" "$out" "$TREE" "$status"
    say "report: $out/report.md"

    exit "$status"
}

run_sanitize() {
    TREE=${TREE:-$ROOT/build-asan}
    SIZE=${SIZE:-small}

    build "$TREE" -DCMAKE_BUILD_TYPE=Debug -DTIV_SANITIZE=ON

    local out status
    out=$(report_dir "$TREE")

    export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1:strict_string_checks=1
    # Leaks are judged from the log below, so they do not replace the exit status of the harness.
    export LSAN_OPTIONS=suppressions=$ROOT/tools/lsan.supp:print_suppressions=0:exitcode=0
    export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:report_error_type=1

    say "running under the sanitizers, log in $out/harness.txt"
    set +e
    contain "$TREE/bin/tiv_harness" --sandbox "$SANDBOX" --size "$SIZE" --patience 120 "${HARNESS_ARGS[@]}" 2>&1 \
        | tee "$out/harness.txt"
    status=${PIPESTATUS[0]}
    set -e

    if ! python3 -I "$ROOT/tools/sanitizer-verdict.py" "$out/harness.txt"; then
        say "the sanitizers found errors, see $out/harness.txt"
        exit 1
    fi

    [ "$status" = 0 ] && say "clean" || say "the harness failed with status $status"
    exit "$status"
}

run_coverage() {
    TREE=${TREE:-$ROOT/build-cov}
    SIZE=${SIZE:-small}

    build "$TREE" -DCMAKE_BUILD_TYPE=Debug \
        "-DCMAKE_CXX_FLAGS=-fprofile-instr-generate -fcoverage-mapping" \
        "-DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate"

    local out status binary
    out=$(report_dir "$TREE")
    binary=$TREE/bin/tiv_harness

    say "running, log in $out/harness.txt"
    set +e
    LLVM_PROFILE_FILE="$out/raw/harness-%p.profraw" contain "$binary" --sandbox "$SANDBOX" --size "$SIZE" \
        "${HARNESS_ARGS[@]}" 2>&1 | tee "$out/harness.txt"
    status=${PIPESTATUS[0]}
    set -e

    llvm-profdata merge -sparse "$out"/raw/*.profraw -o "$out/coverage.profdata"

    # Only the tinyiv sources count, not the harness, SDL or system headers.
    local only=(-instr-profile "$out/coverage.profdata" -ignore-filename-regex '/(harness|bench|_deps|\.download-cache)/|^/usr/')

    llvm-cov report "$binary" "${only[@]}" > "$out/coverage.txt"
    llvm-cov show "$binary" "${only[@]}" -format=html -output-dir "$out/html" -show-line-counts-or-regions
    llvm-cov export "$binary" "${only[@]}" -skip-expansions \
        | jq -r '.data[0].functions[] | select(.count == 0 and (.filenames[0] | contains("/src/"))) | "\(.filenames[0] | sub(".*/src/"; "src/"))\t\(.name)"' \
        | llvm-cxxfilt | sort -u > "$out/never-run.tsv"

    tail -n 1 "$out/coverage.txt"
    say "per file: $out/coverage.txt"
    say "functions never run: $out/never-run.tsv ($(wc -l < "$out/never-run.tsv"))"
    say "line by line: $out/html/index.html"

    exit "$status"
}

cd "$ROOT"

case $MODE in
    perf) run_perf ;;
    sanitize) run_sanitize ;;
    coverage) run_coverage ;;
    *) usage ;;
esac
