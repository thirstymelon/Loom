# Arena allocation

The runtime provides an optional bump arena for workloads that create many short-lived objects and release them together. It is a systems-level API: returned addresses are raw pointers, and the caller must keep the arena alive while using them.

The arena is single-threaded. `loom_arena_alloc` returns null for an invalid handle, zero or non-power-of-two alignment, an invalid size, or exhausted capacity. Allocations are not individually freed. `loom_arena_reset` invalidates **all** pointers from that arena; `loom_arena_destroy` invalidates them and releases the backing memory. The arena does not run Loom `Drop` blocks for objects placed in it.

Declare the C ABI in a Loom module that needs it:

```loom
Extern Block loom_arena_create Takes
    Capacity : Unsigned_64
Produces No_Type Pointer
End loom_arena_create

Extern Block loom_arena_alloc Takes
    Arena : No_Type Pointer
    Size : Unsigned_64
    Alignment : Unsigned_64
Produces No_Type Pointer
End loom_arena_alloc

Extern Block loom_arena_reset Takes
    Arena : No_Type Pointer
End loom_arena_reset

Extern Block loom_arena_destroy Takes
    Arena : No_Type Pointer
End loom_arena_destroy
```

Cast the allocation result to the intended pointer type, check for `Null`, and do not use it after reset or destroy. The runtime API is useful where arena lifetime is already clear; it does not replace Loom's ownership analysis or make raw-pointer dereferences safe.
