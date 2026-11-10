# FSLite benchmarks

The benchmarks are C consumers of the public ABI. Each executable links exactly
one benchmark entry point with either the static or shared engine. No benchmark
changes the engine, volume format, journal mode, synchronous setting, or transaction
semantics. The regular integration tests remain independent.

## Running

Use Zig from `.zig-version` and an optimized build:

```sh
zig build bench -Doptimize=ReleaseSafe
zig build bench-smoke -Doptimize=ReleaseSafe
zig build build-bench -Doptimize=ReleaseSafe
```

`bench` and `bench-smoke` build all executables before running them sequentially,
so workloads do not compete with one another or their compilation. Every explicit
invocation runs again; benchmark runs are not cached. Do not run multiple build
commands or external disk-intensive jobs concurrently when comparing results.

`build-bench` installs libraries, headers, and eight benchmark executables under
`zig-out`. To run just one suite from this checkout:

```sh
./zig-out/bin/small_files-bench-static
./zig-out/bin/streaming_writes-bench-shared --smoke
./zig-out/bin/random_access-bench-static
./zig-out/bin/memory_usage-bench-static
```

Each executable accepts either no arguments (full workloads) or `--smoke`.
Unexpected arguments fail with exit status 2. Shared executables are local build
artifacts, not relocatable release packages.

Private directories are created under `TMPDIR`, falling back to `/tmp`. Choose
`TMPDIR` on the storage you intend to measure; it must already exist and be
writable. Successful runs and reported errors clean their temporary volumes.
A forced process kill can leave a `fslite-bench-*` directory. Full runs need several
hundred MiB of free disk, including SQLite rollback journals. Ordinary tests and
CI smoke runs do not execute the full workloads.

## Workloads

| Suite | Full workload | Smoke workload |
| --- | --- | --- |
| Small files | 1,000 files per size: 0, 128, 1,024, and 4,096 bytes; create then replace; autocommit and one explicit transaction | 8 files per size, same modes |
| Streaming | Create then replace 128 MiB + 17 bytes, using 4,093-byte and 65,536-byte input fragments | 2 MiB + 17 bytes, same fragment sizes |
| Random access | 1,000 deterministic 4 KiB patches in a pre-created, dense 32 MiB file; autocommit and one explicit transaction | 16 patches in a 2 MiB file |
| Memory | Stream 1, 16, and 128 MiB in separate child processes, each using a 64 KiB caller buffer | 1, 4, and 8 MiB |

Small-file timing includes path formatting, the write calls, and transaction
begin/commit where applicable. An operation is one complete file creation or
replacement. Each streaming case is one atomic file replacement, including
writer open, all input fragments, finish (commit), and close. It does not allocate
an input buffer proportional to the file size.

Random-access files are populated before timing. Seed `0x51f17e` determines offsets
and patch payloads, with every fourth patch crossing a storage-chunk boundary.
The same sequence runs in both transaction modes. A separate byte-array model
verifies the entire final file, including unmodified bytes and overlapping patches.
The model and workload generation are outside the timed region. This model's
memory is not used for the memory-usage benchmark.

After timing, every case closes and reopens the volume, checks the expected file
size and content, and runs `fslite_check`. Any validation failure exits nonzero.
Reported write throughput excludes this validation work. The CI smoke step checks
all suites and both linkages on macOS and Linux, in Debug and ReleaseSafe; it has
no speed or memory thresholds because hosted runner measurements are noisy.

## Interpreting output

Header lines identify OS, architecture, linkage, optimization, ABI, workload mode,
temporary storage location, and the engine's DELETE journal/FULL synchronous
settings. Result lines are whitespace-separated `key=value` fields:

- `operations`, `bytes`: completed file writes or patches and submitted payload bytes.
- `seconds`: elapsed `CLOCK_MONOTONIC` wall time, including commit/fsync waits.
- `ops_per_second`, `mib_per_second`: throughput; MiB means 1,048,576 bytes.
- Memory lines additionally report logical file bytes, caller buffer bytes, host
  volume bytes, and baseline/final process peak RSS in MiB.

RSS comes from `getrusage(RUSAGE_SELF)`: macOS reports bytes; Linux reports KiB.
Both are normalized to MiB. Each file size runs in a new child forked from an idle
parent before any engine workload, avoiding the previous case's high-water mark.
The baseline is recorded after opening the volume and initializing the buffer;
the final peak is captured after writer finish, before readback/integrity checks.

Peak RSS includes the runtime, stack, SQLite cache, and caller buffer. It is not
current resident memory, an allocation counter, engine-only memory, or a leak
test. Baseline/final peaks are reported separately; their difference is not a
measurement of live allocations. Child processes and buffered filesystem I/O are
POSIX-specific, matching the current test platforms.

These are warm-process, ordinary-cache measurements, without forced cache drops
or hardware power-loss simulation. A batched transaction amortizes commit cost
and must not be compared as equivalent to independently durable per-call writes.
Repeat full runs on an otherwise idle machine, and retain the commit, compiler,
OS, CPU, storage type/location, and raw output when comparing revisions. No
performance targets or cross-machine conclusions are claimed by this suite.

## Regression baselines

`ci/benchmark-regression.sh` runs the combined `full_performance` executable and
compares each result with `ci/benchmarks-baseline.txt`. Baseline sections are
keyed by hardware (`# Hardware: <normalized-key>`); on GitHub Actions the key is
`github-<ImageOS>-<arch>`. A machine without a matching section reports its
results and skips enforcement, so a new runner image cannot fail the build before
a baseline is recorded.

Record five full repetitions on an otherwise idle machine. Store the median, the
slowest (min), the fastest (max), and `(max-min)/median` as variability.
`ci/benchmark-regression.sh --record` does this automatically and prints a
section ready to paste into the baseline; `REPS` changes the repetition count.
The **Benchmark baseline** workflow runs the same command on each CI runner image
and publishes the sections in its job summary.

Enforcement uses the recorded minimum as the floor with a 35% default tolerance
(`TOLERANCE`), because the short lookup/read workloads still vary more than the
streaming workload. Re-record a section whenever the workload or benchmark
changes; mixing values from different builds produces false regressions.
