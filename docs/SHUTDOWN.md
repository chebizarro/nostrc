# Shutdown Order and Invariants

This document describes the recommended shutdown sequence and invariants for libnostr (relay/subscriptions) and libgo (channels/contexts/select). Following these guidelines ensures clean termination without hangs or deadlocks.

## Recommended Shutdown Order

1. Cancel the connection context
   - Call `relay->priv->connection_context_cancel(relay->priv->connection_context)`.
   - This unblocks `message_loop()` and `write_operations()` and any `go_*_with_context()` operations.

2. Close relay queues
   - Close `relay->priv->write_queue`, `relay->priv->subscription_channel_close_queue`, and `relay->priv->debug_raw` (if enabled).
   - The subscription lifecycle will close `sub->events` upon context cancellation.

3. Snapshot and null out the connection
   - Under the relay mutex, capture `Connection *conn = r->connection` and set `r->connection = NULL`.
   - Workers see `NULL` and/or the canceled context and exit their loops.

4. Wait for worker goroutines
   - Call `go_wait_group_wait(&relay->priv->workers)` to join `message_loop()` and `write_operations()`.

5. Free connection-owned channels
   - After workers exit, free `conn->recv_channel` and `conn->send_channel`.
   - Rationale: avoid use-after-free while workers may still poll `go_select()` / `go_channel_try_receive()`.

6. Close the network connection
   - Call `connection_close(conn)`; this only closes channels (signals) and tears down libwebsockets; it does not free the channels.

7. Destroy wait groups and free resources
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

- Connection channel ownership on shutdown
  - `connection_close()` must not free `conn->recv_channel` or `conn->send_channel`.
  - Only the relay (in `relay_close()`) frees those channels after `go_wait_group_wait()` ensures workers are done.

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

- Use-after-free or sanitizer reports
  - Ensure channels are not freed in `connection_close()`.
  - Ensure `relay_close()` waits for workers, then frees `conn` channels before destroying the connection object.

- Inspect subscription dispatch
  - Ensure `subscription_dispatch_event()` is non-blocking and drops on full/closed.

- Inspect contexts
  - Verify `go_context_cancel()` closes `done` and wakes waiters.

- Inspect channels
  - Verify `go_channel_close()` was called on queues to wake blocked senders/receivers.

## Example Shutdown (relay)

```
// 1) Cancel
relay->priv->connection_context_cancel(relay->priv->connection_context);
// 2) Close relay queues
go_channel_close(relay->priv->write_queue);
go_channel_close(relay->priv->subscription_channel_close_queue);
if (relay->priv->debug_raw) go_channel_close(relay->priv->debug_raw);
// 3) Snapshot and null connection
nsync_mu_lock(&relay->priv->mutex);
Connection *conn = relay->connection;
relay->connection = NULL;
nsync_mu_unlock(&relay->priv->mutex);
// 4) Join workers
go_wait_group_wait(&relay->priv->workers);
// 5) Free connection channels
if (conn && conn->recv_channel) go_channel_free(conn->recv_channel);
if (conn && conn->send_channel) go_channel_free(conn->send_channel);
// 6) Close connection
if (conn) connection_close(conn);
```

## Notes

- NOSTR_DEBUG_SHUTDOWN=1 can be used (if enabled) to emit diagnostic breadcrumbs during teardown.
- Use sanitizer options (ASAN/UBSAN/TSAN) in Debug builds to catch lifecycle issues early.

## Parser Strictness and Safety

- `parse_message()` is intentionally strict on minimal well-formed inputs:
  - EOSE requires a subscription id.
  - CLOSED requires a reason string.
  - OK requires a valid boolean for the third element; optional reason must be a string.
- This ensures malformed inputs fail fast in tests and production.

## Subscription Close Envelope

- `subscription_close()` must allocate a `ClosedEnvelope` with `sizeof(ClosedEnvelope)`, set `base.type = ENVELOPE_CLOSED`, populate `subscription_id`, and free the temporary object after `nostr_envelope_serialize()`.
- Do not use `create_envelope()` for typed extended envelopes; it only allocates the base `Envelope`.
