#include "loom_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
   Global Runtime Abort Handler & Panic Engine
   --------------------------------------------------------------------------- */

void loom_runtime_abort(const char* message) {
    fprintf(stderr, "panic: %s\n", message ? message : "unspecified runtime error");
    fflush(stderr);
    exit(101);
}

void loom_panic(const char* file, int line, const char* reason) {
    fprintf(stderr, "panic: %s at %s:%d\n", reason ? reason : "unknown error", file ? file : "<unknown>", line);
    fflush(stderr);
    exit(101);
}

void loom_panic_overflow(const char* file, int line, const char* op, const char* type_name) {
    const char* kind = "overflow";
    if (op && (strcmp(op, "-") == 0 || strcmp(op, "subtraction") == 0)) {
        if (type_name && strstr(type_name, "Unsigned")) {
            kind = "underflow";
        }
    }
    const char* op_name = op ? op : "operation";
    if (strcmp(op_name, "+") == 0) op_name = "addition";
    else if (strcmp(op_name, "-") == 0) op_name = "subtraction";
    else if (strcmp(op_name, "*") == 0) op_name = "multiplication";
    else if (strcmp(op_name, "/") == 0) op_name = "division";

    fprintf(stderr, "panic: integer %s in %s %s at %s:%d\n",
            kind, type_name ? type_name : "integer", op_name,
            file ? file : "<unknown>", line);
    fflush(stderr);
    exit(101);
}

void loom_panic_bounds(const char* file, int line, int64_t index, int64_t length) {
    fprintf(stderr, "panic: out of bounds access at %s:%d (index %lld, length %lld)\n",
            file ? file : "<unknown>", line, (long long)index, (long long)length);
    fflush(stderr);
    exit(101);
}

void loom_panic_divzero(const char* file, int line) {
    fprintf(stderr, "panic: division by zero at %s:%d\n", file ? file : "<unknown>", line);
    fflush(stderr);
    exit(101);
}

/* ---------------------------------------------------------------------------
   Dynamic Memory Allocation Implementation
   --------------------------------------------------------------------------- */

void* loom_alloc(uint64_t count, uint32_t elem_size, uint32_t alignment) {
    if (elem_size == 0) elem_size = 1;
    if (alignment < sizeof(void*)) alignment = (uint32_t)sizeof(void*);

    /* Overflow check: count * elem_size */
    if (count > 0 && (UINT64_MAX / (uint64_t)elem_size) < count) {
        return NULL; /* Overflow detected */
    }

    uint64_t data_bytes = count * (uint64_t)elem_size;
    /* Ensure minimum allocation to hold a distinct address even if count == 0 */
    if (data_bytes == 0) data_bytes = 1;

    /* Header offset must be at least 32 bytes and a multiple of alignment */
    size_t offset = 32;
    if (alignment > offset) {
        offset = (size_t)alignment;
    }
    while (offset % alignment != 0) {
        offset += sizeof(void*);
    }

    if (data_bytes > (uint64_t)SIZE_MAX || (size_t)data_bytes > SIZE_MAX - offset) {
        return NULL;
    }
    size_t total_bytes = offset + (size_t)data_bytes;
    void* base = NULL;
    int res = posix_memalign(&base, alignment > sizeof(void*) ? alignment : sizeof(void*), total_bytes);
    if (res != 0 || !base) {
        return NULL;
    }

    void* user_ptr = (char*)base + offset;
    LoomAllocationHeader* hdr = (LoomAllocationHeader*)((char*)user_ptr - sizeof(LoomAllocationHeader));
    hdr->magic = LOOM_ALLOC_MAGIC;
    hdr->element_count = count;
    hdr->element_size = elem_size;
    hdr->header_offset = (uint32_t)offset;
    hdr->_reserved = 0;

    return user_ptr;
}

void* loom_realloc(void* ptr, uint64_t new_count, uint32_t elem_size, uint32_t alignment) {
    if (!ptr) {
        return loom_alloc(new_count, elem_size, alignment);
    }

    LoomAllocationHeader* hdr = (LoomAllocationHeader*)((char*)ptr - sizeof(LoomAllocationHeader));
    if (hdr->magic != LOOM_ALLOC_MAGIC) {
        fprintf(stderr, "Loom Runtime Trap: Invalid Resize on non-heap or corrupted pointer (%p)\n", ptr);
        abort();
    }

    void* new_ptr = loom_alloc(new_count, elem_size, alignment);
    if (!new_ptr) {
        return NULL;
    }

    uint64_t old_count = hdr->element_count;
    uint64_t copy_count = (old_count < new_count) ? old_count : new_count;
    if (copy_count > 0) {
        memcpy(new_ptr, ptr, (size_t)(copy_count * (uint64_t)elem_size));
    }

    loom_free(ptr);
    return new_ptr;
}

void* loom_ptr_clone(const void* ptr) {
    if (!ptr) return NULL;
    const LoomAllocationHeader* hdr = (const LoomAllocationHeader*)((const char*)ptr - sizeof(LoomAllocationHeader));
    if (hdr->magic != LOOM_ALLOC_MAGIC) {
        return NULL;
    }
    void* new_ptr = loom_alloc(hdr->element_count, hdr->element_size, 8);
    if (new_ptr && hdr->element_count > 0) {
        memcpy(new_ptr, ptr, (size_t)(hdr->element_count * (uint64_t)hdr->element_size));
    }
    return new_ptr;
}

void loom_free(void* ptr) {
    if (!ptr) {
        return; /* Null is safely a no-op */
    }

    LoomAllocationHeader* hdr = (LoomAllocationHeader*)((char*)ptr - sizeof(LoomAllocationHeader));
    if (hdr->magic != LOOM_ALLOC_MAGIC) {
        fprintf(stderr, "Loom Runtime Trap: Invalid Release on pointer (%p) not allocated by Allocate, or double-free\n", ptr);
        abort();
    }

    /* Clear magic to prevent double release */
    hdr->magic = 0;
    void* base = (char*)ptr - hdr->header_offset;
    free(base);
}

/* ---------------------------------------------------------------------------
   File I/O Implementation
   --------------------------------------------------------------------------- */

void* loom_file_open(const char* path, int mode, int* err) {
    if (!path) {
        if (err) *err = 1;
        return NULL;
    }
    const char* cmode = "rb";
    if (mode == 2) cmode = "wb";
    else if (mode == 3) cmode = "ab";
    else if (mode == 4) cmode = "r+b";
    else if (mode == 5) cmode = "w+b";

    FILE* f = fopen(path, cmode);
    if (!f) {
        if (err) *err = 2; /* OpenFailed */
        return NULL;
    }
    if (err) *err = 0;
    return f;
}

uint64_t loom_file_read(void* handle, void* buf, uint64_t max_bytes, int* err) {
    if (!handle || !buf) {
        if (err) *err = 1;
        return 0;
    }
    size_t n = fread(buf, 1, (size_t)max_bytes, (FILE*)handle);
    if (err) *err = 0;
    return (uint64_t)n;
}

char* loom_file_read_all(const char* path, int* err) {
    if (!path) {
        if (err) *err = 1;
        char* empty = (char*)calloc(1, 1);
        return empty ? empty : "";
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        if (err) *err = 2;
        char* empty = (char*)calloc(1, 1);
        return empty ? empty : "";
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        if (err) *err = 3;
        char* empty = (char*)calloc(1, 1);
        return empty ? empty : "";
    }

    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        if (err) *err = 4;
        char* empty = (char*)calloc(1, 1);
        return empty ? empty : "";
    }

    size_t read_bytes = fread(buf, 1, (size_t)size, f);
    buf[read_bytes] = '\0';
    fclose(f);
    if (err) *err = 0;
    return buf;
}

uint64_t loom_file_write(void* handle, const void* data, uint64_t len, int* err) {
    if (!handle || !data) {
        if (err) *err = 1;
        return 0;
    }
    size_t written = fwrite(data, 1, (size_t)len, (FILE*)handle);
    fflush((FILE*)handle);
    if (err) *err = 0;
    return (uint64_t)written;
}

void loom_file_write_str(void* handle, const char* str) {
    if (!handle || !str) return;
    fwrite(str, 1, strlen(str), (FILE*)handle);
    fflush((FILE*)handle);
}

void loom_file_write_i64(void* handle, int64_t val) {
    if (!handle) return;
    fprintf((FILE*)handle, "%lld", (long long)val);
    fflush((FILE*)handle);
}

void loom_file_write_u64(void* handle, uint64_t val) {
    if (!handle) return;
    fprintf((FILE*)handle, "%llu", (unsigned long long)val);
    fflush((FILE*)handle);
}

void loom_file_write_f64(void* handle, double val) {
    if (!handle) return;
    fprintf((FILE*)handle, "%g", val);
    fflush((FILE*)handle);
}

void loom_file_write_bool(void* handle, bool val) {
    if (!handle) return;
    fputs(val ? "True" : "False", (FILE*)handle);
    fflush((FILE*)handle);
}

void loom_file_write_char(void* handle, char c) {
    if (!handle) return;
    fputc(c, (FILE*)handle);
    fflush((FILE*)handle);
}

void loom_file_write_newline(void* handle) {
    if (!handle) return;
    fputc('\n', (FILE*)handle);
    fflush((FILE*)handle);
}

int loom_file_close(void* handle) {
    if (!handle) return 0;
    return fclose((FILE*)handle);
}

/* ---------------------------------------------------------------------------
   Math Helpers
   --------------------------------------------------------------------------- */

int64_t loom_ipow_i64(int64_t base, int64_t exp) {
    if (exp < 0) loom_runtime_abort("integer exponent must be nonnegative");
    uint64_t base_bits = (uint64_t)base;
    uint64_t result = 1;
    while (exp > 0) {
        if (exp & 1) result *= base_bits;
        base_bits *= base_bits;
        exp >>= 1;
    }
    int64_t signed_result;
    memcpy(&signed_result, &result, sizeof(signed_result));
    return signed_result;
}

uint64_t loom_ipow_u64(uint64_t base, uint64_t exp) {
    uint64_t result = 1;
    while (exp > 0) {
        if (exp & 1) result *= base;
        base *= base;
        exp >>= 1;
    }
    return result;
}
