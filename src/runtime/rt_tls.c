#include "loom_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/socket.h>

/* Dynamic OpenSSL function pointers */
typedef const void* (*fn_TLS_client_method)(void);
typedef void* (*fn_SSL_CTX_new)(const void*);
typedef void (*fn_SSL_CTX_free)(void*);
typedef int (*fn_SSL_CTX_set_default_verify_paths)(void*);
typedef int (*fn_SSL_CTX_load_verify_locations)(void*, const char*, const char*);
typedef void* (*fn_SSL_new)(void*);
typedef void (*fn_SSL_free)(void*);
typedef int (*fn_SSL_set_fd)(void*, int);
typedef int (*fn_SSL_set1_host)(void*, const char*);
typedef long (*fn_SSL_ctrl)(void*, int, long, void*);
typedef int (*fn_SSL_connect)(void*);
typedef int (*fn_SSL_write)(void*, const void*, int);
typedef int (*fn_SSL_read)(void*, void*, int);
typedef int (*fn_SSL_shutdown)(void*);
typedef int (*fn_SSL_set_alpn_protos)(void*, const unsigned char*, unsigned int);
typedef void (*fn_SSL_get0_alpn_selected)(const void*, const unsigned char**, unsigned int*);

typedef void* (*fn_EVP_CIPHER_CTX_new)(void);
typedef void (*fn_EVP_CIPHER_CTX_free)(void*);
typedef const void* (*fn_EVP_aes_128_gcm)(void);
typedef const void* (*fn_EVP_aes_128_ecb)(void);
typedef const void* (*fn_EVP_sha256)(void);
typedef int (*fn_EVP_EncryptInit_ex)(void*, const void*, void*, const unsigned char*, const unsigned char*);
typedef int (*fn_EVP_EncryptUpdate)(void*, unsigned char*, int*, const unsigned char*, int);
typedef int (*fn_EVP_EncryptFinal_ex)(void*, unsigned char*, int*);
typedef int (*fn_EVP_DecryptInit_ex)(void*, const void*, void*, const unsigned char*, const unsigned char*);
typedef int (*fn_EVP_DecryptUpdate)(void*, unsigned char*, int*, const unsigned char*, int);
typedef int (*fn_EVP_DecryptFinal_ex)(void*, unsigned char*, int*);
typedef int (*fn_EVP_CIPHER_CTX_ctrl)(void*, int, int, void*);
typedef unsigned char* (*fn_HMAC)(const void*, const void*, int, const unsigned char*, size_t, unsigned char*, unsigned int*);

typedef struct {
    void* ssl_lib;
    void* crypto_lib;
    fn_TLS_client_method TLS_client_method;
    fn_SSL_CTX_new SSL_CTX_new;
    fn_SSL_CTX_free SSL_CTX_free;
    fn_SSL_CTX_set_default_verify_paths SSL_CTX_set_default_verify_paths;
    fn_SSL_CTX_load_verify_locations SSL_CTX_load_verify_locations;
    fn_SSL_new SSL_new;
    fn_SSL_free SSL_free;
    fn_SSL_set_fd SSL_set_fd;
    fn_SSL_set1_host SSL_set1_host;
    fn_SSL_ctrl SSL_ctrl;
    fn_SSL_connect SSL_connect;
    fn_SSL_write SSL_write;
    fn_SSL_read SSL_read;
    fn_SSL_shutdown SSL_shutdown;
    fn_SSL_set_alpn_protos SSL_set_alpn_protos;
    fn_SSL_get0_alpn_selected SSL_get0_alpn_selected;

    fn_EVP_CIPHER_CTX_new EVP_CIPHER_CTX_new;
    fn_EVP_CIPHER_CTX_free EVP_CIPHER_CTX_free;
    fn_EVP_aes_128_gcm EVP_aes_128_gcm;
    fn_EVP_aes_128_ecb EVP_aes_128_ecb;
    fn_EVP_sha256 EVP_sha256;
    fn_EVP_EncryptInit_ex EVP_EncryptInit_ex;
    fn_EVP_EncryptUpdate EVP_EncryptUpdate;
    fn_EVP_EncryptFinal_ex EVP_EncryptFinal_ex;
    fn_EVP_DecryptInit_ex EVP_DecryptInit_ex;
    fn_EVP_DecryptUpdate EVP_DecryptUpdate;
    fn_EVP_DecryptFinal_ex EVP_DecryptFinal_ex;
    fn_EVP_CIPHER_CTX_ctrl EVP_CIPHER_CTX_ctrl;
    fn_HMAC HMAC;

    int initialized;
} OpenSSLDispatch;

static OpenSSLDispatch g_openssl = {0};

static int init_openssl(void) {
    if (g_openssl.initialized) return g_openssl.ssl_lib != NULL;

    const char* ssl_paths[] = {
        "/opt/homebrew/lib/libssl.dylib",
        "/usr/local/lib/libssl.dylib",
        "libssl.dylib",
        "libssl.so.3",
        "libssl.so",
        NULL
    };
    const char* crypto_paths[] = {
        "/opt/homebrew/lib/libcrypto.dylib",
        "/usr/local/lib/libcrypto.dylib",
        "libcrypto.dylib",
        "libcrypto.so.3",
        "libcrypto.so",
        NULL
    };

    void* ssl = NULL;
    for (int i = 0; ssl_paths[i]; ++i) {
        ssl = dlopen(ssl_paths[i], RTLD_LAZY | RTLD_GLOBAL);
        if (ssl) break;
    }
    void* crypto = NULL;
    for (int i = 0; crypto_paths[i]; ++i) {
        crypto = dlopen(crypto_paths[i], RTLD_LAZY | RTLD_GLOBAL);
        if (crypto) break;
    }

    if (!ssl) {
        g_openssl.initialized = 1;
        return 0;
    }

    g_openssl.ssl_lib = ssl;
    g_openssl.crypto_lib = crypto;
    g_openssl.TLS_client_method = (fn_TLS_client_method)dlsym(ssl, "TLS_client_method");
    g_openssl.SSL_CTX_new = (fn_SSL_CTX_new)dlsym(ssl, "SSL_CTX_new");
    g_openssl.SSL_CTX_free = (fn_SSL_CTX_free)dlsym(ssl, "SSL_CTX_free");
    g_openssl.SSL_CTX_set_default_verify_paths = (fn_SSL_CTX_set_default_verify_paths)dlsym(ssl, "SSL_CTX_set_default_verify_paths");
    g_openssl.SSL_CTX_load_verify_locations = (fn_SSL_CTX_load_verify_locations)dlsym(ssl, "SSL_CTX_load_verify_locations");
    g_openssl.SSL_new = (fn_SSL_new)dlsym(ssl, "SSL_new");
    g_openssl.SSL_free = (fn_SSL_free)dlsym(ssl, "SSL_free");
    g_openssl.SSL_set_fd = (fn_SSL_set_fd)dlsym(ssl, "SSL_set_fd");
    g_openssl.SSL_set1_host = (fn_SSL_set1_host)dlsym(ssl, "SSL_set1_host");
    g_openssl.SSL_ctrl = (fn_SSL_ctrl)dlsym(ssl, "SSL_ctrl");
    g_openssl.SSL_connect = (fn_SSL_connect)dlsym(ssl, "SSL_connect");
    g_openssl.SSL_write = (fn_SSL_write)dlsym(ssl, "SSL_write");
    g_openssl.SSL_read = (fn_SSL_read)dlsym(ssl, "SSL_read");
    g_openssl.SSL_shutdown = (fn_SSL_shutdown)dlsym(ssl, "SSL_shutdown");
    g_openssl.SSL_set_alpn_protos = (fn_SSL_set_alpn_protos)dlsym(ssl, "SSL_set_alpn_protos");
    g_openssl.SSL_get0_alpn_selected = (fn_SSL_get0_alpn_selected)dlsym(ssl, "SSL_get0_alpn_selected");

    if (crypto) {
        g_openssl.EVP_CIPHER_CTX_new = (fn_EVP_CIPHER_CTX_new)dlsym(crypto, "EVP_CIPHER_CTX_new");
        g_openssl.EVP_CIPHER_CTX_free = (fn_EVP_CIPHER_CTX_free)dlsym(crypto, "EVP_CIPHER_CTX_free");
        g_openssl.EVP_aes_128_gcm = (fn_EVP_aes_128_gcm)dlsym(crypto, "EVP_aes_128_gcm");
        g_openssl.EVP_aes_128_ecb = (fn_EVP_aes_128_ecb)dlsym(crypto, "EVP_aes_128_ecb");
        g_openssl.EVP_sha256 = (fn_EVP_sha256)dlsym(crypto, "EVP_sha256");
        g_openssl.EVP_EncryptInit_ex = (fn_EVP_EncryptInit_ex)dlsym(crypto, "EVP_EncryptInit_ex");
        g_openssl.EVP_EncryptUpdate = (fn_EVP_EncryptUpdate)dlsym(crypto, "EVP_EncryptUpdate");
        g_openssl.EVP_EncryptFinal_ex = (fn_EVP_EncryptFinal_ex)dlsym(crypto, "EVP_EncryptFinal_ex");
        g_openssl.EVP_DecryptInit_ex = (fn_EVP_DecryptInit_ex)dlsym(crypto, "EVP_DecryptInit_ex");
        g_openssl.EVP_DecryptUpdate = (fn_EVP_DecryptUpdate)dlsym(crypto, "EVP_DecryptUpdate");
        g_openssl.EVP_DecryptFinal_ex = (fn_EVP_DecryptFinal_ex)dlsym(crypto, "EVP_DecryptFinal_ex");
        g_openssl.EVP_CIPHER_CTX_ctrl = (fn_EVP_CIPHER_CTX_ctrl)dlsym(crypto, "EVP_CIPHER_CTX_ctrl");
        g_openssl.HMAC = (fn_HMAC)dlsym(crypto, "HMAC");
    }

    g_openssl.initialized = 1;
    return g_openssl.TLS_client_method && g_openssl.SSL_CTX_new && g_openssl.SSL_new;
}

typedef struct {
    void* ctx;
    void* ssl;
    int fd;
    char* host;
} LoomTlsSession;

int64_t loom_tls_client_new(const char* host, const char* ca_path) {
    if (!init_openssl()) {
        return -1;
    }

    void* ctx = g_openssl.SSL_CTX_new(g_openssl.TLS_client_method());
    if (!ctx) return -1;

    if (ca_path && *ca_path) {
        if (g_openssl.SSL_CTX_load_verify_locations) {
            g_openssl.SSL_CTX_load_verify_locations(ctx, ca_path, NULL);
        }
    } else {
        if (g_openssl.SSL_CTX_set_default_verify_paths) {
            g_openssl.SSL_CTX_set_default_verify_paths(ctx);
        }
    }

    void* ssl = g_openssl.SSL_new(ctx);
    if (!ssl) {
        g_openssl.SSL_CTX_free(ctx);
        return -1;
    }

    if (host && *host) {
        if (g_openssl.SSL_set1_host) {
            g_openssl.SSL_set1_host(ssl, host);
        }
        if (g_openssl.SSL_ctrl) {
            /* SSL_CTRL_SET_TLSEXT_HOSTNAME = 55, TLSEXT_NAMETYPE_host_name = 0 */
            g_openssl.SSL_ctrl(ssl, 55, 0, (void*)host);
        }
    }

    LoomTlsSession* sess = (LoomTlsSession*)malloc(sizeof(LoomTlsSession));
    if (!sess) {
        g_openssl.SSL_free(ssl);
        g_openssl.SSL_CTX_free(ctx);
        return -1;
    }

    sess->ctx = ctx;
    sess->ssl = ssl;
    sess->fd = -1;
    sess->host = host ? strdup(host) : NULL;

    return (int64_t)(intptr_t)sess;
}

int32_t loom_tls_connect(int64_t handle, int32_t fd) {
    if (handle <= 0) return -1;
    LoomTlsSession* sess = (LoomTlsSession*)(intptr_t)handle;
    sess->fd = fd;

    if (!g_openssl.SSL_set_fd(sess->ssl, fd)) {
        return -1;
    }

    int res = g_openssl.SSL_connect(sess->ssl);
    return res == 1 ? 0 : -1;
}

int64_t loom_tls_send(int64_t handle, const char* data, int64_t len) {
    if (handle <= 0 || !data || len < 0) return -1;
    LoomTlsSession* sess = (LoomTlsSession*)(intptr_t)handle;
    int written = g_openssl.SSL_write(sess->ssl, data, (int)len);
    return (int64_t)written;
}

int64_t loom_tls_receive(int64_t handle, char* buf, int64_t capacity) {
    if (handle <= 0 || !buf || capacity <= 0) return -1;
    LoomTlsSession* sess = (LoomTlsSession*)(intptr_t)handle;
    int read_bytes = g_openssl.SSL_read(sess->ssl, buf, (int)capacity);
    return (int64_t)read_bytes;
}

int32_t loom_tls_close(int64_t handle) {
    if (handle <= 0) return -1;
    LoomTlsSession* sess = (LoomTlsSession*)(intptr_t)handle;
    if (sess->ssl) {
        if (g_openssl.SSL_shutdown) g_openssl.SSL_shutdown(sess->ssl);
        if (g_openssl.SSL_free) g_openssl.SSL_free(sess->ssl);
    }
    if (sess->ctx && g_openssl.SSL_CTX_free) {
        g_openssl.SSL_CTX_free(sess->ctx);
    }
    if (sess->host) free(sess->host);
    free(sess);
    return 0;
}

int32_t loom_tls_set_alpn(int64_t handle, const char* alpn_str) {
    if (handle <= 0 || !alpn_str || !init_openssl() || !g_openssl.SSL_set_alpn_protos) return -1;
    LoomTlsSession* sess = (LoomTlsSession*)(intptr_t)handle;
    unsigned char wire[256];
    size_t wire_len = 0;
    const char* p = alpn_str;
    while (*p && wire_len < 250) {
        const char* next = strchr(p, ',');
        size_t len = next ? (size_t)(next - p) : strlen(p);
        if (len > 0 && len <= 255 && wire_len + 1 + len <= sizeof(wire)) {
            wire[wire_len++] = (unsigned char)len;
            memcpy(&wire[wire_len], p, len);
            wire_len += len;
        }
        if (!next) break;
        p = next + 1;
    }
    if (wire_len == 0) return -1;
    return g_openssl.SSL_set_alpn_protos(sess->ssl, wire, (unsigned int)wire_len) == 0 ? 0 : -1;
}

int64_t loom_tls_get_alpn(int64_t handle, char* out_buf, int64_t capacity) {
    if (handle <= 0 || !out_buf || capacity <= 0 || !init_openssl() || !g_openssl.SSL_get0_alpn_selected) return -1;
    LoomTlsSession* sess = (LoomTlsSession*)(intptr_t)handle;
    const unsigned char* proto = NULL;
    unsigned int proto_len = 0;
    g_openssl.SSL_get0_alpn_selected(sess->ssl, &proto, &proto_len);
    if (!proto || proto_len == 0) {
        out_buf[0] = '\0';
        return 0;
    }
    size_t copy_len = proto_len < (size_t)capacity - 1 ? proto_len : (size_t)capacity - 1;
    memcpy(out_buf, proto, copy_len);
    out_buf[copy_len] = '\0';
    return (int64_t)copy_len;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_to_bytes(const char* hex, unsigned char* out, size_t max_out) {
    if (!hex) return 0;
    size_t len = strlen(hex);
    if (len % 2 != 0 || len / 2 > max_out) return 0;
    for (size_t i = 0; i < len / 2; ++i) {
        int hi = hex_val(hex[2 * i]);
        int lo = hex_val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return (int)(len / 2);
}

static void bytes_to_hex(const unsigned char* in, size_t len, char* out) {
    static const char hex_digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[2 * i] = hex_digits[(in[i] >> 4) & 0x0f];
        out[2 * i + 1] = hex_digits[in[i] & 0x0f];
    }
    out[2 * len] = '\0';
}

static void hkdf_extract(const unsigned char* salt, size_t salt_len, const unsigned char* ikm, size_t ikm_len, unsigned char* prk) {
    unsigned int out_len = 32;
    g_openssl.HMAC(g_openssl.EVP_sha256(), salt, (int)salt_len, ikm, ikm_len, prk, &out_len);
}

static void hkdf_expand_label(const unsigned char* secret, const char* label, size_t out_len, unsigned char* out) {
    unsigned char info[64];
    info[0] = (unsigned char)((out_len >> 8) & 0xff);
    info[1] = (unsigned char)(out_len & 0xff);
    size_t label_len = strlen(label);
    size_t full_label_len = 6 + label_len;
    info[2] = (unsigned char)full_label_len;
    memcpy(&info[3], "tls13 ", 6);
    memcpy(&info[9], label, label_len);
    info[9 + label_len] = 0x00;
    size_t info_len = 10 + label_len;

    unsigned char t[64];
    unsigned char block[128];
    memcpy(block, info, info_len);
    block[info_len] = 0x01;
    unsigned int hmac_len = 32;
    g_openssl.HMAC(g_openssl.EVP_sha256(), secret, 32, block, info_len + 1, t, &hmac_len);
    memcpy(out, t, out_len);
}

static const unsigned char g_quic_v1_salt[20] = {
    0x38, 0x7b, 0x77, 0x11, 0xe7, 0x69, 0x67, 0x07, 0x9a, 0x02,
    0x69, 0xf5, 0x4f, 0x0f, 0x00, 0xf0, 0xe2, 0x77, 0x4e, 0xeb
};

int32_t loom_quic_initial_keys(const char* dcid_hex, char* out_buf, int64_t capacity) {
    if (!init_openssl() || !g_openssl.HMAC || !g_openssl.EVP_sha256) return -1;
    unsigned char dcid[32];
    int dcid_len = hex_to_bytes(dcid_hex, dcid, sizeof(dcid));
    if (dcid_len <= 0) return -1;

    unsigned char initial_secret[32];
    hkdf_extract(g_quic_v1_salt, 20, dcid, (size_t)dcid_len, initial_secret);

    unsigned char client_secret[32];
    hkdf_expand_label(initial_secret, "client in", 32, client_secret);

    unsigned char client_key[16];
    unsigned char client_iv[12];
    unsigned char client_hp[16];
    hkdf_expand_label(client_secret, "quic key", 16, client_key);
    hkdf_expand_label(client_secret, "quic iv", 12, client_iv);
    hkdf_expand_label(client_secret, "quic hp", 16, client_hp);

    char key_hex[33], iv_hex[25], hp_hex[33];
    bytes_to_hex(client_key, 16, key_hex);
    bytes_to_hex(client_iv, 12, iv_hex);
    bytes_to_hex(client_hp, 16, hp_hex);
    snprintf(out_buf, (size_t)capacity, "%s:%s:%s", key_hex, iv_hex, hp_hex);
    return 0;
}

int32_t loom_quic_hp(const char* hp_hex, const char* sample_hex, char* out_mask_hex) {
    if (!init_openssl() || !g_openssl.EVP_CIPHER_CTX_new || !g_openssl.EVP_aes_128_ecb) return -1;
    unsigned char hp[16];
    unsigned char sample[16];
    if (hex_to_bytes(hp_hex, hp, 16) != 16 || hex_to_bytes(sample_hex, sample, 16) != 16) return -1;

    void* ctx = g_openssl.EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    g_openssl.EVP_EncryptInit_ex(ctx, g_openssl.EVP_aes_128_ecb(), NULL, hp, NULL);
    unsigned char mask[16];
    int out_len = 0;
    g_openssl.EVP_EncryptUpdate(ctx, mask, &out_len, sample, 16);
    g_openssl.EVP_CIPHER_CTX_free(ctx);

    bytes_to_hex(mask, 5, out_mask_hex);
    return 0;
}

int32_t loom_quic_encrypt(const char* key_hex, const char* iv_hex, uint64_t pkt_num, const char* ad_hex, const char* plain_hex, char* out_hex, int64_t max_out) {
    if (!init_openssl() || !g_openssl.EVP_CIPHER_CTX_new || !g_openssl.EVP_aes_128_gcm) return -1;
    unsigned char key[16], iv[12], nonce[12];
    if (hex_to_bytes(key_hex, key, 16) != 16 || hex_to_bytes(iv_hex, iv, 12) != 12) return -1;

    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; ++i) {
        nonce[11 - i] ^= (unsigned char)((pkt_num >> (i * 8)) & 0xff);
    }

    unsigned char ad[256];
    int ad_len = hex_to_bytes(ad_hex, ad, sizeof(ad));

    unsigned char plain[4096];
    int plain_len = hex_to_bytes(plain_hex, plain, sizeof(plain));

    void* ctx = g_openssl.EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    g_openssl.EVP_EncryptInit_ex(ctx, g_openssl.EVP_aes_128_gcm(), NULL, NULL, NULL);
    g_openssl.EVP_CIPHER_CTX_ctrl(ctx, 0x9, 12, NULL);
    g_openssl.EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce);

    int len = 0;
    if (ad_len > 0) {
        g_openssl.EVP_EncryptUpdate(ctx, NULL, &len, ad, ad_len);
    }
    unsigned char cipher[4096 + 16];
    int cipher_len = 0;
    if (plain_len > 0) {
        g_openssl.EVP_EncryptUpdate(ctx, cipher, &cipher_len, plain, plain_len);
    }
    int final_len = 0;
    g_openssl.EVP_EncryptFinal_ex(ctx, cipher + cipher_len, &final_len);
    cipher_len += final_len;

    unsigned char tag[16];
    g_openssl.EVP_CIPHER_CTX_ctrl(ctx, 0x10, 16, tag);
    g_openssl.EVP_CIPHER_CTX_free(ctx);

    memcpy(cipher + cipher_len, tag, 16);
    cipher_len += 16;

    if ((size_t)max_out < (size_t)(cipher_len * 2 + 1)) return -1;
    bytes_to_hex(cipher, (size_t)cipher_len, out_hex);
    return 0;
}

int32_t loom_quic_decrypt(const char* key_hex, const char* iv_hex, uint64_t pkt_num, const char* ad_hex, const char* cipher_hex, char* out_hex, int64_t max_out) {
    if (!init_openssl() || !g_openssl.EVP_CIPHER_CTX_new || !g_openssl.EVP_aes_128_gcm) return -1;
    unsigned char key[16], iv[12], nonce[12];
    if (hex_to_bytes(key_hex, key, 16) != 16 || hex_to_bytes(iv_hex, iv, 12) != 12) return -1;

    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; ++i) {
        nonce[11 - i] ^= (unsigned char)((pkt_num >> (i * 8)) & 0xff);
    }

    unsigned char ad[256];
    int ad_len = hex_to_bytes(ad_hex, ad, sizeof(ad));

    unsigned char cipher_and_tag[4096];
    int ct_len = hex_to_bytes(cipher_hex, cipher_and_tag, sizeof(cipher_and_tag));
    if (ct_len < 16) return -1;
    int cipher_len = ct_len - 16;
    unsigned char* tag = cipher_and_tag + cipher_len;

    void* ctx = g_openssl.EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    g_openssl.EVP_DecryptInit_ex(ctx, g_openssl.EVP_aes_128_gcm(), NULL, NULL, NULL);
    g_openssl.EVP_CIPHER_CTX_ctrl(ctx, 0x9, 12, NULL);
    g_openssl.EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce);

    int len = 0;
    if (ad_len > 0) {
        g_openssl.EVP_DecryptUpdate(ctx, NULL, &len, ad, ad_len);
    }
    unsigned char plain[4096];
    int plain_len = 0;
    if (cipher_len > 0) {
        g_openssl.EVP_DecryptUpdate(ctx, plain, &plain_len, cipher_and_tag, cipher_len);
    }
    g_openssl.EVP_CIPHER_CTX_ctrl(ctx, 0x11, 16, tag);
    int final_res = g_openssl.EVP_DecryptFinal_ex(ctx, plain + plain_len, &len);
    g_openssl.EVP_CIPHER_CTX_free(ctx);

    if (final_res <= 0) return -1;
    plain_len += len;

    if ((size_t)max_out < (size_t)(plain_len * 2 + 1)) return -1;
    bytes_to_hex(plain, (size_t)plain_len, out_hex);
    return 0;
}
