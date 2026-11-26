#!/usr/bin/env python3
"""Offline wire-format tests for the Loom DNS encoder and parser."""
import pathlib
import subprocess
import sys
import tempfile

compiler = pathlib.Path(sys.argv[1]).resolve()
runtime = pathlib.Path(sys.argv[2]).resolve()
repo = pathlib.Path(sys.argv[3]).resolve()
stdlib = repo / "stdlib"
source_text = """Module DnsWireTest
Import Dns
Block Main Declares
    Host : Text
    Packet : Unsigned_8 Array[512]
    Random_Bytes : Unsigned_8 Array[8]
    Random_Length : Signed_64
    Query_Length : Signed_64
    Answer_Start : Signed_64
    Packet_Length : Signed_64
    Query_ID : Unsigned_16 = 19533
    Capacity_Result : Signed_64
    Random_Error : Signed_64
    Parsed : IPv4_Result
Implements
    Random_Length = System_Random(Address(Random_Bytes[1]), 8)
    Print(Random_Length)
    Host += "example.com"
    Query_Length = Dns.Encode_A_Query(Host, Address(Packet[1]), 512, Query_ID)
    Print(Query_Length)
    Print(Packet[1])
    Print(Packet[2])
    Print(Packet[3])
    Print(Packet[6])
    Print(Packet[13])
    Print(Packet[21])
    Print(Packet[25])
    Print(Packet[26])
    Print(Packet[27])
    Print(Packet[28])
    Print(Packet[29])
    Answer_Start = Query_Length + 1
    Packet[3] = 129
    Packet[4] = 128
    Packet[7] = 0
    Packet[8] = 1
    Packet[Answer_Start] = 192
    Packet[Answer_Start + 1] = 12
    Packet[Answer_Start + 2] = 0
    Packet[Answer_Start + 3] = 1
    Packet[Answer_Start + 4] = 0
    Packet[Answer_Start + 5] = 1
    Packet[Answer_Start + 6] = 0
    Packet[Answer_Start + 7] = 0
    Packet[Answer_Start + 8] = 0
    Packet[Answer_Start + 9] = 60
    Packet[Answer_Start + 10] = 0
    Packet[Answer_Start + 11] = 4
    Packet[Answer_Start + 12] = 93
    Packet[Answer_Start + 13] = 184
    Packet[Answer_Start + 14] = 216
    Packet[Answer_Start + 15] = 34
    Packet_Length = Query_Length + 16
    Parsed = Dns.Parse_A_Response(Packet[1 .. Packet_Length], Packet_Length as Unsigned_64, Query_ID)
    Print(Parsed.Status)
    Print(Parsed.IPv4)
    Print(Parsed.TTL)
    Packet[Answer_Start + 1] = 255
    Parsed = Dns.Parse_A_Response(Packet[1 .. Packet_Length], Packet_Length as Unsigned_64, Query_ID)
    Print(Parsed.Status)
    Packet[Answer_Start + 1] = 12
    Parsed = Dns.Parse_A_Response(Packet[1 .. Packet_Length], Packet_Length as Unsigned_64, Query_ID + 1)
    Print(Parsed.Status)
    Parsed = Dns.Parse_A_Response(Packet[1 .. Packet_Length], 12, Query_ID)
    Print(Parsed.Status)
    Capacity_Result = Dns.Encode_A_Query(Host, Address(Packet[1]), 28, Query_ID)
    Print(Capacity_Result)
    Random_Error = System_Random(Address(Random_Bytes[1]), -1)
    Print(Random_Error)
End Main
"""

with tempfile.TemporaryDirectory(prefix="loom-dns-wire-") as temp:
    temp = pathlib.Path(temp)
    source, obj, binary = temp / "main.loom", temp / "main.o", temp / "main"
    source.write_text(source_text)
    built = subprocess.run([str(compiler), "--object", "--entry", "-I", str(stdlib), str(source), str(obj)],
                           capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    linked = subprocess.run(["clang", str(obj), str(runtime), "-pthread", "-o", str(binary)],
                            capture_output=True, text=True)
    assert linked.returncode == 0, linked.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    lines = run.stdout.splitlines()
    assert lines[:13] == ["8", "29", "76", "77", "1", "1", "7", "3", "0", "0", "1", "0", "1"], run.stdout
    assert lines[13:] == ["0", "1572395042", "60", "-1", "-1", "-1", "-1", "-1"], run.stdout

print("[PASS] Loom DNS A-query encoding, compressed answer parsing, TTL, and malformed pointer rejection")
