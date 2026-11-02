#!/usr/bin/env python3
import sys
import subprocess
import pathlib
import tempfile

def run():
    print("=" * 80)
    print("  Loom Automated Validation & Differential Correctness Suite")
    print("=" * 80)

    script_dir = pathlib.Path(__file__).resolve().parent
    repo_root = script_dir.parent
    build_dir = repo_root / ".build"
    build_dir.mkdir(exist_ok=True)
    val_dir = script_dir / "validation_tests"
    if len(sys.argv) > 1 and pathlib.Path(sys.argv[1]).is_file():
        compiler = pathlib.Path(sys.argv[1]).resolve()
    elif (repo_root / ".build" / "loomc").exists():
        compiler = repo_root / ".build" / "loomc"
    elif (repo_root / "build" / "loomc").exists():
        compiler = repo_root / "build" / "loomc"
    elif (repo_root / "bin" / "loomc").exists():
        compiler = repo_root / "bin" / "loomc"
    else:
        compiler = pathlib.Path("/usr/local/bin/loomc")
    tests = sorted(val_dir.glob("test*.loom"))
    assert len(tests) >= 14, f"Expected at least 14 validation tests, found {len(tests)} in {val_dir}"
    print(f"  Testing {len(tests)} workloads across Debug (-O0) vs Release (-O3)")

    passed = 0
    failed = 0

    for test in tests:
        name = test.stem
        expects_trap = name.startswith("test15_")
        try:
            with tempfile.TemporaryDirectory(prefix=f"loom-{name}-", dir=build_dir) as temp_dir:
                temp = pathlib.Path(temp_dir)
                obj_dbg, bin_dbg = temp / "debug.o", temp / "debug"
                obj_rel, bin_rel = temp / "release.o", temp / "release"
                subprocess.run([str(compiler), "--object", str(test), str(obj_dbg), "--entry"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                subprocess.run(["clang", str(obj_dbg), "-o", str(bin_dbg)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                out_dbg = (subprocess.run([str(bin_dbg)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
                           if expects_trap else subprocess.check_output([str(bin_dbg)]).decode().strip())
                subprocess.run([str(compiler), "--object", str(test), str(obj_rel), "--entry", "--release"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                subprocess.run(["clang", "-O3", str(obj_rel), "-o", str(bin_rel)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                if expects_trap:
                    out_rel = subprocess.run([str(bin_rel)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
                    if out_dbg == 0 or out_rel == 0:
                        print(f"  [FAIL] {name:<40} Expected a runtime trap in Debug and Release")
                        failed += 1
                        continue
                    print(f"  [PASS] {name:<40} Runtime trap in Debug and Release")
                    passed += 1
                    continue
                out_rel = subprocess.check_output([str(bin_rel)]).decode().strip()

            if out_dbg != out_rel:
                print(f"  [FAIL] {name:<40} Output mismatch: Debug='{out_dbg}' vs Release='{out_rel}'")
                failed += 1
            elif name == "test14_fma_and_float_precision" and out_rel.splitlines()[-1] != "0":
                print(f"  [FAIL] {name:<40} Expected separately rounded multiply/add result 0, got '{out_rel}'")
                failed += 1
            elif name == "test13_empty_and_short_slices" and out_rel.splitlines() != ["0", "1", "3", "4", "5", "7", "8", "9", "33", "35"]:
                print(f"  [FAIL] {name:<40} Incorrect reduction result across vector tails: {out_rel!r}")
                failed += 1
            elif name == "test18_ensure_lifo_and_early_exit" and out_rel.splitlines() != ["3", "4", "8", "2", "1"]:
                print(f"  [FAIL] {name:<40} Ensure order or early-exit behavior is wrong: {out_rel!r}")
                failed += 1
            elif name == "test19_ensure_loop_and_nested_scope" and out_rel.splitlines() != ["1", "2", "7"]:
                print(f"  [FAIL] {name:<40} Ensure loop/scope exit behavior is wrong: {out_rel!r}")
                failed += 1
            else:
                print(f"  [PASS] {name:<40} Output: {out_rel:<15} (Debug == Release)")
                passed += 1
        except Exception as e:
            print(f"  [FAIL] {name:<40} Compilation/Execution error: {e}")
            failed += 1

    print("-" * 80)
    print(f"Validation Summary: {passed} passed, {failed} failed out of {len(tests)} tests.")
    print("=" * 80)
    if failed > 0:
        sys.exit(1)

if __name__ == "__main__":
    run()
