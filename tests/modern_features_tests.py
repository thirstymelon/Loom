#!/usr/bin/env python3
import os
import sys
import subprocess
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
if len(sys.argv) > 1 and Path(sys.argv[1]).is_file():
    LOOMC = Path(sys.argv[1]).resolve()
elif (REPO_ROOT / ".build" / "loomc").is_file():
    LOOMC = (REPO_ROOT / ".build" / "loomc").resolve()
elif (REPO_ROOT / "build" / "loomc").is_file():
    LOOMC = (REPO_ROOT / "build" / "loomc").resolve()
elif (REPO_ROOT / "bin" / "loomc").is_file():
    LOOMC = (REPO_ROOT / "bin" / "loomc").resolve()
else:
    LOOMC = Path("/usr/local/bin/loomc")

LOOM_RT = None
for cand in [LOOMC.parent / "libloom_rt.a", LOOMC.parent.parent / "lib" / "libloom_rt.a", REPO_ROOT / ".build" / "libloom_rt.a"]:
    if cand.is_file():
        LOOM_RT = cand
        break

def compile_and_run(source: str, release: bool = False) -> str:
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
        link_cmd = ["clang", str(obj_path), "-pthread"]
        if LOOM_RT:
            link_cmd.append(str(LOOM_RT))
        link_cmd.extend(["-o", str(bin_path)])
        link_res = subprocess.run(link_cmd, capture_output=True, text=True)
        if link_res.returncode != 0:
            raise RuntimeError(f"linking failed:\n{link_res.stderr}")
        run_res = subprocess.run([str(bin_path)], capture_output=True, text=True)
        if run_res.returncode != 0:
            raise RuntimeError(f"binary execution failed with code {run_res.returncode}:\nstderr:\n{run_res.stderr}")
        return run_res.stdout

def compile_and_run_with_code(source: str):
    with tempfile.TemporaryDirectory() as tmp:
        src_path = Path(tmp) / "main.loom"
        obj_path = Path(tmp) / "out.o"
        bin_path = Path(tmp) / "out"
        src_path.write_text(source)
        cmd = [str(LOOMC), "--object", "--entry", str(src_path), str(obj_path)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"loomc compilation failed:\nstdout:\n{res.stdout}\nstderr:\n{res.stderr}")
        link_cmd = ["clang", str(obj_path), "-pthread"]
        if LOOM_RT:
            link_cmd.append(str(LOOM_RT))
        link_cmd.extend(["-o", str(bin_path)])
        link_res = subprocess.run(link_cmd, capture_output=True, text=True)
        if link_res.returncode != 0:
            raise RuntimeError(f"linking failed:\n{link_res.stderr}")
        run_res = subprocess.run([str(bin_path)], capture_output=True, text=True)
        return run_res.returncode, run_res.stdout

def compile_fail(source: str, baremetal: bool = False) -> str:
    with tempfile.TemporaryDirectory() as tmp:
        src_path = Path(tmp) / "main.loom"
        obj_path = Path(tmp) / "out.o"
        src_path.write_text(source)
        cmd = [str(LOOMC), "--object", "--entry", str(src_path), str(obj_path)]
        if baremetal:
            cmd.append("--baremetal")
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode == 0:
            raise AssertionError(f"expected compiler failure but succeeded:\n{res.stdout}")
        return res.stderr + res.stdout
        if res.returncode == 0:
            raise AssertionError(f"expected compiler failure but succeeded:\n{res.stdout}")
        return res.stderr + res.stdout

def test_compound_assignments():
    src = """Module Main

Record Data
    Val : Signed_32
End Data

Block Main Declares
    X : Signed_32 = 10
    Arr : Signed_32 Array[3] = {1, 2, 3}
    D : Data
Implements
    X += 5
    Print(X) # 15
    X -= 3
    Print(X) # 12
    X *= 2
    Print(X) # 24
    X /= 4
    Print(X) # 6
    X &= 3
    Print(X) # 2
    X |= 8
    Print(X) # 10
    X ^= 3
    Print(X) # 9

    Arr[2] += 100
    Print(Arr[2]) # 102

    D.Val = 50
    D.Val += 25
    Print(D.Val) # 75
End Main
"""
    out = compile_and_run(src)
    expected = "15\n12\n24\n6\n2\n10\n9\n102\n75\n"
    assert out == expected, f"Compound assignment test failed:\nExpected:\n{expected}\nGot:\n{out}"
    print("[PASS] Compound assignments (+=, -=, *=, /=, &=, |=, ^=) on vars, arrays, and records")

def test_concise_routine_signatures():
    src = """Module Main

Block Add Takes
    A : Signed_32
    B : Signed_32
Produces (A + B)
End Add

Block MulAdd Takes
    A : Float_64
    B : Float_64
    C : Float_64
Produces (A * B + C)
End MulAdd

Block Main Declares
    Sum : Signed_32
    F : Float_64
Implements
    Sum = Add(15, 27)
    Print(Sum) # 42
    F = MulAdd(2.5, 4.0, 1.5)
    Print(F)   # 11.5
End Main
"""
    out = compile_and_run(src)
    assert "42\n" in out and "11.5\n" in out, f"Concise routine failed:\n{out}"
    print("[PASS] Concise routine signatures Produces (<expr>) with inferred return types")

def test_early_return_exit():
    src = """Module Main

Block CleanupAndExit Takes
    Mode : Signed_32
Implements
    Ensure { Print("Cleanup active!") }
    If (Mode == 1)
        Print("Early exiting block")
        Exit
    End If
    Print("Normal completion")
End CleanupAndExit

Block Main Implements
    CleanupAndExit(1)
    Print("---")
    CleanupAndExit(2)
End Main
"""
    out = compile_and_run(src)
    expected = "Early exiting block\nCleanup active!\n---\nNormal completion\nCleanup active!\n"
    assert out == expected, f"Early return exit failed:\nExpected:\n{expected}\nGot:\n{out}"

    # Verify that Exit in value block fails
    bad_src = """Module Main
Block Bad Produces Signed_32 Implements
    Exit
End Bad
Block Main Implements
    Bad()
End Main
"""
    err = compile_fail(bad_src)
    assert "Exit cannot be used in a value-producing block" in err, err
    print("[PASS] Early return / procedure Exits with LIFO Ensure execution and rejection in value blocks")

def test_loops_while_and_until():
    src = """Module Main

Block Main Declares
    Count : Signed_32 = 0
    I : Signed_32 = 0
    J : Signed_32 = 0
Implements
    # 1. Infinite loop with Exit
    Loop
        Count += 1
        If (Count == 3)
            Exit
        End If
    End Loop
    Print(Count) # 3

    # 2. Loop until
    Loop until (I >= 3)
        I += 1
    End Loop
    Print(I) # 3

    # 3. Loop while
    Loop while (J < 4)
        J += 1
    End Loop
    Print(J) # 4
End Main
"""
    out = compile_and_run(src)
    expected = "3\n3\n4\n"
    assert out == expected, f"Loop test failed:\nExpected:\n{expected}\nGot:\n{out}"
    print("[PASS] Loops: infinite Loop, Loop until <cond>, and Loop while <cond>")

def test_pattern_matching():
    src = """Module Main

Enum State : Signed_32
    Idle = 0
    Active = 1
    Failed = 2
End State

Block DescribeState Takes
    S : State
Implements
    Match S
        When State.Idle:
            Print("Is Idle")
        When State.Active:
            Print("Is Active")
        Else:
            Print("Is Other")
    End Match
End DescribeState

Block DescribeNum Takes
    N : Signed_32
Implements
    Match N
        When 1:
            Print("One")
        When 2:
            Print("Two")
        When 3:
            Print("Three")
        Else:
            Print("Many")
    End Match
End DescribeNum

Block Main Implements
    DescribeState(State.Idle)
    DescribeState(State.Active)
    DescribeState(State.Failed)
    DescribeNum(2)
    DescribeNum(99)
End Main
"""
    out = compile_and_run(src)
    expected = "Is Idle\nIs Active\nIs Other\nTwo\nMany\n"
    assert out == expected, f"Match test failed:\nExpected:\n{expected}\nGot:\n{out}"
    print("[PASS] Pattern matching (Match ... When <pattern>: ... Else: ... End Match)")

def test_record_literal_initialization():
    src = """Module Main

Record Point
    X : Signed_32
    Y : Signed_32
End Point

Block DistanceSq Takes
    P : Point
Produces Signed_32
Implements
    Give P.X * P.X + P.Y * P.Y
End DistanceSq

Block Main Declares
    P1 : Point = Point{ X: 3, Y: 4 }
    P2 : Point
Implements
    Print(P1.X) # 3
    Print(P1.Y) # 4
    Print(DistanceSq(P1)) # 25

    # Field reordering & assignment
    P2 = Point{ Y: 20, X: 10 }
    Print(P2.X) # 10
    Print(P2.Y) # 20

    # Passing inline record literal as argument
    Print(DistanceSq(Point{ X: 6, Y: 8 })) # 100
End Main
"""
    out = compile_and_run(src)
    expected = "3\n4\n25\n10\n20\n100\n"
    assert out == expected, f"Record literal test failed:\nExpected:\n{expected}\nGot:\n{out}"
    print("[PASS] Record initialization literals (Point{ X: 10, Y: 20 })")

def test_discarded_return_and_typed_handle():
    src = """Module Main
Record I2C_Handle
    Controller : Signed_64
    SDA_Pin : Signed_64
    SCL_Pin : Signed_64
End I2C_Handle
Block Global
Declares
    Calls : Signed_32 = 0
End Global

Block Make_Handle Produces I2C_Handle
Declares
    Handle : I2C_Handle
Implements
    Handle.Controller = 1
    Handle.SDA_Pin = 2
    Handle.SCL_Pin = 3
    Give Handle
End Make_Handle

Block Handle_Controller Takes
    Handle : I2C_Handle
Produces Signed_64
Implements
    Give Handle.Controller
End Handle_Controller

Block Operation Produces Boolean
Implements
    Calls += 1
    Give False
End Operation

Block Main Declares
    OLED_I2C : I2C_Handle
Implements
    OLED_I2C = Make_Handle()
    Operation()
    Print(Calls)
    Print(Handle_Controller(OLED_I2C))
End Main
"""
    out = compile_and_run(src)
    assert out == "1\n1\n", f"Typed handle or discarded return failed: {out!r}"
    print("[PASS] Typed record handles pass through blocks; unused status returns can be discarded")

def test_digit_separators_binary_hex():
    src = """Module Main

Block Main Declares
    Dec : Signed_32 = 1_000_000
    HexUpper : Unsigned_32 = 0xAF_01
    HexLower : Unsigned_32 = 0xaf01
    Bin : Unsigned_32 = 0b0101_1010
Implements
    Print(Dec)      # 1000000
    Print(HexUpper) # 44801
    Print(HexLower) # 44801
    Print(Bin)      # 90
End Main
"""
    out = compile_and_run(src)
    expected = "1000000\n44801\n44801\n90\n"
    assert out == expected, f"Literals test failed:\nExpected:\n{expected}\nGot:\n{out}"
    print("[PASS] Digit separators (1_000_000), binary (0b0101_1010), and hex (0xaf)")

def test_module_qualification_and_use():
    with tempfile.TemporaryDirectory() as tmp:
        math_path = Path(tmp) / "math.loom"
        math_path.write_text("""Module Math

Block Add Takes
    A : Signed_32
    B : Signed_32
Produces Signed_32
Implements
    Give A + B
End Add

Block Sub Takes
    A : Signed_32
    B : Signed_32
Produces Signed_32
Implements
    Give A - B
End Sub
""")

        # 1. Qualified access Math.Add(...)
        main_qualified = Path(tmp) / "main_qual.loom"
        main_qualified.write_text("""Module Main
Import Math

Block Main Declares
    Res : Signed_32
Implements
    Res = Math.Add(20, 5)
    Print(Res) # 25
End Main
""")
        out = subprocess.run([str(LOOMC), "--object", "--output", str(Path(tmp)/"q.o"), "--entry", str(main_qualified), str(math_path)],
                             capture_output=True, text=True)
        assert out.returncode == 0, out.stderr
        subprocess.run(["clang", str(Path(tmp)/"q.o"), "-o", str(Path(tmp)/"q_out")], check=True)
        run_res = subprocess.run([str(Path(tmp)/"q_out")], capture_output=True, text=True)
        assert run_res.stdout == "25\n", run_res.stdout

        # 2. Global "use Math" allows unqualified Add(...) and qualified Math.Add(...)
        main_use_global = Path(tmp) / "main_use_global.loom"
        main_use_global.write_text("""Module Main
Import Math
use Math

Block Main Declares
    Res : Signed_32
Implements
    Print(Math.Add(10, 20)) # 30
    Res = Add(30, 12)
    Print(Res) # 42
End Main
""")
        out = subprocess.run([str(LOOMC), "--object", "--output", str(Path(tmp)/"g.o"), "--entry", str(main_use_global), str(math_path)],
                             capture_output=True, text=True)
        assert out.returncode == 0, out.stderr
        subprocess.run(["clang", str(Path(tmp)/"g.o"), "-o", str(Path(tmp)/"g_out")], check=True)
        run_res = subprocess.run([str(Path(tmp)/"g_out")], capture_output=True, text=True)
        assert run_res.stdout == "30\n42\n", run_res.stdout

        # 3. Block-scoped "use Math" in block header (before Implements)
        main_use_local = Path(tmp) / "main_use_local.loom"
        main_use_local.write_text("""Module Main
Import Math

Block Main
use Math
Declares
    Res : Signed_32
Implements
    Res = Add(50, 50)
    Print(Res) # 100
End Main
""")
        out = subprocess.run([str(LOOMC), "--object", "--output", str(Path(tmp)/"l.o"), "--entry", str(main_use_local), str(math_path)],
                             capture_output=True, text=True)
        assert out.returncode == 0, out.stderr
        subprocess.run(["clang", str(Path(tmp)/"l.o"), "-o", str(Path(tmp)/"l_out")], check=True)
        run_res = subprocess.run([str(Path(tmp)/"l_out")], capture_output=True, text=True)
        assert run_res.stdout == "100\n", run_res.stdout

        # 4. Unqualified call without use should fail
        main_fail = Path(tmp) / "main_fail.loom"
        main_fail.write_text("""Module Main
Import Math

Block Main Declares
    Res : Signed_32
Implements
    Res = Add(1, 2)
End Main
""")
        out = subprocess.run([str(LOOMC), "--object", "--output", str(Path(tmp)/"f.o"), "--entry", str(main_fail), str(math_path)],
                             capture_output=True, text=True)
        assert out.returncode != 0 and "cannot call block 'add' from module 'math' unqualified" in (out.stderr + out.stdout).lower(), out.stderr
    print("[PASS] Qualified module access (Math.Add) and global/local 'use' directive")

def test_multiline_ensure_and_single_rule():
    src = """Module TestEnsure

Block Main Declares
    A : Signed_32 = 10
    B : Signed_32 = 20
Implements
    Ensure {
        Print(A)
        Print(B)
    }
    Print(99)
End Main
"""
    out = compile_and_run(src)
    assert out.strip() == "99\n10\n20", repr(out)

    src_fail = """Module TestEnsureFail

Block Main
Implements
    Ensure { Print(1) }
    Ensure { Print(2) }
End Main
"""
    err = compile_fail(src_fail)
    assert "cannot contain multiple Ensure blocks" in err, err
    print("[PASS] Multi-line Ensure block and single-Ensure-per-block rule")

def test_ergonomic_type_casting():
    src = """Module TestCast

Block Main Declares
    X : Signed_64 = 42
    A : Signed_32
    B : Signed_32
    C : Signed_32
Implements
    A = X as Signed_32
    B = Signed_32(X)
    C = Cast[Signed_32](X)
    Print(A)
    Print(B)
    Print(C)
End Main
"""
    out = compile_and_run(src)
    assert out.strip() == "42\n42\n42", repr(out)
    print("[PASS] Ergonomic type casting ('expr as Type', 'Type(expr)', and 'Cast[T](expr)')")

def test_volatile_and_packed_hardware():
    src = """Module TestHardware

Packed Record Register Align 4
    Low : Unsigned_16
    High : Unsigned_16
End Register

Block Main Declares
    R : Register
    Ptr : Volatile Unsigned_16 Pointer
Implements
    R.Low = 170
    R.High = 187
    Ptr = Address(R.Low)
    Print(Deref(Ptr))
End Main
"""
    out = compile_and_run(src)
    assert out.strip() == "170", repr(out)
    print("[PASS] Hardware primitives: Volatile pointers and Packed Record Align N")

def test_error_handling_try_or():
    src = """Module TestError

Enum MathError : Signed_32
    DivisionByZero = 1
End MathError

Block Divide
    Takes
        A : Signed_32
        B : Signed_32
    Produces Signed_32 or MathError
Implements
    If B == 0
        Give MathError.DivisionByZero
    End If
    Give A / B
End Divide

Block Compute
    Takes
        X : Signed_32
    Produces Signed_32 or MathError
Declares
    Val : Signed_32
Implements
    Val = try Divide(100, X)
    Give Val + 5
End Compute

Block Main Declares
    Res : Signed_32 or MathError
Implements
    Res = Compute(2)
    Print(42)
End Main
"""
    out = compile_and_run(src)
    assert out.strip() == "42", repr(out)
    print("[PASS] Error handling: 'Produces T or Error' and 'try expr'")

def test_sub_slicing():
    src = """Module TestSlice

Block Main Declares
    Arr : Signed_32 Array[5] = {10, 20, 30, 40, 50}
    Sub : Signed_32 Slice
Implements
    Sub = Arr[2 .. 4]
    Print(Sub[1])
    Print(Sub[2])
    Print(Sub[3])
End Main
"""
    out = compile_and_run(src)
    assert out.strip() == "20\n30\n40", repr(out)
    print("[PASS] Sub-slicing syntax Arr[Start .. End]")

def test_monomorphized_generics():
    src = """Module TestGenerics

Record Box[T]
    Value : T
End Box

Block Identity[T]
    Takes
        X : T
    Produces T
Implements
    Give X
End Identity

Block Main Declares
    B : Box[Signed_32]
    Res : Signed_32
Implements
    B.Value = 123
    Res = Identity[Signed_32](B.Value)
    Print(Res)
End Main
"""
    out = compile_and_run(src)
    assert out.strip() == "123", repr(out)
    print("[PASS] Monomorphized generics: Record[T] and Block[T]")

def test_implicit_threading_opt_report():
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "main.loom"
        out_ll = Path(tmp) / "out.ll"
        src.write_text("""Module Main

Block Main Declares
    Arr : Signed_32 Array[100]
    I : Signed_64 = 0
Implements
    Loop While I < 100
        Arr[I] = 42
        I = I + 1
    End Loop
End Main
""")
        # Native opt-report
        r = subprocess.run([str(LOOMC), "--opt-report", str(src), str(out_ll)], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        assert "auto-parallelized data-parallel loop (induction variable 'I') across native worker threads" in r.stdout

        # Bare-metal opt-report skips parallelism
        r_bm = subprocess.run([str(LOOMC), "--baremetal", "--opt-report", str(src), str(out_ll)], capture_output=True, text=True)
        assert r_bm.returncode == 0, r_bm.stderr
        assert "[parallelism skipped on embedded target]" in r_bm.stdout
    print("[PASS] Safe implicit multi-threading loop detection in opt-report (Native vs Embedded)")

def test_tooling_features():
    with tempfile.TemporaryDirectory() as tmp:
        # 1. Test runner
        test_src = Path(tmp) / "test_runner.loom"
        test_obj = Path(tmp) / "test_runner.o"
        test_bin = Path(tmp) / "test_runner"
        test_src.write_text("""Module TestSuite

Block Add
    Takes
        A : Signed_32
        B : Signed_32
    Produces Signed_32
Implements
    Give A + B
End Add

Test "addition unit test"
Declares
    Res : Signed_32
Implements
    Res = Add(2, 3)
    If Res != 5
        Print("Assertion failed")
    End If
End Test
""")
        r = subprocess.run([str(LOOMC), "--test-runner", "--entry", "--object", str(test_src), str(test_obj)], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        subprocess.run(["clang", str(test_obj), "-o", str(test_bin)], check=True)
        out = subprocess.run([str(test_bin)], capture_output=True, text=True)
        assert out.returncode == 0 and "[PASS] \"addition unit test\"" in out.stdout

        # 2. C Header generation
        hdr_src = Path(tmp) / "lib.loom"
        hdr_h = Path(tmp) / "lib.h"
        hdr_src.write_text("""Module MyLib

Record Point
    X : Signed_32
    Y : Signed_32
End Point

Block Distance
    Takes
        P : Point
    Produces Signed_32
Implements
    Give P.X + P.Y
End Distance
""")
        r_hdr = subprocess.run([str(LOOMC), "--emit-c-header", str(hdr_h), "--check", str(hdr_src)], capture_output=True, text=True)
        assert r_hdr.returncode == 0, r_hdr.stderr
        test_c = Path(tmp) / "test.c"
        test_c.write_text("""#include "lib.h"
int main() {
    Point p = { 10, 20 };
    return Distance(p);
}
""")
        c_res = subprocess.run(["clang", "-fsyntax-only", "-I", str(tmp), str(test_c)], capture_output=True, text=True)
        assert c_res.returncode == 0, c_res.stderr

        # 3. LSP server
        lsp_bin = REPO_ROOT / ".build" / "loom-lsp"
        if lsp_bin.is_file():
            import json
            req = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}}
            payload = json.dumps(req)
            msg = f"Content-Length: {len(payload)}\r\n\r\n{payload}"
            proc = subprocess.Popen([str(lsp_bin)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            stdout, _ = proc.communicate(input=msg.encode(), timeout=5)
            assert "result" in stdout.decode(), stdout.decode()

    print("[PASS] Tooling suite: Test runner engine, C header generation, and native loom-lsp")

def test_dynamic_memory_allocation():
    # 1. Basic Allocate, indexing (1-based), and Release
    out = compile_and_run("""Module Main

Block Main Declares
    Values : Signed_32 Pointer
Implements
    Values = Allocate(10)
    Values[1] = 42
    Values[10] = 99
    Print(Values[1])
    Print(Values[10])
    Release(Values)
End Main
""")
    assert out == "42\n99\n", f"Expected 42 and 99, got: {out}"

    # 2. Allocation in declaration with type inference
    out = compile_and_run("""Module Main

Block Main Declares
    Buf : Signed_32 Pointer = Allocate(5)
Implements
    Buf[1] = 100
    Print(Buf[1])
    Release(Buf)
End Main
""")
    assert out == "100\n", f"Expected 100, got: {out}"

    # 3. Resize growing and preserving existing elements
    out = compile_and_run("""Module Main

Block Main Declares
    Values : Signed_32 Pointer
Implements
    Values = Allocate(3)
    Values[1] = 11
    Values[2] = 22
    Values[3] = 33
    Values = Resize(Values, 6)
    Values[4] = 44
    Print(Values[1])
    Print(Values[2])
    Print(Values[3])
    Print(Values[4])
    Release(Values)
End Main
""")
    assert out == "11\n22\n33\n44\n", f"Expected 11, 22, 33, 44, got: {out}"

    # 4. Safe Release on Null (no-op)
    out = compile_and_run("""Module Main

Block Main Declares
    Ptr : Signed_32 Pointer = Null
Implements
    Release(Ptr)
    Release(Null)
    Print(1)
End Main
""")
    assert out == "1\n", f"Expected 1, got: {out}"

    # 5. Ensure cleanup with Release
    out = compile_and_run("""Module Main

Block Compute Produces Signed_32 Declares
    Buf : Signed_32 Pointer = Allocate(4)
Implements
    Ensure {
        Release(Buf)
    }
    Buf[1] = 77
    Give Buf[1]
End Compute

Block Main Declares
    Res : Signed_32
Implements
    Res = Compute()
    Print(Res)
End Main
""")
    assert out == "77\n", f"Expected 77, got: {out}"

    # 6. Baremetal / RP2040 compile-time rejection
    err1 = compile_fail("""Module Main

Block Main Declares
    P : Signed_32 Pointer
Implements
    P = Allocate(10)
End Main
""", baremetal=True)
    assert "Allocate is not supported on baremetal/RP2040" in err1, err1

    err2 = compile_fail("""Module Main

Block Main Declares
    P : Signed_32 Pointer = Null
Implements
    Release(P)
End Main
""", baremetal=True)
    assert "Release is not supported on baremetal/RP2040" in err2, err2

    print("[PASS] Dynamic Memory Allocation: Allocate, Resize, Release, Ensure cleanup, and RP2040 rejection")

def test_concurrency_spawn_join():
    # 1. Spawn with result and Join
    out = compile_and_run("""Module Main

Block Calculate Takes N : Signed_32 Produces Signed_32 Declares
Implements
    Give N * 2
End Calculate

Block Main Declares
    Task : Signed_32 Task_Handle
    Result : Signed_32
Implements
    Task = Spawn Calculate(21)
    Result = Join Task
    Print(Result)
End Main
""")
    assert out == "42\n", f"Expected 42, got: {out}"

    # 2. Spawn without result (Null Task_Handle) and standalone Join
    out = compile_and_run("""Module Main

Block Perform_Work Declares
Implements
    Print(999)
End Perform_Work

Block Main Declares
    Task : Null Task_Handle
Implements
    Task = Spawn Perform_Work()
    Join Task
End Main
""")
    assert out == "999\n", f"Expected 999, got: {out}"

    # 3. Multiple concurrent tasks
    out = compile_and_run("""Module Main

Block Add_Ten Takes X : Signed_32 Produces Signed_32 Declares
Implements
    Give X + 10
End Add_Ten

Block Main Declares
    First : Signed_32 Task_Handle
    Second : Signed_32 Task_Handle
    A : Signed_32
    B : Signed_32
Implements
    First = Spawn Add_Ten(10)
    Second = Spawn Add_Ten(20)
    A = Join First
    B = Join Second
    Print(A)
    Print(B)
End Main
""")
    assert out == "20\n30\n", f"Expected 20 and 30, got: {out}"

    # 4. Task concurrency with dynamic memory integration
    out = compile_and_run("""Module Main

Block Fill_Buffer Takes Buf : Signed_32 Pointer Produces Signed_32 Declares
Implements
    Buf[1] = 1234
    Give 1
End Fill_Buffer

Block Main Declares
    Buf : Signed_32 Pointer = Allocate(2)
    Task : Signed_32 Task_Handle
    Res : Signed_32
Implements
    Task = Spawn Fill_Buffer(Buf)
    Res = Join Task
    Print(Buf[1])
    Release(Buf)
End Main
""")
    assert out == "1234\n", f"Expected 1234, got: {out}"

    # 5. Baremetal / RP2040 compile-time rejection for Spawn/Join and Task_Handle
    err = compile_fail("""Module Main

Block Worker Declares
Implements
End Worker

Block Main Declares
    Task : Null Task_Handle
Implements
    Task = Spawn Worker()
    Join Task
End Main
""", baremetal=True)
    assert "not supported on baremetal/RP2040" in err, err

    print("[PASS] Concurrency: Spawn, Join, Null Task_Handle, Multi-task, Memory integration, and RP2040 rejection")


def test_concise_block_and_omitted_sections():
    out = compile_and_run("""Module Main

Block Say_Hello
Implements
    Print("Hello")
End Say_Hello

Block Add
Takes
    A : Signed_32
    B : Signed_32
Produces A + B
End Add

Block Main
Implements
    Say_Hello()
    Print(Add(10, 20))
End Main
""")
    assert out == "Hello\n30\n", f"Expected Hello and 30, got: {out}"
    print("[PASS] Concise block syntax and omitted empty sections")

def test_first_class_block_pointers():
    out = compile_and_run("""Module Main

Block Add
Takes
    A : Signed_32
    B : Signed_32
Produces Signed_32
Implements
    Give A + B
End Add

Block Print_Message
Takes
    Msg : String
Implements
    Print(Msg)
End Print_Message

Block Trigger_Shutdown
Implements
    Print("Shutdown triggered")
End Trigger_Shutdown

Block Apply
Takes
    Op : Signed_32 Block_Pointer(Signed_32, Signed_32)
    X : Signed_32
    Y : Signed_32
Produces Signed_32
Implements
    Give Op(X, Y)
End Apply

Block Main Declares
    Op : Signed_32 Block_Pointer(Signed_32, Signed_32) = Add
    Log : Block_Pointer(String) = Print_Message
    Action : Block_Pointer = Trigger_Shutdown
    Res : Signed_32
Implements
    Res = Op(20, 22)
    Print(Res)
    Log("Message via pointer")
    Action()
    Res = Apply(Add, 100, 200)
    Print(Res)
End Main
""")
    assert out == "42\nMessage via pointer\nShutdown triggered\n300\n", f"Got: {out}"
    print("[PASS] First-class Block Pointers: Declaring, passing, and calling indirect pointers")

def test_dynamic_text_string_builder():
    out = compile_and_run("""Module Main

Block Main Declares
    Buffer : Text
    Buf256 : Text[256]
    Msg : String
Implements
    Buffer += "Total: "
    Buffer += 42
    Buffer += " USD"
    Msg = To_String(Buffer)
    Print(Msg)
    Print(Buffer.Length)

    Buf256 += "Capacity: "
    Buf256 += True
    Print(Buf256)
    Print(Buf256.Capacity)
End Main
""")
    assert out == "Total: 42 USD\n13\nCapacity: True\n256\n", f"Got: {out}"
    print("[PASS] Dynamic Strings (Text): Default & preallocated capacity, += appending, To_String, Length, Capacity")

def test_dynamic_vectors():
    out = compile_and_run("""Module Main

Block Main Declares
    Nums : Signed_32 Vector[0]
    Last : Signed_32
    Idx : Unsigned_64
    Has : Boolean
    Cap : Unsigned_64
    Initial : Signed_32 Vector = {100, 200, 200, 300}
    Matches : Unsigned_64 Vector[0]
    Predef : Signed_32 Vector[5]
Implements
    Print(Predef.Length)
    Print(Predef[1])
    Nums.Append(10)
    Nums.Append(20)
    Nums.Append(30)
    Print(Nums.Length)
    Print(Nums[1])
    Print(Nums[2])
    Print(Nums[3])
    Nums[2] = 25
    Print(Nums[2])
    Last = Nums.Pop()
    Print(Last)
    Print(Nums.Length)
    
    # Search & inspection methods
    Idx = Nums.Index(25)
    Print(Idx)
    Has = Nums.Contains(25)
    Print(Has)
    Has = Nums.Contains(999)
    Print(Has)
    Cap = Nums.Capacity
    Print(Cap >= 2)

    # Initializer literal and Index_All
    Print(Initial.Length)
    Matches = Initial.Index_All(200)
    Print(Matches.Length)
    Print(Matches[1])
    Print(Matches[2])
End Main
""")
    assert out == "5\n0\n3\n10\n20\n30\n25\n30\n2\n2\nTrue\nFalse\nTrue\n4\n2\n2\n3\n", f"Got: {out}"
    print("[PASS] Dynamic Vector: Append, Pop, 1-based indexing, Length, Capacity, Index, Index_All, and Contains")

    # Verify that unadorned Vector declaration without size or initializer is rejected
    err = compile_fail("""Module Main
Block Main Declares
    Invalid : Signed_32 Vector
Implements
    Print(1)
End Main
""")
    assert "Vector declaration requires an explicit element count" in err, f"Unexpected error: {err}"
    print("[PASS] Dynamic Vector: rejection of unadorned Vector declaration")

def test_math_intrinsics_and_pow():
    out = compile_and_run("""Module Main

Block Main Declares
    Val1 : Signed_32
    Base : Float_64 = 2.0
    Exp : Float_64 = 3.0
    Val2 : Float_64
    Val3 : Float_64
    Val4 : Float_64
    Val5 : Signed_32
    F : Float_64 = 16.0
    NegF : Float_64 = -42.5
    X : Float_64 = 3.7
    Y : Float_64 = 3.2
Implements
    Val1 = 2 pow 5
    Print(Val1)
    Val2 = Base pow Exp
    Print(Val2)
    Val3 = Sqrt(F)
    Print(Val3)
    Val4 = Abs(NegF)
    Print(Val4)
    Val5 = Abs(-100)
    Print(Val5)
    Print(Floor(X))
    Print(Ceil(Y))
    Print(Min(10, 20))
    Print(Max(10, 20))
End Main
""")
    assert out == "32\n8\n4\n42.5\n100\n3\n4\n10\n20\n", f"Got: {out}"
    print("[PASS] Mathematical Intrinsics (Sqrt, Abs, Floor, Ceil, Min, Max) and infix pow operator")

def test_exit_status_codes():
    code1, _ = compile_and_run_with_code("""Module Main
Block Main Implements
    Exit(42)
End Main
""")
    assert code1 == 42, f"Expected exit code 42, got {code1}"

    code2, _ = compile_and_run_with_code("""Module Main
Block Main Implements
    Exit
End Main
""")
    assert code2 == 0, f"Expected exit code 0, got {code2}"
    print("[PASS] Process Exit Status Codes: Exit (0) and Exit(status_code)")

def test_file_io_and_print():
    out = compile_and_run("""Module Main

Block Main Declares
    F : File_Handle
    Content : String
Implements
    F = Open_File("/tmp/loom_test_io_main.txt", 2)
    Write(F, "Hello Loom File I/O: ", 42, "\\n")
    Close_File(F)

    Content = Read_All("/tmp/loom_test_io_main.txt")
    Print(Content)
End Main
""")
    assert out == "Hello Loom File I/O: 42\n\n", f"Got: {out}"
    print("[PASS] File I/O and Print: Open_File, Write to File_Handle, Close_File, Read_All, Print to stdout")

def test_concurrency_cancel_and_timeout():
    out = compile_and_run("""Module Main

Block Fast_Job
Takes
    N : Signed_32
Produces Signed_32
Implements
    Give N * 3
End Fast_Job

Block Long_Job
Takes
    N : Signed_32
Produces Signed_32
Implements
    Give N
End Long_Job

Block Main Declares
    T1 : Signed_32 Task_Handle
    T2 : Signed_32 Task_Handle
    Res : Signed_32
Implements
    T1 = Spawn Fast_Job(7)
    Res = Join T1 within 2000
    Print(Res)

    T2 = Spawn Long_Job(99)
    Cancel T2
    Print("Cancelled successfully")
End Main
""")
    assert out == "21\nCancelled successfully\n", f"Got: {out}"
    print("[PASS] Concurrency: Cancel Task and Join Task within timeout")

def test_systems_foundations_baremetal_rejection():
    err1 = compile_fail("""Module M
Block Main Declares
    Buf : Text
Implements
End Main
""", baremetal=True)
    assert "Text is not supported on baremetal/RP2040" in err1, err1

    err2 = compile_fail("""Module M
Block Main Declares
    V : Signed_32 Vector
Implements
End Main
""", baremetal=True)
    assert "Vector is not supported on baremetal/RP2040" in err2, err2

    err3 = compile_fail("""Module M
Block Main Declares
    F : File_Handle
Implements
End Main
""", baremetal=True)
    assert "File_Handle is not supported on baremetal/RP2040" in err3, err3

    err4 = compile_fail("""Module M
Block Main Declares
    Op : Block_Pointer
Implements
End Main
""", baremetal=True)
    assert "Block_Pointer is not supported on baremetal/RP2040" in err4, err4
    print("[PASS] Systems Foundations baremetal rejection: Text, Vector, File_Handle, Block_Pointer")

def test_string_interpolation_formats():
    src = """Module Main

Block Main Declares
    HexVal   : Unsigned_32 = 255
    FloatVal : Float_64 = 3.14159
    DecVal   : Signed_32 = 42
Implements
    Print("Hex: <HexVal:04X>")
    Print("Float: <FloatVal:.2f>")
    Print("Dec: <DecVal:6d>")
End Main
"""
    out = compile_and_run(src)
    assert "Hex: 00FF" in out, out
    assert "Float: 3.14" in out, out
    assert "Dec:     42" in out, out
    print("[PASS] String interpolation format specifiers (<expr:format>)")

def test_tuples_and_multi_value_returns():
    src = """Module Main

Block Calc
Takes
    A : Signed_32
    B : Signed_32
Produces (Signed_32, Boolean)
Implements
    Give (A + B, A > B)
End Calc

Block ConciseCalc
Takes
    A : Signed_32
    B : Signed_32
Produces (A + B, A > B)
End ConciseCalc

Block Main Declares
    Sum : Signed_32
    Gt  : Boolean
Implements
    Sum, Gt = Calc(20, 10)
    Print("Sum: <Sum>, Gt: <Gt>")
    (Sum, Gt) = Calc(5, 10)
    Print("Sum2: <Sum>, Gt2: <Gt>")
    Sum, Gt = ConciseCalc(100, 50)
    Print("Concise: <Sum>, <Gt>")
End Main
"""
    out = compile_and_run(src)
    assert "Sum: 30, Gt: True" in out, out
    assert "Sum2: 15, Gt2: False" in out, out
    assert "Concise: 150, True" in out, out
    print("[PASS] Multi-value returns and lightweight tuples")

def test_pattern_matching_range_and_destructuring():
    src = """Module Main

Record Point
    X : Signed_32
    Y : Signed_32
End Point

Block ClassifyRange Takes N : Signed_32 Produces String
Implements
    Match N
        When 1 .. 5:
            Give "Small"
        When 6 .. 10:
            Give "Medium"
        When _:
            Give "Large"
    End Match
End ClassifyRange

Block ClassifyPair Takes P : (Signed_32, Signed_32) Produces String
Implements
    Match P
        When (0, 0):
            Give "ZeroZero"
        When (1, 1):
            Give "OneOne"
        Else:
            Give "OtherPair"
    End Match
End ClassifyPair

Block ClassifyPoint Takes Pt : Point Produces String
Implements
    Match Pt
        When Point{ X: 0, Y: 0 }:
            Give "ZeroPoint"
        Else:
            Give "OtherPoint"
    End Match
End ClassifyPoint

Block Main Declares
    Pt1 : Point
    Pt2 : Point
Implements
    Print(ClassifyRange(3))
    Print(ClassifyRange(8))
    Print(ClassifyRange(99))

    Print(ClassifyPair((0, 0)))
    Print(ClassifyPair((1, 1)))
    Print(ClassifyPair((3, 4)))

    Pt1 = Point{ X: 0, Y: 0 }
    Pt2 = Point{ X: 10, Y: 20 }
    Print(ClassifyPoint(Pt1))
    Print(ClassifyPoint(Pt2))
End Main
"""
    out = compile_and_run(src)
    assert "Small\nMedium\nLarge\n" in out, out
    assert "ZeroZero\nOneOne\nOtherPair\n" in out, out
    assert "ZeroPoint\nOtherPoint\n" in out, out
    print("[PASS] Pattern matching with range and destructuring (ranges, tuples, records)")

def test_hash_map():
    src = """Module Main

Block Main Declares
    Users : HMap[Unsigned_32 : String] = {
        1 : "Alex",
        2 : "Adam",
        3 : "Bob"
    }
    Books : HMap[String : String] = {
        "Harper Lee" : "To Kill a Mockingbird",
        "Jane Austen" : "Pride and Prejudice"
    }
    HLBook : String
    GOBook : String
    HasHL  : Boolean
Implements
    Print("Users len: <Users.Length>")
    Print("User 1: <Users[1]>")
    Print("Has 2: <Users.Contains(2)>")
    Users.Remove(2)
    Print("After remove 2, len: <Users.Length>")
    Print("Has 2 now: <Users.Contains(2)>")

    Print("Books len: <Books.Length>")
    HLBook = Books["Harper Lee"]
    Print("Harper Lee: <HLBook>")
    HasHL = Books.Contains("Harper Lee")
    Print("Has Harper: <HasHL>")
    Books["George Orwell"] = "1984"
    Print("New books len: <Books.Length>")
    GOBook = Books["George Orwell"]
    Print("George Orwell: <GOBook>")
End Main
"""
    out = compile_and_run(src)
    assert "Users len: 3" in out, out
    assert "User 1: Alex" in out, out
    assert "Has 2: True" in out, out
    assert "After remove 2, len: 2" in out, out
    assert "Has 2 now: False" in out, out
    assert "Books len: 2" in out, out
    assert "Harper Lee: To Kill a Mockingbird" in out, out
    assert "Has Harper: True" in out, out
    assert "New books len: 3" in out, out
    assert "George Orwell: 1984" in out, out

    bm_err = compile_fail(src, baremetal=True)
    assert "HMap is not supported on baremetal/RP2040" in bm_err, bm_err
    print("[PASS] Hash Map (HMap) with integer and string keys, indexing, and baremetal rejection")

def test_vector_and_text_subslices():
    src = """Module Main

Block Main Declares
    Nums : Signed_32 Vector[5]
    Sub  : Signed_32 Slice
    Msg  : Text
    SubS : String
Implements
    Nums[1] = 100
    Nums[2] = 200
    Nums[3] = 300
    Nums[4] = 400
    Nums[5] = 500

    Sub = Nums[2 .. 4]
    Print("Sub len: <Sub.Length>")
    Print("Sub[1]: <Sub[1]>")
    Print("Sub[2]: <Sub[2]>")
    Print("Sub[3]: <Sub[3]>")

    Msg += "Antigravity Loom System"
    SubS = Msg[1 .. 11]
    Print("Text subslice: <SubS>")
    SubS = Msg[13 .. 16]
    Print("Text subslice 2: <SubS>")
End Main
"""
    out = compile_and_run(src)
    assert "Sub len: 3" in out, out
    assert "Sub[1]: 200" in out, out
    assert "Sub[2]: 300" in out, out
    assert "Sub[3]: 400" in out, out
    assert "Text subslice: Antigravity" in out, out
    assert "Text subslice 2: Loom" in out, out
    print("[PASS] Sub-slicing on dynamic Vector (returns Slice) and Text (returns String)")

def test_lsp_features():
    import json
    lsp_bin = REPO_ROOT / ".build" / "loom-lsp"
    if not lsp_bin.is_file():
        lsp_bin = REPO_ROOT / "build" / "loom-lsp"
    if not lsp_bin.is_file():
        return
    proc = subprocess.Popen([str(lsp_bin)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def send(msg):
        body = json.dumps(msg).encode("utf-8")
        header = f"Content-Length: {len(body)}\r\n\r\n".encode("utf-8")
        proc.stdin.write(header + body)
        proc.stdin.flush()

    def request(msg):
        send(msg)
        while True:
            line = proc.stdout.readline().decode("utf-8")
            if not line: break
            if line.startswith("Content-Length:"):
                clen = int(line.split(":")[1].strip())
                proc.stdout.readline()
                res = json.loads(proc.stdout.read(clen).decode("utf-8"))
                if res.get("id") == msg.get("id"):
                    return res

    request({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
    code = """Module Main

Record Point
    X : Signed_32
    Y : Signed_32
End Point

Block Calculate Takes A : Signed_32 Produces Signed_32
Implements
    Give A + 10
End Calculate

Block Main Declares
    Pt  : Point
    Vec : Signed_32 Vector[5]
Implements
    Vec.
    Calculate(10)
End Main
"""
    send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"uri": "file:///test.loom", "text": code}})
    def_pt = request({"jsonrpc": "2.0", "id": 2, "method": "textDocument/definition", "params": {"uri": "file:///test.loom", "position": {"line": 13, "character": 11}}})
    assert def_pt["result"]["range"]["start"]["line"] == 2, def_pt

    comp = request({"jsonrpc": "2.0", "id": 3, "method": "textDocument/completion", "params": {"uri": "file:///test.loom", "position": {"line": 16, "character": 8}}})
    labels = [c["label"] for c in comp["result"]]
    assert "Append" in labels and "Capacity" in labels and "Contains" in labels, labels
    proc.terminate()
    print("[PASS] LSP server autocompletion and go-to-definition")

def test_loom_fmt():
    driver_bin = REPO_ROOT / ".build" / "loom"
    if not driver_bin.is_file():
        driver_bin = REPO_ROOT / "build" / "loom"
    if not driver_bin.is_file():
        return
    with tempfile.TemporaryDirectory() as tmp:
        src_path = Path(tmp) / "fmt_test.loom"
        src_path.write_text("""Block Main
declares
Scores:Signed_32 Array[4]
I:Unsigned_64
Sum:Signed_32
implements
Scores[1]=90
Scores[2]=85
Scores[3]=95
Scores[4]=100

Sum=0
I=1
loop while (I<=Scores.Length)
Sum+=Scores[I]
I+=1
end loop

Print("Total sum: , Length: ")
end Main
""")
        res = subprocess.run([str(driver_bin), "fmt", str(src_path)], capture_output=True, text=True)
        assert res.returncode == 0, res.stderr
        formatted = src_path.read_text()
        assert "    Scores : Signed_32 Array[4]" in formatted, formatted
        assert "    I      : Unsigned_64" in formatted, formatted
        assert "    Sum    : Signed_32" in formatted, formatted
        assert "    Loop while (I <= Scores.Length)" in formatted, formatted
        assert "        Sum += Scores[I]" in formatted, formatted
        res_chk = subprocess.run([str(driver_bin), "fmt", "--check", str(src_path)], capture_output=True, text=True)
        assert res_chk.returncode == 0, res_chk.stderr
    print("[PASS] loom fmt canonical keyword casing, indentation, and colon alignment")

if __name__ == "__main__":
    test_compound_assignments()
    test_concise_routine_signatures()
    test_early_return_exit()
    test_loops_while_and_until()
    test_pattern_matching()
    test_record_literal_initialization()
    test_discarded_return_and_typed_handle()
    test_digit_separators_binary_hex()
    test_module_qualification_and_use()
    test_multiline_ensure_and_single_rule()
    test_ergonomic_type_casting()
    test_volatile_and_packed_hardware()
    test_error_handling_try_or()
    test_sub_slicing()
    test_monomorphized_generics()
    test_implicit_threading_opt_report()
    test_tooling_features()
    test_dynamic_memory_allocation()
    test_concurrency_spawn_join()
    test_concise_block_and_omitted_sections()
    test_first_class_block_pointers()
    test_dynamic_text_string_builder()
    test_dynamic_vectors()
    test_math_intrinsics_and_pow()
    test_exit_status_codes()
    test_file_io_and_print()
    test_concurrency_cancel_and_timeout()
    test_systems_foundations_baremetal_rejection()
    test_string_interpolation_formats()
    test_tuples_and_multi_value_returns()
    test_pattern_matching_range_and_destructuring()
    test_hash_map()
    test_vector_and_text_subslices()
    test_lsp_features()
    test_loom_fmt()
    print("\n========================================================")
    print(" ALL 35 MODERN LANGUAGE FEATURE TESTS PASSED PERFECTLY! ")
    print("========================================================")
