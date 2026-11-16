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
void        loom_text_free(LoomText* t);

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
uint64_t loom_vec_index(LoomVector* v, const void* elem_val);
void     loom_vec_index_all(LoomVector* out_vec, LoomVector* v, const void* elem_val);
int      loom_vec_contains(LoomVector* v, const void* elem_val);
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
   Math Helpers
   --------------------------------------------------------------------------- */
int64_t  loom_ipow_i64(int64_t base, int64_t exp);
uint64_t loom_ipow_u64(uint64_t base, uint64_t exp);

#ifdef __cplusplus
}
#endif

#endif /* LOOM_RUNTIME_H */
