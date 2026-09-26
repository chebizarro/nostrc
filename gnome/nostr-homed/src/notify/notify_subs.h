/*
 * notify_subs — subscription driver for the notify daemon.
 *
 * Wires up two NIP-01 subscriptions per Piece B D2:
 *   1. {kinds: [9,10,11,12]} for NIP-29 group messages (h-tag filter is
 *      applied client-side against the daemon's cached group set).
 *   2. {kinds: [1059], "#p": [user_pubkey]} for NIP-17 DM gift wraps.
 *
 * Upstream selection follows `nostr_notify_upstream_mode`:
 *   - "direct" (v1 default): connect to home_relays with libnostr's
 *     `nostr_relay_new()` wss client. Home_relays are discovered from the
 *     signer's `org.nostr.Signer.GetRelays` if available, else read from
 *     the daemon's config file's `home_relays` field.
 *   - "session_relay" (v1 stub): would connect to
 *     `$XDG_RUNTIME_DIR/nostr/relay.sock`. Because libnostr's ws client
 *     currently only speaks ws://+wss:// (no unix://), this mode logs
 *     a warning and falls back to "direct". Tracked as a follow-up.
 *
 * Events flow through libnostr's GoChannel; a background pthread pulls
 * from the channel, hex-verifies the fields the notifier cares about
 * (id, kind, content, first "h" tag for NIP-29), snapshots the current
 * suppression generation, and posts a GSource callback to the daemon's
 * main context so the notification build + send + withdraw serializes
 * on the same thread that owns the generation guard.
 */
#ifndef NOSTR_NOTIFY_SUBS_H
#define NOSTR_NOTIFY_SUBS_H

#include <gio/gio.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#include "notify_suppress.h"

/* Forward declarations to avoid pulling in libnostr headers here. */
struct NostrRelay;
struct NostrSubscription;

typedef enum {
  NSN_UPSTREAM_DIRECT = 0,       /* home_relays via libnostr */
  NSN_UPSTREAM_SESSION_RELAY,    /* $XDG_RUNTIME_DIR/nostr/relay.sock (v1 stub) */
} NostrNotifyUpstreamMode;

typedef struct {
  /* User's hex pubkey (64-char lowercase). Empty if the signer had no
   * account (in that case the daemon idles). */
  char user_pubkey_hex[65];

  /* Upstream policy (see NostrNotifyUpstreamMode). */
  NostrNotifyUpstreamMode mode;

  /* NULL-terminated array of home_relay URLs (wss:// or ws://). Only used
   * in DIRECT mode. Owned by the daemon config; not freed here. */
  const char **home_relays;
  size_t home_relays_count;

  /* Guard shared with the daemon main. */
  NostrNotifySuppressGuard *guard;

  /* Owning GApplication for `send_notification` / `withdraw_notification`.
   * The daemon binds this at initialization. */
  GApplication *app;

  /* Cursor bookkeeping — persisted in ~/.local/state/nostr-notify/cursor
   * (0600, atomic tmp+rename). Read at start; refreshed on every
   * successful notification send. */
  int64_t last_seen_created_at;

  /* Daemon start time (Unix seconds) — used as the older bound for
   * catch-up suppression: skip events with created_at < daemon_start - 600. */
  int64_t daemon_start_unix;
} NostrNotifySubsCtx;

/*
 * Start the subscription drivers. Returns TRUE if at least one upstream
 * connection is armed; returns FALSE if no upstream is available (the
 * daemon idles). Non-blocking — spawns internal worker threads.
 */
bool nostr_notify_subs_start(NostrNotifySubsCtx *ctx);

/*
 * Cancel all subscriptions and join worker threads. Idempotent.
 */
void nostr_notify_subs_stop(NostrNotifySubsCtx *ctx);

#endif /* NOSTR_NOTIFY_SUBS_H */
