#!/usr/bin/env python3
import os
import sys
import subprocess
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LOOMC = (REPO_ROOT / "build" / "loomc").resolve()
LOOM_RT = (REPO_ROOT / "build" / "libloom_rt.a").resolve()

def compile_and_run_expect_panic(source: str, release: bool = False):
    with tempfile.TemporaryDirectory() as tmp:
        src_path = Path(tmp) / "main.loom"
        obj_path = Path(tmp) / "out.o"
        bin_path = Path(tmp) / "out"
        src_path.write_text(source)
        cmd = [str(LOOMC), "--object", "--entry", str(src_path), str(obj_path)]
        if release:
            cmd.append("--release")
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"loomc compilation failed:\nstdout:\n{res.stdout}\nstderr:\n{res.stderr}")
        link_cmd = ["clang", str(obj_path), "-pthread", str(LOOM_RT), "-o", str(bin_path)]
        link_res = subprocess.run(link_cmd, capture_output=True, text=True)
        if link_res.returncode != 0:
            raise RuntimeError(f"linking failed:\n{link_res.stderr}")
        run_res = subprocess.run([str(bin_path)], capture_output=True, text=True)
        return run_res

def test_signed_addition_overflow():
    source = """Module Main

Block Main Declares
    A : Signed_32
    B : Signed_32
    C : Signed_32
Implements
    A = 2147483647
    B = 1
    C = A + B
    Print(C)
End Main
"""
    res = compile_and_run_expect_panic(source)
    assert res.returncode == 101, f"Expected returncode 101, got {res.returncode}"
    assert "panic: integer overflow in Signed_32 addition at" in res.stderr, res.stderr
    print("[PASS] Signed_32 addition overflow trapped cleanly with exit code 101")

def test_signed_subtraction_overflow():
    source = """Module Main

Block Main Declares
    A : Signed_32
    B : Signed_32
    C : Signed_32
Implements
    A = -2147483647 - 1
    B = 1
    C = A - B
    Print(C)
End Main
"""
    res = compile_and_run_expect_panic(source)
    assert res.returncode == 101, f"Expected returncode 101, got {res.returncode}"
    assert "panic: integer overflow in Signed_32 subtraction at" in res.stderr, res.stderr
    print("[PASS] Signed_32 subtraction underflow trapped cleanly with exit code 101")

def test_unsigned_subtraction_underflow():
    source = """Module Main

Block Main Declares
    A : Unsigned_32
    B : Unsigned_32
    C : Unsigned_32
Implements
    A = 0
    B = 1
    C = A - B
    Print(C)
End Main
"""
    res = compile_and_run_expect_panic(source)
    assert res.returncode == 101, f"Expected returncode 101, got {res.returncode}"
    assert "panic: integer underflow in Unsigned_32 subtraction at" in res.stderr, res.stderr
    print("[PASS] Unsigned_32 subtraction underflow trapped cleanly with exit code 101")

def test_multiplication_overflow():
    source = """Module Main

Block Main Declares
    A : Signed_32
    B : Signed_32
    C : Signed_32
Implements
    A = 1000000
    B = 3000
    C = A * B
    Print(C)
End Main
"""
    res = compile_and_run_expect_panic(source)
    assert res.returncode == 101, f"Expected returncode 101, got {res.returncode}"
    assert "panic: integer overflow in Signed_32 multiplication at" in res.stderr, res.stderr
    print("[PASS] Signed_32 multiplication overflow trapped cleanly with exit code 101")

def test_division_by_zero():
    source = """Module Main

Block Main Declares
    A : Signed_32
    B : Signed_32
    C : Signed_32
Implements
    A = 42
    B = 0
    C = A / B
    Print(C)
End Main
"""
    res = compile_and_run_expect_panic(source)
    assert res.returncode == 101, f"Expected returncode 101, got {res.returncode}"
    assert "panic: division by zero at" in res.stderr, res.stderr
    print("[PASS] Division by zero trapped cleanly with exit code 101")

def test_modulo_by_zero():
    source = """Module Main

Block Main Declares
    A : Signed_32
    B : Signed_32
    C : Signed_32
Implements
    A = 42
    B = 0
    C = A % B
    Print(C)
End Main
"""
    res = compile_and_run_expect_panic(source)
    assert res.returncode == 101, f"Expected returncode 101, got {res.returncode}"
    assert "panic: division by zero at" in res.stderr, res.stderr
    print("[PASS] Modulo by zero trapped cleanly with exit code 101")

def test_array_bounds_check():
    source = """Module Main

Block Main Declares
    Arr : Signed_32 Array[5]
    Idx : Signed_64
    Val : Signed_32
Implements
    Idx = 10
    Val = Arr[Idx]
    Print(Val)
End Main
"""
    res = compile_and_run_expect_panic(source)
    assert res.returncode == 101, f"Expected returncode 101, got {res.returncode}"
    assert "panic: out of bounds access at" in res.stderr, res.stderr
    assert "(index 10, length 5)" in res.stderr, res.stderr
    print("[PASS] Array bounds overflow trapped cleanly with exit code 101 and index/length details")

def test_release_mode_checked_arithmetic():
    source = """Module Main

Block Main Declares
    A : Signed_32
    B : Signed_32
    C : Signed_32
Implements
    A = 2147483647
    B = 10
    C = A + B
    Print(C)
End Main
"""
    res = compile_and_run_expect_panic(source, release=True)
    assert res.returncode == 101, f"Expected returncode 101, got {res.returncode}"
    assert "panic: integer overflow in Signed_32 addition at" in res.stderr, res.stderr
    print("[PASS] Release mode (--release) preserves implicit checked arithmetic without silent corruption")

if __name__ == "__main__":
    test_signed_addition_overflow()
    test_signed_subtraction_overflow()
    test_unsigned_subtraction_underflow()
    test_multiplication_overflow()
    test_division_by_zero()
    test_modulo_by_zero()
    test_array_bounds_check()
    test_release_mode_checked_arithmetic()
    print("\n========================================================")
    print(" ALL PHASE 1 SAFETY & ARITHMETIC TESTS PASSED PERFECTLY!")
    print("========================================================")
