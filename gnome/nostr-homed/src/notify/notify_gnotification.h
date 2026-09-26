/*
 * notify_gnotification — GNotification builder + activation contract.
 *
 * The notifier owns GApplication ID `org.nostr.NotifyDaemon` (NOT
 * `org.gnostr.Client`, because a GNotification action belongs to the
 * sending GApplication; sharing the ID would break both suppression and
 * activation routing — per §3.3 D4 Finding 4).
 *
 * The default action is `app.open-in-gnostr` carrying a nostr:// URI:
 *   - DM  : `nostr://open?event=<hex64_giftwrap_event_id>`
 *   - Group: `nostr://open?group=<h_tag>&event=<hex64>`
 *
 * On activation the notifier launches GNostr via `Gio.DesktopAppInfo.
 * launch_uris("nostr://…")`; on failure (URI handler not registered) the
 * fallback path is a DBus call to `org.gnostr.Client` if it is running.
 * Registering the `nostr://` scheme in GNostr's `.desktop` file is a
 * required GNostr-side follow-up (tracked separately).
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
                                     NostrNotifyBuild *out);

/*
 * Build a NIP-29 group message notification (kinds 9-12).
 *
 * `group_display_name` — from cached kind 39000/39001 metadata; NULL falls
 *   back to the raw `h_tag`.
 * `h_tag` — group identifier (never NULL).
 * `event_id_hex` — 64-char lowercase hex event id.
 * `content_utf8` — plaintext content; may be markup-safe or not. This
 *   function truncates to 80 chars, escapes GTK markup and normalizes
 *   whitespace (Finding 14 asserts ≤80 chars).
 */
GNotification *nostr_notify_build_group(const char *group_display_name,
                                        const char *h_tag,
                                        const char *event_id_hex,
                                        const char *content_utf8,
                                        NostrNotifyBuild *out);

/*
 * Free the withdraw_id string previously produced by a build_ call.
 */
void nostr_notify_build_dispose(NostrNotifyBuild *b);

/*
 * Activate GNostr for a `nostr://` URI, either via GAppInfo dispatch or via
 * a DBus call to `org.gnostr.Client` if it owns a bus name. Returns TRUE
 * on successful dispatch (fire-and-forget).
 */
bool nostr_notify_activate_deep_link(const char *nostr_uri);

/*
 * Testing helpers — exposed so test_notify_sub can assert opacity + preview
 * truncation without depending on the private GLib GNotification serialize
 * API. Callers own the returned strings (g_free).
 */
const char *nostr_notify_dm_title(void);
const char *nostr_notify_dm_opaque_body(void);
char *nostr_notify_group_preview(const char *content_utf8);
char *nostr_notify_dm_deep_link_uri(const char *giftwrap_event_id);
char *nostr_notify_group_deep_link_uri(const char *h_tag,
                                       const char *event_id_hex);

#endif /* NOSTR_NOTIFY_GNOTIFICATION_H */
