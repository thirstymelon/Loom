import subprocess
import tempfile
from pathlib import Path

LOOMC = Path("/Users/lokesh/Desktop/Loom/build/loomc")
LOOM_RT = Path("/Users/lokesh/Desktop/Loom/build/libloom_rt.a")

def compile_and_expect_error(source: str, expected_fragment: str):
    with tempfile.TemporaryDirectory() as tmpdir:
        src = Path(tmpdir) / "test.loom"
        src.write_text(source)
        res = subprocess.run([str(LOOMC), "--check", str(src)],
                             capture_output=True, text=True)
        assert res.returncode != 0, f"Compilation unexpectedly succeeded! Output:\n{res.stdout}\n{res.stderr}"
        combined = res.stdout + res.stderr
        assert expected_fragment.lower() in combined.lower(), \
            f"Expected error fragment '{expected_fragment}' not found in:\n{combined}"

def compile_and_run(source: str) -> str:
    with tempfile.TemporaryDirectory() as tmpdir:
        src = Path(tmpdir) / "test.loom"
        obj = Path(tmpdir) / "test.o"
        exe = Path(tmpdir) / "test_bin"
        src.write_text(source)
        res = subprocess.run([str(LOOMC), "--object", str(src), str(obj), "--entry"], capture_output=True, text=True)
        assert res.returncode == 0, f"Compilation failed: {res.stderr}\n{res.stdout}"
        link = subprocess.run(["clang", str(obj), str(LOOM_RT), "-lpthread", "-o", str(exe)], capture_output=True, text=True)
        assert link.returncode == 0, f"Linking failed: {link.stderr}\n{link.stdout}"
        run_res = subprocess.run([str(exe)], capture_output=True, text=True)
        return run_res.stdout + run_res.stderr

def test_spawn_bare_resource_rejected():
    source = """
Module Main
Block Worker Takes V : Signed_32 Vector[0]
Implements
End Worker

Block Main Declares
    V : Signed_32 Vector[0]
    T : Null Task_Handle
Implements
    Append(V, 10)
    T = Spawn Worker(V)
End Main
"""
    compile_and_expect_error(source, "cannot pass resource 'V' to Spawn task without explicit Move")
    print("[PASS] Passing bare resource to Spawn rejected at compile time")

def test_spawn_moved_resource_accepted_and_invalidated():
    source = """
Module Main
Block Worker Takes V : Signed_32 Vector[0]
Implements
End Worker

Block Main Declares
    V : Signed_32 Vector[0]
    T : Null Task_Handle
Implements
    Append(V, 10)
    T = Spawn Worker(Move V)
    Print(V.Length)
End Main
"""
    compile_and_expect_error(source, "use of moved value 'V'")
    print("[PASS] Spawn Worker(Move V) invalidates V in spawning thread")

def test_spawn_borrow_rejected():
    source = """
Module Main
Block Worker Takes V : Signed_32 Vector[0]
Implements
End Worker

Block Main Declares
    V : Signed_32 Vector[0]
    T : Null Task_Handle
Implements
    Append(V, 10)
    T = Spawn Worker(Borrow V)
End Main
"""
    compile_and_expect_error(source, "cannot pass borrowed reference to Spawn task")
    print("[PASS] Passing borrowed reference (Borrow V) to Spawn rejected at compile time")

def test_spawn_ref_parameter_rejected():
    source = """
Module Main
Block Worker Takes V : Signed_32 Vector[0]
Implements
End Worker

Block Dispatch Takes Ref V : Signed_32 Vector[0]
Declares
    T : Null Task_Handle
Implements
    T = Spawn Worker(V)
End Dispatch

Block Main
Implements
End Main
"""
    compile_and_expect_error(source, "cannot pass borrowed reference 'V' to Spawn task")
    print("[PASS] Passing 'Takes Ref' parameter across thread boundary to Spawn rejected at compile time")

def test_channel_send_bare_resource_rejected():
    source = """
Module Main
Block Main Declares
    Ch : Channel[Signed_32 Vector[0]]
    V : Signed_32 Vector[0]
Implements
    Ch = Channel_Create[Signed_32 Vector[0]](10)
    Append(V, 42)
    Channel_Send(Ch, V)
End Main
"""
    compile_and_expect_error(source, "cannot send resource 'V' across channel without explicit 'Move V'")
    print("[PASS] Sending bare resource across Channel rejected at compile time")

def test_channel_send_borrow_rejected():
    source = """
Module Main
Block Main Declares
    Ch : Channel[Signed_32 Vector[0]]
    V : Signed_32 Vector[0]
Implements
    Ch = Channel_Create[Signed_32 Vector[0]](10)
    Append(V, 42)
    Channel_Send(Ch, Borrow V)
End Main
"""
    compile_and_expect_error(source, "cannot send borrowed reference across thread boundary")
    print("[PASS] Sending borrowed reference across Channel rejected at compile time")

def test_channel_send_move_invalidates_owner():
    source = """
Module Main
Block Main Declares
    Ch : Channel[Signed_32 Vector[0]]
    V : Signed_32 Vector[0]
Implements
    Ch = Channel_Create[Signed_32 Vector[0]](10)
    Append(V, 42)
    Channel_Send(Ch, Move V)
    Print(V.Length)
End Main
"""
    compile_and_expect_error(source, "use of moved value 'V'")
    print("[PASS] Channel_Send(Ch, Move V) invalidates V in sending thread")

def test_thread_concurrency_end_to_end():
    source = """
Module Main
Block Sender Takes Chan : Channel[Signed_32]
Implements
    Channel_Send(Chan, 777)
End Sender

Block Main Declares
    Chan : Channel[Signed_32]
    H : Null Task_Handle
    Val : Signed_32
Implements
    Chan = Channel_Create[Signed_32](4)
    H = Spawn Sender(Chan)
    Val = Channel_Receive(Chan)
    Join(H)
    Print(Val)
End Main
"""
    out = compile_and_run(source)
    assert out.strip() == "777", f"Unexpected output: {out}"
    print("[PASS] Send-safe primitive communication across spawned threads succeeds end-to-end")

if __name__ == "__main__":
    test_spawn_bare_resource_rejected()
    test_spawn_moved_resource_accepted_and_invalidated()
    test_spawn_borrow_rejected()
    test_spawn_ref_parameter_rejected()
    test_channel_send_bare_resource_rejected()
    test_channel_send_borrow_rejected()
    test_channel_send_move_invalidates_owner()
    test_thread_concurrency_end_to_end()
    print("\n============================================================")
    print(" ALL PHASE 5 THREAD ISOLATION TESTS PASSED PERFECTLY!")
    print("============================================================")
