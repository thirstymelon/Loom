#!/usr/bin/env python3
"""Regression checks for Loom runtime and IEEE float fixes."""
import os
import pathlib
import subprocess
import sys
import tempfile


compiler = pathlib.Path(sys.argv[1]).resolve()
repo = pathlib.Path(sys.argv[2]).resolve()
runtime = repo / "src/runtime/loom_runtime.c"
clang = os.environ.get("CC", "clang")

runtime_sources = [
    repo / "src/runtime/rt_core.c",
    repo / "src/runtime/rt_arena.c",
    repo / "src/runtime/rt_smart_ptr.c",
    repo / "src/runtime/rt_collections.c",
    repo / "src/runtime/rt_threads.c",
    repo / "src/runtime/rt_hash.c",
]
monolith_source = "\n".join(p.read_text() for p in runtime_sources)


def run(command, *, check=True, env=None):
    result = subprocess.run([str(part) for part in command], text=True,
                            capture_output=True, env=env, timeout=30)
    if check and result.returncode != 0:
        raise AssertionError(f"command failed: {command}\n{result.stderr}")
    return result


with tempfile.TemporaryDirectory(prefix="loom-runtime-regressions-") as td:
    td = pathlib.Path(td)

    # IEEE NaN comparison and special-value behavior must match between modes.
    nan_loom = td / "nan.loom"
    nan_loom.write_text("""Module NanRegression
Extern Block GetNaN
Produces Float_64
End GetNaN
Block Main Implements
    Print(GetNaN() != GetNaN())
End Main
""")
    float_c = td / "float.c"
    float_c.write_text("#include <math.h>\ndouble GetNaN(void) { return NAN; }\n")
    for release in (False, True):
        obj, binary = td / ("nan-release.o" if release else "nan-debug.o"), td / ("nan-release" if release else "nan-debug")
        command = [compiler, "--object", "--entry", nan_loom, obj]
        if release:
            command.insert(2, "--release")
        run(command)
        run([clang, obj, float_c, "-o", binary])
        assert run([binary]).stdout == "True\n"

    inf_loom = td / "inf.loom"
    inf_loom.write_text("""Module InfinityRegression
Extern Block GetInf
Produces Float_64
End GetInf
Block Main Declares
    Value : Float_64
Implements
    Value = GetInf() * 0.0
    Print(Value)
End Main
""")
    inf_c = td / "inf.c"
    inf_c.write_text("#include <math.h>\ndouble GetInf(void) { return INFINITY; }\n")
    outputs = []
    for release in (False, True):
        obj, binary = td / ("inf-release.o" if release else "inf-debug.o"), td / ("inf-release" if release else "inf-debug")
        command = [compiler, "--object", "--entry", inf_loom, obj]
        if release:
            command.insert(2, "--release")
        run(command)
        run([clang, obj, inf_c, "-o", binary])
        outputs.append(run([binary]).stdout.strip())
    assert outputs == ["nan", "nan"], outputs

    integer_edges = td / "integer_edges.loom"
    integer_edges.write_text("""Module IntegerDivisionRegression
Block Divide Takes
    A : Signed_32
    B : Signed_32
Produces Signed_32
Implements
    Give A / B
End Divide
Block Remainder Takes
    A : Signed_32
    B : Signed_32
Produces Signed_32
Implements
    Give A Mod B
End Remainder
Block Main Declares
    Minimum : Signed_32
Implements
    Minimum = -2147483647 - 1
    Print(Divide(Minimum, -1))
    Print(Remainder(Minimum, -1))
End Main
""")
    int_obj, int_bin = td / "integer_edges.o", td / "integer_edges"
    run([compiler, "--object", "--entry", integer_edges, int_obj])
    run([clang, int_obj, runtime, "-pthread", "-o", int_bin])
    res = run([int_bin], check=False)
    assert res.returncode != 0 and "integer overflow in Signed_32 division" in res.stderr

    divide_zero = td / "divide_zero.loom"
    divide_zero.write_text("""Module DivideZeroRegression
Block Divide Takes
    A : Signed_32
    B : Signed_32
Produces Signed_32
Implements
    Give A / B
End Divide
Block Main Declares
    Denominator : Signed_32 = 0
    Result : Signed_32
Implements
    Result = Divide(1, Denominator)
    Print(Result)
End Main
""")
    zero_obj, zero_bin = td / "divide_zero.o", td / "divide_zero"
    run([compiler, "--object", "--entry", divide_zero, zero_obj])
    run([clang, zero_obj, runtime, "-pthread", "-o", zero_bin])
    assert run([zero_bin], check=False).returncode != 0

    power = td / "power.loom"
    power.write_text("""Module PowerRegression
Block Power Takes
    Base : Signed_64
    Exponent : Signed_64
Produces Signed_64
Implements
    Give Base pow Exponent
End Power
Block Main Implements
    Print(Power(2, 63))
End Main
""")
    power_obj, power_bin = td / "power.o", td / "power"
    run([compiler, "--object", "--entry", power, power_obj])
    run([clang, power_obj, runtime, "-pthread", "-o", power_bin])
    assert run([power_bin]).stdout == "-9223372036854775808\n"

    negative_power = td / "negative_power.loom"
    negative_power.write_text("""Module NegativePowerRegression
Block Power Takes
    Base : Signed_64
    Exponent : Signed_64
Produces Signed_64
Implements
    Give Base pow Exponent
End Power
Block Main Implements
    Print(Power(2, -1))
End Main
""")
    negative_obj, negative_bin = td / "negative_power.o", td / "negative_power"
    run([compiler, "--object", "--entry", negative_power, negative_obj])
    run([clang, negative_obj, runtime, "-pthread", "-o", negative_bin])
    failed = run([negative_bin], check=False)
    assert failed.returncode != 0 and "integer exponent must be nonnegative" in failed.stderr

    shift = td / "shift.loom"
    shift.write_text("""Module ShiftRegression
Block Shift Takes
    Value : Signed_32
    Count : Signed_32
Produces Signed_32
Implements
    Give Value LSL Count
End Shift
Block Main Implements
    Print(Shift(1, 31))
End Main
""")
    shift_obj, shift_bin = td / "shift.o", td / "shift"
    run([compiler, "--object", "--entry", shift, shift_obj])
    run([clang, shift_obj, runtime, "-pthread", "-o", shift_bin])
    assert run([shift_bin]).stdout == "-2147483648\n"
    for bad_count in ("32", "-1"):
        bad_shift = td / f"shift_{bad_count.replace('-', 'neg')}.loom"
        bad_shift.write_text(shift.read_text().replace("Shift(1, 31)", f"Shift(1, {bad_count})"))
        bad_obj, bad_bin = td / "bad_shift.o", td / "bad_shift"
        run([compiler, "--object", "--entry", bad_shift, bad_obj])
        run([clang, bad_obj, runtime, "-pthread", "-o", bad_bin])
        assert run([bad_bin], check=False).returncode != 0

    # Grow Text while the append source aliases the old allocation.
    alias = td / "text_alias.loom"
    alias.write_text("Module TextAliasRegression\nBlock Main\nDeclares\n    Buffer : Text\n    View : String\nImplements\n    Buffer += \"" + "A" * 63 + "\"\n    View = To_String(Buffer)\n    Buffer += View\n    Print(Buffer.Length)\nEnd Main\n")
    alias_obj, alias_bin = td / "text_alias.o", td / "text_alias"
    run([compiler, "--object", "--entry", alias, alias_obj])
    run([clang, "-fsanitize=address,undefined", "-fno-omit-frame-pointer", alias_obj,
         runtime, "-pthread", "-o", alias_bin])
    asan_env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    assert run([alias_bin], env=asan_env).stdout == "126\n"

    # Force realloc to fail and verify Text aborts before writing past capacity.
    failed_runtime = td / "runtime_fail_realloc.c"
    source = "#include <stddef.h>\n#include <stdlib.h>\n#include <stdint.h>\nstatic void* audit_fail_realloc(void* p, size_t n) { (void)p; (void)n; return NULL; }\n" + monolith_source
    source = source.replace("realloc(t->data, new_cap)", "audit_fail_realloc(t->data, new_cap)")
    assert source != monolith_source
    failed_runtime.write_text(source)
    failure_harness = td / "text_oom.c"
    failure_harness.write_text("""#include \"loom_runtime.h\"
int main(void) {
    LoomText text;
    loom_text_init(&text, 2);
    loom_text_append_char(&text, 'A');
    loom_text_append_char(&text, 'B');
    return 0;
}
""")
    failure_bin = td / "text_oom"
    run([clang, "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", repo / "src/runtime",
         failure_harness, failed_runtime, "-pthread", "-o", failure_bin])
    failed = run([failure_bin], check=False, env=asan_env)
    assert failed.returncode != 0
    assert "could not grow Text buffer" in failed.stderr
    assert "heap-buffer-overflow" not in failed.stderr

    # A full Vector must also stop when growth fails, before its next write.
    vector_runtime = td / "runtime_fail_vector.c"
    source = "#include <stddef.h>\n#include <stdlib.h>\n#include <stdint.h>\nstatic void* audit_fail_vector_grow(void* p, uint64_t n, uint32_t s, uint32_t a) { (void)p; (void)n; (void)s; (void)a; return NULL; }\n" + monolith_source
    source = source.replace(
        "loom_realloc(v->data, new_cap, v->elem_size, v->elem_align)",
        "audit_fail_vector_grow(v->data, new_cap, v->elem_size, v->elem_align)",
    )
    assert source != monolith_source
    vector_runtime.write_text(source)
    vector_harness = td / "vector_oom.c"
    vector_harness.write_text("""#include \"loom_runtime.h\"
int main(void) {
    LoomVector vector;
    int first = 1, next = 2;
    loom_vec_init(&vector, 1, sizeof(int), _Alignof(int));
    loom_vec_append(&vector, &first);
    loom_vec_append(&vector, &next);
    return 0;
}
""")
    vector_bin = td / "vector_oom"
    run([clang, "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", repo / "src/runtime",
         vector_harness, vector_runtime, "-pthread", "-o", failure_bin])
    failed = run([failure_bin], check=False, env=asan_env)
    assert failed.returncode != 0 and "could not grow Vector storage" in failed.stderr
    assert "heap-buffer-overflow" not in failed.stderr

    # HMap must stop cleanly if an individual value allocation fails.
    hmap_runtime = td / "runtime_fail_hmap_value.c"
    source = "#include <stddef.h>\n#include <stdlib.h>\n#include <stdint.h>\nstatic void* audit_fail_hmap_value(size_t n) { (void)n; return NULL; }\n" + monolith_source
    source = source.replace(
        "malloc(m->val_size)", "audit_fail_hmap_value(m->val_size)",
    )
    assert source != monolith_source
    hmap_runtime.write_text(source)
    hmap_harness = td / "hmap_oom.c"
    hmap_harness.write_text("""#include \"loom_runtime.h\"
int main(void) {
    LoomHMap* map = loom_hmap_create(0, sizeof(int), _Alignof(int));
    int value = 7;
    loom_hmap_insert_i64(map, 1, &value);
    return 0;
}
""")
    hmap_bin = td / "hmap_oom"
    run([clang, "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", repo / "src/runtime",
         hmap_harness, hmap_runtime, "-pthread", "-o", hmap_bin])
    failed = run([hmap_bin], check=False, env=asan_env)
    assert failed.returncode != 0 and "could not allocate HMap value" in failed.stderr

    # A resize allocation failure must preserve the old table until failure.
    hmap_resize_runtime = td / "runtime_fail_hmap_resize.c"
    source = "#include <stddef.h>\n#include <stdlib.h>\n#include <stdint.h>\nstatic void* audit_fail_hmap_resize(size_t n, size_t s) { (void)n; (void)s; return NULL; }\n" + monolith_source
    source = source.replace(
        "LoomHMapEntry* new_entries = (LoomHMapEntry*)calloc(new_cap, sizeof(LoomHMapEntry));",
        "LoomHMapEntry* new_entries = (LoomHMapEntry*)audit_fail_hmap_resize(new_cap, sizeof(LoomHMapEntry));",
    )
    assert source != monolith_source
    hmap_resize_runtime.write_text(source)
    hmap_resize_harness = td / "hmap_resize_oom.c"
    hmap_resize_harness.write_text("""#include \"loom_runtime.h\"
int main(void) {
    LoomHMap* map = loom_hmap_create(0, sizeof(int), _Alignof(int));
    int value = 7;
    for (int i = 0; i < 12; ++i) loom_hmap_insert_i64(map, i, &value);
    return 0;
}
""")
    hmap_resize_bin = td / "hmap_resize_oom"
    run([clang, "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", repo / "src/runtime",
         hmap_resize_harness, hmap_resize_runtime, "-pthread", "-o", hmap_resize_bin])
    failed = run([hmap_resize_bin], check=False, env=asan_env)
    assert failed.returncode != 0 and "could not grow HMap entries" in failed.stderr

    # Cancel many queued tasks; Join must wait for the worker to relinquish each handle.
    task_harness = td / "task_cancel.c"
    task_harness.write_text("""#include \"loom_runtime.h\"
#include <unistd.h>
enum { N = 1000 };
static void work(void* args, void* result) {
    (void)args; (void)result;
    usleep(10000);
}
int main(void) {
    LoomTask* tasks[N];
    for (int i = 0; i < N; ++i) tasks[i] = loom_task_spawn(work, NULL, 0, 0);
    for (int i = 0; i < N; ++i) loom_task_cancel(tasks[i]);
    for (int i = 0; i < N; ++i) loom_task_join(tasks[i], NULL);
    return 0;
}
""")
    task_bin = td / "task_cancel"
    run([clang, "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", repo / "src/runtime",
         task_harness, runtime, "-pthread", "-o", task_bin])
    run([task_bin], env=asan_env)

    # Arena allocation validates alignment/capacity and supports whole-arena reset.
    arena_harness = td / "arena.c"
    arena_harness.write_text("""#include \"loom_runtime.h\"
#include <assert.h>
#include <stdint.h>
int main(void) {
    LoomArena* arena = loom_arena_create(128);
    assert(arena);
    unsigned char* a = (unsigned char*)loom_arena_alloc(arena, 3, 1);
    unsigned char* b = (unsigned char*)loom_arena_alloc(arena, 16, 16);
    assert(a && b && ((uintptr_t)b & 15u) == 0);
    a[0] = 42;
    assert(loom_arena_alloc(arena, 1024, 8) == NULL);
    assert(loom_arena_alloc(arena, 1, 3) == NULL);
    loom_arena_reset(arena);
    assert(loom_arena_alloc(arena, 3, 1) == a);
    loom_arena_destroy(arena);
    loom_arena_destroy(NULL);
    return 0;
}
""")
    arena_bin = td / "arena"
    run([clang, "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", repo / "src/runtime",
         arena_harness, runtime, "-pthread", "-o", arena_bin])
    run([arena_bin], env=asan_env)

    # The public C ABI is usable from Loom without a compiler-specific builtin.
    arena_loom = td / "arena.loom"
    arena_loom.write_text("""Module ArenaAPIRegression
Extern Block loom_arena_create Takes Capacity : Unsigned_64 Produces No_Type Pointer End loom_arena_create
Extern Block loom_arena_alloc Takes Arena : No_Type Pointer, Size : Unsigned_64, Alignment : Unsigned_64 Produces No_Type Pointer End loom_arena_alloc
Extern Block loom_arena_reset Takes Arena : No_Type Pointer End loom_arena_reset
Extern Block loom_arena_destroy Takes Arena : No_Type Pointer End loom_arena_destroy
Block Main Declares
    Arena : No_Type Pointer
    Data : Signed_32 Pointer
Implements
    Arena = loom_arena_create(128)
    Data = loom_arena_alloc(Arena, 16, 4) as Signed_32 Pointer
    Data[1] = 123
    Print(Data[1])
    loom_arena_reset(Arena)
    Data = loom_arena_alloc(Arena, 16, 4) as Signed_32 Pointer
    Data[1] = 456
    Print(Data[1])
    loom_arena_destroy(Arena)
End Main
""")
    arena_loom_obj, arena_loom_bin = td / "arena-loom.o", td / "arena-loom"
    run([compiler, "--object", "--entry", arena_loom, arena_loom_obj])
    run([clang, arena_loom_obj, runtime, "-pthread", "-o", arena_loom_bin])
    assert run([arena_loom_bin]).stdout == "123\n456\n"

print("passed IEEE float, integer division, Text/Vector/HMap/Arena allocation, and task cancellation regressions")
