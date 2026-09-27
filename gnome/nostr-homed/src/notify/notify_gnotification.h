/*
 * notify_gnotification — GNotification builder + activation contract.
 *
 * The notifier owns GApplication ID `org.nostr.NotifyDaemon` (NOT
 * `org.gnostr.Client`, because a GNotification action belongs to the
 * sending GApplication; sharing the ID would break both suppression and
 * activation routing — per §3.3 D4 Finding 4).
 *
 * The default action is `app.open-in-gnostr` carrying a NIP-21 URI
 * `nostr:nevent1…` (event id + kind TLV + the relay the event arrived on):
 *   - DM   : kind 1059 (the gift wrap; its random pubkey is NOT encoded)
 *   - Group: kind 9..12 (the NIP-29 `h` tag is inside the signed event and
 *            the relay hint is the group relay, so no extra parameter)
 * (nostrc-1v65; replaces the invented `nostr://open?event=` form, which
 * nostr-dispatcher still accepts for one transition release.)
 *
 * On activation the notifier opens the URI through the `nostr:` scheme
 * default — nostr-dispatcher, which routes by kind to the registered app —
 * falling back to calling org.nostr.Dispatcher1.Open on the session bus.
 *
 * DM bodies are OPAQUE per §3.3 D3 Finding 14: kind-1059 gift wraps
 * have a fresh random outer pubkey so the notifier cannot derive
 * sender/thread without unwrapping. The visible body is a fixed
 * "You have a new encrypted direct message." string with no hex/pubkey
 * leaks. NIP-29 group messages (kinds 9-12) are plaintext, so the body
 * is a markup-escaped preview truncated to 80 chars.
 */
#ifndef NOSTR_NOTIFY_GNOTIFICATION_H
#define NOSTR_NOTIFY_GNOTIFICATION_H

#include <gio/gio.h>
#include <stdbool.h>

typedef struct {
  /* Withdraw-cookie for GApplication.withdraw_notification(). Deterministic
   * per (thread_key, class) so subsequent events in the same thread coalesce
   * onto the same visible notification. */
  char *withdraw_id;
} NostrNotifyBuild;

/*
 * Build the DM notification for a kind-1059 gift-wrap event. The visible
 * body deliberately carries NO sender/pubkey information (the plan asserts
 * "assert no hex8/pubkey leaks in body text").
 *
 * `giftwrap_event_id` MUST be lowercase hex (64 chars). It flows into the
 * deep-link URI ONLY — not the visible body.
 *
 * Sets `out->withdraw_id` to a stable string the caller passes to
 * `g_application_withdraw_notification()`. On failure returns NULL and
 * leaves `*out` zeroed.
 */
GNotification *nostr_notify_build_dm(const char *giftwrap_event_id,
                                     const char *relay_url,
                                     NostrNotifyBuild *out);

/*
 * Build a NIP-29 group message notification (kinds 9-12).
 *
 * `group_display_name` — from cached kind 39000/39001 metadata, looked up
 *   by nostr_notify_group_key(relay_url, h_tag); NULL falls back to
 *   nostr_notify_group_fallback_title() ("<h> · <relay host>").
 * `h_tag` — group identifier (never NULL).
 * `event_id_hex` — 64-char lowercase hex event id.
 * `kind` — the event kind (9..12); encoded in the deep link.
 * `content_utf8` — plaintext content; may be markup-safe or not. This
 *   function truncates to 80 chars, escapes GTK markup and normalizes
 *   whitespace (Finding 14 asserts ≤80 chars). NULL = no preview
 *   (group_preview = false): the body is nostr_notify_group_fixed_body().
 * `relay_url` — relay the event arrived on (deep-link relay hint); may be
 *   NULL.
 */
GNotification *nostr_notify_build_group(const char *group_display_name,
                                        const char *h_tag,
                                        const char *event_id_hex,
                                        int kind,
                                        const char *content_utf8,
                                        const char *relay_url,
                                        NostrNotifyBuild *out);

/*
 * Free the withdraw_id string previously produced by a build_ call.
 */
void nostr_notify_build_dispose(NostrNotifyBuild *b);

/*
 * Open a `nostr:` deep link via the scheme default (nostr-dispatcher) or,
 * failing that, org.nostr.Dispatcher1.Open. Returns TRUE on successful
 * dispatch (fire-and-forget).
 */
bool nostr_notify_activate_deep_link(const char *nostr_uri);

/*
 * Testing helpers — exposed so test_notify_sub can assert opacity + preview
 * truncation without depending on the private GLib GNotification serialize
 * API. Callers own the returned strings (g_free).
 */
const char *nostr_notify_dm_title(void);
const char *nostr_notify_dm_opaque_body(void);
const char *nostr_notify_group_fixed_body(void);
char *nostr_notify_group_preview(const char *content_utf8);
char *nostr_notify_event_deep_link_uri(const char *event_id_hex, int kind,
                                       const char *relay_url);
char *nostr_notify_dm_deep_link_uri(const char *giftwrap_event_id,
                                    const char *relay_url);
char *nostr_notify_group_deep_link_uri(const char *event_id_hex, int kind,
                                       const char *relay_url);

/*
 * NIP-29 identifies a group by (relay, id): the same `h` on two relays can
 * be two different communities (forks). This key — "grp:<relay8>:<h>",
 * relay8 = first 8 hex of sha256(normalized relay URL), h bounded to 64
 * bytes — is the withdraw id of a group's notification and the key for any
 * per-group cache (39000 display names). NULL for a NULL/empty h.
 * (nostrc-a33z)
 */
char *nostr_notify_group_key(const char *relay_url, const char *h_tag);
/* Title when no display name is cached: "<h> · <relay host>" (just <h>
 * when the relay is unknown). Not markup-escaped. */
char *nostr_notify_group_fallback_title(const char *h_tag,
                                        const char *relay_url);

#endif /* NOSTR_NOTIFY_GNOTIFICATION_H */
