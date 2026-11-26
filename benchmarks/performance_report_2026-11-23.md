# Loom, C, and Rust Performance Report

**Run date:** 2026-11-23  
**Host:** macOS 27.2.1, Apple Silicon arm64  
**Compilers:** Loom 1.4.0 / LLVM 23.1.6; Homebrew Clang 23.1.6; rustc 1.99.1

## Method

The reproducible runner is [`tests/benchmarks.py`](../tests/benchmarks.py). It uses one warm-up and seven timed samples, checks equal output, and records medians, standard deviations, compile/link time, peak process RSS, and executable size. All implementations use release optimization and target the host CPU. Raw data is in [`results_2026-11-23.json`](results_2026-11-23.json).

This is a single-machine comparison. Timings include process startup and output. Compile times are one sample and are not reliable compiler rankings. “Peak memory” is OS-reported RSS. Rust binaries use its standard output/runtime; this is not a comparison against no-std Rust.

## Runtime results

| Workload | Loom | C | Rust | Loom vs C | Loom vs Rust |
|---|---:|---:|---:|---:|---:|
| Recursive Fibonacci, N=35 | 30.45 ms | 30.38 ms | 31.31 ms | 1.00× | 0.97× |
| Array sum, 102.4M visits | 6.02 ms | 5.73 ms | 6.03 ms | 1.05× slower | 1.00× |
| Integer transform, 100M iterations | 98.58 ms | 98.99 ms | 99.31 ms | 1.00× (0.4% faster) | 0.99× |
| 16 MiB heap write/read | 5.14 ms | 5.37 ms | 5.58 ms | 0.96× | 0.92× |
| Float32 vector add, 20 × 4M elements | 29.32 ms | 27.27 ms | 27.75 ms | 1.08× slower | 1.06× slower |
| Fixed-array fill, 16 × 4 MiB | 4.33 ms | 2.76 ms | 3.12 ms | 1.57× slower | 1.39× slower |
| Same-index map, 16 × 256K elements | 3.37 ms | 3.34 ms | 3.65 ms | 1.01× slower | 0.92× |

The output checksums matched for all workloads. The same-index map benchmark now falls below the compiler’s parallelization threshold and runs sequentially; this avoids adding thread-pool dispatch to a case that did not demonstrate a parallel benefit. The timings show parity on several scalar workloads, but no general Loom speed advantage. The Float32 vector and fill kernels are slower than optimized C in this sample and need further profiling and threshold tuning.

## Peak resident memory

| Workload | Loom | C | Rust |
|---|---:|---:|---:|
| Fibonacci | 1.61 MiB | 1.59 MiB | 1.77 MiB |
| Array sum | 1.61 MiB | 1.61 MiB | 1.78 MiB |
| Integer transform | 1.59 MiB | 1.59 MiB | 1.78 MiB |
| 16 MiB heap pass | 17.64 MiB | 17.63 MiB | 17.80 MiB |
| Float32 vector add | 49.84 MiB | 49.66 MiB | 49.84 MiB |
| Fixed-array fill | 5.72 MiB | 5.61 MiB | 5.77 MiB |
| Same-index map | 4.58 MiB | 4.59 MiB | 4.77 MiB |

The explicit heap workload has essentially the same memory footprint in all three languages. For small cases, runtime/process overhead dominates.

## Executable size and build time

Loom binaries ranged from **33.7–36.1 KiB** for scalar workloads and **55.1 KiB** for the vector/fill workloads that link the parallel runtime. C ranged from **33.6–33.8 KiB**. Rust ranged from **470 KiB raw / 435 KiB stripped** for most workloads and **490 KiB / 454 KiB stripped** for vector addition. The linked Loom runtime costs about 17 KiB in these programs. Dynamic host libraries are not included in these executable sizes.

Observed compile/link times were 115–147 ms for Loom, 147–186 ms for C, and 107–515 ms for Rust. These single measurements are affected by cache and startup state and should not be used to rank toolchains.

## Compiler changes and optimization findings

Explicit `Wrapping_Add`, `Wrapping_Sub`, and `Wrapping_Mul` intrinsics give code that needs fixed-width modulo behavior a path without checked-overflow branches. Ordinary arithmetic remains checked. This allowed the earlier reduction and Fibonacci benchmarks to approach optimized C/Rust while preserving checked semantics by default. The benchmark sources use wrapping arithmetic only where their input ranges or intended modulo behavior make it correct.

The compiler has a native Float32 elementwise path for `Vector_Add`, `Vector_Sub`, `Vector_Mul`, and `Vector_Scale`. It falls back to sequential execution for small inputs, partial overlap, nested worker execution, or active explicit tasks; exact in-place aliases remain supported. This path is currently slower than C by about 8% in the 48 MiB vector-add run.

Two restricted fixed-array loop transforms are also present: large constant fills, and elementwise loops. Elementwise recognition requires an exact compile-time `1..N` range, ordered writes to local fixed arrays at that same index, scalar expressions with same-index reads, no unknown calls, no cross-index reads, and no pointer containers. It supports multiple writes per iteration, including a later write that consumes an earlier same-index result. The threshold is 524,288 elements. Integer fills with repeated-byte patterns use `memset`; other constant fills can use the worker pool. The tested nonuniform fill was about 57% slower than C, so profitability requires more profiling and target-specific tuning.

Loom's compiler diagnostics now support a machine-readable JSON mode and UTF-16 LSP-compatible ranges. See [`docs/compiler-diagnostics.md`](../docs/compiler-diagnostics.md). The formatter/style recommendations are in [`docs/style.md`](../docs/style.md).

## Recommendations

1. Profile vector and fill kernels with hardware counters; compare sequential LLVM output, worker-pool overhead, and C/Rust implementations under the same thread count. Only select parallel paths when per-target measurements show a gain.
2. Add compiler optimization remarks for vectorization failures, bounds-check elimination, alias uncertainty, and checked arithmetic.
3. Build reusable read/write/call/escape/trap effect summaries and alias/dependence analysis before extending automatic loop parallelization.
4. Specify reduction overflow and floating-point determinism before implementing implicit reductions.
5. Add race-sanitized stress tests for worker/task interaction, nested calls, shutdown, cancellation, and many simultaneous requests.
6. Keep checked arithmetic as the default; benchmark explicitly wrapping arithmetic only where modulo behavior is intended.
7. Add repeatable cold/warm compiler throughput, energy, and scaling benchmarks across targets before making broad performance claims.

## Limits

These seven workloads on one Apple Silicon host do not establish cross-platform rankings, broad memory-safety comparisons, energy use, multithread scaling, or a claim that Loom beats C. The results are local engineering measurements, not release performance guarantees.
