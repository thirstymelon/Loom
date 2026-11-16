#!/usr/bin/env python3
import os
import pathlib
import select
import signal
import subprocess
import sys
import tempfile
import time

loom = pathlib.Path(sys.argv[1]).resolve()
loomc = pathlib.Path(sys.argv[2]).resolve()
project = pathlib.Path(sys.argv[3]).resolve()


def read_until(stream, predicate, timeout):
    collected = bytearray()
    deadline = time.monotonic() + timeout
    fd = stream.fileno()
    while time.monotonic() < deadline:
        if predicate(collected):
            break
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        chunk = os.read(fd, 8192)
        if not chunk:
            break
        collected.extend(chunk)
    return bytes(collected)


with tempfile.TemporaryDirectory(prefix="loom-native-baremetal-", dir=project / "target") as home:
    home_path = pathlib.Path(home)
    tc = home_path / ".loom" / "toolchains" / "native"
    (tc / "bin").mkdir(parents=True)
    (tc / "bin" / "loomc").symlink_to(loomc)
    (tc / "toolchain.toml").write_text(
        'name = "native"\nkind = "native"\nversion = "1.3.0"\ndescription = "bare-metal test"\n'
    )
    env = os.environ.copy()
    env["HOME"] = str(home_path)
    vm = subprocess.Popen(
        [str(loom), "run", "--release"], cwd=project, env=env,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        start_new_session=True
    )
    output = b""
    try:
        startup = read_until(vm.stdout, lambda data: b"loom> " in data, 30)
        output += startup
        if b"loom> " not in startup:
            raise AssertionError(f"Loom did not boot the AArch64 shell\n{startup.decode(errors='replace')}")
        assert vm.stdin is not None
        vm.stdin.write(b"help\nuname\necho qemu-test\n")
        vm.stdin.flush()
        interaction = read_until(vm.stdout, lambda data: data.count(b"qemu-test") >= 2, 5)
        output += interaction
    finally:
        try:
            os.killpg(vm.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            remainder, _ = vm.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(vm.pid, signal.SIGKILL)
            remainder, _ = vm.communicate(timeout=5)
        output += remainder

    decoded = output.decode(errors="replace")
    for expected in ("Loom Native Bare-Metal Kernel", "Commands:", "AArch64 / ARMv8-A / QEMU virt", "qemu-test"):
        if expected not in decoded:
            raise AssertionError(f"QEMU shell output missing {expected!r}\n{decoded}")
    if not (project / "target" / "LoomKernel.elf").is_file():
        raise AssertionError("bare-metal build did not produce target/LoomKernel.elf")
    if not (project / "target" / "LoomKernel.bin").is_file():
        raise AssertionError("bare-metal build did not produce target/LoomKernel.bin")

print("AArch64 bare-metal ELF and flat binary built; shell test passed through QEMU")
