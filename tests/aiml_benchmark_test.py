#!/usr/bin/env python3
import os
import sys
import time
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

def compile_and_run(source: str, release: bool = True) -> str:
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
        link_cmd = ["clang", str(obj_path), "-framework", "Accelerate", "-o", str(bin_path)]
        link_res = subprocess.run(link_cmd, capture_output=True, text=True)
        if link_res.returncode != 0:
            raise RuntimeError(f"linking failed:\n{link_res.stderr}")
        run_res = subprocess.run([str(bin_path)], capture_output=True, text=True)
        if run_res.returncode != 0:
            raise RuntimeError(f"binary execution failed with code {run_res.returncode}:\nstderr:\n{run_res.stderr}")
        return run_res.stdout

def test_matmul_variants():
    source = """
Module TestMatMulVariants

Block Main
    Declares
        A : Array[Float_32, 4]
        B : Array[Float_32, 4]
        Bias : Array[Float_32, 2]
        C1 : Array[Float_32, 4]
        C2 : Array[Float_32, 4]
        C3 : Array[Float_32, 4]
        C4 : Array[Float_32, 4]
    Implements
        A[1] = 1.0; A[2] = 2.0; A[3] = 3.0; A[4] = 4.0
        B[1] = 5.0; B[2] = 6.0; B[3] = 7.0; B[4] = 8.0
        Bias[1] = 10.0; Bias[2] = 20.0

        # C1 = A * B -> [19, 22, 43, 50]
        MatMul(A, B, C1, 2, 2, 2)
        Print(C1[1])
        Print(C1[2])
        Print(C1[3])
        Print(C1[4])

        # C2 = A * B + Bias -> [29, 42, 53, 70]
        MatMul_Add(A, B, Bias, C2, 2, 2, 2)
        Print(C2[1])
        Print(C2[2])
        Print(C2[3])
        Print(C2[4])

        # C3 = A * B^T -> [17, 23, 39, 53]
        MatMul_TransposeB(A, B, C3, 2, 2, 2)
        Print(C3[1])
        Print(C3[2])
        Print(C3[3])
        Print(C3[4])

        # C4 = A^T * B -> [26, 30, 38, 44]
        MatMul_TransposeA(A, B, C4, 2, 2, 2)
        Print(C4[1])
        Print(C4[2])
        Print(C4[3])
        Print(C4[4])
End Main
"""
    out = compile_and_run(source).strip().splitlines()
    expected = [
        "19", "22", "43", "50",
        "29", "42", "53", "70",
        "17", "23", "39", "53",
        "26", "30", "38", "44"
    ]
    assert out == expected, f"Expected {expected}, got {out}"
    print("✓ MatMul, MatMul_Add, MatMul_TransposeB, MatMul_TransposeA verified")

def test_activations_and_layers():
    source = """
Module TestActivations

Block Main
    Declares
        Act : Array[Float_32, 4]
        Grad : Array[Float_32, 4]
        dAct : Array[Float_32, 4]
        SM : Array[Float_32, 2]
        G_X : Array[Float_32, 2]
        LN_X : Array[Float_32, 4]
        LN_Gamma : Array[Float_32, 2]
        LN_Beta : Array[Float_32, 2]
        LN_Out : Array[Float_32, 4]
    Implements
        # 1. ReLU
        Act[1] = -2.0; Act[2] = 0.0; Act[3] = 4.0; Act[4] = -1.5
        ReLU(Act, 4)
        Print(Act[1])
        Print(Act[2])
        Print(Act[3])
        Print(Act[4])

        # 2. ReLU_Backward
        Grad[1] = 10.0; Grad[2] = 20.0; Grad[3] = 30.0; Grad[4] = 40.0
        ReLU_Backward(Grad, Act, dAct, 4)
        Print(dAct[1])
        Print(dAct[2])
        Print(dAct[3])
        Print(dAct[4])

        # 3. Softmax
        SM[1] = 0.0; SM[2] = 0.0
        Softmax(SM, 2)
        Print(SM[1])
        Print(SM[2])

        # 4. GELU (0.0 -> 0.0)
        G_X[1] = 0.0; G_X[2] = 1.0
        GELU(G_X, 2)
        Print(G_X[1])
        Print(G_X[2])

        # 5. LayerNorm
        LN_X[1] = 2.0; LN_X[2] = 4.0
        LN_X[3] = 10.0; LN_X[4] = 20.0
        LN_Gamma[1] = 1.0; LN_Gamma[2] = 1.0
        LN_Beta[1] = 0.0; LN_Beta[2] = 0.0
        LayerNorm(LN_X, LN_Gamma, LN_Beta, LN_Out, 2, 2)
        Print(LN_Out[1])
        Print(LN_Out[2])
End Main
"""
    out = compile_and_run(source).strip().splitlines()
    assert out[0] == "0"
    assert out[1] == "0"
    assert out[2] == "4"
    assert out[3] == "0"
    assert out[4] == "0"
    assert out[5] == "0"
    assert out[6] == "30"
    assert out[7] == "0"
    assert out[8] == "0.5"
    assert out[9] == "0.5"
    assert out[10] == "0"
    assert "0.841" in out[11]
    assert "-0.999" in out[12]
    assert "0.999" in out[13]
    print("✓ ReLU, ReLU_Backward, Softmax, GELU, LayerNorm verified")

def test_vector_ops_and_parallel():
    source = """
Module TestVectorAndParallel

Block Global
    Declares
        PArr : Array[Float_32, 8]
End Global

Block ScaleWorker
    Takes
        Idx : Signed_64
    Implements
        PArr[Idx] = PArr[Idx] * 3.0
End ScaleWorker

Block Main
    Declares
        V1 : Array[Float_32, 4]
        V2 : Array[Float_32, 4]
        VRes : Array[Float_32, 4]
        Dot : Float_32
    Implements
        V1[1] = 1.0; V1[2] = 2.0; V1[3] = 3.0; V1[4] = 4.0
        V2[1] = 10.0; V2[2] = 20.0; V2[3] = 30.0; V2[4] = 40.0

        Dot = Vector_Dot(V1, V2, 4)
        Print(Dot)

        Vector_Add(V1, V2, VRes, 4)
        Print(VRes[1])
        Print(VRes[4])

        Vector_Sub(V2, V1, VRes, 4)
        Print(VRes[1])
        Print(VRes[4])

        Vector_Mul(V1, V2, VRes, 4)
        Print(VRes[1])
        Print(VRes[4])

        Vector_Scale(V1, 2.5, VRes, 4)
        Print(VRes[1])
        Print(VRes[4])

        PArr[1] = 1.0; PArr[2] = 2.0; PArr[3] = 3.0; PArr[4] = 4.0
        PArr[5] = 5.0; PArr[6] = 6.0; PArr[7] = 7.0; PArr[8] = 8.0
        Parallel_For(8, ScaleWorker)
        Print(PArr[1])
        Print(PArr[8])
End Main
"""
    out = compile_and_run(source).strip().splitlines()
    assert out[0] == "300"
    assert out[1] == "11"
    assert out[2] == "44"
    assert out[3] == "9"
    assert out[4] == "36"
    assert out[5] == "10"
    assert out[6] == "160"
    assert out[7] == "2.5"
    assert out[8] == "10"
    assert out[9] == "3"
    assert out[10] == "24"
    print("✓ Vector_Dot, Vector_Add, Vector_Sub, Vector_Mul, Vector_Scale, Parallel_For verified")

def test_high_performance_gemm():
    source = """
Module TestGEMMThroughput

Block Global
    Declares
        A : Array[Float_32, 262144]
        B : Array[Float_32, 262144]
        C : Array[Float_32, 262144]
End Global

Block Init
    Takes
        Idx : Signed_64
    Implements
        A[Idx] = 0.01 as Float_32
        B[Idx] = 0.02 as Float_32
End Init

Block Main
    Declares
        I : Signed_64
    Implements
        Parallel_For(262144, Init)
        I = 1
        Loop while I <= 20
            MatMul(A, B, C, 512, 512, 512)
            I = I + 1
        End Loop
        Print(C[1])
End Main
"""
    t0 = time.perf_counter()
    out = compile_and_run(source, release=True).strip().splitlines()
    elapsed = time.perf_counter() - t0
    # 20 * 2 * 512^3 = 5.368 GFLOPs
    gflops = (20.0 * 2.0 * (512**3) / 1e9) / max(0.001, elapsed)
    assert len(out) >= 1
    assert "0.102" in out[0]
    print(f"✓ Apple Silicon Hardware GEMM throughput: ~{gflops:.1f} GFLOPS (total {elapsed:.3f}s)")

if __name__ == "__main__":
    print("Testing Loom Native Apple Silicon AIML Primitives...")
    test_matmul_variants()
    test_activations_and_layers()
    test_vector_ops_and_parallel()
    test_high_performance_gemm()
    print("ALL AIML NATIVE TESTS PASSED SUCCESSFULLY!")
