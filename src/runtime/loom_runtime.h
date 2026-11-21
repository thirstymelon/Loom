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
void        loom_text_free(LoomText* t);

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
   Native Sockets / Networking API (Loom Native)
   --------------------------------------------------------------------------- */
int32_t  loom_net_tcp_listen(const char* host, uint16_t port);
int32_t  loom_net_tcp_accept(int32_t listener_fd);
int32_t  loom_net_tcp_connect(const char* host, uint16_t port);
int64_t  loom_net_tcp_send(int32_t fd, const void* buf, uint64_t len);
int64_t  loom_net_tcp_receive(int32_t fd, void* buf, uint64_t max_len);
int32_t  loom_net_tcp_close(int32_t fd);

int32_t  loom_net_udp_bind(const char* host, uint16_t port);
int64_t  loom_net_udp_send_to(int32_t fd, const char* host, uint16_t port, const void* buf, uint64_t len);
int64_t  loom_net_udp_receive_from(int32_t fd, void* buf, uint64_t max_len);
int32_t  loom_net_udp_close(int32_t fd);

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

#ifdef __cplusplus
}
#endif

#endif /* LOOM_RUNTIME_H */
