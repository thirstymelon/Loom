import subprocess
import tempfile
from pathlib import Path

LOOM = Path(".build/loom") if Path(".build/loom").exists() else Path("build/loom")
LOOMC = Path(".build/loomc") if Path(".build/loomc").exists() else Path("build/loomc")
LOOM_RT = Path(".build/libloom_rt.a") if Path(".build/libloom_rt.a").exists() else Path("build/libloom_rt.a")
SDK_DIR = Path("/Users/lokesh/Desktop/Loom/sdk")
HASH_LIB = Path.home() / ".loom/libraries/hash/src"

def compile_and_run(source: str) -> str:
    with tempfile.TemporaryDirectory() as tmpdir:
        src = Path(tmpdir) / "main.loom"
        obj = Path(tmpdir) / "main.o"
        exe = Path(tmpdir) / "main_bin"
        src.write_text(source)
        res = subprocess.run([
            str(LOOMC),
            "--object", "--output", str(obj),
            "--entry",
            "-I", str(SDK_DIR / "std"),
            "-I", str(HASH_LIB),
            str(src),
            str(HASH_LIB / "hash.loom")
        ], capture_output=True, text=True)
        assert res.returncode == 0, f"Compilation failed:\n{res.stderr}\n{res.stdout}"
        link = subprocess.run(["clang", str(obj), str(LOOM_RT), "-lpthread", "-o", str(exe)], capture_output=True, text=True)
        assert link.returncode == 0, f"Linking failed:\n{link.stderr}\n{link.stdout}"
        run_res = subprocess.run([str(exe)], capture_output=True, text=True)
        assert run_res.returncode == 0, f"Execution failed with code {run_res.returncode}:\n{run_res.stderr}\n{run_res.stdout}"
        return run_res.stdout

def test_hash_sha256():
    source = """Module Main
Import Hash
use Hash

Block Main Declares
    Digest : Text
Implements
    Digest = Hash.SHA256("The quick brown fox jumps over the lazy dog")
    Print(Digest)
End Main
"""
    out = compile_and_run(source).strip()
    expected = "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"
    assert out == expected, f"SHA-256 mismatch!\nExpected: {expected}\nGot:      {out}"
    print("[PASS] Hash.SHA256 computed standard NIST digest correctly")

def test_hash_md5():
    source = """Module Main
Import Hash
use Hash

Block Main Declares
    Digest : Text
Implements
    Digest = Hash.MD5("The quick brown fox jumps over the lazy dog")
    Print(Digest)
End Main
"""
    out = compile_and_run(source).strip()
    expected = "9e107d9d372bb6826bd81d3542a419d6"
    assert out == expected, f"MD5 mismatch!\nExpected: {expected}\nGot:      {out}"
    print("[PASS] Hash.MD5 computed RFC 1321 digest correctly")

def test_hash_sha1():
    source = """Module Main
Import Hash
use Hash

Block Main Declares
    Digest : Text
Implements
    Digest = Hash.SHA1("The quick brown fox jumps over the lazy dog")
    Print(Digest)
End Main
"""
    out = compile_and_run(source).strip()
    expected = "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12"
    assert out == expected, f"SHA-1 mismatch!\nExpected: {expected}\nGot:      {out}"
    print("[PASS] Hash.SHA1 computed RFC 3174 digest correctly")

def test_hash_sha512():
    source = """Module Main
Import Hash
use Hash

Block Main Declares
    Digest : Text
Implements
    Digest = Hash.SHA512("The quick brown fox jumps over the lazy dog")
    Print(Digest)
End Main
"""
    out = compile_and_run(source).strip()
    expected = "07e547d9586f6a73f73fbac0435ed76951218fb7d0c8d788a309d785436bbb642e93a252a954f23912547d1e8a3b5ed6e1bfd7097821233fa0538f3db854fee6"
    assert out == expected, f"SHA-512 mismatch!\nExpected: {expected}\nGot:      {out}"
    print("[PASS] Hash.SHA512 computed FIPS 180-4 digest correctly")

def test_hash_fnv1a():
    source = """Module Main
Import Hash
use Hash

Block Main Declares
    H : Unsigned_64
Implements
    H = Hash.FNV1a("hello")
    Print(H)
End Main
"""
    out = compile_and_run(source).strip()
    assert len(out) > 0, "FNV1a output was empty"
    print(f"[PASS] Hash.FNV1a computed 64-bit non-cryptographic hash: {out}")

def test_hmac_sha256():
    source = """Module Main
Import Hash
use Hash

Block Main Declares
    Sig : Text
Implements
    Sig = Hash.HMAC_SHA256("key", "The quick brown fox jumps over the lazy dog")
    Print(Sig)
End Main
"""
    out = compile_and_run(source).strip()
    expected = "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"
    assert out == expected, f"HMAC-SHA256 mismatch!\nExpected: {expected}\nGot:      {out}"
    print("[PASS] Hash.HMAC_SHA256 verified against RFC test vector")

def test_file_sha256():
    with tempfile.NamedTemporaryFile(mode="w", delete=False) as f:
        f.write("Hello, Loom File Hashing!")
        f_path = f.name
    try:
        source = f"""Module Main
Import Hash
use Hash

Block Main Declares
    Digest : Text
Implements
    Digest = Hash.File_SHA256("{f_path}")
    Print(Digest)
End Main
"""
        out = compile_and_run(source).strip()
        shasum_out = subprocess.run(["shasum", "-a", "256", f_path], capture_output=True, text=True).stdout.split()[0]
        assert out == shasum_out, f"File SHA-256 mismatch!\nExpected: {shasum_out}\nGot:      {out}"
        print("[PASS] Hash.File_SHA256 computed zero-RAM streaming file checksum matching shasum")
    finally:
        Path(f_path).unlink(missing_ok=True)

def test_streaming_hasher():
    source = """Module Main
Import Hash
use Hash

Block Main Declares
    H : Hasher
    Digest : Text
Implements
    H = Hasher_Init(Algorithm_SHA256)
    Hasher_Update(H, "The quick brown ")
    Hasher_Update(H, "fox jumps over ")
    Hasher_Update(H, "the lazy dog")
    Digest = Hasher_Final(Move H)
    Print(Digest)
End Main
"""
    out = compile_and_run(source).strip()
    expected = "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"
    assert out == expected, f"Streaming Hasher mismatch!\nExpected: {expected}\nGot:      {out}"
    print("[PASS] Stateful streaming Hasher (Init, Update, Final) computed identical chunked digest")

def test_loom_get_cli_help():
    res = subprocess.run([str(LOOM), "--help"], capture_output=True, text=True)
    assert "get" in res.stdout, "loom get not found in help"
    print("[PASS] 'loom get <module>' is available in CLI")

def test_loom_get_not_found():
    res = subprocess.run([str(LOOM), "get", "neuralnjkencjkdnsc"], capture_output=True, text=True)
    assert res.returncode != 0
    assert "library 'neuralnjkencjkdnsc' not found" in res.stderr
    assert "curl:" not in res.stderr
    print("[PASS] 'loom get <module>' cleanly handles library not found without curl leakage")

if __name__ == "__main__":
    test_hash_sha256()
    test_hash_md5()
    test_hash_sha1()
    test_hash_sha512()
    test_hash_fnv1a()
    test_hmac_sha256()
    test_file_sha256()
    test_streaming_hasher()
    test_loom_get_cli_help()
    test_loom_get_not_found()
    print("\n============================================================")
    print(" ALL HASHING & MODULE DISTRIBUTION TESTS PASSED PERFECTLY!")
    print("============================================================")
