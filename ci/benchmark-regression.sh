#!/usr/bin/env bash
set -euo pipefail

# Benchmark regression check
# Compares current benchmark results against baseline stored in ci/benchmarks-baseline.txt
# Exits non-zero if performance regresses beyond tolerance

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
    "${BENCHMARK_DIR}/zig-out/bin/full_performance-bench-static" > "${BASELINE_FILE}"
    echo "Baseline created. Review and commit it."
    exit 0
fi

# Run current benchmarks
echo "Running benchmarks for regression check..."
CURRENT_OUTPUT="$("${BENCHMARK_DIR}/zig-out/bin/full_performance-bench-static" 2>&1)"

echo "Current benchmark results:"
echo "${CURRENT_OUTPUT}"
echo ""

# Parse and compare
REGRESSION=0
while IFS= read -r line; do
    # Parse lines like: "Path lookup benchmark (iter=30): 0.001 seconds, 42373 ops/sec"
    if [[ "${line}" =~ ^(.+)\ benchmark\ \(iter=[0-9]+\):\ [0-9.]+\ seconds,\ ([0-9.]+)\ ops/sec ]]; then
        name="${BASH_REMATCH[1]}"
        current_ops="${BASH_REMATCH[2]}"

        # Find baseline value by parsing the baseline file
        baseline_ops=$(grep "${name}" "${BASELINE_FILE}" | grep -oP '\d+\.?\d*\ ops/sec' | head -1 | grep -oP '^\d+\.?\d*' || echo "0")

        if [[ "${baseline_ops}" == "0" ]]; then
            echo "WARNING: No baseline for '${name}', skipping"
            continue
        fi

        # Calculate change percentage
        change=$(python3 -c "
current = ${current_ops}
baseline = ${baseline_ops}
if baseline > 0:
    print(f'{(current - baseline) / baseline * 100:.1f}')
else:
    print('0')
" 2>/dev/null || echo "0")

        # Check for regression (performance decrease beyond tolerance)
        regression_check=$(python3 -c "
change = ${change}
tolerance = ${TOLERANCE}
if change < -tolerance * 100:
    print('REGRESSION')
else:
    print('OK')
" 2>/dev/null || echo "OK")

        if [[ "${regression_check}" == "REGRESSION" ]]; then
            echo "REGRESSION DETECTED: ${name} - ${change}% change (baseline: ${baseline_ops} ops/sec, current: ${current_ops} ops/sec)"
            REGRESSION=1
        else
            echo "OK: ${name} - ${change}% change"
        fi
    fi
done <<< "${CURRENT_OUTPUT}"

if [[ ${REGRESSION} -eq 1 ]]; then
    echo ""
    echo "PERFORMANCE REGRESSION DETECTED!"
    echo "Current results exceed tolerance of ${TOLERANCE} (${TOLERANCE*100}%)"
    exit 1
fi

echo ""
echo "All benchmarks within tolerance. No performance regressions detected."
exit 0