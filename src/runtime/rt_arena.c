#include "loom_runtime.h"

#include <stdint.h>
#include <stdlib.h>

struct LoomArena {
    unsigned char* storage;
    size_t capacity;
    size_t offset;
};

LoomArena* loom_arena_create(uint64_t capacity) {
    if (capacity == 0 || capacity > SIZE_MAX) return NULL;
    LoomArena* arena = (LoomArena*)malloc(sizeof(*arena));
    if (!arena) return NULL;
    arena->storage = (unsigned char*)malloc((size_t)capacity);
    if (!arena->storage) {
        free(arena);
        return NULL;
    }
    arena->capacity = (size_t)capacity;
    arena->offset = 0;
    return arena;
}

void* loom_arena_alloc(LoomArena* arena, uint64_t size, uint64_t alignment) {
    if (!arena || size > SIZE_MAX || alignment == 0 || alignment > SIZE_MAX ||
        (alignment & (alignment - 1)) != 0) return NULL;

    const uintptr_t base = (uintptr_t)arena->storage;
    if (arena->offset > UINTPTR_MAX - base) return NULL;
    const uintptr_t current = base + arena->offset;
    const uintptr_t mask = (uintptr_t)alignment - 1;
    if (current > UINTPTR_MAX - mask) return NULL;
    const uintptr_t aligned = (current + mask) & ~mask;
    if (aligned < base) return NULL;
    const uintptr_t delta = aligned - base;
    if (delta > arena->capacity || size > arena->capacity - (size_t)delta) return NULL;

    arena->offset = (size_t)delta + (size_t)size;
    return (void*)aligned;
}

void loom_arena_reset(LoomArena* arena) {
    if (arena) arena->offset = 0;
}

void loom_arena_destroy(LoomArena* arena) {
    if (!arena) return;
    free(arena->storage);
    arena->storage = NULL;
    arena->capacity = 0;
    arena->offset = 0;
    free(arena);
}
