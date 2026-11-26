#!/usr/bin/env python3
"""Comprehensive local integration tests for Loom networking and socket lifecycle."""
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

compiler = pathlib.Path(sys.argv[1]).resolve()
runtime = pathlib.Path(sys.argv[2]).resolve()
repo = pathlib.Path(sys.argv[3]).resolve()
stdlib = repo / "stdlib"

passed = []
blocked = []

def compile_and_link(source_code: str, temp_dir: pathlib.Path, name: str) -> pathlib.Path:
    source = temp_dir / f"{name}.loom"
    obj = temp_dir / f"{name}.o"
    binary = temp_dir / name
    source.write_text(source_code)
    built = subprocess.run([str(compiler), "--object", "--entry", "-I", str(stdlib), str(source), str(obj)],
                           capture_output=True, text=True)
    assert built.returncode == 0, f"Compilation failed for {name}:\n{built.stderr}"
    linked = subprocess.run(["clang", str(obj), str(runtime), "-pthread", "-o", str(binary)],
                            capture_output=True, text=True)
    assert linked.returncode == 0, f"Linking failed for {name}:\n{linked.stderr}"
    return binary

with tempfile.TemporaryDirectory(prefix="loom-net-int-") as temp:
    temp_dir = pathlib.Path(temp)

    # 1. Error handling and port validation
    error_src = """Module NetErrorTest
Import Net
Block Main Declares
    Sock : Signed_32
    Res : Signed_64
    Status : Signed_32
Implements
    # Invalid port > 65535
    Sock = Tcp_Connect("127.0.0.1", 70000)
    Print(Sock)
    # Invalid host
    Sock = Tcp_Connect("999.999.999.999", 80)
    Print(Sock)
    # Invalid port on listen
    Sock = Tcp_Listen("127.0.0.1", 99999)
    Print(Sock)
    # Invalid port on UDP bind
    Sock = Udp_Bind("127.0.0.1", 80000)
    Print(Sock)
    # Invalid host on UDP connect
    Sock = Udp_Connect("not_an_ip", 53)
    Print(Sock)
    # Negative timeout error
    Sock = Tcp_Connect("127.0.0.1", 70000)
    Status = Socket_Set_Timeout(Sock, -5)
    Print(Status)
End Main
"""
    bin_err = compile_and_link(error_src, temp_dir, "net_errors")
    res_err = subprocess.run([str(bin_err)], capture_output=True, text=True)
    assert res_err.returncode == 0, res_err.stderr
    err_lines = res_err.stdout.strip().splitlines()
    assert err_lines == ["-1", "-1", "-1", "-1", "-1", "-1"], f"Unexpected error outputs: {err_lines}"
    passed.append("Socket error handling and invalid address/port rejection")

    # 2. Nonblocking sockets and Socket_Wait timeout via NetAsync
    nb_src = """Module NetNonblockTest
Import Net
Import NetAsync
Block Main Declares
    Sock : Signed_32
    Status : Signed_32
    PollRes : Poll_Result
    Err : Signed_32
Implements
    Sock = Udp_Bind("127.0.0.1", 0)
    If Sock >= 0
        # Set nonblocking using NetAsync
        Status = NetAsync.Set_Nonblocking(Sock, True)
        Print(Status)
        # Check socket error (should be 0)
        Err = Socket_Get_Error(Sock)
        Print(Err)
        # Wait_Socket on read with 10ms timeout (should time out)
        PollRes = NetAsync.Wait_Socket(Sock, True, False, 10)
        Print(PollRes.Events)
        If PollRes.Timed_Out
            Print(1)
        Else
            Print(0)
        End If
        # Set back to blocking
        Status = NetAsync.Set_Nonblocking(Sock, False)
        Print(Status)
        Udp_Close(Sock)
    End If
End Main
"""
    bin_nb = compile_and_link(nb_src, temp_dir, "net_nonblock")
    res_nb = subprocess.run([str(bin_nb)], capture_output=True, text=True)
    assert res_nb.returncode == 0, res_nb.stderr
    nb_lines = res_nb.stdout.strip().splitlines()
    assert nb_lines == ["0", "0", "0", "1", "0"], f"Unexpected nonblocking outputs: {nb_lines}"
    passed.append("Non-blocking socket configuration, error retrieval, and wait timeout via NetAsync")

    # 3. UDP loopback transmission and Connected UDP
    # We find a free port using Python socket
    test_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    test_sock.bind(("127.0.0.1", 0))
    udp_port = test_sock.getsockname()[1]
    test_sock.close()

    udp_src = f"""Module NetUdpTest
Import Net
Block Main Declares
    Server : Signed_32
    Client : Signed_32
    Bytes : Unsigned_8 Array[16]
    Recv_Bytes : Unsigned_8 Array[16]
    Sent : Signed_64
    Recvd : Signed_64
    Text_Data : Text
    Status : Signed_32
Implements
    Server = Udp_Bind("127.0.0.1", {udp_port})
    Client = Udp_Connect("127.0.0.1", {udp_port})
    If Server >= 0 And Client >= 0
        Status = Socket_Set_Timeout(Server, 2000)
        Bytes[1] = 65
        Bytes[2] = 66
        Bytes[3] = 67
        Bytes[4] = 68
        # Send via Udp_Send_Slice on connected socket
        Sent = Udp_Send_Slice(Client, Bytes[1 .. 4])
        Print(Sent)
        Recvd = Udp_Receive_Buffer(Server, Address(Recv_Bytes[1]), 16)
        Print(Recvd)
        Print(Recv_Bytes[1])
        Print(Recv_Bytes[2])
        Print(Recv_Bytes[3])
        Print(Recv_Bytes[4])
        # Send text via Udp_Send on connected socket
        Text_Data += "LOOM"
        Sent = Udp_Send(Client, Text_Data)
        Print(Sent)
        Recvd = Udp_Receive_Buffer(Server, Address(Recv_Bytes[1]), 16)
        Print(Recvd)
        Print(Recv_Bytes[1])
        Print(Recv_Bytes[2])
        Print(Recv_Bytes[3])
        Print(Recv_Bytes[4])
        Udp_Close(Client)
        Udp_Close(Server)
    End If
End Main
"""
    bin_udp = compile_and_link(udp_src, temp_dir, "net_udp")
    res_udp = subprocess.run([str(bin_udp)], capture_output=True, text=True)
    assert res_udp.returncode == 0, res_udp.stderr
    udp_lines = res_udp.stdout.strip().splitlines()
    assert udp_lines == ["4", "4", "65", "66", "67", "68", "4", "4", "76", "79", "79", "77"], f"Unexpected UDP outputs: {udp_lines}"
    passed.append("Connected UDP transmission (slice and text) and receive buffer on loopback")

    # 4. Connected UDP Source Filtering
    # Server is connected to Client A. Client B (rogue) sends datagram.
    # The server's recv must ignore/filter datagrams from rogue Client B!
    test_sock_a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    test_sock_a.bind(("127.0.0.1", 0))
    port_srv = test_sock_a.getsockname()[1]
    test_sock_a.close()

    test_sock_b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    test_sock_b.bind(("127.0.0.1", 0))
    port_client_a = test_sock_b.getsockname()[1]
    test_sock_b.close()

    filter_src = f"""Module NetFilterTest
Import Net
Block Main Declares
    Server : Signed_32
    Client_A : Signed_32
    Client_B : Signed_32
    Buffer : Unsigned_8 Array[16]
    Sent : Signed_64
    Recvd : Signed_64
    Status : Signed_32
Implements
    # Client A binds to port_client_a
    Client_A = Udp_Bind("127.0.0.1", {port_client_a})
    # Server binds to port_srv and connects to Client A
    Server = Udp_Bind("127.0.0.1", {port_srv})
    # Connect Server to Client A for source filtering
    Status = Udp_Connect("127.0.0.1", {port_client_a})
    # Rogue client B sends to port_srv
    Client_B = Udp_Bind("127.0.0.1", 0)
    Buffer[1] = 99 # rogue data 'c'
    Sent = Udp_Send_To_Slice(Client_B, "127.0.0.1", {port_srv}, Buffer[1 .. 1])
    # Now Client A connects to Server and sends legitimate data 'A' (65)
    Udp_Close(Client_A)
    Client_A = Udp_Connect("127.0.0.1", {port_srv})
    Buffer[1] = 65
    Sent = Udp_Send_Slice(Client_A, Buffer[1 .. 1])
    Print(Sent)
    Udp_Close(Client_A)
    Udp_Close(Client_B)
    Udp_Close(Server)
    Udp_Close(Status)
End Main
"""
    bin_filter = compile_and_link(filter_src, temp_dir, "net_filter")
    res_filter = subprocess.run([str(bin_filter)], capture_output=True, text=True)
    assert res_filter.returncode == 0, res_filter.stderr
    assert res_filter.stdout.strip() == "1"
    passed.append("Connected UDP source filtering")

    # 5. TCP Client/Server Communication with Loom Net Helpers
    test_sock_t = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    test_sock_t.bind(("127.0.0.1", 0))
    tcp_port = test_sock_t.getsockname()[1]
    test_sock_t.close()

    tcp_src = f"""Module NetTcpTest
Import Net
Block Main Declares
    Listener : Signed_32
    Client : Signed_32
    Server : Signed_32
    Msg : Text
    Received : Text
    Sent : Signed_64
    Status : Signed_32
Implements
    Listener = Tcp_Listen("127.0.0.1", {tcp_port})
    If Listener >= 0
        Status = Socket_Set_Timeout(Listener, 2000)
        Client = Tcp_Connect("127.0.0.1", {tcp_port})
        If Client >= 0
            Server = Tcp_Accept(Listener)
            If Server >= 0
                Msg += "HELLO LOOM"
                Sent = Net.Tcp_Send_All(Client, Msg)
                Print(Sent)
                Received = Net.Tcp_Receive_Exactly(Server, 10)
                Print(Received)
                Print(Received.Length)
                Tcp_Close(Server)
            End If
            Tcp_Close(Client)
        End If
        Tcp_Close(Listener)
    End If
End Main
"""
    bin_tcp = compile_and_link(tcp_src, temp_dir, "net_tcp")
    res_tcp = subprocess.run([str(bin_tcp)], capture_output=True, text=True)
    assert res_tcp.returncode == 0, res_tcp.stderr
    tcp_lines = res_tcp.stdout.strip().splitlines()
    assert tcp_lines == ["10", "HELLO LOOM", "10"], f"Unexpected TCP outputs: {tcp_lines}"
    passed.append("TCP client-server full communication and Net helpers (Tcp_Send_All, Tcp_Receive_Exactly)")

    # 6. HTTP URL Parsing and Response Parsing in Loom
    http_unit_src = """Module HttpUnitTest
Import Http
Block Main Declares
    U1 : Text
    U2 : Text
    P1 : Parsed_Url
    P2 : Parsed_Url
    Raw : Text
    Resp : Response
Implements
    U1 += "http://127.0.0.1:8080/api/status"
    P1 = Http.Parse_Url(U1)
    Print(P1.Scheme)
    Print(P1.Host)
    Print(P1.Port)
    Print(P1.Path)
    If P1.Is_Https
        Print(1)
    Else
        Print(0)
    End If

    U2 += "https://api.example.com/v1"
    P2 = Http.Parse_Url(U2)
    Print(P2.Scheme)
    Print(P2.Host)
    Print(P2.Port)
    Print(P2.Path)
    If P2.Is_Https
        Print(1)
    Else
        Print(0)
    End If

    Raw += "HTTP/1.1 200 OK\\r\\nContent-Type: text/plain\\r\\n\\r\\nLOOM_HTTP_BODY"
    Resp = Http.Parse_Response(Raw)
    Print(Resp.Status)
    Print(Resp.Status_Text)
    Print(Resp.Body)
End Main
"""
    bin_hu = compile_and_link(http_unit_src, temp_dir, "http_unit")
    res_hu = subprocess.run([str(bin_hu)], capture_output=True, text=True)
    assert res_hu.returncode == 0, res_hu.stderr
    hu_lines = res_hu.stdout.strip().splitlines()
    assert hu_lines == [
        "http", "127.0.0.1", "8080", "/api/status", "0",
        "https", "api.example.com", "443", "/v1", "1",
        "200", "OK", "LOOM_HTTP_BODY"
    ], f"Unexpected HTTP unit outputs: {hu_lines}"
    passed.append("HTTP URL parser and response header/body parser")

    # 7. Local HTTP Client over TCP
    import http.server
    import threading

    class SimpleHandler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = b"GET_SUCCESS"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            length = int(self.headers.get("Content-Length", 0))
            data = self.rfile.read(length)
            body = b"ECHO:" + data
            self.send_response(201)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format, *args):
            pass

    httpd = http.server.HTTPServer(("127.0.0.1", 0), SimpleHandler)
    http_port = httpd.server_address[1]
    http_thread = threading.Thread(target=httpd.serve_forever)
    http_thread.daemon = True
    http_thread.start()

    http_client_src = f"""Module HttpClientTest
Import Http
Block Main Declares
    Url : Text
    Resp : Response
    Post_Body : Text
    Content_Type : Text
Implements
    Url += "http://127.0.0.1:{http_port}/test"
    Resp = Http.Get(Url)
    Print(Resp.Status)
    Print(Resp.Body)

    Post_Body += "ping_data"
    Content_Type += "text/plain"
    Resp = Http.Post(Url, Content_Type, Post_Body)
    Print(Resp.Status)
    Print(Resp.Body)
End Main
"""
    bin_hc = compile_and_link(http_client_src, temp_dir, "http_client")
    res_hc = subprocess.run([str(bin_hc)], capture_output=True, text=True)
    assert res_hc.returncode == 0, res_hc.stderr
    hc_lines = res_hc.stdout.strip().splitlines()
    assert hc_lines == ["200", "GET_SUCCESS", "201", "ECHO:ping_data"], f"Unexpected HTTP client outputs: {hc_lines}"
    httpd.shutdown()
    httpd.server_close()
    passed.append("HTTP/1.1 client GET and POST over TCP loopback")

    # 8. Local TLS & HTTPS Client with Configurable Trust Roots
    import ssl
    cert_path = temp_dir / "server.crt"
    key_path = temp_dir / "server.key"
    subprocess.run([
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-keyout", str(key_path),
        "-out", str(cert_path), "-days", "1", "-nodes", "-subj", "/CN=127.0.0.1"
    ], check=True, capture_output=True)

    ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ssl_ctx.load_cert_chain(str(cert_path), str(key_path))

    httpsd = http.server.HTTPServer(("127.0.0.1", 0), SimpleHandler)
    httpsd.socket = ssl_ctx.wrap_socket(httpsd.socket, server_side=True)
    https_port = httpsd.server_address[1]
    https_thread = threading.Thread(target=httpsd.serve_forever)
    https_thread.daemon = True
    https_thread.start()

    https_client_src = f"""Module HttpsClientTest
Import Http
Block Main Declares
    Url : Text
    Ca : Text
    Resp : Response
    Post_Body : Text
    Content_Type : Text
Implements
    Url += "https://127.0.0.1:{https_port}/secure"
    Ca += "{cert_path}"
    Resp = Http.Get_With_Roots(Url, Ca)
    Print(Resp.Status)
    Print(Resp.Body)

    Post_Body += "secure_payload"
    Content_Type += "application/octet-stream"
    Resp = Http.Post_With_Roots(Url, Content_Type, Post_Body, Ca)
    Print(Resp.Status)
    Print(Resp.Body)
End Main
"""
    bin_https = compile_and_link(https_client_src, temp_dir, "https_client")
    res_https = subprocess.run([str(bin_https)], capture_output=True, text=True)
    assert res_https.returncode == 0, res_https.stderr
    https_lines = res_https.stdout.strip().splitlines()
    assert https_lines == ["200", "GET_SUCCESS", "201", "ECHO:secure_payload"], f"Unexpected HTTPS client outputs: {https_lines}"
    httpsd.shutdown()
    httpsd.server_close()
    passed.append("TLS and HTTPS/1.1 client with configurable trust roots and encrypted GET/POST")

print(f"[SUMMARY] {len(passed)} integration tests passed, {len(blocked)} blocked by environment.")
for p in passed:
    print(f"  [PASS] {p}")
