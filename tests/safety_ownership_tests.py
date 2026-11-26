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

def test_bare_assignment_of_resource_rejected():
    source = """Module Main

Block Main Declares
    Buf1 : Signed_32 Pointer
    Buf2 : Signed_32 Pointer
Implements
    Buf1 = Allocate(5)
    Buf2 = Buf1
End Main
"""
    res = compile_source(source)
    assert res.returncode != 0, "Expected compile error for bare resource assignment"
    assert "cannot be implicitly assigned without explicit ownership operator" in res.stderr, res.stderr
    print("[PASS] Bare resource assignment rejected: requires explicit Move, Copy, or Borrow")

def test_move_semantics_and_invalidation():
    source = """Module Main

Block Main Declares
    Buf1 : Signed_32 Pointer
    Buf2 : Signed_32 Pointer
    Val : Signed_32
Implements
    Buf1 = Allocate(5)
    Buf1[1] = 77
    Buf2 = Move Buf1
    Val = Buf1[1]
End Main
"""
    res = compile_source(source)
    assert res.returncode != 0, "Expected compile error when reading moved resource"
    assert "use of moved value 'buf1'" in res.stderr.lower(), res.stderr
    print("[PASS] Move transfers ownership and invalidates previous owner at compile time")

def test_copy_semantics_independent_duplicate():
    source = """Module Main

Block Main Declares
    Buf1 : Signed_32 Pointer
    Buf2 : Signed_32 Pointer
Implements
    Buf1 = Allocate(5)
    Buf1[1] = 42
    Buf2 = Copy Buf1
    Buf2[1] = 99
    Print(Buf1[1])
    Print(Buf2[1])
End Main
"""
    out = compile_and_run(source)
    assert out.strip().splitlines() == ["42", "99"], f"Unexpected output: {out}"
    print("[PASS] Copy creates independent duplicate heap buffer")

def test_borrow_read_only_view():
    source = """Module Main

Block Main Declares
    Buf1 : Signed_32 Pointer
    View : Signed_32 Pointer
Implements
    Buf1 = Allocate(5)
    Buf1[1] = 88
    View = Borrow Buf1
    Print(View[1])
End Main
"""
    out = compile_and_run(source)
    assert out.strip() == "88", f"Unexpected output: {out}"
    print("[PASS] Borrow allows read access to resource without transferring ownership")

def test_borrow_mutation_rejected():
    source = """Module Main

Block Main Declares
    Buf1 : Signed_32 Pointer
    View : Signed_32 Pointer
Implements
    Buf1 = Allocate(5)
    View = Borrow Buf1
    View[1] = 100
End Main
"""
    res = compile_source(source)
    assert res.returncode != 0, "Expected compile error for mutating read-only borrow"
    assert "cannot mutate read-only borrow 'view'" in res.stderr.lower(), res.stderr
    print("[PASS] Mutating read-only Borrow rejected at compile time")

def test_release_flow_across_alternative_branches():
    both_paths = """Module Main

Block Main Declares
    Buf : Signed_32 Pointer
    Choose : Boolean = True
Implements
    Buf = Allocate(1)
    If Choose
        Release(Buf)
    Else
        Release(Buf)
    End If
End Main
"""
    res = compile_source(both_paths)
    assert res.returncode == 0, f"Alternative releases should be checked per path, got: {res.stderr}"

    possible_release = """Module Main

Block Main Declares
    Buf : Signed_32 Pointer
    Choose : Boolean = True
Implements
    Buf = Allocate(1)
    If Choose
        Release(Buf)
    End If
    Release(Buf)
End Main
"""
    res = compile_source(possible_release)
    assert res.returncode != 0, "A release after a branch-conditional release must be rejected"
    assert "double free" in res.stderr.lower(), res.stderr
    print("[PASS] Ownership flow distinguishes alternative paths and possible double release")

def test_parameter_mode_ref_read_only():
    source = """Module Main

Record Document
    Title : String
End Document

Block Inspect_Doc Takes Ref Doc : Document
Implements
    Doc.Title = "Mutated"
End Inspect_Doc

Block Main Implements
End Main
"""
    res = compile_source(source)
    assert res.returncode != 0, "Expected compile error for mutating Ref parameter"
    assert "cannot mutate read-only 'ref' parameter 'doc'" in res.stderr.lower(), res.stderr
    print("[PASS] Mutating 'Takes Ref' parameter rejected at compile time")

def test_parameter_mode_mut_exclusive():
    source = """Module Main

Record Document
    Val : Signed_32
End Document

Block Update_Doc Takes Mut Doc : Document
Implements
    Doc.Val = 100
End Update_Doc

Block Main Declares
    D : Document
Implements
    D = Document{ Val: 42 }
    Update_Doc(D)
    Print(D.Val)
End Main
"""
    out = compile_and_run(source)
    assert out.strip() == "100", f"Unexpected output: {out}"
    print("[PASS] 'Takes Mut' parameter permits exclusive mutation")

if __name__ == "__main__":
    test_bare_assignment_of_resource_rejected()
    test_move_semantics_and_invalidation()
    test_copy_semantics_independent_duplicate()
    test_borrow_read_only_view()
    test_borrow_mutation_rejected()
    test_release_flow_across_alternative_branches()
    test_parameter_mode_ref_read_only()
    test_parameter_mode_mut_exclusive()
    print("\n============================================================")
    print(" ALL PHASE 3 & 4 OWNERSHIP & BORROW TESTS PASSED PERFECTLY!")
    print("============================================================")
