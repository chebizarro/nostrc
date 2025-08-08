# Shutdown Order and Invariants

This document describes the recommended shutdown sequence and invariants for libnostr (relay/subscriptions) and libgo (channels/contexts/select). Following these guidelines ensures clean termination without hangs or deadlocks.

## Recommended Shutdown Order

1. Cancel the connection context
   - Call `relay->priv->connection_context_cancel(relay->priv->connection_context)`.
   - This unblocks `message_loop()` and `write_operations()` waits and any `go_*_with_context()` operations.

2. Close channels/queues
   - Close `relay->priv->write_queue`, `relay->priv->subscription_channel_close_queue`, and `relay->priv->debug_raw` (if enabled).
   - The subscription lifecycle closes `sub->events` upon context cancellation.

3. Close the network connection
   - Call `connection_close(relay->connection)` last, after cancellation and queue closure.

4. Wait for worker goroutines
   - Call `go_wait_group_wait(&relay->priv->workers)` to join `message_loop()` and `write_operations()`.

5. Destroy wait groups and free resources
   - `go_wait_group_destroy(&relay->priv->workers)`; then free relay, subscriptions, and other allocations.

## Invariants and Best Practices

- Do not block on event dispatch during shutdown
  - Use `go_channel_try_send()` in `subscription_dispatch_event()`.
  - Drop and free events when the subscription is not live, the channel is full, or closed.

- Make channel waits cancel-aware
  - Use `go_channel_send_with_context()` / `go_channel_receive_with_context()` for blocking operations.
  - These functions wake when: channel has room/data, channel is closed, or context is canceled.

- Select must wake on closure/cancel
  - `go_select()` treats closed receive channels as ready (no hang on shutdown).
  - Provide a case on `ctx->done` when waiting on cancellation.

- Subscription lifecycle
  - `subscription_start()` monitors the subscription context and, on cancel, calls `subscription_unsub()` and closes `sub->events` to unblock senders.

- Relay workers
  - `write_operations()` includes a fast-path `go_context_is_canceled()` check to break promptly even if `go_select()` misses closure.

## Troubleshooting Hangs

If the process prints "Closing..." but does not exit:

- Check for a wait at `go_wait_group_wait()`
  - One of the workers likely has not exited.

- Inspect worker threads
  - `message_loop()`: may be blocked in I/O if context not canceled or connection not closed.
  - `write_operations()`: ensure select wakes on closed channels/cancel; verify fast-path cancel break.

- Inspect subscription dispatch
  - Ensure `subscription_dispatch_event()` is non-blocking and drops on full/closed.

- Inspect contexts
  - Verify `go_context_cancel()` closes `done` and wakes waiters.

- Inspect channels
  - Verify `go_channel_close()` was called on queues to wake blocked senders/receivers.

## Example Shutdown (relay)

```
// Signal background loops to stop
relay->priv->connection_context_cancel(relay->priv->connection_context);
// Close queues to unblock workers
go_channel_close(relay->priv->write_queue);
go_channel_close(relay->priv->subscription_channel_close_queue);
if (relay->priv->debug_raw) go_channel_close(relay->priv->debug_raw);
// Close network connection last
connection_close(relay->connection);
// Wait for workers
go_wait_group_wait(&relay->priv->workers);
```

## Notes

- NOSTR_DEBUG_SHUTDOWN=1 can be used (if enabled) to emit diagnostic breadcrumbs during teardown.
- Use sanitizer options (ASAN/UBSAN/TSAN) in Debug builds to catch lifecycle issues early.
