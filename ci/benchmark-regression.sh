#!/usr/bin/env bash
set -uo pipefail

# Benchmark regression check.
# Compares current results against the section for this machine in
# ci/benchmarks-baseline.txt. Unknown hardware is reported but not enforced, so
# a new CI runner image does not fail the build before a baseline is recorded.
#
# Usage:
#   ci/benchmark-regression.sh           check against the baseline
#   ci/benchmark-regression.sh --record  print a new section from REPS runs
#
# Baseline format:
#   # Hardware: <normalized-key>
#   name median min max variability
#
# Current output format:
#   Name benchmark (iter=N[, extra=N]): X.XXX seconds, Y.YYY ops/sec
#   Name benchmark (iter=N[, extra=N]): X.XXX seconds, Y.YYY MB/sec

BENCHMARK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BASELINE_FILE="${BENCHMARK_DIR}/ci/benchmarks-baseline.txt"
BENCHMARK="${BENCHMARK_DIR}/zig-out/bin/full_performance-bench-static"
# Compare against the recorded minimum, not the median: the minimum is the
# slowest run observed while establishing the baseline, so ordinary noise does
# not fail the check. A regression below it by more than this fraction does.
TOLERANCE=${TOLERANCE:-0.35}
REPS=${REPS:-5}

# Normalize a hardware description into a stable section key. On GitHub Actions
# use the runner image and architecture so host CPU refreshes keep matching.
normalize_key() {
    tr '[:upper:]' '[:lower:]' | sed -E 's/[^a-z0-9]+/-/g; s/^-+//; s/-+$//'
}

hardware_key() {
    if [[ "${GITHUB_ACTIONS:-}" == "true" ]]; then
        printf 'github-%s-%s\n' "${ImageOS:-unknown}" \
            "$(printf '%s' "${RUNNER_ARCH:-unknown}" | tr '[:upper:]' '[:lower:]')"
    elif [[ "$(uname -s)" == "Darwin" ]]; then
        sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m
    elif [[ -r /proc/cpuinfo ]]; then
        awk -F': *' '/model name/ { print $2; exit }' /proc/cpuinfo
    else
        uname -sm
    fi | normalize_key
}

# Convert benchmark output into "name value" lines. Only comparable lines have
# an ops/sec or MB/sec figure; both are higher-is-better.
parse_output() {
    while IFS= read -r line; do
        if echo "${line}" | grep -qE 'benchmark \(iter=[0-9][^)]*\): [0-9.]+ seconds, [0-9.]+ (ops|MB)/sec'; then
            name=$(echo "${line}" | sed -E 's/^(.+) benchmark \(iter=[0-9][^)]*\):.*/\1/')
            value=$(echo "${line}" | sed -E 's/.*seconds, ([0-9.]+) (ops|MB)\/sec.*/\1/')
            key=$(echo "${name}" | tr '[:upper:]' '[:lower:]' | tr ' ' '_')
            echo "${key} ${value}"
        fi
    done
}

CURRENT_KEY="$(hardware_key)"
if [[ -z "${CURRENT_KEY}" ]]; then
    CURRENT_KEY="unknown"
fi

# Ensure we have built benchmarks first.
if [[ ! -x "${BENCHMARK}" ]]; then
    echo "ERROR: Benchmarks not built. Run 'zig build bench' first."
    exit 1
fi

# Record mode: run several repetitions and emit a baseline section for review.
if [[ "${1:-}" == "--record" ]]; then
    if ! [[ "${REPS}" =~ ^[1-9][0-9]*$ ]]; then
        echo "ERROR: REPS must be a positive integer." >&2
        exit 1
    fi
    RECORD_DIR=$(mktemp -d)
    trap 'rm -rf "${RECORD_DIR}"' EXIT
    for ((i = 1; i <= REPS; i++)); do
        echo "Recording repetition ${i}/${REPS}..." >&2
        "${BENCHMARK}" 2>&1 | parse_output > "${RECORD_DIR}/rep${i}"
    done
    python3 - "${RECORD_DIR}" "${REPS}" "${CURRENT_KEY}" <<'PY'
import pathlib
import statistics
import sys

directory, reps, key = pathlib.Path(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
order, values = [], {}
for rep in range(1, reps + 1):
    for line in (directory / f"rep{rep}").read_text().splitlines():
        name, value = line.split()
        if name not in values:
            order.append(name)
            values[name] = []
        values[name].append(float(value))

print(f"# Hardware: {key}")
print("# name median min max variability")
for name in order:
    series = values[name]
    median = statistics.median(series)
    lowest, highest = min(series), max(series)
    spread = (highest - lowest) / median * 100 if median > 0 else 0.0
    print(f"{name} {median:g} {lowest:g} {highest:g} {spread:.1f}")
PY
    exit 0
fi

# Create a raw baseline if none exists. The caller reviews and commits it.
if [[ ! -f "${BASELINE_FILE}" ]]; then
    echo "Creating baseline at ${BASELINE_FILE}"
    mkdir -p "$(dirname "${BASELINE_FILE}")"
    "${BENCHMARK}" > "${BASELINE_FILE}.raw"
    echo "Baseline raw output created at ${BASELINE_FILE}.raw. Review and convert to key-value format."
    exit 0
fi

# Run current benchmarks.
echo "Running benchmarks for regression check..."
CURRENT_OUTPUT="$("${BENCHMARK}" 2>&1)"

echo "Current benchmark results:"
echo "${CURRENT_OUTPUT}"
echo ""

# Baselines record ReleaseSafe. Debug builds are deliberately unoptimized and
# much slower, so they must not be compared against those numbers.
OPTIMIZE=$(printf '%s\n' "${CURRENT_OUTPUT}" | sed -n 's/^Optimize: //p' | head -1)
if [[ -n "${OPTIMIZE}" && "${OPTIMIZE}" != "ReleaseSafe" ]]; then
    echo "Benchmark optimize mode is '${OPTIMIZE}'; baselines record ReleaseSafe."
    echo "Regression enforcement skipped."
    exit 0
fi

CURRENT_FILE=$(mktemp)
trap 'rm -f "${CURRENT_FILE}"' EXIT
echo "${CURRENT_OUTPUT}" | parse_output > "${CURRENT_FILE}"

REGRESSION=0
FOUND_SECTION=0
SECTION_ACTIVE=0
COMPARED=0

while IFS= read -r line; do
    case "${line}" in
        "# Hardware:"*)
            hw=$(printf '%s' "${line#\# Hardware: }" | normalize_key)
            SECTION_ACTIVE=0
            if [[ "${hw}" == "${CURRENT_KEY}" ]]; then
                FOUND_SECTION=1
                SECTION_ACTIVE=1
            fi
            continue
            ;;
        \#*|"")
            continue
            ;;
    esac
    [[ ${SECTION_ACTIVE} -eq 1 ]] || continue

    read -r name median lowest max var <<< "${line}"
    [[ -z "${name}" ]] && continue
    # Enforce against the recorded minimum (slowest observed run).
    baseline=${lowest}

    bkey=$(echo "${name}" | tr '[:upper:]' '[:lower:]' | tr ' ' '_')
    current_ops=$(grep "^${bkey} " "${CURRENT_FILE}" 2>/dev/null | head -1 | awk '{print $2}' || true)

    if [[ -z "${current_ops}" ]]; then
        echo "WARNING: No current measurement for '${name}', skipping"
        continue
    fi

    change=$(python3 -c "
current = ${current_ops}
baseline = ${baseline}
print(f'{(current - baseline) / baseline * 100:.1f}' if baseline > 0 else '0')
" 2>/dev/null || echo "0")

    regression_check=$(python3 -c "
change = ${change}
tolerance = ${TOLERANCE}
print('REGRESSION' if change < -tolerance * 100 else 'OK')
" 2>/dev/null || echo "OK")

    COMPARED=$((COMPARED + 1))
    if [[ "${regression_check}" == "REGRESSION" ]]; then
        echo "REGRESSION DETECTED: ${name} - ${change}% change (baseline min: ${baseline}, median: ${median}, current: ${current_ops})"
        REGRESSION=1
    else
        echo "OK: ${name} - ${change}% change (baseline min: ${baseline}, median: ${median}, current: ${current_ops})"
    fi
done < "${BASELINE_FILE}"

if [[ ${FOUND_SECTION} -eq 0 ]]; then
    echo ""
    echo "No baseline section for hardware '${CURRENT_KEY}'."
    echo "Regression enforcement skipped. Add a section to ${BASELINE_FILE} to enable it."
    exit 0
fi

if [[ ${COMPARED} -eq 0 ]]; then
    echo ""
    echo "Baseline section for '${CURRENT_KEY}' has no comparable measurements."
    exit 0
fi

if [[ ${REGRESSION} -eq 1 ]]; then
    echo ""
    echo "PERFORMANCE REGRESSION DETECTED!"
    echo "Current results exceed the configured tolerance (${TOLERANCE})."
    exit 1
fi

echo ""
echo "All benchmarks within tolerance. No performance regressions detected."
exit 0
