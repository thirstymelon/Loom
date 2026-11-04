#!/usr/bin/env python3
import pathlib
import json
import subprocess
import sys
import tempfile

loom = pathlib.Path(sys.argv[1]).resolve()
loomc = pathlib.Path(sys.argv[2]).resolve()

def run(args, cwd, check=True):
    result = subprocess.run([str(loom), *args], cwd=cwd, text=True, capture_output=True)
    if check and result.returncode:
        raise AssertionError(f"command failed: {args}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result

with tempfile.TemporaryDirectory(prefix="loom-cli-suite-") as tmp:
    parent = pathlib.Path(tmp)
    run(["new", "hello"], parent)
    project = parent / "hello"
    assert (project / "config.toml").exists()
    assert (project / "README.md").exists()
    assert (project / "src/main.loom").read_text().find("Block Main") >= 0
    run(["graph"], project)
    one_file_graph = (project / "loom-deps.mmd").read_text()
    assert 'b0["Main"]' in one_file_graph and "|calls|" not in one_file_graph
    assert "project.o" not in one_file_graph and "Native Build" not in one_file_graph
    run(["graph", "--config", "config.toml"], project)
    assert (project / "loom-deps.mmd").read_text() == one_file_graph
    (project / "src/main.loom").write_text('Module Main\nImport Math\nImport IO\nuse Math\nuse IO\n\nBlock Main Declares\n    Answer : Signed_32\nImplements\n    Answer = Helper(41)\n    Answer = Identity(Answer)\n    Log()\n    Write("Hello from Loom!")\n    Write("")\nEnd Main\n')

    (project / "src/math.loom").write_text("Module Math\n\nBlock Helper Takes\n    Value : Signed_32\nProduces Signed_32\nImplements\n    Give Value + 1\nEnd Helper\n")
    (project / "src/io helper.loom").write_text("Module IO\nImport Math\nuse Math\n\nBlock Identity Takes\n    Value : Signed_32\nProduces Signed_32\nImplements\n    Give Helper(Value)\nEnd Identity\nBlock Log Implements\nEnd Log\n")
    config = (project / "config.toml").read_text().replace('clang = "clang"', f'clang = "{__import__("shutil").which("clang")}"')
    (project / "config.toml").write_text(config)
    (project / "config.toml").write_text(config.replace('target-dir = "target"', 'target-dir = "artifacts/build"').replace('executable = "hello"', 'executable = "app"'))
    result = run(["build", "--release"], project)
    executable = project / "artifacts/build/app"
    assert executable.is_file(), result.stdout
    assert "Dependency analysis" not in result.stdout
    assert not (project / "loom-deps.mmd").exists()
    assert (project / "artifacts/build/obj/hello.o").is_file()
    assert "Build succeeded" in result.stdout
    run(["graph"], project)
    graph = project / "loom-deps.mmd"
    graph_text = graph.read_text()
    assert graph_text.startswith("flowchart TD")
    assert '"Main"' in graph_text and '"Helper"' in graph_text, graph_text
    assert "|calls|" in graph_text and "hello.o" not in graph_text and "hello" not in graph_text
    assert " -->|calls| " in graph_text
    assert "sources" not in graph_text.lower() and "native build" not in graph_text.lower()
    run(["graph"], project)
    assert (project / "loom-deps.mmd").read_text() == graph_text
    data_result = run(["graph", "--view", "data", "--output", "loom-data.mmd"], project)
    data_text = (project / "loom-data.mmd").read_text()
    assert "|reads|" in data_text and "|writes|" in data_text
    json_result = run(["graph", "--view", "data", "--format", "json"], project)
    model = json.loads((project / "loom-graph.json").read_text())
    assert model["schemaVersion"] == 1 and model["view"] == "data"
    assert set(model["sourceFiles"]) == {"src/main.loom", "src/math.loom", "src/io helper.loom"} and model.get("diagnostics") == [], model
    assert any(node["kind"] == "local" for node in model["nodes"])
    assert any(edge["kind"] == "reads" for edge in model["edges"])
    (project / "loom-graph.json").unlink()
    (project / "loom-data.mmd").unlink()
    run_result = subprocess.run([str(executable)], cwd=project, text=True, capture_output=True)
    assert run_result.returncode == 0 and run_result.stdout == "Hello from Loom!\n\n", run_result
    run_result = run(["run", "--config", "config.toml"], project)
    assert "Hello from Loom!\n\n" in run_result.stdout, run_result.stdout
    assert len(list((project / "artifacts/build/obj").glob("*.o"))) == 1
    assert not (project / "artifacts/build/loom_entry.c").exists()

    # Verify that missing Import triggers compilation failure
    (project / "src/main.loom").write_text('Module Main\n\nBlock Main Implements\n    Helper(1)\nEnd Main\n')
    fail_res = run(["build"], project, check=False)
    assert fail_res.returncode != 0 and "without 'Import Math'" in (fail_res.stderr + fail_res.stdout), fail_res.stderr + fail_res.stdout

    (project / "src/main.loom").write_text('''Module Main

Record Pair
    Value : Signed_32
    Label : String
End Pair
Block Main Declares
    A : Signed_32 = 4
    B : Signed_32 = 5
    Sum : Signed_32 = Add(A, B)
    Ready : Boolean = True
    Ratio : Float_64 = 1.5
    Letter : Character = 'é'
    Text : String = "done"
    Pair_Value : Pair
Implements
    Pair_Value.Value = Sum
    Pair_Value.Label = "record"
    Write(Sum)
    Write(Ready)
    Write(Ratio)
    Write(Letter)
    Write(Text)
    Write(Pair_Value)
End Main
Block Add Takes
    Left : Signed_32
    Right : Signed_32
Produces Signed_32
Implements
    Give Left + Right
End Add
''')
    run(["build"], project)
    typed_run = subprocess.run([str(executable)], cwd=project, text=True, capture_output=True)
    expected_typed = "9\nTrue\n1.5\né\ndone\nPair{Value: 9, Label: record}\n"
    assert typed_run.returncode == 0 and typed_run.stdout == expected_typed, typed_run
    run(["build"], project)  # Replacing an existing output is supported.
    assert not (project / "loom-deps.mmd").exists()
    run(["graph"], project)
    graph_text = (project / "loom-deps.mmd").read_text()
    run_result = subprocess.run([str(executable)], cwd=project, text=True, capture_output=True)
    assert run_result.returncode == 0 and run_result.stdout == expected_typed, run_result

    run(["clean"], project)
    assert not (project / "artifacts/build").exists()
    assert (project / "src/main.loom").exists()

    checked = run(["graph", "--check"], project)
    assert "validated" in checked.stdout.lower()
    custom_graph = run(["graph", "--output", "docs/graph with spaces.mmd", "--format", "mermaid"], project)
    assert "graph with spaces.mmd" in custom_graph.stdout
    assert (project / "docs/graph with spaces.mmd").read_text() == graph_text
    invalid_output = run(["graph", "--output", "config.toml/nope.mmd"], project, check=False)
    assert invalid_output.returncode != 0 and "graph" in invalid_output.stderr.lower()
    missing_config = run(["graph", "--config", "missing.toml"], project, check=False)
    assert missing_config.returncode != 0 and "missing.toml" in missing_config.stderr
    version = run(["--version"], project)
    assert "1.1.0" in version.stdout
    help_text = run(["--help"], project)
    assert "loom graph" in help_text.stdout and "--verbose" in help_text.stdout
    unsupported_mode = run(["build", "--freestanding"], project, check=False)
    assert unsupported_mode.returncode != 0 and "unknown argument" in unsupported_mode.stderr

    (project / "src/main.loom").write_text("Module Main\n\nBlock Other\n    Implements\nEnd Other\n")
    failed = run(["build"], project, check=False)
    assert failed.returncode != 0
    assert "must declare the parameterless procedure Block Main" in failed.stderr
    (project / "src/main.loom").write_text("Module Main\n\nBlock Main\nImplements\nWrite(Unknown)\nEnd Main\n")
    failed = run(["build"], project, check=False)
    assert failed.returncode != 0 and "error[E" in failed.stderr
    assert failed.stderr.count("error[E") == 1 and "Loom compilation failed" not in failed.stderr

    original = parent / "outside-marker"
    original.write_text("keep")
    (project / "config.toml").write_text((project / "config.toml").read_text().replace('target-dir = "artifacts/build"', 'target-dir = ".."'))
    failed = run(["clean"], project, check=False)
    assert failed.returncode != 0
    assert original.read_text() == "keep"

    run(["new", "feature_tour"], parent)
    feature_project = parent / "feature_tour"
    feature_project.rename(parent / "feature tour")
    feature_project = parent / "feature tour"
    (feature_project / "src/main.loom").write_text(pathlib.Path(__file__).resolve().parent.joinpath("fixtures/basic.loom").read_text())
    feature_config = (feature_project / "config.toml").read_text().replace('clang = "clang"', f'clang = "{__import__("shutil").which("clang")}"')
    (feature_project / "config.toml").write_text(feature_config)
    native_build = run(["build"], feature_project)
    feature_executable = feature_project / "target/feature_tour"
    assert feature_executable.is_file() and "Build succeeded" in native_build.stdout
    native_run = subprocess.run([str(feature_executable)], cwd=feature_project, text=True, capture_output=True)
    expected = ("==========================================\n"
                "       Loom Language Feature Showcase     \n"
                "==========================================\n\n"
                "1. Scalars, Function Calls & Recursion:\n"
                "Loom: Native Block-Oriented Systems Language\n"
                "⚡\n"
                "42\n"
                "120\n\n"
                "2. Structured Records, Record Returns & Pointers:\n"
                "Telemetry{Reading_Id: 101, Mode: 2, Position: Vector3{X: 12.5, Y: 48.2, Z: 3500}, Speed: 240, Active: True, Tag: Telemetry: Nominal}\n"
                "2\n"
                "240\n\n"
                "3. Multi-branch Conditionals (Else If):\n"
                "Cruising Altitude\n"
                "Condition branch: Verified (>=, And, Not)\n\n"
                "4. Low-Level Systems Operations:\n"
                "6619140\n"
                "218\n"
                "18\n"
                "5\n\n"
                "5. Loops and Flow Control:\n"
                "3\n"
                "Loop finished successfully with Exit\n\n"
                "==========================================\n"
                "Selected Loom language features demonstrated!\n"
                "==========================================\n")
    assert native_run.returncode == 0 and native_run.stdout == expected, native_run
    assert not (feature_project / "loom-deps.mmd").exists()
    assert (feature_project / "target/obj/feature_tour.o").is_file()
    assert not (feature_project / "target/obj/project.o").exists()
    run(["clean"], feature_project)
    assert not (feature_project / "target").exists()
    assert (feature_project / "src/main.loom").exists()

    # Verify bare-metal RP2040 project scaffolding and build
    run(["new", "rp_blink", "--rp2040"], parent)
    rp_project = parent / "rp_blink"
    assert (rp_project / "config.toml").exists()
    assert not (rp_project / "rp2040").exists()
    assert not (rp_project / "src/bsp").exists()
    assert not (rp_project / "boot.s").exists()
    assert not (rp_project / "linker.ld").exists()
    assert (rp_project / "wokwi.toml").exists()
    assert (rp_project / "diagram.json").exists()
    assert (rp_project / "src/main.loom").exists()
    assert not (rp_project / "src/hardware.loom").exists()
    assert "Block Main" in (rp_project / "src/main.loom").read_text()
    # Verify that native toolchain initially rejects RP2040 project
    rp_default_fail = run(["build"], rp_project, check=False)
    assert rp_default_fail.returncode != 0
    assert "not supported by the Loom native desktop toolchain" in (rp_default_fail.stderr + rp_default_fail.stdout)

    # Verify toolchain CLI commands: loom toolchains, loom use <tc>
    tc_list = run(["toolchains"], parent)
    assert "Installed Loom toolchains:" in tc_list.stdout
    assert "native" in tc_list.stdout
    assert "rp2040" in tc_list.stdout

    # Switch default to rp2040 with 'loom use rp2040' -> RP2040 project builds successfully
    use_rp = run(["use", "rp2040"], parent)
    assert "Using toolchain 'rp2040'" in use_rp.stdout
    use_show = run(["use"], parent)
    assert "Active toolchain: rp2040" in use_show.stdout

    tc_list_rp = run(["toolchains"], parent)
    assert "rp2040" in tc_list_rp.stdout and "(default)" in tc_list_rp.stdout

    rp_build = run(["build"], rp_project)
    assert (rp_project / "target/rp_blink.elf").is_file()
    assert (rp_project / "target/rp_blink.bin").is_file()
    assert (rp_project / "target/rp_blink.uf2").is_file()
    assert (rp_project / "target/obj/rp_blink.o").is_file()
    assert not (rp_project / "target/obj/project.o").exists()
    assert not (rp_project / "loom-deps.mmd").exists()
    assert "UF2 image:" in rp_build.stdout

    # When default is rp2040, desktop project must be rejected
    nat_rp_fail = run(["build"], feature_project, check=False)
    assert nat_rp_fail.returncode != 0
    assert "desktop host target is not supported by the Loom 'rp2040' embedded toolchain" in (nat_rp_fail.stderr + nat_rp_fail.stdout)

    # Switch default back to native with 'loom use native' -> desktop project builds, RP2040 project rejected
    use_nat = run(["use", "native"], parent)
    assert "Using toolchain 'native'" in use_nat.stdout

    tc_list_nat = run(["toolchains"], parent)
    assert "native" in tc_list_nat.stdout and "(default)" in tc_list_nat.stdout

    nat_build = run(["build"], feature_project)
    assert "Build succeeded" in nat_build.stdout

    rp_fail_again = run(["build"], rp_project, check=False)
    assert rp_fail_again.returncode != 0
    assert "not supported by the Loom native desktop toolchain" in (rp_fail_again.stderr + rp_fail_again.stdout)

    # Backward compatibility: verify 'loom set default rp2040' and 'loom set default native'
    run(["set", "default", "rp2040"], parent)
    assert "rp2040" in run(["toolchains"], parent).stdout
    run(["set", "default", "native"], parent)
    assert "native" in run(["toolchains"], parent).stdout

    # Explicit toolchain selection succeeds when matching
    rp_build_explicit = run(["build", "--toolchain", "rp2040"], rp_project)
    assert "Toolchain:   rp2040" in rp_build_explicit.stdout
    assert "Build succeeded" in rp_build_explicit.stdout

print("loom workspace CLI tests passed")
