#!/usr/bin/env python3
import os
import subprocess
import time
import math
import statistics
import shutil
import json
import platform
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BENCH_DIR = REPO_ROOT / ".build" / "benchmarks"
BENCH_DIR.mkdir(parents=True, exist_ok=True)
LOOMC = REPO_ROOT / ".build" / "loomc"
if not LOOMC.exists():
    LOOMC = REPO_ROOT / "bin" / "loomc"

# Kept in a separately compiled object so each language sees the same opaque
# runtime input boundary and cannot constant-fold the integer workload away.
OPAQUE_C = r"""
#include <stdint.h>
__attribute__((noinline)) int64_t Opaque(int64_t value) {
    volatile int64_t observed = value;
    return observed;
}
__attribute__((noinline)) int32_t *OpaquePointer(int32_t *value) {
    volatile int32_t *observed = value;
    return (int32_t *)observed;
}
__attribute__((noinline)) void OpaqueTouch(int32_t *value, int64_t iteration) {
    volatile int32_t *observed = value;
    observed[0] = 1 + (int32_t)(iteration & 1);
}
"""

# -----------------------------------------------------------------------------
# Benchmark 1: Recursive Fibonacci (overhead / Instruction selection)
# -----------------------------------------------------------------------------
LOOM_FIB = """
Module FibBench

Block Fib
    Takes N : Signed_32
    Produces Signed_32
    Implements
    If N <= 1
        Give N
    End If
    Give Fib(N - 1) + Fib(N - 2)
End Fib

Block Main
    Declares
        Arg : Signed_32 = 35
        Res : Signed_32
    Implements
        Res = Fib(Arg)
        Write(Res)
End Main
"""

C_FIB = """
#include <stdio.h>
#include <stdint.h>

__attribute__((noinline))
int32_t Fib(int32_t n) {
    if (n <= 1) return n;
    return Fib(n - 1) + Fib(n - 2);
}

int main() {
    volatile int32_t arg = 35;
    int32_t res = Fib(arg);
    printf("%d\\n", res);
    return 0;
}
"""

RUST_FIB = """
#[inline(never)]
fn fib(n: i32) -> i32 {
    if n <= 1 {
        n
    } else {
        fib(n - 1) + fib(n - 2)
    }
}

fn main() {
    let arg = std::hint::black_box(35);
    let res = fib(arg);
    println!("{}", res);
}
"""

# -----------------------------------------------------------------------------
# Benchmark 2: Vectorized Array Sum (102.4M element visits)
# -----------------------------------------------------------------------------
LOOM_VEC = """
Module VecBench

Extern Block OpaquePointer Takes
    P : Signed_32 Pointer
Produces Signed_32 Pointer
End OpaquePointer
Extern Block OpaqueTouch Takes
    P : Signed_32 Pointer
    Iteration : Signed_64
End OpaqueTouch

Block VectorSum
    Takes
        Ptr : Signed_32 Pointer
        Length : Unsigned_64
    Produces Signed_32
    Declares
        Slc : Signed_32 Slice
        Acc : Signed_32 = 0
        I : Unsigned_64 = 1
    Implements
    Slc = Slice(Ptr, Length)
    Loop Until I > Slc.Length
        Acc = Acc + Slc[I]
        I = I + 1
    End Loop
    Give Acc
End VectorSum

Block Main
    Declares
        Arr : Signed_32 Array[1024]
        Slc : Signed_32 Slice
        I : Unsigned_64 = 1
        Iter : Unsigned_64 = 1
        Total : Signed_32 = 0
        Ptr : Signed_32 Pointer
    Implements
    Loop Until I > 1024
        Arr[I] = 1
        I = I + 1
    End Loop
    Ptr = OpaquePointer(Address(Arr[1]))
    Loop Until Iter > 100000
        OpaqueTouch(Ptr, Cast[Signed_64](Iter))
        Total = Total + VectorSum(Ptr, 1024)
        Iter = Iter + 1
    End Loop
    Write(Total)
End Main
"""

C_VEC = """
#include <stdio.h>
#include <stdint.h>
extern int32_t *OpaquePointer(int32_t *);
extern void OpaqueTouch(int32_t *, int64_t);

int32_t VectorSum(const int32_t* ptr, uint64_t len) {
    int32_t acc = 0;
    for (uint64_t i = 0; i < len; ++i) {
        acc += ptr[i];
    }
    return acc;
}

int main() {
    int32_t arr[1024];
    for (int i = 0; i < 1024; ++i) arr[i] = 1;
    int32_t total = 0;
    const int32_t* p = OpaquePointer(arr);
    for (uint64_t iter = 0; iter < 100000; ++iter) {
        OpaqueTouch((int32_t *)p, (int64_t)iter);
        total += VectorSum(p, 1024);
    }
    printf("%d\\n", total);
    return 0;
}
"""

RUST_VEC = """
unsafe extern "C" { fn OpaquePointer(value: *mut i32) -> *mut i32; fn OpaqueTouch(value: *mut i32, iteration: i64); }
fn vector_sum(slice: &[i32]) -> i32 {
    slice.iter().sum()
}

fn main() {
    let arr = [1i32; 1024];
    let p = unsafe { OpaquePointer(arr.as_ptr() as *mut i32) };
    let mut total = 0i32;
    for iteration in 0..100000 {
        unsafe { OpaqueTouch(p, iteration) };
        total = total.wrapping_add(vector_sum(unsafe { std::slice::from_raw_parts(p, 1024) }));
    }
    println!("{}", total);
}
"""

# -----------------------------------------------------------------------------
# Benchmark 3: Opaque-input integer transform (100,000,000 iterations)
# -----------------------------------------------------------------------------
LOOM_MADD = """
Module MaddBench

Extern Block Opaque Takes
    Value : Signed_64
Produces Signed_64
End Opaque

Block ComputeTransform
    Takes
        Base : Signed_64
        Count : Signed_64
    Produces Signed_64
    Declares
        Acc : Signed_64 = Base
        I : Signed_64 = 1
    Implements
    Loop Until I > Count
        Acc = Acc + (Opaque(I) * 7)
        I = I + 1
    End Loop
    Give Acc
End ComputeTransform

Block Main
    Declares
        Res : Signed_64
    Implements
        Res = ComputeTransform(100, 100000000)
        Write(Res)
End Main
"""

C_MADD = """
#include <stdio.h>
#include <stdint.h>
extern int64_t Opaque(int64_t);

__attribute__((noinline))
int64_t ComputeTransform(int64_t base, int64_t count) {
    int64_t acc = base;
    for (int64_t i = 1; i <= count; ++i) {
        acc += Opaque(i) * 7;
    }
    return acc;
}

int main() {
    int64_t res = ComputeTransform(100, 100000000LL);
    printf("%lld\\n", res);
    return 0;
}
"""

RUST_MADD = """
unsafe extern "C" { fn Opaque(value: i64) -> i64; }
#[inline(never)]
fn compute_transform(base: i64, count: i64) -> i64 {
    let mut acc = base;
    for i in 1..=count {
        acc = acc.wrapping_add(unsafe { Opaque(i as i64) }.wrapping_mul(7));
    }
    acc
}

fn main() {
    let res = compute_transform(100, 100000000);
    println!("{}", res);
}
"""

# -----------------------------------------------------------------------------
# Benchmark Runner
# -----------------------------------------------------------------------------
def run_command(cmd, timeout=60):
    cmd = [str(part) for part in cmd]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout)
    if p.returncode != 0:
        raise RuntimeError(f"Command failed: {' '.join(cmd)}\nStdout: {p.stdout}\nStderr: {p.stderr}")
    return p.stdout.strip()

def measure_execution(binary_path, runs=7):
    times = []
    run_command([binary_path])  # warm up once; this run is not included in the samples
    outputs = []
    for _ in range(runs):
        start = time.perf_counter()
        out = run_command([binary_path])
        elapsed = (time.perf_counter() - start) * 1000.0
        times.append(elapsed)
        outputs.append(out)
    if len(set(outputs)) != 1:
        raise RuntimeError(f"Non-deterministic benchmark output from {binary_path}: {outputs}")
    return statistics.median(times), statistics.stdev(times), outputs[0], times

def get_binary_size(path):
    return os.path.getsize(path)

def main():
    print("=" * 80)
    print("      Loom Native Benchmark Suite (equivalent integer workloads)")
    print(f"      Host: {platform.platform()} | Machine: {platform.machine()}")
    print("      Comparators: Loom --release, Clang -O3 -mcpu=native, Rust -O target-cpu=native")
    print("      Timing: one warm-up, seven samples, median and standard deviation")
    print("=" * 80)

    benchmarks = [
        ("Recursive Fibonacci (N=35)", "fib", LOOM_FIB, C_FIB, RUST_FIB),
        ("Array Sum (102.4M elements)", "vec", LOOM_VEC, C_VEC, RUST_VEC),
        ("Integer Transform (100M iterations)", "madd", LOOM_MADD, C_MADD, RUST_MADD),
    ]

    results = []
    rustc = shutil.which("rustc")
    opaque_src = BENCH_DIR / "opaque.c"
    opaque_obj = BENCH_DIR / "opaque.o"
    opaque_src.write_text(OPAQUE_C)
    run_command(["clang", "-O3", "-c", opaque_src, "-o", opaque_obj])
    compiler_versions = {"loomc": run_command([LOOMC, "--version"]), "clang": run_command(["clang", "--version"]).splitlines()[0]}
    if rustc:
        compiler_versions["rustc"] = run_command([rustc, "--version"])

    for name, tag, loom_src, c_src, rust_src in benchmarks:
        print(f"\n--- Running Benchmark: {name} ---")
        loom_file, loom_obj, loom_bin = (BENCH_DIR / f"{tag}.loom", BENCH_DIR / f"{tag}_loom.o", BENCH_DIR / f"{tag}_loom")
        c_file, c_bin = BENCH_DIR / f"{tag}.c", BENCH_DIR / f"{tag}_c"
        loom_file.write_text(loom_src)
        c_file.write_text(c_src)
        run_command([LOOMC, "--object", "--release", "--entry", loom_file, loom_obj])
        run_command(["clang", "-O3", "-mcpu=native", loom_obj, opaque_obj, "-o", loom_bin])
        run_command(["clang", "-O3", "-mcpu=native", c_file, opaque_obj, "-o", c_bin])
        implementations = {"Loom": (loom_bin, get_binary_size(loom_bin)), "C": (c_bin, get_binary_size(c_bin))}
        if rustc:
            rust_file, rust_bin = BENCH_DIR / f"{tag}.rs", BENCH_DIR / f"{tag}_rust"
            rust_file.write_text(rust_src)
            run_command([rustc, "-O", "-C", "target-cpu=native", rust_file,
                         "-C", f"link-arg={opaque_obj}", "-o", rust_bin])
            implementations["Rust"] = (rust_bin, get_binary_size(rust_bin))

        measurements = {}
        outputs = {}
        for language, (binary, size) in implementations.items():
            median, deviation, output, samples = measure_execution(binary)
            outputs[language] = output
            measurements[language] = {"median_ms": median, "stdev_ms": deviation,
                                      "samples_ms": samples, "executable_bytes": size,
                                      "raw_stdout": output}
            print(f"  [{language:<4}] median {median:8.2f} ms | stdev {deviation:6.2f} ms | executable {size:7d} B | checksum: {output}")
        if len(set(outputs.values())) != 1:
            raise RuntimeError(f"Checksum mismatch for {name}: {outputs}")
        print(f"  Equivalent checksum confirmed: {next(iter(outputs.values()))}")
        results.append({"name": name, "implementations": measurements})

    raw = {"host": platform.platform(), "machine": platform.machine(),
           "compilers": compiler_versions, "runs": 7, "warmups": 1, "benchmarks": results}
    raw_path = BENCH_DIR / "results.json"
    raw_path.write_text(json.dumps(raw, indent=2) + "\n")
    print(f"\nRaw checksums and timing samples: {raw_path}")

if __name__ == "__main__":
    main()
