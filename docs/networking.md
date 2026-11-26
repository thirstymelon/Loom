# Networking in Loom

Loom's networking stack is designed with a clean separation between compiler-lowered host operating-system primitives and high-level Loom standard library modules.

There is no separate C networking runtime (`rt_net.c`) and no `Extern Block` required in Loom source code. The compiler lowers primitive socket and readiness operations directly to host POSIX ABI calls (`socket`, `bind`, `connect`, `listen`, `accept`, `send`, `recv`, `sendto`, `recvfrom`, `setsockopt`, `getsockopt`, `poll`, `fcntl`, `close`), with dynamic platform TLS dispatch via OpenSSL and macOS Security framework.

## 1. Low-Level Compiler Primitives

All low-level networking primitives return negative values (typically `-1`) on failure, ensuring straightforward error propagation.

### TCP Primitives
- `Tcp_Listen(host: Text, port: Signed_32/Unsigned_16) -> Signed_32`: Binds an IPv4 socket to `host:port`, calls `listen()`, and returns the listening file descriptor.
- `Tcp_Listen_V6(host: Text, port: Signed_32/Unsigned_16) -> Signed_32`: Binds an IPv6 socket to `host:port` and listens.
- `Tcp_Accept(listener_fd: Signed_32) -> Signed_32`: Accepts a pending incoming client connection and returns the client descriptor.
- `Tcp_Connect(host: Text, port: Signed_32/Unsigned_16) -> Signed_32`: Establishes an outbound IPv4 TCP connection to `host:port`.
- `Tcp_Connect_V6(host: Text, port: Signed_32/Unsigned_16) -> Signed_32`: Establishes an outbound IPv6 TCP connection to `host:port`.
- `Tcp_Send(fd: Signed_32, data: Text/String) -> Signed_64`: Transmits bytes from `Text` or `String` and returns bytes accepted by the socket.
- `Tcp_Send_At(fd: Signed_32, data: Text, offset: Unsigned_64) -> Signed_64`: Transmits bytes starting at a 1-based offset.
- `Tcp_Send_Slice(fd: Signed_32, bytes: Unsigned_8 Slice) -> Signed_64`: Transmits a binary byte slice.
- `Tcp_Receive(fd: Signed_32, max_len: Signed_64) -> Text`: Reads up to `max_len` bytes into a new `Text` buffer.
- `Tcp_Receive_Buffer(fd: Signed_32, buf: Unsigned_8 Pointer, capacity: Signed_64) -> Signed_64`: Reads raw bytes into caller-allocated memory; returns bytes read, `0` on peer shutdown (EOF), or `-1` on error.
- `Tcp_Close(fd: Signed_32) -> Signed_32`: Closes the socket descriptor and releases OS resources.

### UDP Primitives
- `Udp_Bind(host: Text, port: Signed_32/Unsigned_16) -> Signed_32`: Creates and binds an IPv4 UDP socket.
- `Udp_Bind_V6(host: Text, port: Signed_32/Unsigned_16) -> Signed_32`: Creates and binds an IPv6 UDP socket.
- `Udp_Connect(host: Text, port: Signed_32/Unsigned_16) -> Signed_32`: Connects an IPv4 UDP socket to a specific peer endpoint for kernel-enforced source filtering and default destination routing.
- `Udp_Connect_V6(host: Text, port: Signed_32/Unsigned_16) -> Signed_32`: Connects an IPv6 UDP socket to a peer endpoint.
- `Udp_Send(fd: Signed_32, data: Text/String) -> Signed_64`: Sends datagram data on a connected UDP socket.
- `Udp_Send_Slice(fd: Signed_32, bytes: Unsigned_8 Slice) -> Signed_64`: Sends a byte slice on a connected UDP socket.
- `Udp_Send_To(fd: Signed_32, host: Text, port: Signed_32, data: Text/String) -> Signed_64`: Sends an IPv4 datagram to the specified destination.
- `Udp_Send_To_V6(fd: Signed_32, host: Text, port: Signed_32, data: Text/String) -> Signed_64`: Sends an IPv6 datagram to the specified destination.
- `Udp_Send_To_Slice(fd: Signed_32, host: Text, port: Signed_32, bytes: Unsigned_8 Slice) -> Signed_64`: Sends a byte slice to an IPv4 destination.
- `Udp_Send_To_Slice_V6(fd: Signed_32, host: Text, port: Signed_32, bytes: Unsigned_8 Slice) -> Signed_64`: Sends a byte slice to an IPv6 destination.
- `Udp_Receive_Buffer(fd: Signed_32, buf: Unsigned_8 Pointer, capacity: Signed_64) -> Signed_64`: Receives a datagram into caller-allocated memory.
- `Udp_Receive_From(fd: Signed_32, max_len: Signed_64) -> Text`: Receives a datagram as `Text`.
- `Udp_Close(fd: Signed_32) -> Signed_32`: Closes the UDP socket.

### Socket Control & Readiness Primitives
- `Socket_Set_Timeout(fd: Signed_32, milliseconds: Signed_32) -> Signed_32`: Sets `SO_RCVTIMEO` and `SO_SNDTIMEO`. `0` disables timeout.
- `Socket_Wait(fd: Signed_32, event_mask: Unsigned_32, timeout_ms: Signed_32) -> Signed_32`: Waits for readiness using OS `poll()`.
  - Bit 1: Readable (`POLLIN`)
  - Bit 2: Writable (`POLLOUT`)
  - Bit 4: Error/Hangup (`POLLERR` | `POLLHUP` | `POLLNVAL`)
  - Returns `0` on timeout, negative on error.
- `Socket_Set_Nonblocking(fd: Signed_32, nonblocking: Boolean) -> Signed_32`: Configures non-blocking I/O mode via `fcntl(F_SETFL, O_NONBLOCK)`.
- `Socket_Get_Error(fd: Signed_32) -> Signed_32`: Retrieves pending socket error via `getsockopt(SOL_SOCKET, SO_ERROR)`. Returns `0` if clean.
- `System_Random(buf: Unsigned_8 Pointer, len: Signed_64) -> Signed_64`: Populates buffer with cryptographically secure random bytes from OS entropy (`getentropy` on Darwin, `getrandom` on Linux).

### TLS Backend Primitives
- `Tls_Client_New(host: Text, ca_path: Text) -> Signed_64`: Instantiates a TLS client context with hostname verification and configurable trust roots.
- `Tls_Connect(handle: Signed_64, fd: Signed_32) -> Signed_32`: Performs TLS handshake and certificate validation over the connected socket.
- `Tls_Send(handle: Signed_64, data: Text/String) -> Signed_64`: Encrypts and transmits data.
- `Tls_Send_Slice(handle: Signed_64, bytes: Unsigned_8 Slice) -> Signed_64`: Encrypts and transmits a binary slice.
- `Tls_Receive(handle: Signed_64, max_len: Signed_64) -> Text`: Decrypts and returns up to `max_len` bytes as `Text`.
- `Tls_Receive_Buffer(handle: Signed_64, buf: Unsigned_8 Pointer, capacity: Signed_64) -> Signed_64`: Decrypts into raw memory.
- `Tls_Close(handle: Signed_64) -> Signed_32`: Performs clean TLS shutdown and releases context resources.

---

## 2. Standard Library Modules

Standard library modules are written entirely in native Loom code and located in `stdlib/`:

### Module `Net` (`stdlib/Net.loom`)
Convenience streaming helpers for TCP and UDP:
- `Net.Tcp_Send_All(Connection, Data)`: Loops until all text bytes are transmitted.
- `Net.Tcp_Receive_Exactly(Connection, Byte_Count)`: Accumulates exactly `Byte_Count` bytes unless EOF is reached.
- `Net.Tcp_Receive_Until_Close(Connection, Chunk_Size)`: Accumulates data until the peer closes the connection.
- `Net.Tcp_Wait_Readable(Connection, Timeout_Ms)`: Returns boolean indicating read readiness.
- `Net.Tcp_Wait_Writable(Connection, Timeout_Ms)`: Returns boolean indicating write readiness.
- `Net.Socket_Make_Nonblocking(Connection, Nonblocking)`: Toggles non-blocking socket state.
- `Net.Socket_Error(Connection)`: Returns current socket error code.

### Module `NetAsync` (`stdlib/NetAsync.loom`)
Non-blocking polling and readiness abstraction:
- `Record Poll_Result`: Exposes `Events`, `Readable`, `Writable`, `Error`, and `Timed_Out`.
- `NetAsync.Wait_Socket(Connection, Want_Read, Want_Write, Timeout_Ms) -> Poll_Result`: Structured poll wrapper.
- `NetAsync.Wait_Connect(Connection, Timeout_Ms) -> Signed_32`: Non-blocking connect completion verification.
- `NetAsync.Read_Nonblocking(Connection, Max_Bytes, Timeout_Ms) -> Text`: Timed non-blocking read.
- `NetAsync.Write_Nonblocking(Connection, Data, Timeout_Ms) -> Signed_64`: Timed non-blocking send.

### Module `Dns` (`stdlib/Dns.loom`)
DNS wire-format encoder, parser, and resolver:
- `Dns.Encode_A_Query(Host, Output, Capacity, Query_ID)`: Wire-format serialization.
- `Dns.Parse_A_Response(Packet, Packet_Length, Query_ID)`: Validates ID, flags, skips question section, resolves compressed domain pointers (0xC0), and parses IPv4 A-records and TTL.
- `Dns.Resolve_A(Host, Name_Server, Port, Timeout_Ms)`: Performs resolution via connected UDP (`Udp_Connect`), enforcing OS-level source filtering so spoofed responses are rejected.
- `Dns.Resolve_A_V6(Host, Name_Server, Port, Timeout_Ms)`: IPv6 DNS resolution.

### Module `Tls` (`stdlib/Tls.loom`)
Full TLS client transport layer:
- `Record Connection`: Encapsulates `Handle`, `Socket`, `Host`, and `Connected`.
- `Tls.Connect(Host, Port) -> Connection`: Establishes TLS connection using system default trust store.
- `Tls.Connect_With_Roots(Host, Port, Ca_Path) -> Connection`: Establishes TLS connection with custom CA certificate path.
- `Tls.Send(Conn, Data)`, `Tls.Send_Slice(Conn, Bytes)`, `Tls.Send_All(Conn, Data)`.
- `Tls.Receive(Conn, Max_Bytes)`, `Tls.Receive_Buffer(Conn, Buffer, Capacity)`.
- `Tls.Receive_Until_Close(Conn, Chunk_Size)`.
- `Tls.Close(Conn)`: Shuts down TLS and closes the underlying socket.

### Module `Http` (`stdlib/Http.loom`)
HTTP/1.1 client supporting HTTP and HTTPS:
- `Record Parsed_Url`: `Scheme`, `Host`, `Port`, `Path`, `Is_Https`.
- `Record Response`: `Status` (e.g. 200), `Status_Text` (e.g. "OK"), `Headers`, `Body`.
- `Http.Parse_Url(Url) -> Parsed_Url`: Decomposes URLs (`http://`, `https://`, port overrides, paths).
- `Http.Parse_Response(Raw_Response) -> Response`: Parses HTTP status code, status text, header block, and body.
- `Http.Get(Url) -> Response`: Dispatches GET request. Automatically routes through TLS for `https://`.
- `Http.Post(Url, Content_Type, Body) -> Response`: Dispatches POST request with automatic `Content-Length`.
- `Http.Get_With_Roots(Url, Ca_Path) -> Response`: HTTPS GET with custom trust roots.
- `Http.Post_With_Roots(Url, Content_Type, Body, Ca_Path) -> Response`: HTTPS POST with custom trust roots.
- `Http.Request(Method, Url, Headers, Body, Ca_Path) -> Response`: General HTTP/1.1 request dispatcher.

---

## 3. Example Usage

### HTTPS GET Request in Loom
```loom
Module Main
Import Http

Block Main Declares
    Url : Text
    Resp : Response
Implements
    Url += "https://127.0.0.1:8443/api/v1/health"
    Resp = Http.Get(Url)
    Print(Resp.Status)
    Print(Resp.Status_Text)
    Print(Resp.Body)
End Main
```

### Connected UDP with Source Filtering
```loom
Module UdpExample
Import Net

Block Main Declares
    Socket : Signed_32
    Bytes : Unsigned_8 Array[4]
    Sent : Signed_64
Implements
    Socket = Udp_Connect("127.0.0.1", 5353)
    If Socket >= 0
        Bytes[1] = 1; Bytes[2] = 2; Bytes[3] = 3; Bytes[4] = 4
        Sent = Udp_Send_Slice(Socket, Bytes[1 .. 4])
        Udp_Close(Socket)
    End If
End Main
```

---

## 4. HTTP/2 Architecture (`stdlib/Hpack.loom` & `stdlib/Http2.loom`)

Loom implements complete HTTP/2 binary framing and header compression per **RFC 9113** and **RFC 7541**:

- **ALPN Protocol Negotiation**: `Tls_Set_Alpn(Handle, "h2,http/1.1")` and `Tls_Get_Alpn(Handle)` negotiate `h2` during TLS handshakes.
- **HPACK Compression (`Hpack.loom`)**: 61-entry static table, prefix integer encoding/decoding, and literal header representations.
- **HTTP/2 Client (`Http2.loom`)**: 24-byte connection preface (`PRI * HTTP/2.0...`), SETTINGS exchange, WINDOW_UPDATE flow control, and stream multiplexing.

### HTTP/2 Client Example
```loom
Module Http2Example
Import Http2
use Http2

Block Main Declares
    Conn : Http2_Connection
    Host : Text
    Path : Text
    Empty_Hdr : Text
    Empty_Body : Text
    Resp : Response
    Status : Signed_32
Implements
    Host += "example.com"
    Path += "/"
    Conn = Http2.Connect_H2(Host, 443)
    If Conn.Is_Connected
        Resp = Http2.Request_H2(Conn, "GET", Path, Empty_Hdr, Empty_Body)
        Print("HTTP/2 Status: ")
        Print(Resp.Status)
        Print("\n")
        Status = Http2.Close_H2(Conn)
    End If
End Main
```

---

## 5. HTTP/3 & QUIC Transport (`stdlib/Quic.loom`, `stdlib/Qpack.loom`, `stdlib/Http3.loom`)

Loom implements **QUIC (RFC 9000)** over connected UDP and **HTTP/3 (RFC 9114)** with **QPACK (RFC 9204)**:

- **QUIC Cryptographic Primitives**: Lowered via OpenSSL dynamic dispatch (`Quic_Initial_Keys`, `Quic_Hp_Mask`, `Quic_Encrypt`, `Quic_Decrypt`).
- **QPACK Compression (`Qpack.loom`)**: Static table lookups and integer variable-length codecs.
- **HTTP/3 Client (`Http3.loom`)**: Control stream establishment and bidirectional request/response multiplexing.

---

## 6. Standalone HTTP Server Library (`loom-libraries/http-server`)

The standalone HTTP server library is located in `~/Desktop/loom-libraries/libraries/http-server/`:

- **Request Parsing (`HttpServer.Parse_Request`)**: Parses method, URL path, query string parameters, multi-line headers, and request body.
- **Response Builder**: Fluent builder for status codes (`Res_Status`), response headers (`Res_Header`), plain text (`Res_Text`), and JSON (`Res_Json`).
- **TCP Listener**: Nonblocking and blocking socket listener management (`Server_Start`, `Server_Accept`, `Server_Close`).

### Server Example
```loom
Module Main
Import HttpServer
use HttpServer

Block Main Declares
    Cfg : Server_Config
    S : Server
    Client_Fd : Signed_32
    Req : Request
    Res : Response_Writer
    Raw_Msg : Text
    Status : Signed_32
Implements
    Cfg.Host += "127.0.0.1"
    Cfg.Port = 8080

    S = HttpServer.Server_New(Cfg)
    S = HttpServer.Server_Start(S)
    Print("Loom HTTP Server running on http://127.0.0.1:8080\n")

    Client_Fd = HttpServer.Server_Accept(S)
    If Client_Fd >= 0
        Raw_Msg = Net.Tcp_Receive_Until_Close(Client_Fd, 4096)
        Req = HttpServer.Parse_Request(Raw_Msg)

        Res = HttpServer.Res_New()
        Res = HttpServer.Res_Json(Res, "{\"status\":\"ok\",\"engine\":\"loom\"}")
        Status = HttpServer.Res_Send(Res, Client_Fd)
        Tcp_Close(Client_Fd)
    End If

    Status = HttpServer.Server_Close(S)
End Main
```

