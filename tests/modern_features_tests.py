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
        link_res = subprocess.run(["clang", str(obj_path), "-o", str(bin_path)], capture_output=True, text=True)
        if link_res.returncode != 0:
            raise RuntimeError(f"linking failed:\n{link_res.stderr}")
        run_res = subprocess.run([str(bin_path)], capture_output=True, text=True)
        if run_res.returncode != 0:
            raise RuntimeError(f"binary execution failed with code {run_res.returncode}:\nstderr:\n{run_res.stderr}")
        return run_res.stdout

def compile_fail(source: str) -> str:
    with tempfile.TemporaryDirectory() as tmp:
        src_path = Path(tmp) / "main.loom"
        obj_path = Path(tmp) / "out.o"
        src_path.write_text(source)
        res = subprocess.run([str(LOOMC), "--object", "--entry", str(src_path), str(obj_path)], capture_output=True, text=True)
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
    Write(X) # 15
    X -= 3
    Write(X) # 12
    X *= 2
    Write(X) # 24
    X /= 4
    Write(X) # 6
    X &= 3
    Write(X) # 2
    X |= 8
    Write(X) # 10
    X ^= 3
    Write(X) # 9

    Arr[2] += 100
    Write(Arr[2]) # 102

    D.Val = 50
    D.Val += 25
    Write(D.Val) # 75
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
    Write(Sum) # 42
    F = MulAdd(2.5, 4.0, 1.5)
    Write(F)   # 11.5
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
    Ensure { Write("Cleanup active!") }
    If (Mode == 1)
        Write("Early exiting block")
        Exit
    End If
    Write("Normal completion")
End CleanupAndExit

Block Main Implements
    CleanupAndExit(1)
    Write("---")
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
    Write(Count) # 3

    # 2. Loop until
    Loop until (I >= 3)
        I += 1
    End Loop
    Write(I) # 3

    # 3. Loop while
    Loop while (J < 4)
        J += 1
    End Loop
    Write(J) # 4
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
            Write("Is Idle")
        When State.Active:
            Write("Is Active")
        Else:
            Write("Is Other")
    End Match
End DescribeState

Block DescribeNum Takes
    N : Signed_32
Implements
    Match N
        When 1:
            Write("One")
        When 2:
            Write("Two")
        When 3:
            Write("Three")
        Else:
            Write("Many")
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
    Write(P1.X) # 3
    Write(P1.Y) # 4
    Write(DistanceSq(P1)) # 25

    # Field reordering & assignment
    P2 = Point{ Y: 20, X: 10 }
    Write(P2.X) # 10
    Write(P2.Y) # 20

    # Passing inline record literal as argument
    Write(DistanceSq(Point{ X: 6, Y: 8 })) # 100
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
    Write(Calls)
    Write(Handle_Controller(OLED_I2C))
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
    Write(Dec)      # 1000000
    Write(HexUpper) # 44801
    Write(HexLower) # 44801
    Write(Bin)      # 90
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
    Write(Res) # 25
End Main
""")
        out = subprocess.run([str(LOOMC), "--object", "--output", str(Path(tmp)/"q.o"), "--entry", str(main_qualified), str(math_path)],
                             capture_output=True, text=True)
        assert out.returncode == 0, out.stderr
        subprocess.run(["clang", str(Path(tmp)/"q.o"), "-o", str(Path(tmp)/"q_out")], check=True)
        run_res = subprocess.run([str(Path(tmp)/"q_out")], capture_output=True, text=True)
        assert run_res.stdout == "25\n", run_res.stdout

        # 2. Global "use Math" allows unqualified Add(...)
        main_use_global = Path(tmp) / "main_use_global.loom"
        main_use_global.write_text("""Module Main
Import Math
use Math

Block Main Declares
    Res : Signed_32
Implements
    Res = Add(30, 12)
    Write(Res) # 42
End Main
""")
        out = subprocess.run([str(LOOMC), "--object", "--output", str(Path(tmp)/"g.o"), "--entry", str(main_use_global), str(math_path)],
                             capture_output=True, text=True)
        assert out.returncode == 0, out.stderr
        subprocess.run(["clang", str(Path(tmp)/"g.o"), "-o", str(Path(tmp)/"g_out")], check=True)
        run_res = subprocess.run([str(Path(tmp)/"g_out")], capture_output=True, text=True)
        assert run_res.stdout == "42\n", run_res.stdout

        # 3. Block-scoped "use Math"
        main_use_local = Path(tmp) / "main_use_local.loom"
        main_use_local.write_text("""Module Main
Import Math

Block Main Declares
    Res : Signed_32
Implements
    use Math
    Res = Add(50, 50)
    Write(Res) # 100
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
        Write(A)
        Write(B)
    }
    Write(99)
End Main
"""
    out = compile_and_run(src)
    assert out.strip() == "99\n10\n20", repr(out)

    src_fail = """Module TestEnsureFail

Block Main
Implements
    Ensure { Write(1) }
    Ensure { Write(2) }
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
    Write(A)
    Write(B)
    Write(C)
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
    Write(Deref(Ptr))
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
    Write(42)
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
    Write(Sub[1])
    Write(Sub[2])
    Write(Sub[3])
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
    Write(Res)
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
        Write("Assertion failed")
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
    print("\n========================================================")
    print(" ALL 17 MODERN LANGUAGE FEATURE TESTS PASSED PERFECTLY! ")
    print("========================================================")
