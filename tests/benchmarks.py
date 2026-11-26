#!/usr/bin/env python3
import os
import subprocess
import time
import math
import statistics
import shutil
import json
import platform
import sys
try:
    import resource
except ImportError:
    resource = None
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BENCH_DIR = REPO_ROOT / ".build" / "benchmarks"
BENCH_DIR.mkdir(parents=True, exist_ok=True)
LOOMC = REPO_ROOT / ".build-release" / "loomc"
if not LOOMC.exists():
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
__attribute__((noinline)) void OpaqueFloatTouch(float *value, int64_t iteration) {
    volatile float *observed = value;
    observed[0] = 1.0f + (float)(iteration & 1);
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
    Give Wrapping_Add(Fib(N - 1), Fib(N - 2))
End Fib

Block Main
    Declares
        Arg : Signed_32 = 35
        Res : Signed_32
    Implements
        Res = Fib(Arg)
        Print(Res)
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
        fib(n - 1).wrapping_add(fib(n - 2))
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
    Implements
    Slc = Slice(Ptr, Length)
    Loop Value in Slc
        Acc = Wrapping_Add(Acc, Value)
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
    Print(Total)
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
        Print(Res)
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
# Benchmark 4: Heap allocation and 16 MiB write/read pass
# -----------------------------------------------------------------------------
LOOM_MEMORY = """
Module MemoryBench

Extern Block OpaqueTouch Takes
    P : Signed_32 Pointer
    Iteration : Signed_64
End OpaqueTouch

Block Main
Declares
    Buffer : Signed_32 Pointer
    I : Signed_64 = 1
    Total : Signed_64 = 0
Implements
    Buffer = Allocate(4194304)
    Loop Until I > 4194304
        Buffer[I] = Cast[Signed_32](I)
        I = Wrapping_Add(I, 1)
    End Loop
    OpaqueTouch(Buffer, 0)
    I = 1
    Loop Until I > 4194304
        Total = Wrapping_Add(Total, Cast[Signed_64](Buffer[I]))
        I = Wrapping_Add(I, 1)
    End Loop
    Print(Total)
    Release(Buffer)
End Main
"""

C_MEMORY = r"""
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
extern void OpaqueTouch(int32_t *, int64_t);
int main(void) {
    const uint64_t n = 4194304;
    int32_t *buffer = (int32_t *)malloc(n * sizeof(int32_t));
    if (!buffer) return 2;
    for (uint64_t i = 0; i < n; ++i) buffer[i] = (int32_t)(i + 1);
    OpaqueTouch(buffer, 0);
    uint64_t total = 0;
    for (uint64_t i = 0; i < n; ++i) total += (uint64_t)(uint32_t)buffer[i];
    printf("%llu\n", (unsigned long long)total);
    free(buffer);
    return 0;
}
"""

RUST_MEMORY = r"""
use std::hint::black_box;
unsafe extern "C" { fn OpaqueTouch(value: *mut i32, iteration: i64); }
fn main() {
    let n = 4_194_304usize;
    let mut buffer = vec![0i32; n];
    for i in 0..n { buffer[i] = (i + 1) as i32; }
    unsafe { OpaqueTouch(buffer.as_mut_ptr(), 0); }
    let mut total = 0u64;
    for value in &buffer { total = total.wrapping_add(*value as u32 as u64); }
    println!("{}", black_box(total));
}
"""

# -----------------------------------------------------------------------------
# Benchmark 5: Implicit parallel vector primitive (4M Float_32 values)
# -----------------------------------------------------------------------------
LOOM_VECTOR = """
Module VectorBench

Extern Block OpaqueFloatTouch Takes
    P : Float_32 Pointer
    Iteration : Signed_64
End OpaqueFloatTouch

Block Main
Declares
    Left : Float_32 Pointer
    Right : Float_32 Pointer
    Output : Float_32 Pointer
    N : Signed_64 = 4194304
    I : Signed_64 = 1
    Repeat : Signed_64 = 0
Implements
    Left = Allocate(N)
    Right = Allocate(N)
    Output = Allocate(N)
    Loop Until I > N
        Left[I] = 1.0
        Right[I] = 2.0
        I = Wrapping_Add(I, 1)
    End Loop
    Loop Until Repeat >= 20
        OpaqueFloatTouch(Left, Repeat)
        Vector_Add(Left, Right, Output, N)
        Repeat = Wrapping_Add(Repeat, 1)
    End Loop
    Print(Output[1])
    Print(Output[N])
    Release(Left)
    Release(Right)
    Release(Output)
End Main
"""

C_VECTOR = r"""
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
extern void OpaqueFloatTouch(float *, int64_t);
int main(void) {
    const uint64_t n = 4194304;
    float *left = (float *)malloc(n * sizeof(float));
    float *right = (float *)malloc(n * sizeof(float));
    float *out = (float *)malloc(n * sizeof(float));
    if (!left || !right || !out) return 2;
    for (uint64_t i = 0; i < n; ++i) { left[i] = 1.0f; right[i] = 2.0f; }
    for (int64_t i = 0; i < 20; ++i) {
        OpaqueFloatTouch(left, i);
        for (uint64_t j = 0; j < n; ++j) out[j] = left[j] + right[j];
    }
    printf("%.0f\n%.0f\n", out[0], out[n - 1]);
    free(left); free(right); free(out);
    return 0;
}
"""

RUST_VECTOR = r"""
unsafe extern "C" { fn OpaqueFloatTouch(value: *mut f32, iteration: i64); }
fn main() {
    let n = 4_194_304usize;
    let mut left = vec![1.0f32; n];
    let right = vec![2.0f32; n];
    let mut out = vec![0.0f32; n];
    for i in 0..20 {
        unsafe { OpaqueFloatTouch(left.as_mut_ptr(), i); }
        for j in 0..n { out[j] = left[j] + right[j]; }
    }
    println!("{:.0}\n{:.0}", out[0], out[n - 1]);
}
"""

# -----------------------------------------------------------------------------
# Benchmark 6: Compiler-proven implicit parallel fixed-array fill
# -----------------------------------------------------------------------------
LOOM_PARALLEL_FILL = """
Module ParallelFillBench

Block Main Declares
    Values : Signed_32 Array[1048576]
    Repeat : Signed_32 = 0
Implements
    Loop Until Repeat >= 16
        Loop I in 1..1048576
            Values[I] = 16909060
        End Loop
        Repeat = Wrapping_Add(Repeat, 1)
    End Loop
    Print(Values[1])
    Print(Values[524288])
    Print(Values[1048576])
End Main
"""

C_PARALLEL_FILL = r"""
#include <stdint.h>
#include <stdio.h>
enum { N = 1048576 };
static int32_t values[N];
__attribute__((noinline)) static void fill(void) {
    for (uint64_t i = 0; i < N; ++i) values[i] = 16909060;
}
int main(void) {
    for (int repeat = 0; repeat < 16; ++repeat) fill();
    printf("%d\n%d\n%d\n", values[0], values[N / 2 - 1], values[N - 1]);
    return 0;
}
"""

RUST_PARALLEL_FILL = r"""
const N: usize = 1_048_576;
#[inline(never)] fn fill(values: &mut [i32; N]) {
    for value in values.iter_mut() { *value = 16_909_060; }
}
fn main() {
    let mut values = [0i32; N];
    for _ in 0..16 { fill(&mut values); }
    println!("{}\n{}\n{}", values[0], values[N / 2 - 1], values[N - 1]);
}
"""

# -----------------------------------------------------------------------------
# Benchmark 7: Compiler-proven implicit same-index array map
# -----------------------------------------------------------------------------
LOOM_PARALLEL_MAP = """
Module ParallelMapBench

Block Main Declares
    Left : Signed_32 Array[262144]
    Right : Signed_32 Array[262144]
    Output : Signed_32 Array[262144]
    Repeat : Signed_32 = 0
Implements
    Loop I in 1..262144
        Left[I] = I
        Right[I] = 3
    End Loop
    Loop Until Repeat >= 16
        Loop I in 1..262144
            Output[I] = Wrapping_Add(Wrapping_Mul(Left[I], Right[I]), 1)
        End Loop
        Repeat = Wrapping_Add(Repeat, 1)
    End Loop
    Print(Output[1])
    Print(Output[131072])
    Print(Output[262144])
End Main
"""

C_PARALLEL_MAP = r"""
#include <stdint.h>
#include <stdio.h>
enum { N = 262144 };
static int32_t left[N], right[N], output[N];
__attribute__((noinline)) static void map(void) {
    for (uint64_t i = 0; i < N; ++i) output[i] = (int32_t)((uint32_t)left[i] * (uint32_t)right[i] + 1u);
}
int main(void) {
    for (uint64_t i = 0; i < N; ++i) { left[i] = (int32_t)i + 1; right[i] = 3; }
    for (int repeat = 0; repeat < 16; ++repeat) map();
    printf("%d\n%d\n%d\n", output[0], output[N / 2 - 1], output[N - 1]);
    return 0;
}
"""

RUST_PARALLEL_MAP = r"""
const N: usize = 262_144;
#[inline(never)] fn map(left: &[i32; N], right: &[i32; N], output: &mut [i32; N]) {
    for i in 0..N { output[i] = left[i].wrapping_mul(right[i]).wrapping_add(1); }
}
fn main() {
    let mut left = [0i32; N];
    let mut right = [3i32; N];
    let mut output = [0i32; N];
    for i in 0..N { left[i] = i as i32 + 1; }
    for _ in 0..16 { map(&left, &right, &mut output); }
    println!("{}\n{}\n{}", output[0], output[N / 2 - 1], output[N - 1]);
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
    helper = subprocess.run([sys.executable, __file__, "--measure-child", str(binary_path), str(runs)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=60)
    if helper.returncode != 0:
        raise RuntimeError(f"Benchmark failed: {helper.stderr}")
    measurement = json.loads(helper.stdout)
    times = measurement["samples_ms"]
    outputs = measurement["outputs"]
    if len(set(outputs)) != 1:
        raise RuntimeError(f"Non-deterministic benchmark output from {binary_path}: {outputs}")
    return statistics.median(times), statistics.stdev(times), outputs[0], times, measurement["peak_rss_bytes"]

def measure_child(binary_path, runs):
    outputs = []
    samples = []
    p = subprocess.run([binary_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=60)
    if p.returncode != 0:
        raise RuntimeError(p.stderr)
    # Warm-up is deliberately discarded.
    for _ in range(runs):
        start = time.perf_counter()
        p = subprocess.run([binary_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=60)
        samples.append((time.perf_counter() - start) * 1000.0)
        if p.returncode != 0:
            raise RuntimeError(p.stderr)
        outputs.append(p.stdout.strip())
    rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss if resource is not None else None
    if rss is not None and platform.system() != "Darwin":
        rss *= 1024
    print(json.dumps({"samples_ms": samples, "outputs": outputs,
                      "peak_rss_bytes": int(rss) if rss is not None else None}))

def get_binary_size(path):
    return os.path.getsize(path)

def main():
    print("=" * 80)
    print("      Loom Native Benchmark Suite (equivalent integer workloads)")
    print(f"      Host: {platform.platform()} | Machine: {platform.machine()}")
    print("      Comparators: Loom --release, Clang -O3 -mcpu=native, Rust -O target-cpu=native")
    print("      Timing: one warm-up, seven samples, median and standard deviation")
    print("      Additional metrics: compiler time, peak resident set, raw and stripped executable size")
    print("=" * 80)

    benchmarks = [
        ("Recursive Fibonacci (N=35)", "fib", LOOM_FIB, C_FIB, RUST_FIB),
        ("Array Sum (102.4M elements)", "vec", LOOM_VEC, C_VEC, RUST_VEC),
        ("Integer Transform (100M iterations)", "madd", LOOM_MADD, C_MADD, RUST_MADD),
        ("Heap Allocation + 16 MiB Write/Read", "memory", LOOM_MEMORY, C_MEMORY, RUST_MEMORY),
        ("Float32 Vector_Add + 48 MiB working set", "vector", LOOM_VECTOR, C_VECTOR, RUST_VECTOR),
        ("Compiler-proven implicit array fill (16 × 4 MiB)", "parallel_fill", LOOM_PARALLEL_FILL, C_PARALLEL_FILL, RUST_PARALLEL_FILL),
        ("Compiler-proven implicit same-index map (16 × 256K elements)", "parallel_map", LOOM_PARALLEL_MAP, C_PARALLEL_MAP, RUST_PARALLEL_MAP),
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
        compile_start = time.perf_counter()
        run_command([LOOMC, "--object", "--release", "--entry", loom_file, loom_obj])
        loom_compile_ms = (time.perf_counter() - compile_start) * 1000.0
        runtime_archive = LOOMC.parent / "libloom_rt.a"
        if not runtime_archive.exists():
            runtime_archive = REPO_ROOT / ".build" / "libloom_rt.a"
        if not runtime_archive.exists():
            raise RuntimeError(f"Loom runtime archive not found: {runtime_archive}")
        compile_start = time.perf_counter()
        run_command(["clang", "-O3", "-mcpu=native", loom_obj, opaque_obj, runtime_archive, "-lm", "-o", loom_bin])
        loom_compile_ms += (time.perf_counter() - compile_start) * 1000.0
        compile_start = time.perf_counter()
        run_command(["clang", "-O3", "-mcpu=native", c_file, opaque_obj, "-o", c_bin])
        c_compile_ms = (time.perf_counter() - compile_start) * 1000.0
        implementations = {"Loom": (loom_bin, loom_compile_ms), "C": (c_bin, c_compile_ms)}
        if rustc:
            rust_file, rust_bin = BENCH_DIR / f"{tag}.rs", BENCH_DIR / f"{tag}_rust"
            rust_file.write_text(rust_src)
            compile_start = time.perf_counter()
            run_command([rustc, "-O", "-C", "target-cpu=native", rust_file,
                         "-C", f"link-arg={opaque_obj}", "-o", rust_bin])
            implementations["Rust"] = (rust_bin, (time.perf_counter() - compile_start) * 1000.0)

        measurements = {}
        outputs = {}
        for language, (binary, compile_ms) in implementations.items():
            size = get_binary_size(binary)
            stripped = BENCH_DIR / f"{tag}_{language.lower()}_stripped"
            if platform.system() == "Darwin":
                run_command(["strip", "-S", "-o", stripped, binary])
            else:
                shutil.copy2(binary, stripped)
                if shutil.which("strip"):
                    run_command(["strip", "--strip-unneeded", stripped])
            median, deviation, output, samples, peak_rss = measure_execution(binary)
            outputs[language] = output
            measurements[language] = {"median_ms": median, "stdev_ms": deviation,
                                      "samples_ms": samples, "executable_bytes": size,
                                      "stripped_executable_bytes": get_binary_size(stripped),
                                      "peak_rss_bytes": peak_rss, "compile_and_link_ms": compile_ms,
                                      "raw_stdout": output}
            rss_text = f"{peak_rss / (1024 * 1024):.2f} MiB" if peak_rss is not None else "unavailable"
            print(f"  [{language:<4}] median {median:8.2f} ms | stdev {deviation:6.2f} ms | "
                  f"peak RSS {rss_text:>12} | binary {size:7d} B ({get_binary_size(stripped):7d} B stripped) | "
                  f"compile+link {compile_ms:8.1f} ms | checksum: {output}")
        if len(set(outputs.values())) != 1:
            raise RuntimeError(f"Checksum mismatch for {name}: {outputs}")
        print(f"  Equivalent checksum confirmed: {next(iter(outputs.values()))}")
        results.append({"name": name, "implementations": measurements})

    raw = {"host": platform.platform(), "machine": platform.machine(),
           "compilers": compiler_versions, "optimization": "Loom --release; Clang -O3 -mcpu=native; Rust -O -C target-cpu=native",
           "runs": 7, "warmups": 1, "benchmarks": results}
    raw_path = BENCH_DIR / "results.json"
    raw_path.write_text(json.dumps(raw, indent=2) + "\n")
    print(f"\nRaw checksums and timing samples: {raw_path}")

if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--measure-child":
        try:
            measure_child(sys.argv[2], int(sys.argv[3]))
        except Exception as error:
            print(str(error), file=sys.stderr)
            raise SystemExit(1)
    else:
        main()
