#include "loom_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>

#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

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
    bool worker_done;
    bool counted_as_user_task;
};

#define TASK_QUEUE_CAPACITY 1024

typedef struct {
    LoomTask* queue_head;
    LoomTask* queue_tail;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    bool shutdown;
    int worker_count;
    pthread_t workers[32];
} LoomThreadPool;

static LoomThreadPool g_pool = {
    .queue_head = NULL,
    .queue_tail = NULL,
    .queue_mutex = PTHREAD_MUTEX_INITIALIZER,
    .queue_cond = PTHREAD_COND_INITIALIZER,
    .shutdown = false,
    .worker_count = 0
};

static _Thread_local int g_inside_loom_worker = 0;
static void loom_init_thread_pool(void);
static _Atomic(uint32_t) g_user_tasks_outstanding = 0;

static struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool active;
    uint64_t count;
    uint64_t grain;
    _Atomic(uint64_t) next;
    uint32_t active_workers;
    LoomParallelFn fn;
    LoomParallelChunkFn chunk_fn;
    void* context;
} g_parallel = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .active = false,
    .count = 0,
    .grain = 1,
    .next = 0,
    .active_workers = 0,
    .fn = NULL,
    .context = NULL
};

static uint64_t loom_parallel_claim(uint64_t count, uint64_t grain) {
    uint64_t begin = atomic_load_explicit(&g_parallel.next, memory_order_relaxed);
    for (;;) {
        if (begin >= count) return count;
        const uint64_t remaining = count - begin;
        const uint64_t end = remaining < grain ? count : begin + grain;
        if (atomic_compare_exchange_weak_explicit(&g_parallel.next, &begin, end,
                memory_order_relaxed, memory_order_relaxed)) return begin;
    }
}

static void loom_parallel_run_claimed(uint64_t begin, LoomParallelFn fn, void* context,
                                      LoomParallelChunkFn chunk_fn, uint64_t count, uint64_t grain) {
    uint64_t remaining = count - begin;
    uint64_t end = remaining < grain ? count : begin + grain;
    if (chunk_fn) chunk_fn(context, begin, end);
    else for (uint64_t i = begin; i < end; ++i) fn(context, i);
}

static bool loom_parallel_worker_run(void) {
    pthread_mutex_lock(&g_parallel.mutex);
    if (!g_parallel.active) {
        pthread_mutex_unlock(&g_parallel.mutex);
        return false;
    }
    const uint64_t count = g_parallel.count, grain = g_parallel.grain;
    uint64_t begin = loom_parallel_claim(count, grain);
    if (begin >= g_parallel.count) {
        pthread_mutex_unlock(&g_parallel.mutex);
        return false;
    }
    ++g_parallel.active_workers;
    LoomParallelFn fn = g_parallel.fn;
    LoomParallelChunkFn chunk_fn = g_parallel.chunk_fn;
    void* context = g_parallel.context;
    pthread_mutex_unlock(&g_parallel.mutex);

    g_inside_loom_worker = 1;
    do {
        loom_parallel_run_claimed(begin, fn, context, chunk_fn, count, grain);
        begin = loom_parallel_claim(count, grain);
    } while (begin < count);
    g_inside_loom_worker = 0;

    pthread_mutex_lock(&g_parallel.mutex);
    --g_parallel.active_workers;
    pthread_cond_broadcast(&g_parallel.cond);
    pthread_mutex_unlock(&g_parallel.mutex);
    return true;
}

static void* loom_worker_thread(void* arg) {
    (void)arg;
    while (1) {
        if (loom_parallel_worker_run()) continue;
        pthread_mutex_lock(&g_pool.queue_mutex);
        while (!g_pool.queue_head && !g_pool.shutdown) {
            pthread_cond_wait(&g_pool.queue_cond, &g_pool.queue_mutex);
            pthread_mutex_unlock(&g_pool.queue_mutex);
            if (loom_parallel_worker_run()) {
                pthread_mutex_lock(&g_pool.queue_mutex);
                break;
            }
            pthread_mutex_lock(&g_pool.queue_mutex);
        }

        if (!g_pool.queue_head && !g_pool.shutdown) {
            pthread_mutex_unlock(&g_pool.queue_mutex);
            continue;
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
                if (task->counted_as_user_task) {
                    atomic_fetch_sub_explicit(&g_user_tasks_outstanding, 1, memory_order_release);
                    task->counted_as_user_task = false;
                }
                task->worker_done = true;
                pthread_cond_broadcast(&task->cond);
                pthread_mutex_unlock(&task->lock);
                continue;
            }
            task->state = LOOM_TASK_RUNNING;
            pthread_mutex_unlock(&task->lock);

            /* Execute target task procedure */
            g_inside_loom_worker = 1;
            task->entry(task->args, task->result);
            g_inside_loom_worker = 0;

            pthread_mutex_lock(&task->lock);
            if (task->state != LOOM_TASK_CANCELLED) {
                task->state = LOOM_TASK_COMPLETED;
            }
            if (task->counted_as_user_task) {
                atomic_fetch_sub_explicit(&g_user_tasks_outstanding, 1, memory_order_release);
                task->counted_as_user_task = false;
            }
            task->worker_done = true;
            pthread_cond_broadcast(&task->cond);
            pthread_mutex_unlock(&task->lock);
        }
    }
    return NULL;
}

static void loom_parallel_dispatch(uint64_t count, uint64_t minimum_grain, LoomParallelFn fn,
                                   LoomParallelChunkFn chunk_fn, void* context) {
    if ((!fn && !chunk_fn) || count == 0) return;
    if (minimum_grain == 0) minimum_grain = 1;
    if (count < minimum_grain || g_inside_loom_worker ||
        atomic_load_explicit(&g_user_tasks_outstanding, memory_order_acquire) != 0) {
        if (chunk_fn) chunk_fn(context, 0, count);
        else for (uint64_t i = 0; i < count; ++i) fn(context, i);
        return;
    }

    loom_init_thread_pool();
    uint64_t needed = count / minimum_grain + (count % minimum_grain != 0);
    /* Fewer than eight chunks rarely amortize pool wakeups and synchronization. */
    if (g_pool.worker_count < 2 || needed < 8) {
        if (chunk_fn) chunk_fn(context, 0, count);
        else for (uint64_t i = 0; i < count; ++i) fn(context, i);
        return;
    }

    pthread_mutex_lock(&g_parallel.mutex);
    while (g_parallel.active) pthread_cond_wait(&g_parallel.cond, &g_parallel.mutex);
    g_parallel.active = true;
    g_parallel.count = count;
    g_parallel.grain = minimum_grain;
    g_parallel.active_workers = 0;
    g_parallel.fn = fn;
    g_parallel.chunk_fn = chunk_fn;
    g_parallel.context = context;
    atomic_store_explicit(&g_parallel.next, 0, memory_order_relaxed);
    pthread_mutex_unlock(&g_parallel.mutex);

    pthread_mutex_lock(&g_pool.queue_mutex);
    pthread_cond_broadcast(&g_pool.queue_cond);
    pthread_mutex_unlock(&g_pool.queue_mutex);

    uint64_t begin;
    while ((begin = loom_parallel_claim(count, minimum_grain)) < count)
        loom_parallel_run_claimed(begin, fn, context, chunk_fn, count, minimum_grain);

    pthread_mutex_lock(&g_parallel.mutex);
    while (g_parallel.active_workers != 0)
        pthread_cond_wait(&g_parallel.cond, &g_parallel.mutex);
    g_parallel.active = false;
    g_parallel.fn = NULL;
    g_parallel.chunk_fn = NULL;
    g_parallel.context = NULL;
    pthread_cond_broadcast(&g_parallel.cond);
    pthread_mutex_unlock(&g_parallel.mutex);
}

void loom_parallel_for(uint64_t count, uint64_t minimum_grain, LoomParallelFn fn, void* context) {
    loom_parallel_dispatch(count, minimum_grain, fn, NULL, context);
}

void loom_parallel_for_chunks(uint64_t count, uint64_t minimum_grain, LoomParallelChunkFn fn, void* context) {
    loom_parallel_dispatch(count, minimum_grain, NULL, fn, context);
}

typedef struct {
    uint32_t operation;
    const float* left;
    const float* right;
    float* output;
    float factor;
} LoomF32ApplyContext;

static void loom_f32_apply_chunk(void* opaque, uint64_t begin, uint64_t end) {
    const LoomF32ApplyContext* c = (const LoomF32ApplyContext*)opaque;
    if (c->operation == 3) {
        for (uint64_t i = begin; i < end; ++i) c->output[i] = c->left[i] * c->factor;
    } else if (c->operation == 0) {
        for (uint64_t i = begin; i < end; ++i) c->output[i] = c->left[i] + c->right[i];
    } else if (c->operation == 1) {
        for (uint64_t i = begin; i < end; ++i) c->output[i] = c->left[i] - c->right[i];
    } else {
        for (uint64_t i = begin; i < end; ++i) c->output[i] = c->left[i] * c->right[i];
    }
}

static int loom_ranges_partially_overlap(const void* a, const void* b, uint64_t bytes) {
    if (a == b) return 0; /* Exact in-place operation is element-independent. */
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    if (bytes > UINTPTR_MAX - x || bytes > UINTPTR_MAX - y) return 1;
    return x < y + bytes && y < x + bytes;
}

void loom_vec_f32_apply(uint32_t operation, const float* left, const float* right,
                        float* output, float factor, uint64_t count) {
    if (count == 0) return;
    if (operation > 3) loom_runtime_abort("invalid Vector operation");
    if (!left || !output || (operation != 3 && !right))
        loom_runtime_abort("null pointer in Vector operation");
    if (count > SIZE_MAX / sizeof(float)) loom_runtime_abort("Vector operation size overflow");
    const uint64_t bytes = count * sizeof(float);
    if (loom_ranges_partially_overlap(left, output, bytes) ||
        (operation != 3 && loom_ranges_partially_overlap(right, output, bytes))) {
        LoomF32ApplyContext context = {operation, left, right, output, factor};
        loom_f32_apply_chunk(&context, 0, count);
        return;
    }
    LoomF32ApplyContext context = {operation, left, right, output, factor};
    loom_parallel_for_chunks(count, 32768, loom_f32_apply_chunk, &context);
}

static void loom_init_thread_pool_once(void) {
    pthread_mutex_lock(&g_pool.queue_mutex);

    int ncpus = 4;
#if defined(_SC_NPROCESSORS_ONLN)
    long sc = sysconf(_SC_NPROCESSORS_ONLN);
    if (sc > 0) ncpus = (int)sc;
#endif
    if (ncpus > 16) ncpus = 16;
    if (ncpus < 2)  ncpus = 2;

    g_pool.worker_count = 0;
    for (int i = 0; i < ncpus; i++) {
        if (pthread_create(&g_pool.workers[g_pool.worker_count], NULL, loom_worker_thread, NULL) == 0) {
            g_pool.worker_count++;
        }
    }
    pthread_mutex_unlock(&g_pool.queue_mutex);
    if (g_pool.worker_count == 0) loom_runtime_abort("could not start a task worker thread");
}

static pthread_once_t g_pool_once = PTHREAD_ONCE_INIT;

static void loom_init_thread_pool(void) {
    if (pthread_once(&g_pool_once, loom_init_thread_pool_once) != 0) {
        loom_runtime_abort("could not initialize task worker pool");
    }
}

LoomTask* loom_task_spawn(LoomTaskFn entry, void* args, size_t args_size, size_t result_size) {
    if (!entry || (args_size > 0 && !args)) loom_runtime_abort("invalid task entry or argument buffer");
    loom_init_thread_pool();

    LoomTask* task = (LoomTask*)malloc(sizeof(LoomTask));
    if (!task) loom_runtime_abort("could not allocate task handle");

    task->magic = LOOM_TASK_MAGIC;
    task->state = LOOM_TASK_PENDING;
    task->entry = entry;
    task->args_size = args_size;
    task->result_size = result_size;
    task->next = NULL;
    task->worker_done = false;
    task->counted_as_user_task = true;
    atomic_fetch_add_explicit(&g_user_tasks_outstanding, 1, memory_order_release);

    if (args_size > 0 && args) {
        task->args = malloc(args_size);
        if (!task->args) loom_runtime_abort("could not allocate task arguments");
        memcpy(task->args, args, args_size);
    } else {
        task->args = NULL;
    }

    if (result_size > 0) {
        task->result = malloc(result_size);
        if (!task->result) loom_runtime_abort("could not allocate task result");
        memset(task->result, 0, result_size);
    } else {
        task->result = NULL;
    }

    if (pthread_mutex_init(&task->lock, NULL) != 0 || pthread_cond_init(&task->cond, NULL) != 0) {
        loom_runtime_abort("could not initialize task synchronization primitives");
    }

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

    while (task->state != LOOM_TASK_COMPLETED && task->state != LOOM_TASK_FAILED &&
           (task->state != LOOM_TASK_CANCELLED || !task->worker_done)) {
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
    while (task->state != LOOM_TASK_COMPLETED && task->state != LOOM_TASK_FAILED &&
           (task->state != LOOM_TASK_CANCELLED || !task->worker_done)) {
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

    if ((task->state == LOOM_TASK_CANCELLED && task->worker_done) || task->state == LOOM_TASK_FAILED) {
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
   Mutex Implementation
   --------------------------------------------------------------------------- */

struct LoomMutex {
    pthread_mutex_t mtx;
};

LoomMutex* loom_mutex_create(void) {
    LoomMutex* m = (LoomMutex*)malloc(sizeof(LoomMutex));
    if (!m) return NULL;
    pthread_mutex_init(&m->mtx, NULL);
    return m;
}

void loom_mutex_lock(LoomMutex* m) {
    if (m) pthread_mutex_lock(&m->mtx);
}

int loom_mutex_try_lock(LoomMutex* m) {
    if (!m) return 0;
    return pthread_mutex_trylock(&m->mtx) == 0 ? 1 : 0;
}

void loom_mutex_unlock(LoomMutex* m) {
    if (m) pthread_mutex_unlock(&m->mtx);
}

void loom_mutex_free(LoomMutex* m) {
    if (m) {
        pthread_mutex_destroy(&m->mtx);
        free(m);
    }
}

/* ---------------------------------------------------------------------------
   Channel[T] Implementation
   --------------------------------------------------------------------------- */

struct LoomChannel {
    pthread_mutex_t mutex;
    pthread_cond_t  cond_not_empty;
    pthread_cond_t  cond_not_full;
    uint8_t*        buffer;
    uint64_t        capacity;
    uint64_t        count;
    uint64_t        head;
    uint64_t        tail;
    uint32_t        elem_size;
    int             closed;
};

LoomChannel* loom_channel_create(uint64_t capacity, uint32_t elem_size) {
    if (capacity == 0) capacity = 1;
    if (elem_size == 0) elem_size = 1;
    LoomChannel* ch = (LoomChannel*)calloc(1, sizeof(LoomChannel));
    if (!ch) return NULL;
    ch->capacity = capacity;
    ch->elem_size = elem_size;
    ch->buffer = (uint8_t*)calloc(capacity, elem_size);
    if (!ch->buffer) { free(ch); return NULL; }
    pthread_mutex_init(&ch->mutex, NULL);
    pthread_cond_init(&ch->cond_not_empty, NULL);
    pthread_cond_init(&ch->cond_not_full, NULL);
    return ch;
}

void loom_channel_send(LoomChannel* ch, const void* elem_val) {
    if (!ch || !elem_val) return;
    pthread_mutex_lock(&ch->mutex);
    while (ch->count == ch->capacity && !ch->closed) {
        pthread_cond_wait(&ch->cond_not_full, &ch->mutex);
    }
    if (ch->closed) {
        pthread_mutex_unlock(&ch->mutex);
        return;
    }
    memcpy(ch->buffer + (ch->tail * ch->elem_size), elem_val, ch->elem_size);
    ch->tail = (ch->tail + 1) % ch->capacity;
    ch->count++;
    pthread_cond_signal(&ch->cond_not_empty);
    pthread_mutex_unlock(&ch->mutex);
}

int loom_channel_try_send(LoomChannel* ch, const void* elem_val) {
    if (!ch || !elem_val) return 0;
    pthread_mutex_lock(&ch->mutex);
    if (ch->count == ch->capacity || ch->closed) {
        pthread_mutex_unlock(&ch->mutex);
        return 0;
    }
    memcpy(ch->buffer + (ch->tail * ch->elem_size), elem_val, ch->elem_size);
    ch->tail = (ch->tail + 1) % ch->capacity;
    ch->count++;
    pthread_cond_signal(&ch->cond_not_empty);
    pthread_mutex_unlock(&ch->mutex);
    return 1;
}

void loom_channel_receive(LoomChannel* ch, void* out_val) {
    if (!ch || !out_val) return;
    pthread_mutex_lock(&ch->mutex);
    while (ch->count == 0 && !ch->closed) {
        pthread_cond_wait(&ch->cond_not_empty, &ch->mutex);
    }
    if (ch->count == 0 && ch->closed) {
        memset(out_val, 0, ch->elem_size);
        pthread_mutex_unlock(&ch->mutex);
        return;
    }
    memcpy(out_val, ch->buffer + (ch->head * ch->elem_size), ch->elem_size);
    ch->head = (ch->head + 1) % ch->capacity;
    ch->count--;
    pthread_cond_signal(&ch->cond_not_full);
    pthread_mutex_unlock(&ch->mutex);
}

int loom_channel_try_receive(LoomChannel* ch, void* out_val) {
    if (!ch || !out_val) return 0;
    pthread_mutex_lock(&ch->mutex);
    if (ch->count == 0) {
        pthread_mutex_unlock(&ch->mutex);
        return 0;
    }
    memcpy(out_val, ch->buffer + (ch->head * ch->elem_size), ch->elem_size);
    ch->head = (ch->head + 1) % ch->capacity;
    ch->count--;
    pthread_cond_signal(&ch->cond_not_full);
    pthread_mutex_unlock(&ch->mutex);
    return 1;
}

void loom_channel_close(LoomChannel* ch) {
    if (!ch) return;
    pthread_mutex_lock(&ch->mutex);
    ch->closed = 1;
    pthread_cond_broadcast(&ch->cond_not_empty);
    pthread_cond_broadcast(&ch->cond_not_full);
    pthread_mutex_unlock(&ch->mutex);
}

void loom_channel_free(LoomChannel* ch) {
    if (!ch) return;
    loom_channel_close(ch);
    pthread_mutex_destroy(&ch->mutex);
    pthread_cond_destroy(&ch->cond_not_empty);
    pthread_cond_destroy(&ch->cond_not_full);
    if (ch->buffer) free(ch->buffer);
    free(ch);
}
