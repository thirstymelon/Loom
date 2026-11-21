#!/usr/bin/env python3
import os
import sys
import subprocess
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LOOMC = (REPO_ROOT / "build" / "loomc").resolve()
LOOM_RT = (REPO_ROOT / "build" / "libloom_rt.a").resolve()

def compile_source(source: str):
    with tempfile.TemporaryDirectory() as tmp:
        src_path = Path(tmp) / "main.loom"
        obj_path = Path(tmp) / "out.o"
        src_path.write_text(source)
        cmd = [str(LOOMC), "--object", "--entry", str(src_path), str(obj_path)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        return res

def compile_and_run(source: str) -> str:
    with tempfile.TemporaryDirectory() as tmp:
        src_path = Path(tmp) / "main.loom"
        obj_path = Path(tmp) / "out.o"
        bin_path = Path(tmp) / "out"
        src_path.write_text(source)
        cmd = [str(LOOMC), "--object", "--entry", str(src_path), str(obj_path)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"loomc compilation failed:\nstdout:\n{res.stdout}\nstderr:\n{res.stderr}")
        link_cmd = ["clang", str(obj_path), "-pthread", str(LOOM_RT), "-o", str(bin_path)]
        link_res = subprocess.run(link_cmd, capture_output=True, text=True)
        if link_res.returncode != 0:
            raise RuntimeError(f"linking failed:\n{link_res.stderr}")
        run_res = subprocess.run([str(bin_path)], capture_output=True, text=True)
        return run_res.stdout

def test_direct_uninitialized_read_fails():
    source = """Module Main

Block Main Declares
    A : Signed_32
Implements
    Print(A)
End Main
"""
    res = compile_source(source)
    assert res.returncode != 0, "Expected compile error for uninitialized variable read"
    assert "may be used uninitialized" in res.stderr, res.stderr
    print("[PASS] Direct uninitialized read rejected at compile time")

def test_if_without_else_assignment_fails():
    source = """Module Main

Block Main Declares
    A : Signed_32
    Cond : Boolean = True
Implements
    If Cond
        A = 100
    End If
    Print(A)
End Main
"""
    res = compile_source(source)
    assert res.returncode != 0, "Expected compile error for variable assigned only in If branch"
    assert "may be used uninitialized" in res.stderr, res.stderr
    print("[PASS] Partial If-branch assignment rejected when read outside")

def test_if_else_both_assigned_succeeds():
    source = """Module Main

Block Main Declares
    A : Signed_32
    Cond : Boolean = True
Implements
    If Cond
        A = 100
    Else
        A = 200
    End If
    Print(A)
End Main
"""
    out = compile_and_run(source)
    assert out.strip() == "100", f"Expected 100, got: {out}"
    print("[PASS] If-Else assignment in both branches definitely assigns variable")

def test_match_exhaustive_default_assigned_succeeds():
    source = """Module Main

Block Main Declares
    A : Signed_32
    X : Signed_32 = 1
Implements
    Match X
    When 1:
        A = 10
    Default:
        A = 20
    End Match
    Print(A)
End Main
"""
    out = compile_and_run(source)
    assert out.strip() == "10", f"Expected 10, got: {out}"
    print("[PASS] Match with Default branch assignment definitely assigns variable")

def test_match_without_default_fails():
    source = """Module Main

Block Main Declares
    A : Signed_32
    X : Signed_32 = 1
Implements
    Match X
    When 1:
        A = 10
    End Match
    Print(A)
End Main
"""
    res = compile_source(source)
    assert res.returncode != 0, "Expected compile error for Match without Default"
    print("[PASS] Non-exhaustive Match assignment rejected when read outside")

def test_declaration_initializer_succeeds():
    source = """Module Main

Block Main Declares
    A : Signed_32 = 42
Implements
    Print(A)
End Main
"""
    out = compile_and_run(source)
    assert out.strip() == "42", f"Expected 42, got: {out}"
    print("[PASS] Declaration initializer satisfies definite assignment")

def test_read_before_assign_in_same_block_fails():
    source = """Module Main

Block Main Declares
    A : Signed_32
    B : Signed_32
Implements
    B = A + 1
    A = 5
    Print(B)
End Main
"""
    res = compile_source(source)
    assert res.returncode != 0, "Expected compile error for reading before assignment"
    assert "may be used uninitialized" in res.stderr, res.stderr
    print("[PASS] Reading before assignment in statement sequence rejected at compile time")

if __name__ == "__main__":
    test_direct_uninitialized_read_fails()
    test_if_without_else_assignment_fails()
    test_if_else_both_assigned_succeeds()
    test_match_exhaustive_default_assigned_succeeds()
    test_match_without_default_fails()
    test_declaration_initializer_succeeds()
    test_read_before_assign_in_same_block_fails()
    print("\n============================================================")
    print(" ALL PHASE 2 DEFINITE ASSIGNMENT TESTS PASSED PERFECTLY!")
    print("============================================================")
