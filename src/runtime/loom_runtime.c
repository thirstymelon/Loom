#include "loom_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

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
        /* On reallocation failure, the original allocation must remain untouched */
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
   Task Concurrency Runtime Implementation
   --------------------------------------------------------------------------- */

#define LOOM_TASK_MAGIC 0x4C4F4F4D5441534BULL /* "LOOMTASK" */

struct LoomTask {
    uint64_t magic;
    _Atomic(uint32_t) state;
    LoomTaskFn entry;
    void* args;
    size_t args_size;
    void* result;
    size_t result_size;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct LoomTask* next;
};

#define TASK_QUEUE_CAPACITY 1024

typedef struct {
    LoomTask* queue_head;
    LoomTask* queue_tail;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    bool initialized;
    bool shutdown;
    int worker_count;
    pthread_t workers[32];
} LoomThreadPool;

static LoomThreadPool g_pool = {
    .queue_head = NULL,
    .queue_tail = NULL,
    .queue_mutex = PTHREAD_MUTEX_INITIALIZER,
    .queue_cond = PTHREAD_COND_INITIALIZER,
    .initialized = false,
    .shutdown = false,
    .worker_count = 0
};

static void* loom_worker_thread(void* arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&g_pool.queue_mutex);
        while (!g_pool.queue_head && !g_pool.shutdown) {
            pthread_cond_wait(&g_pool.queue_cond, &g_pool.queue_mutex);
        }

        if (g_pool.shutdown && !g_pool.queue_head) {
            pthread_mutex_unlock(&g_pool.queue_mutex);
            break;
        }

        LoomTask* task = g_pool.queue_head;
        if (task) {
            g_pool.queue_head = task->next;
            if (!g_pool.queue_head) g_pool.queue_tail = NULL;
            task->next = NULL;
        }
        pthread_mutex_unlock(&g_pool.queue_mutex);

        if (task) {
            pthread_mutex_lock(&task->lock);
            if (task->state == LOOM_TASK_CANCELLED) {
                pthread_cond_broadcast(&task->cond);
                pthread_mutex_unlock(&task->lock);
                continue;
            }
            task->state = LOOM_TASK_RUNNING;
            pthread_mutex_unlock(&task->lock);

            /* Execute target task procedure */
            task->entry(task->args, task->result);

            pthread_mutex_lock(&task->lock);
            if (task->state != LOOM_TASK_CANCELLED) {
                task->state = LOOM_TASK_COMPLETED;
            }
            pthread_cond_broadcast(&task->cond);
            pthread_mutex_unlock(&task->lock);
        }
    }
    return NULL;
}

static void loom_init_thread_pool(void) {
    if (g_pool.initialized) return;

    pthread_mutex_lock(&g_pool.queue_mutex);
    if (g_pool.initialized) {
        pthread_mutex_unlock(&g_pool.queue_mutex);
        return;
    }

    int ncpus = 4;
#if defined(_SC_NPROCESSORS_ONLN)
    long sc = sysconf(_SC_NPROCESSORS_ONLN);
    if (sc > 0) ncpus = (int)sc;
#endif
    if (ncpus > 16) ncpus = 16;
    if (ncpus < 2)  ncpus = 2;

    g_pool.worker_count = ncpus;
    for (int i = 0; i < ncpus; i++) {
        pthread_create(&g_pool.workers[i], NULL, loom_worker_thread, NULL);
    }

    g_pool.initialized = true;
    pthread_mutex_unlock(&g_pool.queue_mutex);
}

LoomTask* loom_task_spawn(LoomTaskFn entry, void* args, size_t args_size, size_t result_size) {
    loom_init_thread_pool();

    LoomTask* task = (LoomTask*)malloc(sizeof(LoomTask));
    if (!task) return NULL;

    task->magic = LOOM_TASK_MAGIC;
    task->state = LOOM_TASK_PENDING;
    task->entry = entry;
    task->args_size = args_size;
    task->result_size = result_size;
    task->next = NULL;

    if (args_size > 0 && args) {
        task->args = malloc(args_size);
        if (task->args) memcpy(task->args, args, args_size);
    } else {
        task->args = NULL;
    }

    if (result_size > 0) {
        task->result = malloc(result_size);
        if (task->result) memset(task->result, 0, result_size);
    } else {
        task->result = NULL;
    }

    pthread_mutex_init(&task->lock, NULL);
    pthread_cond_init(&task->cond, NULL);

    /* Enqueue task */
    pthread_mutex_lock(&g_pool.queue_mutex);
    if (g_pool.queue_tail) {
        g_pool.queue_tail->next = task;
        g_pool.queue_tail = task;
    } else {
        g_pool.queue_head = task;
        g_pool.queue_tail = task;
    }
    pthread_cond_signal(&g_pool.queue_cond);
    pthread_mutex_unlock(&g_pool.queue_mutex);

    return task;
}

void loom_task_join(LoomTask* task, void* result_out) {
    if (!task) return;

    if (task->magic != LOOM_TASK_MAGIC) {
        fprintf(stderr, "Loom Runtime Trap: Invalid or already-joined Task_Handle (%p)\n", (void*)task);
        abort();
    }

    pthread_mutex_lock(&task->lock);
    if (task->state == LOOM_TASK_JOINED) {
        pthread_mutex_unlock(&task->lock);
        fprintf(stderr, "Loom Runtime Trap: Task already joined\n");
        abort();
    }

    while (task->state != LOOM_TASK_COMPLETED && task->state != LOOM_TASK_FAILED && task->state != LOOM_TASK_CANCELLED) {
        pthread_cond_wait(&task->cond, &task->lock);
    }

    if (result_out && task->result && task->result_size > 0) {
        memcpy(result_out, task->result, task->result_size);
    }

    task->state = LOOM_TASK_JOINED;
    task->magic = 0; /* Invalidate magic to prevent double join */
    pthread_mutex_unlock(&task->lock);

    /* Free task allocations */
    if (task->args) free(task->args);
    if (task->result) free(task->result);
    pthread_mutex_destroy(&task->lock);
    pthread_cond_destroy(&task->cond);
    free(task);
}

void loom_task_cancel(LoomTask* task) {
    if (!task || task->magic != LOOM_TASK_MAGIC) return;
    pthread_mutex_lock(&task->lock);
    if (task->state != LOOM_TASK_COMPLETED && task->state != LOOM_TASK_JOINED) {
        task->state = LOOM_TASK_CANCELLED;
        pthread_cond_broadcast(&task->cond);
    }
    pthread_mutex_unlock(&task->lock);
}

int loom_task_join_timeout(LoomTask* task, void* result_out, uint64_t timeout_ms) {
    if (!task || task->magic != LOOM_TASK_MAGIC) return 2; /* Error */

    pthread_mutex_lock(&task->lock);
    if (task->state == LOOM_TASK_JOINED) {
        pthread_mutex_unlock(&task->lock);
        return 2;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(timeout_ms / 1000);
    ts.tv_nsec += (long)((timeout_ms % 1000) * 1000000ULL);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    int wait_res = 0;
    while (task->state != LOOM_TASK_COMPLETED && task->state != LOOM_TASK_FAILED && task->state != LOOM_TASK_CANCELLED) {
        wait_res = pthread_cond_timedwait(&task->cond, &task->lock, &ts);
        if (wait_res != 0) {
            break; /* Timeout */
        }
    }

    if (task->state == LOOM_TASK_COMPLETED) {
        if (result_out && task->result && task->result_size > 0) {
            memcpy(result_out, task->result, task->result_size);
        }
        task->state = LOOM_TASK_JOINED;
        task->magic = 0;
        pthread_mutex_unlock(&task->lock);

        if (task->args) free(task->args);
        if (task->result) free(task->result);
        pthread_mutex_destroy(&task->lock);
        pthread_cond_destroy(&task->cond);
        free(task);
        return 0; /* Success */
    }

    if (task->state == LOOM_TASK_CANCELLED || task->state == LOOM_TASK_FAILED) {
        task->state = LOOM_TASK_JOINED;
        task->magic = 0;
        pthread_mutex_unlock(&task->lock);

        if (task->args) free(task->args);
        if (task->result) free(task->result);
        pthread_mutex_destroy(&task->lock);
        pthread_cond_destroy(&task->cond);
        free(task);
        return 2; /* Task failed / cancelled */
    }

    pthread_mutex_unlock(&task->lock);
    return 1; /* Timed out */
}

/* ---------------------------------------------------------------------------
   Dynamic Strings / Text Implementation
   --------------------------------------------------------------------------- */

void loom_text_init(LoomText* t, uint64_t init_cap) {
    if (!t) return;
    if (init_cap == 0) init_cap = 64;
    t->data = (char*)malloc(init_cap);
    if (t->data) {
        t->data[0] = '\0';
        t->length = 0;
        t->capacity = init_cap;
    } else {
        t->data = NULL;
        t->length = 0;
        t->capacity = 0;
    }
}

static void loom_text_ensure_cap(LoomText* t, uint64_t needed) {
    if (!t) return;
    if (!t->data || t->capacity == 0) {
        loom_text_init(t, needed > 64 ? needed : 64);
        return;
    }
    if (needed <= t->capacity) return;

    uint64_t new_cap = t->capacity * 2;
    if (new_cap < needed) new_cap = needed + 64;

    char* new_data = (char*)realloc(t->data, new_cap);
    if (new_data) {
        t->data = new_data;
        t->capacity = new_cap;
    }
}

void loom_text_append_str(LoomText* t, const char* str) {
    if (!t || !str) return;
    uint64_t slen = (uint64_t)strlen(str);
    loom_text_ensure_cap(t, t->length + slen + 1);
    if (t->data) {
        memcpy(t->data + t->length, str, slen);
        t->length += slen;
        t->data[t->length] = '\0';
    }
}

void loom_text_append_char(LoomText* t, char c) {
    if (!t) return;
    loom_text_ensure_cap(t, t->length + 2);
    if (t->data) {
        t->data[t->length++] = c;
        t->data[t->length] = '\0';
    }
}

void loom_text_append_i64(LoomText* t, int64_t val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)val);
    loom_text_append_str(t, buf);
}

void loom_text_append_u64(LoomText* t, uint64_t val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)val);
    loom_text_append_str(t, buf);
}

void loom_text_append_f64(LoomText* t, double val) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", val);
    loom_text_append_str(t, buf);
}

void loom_text_append_bool(LoomText* t, bool val) {
    loom_text_append_str(t, val ? "True" : "False");
}

const char* loom_text_to_string(LoomText* t) {
    if (!t || !t->data) return "";
    return t->data;
}

char* loom_text_subslice(LoomText* t, int64_t start, int64_t end) {
    if (!t || !t->data) {
        char* empty = (char*)malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    if (start < 1) start = 1;
    if (end > (int64_t)t->length) end = (int64_t)t->length;
    if (start > end) {
        char* empty = (char*)malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    int64_t len = end - start + 1;
    char* res = (char*)malloc((size_t)len + 1);
    if (!res) return NULL;
    memcpy(res, t->data + (start - 1), (size_t)len);
    res[len] = '\0';
    return res;
}

void loom_text_free(LoomText* t) {
    if (!t) return;
    if (t->data) free(t->data);
    t->data = NULL;
    t->length = 0;
    t->capacity = 0;
}

/* ---------------------------------------------------------------------------
   Dynamic Vector Implementation
   --------------------------------------------------------------------------- */

void loom_vec_init(LoomVector* v, uint64_t init_elem_count, uint32_t elem_size, uint32_t elem_align) {
    if (!v) return;
    if (elem_size == 0) elem_size = 1;
    if (elem_align == 0) elem_align = 8;
    uint64_t alloc_cap = init_elem_count > 0 ? init_elem_count : 8;

    v->elem_size = elem_size;
    v->elem_align = elem_align;
    v->capacity = alloc_cap;
    v->length = init_elem_count;
    v->data = loom_alloc(alloc_cap, elem_size, elem_align);
    if (v->data && init_elem_count > 0) {
        memset(v->data, 0, init_elem_count * (size_t)elem_size);
    }
}

void* loom_vec_grow(LoomVector* v) {
    if (!v) return NULL;
    uint64_t new_cap = v->capacity > 0 ? v->capacity * 2 : 16;
    void* new_data = loom_realloc(v->data, new_cap, v->elem_size, v->elem_align);
    if (new_data) {
        v->data = new_data;
        v->capacity = new_cap;
    }
    return v->data;
}

void* loom_vec_get(LoomVector* v, uint64_t one_based_idx) {
    if (!v || !v->data || one_based_idx < 1 || one_based_idx > v->length) {
        fprintf(stderr, "Loom Runtime Trap: Vector index %llu out of bounds (length %llu)\n",
                (unsigned long long)one_based_idx, (unsigned long long)(v ? v->length : 0));
        abort();
    }
    return (char*)v->data + (one_based_idx - 1) * (uint64_t)v->elem_size;
}

void loom_vec_append(LoomVector* v, const void* elem_val) {
    if (!v || !elem_val) return;
    if (v->length >= v->capacity) {
        loom_vec_grow(v);
    }
    memcpy((char*)v->data + v->length * (size_t)v->elem_size, elem_val, v->elem_size);
    v->length++;
}

void loom_vec_pop(LoomVector* v, void* out_val) {
    if (!v || v->length == 0) {
        fprintf(stderr, "Loom Runtime Trap: Vector underflow in Pop()\n");
        abort();
    }
    v->length--;
    if (out_val) {
        memcpy(out_val, (char*)v->data + v->length * (size_t)v->elem_size, v->elem_size);
    }
}

uint64_t loom_vec_index(LoomVector* v, const void* elem_val) {
    if (!v || !v->data || !elem_val || v->length == 0) return 0;

    if (v->elem_size == 4) {
        uint32_t target;
        memcpy(&target, elem_val, 4);
        const uint32_t* p = (const uint32_t*)v->data;
        for (uint64_t i = 0; i < v->length; ++i) {
            if (p[i] == target) return i + 1;
        }
        return 0;
    }
    if (v->elem_size == 8) {
        uint64_t target;
        memcpy(&target, elem_val, 8);
        const uint64_t* p = (const uint64_t*)v->data;
        for (uint64_t i = 0; i < v->length; ++i) {
            if (p[i] == target) return i + 1;
        }
        return 0;
    }

    for (uint64_t i = 0; i < v->length; ++i) {
        if (memcmp((const char*)v->data + i * (size_t)v->elem_size, elem_val, v->elem_size) == 0) {
            return i + 1;
        }
    }
    return 0;
}

void loom_vec_index_all(LoomVector* out_vec, LoomVector* v, const void* elem_val) {
    if (!out_vec) return;
    loom_vec_init(out_vec, 0, sizeof(uint64_t), sizeof(uint64_t));
    if (!v || !v->data || !elem_val || v->length == 0) return;

    if (v->elem_size == 4) {
        uint32_t target;
        memcpy(&target, elem_val, 4);
        const uint32_t* p = (const uint32_t*)v->data;
        for (uint64_t i = 0; i < v->length; ++i) {
            if (p[i] == target) {
                uint64_t idx = i + 1;
                loom_vec_append(out_vec, &idx);
            }
        }
        return;
    }
    if (v->elem_size == 8) {
        uint64_t target;
        memcpy(&target, elem_val, 8);
        const uint64_t* p = (const uint64_t*)v->data;
        for (uint64_t i = 0; i < v->length; ++i) {
            if (p[i] == target) {
                uint64_t idx = i + 1;
                loom_vec_append(out_vec, &idx);
            }
        }
        return;
    }

    for (uint64_t i = 0; i < v->length; ++i) {
        if (memcmp((const char*)v->data + i * (size_t)v->elem_size, elem_val, v->elem_size) == 0) {
            uint64_t idx = i + 1;
            loom_vec_append(out_vec, &idx);
        }
    }
}

int loom_vec_contains(LoomVector* v, const void* elem_val) {
    return loom_vec_index(v, elem_val) != 0 ? 1 : 0;
}

void loom_vec_free(LoomVector* v) {
    if (!v) return;
    if (v->data) loom_free(v->data);
    v->data = NULL;
    v->length = 0;
    v->capacity = 0;
}

/* ---------------------------------------------------------------------------
   Dynamic Hash Map Implementation (Loom Native)
   --------------------------------------------------------------------------- */

#define LOOM_HMAP_EMPTY     0
#define LOOM_HMAP_OCCUPIED  1
#define LOOM_HMAP_DELETED   2

typedef struct {
    uint8_t state;
    union {
        int64_t i64;
        char*   str;
    } key;
    void* value;
} LoomHMapEntry;

struct LoomHMap {
    uint32_t key_kind; /* 0 = i64, 1 = str */
    uint32_t val_size;
    uint32_t val_align;
    uint64_t length;
    uint64_t capacity;
    LoomHMapEntry* entries;
};

static uint64_t loom_hash_i64(int64_t x) {
    uint64_t z = (uint64_t)x;
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static uint64_t loom_hash_str(const char* s) {
    uint64_t hash = UINT64_C(14695981039346656037);
    if (!s) return hash;
    while (*s) {
        hash ^= (uint8_t)(*s++);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

LoomHMap* loom_hmap_create(uint32_t key_kind, uint32_t val_size, uint32_t val_align) {
    LoomHMap* m = (LoomHMap*)calloc(1, sizeof(LoomHMap));
    if (!m) return NULL;
    m->key_kind = key_kind;
    m->val_size = val_size ? val_size : 8;
    m->val_align = val_align ? val_align : 8;
    m->capacity = 16;
    m->length = 0;
    m->entries = (LoomHMapEntry*)calloc(m->capacity, sizeof(LoomHMapEntry));
    return m;
}

static void loom_hmap_resize(LoomHMap* m, uint64_t new_cap) {
    if (new_cap < 16) new_cap = 16;
    LoomHMapEntry* old_entries = m->entries;
    uint64_t old_cap = m->capacity;

    m->entries = (LoomHMapEntry*)calloc(new_cap, sizeof(LoomHMapEntry));
    m->capacity = new_cap;
    m->length = 0;

    for (uint64_t i = 0; i < old_cap; ++i) {
        if (old_entries[i].state == LOOM_HMAP_OCCUPIED) {
            if (m->key_kind == 0) {
                loom_hmap_insert_i64(m, old_entries[i].key.i64, old_entries[i].value);
            } else {
                loom_hmap_insert_str(m, old_entries[i].key.str, old_entries[i].value);
                free(old_entries[i].key.str);
            }
            free(old_entries[i].value);
        }
    }
    free(old_entries);
}

void loom_hmap_insert_i64(LoomHMap* m, int64_t key, const void* val) {
    if (!m) return;
    if ((m->length + 1) * 10 >= m->capacity * 7) {
        loom_hmap_resize(m, m->capacity * 2);
    }
    uint64_t h = loom_hash_i64(key);
    uint64_t mask = m->capacity - 1;
    uint64_t idx = h & mask;
    int64_t first_deleted = -1;

    for (uint64_t i = 0; i < m->capacity; ++i) {
        idx = (h + i) & mask;
        if (m->entries[idx].state == LOOM_HMAP_OCCUPIED) {
            if (m->entries[idx].key.i64 == key) {
                memcpy(m->entries[idx].value, val, m->val_size);
                return;
            }
        } else if (m->entries[idx].state == LOOM_HMAP_DELETED) {
            if (first_deleted < 0) first_deleted = (int64_t)idx;
        } else {
            break;
        }
    }

    uint64_t target = (first_deleted >= 0) ? (uint64_t)first_deleted : idx;
    m->entries[target].state = LOOM_HMAP_OCCUPIED;
    m->entries[target].key.i64 = key;
    if (!m->entries[target].value) {
        m->entries[target].value = malloc(m->val_size);
    }
    memcpy(m->entries[target].value, val, m->val_size);
    m->length++;
}

void loom_hmap_insert_str(LoomHMap* m, const char* key, const void* val) {
    if (!m || !key) return;
    if ((m->length + 1) * 10 >= m->capacity * 7) {
        loom_hmap_resize(m, m->capacity * 2);
    }
    uint64_t h = loom_hash_str(key);
    uint64_t mask = m->capacity - 1;
    uint64_t idx = h & mask;
    int64_t first_deleted = -1;

    for (uint64_t i = 0; i < m->capacity; ++i) {
        idx = (h + i) & mask;
        if (m->entries[idx].state == LOOM_HMAP_OCCUPIED) {
            if (strcmp(m->entries[idx].key.str, key) == 0) {
                memcpy(m->entries[idx].value, val, m->val_size);
                return;
            }
        } else if (m->entries[idx].state == LOOM_HMAP_DELETED) {
            if (first_deleted < 0) first_deleted = (int64_t)idx;
        } else {
            break;
        }
    }

    uint64_t target = (first_deleted >= 0) ? (uint64_t)first_deleted : idx;
    m->entries[target].state = LOOM_HMAP_OCCUPIED;
    m->entries[target].key.str = strdup(key);
    if (!m->entries[target].value) {
        m->entries[target].value = malloc(m->val_size);
    }
    memcpy(m->entries[target].value, val, m->val_size);
    m->length++;
}

void* loom_hmap_get_i64(LoomHMap* m, int64_t key) {
    if (!m || m->capacity == 0) return NULL;
    uint64_t h = loom_hash_i64(key);
    uint64_t mask = m->capacity - 1;
    for (uint64_t i = 0; i < m->capacity; ++i) {
        uint64_t idx = (h + i) & mask;
        if (m->entries[idx].state == LOOM_HMAP_OCCUPIED) {
            if (m->entries[idx].key.i64 == key) return m->entries[idx].value;
        } else if (m->entries[idx].state == LOOM_HMAP_EMPTY) {
            return NULL;
        }
    }
    return NULL;
}

void* loom_hmap_get_str(LoomHMap* m, const char* key) {
    if (!m || !key || m->capacity == 0) return NULL;
    uint64_t h = loom_hash_str(key);
    uint64_t mask = m->capacity - 1;
    for (uint64_t i = 0; i < m->capacity; ++i) {
        uint64_t idx = (h + i) & mask;
        if (m->entries[idx].state == LOOM_HMAP_OCCUPIED) {
            if (strcmp(m->entries[idx].key.str, key) == 0) return m->entries[idx].value;
        } else if (m->entries[idx].state == LOOM_HMAP_EMPTY) {
            return NULL;
        }
    }
    return NULL;
}

int loom_hmap_contains_i64(LoomHMap* m, int64_t key) {
    return loom_hmap_get_i64(m, key) != NULL ? 1 : 0;
}

int loom_hmap_contains_str(LoomHMap* m, const char* key) {
    return loom_hmap_get_str(m, key) != NULL ? 1 : 0;
}

int loom_hmap_remove_i64(LoomHMap* m, int64_t key) {
    if (!m || m->capacity == 0) return 0;
    uint64_t h = loom_hash_i64(key);
    uint64_t mask = m->capacity - 1;
    for (uint64_t i = 0; i < m->capacity; ++i) {
        uint64_t idx = (h + i) & mask;
        if (m->entries[idx].state == LOOM_HMAP_OCCUPIED) {
            if (m->entries[idx].key.i64 == key) {
                m->entries[idx].state = LOOM_HMAP_DELETED;
                m->length--;
                return 1;
            }
        } else if (m->entries[idx].state == LOOM_HMAP_EMPTY) {
            return 0;
        }
    }
    return 0;
}

int loom_hmap_remove_str(LoomHMap* m, const char* key) {
    if (!m || !key || m->capacity == 0) return 0;
    uint64_t h = loom_hash_str(key);
    uint64_t mask = m->capacity - 1;
    for (uint64_t i = 0; i < m->capacity; ++i) {
        uint64_t idx = (h + i) & mask;
        if (m->entries[idx].state == LOOM_HMAP_OCCUPIED) {
            if (strcmp(m->entries[idx].key.str, key) == 0) {
                m->entries[idx].state = LOOM_HMAP_DELETED;
                free(m->entries[idx].key.str);
                m->entries[idx].key.str = NULL;
                m->length--;
                return 1;
            }
        } else if (m->entries[idx].state == LOOM_HMAP_EMPTY) {
            return 0;
        }
    }
    return 0;
}

uint64_t loom_hmap_length(LoomHMap* m) {
    return m ? m->length : 0;
}

void loom_hmap_free(LoomHMap* m) {
    if (!m) return;
    if (m->entries) {
        for (uint64_t i = 0; i < m->capacity; ++i) {
            if (m->entries[i].state == LOOM_HMAP_OCCUPIED) {
                if (m->key_kind == 1 && m->entries[i].key.str) {
                    free(m->entries[i].key.str);
                }
            }
            if (m->entries[i].value) {
                free(m->entries[i].value);
            }
        }
        free(m->entries);
    }
    free(m);
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
        return NULL;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        if (err) *err = 2;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        if (err) *err = 3;
        return NULL;
    }

    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        if (err) *err = 4;
        return NULL;
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
    if (exp < 0) return 0;
    int64_t result = 1;
    while (exp > 0) {
        if (exp & 1) result *= base;
        base *= base;
        exp >>= 1;
    }
    return result;
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
