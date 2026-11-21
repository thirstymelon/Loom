#include "loom_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
   Dynamic Strings / Text Implementation
   --------------------------------------------------------------------------- */

void loom_text_init(LoomText* t, uint64_t init_cap) {
    if (!t) return;
    if (init_cap == 0) init_cap = 64;
    if (init_cap > (uint64_t)SIZE_MAX) loom_runtime_abort("Text capacity exceeds addressable memory");
    t->data = (char*)malloc(init_cap);
    if (t->data) {
        t->data[0] = '\0';
        t->length = 0;
        t->capacity = init_cap;
    } else {
        loom_runtime_abort("could not allocate Text buffer");
    }
}

static void loom_text_ensure_cap(LoomText* t, uint64_t needed) {
    if (!t) loom_runtime_abort("invalid Text buffer");
    if (!t->data || t->capacity == 0) {
        loom_text_init(t, needed > 64 ? needed : 64);
        return;
    }
    if (needed <= t->capacity) return;

    uint64_t new_cap;
    if (t->capacity > UINT64_MAX / 2) new_cap = needed;
    else new_cap = t->capacity * 2;
    if (new_cap < needed) {
        if (needed > UINT64_MAX - 64) loom_runtime_abort("Text capacity overflow");
        new_cap = needed + 64;
    }
    if (new_cap > (uint64_t)SIZE_MAX) loom_runtime_abort("Text capacity exceeds addressable memory");

    char* new_data = (char*)realloc(t->data, new_cap);
    if (!new_data) loom_runtime_abort("could not grow Text buffer");
    t->data = new_data;
    t->capacity = new_cap;
}

void loom_text_append_str(LoomText* t, const char* str) {
    if (!t || !str) return;
    uint64_t slen = (uint64_t)strlen(str);
    if (t->length == UINT64_MAX || slen > UINT64_MAX - t->length - 1) {
        loom_runtime_abort("Text length overflow");
    }
    uintptr_t data_addr = (uintptr_t)t->data;
    uintptr_t str_addr = (uintptr_t)str;
    bool aliases = t->data && str_addr >= data_addr && str_addr - data_addr < t->capacity;
    uint64_t source_offset = aliases ? (uint64_t)(str_addr - data_addr) : 0;
    loom_text_ensure_cap(t, t->length + slen + 1);
    if (aliases) str = t->data + source_offset;
    memmove(t->data + t->length, str, (size_t)slen);
    t->length += slen;
    t->data[t->length] = '\0';
}

void loom_text_append_char(LoomText* t, char c) {
    if (!t) return;
    if (t->length > UINT64_MAX - 2) loom_runtime_abort("Text length overflow");
    loom_text_ensure_cap(t, t->length + 2);
    t->data[t->length++] = c;
    t->data[t->length] = '\0';
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
    if (!res) loom_runtime_abort("could not allocate Text subslice");
    memcpy(res, t->data + (start - 1), (size_t)len);
    res[len] = '\0';
    return res;
}

int loom_text_contains(LoomText* t, const char* sub) {
    if (!t || !t->data || !sub) return 0;
    return strstr(t->data, sub) != NULL ? 1 : 0;
}

uint64_t loom_text_index(LoomText* t, const char* sub) {
    if (!t || !t->data || !sub) return 0;
    const char* found = strstr(t->data, sub);
    if (!found) return 0;
    return (uint64_t)(found - t->data + 1);
}

void loom_text_clear(LoomText* t) {
    if (!t) return;
    t->length = 0;
    if (t->data) t->data[0] = '\0';
}

void loom_text_clone(LoomText* dest, const LoomText* src) {
    if (!dest || !src) return;
    loom_text_init(dest, src->capacity > 0 ? src->capacity : 64);
    if (src->data && src->length > 0) {
        loom_text_append_str(dest, src->data);
    }
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
    if (!v->data) loom_runtime_abort("could not allocate Vector storage");
    if (v->data && init_elem_count > 0) {
        memset(v->data, 0, init_elem_count * (size_t)elem_size);
    }
}

void* loom_vec_grow(LoomVector* v) {
    if (!v) return NULL;
    if (v->capacity > UINT64_MAX / 2) loom_runtime_abort("Vector capacity overflow");
    uint64_t new_cap = v->capacity > 0 ? v->capacity * 2 : 16;
    void* new_data = loom_realloc(v->data, new_cap, v->elem_size, v->elem_align);
    if (!new_data) loom_runtime_abort("could not grow Vector storage");
    v->data = new_data;
    v->capacity = new_cap;
    return v->data;
}

void* loom_vec_get(LoomVector* v, uint64_t one_based_idx) {
    if (!v || !v->data || one_based_idx < 1 || one_based_idx > v->length) {
        loom_panic_bounds("<vector>", 0, (int64_t)one_based_idx, (int64_t)(v ? v->length : 0));
    }
    return (char*)v->data + (one_based_idx - 1) * (uint64_t)v->elem_size;
}

void loom_vec_append(LoomVector* v, const void* elem_val) {
    if (!v || !elem_val) return;
    if (!v->data) loom_runtime_abort("Vector has no allocated storage");
    if (v->length == UINT64_MAX) loom_runtime_abort("Vector length overflow");
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

uint64_t loom_buf_index(const void* data, uint64_t length, uint32_t elem_size, const void* elem_val) {
    if (!data || !elem_val || length == 0) return 0;

    if (elem_size == 4) {
        uint32_t target;
        memcpy(&target, elem_val, 4);
        const uint32_t* p = (const uint32_t*)data;
        for (uint64_t i = 0; i < length; ++i) {
            if (p[i] == target) return i + 1;
        }
        return 0;
    }
    if (elem_size == 8) {
        uint64_t target;
        memcpy(&target, elem_val, 8);
        const uint64_t* p = (const uint64_t*)data;
        for (uint64_t i = 0; i < length; ++i) {
            if (p[i] == target) return i + 1;
        }
        return 0;
    }

    for (uint64_t i = 0; i < length; ++i) {
        if (memcmp((const char*)data + i * (size_t)elem_size, elem_val, elem_size) == 0) {
            return i + 1;
        }
    }
    return 0;
}

int loom_buf_contains(const void* data, uint64_t length, uint32_t elem_size, const void* elem_val) {
    return loom_buf_index(data, length, elem_size, elem_val) != 0 ? 1 : 0;
}

void loom_buf_index_all(LoomVector* out_vec, const void* data, uint64_t length, uint32_t elem_size, const void* elem_val) {
    if (!out_vec) return;
    loom_vec_init(out_vec, 0, sizeof(uint64_t), sizeof(uint64_t));
    if (!data || !elem_val || length == 0) return;

    if (elem_size == 4) {
        uint32_t target;
        memcpy(&target, elem_val, 4);
        const uint32_t* p = (const uint32_t*)data;
        for (uint64_t i = 0; i < length; ++i) {
            if (p[i] == target) {
                uint64_t idx = i + 1;
                loom_vec_append(out_vec, &idx);
            }
        }
        return;
    }
    if (elem_size == 8) {
        uint64_t target;
        memcpy(&target, elem_val, 8);
        const uint64_t* p = (const uint64_t*)data;
        for (uint64_t i = 0; i < length; ++i) {
            if (p[i] == target) {
                uint64_t idx = i + 1;
                loom_vec_append(out_vec, &idx);
            }
        }
        return;
    }

    for (uint64_t i = 0; i < length; ++i) {
        if (memcmp((const char*)data + i * (size_t)elem_size, elem_val, elem_size) == 0) {
            uint64_t idx = i + 1;
            loom_vec_append(out_vec, &idx);
        }
    }
}

uint64_t loom_vec_index(LoomVector* v, const void* elem_val) {
    if (!v) return 0;
    return loom_buf_index(v->data, v->length, v->elem_size, elem_val);
}

void loom_vec_index_all(LoomVector* out_vec, LoomVector* v, const void* elem_val) {
    if (!v) return;
    loom_buf_index_all(out_vec, v->data, v->length, v->elem_size, elem_val);
}

int loom_vec_contains(LoomVector* v, const void* elem_val) {
    if (!v) return 0;
    return loom_buf_contains(v->data, v->length, v->elem_size, elem_val);
}

void loom_vec_clear(LoomVector* v) {
    if (v) v->length = 0;
}

void loom_vec_clone(LoomVector* dest, const LoomVector* src) {
    if (!dest || !src) return;
    loom_vec_init(dest, src->capacity, src->elem_size, src->elem_align);
    dest->length = src->length;
    if (src->data && src->length > 0) {
        memcpy(dest->data, src->data, (size_t)(src->length * (uint64_t)src->elem_size));
    }
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
    if (!m) loom_runtime_abort("could not allocate HMap");
    m->key_kind = key_kind;
    m->val_size = val_size ? val_size : 8;
    m->val_align = val_align ? val_align : 8;
    m->capacity = 16;
    m->length = 0;
    m->entries = (LoomHMapEntry*)calloc(m->capacity, sizeof(LoomHMapEntry));
    if (!m->entries) {
        free(m);
        loom_runtime_abort("could not allocate HMap entries");
    }
    return m;
}

static void loom_hmap_resize(LoomHMap* m, uint64_t new_cap) {
    if (new_cap < 16) new_cap = 16;
    LoomHMapEntry* old_entries = m->entries;
    uint64_t old_cap = m->capacity;

    LoomHMapEntry* new_entries = (LoomHMapEntry*)calloc(new_cap, sizeof(LoomHMapEntry));
    if (!new_entries) loom_runtime_abort("could not grow HMap entries");
    m->entries = new_entries;
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
    if (!val || !m->entries) loom_runtime_abort("invalid HMap insertion storage");
    if (m->capacity > UINT64_MAX / 2) loom_runtime_abort("HMap capacity overflow");
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
        if (!m->entries[target].value) loom_runtime_abort("could not allocate HMap value");
    }
    memcpy(m->entries[target].value, val, m->val_size);
    m->length++;
}

void loom_hmap_insert_str(LoomHMap* m, const char* key, const void* val) {
    if (!m || !key) return;
    if (!val || !m->entries) loom_runtime_abort("invalid HMap insertion storage");
    if (m->capacity > UINT64_MAX / 2) loom_runtime_abort("HMap capacity overflow");
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
    if (!m->entries[target].key.str) loom_runtime_abort("could not allocate HMap key");
    if (!m->entries[target].value) {
        m->entries[target].value = malloc(m->val_size);
        if (!m->entries[target].value) loom_runtime_abort("could not allocate HMap value");
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

int loom_hmap_iter_next(LoomHMap* m, uint64_t* bucket_idx, void** out_key, void** out_val) {
    if (!m || !bucket_idx || !out_key || !out_val) return 0;
    while (*bucket_idx < m->capacity) {
        uint64_t idx = (*bucket_idx)++;
        if (m->entries[idx].state == LOOM_HMAP_OCCUPIED) {
            if (m->key_kind == 1) {
                *out_key = (void*)m->entries[idx].key.str;
            } else {
                *out_key = (void*)(uintptr_t)m->entries[idx].key.i64;
            }
            *out_val = m->entries[idx].value;
            return 1;
        }
    }
    return 0;
}

void loom_hmap_clear(LoomHMap* m) {
    if (!m || !m->entries) return;
    for (uint64_t i = 0; i < m->capacity; ++i) {
        if (m->entries[i].state == LOOM_HMAP_OCCUPIED) {
            if (m->key_kind == 1 && m->entries[i].key.str) {
                free(m->entries[i].key.str);
                m->entries[i].key.str = NULL;
            }
            if (m->entries[i].value) {
                free(m->entries[i].value);
                m->entries[i].value = NULL;
            }
        }
        m->entries[i].state = LOOM_HMAP_EMPTY;
    }
    m->length = 0;
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
