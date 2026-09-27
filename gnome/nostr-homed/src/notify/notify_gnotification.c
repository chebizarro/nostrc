/*
 * notify_gnotification.c — GNotification builder implementation.
 *
 * See notify_gnotification.h for the design contract. Two rules matter:
 *
 *   1. DM bodies MUST be opaque. Kind-1059 gift wraps use a fresh random
 *      wrapper key per NIP-17; the outer pubkey is NOT the sender, so
 *      leaking any part of it (even a hex prefix) misleads users. The DM
 *      body is a fixed English string, no hex/pubkey substitution.
 *   2. Preview length is 80 chars for NIP-29 group messages (Finding 14),
 *      counted in UTF-8 characters and markup-escaped so a `<script>` in
 *      user content cannot bleed into GTK Pango markup.
 *
 * The withdraw_id encodes the "thread" for coalescing purposes: subsequent
 * events in the same thread reuse the id so the notifier can call
 * `g_application_withdraw_notification()` to update-in-place.
 */
#include "notify_gnotification.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/nip19/nip19.h" /* nostr_nip19_encode_nevent */

/* Fixed constant used in the DM body — the plan asserts fully opaque. */
static const char *const kDmOpaqueBody =
    "You have a new encrypted direct message.";
static const char *const kDmTitle = "Nostr message";
/* group_preview = false: like DMs, say only that something arrived. */
static const char *const kGroupFixedBody = "New message in this group.";
static const char *const kGroupCategory = "x-nostr.group";
static const char *const kDmCategory = "im.received";

/*
 * UTF-8-safe truncation to at most `max_chars` code points followed by
 * ellipsis if truncation happened. Returns a newly-allocated GLib string;
 * caller frees. `s_in` may be NULL.
 */
char *nostr_notify_group_preview(const char *content_utf8);
static char *truncate_utf8(const char *s_in, size_t max_chars) {
  if (!s_in) return g_strdup("");
  const char *p = s_in;
  size_t chars = 0;
  while (*p && chars < max_chars) {
    gunichar c = g_utf8_get_char_validated(p, -1);
    if (c == (gunichar)-1 || c == (gunichar)-2) {
      /* Invalid UTF-8 — cut before the offender rather than trust it. */
      break;
    }
    /* Fold ASCII control chars to space so a lone \n does not break the
     * one-line preview. */
    p = g_utf8_next_char(p);
    chars++;
  }
  size_t take = (size_t)(p - s_in);
  gchar *raw = g_strndup(s_in, take);
  /* Normalize whitespace: collapse \n/\r/\t to single space. */
  for (char *q = raw; *q; q++)
    if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
  /* Escape GTK markup so a preview-embedded `<b>` does not render. */
  gchar *escaped = g_markup_escape_text(raw, -1);
  g_free(raw);
  if (*p) {
    /* Truncation happened — append horizontal ellipsis. */
    gchar *with_ellipsis = g_strconcat(escaped, "\xe2\x80\xa6", NULL);
    g_free(escaped);
    return with_ellipsis;
  }
  return escaped;
}

/*
 * Withdraw-id for coalescing. Format:
 *   "dm:<giftwrap-8hex-prefix>"  — DMs coalesce per-gift-wrap (no thread key
 *                                  known without unwrapping; keep this
 *                                  ID INTERNAL — never rendered in body,
 *                                  never returned to callers as a "thread
 *                                  hint").
 *   "grp:<relay8>:<h_tag>"       — one coalesced notification per group,
 *                                  where a group is (relay, id): NIP-29
 *                                  lets the same id name different
 *                                  communities (forks) on different relays
 *                                  (nostrc-a33z). relay8 = first 8 hex of
 *                                  sha256(normalized relay URL), keeping
 *                                  the id bounded.
 * The 8-hex prefix is a purely-internal coalescing key. It never appears
 * in the user-visible body — the DM body is the fixed opacity string.
 */
const char *nostr_notify_dm_title(void) { return kDmTitle; }
const char *nostr_notify_dm_opaque_body(void) { return kDmOpaqueBody; }
const char *nostr_notify_group_fixed_body(void) { return kGroupFixedBody; }

char *nostr_notify_group_preview(const char *content_utf8) {
  return truncate_utf8(content_utf8, 80);
}

static bool is_hex64(const char *s) {
  if (!s || strlen(s) != 64) return false;
  for (const char *p = s; *p; p++)
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
  return true;
}

/*
 * NIP-21 deep link: `nostr:nevent1…` carrying the event id, the kind TLV
 * (so nostr-dispatcher can route without a fetch) and the relay the event
 * arrived on as the relay hint. No author TLV: for DMs the gift-wrap
 * pubkey is a random throwaway key, and for groups the handler learns the
 * author from the (signed) event it fetches. No `h` query parameter: the
 * NIP-29 `h` tag is inside the signed event and the relay hint is the
 * group relay, so the handler has the group context once it fetches;
 * NIP-21 defines no query parameters and dispatchers strip them anyway.
 */
char *nostr_notify_event_deep_link_uri(const char *event_id_hex, int kind,
                                       const char *relay_url) {
  if (!is_hex64(event_id_hex) || kind <= 0) return NULL;
  char *relays[1] = {NULL};
  size_t n = 0;
  if (relay_url && strlen(relay_url) <= 255 &&
      (g_str_has_prefix(relay_url, "wss://") || g_str_has_prefix(relay_url, "ws://")))
    relays[n++] = (char *)relay_url;
  NostrEventPointer ptr = {
      .id = (char *)event_id_hex,
      .relays = n ? relays : NULL,
      .relays_count = n,
      .author = NULL,
      .kind = kind,
  };
  char *bech = NULL;
  if (nostr_nip19_encode_nevent(&ptr, &bech) != 0 || !bech) return NULL;
  char *uri = g_strconcat("nostr:", bech, NULL);
  free(bech);
  return uri;
}

char *nostr_notify_dm_deep_link_uri(const char *giftwrap_event_id,
                                    const char *relay_url) {
  return nostr_notify_event_deep_link_uri(giftwrap_event_id, 1059, relay_url);
}

char *nostr_notify_group_deep_link_uri(const char *event_id_hex, int kind,
                                       const char *relay_url) {
  if (kind < 9 || kind > 12) return NULL;
  return nostr_notify_event_deep_link_uri(event_id_hex, kind, relay_url);
}

static char *dm_withdraw_id(const char *giftwrap_hex) {
  if (!giftwrap_hex || strlen(giftwrap_hex) < 8) return g_strdup("dm:x");
  return g_strdup_printf("dm:%.8s", giftwrap_hex);
}

/* Relay URLs that differ only in scheme/host case or a trailing slash are
 * the same relay. The path is kept as-is (it is case-sensitive). */
static char *normalize_relay_url(const char *relay_url) {
  if (!relay_url) return g_strdup("");
  gchar *u = g_strstrip(g_strdup(relay_url));
  size_t n = strlen(u);
  while (n > 0 && u[n - 1] == '/') u[--n] = '\0';
  char *sep = strstr(u, "://");
  char *auth_end = sep ? strchr(sep + 3, '/') : NULL;
  size_t lower_to = auth_end ? (size_t)(auth_end - u) : n;
  for (size_t i = 0; i < lower_to; i++) u[i] = g_ascii_tolower(u[i]);
  return u;
}

char *nostr_notify_group_key(const char *relay_url, const char *h_tag) {
  if (!h_tag || !*h_tag) return NULL;
  g_autofree char *norm = normalize_relay_url(relay_url);
  g_autofree char *digest =
      g_compute_checksum_for_string(G_CHECKSUM_SHA256, norm, -1);
  /* Bound length to a sane cap. */
  g_autofree char *bounded = g_strndup(h_tag, 64);
  return g_strdup_printf("grp:%.8s:%s", digest, bounded);
}

char *nostr_notify_group_fallback_title(const char *h_tag,
                                        const char *relay_url) {
  if (!h_tag) return NULL;
  g_autofree char *norm = normalize_relay_url(relay_url);
  const char *host = strstr(norm, "://");
  host = host ? host + 3 : norm;
  size_t host_len = strcspn(host, "/");
  if (host_len == 0) return g_strdup(h_tag);
  return g_strdup_printf("%s \xc2\xb7 %.*s", h_tag, (int)host_len, host);
}

static char *group_withdraw_id(const char *relay_url, const char *h_tag) {
  char *id = nostr_notify_group_key(relay_url, h_tag);
  return id ? id : g_strdup("grp:x");
}

GNotification *nostr_notify_build_dm(const char *giftwrap_event_id,
                                     const char *relay_url,
                                     NostrNotifyBuild *out) {
  char *uri = nostr_notify_dm_deep_link_uri(giftwrap_event_id, relay_url);
  if (!uri) {
    /* Malformed — refuse to build. Callers should log; the notifier
     * shouldn't advertise a broken deep link. */
    if (out) memset(out, 0, sizeof *out);
    return NULL;
  }

  /* Body is fixed opacity per §3.3 D3 Finding 14. Do NOT include the
   * giftwrap id, its prefix, or any pubkey byte in the body — it's a
   * random per-message key. */
  GNotification *n = g_notification_new(kDmTitle);
  g_notification_set_body(n, kDmOpaqueBody);
  g_notification_set_priority(n, G_NOTIFICATION_PRIORITY_NORMAL);
  g_notification_set_category(n, kDmCategory);

  /* Deep link: nevent for the gift wrap (opaque handle; kind 1059 routes it
   * to the DM handler). The handler unwraps on click. */
  g_notification_set_default_action_and_target(n, "app.open-in-gnostr",
                                               "s", uri);
  g_free(uri);

  if (out) out->withdraw_id = dm_withdraw_id(giftwrap_event_id);
  return n;
}

GNotification *nostr_notify_build_group(const char *group_display_name,
                                        const char *h_tag,
                                        const char *event_id_hex,
                                        int kind,
                                        const char *content_utf8,
                                        const char *relay_url,
                                        NostrNotifyBuild *out) {
  char *uri = h_tag ? nostr_notify_group_deep_link_uri(event_id_hex, kind, relay_url)
                    : NULL;
  if (!uri) {
    if (out) memset(out, 0, sizeof *out);
    return NULL;
  }

  /* Without cached 39000 metadata, name the relay next to the bare id:
   * the same id on two relays is two different groups (nostrc-a33z). */
  g_autofree char *fallback =
      (group_display_name && *group_display_name)
          ? NULL
          : nostr_notify_group_fallback_title(h_tag, relay_url);
  const char *title = fallback ? fallback : group_display_name;
  g_autofree char *safe_title = g_markup_escape_text(title, -1);
  g_autofree char *body = content_utf8 ? truncate_utf8(content_utf8, 80) : g_strdup(kGroupFixedBody);

  GNotification *n = g_notification_new(safe_title);
  g_notification_set_body(n, body ? body : "");
  g_notification_set_priority(n, G_NOTIFICATION_PRIORITY_NORMAL);
  g_notification_set_category(n, kGroupCategory);

  g_notification_set_default_action_and_target(n, "app.open-in-gnostr",
                                               "s", uri);
  g_free(uri);

  if (out) out->withdraw_id = group_withdraw_id(relay_url, h_tag);
  return n;
}

void nostr_notify_build_dispose(NostrNotifyBuild *b) {
  if (!b) return;
  g_free(b->withdraw_id);
  b->withdraw_id = NULL;
}

/*
 * Open a `nostr:` deep link.
 *
 * Preferred path: the registered `x-scheme-handler/nostr` default, which is
 * nostr-dispatcher (org.nostr.Dispatcher.desktop); it resolves the kind and
 * routes to the right app. Fallback: call org.nostr.Dispatcher1.Open on the
 * session bus directly (D-Bus activatable even when the MIME database has
 * not been refreshed yet).
 *
 * Fire-and-forget: the notifier does not observe the launched app's exit;
 * the user's click is complete after we dispatch.
 */
bool nostr_notify_activate_deep_link(const char *nostr_uri) {
  if (!nostr_uri || !*nostr_uri) return false;

  /* Try URI-scheme dispatch first. */
  g_autoptr(GAppInfo) handler =
      g_app_info_get_default_for_uri_scheme("nostr");
  if (handler) {
    GList *uris = NULL;
    uris = g_list_prepend(uris, (gpointer)nostr_uri);
    GError *err = NULL;
    gboolean ok = g_app_info_launch_uris(handler, uris, NULL, &err);
    g_list_free(uris);
    if (ok) return true;
    g_clear_error(&err);
    /* Fall through to DBus. */
  }

  /* Fallback: org.nostr.Dispatcher1.Open (auto-starts the dispatcher). */
  g_autoptr(GError) err = NULL;
  g_autoptr(GDBusConnection) bus =
      g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) return false;
  g_dbus_connection_call(bus,
                         "org.nostr.Dispatcher1",
                         "/org/nostr/Dispatcher1",
                         "org.nostr.Dispatcher1",
                         "Open",
                         g_variant_new("(s@a{sv})", nostr_uri,
                                       g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)),
                         NULL,
                         G_DBUS_CALL_FLAGS_NONE,
                         20000,
                         NULL,
                         NULL,
                         NULL);
  /* We can't easily observe success without a callback; fire-and-forget
   * is what the plan asks for. */
  return true;
}
