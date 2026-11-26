#ifndef LOOM_RUNTIME_H
#define LOOM_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOOM_ALLOC_MAGIC 0x4C4F4F4D414C4C43ULL /* "LOOMALLC" */

typedef struct {
    uint64_t magic;         /* LOOM_ALLOC_MAGIC verification */
    uint64_t element_count; /* Number of elements allocated */
    uint32_t element_size;  /* Byte size of each element */
    uint32_t header_offset; /* Byte offset from base memory to user pointer */
    uint64_t _reserved;     /* Alignment padding */
} LoomAllocationHeader;

/* ---------------------------------------------------------------------------
   Dynamic Memory Allocation API (Loom Native)
   --------------------------------------------------------------------------- */
void* loom_alloc(uint64_t count, uint32_t elem_size, uint32_t alignment);
void* loom_realloc(void* ptr, uint64_t new_count, uint32_t elem_size, uint32_t alignment);
void* loom_ptr_clone(const void* ptr);
void  loom_free(void* ptr);

/* Bump arena: single-threaded; reset invalidates every pointer it returned. */
typedef struct LoomArena LoomArena;
LoomArena* loom_arena_create(uint64_t capacity);
void*      loom_arena_alloc(LoomArena* arena, uint64_t size, uint64_t alignment);
void       loom_arena_reset(LoomArena* arena);
void       loom_arena_destroy(LoomArena* arena);

/* ---------------------------------------------------------------------------
   Task Concurrency API (Loom Native)
   --------------------------------------------------------------------------- */
typedef enum {
    LOOM_TASK_PENDING   = 0,
    LOOM_TASK_RUNNING   = 1,
    LOOM_TASK_COMPLETED = 2,
    LOOM_TASK_FAILED    = 3,
    LOOM_TASK_JOINED    = 4,
    LOOM_TASK_CANCELLED = 5
} LoomTaskState;

typedef void (*LoomTaskFn)(void* args, void* result);

typedef struct LoomTask LoomTask;

LoomTask* loom_task_spawn(LoomTaskFn entry, void* args, size_t args_size, size_t result_size);
void      loom_task_join(LoomTask* task, void* result_out);
void      loom_task_cancel(LoomTask* task);
int       loom_task_join_timeout(LoomTask* task, void* result_out, uint64_t timeout_ms);
typedef void (*LoomParallelFn)(void* context, uint64_t index);
typedef void (*LoomParallelChunkFn)(void* context, uint64_t begin, uint64_t end);
void      loom_parallel_for(uint64_t count, uint64_t minimum_grain, LoomParallelFn fn, void* context);
void      loom_parallel_for_chunks(uint64_t count, uint64_t minimum_grain, LoomParallelChunkFn fn, void* context);
void      loom_vec_f32_apply(uint32_t operation, const float* left, const float* right,
                             float* output, float factor, uint64_t count);

/* ---------------------------------------------------------------------------
   Dynamic Strings / Text API (Loom Native)
   --------------------------------------------------------------------------- */
typedef struct {
    char* data;
    uint64_t length;
    uint64_t capacity;
} LoomText;

void        loom_text_init(LoomText* t, uint64_t init_cap);
void        loom_text_append_str(LoomText* t, const char* str);
void        loom_text_append_buf(LoomText* t, const char* data, uint64_t len);
void        loom_text_append_char(LoomText* t, char c);
void        loom_text_append_i64(LoomText* t, int64_t val);
void        loom_text_append_u64(LoomText* t, uint64_t val);
void        loom_text_append_f64(LoomText* t, double val);
void        loom_text_append_bool(LoomText* t, bool val);
const char* loom_text_to_string(LoomText* t);
char*       loom_text_subslice(LoomText* t, int64_t start, int64_t end);
int         loom_text_contains(LoomText* t, const char* sub);
uint64_t    loom_text_index(LoomText* t, const char* sub);
void        loom_text_clear(LoomText* t);
void        loom_text_clone(LoomText* dest, const LoomText* src);
int         loom_text_equals(const LoomText* a, const LoomText* b);
int         loom_string_equals(const char* a, const char* b);
void        loom_text_free(LoomText* t);
uint32_t    loom_utf8_decode_at(const char* data, uint64_t length, uint64_t offset);
uint32_t    loom_utf8_width_at(const char* data, uint64_t length, uint64_t offset);

/* ---------------------------------------------------------------------------
   Cryptographic & Non-Cryptographic Hashing API (Loom Native)
   --------------------------------------------------------------------------- */
typedef enum {
    LOOM_HASH_MD5      = 1,
    LOOM_HASH_SHA1     = 2,
    LOOM_HASH_SHA224   = 3,
    LOOM_HASH_SHA256   = 4,
    LOOM_HASH_SHA384   = 5,
    LOOM_HASH_SHA512   = 6,
    LOOM_HASH_SHA3_256 = 7,
    LOOM_HASH_SHA3_512 = 8,
    LOOM_HASH_BLAKE3   = 9,
    LOOM_HASH_FNV1A    = 10
} LoomHashAlgo;

typedef struct LoomHasher LoomHasher;

void        loom_hash_md5(const char* data, LoomText* out);
void        loom_hash_sha1(const char* data, LoomText* out);
void        loom_hash_sha224(const char* data, LoomText* out);
void        loom_hash_sha256(const char* data, LoomText* out);
void        loom_hash_sha384(const char* data, LoomText* out);
void        loom_hash_sha512(const char* data, LoomText* out);
void        loom_hash_sha3_256(const char* data, LoomText* out);
void        loom_hash_sha3_512(const char* data, LoomText* out);
void        loom_hash_blake3(const char* data, LoomText* out);
uint64_t    loom_hash_fnv1a(const char* data);

void        loom_hmac_sha256(const char* key, const char* msg, LoomText* out);
void        loom_hmac_sha512(const char* key, const char* msg, LoomText* out);
void        loom_hmac_md5(const char* key, const char* msg, LoomText* out);
void        loom_hmac_sha1(const char* key, const char* msg, LoomText* out);

int         loom_hash_file(int algo, const char* path, LoomText* out);

LoomHasher* loom_hasher_create(int algo);
void        loom_hasher_update(LoomHasher* h, const char* data);
void        loom_hasher_final(LoomHasher* h, LoomText* out);
void        loom_hasher_free(LoomHasher* h);

/* ---------------------------------------------------------------------------
   Dynamic Vector API (Loom Native)
   --------------------------------------------------------------------------- */
typedef struct {
    void* data;
    uint64_t length;
    uint64_t capacity;
    uint32_t elem_size;
    uint32_t elem_align;
} LoomVector;

void     loom_vec_init(LoomVector* v, uint64_t init_elem_count, uint32_t elem_size, uint32_t elem_align);
void*    loom_vec_grow(LoomVector* v);
void*    loom_vec_get(LoomVector* v, uint64_t one_based_idx);
void     loom_vec_append(LoomVector* v, const void* elem_val);
void     loom_vec_pop(LoomVector* v, void* out_val);
uint64_t loom_buf_index(const void* data, uint64_t length, uint32_t elem_size, const void* elem_val);
int      loom_buf_contains(const void* data, uint64_t length, uint32_t elem_size, const void* elem_val);
void     loom_buf_index_all(LoomVector* out_vec, const void* data, uint64_t length, uint32_t elem_size, const void* elem_val);

uint64_t loom_vec_index(LoomVector* v, const void* elem_val);
void     loom_vec_index_all(LoomVector* out_vec, LoomVector* v, const void* elem_val);
int      loom_vec_contains(LoomVector* v, const void* elem_val);
void     loom_vec_clear(LoomVector* v);
void     loom_vec_clone(LoomVector* dest, const LoomVector* src);
void     loom_vec_free(LoomVector* v);

/* ---------------------------------------------------------------------------
   Dynamic Hash Map API (Loom Native)
   --------------------------------------------------------------------------- */
typedef struct LoomHMap LoomHMap;

LoomHMap* loom_hmap_create(uint32_t key_kind, uint32_t val_size, uint32_t val_align);
void      loom_hmap_insert_i64(LoomHMap* m, int64_t key, const void* val);
void      loom_hmap_insert_str(LoomHMap* m, const char* key, const void* val);
void*     loom_hmap_get_i64(LoomHMap* m, int64_t key);
void*     loom_hmap_get_str(LoomHMap* m, const char* key);
int       loom_hmap_contains_i64(LoomHMap* m, int64_t key);
int       loom_hmap_contains_str(LoomHMap* m, const char* key);
int       loom_hmap_remove_i64(LoomHMap* m, int64_t key);
int       loom_hmap_remove_str(LoomHMap* m, const char* key);
uint64_t  loom_hmap_length(LoomHMap* m);
int       loom_hmap_iter_next(LoomHMap* m, uint64_t* bucket_idx, void** out_key, void** out_val);
void      loom_hmap_clear(LoomHMap* m);
void      loom_hmap_free(LoomHMap* m);

/* ---------------------------------------------------------------------------
   File I/O API (Loom Native)
   --------------------------------------------------------------------------- */
void*    loom_file_open(const char* path, int mode, int* err);
uint64_t loom_file_read(void* handle, void* buf, uint64_t max_bytes, int* err);
char*    loom_file_read_all(const char* path, int* err);
uint64_t loom_file_write(void* handle, const void* data, uint64_t len, int* err);
void     loom_file_write_str(void* handle, const char* str);
void     loom_file_write_i64(void* handle, int64_t val);
void     loom_file_write_u64(void* handle, uint64_t val);
void     loom_file_write_f64(void* handle, double val);
void     loom_file_write_bool(void* handle, bool val);
void     loom_file_write_char(void* handle, char c);
void     loom_file_write_newline(void* handle);
int      loom_file_close(void* handle);

/* ---------------------------------------------------------------------------
   Smart Pointer (Smt_Pointer[T]) API (Loom Native)
   --------------------------------------------------------------------------- */
typedef void (*LoomDestructorFn)(void*);

typedef struct {
    uint64_t ref_count;
    LoomDestructorFn destructor;
} LoomSmtControlBlock;

void*    loom_smt_alloc(uint64_t payload_size, LoomDestructorFn dtor);
void*    loom_smt_retain(void* payload_ptr);
void     loom_smt_release(void* payload_ptr);
uint64_t loom_smt_count(const void* payload_ptr);

/* ---------------------------------------------------------------------------
   Mutex API (Loom Native)
   --------------------------------------------------------------------------- */
typedef struct LoomMutex LoomMutex;

LoomMutex* loom_mutex_create(void);
void       loom_mutex_lock(LoomMutex* m);
int        loom_mutex_try_lock(LoomMutex* m);
void       loom_mutex_unlock(LoomMutex* m);
void       loom_mutex_free(LoomMutex* m);

/* ---------------------------------------------------------------------------
   Channel[T] Concurrency API (Loom Native)
   --------------------------------------------------------------------------- */
typedef struct LoomChannel LoomChannel;

LoomChannel* loom_channel_create(uint64_t capacity, uint32_t elem_size);
void         loom_channel_send(LoomChannel* ch, const void* elem_val);
int          loom_channel_try_send(LoomChannel* ch, const void* elem_val);
void         loom_channel_receive(LoomChannel* ch, void* out_val);
int          loom_channel_try_receive(LoomChannel* ch, void* out_val);
void         loom_channel_close(LoomChannel* ch);
void         loom_channel_free(LoomChannel* ch);

/* ---------------------------------------------------------------------------
   Math Helpers
   --------------------------------------------------------------------------- */
int64_t  loom_ipow_i64(int64_t base, int64_t exp);
uint64_t loom_ipow_u64(uint64_t base, uint64_t exp);
void     loom_runtime_abort(const char* message);

/* ---------------------------------------------------------------------------
   Panic & Safety Diagnostics API (Loom Native)
   --------------------------------------------------------------------------- */
void loom_panic(const char* file, int line, const char* reason);
void loom_panic_overflow(const char* file, int line, const char* op, const char* type_name);
void loom_panic_bounds(const char* file, int line, int64_t index, int64_t length);
void loom_panic_divzero(const char* file, int line);

/* ---------------------------------------------------------------------------
   TLS / Secure Transport API (Loom Native)
   --------------------------------------------------------------------------- */
int64_t loom_tls_client_new(const char* host, const char* ca_path);
int32_t loom_tls_connect(int64_t handle, int32_t fd);
int64_t loom_tls_send(int64_t handle, const char* data, int64_t len);
int64_t loom_tls_receive(int64_t handle, char* buf, int64_t capacity);
int32_t loom_tls_close(int64_t handle);

#ifdef __cplusplus
}
#endif

#endif /* LOOM_RUNTIME_H */
