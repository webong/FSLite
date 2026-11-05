#!/bin/bash

set -e

echo "=== FSLite Prepared Statement Reuse Benchmarks ==="
echo "Running comprehensive benchmarks for streaming writes, random-access writes, and memory usage"
echo ""

# Check if benchmark executables exist
if [ ! -f "zig-out/bin/small_files-bench-static" ] || [ ! -f "zig-out/bin/streaming_writes-bench-static" ] || [ ! -f "zig-out/bin/random_access-bench-static" ] || [ ! -f "zig-out/bin/memory-bench-static" ]; then
    echo "Benchmark executables not found. Building..."
    zig build -Doptimize=ReleaseSafe 2>&1 | tail -20
fi

echo "=== Running Static Library Benchmarks ==="
echo ""

echo "1. Small Files Benchmark (static):"
zig-out/bin/small_files-bench-static

echo ""
echo "2. Streaming Writes Benchmark (static):"
zig-out/bin/streaming_writes-bench-static

echo ""
echo "3. Random Access Benchmarks (static):"
zig-out/bin/random_access-bench-static

echo ""
echo "4. Memory Usage Benchmark (static):"
zig-out/bin/memory-bench-static

echo ""
echo "=== Running Shared Library Benchmarks ==="
echo ""

echo "1. Small Files Benchmark (shared):"
zig-out/bin/small_files-bench-shared

echo ""
echo "2. Streaming Writes Benchmark (shared):"
zig-out/bin/streaming_writes-bench-shared

echo ""
echo "3. Random Access Benchmarks (shared):"
zig-out/bin/random_access-bench-shared

echo ""
echo "4. Memory Usage Benchmark (shared):"
zig-out/bin/memory-bench-shared

echo ""
echo "=== Benchmark Complete ==="