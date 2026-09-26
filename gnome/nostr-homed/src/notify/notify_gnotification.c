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
#include <string.h>

/* Fixed constant used in the DM body — the plan asserts fully opaque. */
static const char *const kDmOpaqueBody =
    "You have a new encrypted direct message.";
static const char *const kDmTitle = "Nostr message";
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
 *   "grp:<h_tag>"                — one coalesced notification per group.
 * The 8-hex prefix is a purely-internal coalescing key. It never appears
 * in the user-visible body — the DM body is the fixed opacity string.
 */
const char *nostr_notify_dm_title(void) { return kDmTitle; }
const char *nostr_notify_dm_opaque_body(void) { return kDmOpaqueBody; }

char *nostr_notify_group_preview(const char *content_utf8) {
  return truncate_utf8(content_utf8, 80);
}

char *nostr_notify_dm_deep_link_uri(const char *giftwrap_event_id) {
  if (!giftwrap_event_id) return NULL;
  return g_strdup_printf("nostr://open?event=%s", giftwrap_event_id);
}

char *nostr_notify_group_deep_link_uri(const char *h_tag,
                                       const char *event_id_hex) {
  if (!h_tag || !event_id_hex) return NULL;
  return g_strdup_printf("nostr://open?group=%s&event=%s", h_tag,
                         event_id_hex);
}

static char *dm_withdraw_id(const char *giftwrap_hex) {
  if (!giftwrap_hex || strlen(giftwrap_hex) < 8) return g_strdup("dm:x");
  return g_strdup_printf("dm:%.8s", giftwrap_hex);
}

static char *group_withdraw_id(const char *h_tag) {
  if (!h_tag) return g_strdup("grp:x");
  /* Bound length to a sane cap. */
  gchar *bounded = g_strndup(h_tag, 64);
  gchar *id = g_strdup_printf("grp:%s", bounded);
  g_free(bounded);
  return id;
}

GNotification *nostr_notify_build_dm(const char *giftwrap_event_id,
                                     NostrNotifyBuild *out) {
  if (!giftwrap_event_id || strlen(giftwrap_event_id) != 64) {
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

  /* Deep link: only the giftwrap event id (opaque handle). GNostr unwraps
   * and routes on click. */
  char *uri = g_strdup_printf("nostr://open?event=%s", giftwrap_event_id);
  g_notification_set_default_action_and_target(n, "app.open-in-gnostr",
                                               "s", uri);
  g_free(uri);

  if (out) out->withdraw_id = dm_withdraw_id(giftwrap_event_id);
  return n;
}

GNotification *nostr_notify_build_group(const char *group_display_name,
                                        const char *h_tag,
                                        const char *event_id_hex,
                                        const char *content_utf8,
                                        NostrNotifyBuild *out) {
  if (!h_tag || !event_id_hex || strlen(event_id_hex) != 64) {
    if (out) memset(out, 0, sizeof *out);
    return NULL;
  }

  const char *title =
      (group_display_name && *group_display_name) ? group_display_name : h_tag;
  g_autofree char *safe_title = g_markup_escape_text(title, -1);
  g_autofree char *body = truncate_utf8(content_utf8, 80);

  GNotification *n = g_notification_new(safe_title);
  g_notification_set_body(n, body ? body : "");
  g_notification_set_priority(n, G_NOTIFICATION_PRIORITY_NORMAL);
  g_notification_set_category(n, kGroupCategory);

  char *uri = g_strdup_printf("nostr://open?group=%s&event=%s", h_tag,
                              event_id_hex);
  g_notification_set_default_action_and_target(n, "app.open-in-gnostr",
                                               "s", uri);
  g_free(uri);

  if (out) out->withdraw_id = group_withdraw_id(h_tag);
  return n;
}

void nostr_notify_build_dispose(NostrNotifyBuild *b) {
  if (!b) return;
  g_free(b->withdraw_id);
  b->withdraw_id = NULL;
}

/*
 * Activate GNostr for a nostr:// URI.
 *
 * Preferred path: `Gio.DesktopAppInfo.launch_uris()` if a handler for the
 * `nostr:` scheme is registered — this works whether GNostr is running or
 * not. Fallback: session-bus call to `org.gnostr.Client` if it owns a bus
 * name (GNostr registers an application ID matching that name).
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

  /* Fallback: DBus Activate on `org.gnostr.Client` if it exists. */
  g_autoptr(GError) err = NULL;
  g_autoptr(GDBusConnection) bus =
      g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) return false;
  GVariant *params = g_variant_new_parsed(
      "([%s], @a{sv} {})", nostr_uri);
  /* org.freedesktop.Application.Open — the standard DBus activation call
   * for URL-carrying deep links. */
  g_dbus_connection_call(bus,
                         "org.gnostr.Client",
                         "/org/gnostr/Client",
                         "org.freedesktop.Application",
                         "Open",
                         params,
                         NULL,
                         G_DBUS_CALL_FLAGS_NO_AUTO_START,
                         2000,
                         NULL,
                         NULL,
                         NULL);
  /* We can't easily observe success without a callback; fire-and-forget
   * is what the plan asks for. */
  return true;
}
