#!/usr/bin/env bash
set -uo pipefail

# Benchmark regression check
# Compares current benchmark results against baseline stored in ci/benchmarks-baseline.txt
# Exits non-zero if performance regresses beyond tolerance
#
# Baseline format (key-value):
#   # Hardware: ...
#   # Zig: ...
#   name median min max variability
#
# Current benchmark output format:
#   Name benchmark (iter=N[, extra=N]): X.XXX seconds, Y.YYY ops/sec

BENCHMARK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BASELINE_FILE="${BENCHMARK_DIR}/ci/benchmarks-baseline.txt"
TOLERANCE=${TOLERANCE:-0.15}  # 15% tolerance

# Ensure we have built benchmarks first
if [[ ! -x "${BENCHMARK_DIR}/zig-out/bin/full_performance-bench-static" ]]; then
    echo "ERROR: Benchmarks not built. Run 'zig build bench' first."
    exit 1
fi

# Create baseline if it doesn't exist
if [[ ! -f "${BASELINE_FILE}" ]]; then
    echo "Creating baseline at ${BASELINE_FILE}"
    mkdir -p "$(dirname "${BASELINE_FILE}")"
    "${BENCHMARK_DIR}/zig-out/bin/full_performance-bench-static" > "${BASELINE_FILE}.raw"
    echo "Baseline raw output created at ${BASELINE_FILE}.raw. Review and convert to key-value format."
    exit 0
fi

# Run current benchmarks
echo "Running benchmarks for regression check..."
CURRENT_OUTPUT="$("${BENCHMARK_DIR}/zig-out/bin/full_performance-bench-static" 2>&1)"

echo "Current benchmark results:"
echo "${CURRENT_OUTPUT}"
echo ""

# Parse current results into temp file for lookup
CURRENT_FILE=$(mktemp)
trap 'rm -f "${CURRENT_FILE}"' EXIT

# Use process substitution to avoid subshell issue
while IFS= read -r line; do
    # Parse lines like: "Path lookup benchmark (iter=30): 0.001 seconds, 42373 ops/sec"
    # or: "Random access reads benchmark (iter=30, file_size=64KB): 0.001 seconds, 30457 ops/sec"
    if echo "${line}" | grep -qE 'benchmark \(iter=[0-9][^)]*\): [0-9.]+ seconds, [0-9.]+ ops/sec'; then
        name=$(echo "${line}" | sed -E 's/^(.+) benchmark \(iter=[0-9][^)]*\):.*/\1/')
        ops=$(echo "${line}" | sed -E 's/.*seconds, ([0-9.]+) ops\/sec.*/\1/')
        # Normalize name: lowercase, replace spaces with underscores
        key=$(echo "${name}" | tr '[:upper:]' '[:lower:]' | tr ' ' '_')
        echo "${key} ${ops}" >> "${CURRENT_FILE}"
    fi
done < <(echo "${CURRENT_OUTPUT}")

# Parse and compare against baseline
REGRESSION=0
while IFS= read -r line; do
    # Skip comments and empty lines
    [[ "${line}" =~ ^# ]] && continue
    [[ -z "${line}" ]] && continue

    # Parse: name median min max variability
    read -r name median min max var <<< "${line}"
    [[ -z "${name}" ]] && continue

    # Normalize baseline name to match current key format
    bkey=$(echo "${name}" | tr '[:upper:]' '[:lower:]' | tr ' ' '_')

    # Look up current value for this key
    current_ops=$(grep "^${bkey} " "${CURRENT_FILE}" 2>/dev/null | head -1 | awk '{print $2}' || true)

    if [[ -z "${current_ops}" ]]; then
        echo "WARNING: No current measurement for '${name}', skipping"
        continue
    fi

    # Calculate change percentage
    change=$(python3 -c "
current = ${current_ops}
baseline = ${median}
if baseline > 0:
    print(f'{(current - baseline) / baseline * 100:.1f}')
else:
    print('0')
" 2>/dev/null || echo "0")

    # Check for regression
    regression_check=$(python3 -c "
change = ${change}
tolerance = ${TOLERANCE}
if change < -tolerance * 100:
    print('REGRESSION')
else:
    print('OK')
" 2>/dev/null || echo "OK")

    if [[ "${regression_check}" == "REGRESSION" ]]; then
        echo "REGRESSION DETECTED: ${name} - ${change}% change (baseline: ${median} ops/sec, current: ${current_ops} ops/sec)"
        REGRESSION=1
    else
        echo "OK: ${name} - ${change}% change (baseline: ${median}, current: ${current_ops})"
    fi
done < "${BASELINE_FILE}"

if [[ ${REGRESSION} -eq 1 ]]; then
    echo ""
    echo "PERFORMANCE REGRESSION DETECTED!"
    echo "Current results exceed tolerance of ${TOLERANCE} (${TOLERANCE*100}%)"
    exit 1
fi

echo ""
echo "All benchmarks within tolerance. No performance regressions detected."
exit 0
