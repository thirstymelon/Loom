#!/usr/bin/env python3
"""Integration tests for Loom HTTP/2, HTTP/3 (QUIC/QPACK), and HTTP Server library."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LOOMC = REPO_ROOT / "build" / "loomc"
LIB_RT = REPO_ROOT / "build" / "libloom_rt.a"
STDLIB = REPO_ROOT / "stdlib"
HTTP_SERVER_SRC = Path("/Users/lokesh/Desktop/loom-libraries/libraries/http-server/src")

def compile_and_run(source_code: str, extra_inc: list = None) -> tuple:
    with tempfile.TemporaryDirectory() as tmpdir:
        src = Path(tmpdir) / "test.loom"
        obj = Path(tmpdir) / "test.o"
        exe = Path(tmpdir) / "test"
        src.write_text(source_code)

        cmd = [str(LOOMC), "-I", str(STDLIB)]
        if extra_inc:
            for inc in extra_inc:
                cmd.extend(["-I", str(inc)])
        cmd.extend(["--object", str(src), str(obj), "--entry"])

        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            return False, f"Compile error: {res.stderr}\n{res.stdout}"

        link = subprocess.run(["clang", str(obj), str(LIB_RT), "-o", str(exe)], capture_output=True, text=True)
        if link.returncode != 0:
            return False, f"Link error: {link.stderr}\n{link.stdout}"

        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
        return run.returncode == 0, run.stdout + run.stderr

class Http2Http3ServerTests(unittest.TestCase):
    def test_hpack_roundtrip(self):
        code = r"""Module TestHpack
Import Hpack
use Hpack

Block Main Declares
    Ctx : Context
    Raw : Text
    Enc : Text
    Dec : Text
    Expected : Text
Implements
    Ctx = Hpack.Context_New(4096)
    Raw += ":method: GET\r\n:path: /\r\n:scheme: https\r\nx-custom: test-val\r\n"
    Enc = Hpack.Encode_Headers(Ctx, Raw)
    Dec = Hpack.Decode_Headers(Ctx, Enc)
    Expected += ":method: GET\r\n:path: /\r\n:scheme: https\r\nx-custom: test-val\r\n"
    If Dec == Expected
        Print("PASS_HPACK\n")
    Else
        Print("FAIL_HPACK\n")
    End If
End Main
"""
        ok, out = compile_and_run(code)
        self.assertTrue(ok, f"Failed execution: {out}")
        self.assertIn("PASS_HPACK", out)

    def test_http2_frame_formatting(self):
        code = r"""Module TestH2Frame
Import Http2
use Http2

Block Main Declares
    Payload : Text
    Frm : Text
Implements
    Payload += "pingdata"
    Frm = Http2.Format_Frame(Payload.Length as Unsigned_32, 6 as Unsigned_8, 0 as Unsigned_8, 0 as Unsigned_32, Payload)
    If Frm.Length == 17
        Print("PASS_H2_FRAME\n")
    Else
        Print("FAIL_H2_FRAME\n")
    End If
End Main
"""
        ok, out = compile_and_run(code)
        self.assertTrue(ok, f"Failed execution: {out}")
        self.assertIn("PASS_H2_FRAME", out)

    def test_qpack_roundtrip(self):
        code = r"""Module TestQpack
Import Hpack
Import Qpack

Block Main Declares
    Ctx : Qpack_Context
    Raw : Text
    Enc : Text
    Dec : Text
    Expected : Text
Implements
    Ctx = Qpack.Qpack_New(4096)
    Raw += ":method: GET\r\n:path: /\r\n:scheme: https\r\nuser-agent: loom-http3\r\n"
    Enc = Qpack.Qpack_Encode_Headers(Ctx, Raw)
    Dec = Qpack.Qpack_Decode_Headers(Ctx, Enc)
    Expected += ":method: GET\r\n:path: /\r\n:scheme: https\r\nuser-agent: loom-http3\r\n"
    If Dec == Expected
        Print("PASS_QPACK\n")
    Else
        Print("FAIL_QPACK\n")
    End If
End Main
"""
        ok, out = compile_and_run(code)
        self.assertTrue(ok, f"Failed execution: {out}")
        self.assertIn("PASS_QPACK", out)

    def test_quic_varint_and_crypto(self):
        code = r"""Module TestQuicCrypto
Import Quic

Block Main Declares
    V : Text
    Di : Decoded_Varint
    Dcid : Text
    Keys : Text
Implements
    V = Quic.Varint_Encode(15213)
    Di = Quic.Varint_Decode(V, 1)
    If Di.Value == 15213
        Print("PASS_QUIC_VARINT\n")
    End If

    Dcid += "8394c8f03e515708"
    Keys = Quic_Initial_Keys(Dcid)
    If Keys.Length == 90
        Print("PASS_QUIC_CRYPTO\n")
    End If
End Main
"""
        ok, out = compile_and_run(code)
        self.assertTrue(ok, f"Failed execution: {out}")
        self.assertIn("PASS_QUIC_VARINT", out)
        self.assertIn("PASS_QUIC_CRYPTO", out)

    def test_http3_framing(self):
        code = r"""Module TestH3Frame
Import Http3
use Http3

Block Main Declares
    Payload : Text
    Frm : Text
Implements
    Payload += "hello h3"
    Frm = Http3.Format_H3_Frame(0, Payload)
    If Frm.Length == 10
        Print("PASS_H3_FRAME\n")
    End If
End Main
"""
        ok, out = compile_and_run(code)
        self.assertTrue(ok, f"Failed execution: {out}")
        self.assertIn("PASS_H3_FRAME", out)

    def test_http_server_library(self):
        if not HTTP_SERVER_SRC.exists():
            self.skipTest("HttpServer library not found")
        code = r"""Module TestServerLib
Import HttpServer
use HttpServer

Block Main Declares
    Raw : Text
    Req : Request
    Res : Response_Writer
    Exp_Method : Text
    Exp_Path : Text
Implements
    Exp_Method += "GET"
    Exp_Path += "/healthz"

    Raw += "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n"
    Req = HttpServer.Parse_Request(Raw)
    If Req.Method == Exp_Method And Req.Path == Exp_Path
        Print("PASS_SERVER_PARSE\n")
    End If

    Res = HttpServer.Res_New()
    Res = HttpServer.Res_Status(Res, 200)
    Res = HttpServer.Res_Json(Res, "{\"status\":\"healthy\"}")
    If Res.Status_Code == 200 And Res.Body.Length > 0
        Print("PASS_SERVER_RESPONSE\n")
    End If
End Main
"""
        ok, out = compile_and_run(code, extra_inc=[HTTP_SERVER_SRC])
        self.assertTrue(ok, f"Failed execution: {out}")
        self.assertIn("PASS_SERVER_PARSE", out)
        self.assertIn("PASS_SERVER_RESPONSE", out)

    def test_web_server_poc_live(self):
        poc_src = Path("/Users/lokesh/Desktop/loom-libraries/libraries/http-server/examples/web_server_poc.loom")
        if not poc_src.exists():
            self.skipTest("web_server_poc.loom not found")

        with tempfile.TemporaryDirectory() as tmpdir:
            obj = Path(tmpdir) / "poc.o"
            exe = Path(tmpdir) / "poc"
            cmd = [
                str(LOOMC),
                "-I", str(STDLIB),
                "-I", str(HTTP_SERVER_SRC),
                "--object", str(poc_src), str(obj),
                "--entry"
            ]
            res = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"Compile error: {res.stderr}\n{res.stdout}")

            link = subprocess.run(["clang", str(obj), str(LIB_RT), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(link.returncode, 0, f"Link error: {link.stderr}\n{link.stdout}")

            proc = subprocess.Popen([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                import http.client
                import time
                time.sleep(0.5)

                conn = http.client.HTTPConnection("127.0.0.1", 8088, timeout=5)
                conn.request("GET", "/api/status")
                r = conn.getresponse()
                body = r.read().decode("utf-8")
                conn.close()

                self.assertEqual(r.status, 200)
                self.assertIn('"status":"online"', body)

                # Shutdown server
                conn = http.client.HTTPConnection("127.0.0.1", 8088, timeout=5)
                conn.request("GET", "/api/shutdown")
                r = conn.getresponse()
                conn.close()
                self.assertEqual(r.status, 200)

                proc.wait(timeout=5)
                self.assertEqual(proc.returncode, 0)
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    proc.wait(timeout=2)

if __name__ == "__main__":
    unittest.main()
