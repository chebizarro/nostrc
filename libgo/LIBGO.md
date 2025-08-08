# libgo: Concurrency Primitives for C

libgo provides Go-like concurrency primitives implemented in C: channels, contexts (cancellation & deadlines), select, wait groups, and a ticker utility. All primitives are built on top of nsync mutexes and condition variables and are designed to be safe and efficient.

This document explains the public API, threading/blocking/cancellation semantics, error handling, and usage examples.

- Threading model: All operations are thread-safe. Blocking operations wait on condition variables and wake upon state changes or cancellation.
- Memory model: Create primitives with their constructor/init; free/destroy with the corresponding free/destroy functions. Do not free the underlying structs manually unless explicitly documented.

## Goroutine Wrapper: `go()`

Header: `libgo/include/go.h`

- `int go(void *(*start_routine)(void *), void *arg);`
  - Thin wrapper around `pthread_create` to launch a detached thread (goroutine-like) running `start_routine(arg)`.
  - Returns `0` on success, non-zero on failure.

Guidelines:
- Prefer using `GoWaitGroup` to coordinate completion rather than sleeping.
- Pass a small heap/stack struct as `arg` when multiple parameters are needed.
- Combine with channels/contexts for structured concurrency.

Example: simple goroutines with wait group
```c
typedef struct { const char *msg; GoWaitGroup *wg; } MsgArgs;

void *print_message(void *arg) {
    MsgArgs *ma = (MsgArgs*)arg;
    printf("%s\n", ma->msg);
    go_wait_group_done(ma->wg);
    return NULL;
}

int main(){
    GoWaitGroup wg; go_wait_group_init(&wg);
    go_wait_group_add(&wg, 2);
    MsgArgs a = { .msg = "Hello from thread 1", .wg = &wg };
    MsgArgs b = { .msg = "Hello from thread 2", .wg = &wg };
    go(print_message, &a);
    go(print_message, &b);
    go_wait_group_wait(&wg);
    go_wait_group_destroy(&wg);
}
```

Example: goroutines with channels and close
```c
typedef struct { GoChannel *c; GoWaitGroup *wg; } Args;

void *producer(void *arg){
    Args *a = (Args*)arg;
    for (int i=0;i<10;i++){ int *v = malloc(sizeof(int)); *v = i; go_channel_send(a->c, v); }
    go_channel_close(a->c); // signal done
    go_wait_group_done(a->wg);
    return NULL;
}

void *consumer(void *arg){
    Args *a = (Args*)arg; int *v;
    while (go_channel_receive(a->c, (void**)&v) == 0){ printf("%d\n", *v); free(v); }
    go_wait_group_done(a->wg);
    return NULL;
}
```

## Channels

Header: `libgo/include/channel.h`

Create a bounded channel of pointers:

- `GoChannel *go_channel_create(size_t capacity);`
- `void go_channel_free(GoChannel *chan);`
- `void go_channel_close(GoChannel *chan);` — Marks the channel closed and wakes all waiters. No more sends are accepted; receives will drain remaining buffered items and then fail.

Blocking operations:
- `int go_channel_send(GoChannel *chan, void *data);`
- `int go_channel_receive(GoChannel *chan, void **data);`

Context-aware blocking operations:
- `int go_channel_send_with_context(GoChannel *chan, void *data, GoContext *ctx);`
- `int go_channel_receive_with_context(GoChannel *chan, void **data, GoContext *ctx);`

Non-blocking operations:
- `int go_channel_try_send(GoChannel *chan, void *data);`
- `int go_channel_try_receive(GoChannel *chan, void **data);`

Helpers:
- `int go_channel_has_space(const void *chan);`
- `int go_channel_has_data(const void *chan);`

Return conventions:
- `0` on success; `-1` on failure (e.g., would block, channel closed and no data).

Semantics:
- Send blocks while the buffer is full (unless try_send). Receive blocks while empty and not closed (unless try_receive).
- Closing a channel wakes waiting senders/receivers. After close, sends fail; receives succeed while buffer has elements, then fail.
- All operations are safe under concurrent usage by multiple producers/consumers.

Example (producer/consumer):
```c
void *producer(void *arg) {
    GoChannel *c = arg;
    for (int i = 0; i < 10; ++i) {
        go_channel_send(c, (void*)(long)i);
    }
    go_channel_close(c);
    return NULL;
}

void *consumer(void *arg) {
    GoChannel *c = arg; void *v;
    while (go_channel_receive(c, &v) == 0) {
        printf("got %ld\n", (long)v);
    }
    return NULL;
}
```

## Contexts

Header: `libgo/include/context.h`

Create cancelable or deadline contexts:
- `CancelContextResult go_context_with_cancel(GoContext *parent);` — returns `{ context, cancel }`. Call `cancel(ctx)` to cancel.
- `GoContext *go_with_deadline(GoContext *parent, struct timespec deadline);`

Operations:
- `void go_context_wait(GoContext *ctx);` — block until canceled/deadline.
- `Error *go_context_err(GoContext *ctx);` — returns an error pointer; `err->message` is set to `"context canceled"` or `"context deadline exceeded"` when done; `NULL` while active.
- `void go_context_free(GoContext *ctx);` — frees context and its vtable; do not `free()` the struct or vtable yourself.

Semantics:
- Cancel/Deadline wakes all waiters. Blocking channel ops with a context wake and return promptly on cancellation/deadline.
- Contexts are thread-safe; cancellation may be invoked from any thread and is idempotent.

Example:
```c
CancelContextResult r = go_context_with_cancel(NULL);
// Start a worker that blocks on ctx
pthread_t th; pthread_create(&th, NULL, worker_waiting_on_ctx, r.context);
// Cancel after 2s
sleep(2); r.cancel(r.context);
pthread_join(th, NULL);
go_context_free(r.context);
```

## Select

Header: `libgo/include/select.h`

- `typedef enum { GO_SELECT_SEND, GO_SELECT_RECEIVE } GoSelectOp;`
- `typedef struct { GoSelectOp op; GoChannel *chan; void *value; void **recv_buf; } GoSelectCase;`
- `int go_select(GoSelectCase *cases, size_t num_cases);` — returns the index of the case performed.

Semantics:
- libgo's select performs a fair randomized polling over the provided cases using non-blocking channel operations. It avoids locking multiple channels simultaneously to prevent deadlocks. It sleeps briefly (1ms) between polling passes when nothing is ready.

Example:
```c
void *out = NULL;
GoSelectCase cases[2] = {
  { .op = GO_SELECT_RECEIVE, .chan = c1, .recv_buf = &out },
  { .op = GO_SELECT_SEND, .chan = c2, .value = (void*)123 },
};
int idx = go_select(cases, 2);
```

## Wait Groups

Header: `libgo/include/wait_group.h`

- `void go_wait_group_init(GoWaitGroup *wg);`
- `void go_wait_group_add(GoWaitGroup *wg, int delta);`
- `void go_wait_group_done(GoWaitGroup *wg);`
- `void go_wait_group_wait(GoWaitGroup *wg);`
- `void go_wait_group_destroy(GoWaitGroup *wg);`

Semantics:
- `add(delta)` increments the internal counter; `done()` decrements. `wait()` blocks until the counter reaches zero and wakes on broadcast when it does.

Example:
```c
GoWaitGroup wg; go_wait_group_init(&wg);
go_wait_group_add(&wg, n);
for (int i = 0; i < n; ++i) pthread_create(&ths[i], NULL, worker, &wg);
go_wait_group_wait(&wg);
go_wait_group_destroy(&wg);
```

## Ticker

Header: `libgo/include/ticker.h`

- `Ticker *create_ticker(size_t interval_ms);`
- `void stop_ticker(Ticker *ticker);`
- `ticker->c` is a `GoChannel*` that receives a tick signal (NULL payload) approximately every `interval_ms`.

Semantics:
- Drop-if-full: ticks are sent with `go_channel_try_send()` so a slow consumer will not block the ticker thread; it may miss ticks.

Example:
```c
Ticker *t = create_ticker(50);
void *tick;
int count = 0;
while (count < 10) {
    if (go_channel_receive(t->c, &tick) == 0) count++;
}
stop_ticker(t);
```

## Error

Header: `libgo/include/error.h`

- `typedef struct { int code; char *message; } Error;`
- `Error *new_error(int code, const char *format, ...);`
- `void free_error(Error *err);`
- `void print_error(const Error *err);`
- `int is_error(const Error *err);`

Example:
```c
Error *e = new_error(42, "failed to open: %s", path);
if (is_error(e)) { print_error(e); }
free_error(e);
```

## Reference Pointer (GoRefPtr)

Header: `libgo/include/refptr.h`

- `GoRefPtr make_go_refptr(void *ptr, void (*destructor)(void *));`
- `void go_refptr_retain(GoRefPtr *ref);`
- `void go_refptr_release(GoRefPtr *ref);`
- Auto-cleanup helpers (GCC/Clang): `go_autoptr(Type)`, `go_autostr`.

Example (reference counting a malloc’d buffer):
```c
void free_buf(void *p){ free(p); }
go_autoptr(Buffer) GoRefPtr r = make_go_refptr(malloc(128), free_buf);
go_refptr_retain(&r); // ...share...
// automatically released at end of scope (or call go_refptr_release manually)
```

## Hash Map

Header: `libgo/include/hash_map.h`

- Thread-safe hash map with per-bucket locks.
- Create: `GoHashMap *go_hash_map_create(size_t buckets);`
- Insert: `go_hash_map_insert_str(map, "key", value);`, `go_hash_map_insert_int(map, 123, value);`
- Get: `go_hash_map_get_string(map, "key");`, `go_hash_map_get_int(map, 123);`
- Iterate: `go_hash_map_for_each(map, bool (*fn)(HashKey*, void*));`
- Remove: `go_hash_map_remove_{str,int}()`; Destroy: `go_hash_map_destroy(map);`

Example:
```c
GoHashMap *m = go_hash_map_create(64);
go_hash_map_insert_str(m, "name", (void*)"alice");
const char *name = go_hash_map_get_string(m, "name");
go_hash_map_destroy(m);
```

## Arrays

Headers: `libgo/include/int_array.h`, `libgo/include/string_array.h`

- IntArray:
  - `int_array_init(&arr);`, `int_array_add(&arr, v);`, `int_array_get(&arr, i);`, `int_array_remove(&arr, i);`, `int_array_free(&arr);`
- StringArray:
  - `string_array_init(&arr);`, `string_array_add(&arr, "foo");`, `string_array_get(&arr, i);`, `string_array_remove(&arr, i);`, `string_array_free(&arr);`

Example:
```c
IntArray a; int_array_init(&a); int_array_add(&a, 7); printf("%d\n", int_array_get(&a,0)); int_array_free(&a);
StringArray s; string_array_init(&s); string_array_add(&s, "x"); printf("%s\n", string_array_get(&s,0)); string_array_free(&s);
```

## LongAdder (Counter)

Header: `libgo/include/counter.h`

- Sharded atomic counter for low-contention increments.
- `LongAdder *long_adder_create();`
- `void long_adder_increment(LongAdder *);`
- `long long long_adder_sum(LongAdder *);`
- `void long_adder_reset(LongAdder *);`
- `void long_adder_destroy(LongAdder *);`

Example:
```c
LongAdder *ad = long_adder_create();
for (int i=0;i<1000;i++) long_adder_increment(ad);
printf("sum=%lld\n", long_adder_sum(ad));
long_adder_destroy(ad);
```

## Error Handling

- Channel operations return `0` on success; `-1` on failure (closed or would block for try-ops).
- Context errors can be retrieved via `go_context_err(ctx)` once canceled/timed out; message is one of:
  - `"context canceled"`
  - `"context deadline exceeded"`

## Threading & Blocking Rules

- Do not hold external locks while calling blocking operations on channels or contexts.
- Closing channels and canceling contexts wake all waiters via condition variable broadcasts.
- Context-aware channel operations check cancellation in their wait loops and return promptly when canceled.

## Examples

- See `libgo/examples/` and tests in `libgo/tests/` for usage patterns:
  - `go_channel_test.c` — basic send/receive
  - `go_context_cancel_test.c` — context cancellation
  - `go_context_timeout_test.c` — deadlines
  - `go_ticker_test.c` — ticker usage
  - `go_select_test.c` — select over send/receive
  - `go_wait_group_test.c` — wait group coordination
  - `go_channel_close_test.c` — channel close semantics

## Notes

- Ensure `nsync` is installed and discoverable by CMake. See `DEVELOPMENT.md` for setup.
- The library uses `cmake` + `ctest`. Run from `libgo/`:
  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
  cmake --build build -j
  ctest --test-dir build --output-on-failure
  ```
