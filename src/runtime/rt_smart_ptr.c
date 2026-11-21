#include "loom_runtime.h"

#include <stdlib.h>

/* ---------------------------------------------------------------------------
   Smart Pointer (Smt_Pointer[T]) Implementation
   --------------------------------------------------------------------------- */

void* loom_smt_alloc(uint64_t payload_size, LoomDestructorFn dtor) {
    size_t total = sizeof(LoomSmtControlBlock) + (size_t)payload_size;
    LoomSmtControlBlock* cb = (LoomSmtControlBlock*)calloc(1, total);
    if (!cb) return NULL;
    cb->ref_count = 1;
    cb->destructor = dtor;
    return (void*)((char*)cb + sizeof(LoomSmtControlBlock));
}

void* loom_smt_retain(void* payload_ptr) {
    if (!payload_ptr) return NULL;
    LoomSmtControlBlock* cb = (LoomSmtControlBlock*)((char*)payload_ptr - sizeof(LoomSmtControlBlock));
    __atomic_add_fetch(&cb->ref_count, 1, __ATOMIC_SEQ_CST);
    return payload_ptr;
}

void loom_smt_release(void* payload_ptr) {
    if (!payload_ptr) return;
    LoomSmtControlBlock* cb = (LoomSmtControlBlock*)((char*)payload_ptr - sizeof(LoomSmtControlBlock));
    if (__atomic_sub_fetch(&cb->ref_count, 1, __ATOMIC_SEQ_CST) == 0) {
        if (cb->destructor) {
            cb->destructor(payload_ptr);
        }
        free(cb);
    }
}

uint64_t loom_smt_count(const void* payload_ptr) {
    if (!payload_ptr) return 0;
    const LoomSmtControlBlock* cb = (const LoomSmtControlBlock*)((const char*)payload_ptr - sizeof(LoomSmtControlBlock));
    return __atomic_load_n(&cb->ref_count, __ATOMIC_SEQ_CST);
}
