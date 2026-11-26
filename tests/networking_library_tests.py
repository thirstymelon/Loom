#!/usr/bin/env python3
"""Compiler and packaging checks for the Loom-native networking library."""
import pathlib
import subprocess
import sys
import tempfile

compiler = pathlib.Path(sys.argv[1]).resolve()
repo = pathlib.Path(sys.argv[2]).resolve()
stdlib = repo / "stdlib"
for mod_name in ("Net.loom", "Dns.loom", "NetAsync.loom", "Tls.loom", "Http.loom"):
    mod_path = stdlib / mod_name
    assert mod_path.is_file(), f"{mod_name} standard library module is missing"
    assert "Extern Block" not in mod_path.read_text(), f"{mod_name} must not use Extern Blocks"
assert not (repo / "src/runtime/rt_net.c").exists(), "network C implementation must not be present"

with tempfile.TemporaryDirectory(prefix="loom-net-library-") as temp:
    temp = pathlib.Path(temp)
    source = temp / "main.loom"
    ir = temp / "main.ll"
    source.write_text("""Module NetLibraryCheck
Import Net
Import NetAsync
Import Dns
Import Tls
Import Http
Block Main Declares
    Data : Text
    Bytes : Unsigned_8 Array[32]
    Socket : Signed_32 = -1
    Listener : Signed_32
    Datagram : Signed_32
    Listener_V6 : Signed_32
    Datagram_V6 : Signed_32
    Io_Count : Signed_64
    Socket_Status : Signed_32
Implements
    Data += "compiler-backed networking"
    Listener = Tcp_Listen("127.0.0.1", 70000)
    Socket = Tcp_Connect("127.0.0.1", 443)
    Datagram = Udp_Bind("127.0.0.1", 0)
    Listener_V6 = Tcp_Listen_V6("::1", 70000)
    Datagram_V6 = Udp_Bind_V6("::1", 0)
    Socket_Status = Socket_Set_Timeout(Socket, 1000)
    Socket_Status = Socket_Wait(Socket, 3, 1000)
    Socket_Status = Socket_Set_Nonblocking(Socket, True)
    Socket_Status = Socket_Get_Error(Socket)
    Socket = Tcp_Accept(Listener)
    Io_Count = Tcp_Send(Socket, Data)
    Data = Tcp_Receive(Socket, 16)
    Udp_Close(Datagram)
    Datagram = Udp_Connect("127.0.0.1", 53)
    Datagram_V6 = Udp_Connect_V6("::1", 53)
    Io_Count = Udp_Send(Datagram, Data)
    Io_Count = Udp_Send_Slice(Datagram, Bytes[1 .. 32])
    Udp_Close(Datagram)
    Udp_Close(Datagram_V6)
    Tcp_Close(Socket)
    Tcp_Close(Listener)
    Net.Tcp_Send_All(Socket, Data)
    Net.Tcp_Receive_Exactly(Socket, 1)
    Net.Tcp_Receive_Until_Close(Socket, 16)
    Net.Socket_Make_Nonblocking(Socket, False)
    Net.Socket_Error(Socket)
    Net.Udp_Wait(Socket, 1, 100)
    Net.Udp_Set_Timeout(Socket, 500)
    Net.Tcp_Wait_Readable(Socket, 50)
    Net.Tcp_Wait_Writable(Socket, 50)
    Io_Count = Tcp_Send_Slice(Socket, Bytes[1 .. 32])
    Io_Count = Tcp_Receive_Buffer(Socket, Address(Bytes[1]), 32)
    Io_Count = Udp_Send_To_Slice(Socket, "127.0.0.1", 53, Bytes[1 .. 32])
    Io_Count = Udp_Send_To_Slice_V6(Datagram_V6, "::1", 53, Bytes[1 .. 32])
    Io_Count = Udp_Receive_Buffer(Socket, Address(Bytes[1]), 32)
End Main
""")
    checked = subprocess.run([str(compiler), "--check", "-I", str(stdlib), str(source)],
                             capture_output=True, text=True)
    assert checked.returncode == 0, checked.stderr
    compiled = subprocess.run([str(compiler), "-I", str(stdlib), str(source), str(ir)],
                              capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    generated = ir.read_text()
    assert "@send(" in generated, "expected direct host socket ABI lowering"
    assert "@recv(" in generated, "expected direct host socket ABI lowering"
    assert "@sendto(" in generated and "@recvfrom(" in generated, "expected UDP ABI lowering"
    assert "@setsockopt(" in generated and "@poll(" in generated, "expected timeout and readiness primitives"
    assert "__loom_native_tcp_listen_v6" in generated and "__loom_native_udp_send_to_v6" in generated
    assert "__loom_native_udp_connect" in generated and "__loom_native_udp_connect_v6" in generated
    assert "__loom_native_socket_set_nonblocking" in generated and "__loom_native_socket_get_error" in generated
    for symbol in ("@inet_pton(", "@socket(", "@bind(", "@listen(", "@connect(", "@accept(", "@close(", "@fcntl(", "@getsockopt("):
        assert symbol in generated, f"missing host ABI lowering for {symbol}"
    assert "icmp ule i64" in generated, "port must be validated before conversion to network byte order"
    assert "loom_net_" not in generated, "legacy C networking runtime should not be referenced"

print("[PASS] Loom networking library imports, type-checks, and lowers without C networking or Extern Blocks")
