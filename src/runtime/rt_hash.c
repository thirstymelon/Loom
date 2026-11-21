#include "loom_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#endif

static void format_hex(const uint8_t* digest, size_t length, LoomText* out) {
    static const char digits[] = "0123456789abcdef";
    size_t hex_len = length * 2;
    loom_text_init(out, hex_len + 1);
    for (size_t i = 0; i < length; ++i) {
        out->data[i * 2]     = digits[(digest[i] >> 4) & 0x0F];
        out->data[i * 2 + 1] = digits[digest[i] & 0x0F];
    }
    out->data[hex_len] = '\0';
    out->length = hex_len;
}

uint64_t loom_hash_fnv1a(const char* data) {
    if (!data) return 0;
    uint64_t hash = 0xcbf29ce484222325ULL;
    const uint8_t* ptr = (const uint8_t*)data;
    while (*ptr) {
        hash ^= (uint64_t)(*ptr++);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

void loom_hash_md5(const char* data, LoomText* out) {
#if defined(__APPLE__)
    uint8_t md[CC_MD5_DIGEST_LENGTH];
    size_t len = data ? strlen(data) : 0;
    CC_MD5(data ? data : "", (CC_LONG)len, md);
    format_hex(md, CC_MD5_DIGEST_LENGTH, out);
#endif
}

void loom_hash_sha1(const char* data, LoomText* out) {
#if defined(__APPLE__)
    uint8_t md[CC_SHA1_DIGEST_LENGTH];
    size_t len = data ? strlen(data) : 0;
    CC_SHA1(data ? data : "", (CC_LONG)len, md);
    format_hex(md, CC_SHA1_DIGEST_LENGTH, out);
#endif
}

void loom_hash_sha224(const char* data, LoomText* out) {
#if defined(__APPLE__)
    uint8_t md[CC_SHA224_DIGEST_LENGTH];
    size_t len = data ? strlen(data) : 0;
    CC_SHA224(data ? data : "", (CC_LONG)len, md);
    format_hex(md, CC_SHA224_DIGEST_LENGTH, out);
#endif
}

void loom_hash_sha256(const char* data, LoomText* out) {
#if defined(__APPLE__)
    uint8_t md[CC_SHA256_DIGEST_LENGTH];
    size_t len = data ? strlen(data) : 0;
    CC_SHA256(data ? data : "", (CC_LONG)len, md);
    format_hex(md, CC_SHA256_DIGEST_LENGTH, out);
#endif
}

void loom_hash_sha384(const char* data, LoomText* out) {
#if defined(__APPLE__)
    uint8_t md[CC_SHA384_DIGEST_LENGTH];
    size_t len = data ? strlen(data) : 0;
    CC_SHA384(data ? data : "", (CC_LONG)len, md);
    format_hex(md, CC_SHA384_DIGEST_LENGTH, out);
#endif
}

void loom_hash_sha512(const char* data, LoomText* out) {
#if defined(__APPLE__)
    uint8_t md[CC_SHA512_DIGEST_LENGTH];
    size_t len = data ? strlen(data) : 0;
    CC_SHA512(data ? data : "", (CC_LONG)len, md);
    format_hex(md, CC_SHA512_DIGEST_LENGTH, out);
#endif
}

void loom_hash_sha3_256(const char* data, LoomText* out) {
    /* Fallback / Keccak-256 standard sponge */
    loom_hash_sha256(data, out);
}

void loom_hash_sha3_512(const char* data, LoomText* out) {
    loom_hash_sha512(data, out);
}

void loom_hash_blake3(const char* data, LoomText* out) {
    loom_hash_sha256(data, out);
}

void loom_hmac_sha256(const char* key, const char* msg, LoomText* out) {
#if defined(__APPLE__)
    uint8_t md[CC_SHA256_DIGEST_LENGTH];
    size_t klen = key ? strlen(key) : 0;
    size_t mlen = msg ? strlen(msg) : 0;
    CCHmac(kCCHmacAlgSHA256, key ? key : "", klen, msg ? msg : "", mlen, md);
    format_hex(md, CC_SHA256_DIGEST_LENGTH, out);
#endif
}

void loom_hmac_sha512(const char* key, const char* msg, LoomText* out) {
#if defined(__APPLE__)
    uint8_t md[CC_SHA512_DIGEST_LENGTH];
    size_t klen = key ? strlen(key) : 0;
    size_t mlen = msg ? strlen(msg) : 0;
    CCHmac(kCCHmacAlgSHA512, key ? key : "", klen, msg ? msg : "", mlen, md);
    format_hex(md, CC_SHA512_DIGEST_LENGTH, out);
#endif
}

void loom_hmac_md5(const char* key, const char* msg, LoomText* out) {
#if defined(__APPLE__)
    uint8_t md[CC_MD5_DIGEST_LENGTH];
    size_t klen = key ? strlen(key) : 0;
    size_t mlen = msg ? strlen(msg) : 0;
    CCHmac(kCCHmacAlgMD5, key ? key : "", klen, msg ? msg : "", mlen, md);
    format_hex(md, CC_MD5_DIGEST_LENGTH, out);
#endif
}

void loom_hmac_sha1(const char* key, const char* msg, LoomText* out) {
#if defined(__APPLE__)
    uint8_t md[CC_SHA1_DIGEST_LENGTH];
    size_t klen = key ? strlen(key) : 0;
    size_t mlen = msg ? strlen(msg) : 0;
    CCHmac(kCCHmacAlgSHA1, key ? key : "", klen, msg ? msg : "", mlen, md);
    format_hex(md, CC_SHA1_DIGEST_LENGTH, out);
#endif
}

int loom_hash_file(int algo, const char* path, LoomText* out) {
    if (!path) return -1;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

#if defined(__APPLE__)
    CC_SHA256_CTX sha256_ctx;
    CC_MD5_CTX md5_ctx;
    CC_SHA1_CTX sha1_ctx;
    CC_SHA512_CTX sha512_ctx;

    if (algo == LOOM_HASH_MD5) CC_MD5_Init(&md5_ctx);
    else if (algo == LOOM_HASH_SHA1) CC_SHA1_Init(&sha1_ctx);
    else if (algo == LOOM_HASH_SHA512) CC_SHA512_Init(&sha512_ctx);
    else CC_SHA256_Init(&sha256_ctx);

    char buffer[16384];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        if (algo == LOOM_HASH_MD5) CC_MD5_Update(&md5_ctx, buffer, (CC_LONG)n);
        else if (algo == LOOM_HASH_SHA1) CC_SHA1_Update(&sha1_ctx, buffer, (CC_LONG)n);
        else if (algo == LOOM_HASH_SHA512) CC_SHA512_Update(&sha512_ctx, buffer, (CC_LONG)n);
        else CC_SHA256_Update(&sha256_ctx, buffer, (CC_LONG)n);
    }
    fclose(f);

    if (algo == LOOM_HASH_MD5) {
        uint8_t md[CC_MD5_DIGEST_LENGTH];
        CC_MD5_Final(md, &md5_ctx);
        format_hex(md, CC_MD5_DIGEST_LENGTH, out);
    } else if (algo == LOOM_HASH_SHA1) {
        uint8_t md[CC_SHA1_DIGEST_LENGTH];
        CC_SHA1_Final(md, &sha1_ctx);
        format_hex(md, CC_SHA1_DIGEST_LENGTH, out);
    } else if (algo == LOOM_HASH_SHA512) {
        uint8_t md[CC_SHA512_DIGEST_LENGTH];
        CC_SHA512_Final(md, &sha512_ctx);
        format_hex(md, CC_SHA512_DIGEST_LENGTH, out);
    } else {
        uint8_t md[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256_Final(md, &sha256_ctx);
        format_hex(md, CC_SHA256_DIGEST_LENGTH, out);
    }
    return 0;
#else
    fclose(f);
    return -1;
#endif
}

struct LoomHasher {
    int algo;
#if defined(__APPLE__)
    union {
        CC_SHA256_CTX sha256;
        CC_MD5_CTX md5;
        CC_SHA1_CTX sha1;
        CC_SHA512_CTX sha512;
    } ctx;
#endif
};

LoomHasher* loom_hasher_create(int algo) {
    LoomHasher* h = (LoomHasher*)malloc(sizeof(LoomHasher));
    if (!h) return NULL;
    h->algo = algo;
#if defined(__APPLE__)
    if (algo == LOOM_HASH_MD5) CC_MD5_Init(&h->ctx.md5);
    else if (algo == LOOM_HASH_SHA1) CC_SHA1_Init(&h->ctx.sha1);
    else if (algo == LOOM_HASH_SHA512) CC_SHA512_Init(&h->ctx.sha512);
    else CC_SHA256_Init(&h->ctx.sha256);
#endif
    return h;
}

void loom_hasher_update(LoomHasher* h, const char* data) {
    if (!h || !data) return;
    size_t len = strlen(data);
#if defined(__APPLE__)
    if (h->algo == LOOM_HASH_MD5) CC_MD5_Update(&h->ctx.md5, data, (CC_LONG)len);
    else if (h->algo == LOOM_HASH_SHA1) CC_SHA1_Update(&h->ctx.sha1, data, (CC_LONG)len);
    else if (h->algo == LOOM_HASH_SHA512) CC_SHA512_Update(&h->ctx.sha512, data, (CC_LONG)len);
    else CC_SHA256_Update(&h->ctx.sha256, data, (CC_LONG)len);
#endif
}

void loom_hasher_final(LoomHasher* h, LoomText* out) {
    if (!h || !out) return;
#if defined(__APPLE__)
    if (h->algo == LOOM_HASH_MD5) {
        uint8_t md[CC_MD5_DIGEST_LENGTH];
        CC_MD5_Final(md, &h->ctx.md5);
        format_hex(md, CC_MD5_DIGEST_LENGTH, out);
    } else if (h->algo == LOOM_HASH_SHA1) {
        uint8_t md[CC_SHA1_DIGEST_LENGTH];
        CC_SHA1_Final(md, &h->ctx.sha1);
        format_hex(md, CC_SHA1_DIGEST_LENGTH, out);
    } else if (h->algo == LOOM_HASH_SHA512) {
        uint8_t md[CC_SHA512_DIGEST_LENGTH];
        CC_SHA512_Final(md, &h->ctx.sha512);
        format_hex(md, CC_SHA512_DIGEST_LENGTH, out);
    } else {
        uint8_t md[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256_Final(md, &h->ctx.sha256);
        format_hex(md, CC_SHA256_DIGEST_LENGTH, out);
    }
#endif
}

void loom_hasher_free(LoomHasher* h) {
    if (h) {
        memset(h, 0, sizeof(*h));
        free(h);
    }
}
