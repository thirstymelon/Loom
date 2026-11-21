#include "loom_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

/* ---------------------------------------------------------------------------
   Native Sockets / Networking Implementation
   --------------------------------------------------------------------------- */

int32_t loom_net_tcp_listen(const char* host, uint16_t port) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
#if !defined(_WIN32)
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!host || strlen(host) == 0 || strcmp(host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, host, &addr.sin_addr);
    }
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#if !defined(_WIN32)
        close(fd);
#else
        closesocket(fd);
#endif
        return -1;
    }
    if (listen(fd, 128) < 0) {
#if !defined(_WIN32)
        close(fd);
#else
        closesocket(fd);
#endif
        return -1;
    }
    return (int32_t)fd;
}

int32_t loom_net_tcp_accept(int32_t listener_fd) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(listener_fd, (struct sockaddr*)&client_addr, &addr_len);
    return (int32_t)client_fd;
}

int32_t loom_net_tcp_connect(const char* host, uint16_t port) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#if !defined(_WIN32)
        close(fd);
#else
        closesocket(fd);
#endif
        return -1;
    }
    return (int32_t)fd;
}

int64_t loom_net_tcp_send(int32_t fd, const void* buf, uint64_t len) {
    if (fd < 0 || !buf) return -1;
    ssize_t sent = send(fd, (const char*)buf, (size_t)len, 0);
    return (int64_t)sent;
}

int64_t loom_net_tcp_receive(int32_t fd, void* buf, uint64_t max_len) {
    if (fd < 0 || !buf || max_len == 0) return 0;
    size_t fetch_len = max_len > 1 ? (size_t)(max_len - 1) : (size_t)max_len;
    ssize_t recvd = recv(fd, (char*)buf, fetch_len, 0);
    if (recvd <= 0) {
        ((char*)buf)[0] = '\0';
        return 0;
    }
    ((char*)buf)[recvd] = '\0';
    return (int64_t)recvd;
}

int32_t loom_net_tcp_close(int32_t fd) {
    if (fd >= 0) {
#if !defined(_WIN32)
        return close(fd);
#else
        return closesocket(fd);
#endif
    }
    return 0;
}

int32_t loom_net_udp_bind(const char* host, uint16_t port) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!host || strlen(host) == 0 || strcmp(host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, host, &addr.sin_addr);
    }
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#if !defined(_WIN32)
        close(fd);
#else
        closesocket(fd);
#endif
        return -1;
    }
    return (int32_t)fd;
}

int64_t loom_net_udp_send_to(int32_t fd, const char* host, uint16_t port, const void* buf, uint64_t len) {
    if (fd < 0 || !buf) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    ssize_t sent = sendto(fd, (const char*)buf, (size_t)len, 0, (struct sockaddr*)&addr, sizeof(addr));
    return (int64_t)sent;
}

int64_t loom_net_udp_receive_from(int32_t fd, void* buf, uint64_t max_len) {
    if (fd < 0 || !buf || max_len == 0) return -1;
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    ssize_t recvd = recvfrom(fd, (char*)buf, (size_t)max_len, 0, (struct sockaddr*)&client_addr, &addr_len);
    return (int64_t)recvd;
}

int32_t loom_net_udp_close(int32_t fd) {
    if (fd >= 0) {
#if !defined(_WIN32)
        return close(fd);
#else
        return closesocket(fd);
#endif
    }
    return 0;
}
