#include "gh-privacy-summary.h"

#include <string.h>

/* Marks translatable source strings for xgettext (--keyword=N_); tr() looks
 * them up in the application's text domain at run time, as
 * gh-message-status.c does. */
#define N_(text) (text)

static const gchar *
tr(const gchar *text)
{
  return g_dgettext(NULL, text);
}

/* ---- Strings table ------------------------------------------------------------------ */

/* Charter §1.4 non-goals that hold for every kind of conversation, and the
 * network facts of §2.2 ("Network observer", "Anyone"). */
static const gchar *const not_hidden_nostr =
  N_("That you use Nostr, and which relays you use: they are visible to your network.");
static const gchar *const not_hidden_timing =
  N_("Timing: someone watching many relays at once may link when messages are sent and "
     "received.");
static const gchar *const not_hidden_malware =
  N_("Apps running as you on this computer outside a sandbox can read what Groundhog shows.");
static const gchar *const disappearing_not_guaranteed =
  N_("Deleting everywhere: disappearing messages are deleted from this device, but relays and "
     "other people's apps may keep copies.");
static const gchar *const your_ip =
  N_("Relays you connect to can see your IP address.");

/* The Stored on This Device line: saved is the backend's full sentence; in
 * memory or with no store open, what Groundhog does instead, then what still
 * holds elsewhere (recovery, the relay's copy). */
static gchar *
storage_line(GhPrivacyStorage storage, const gchar *saved, const gchar *elsewhere)
{
  switch (storage) {
  case GH_PRIVACY_STORAGE_MEMORY:
    return g_strdup_printf("%s %s",
                           tr(N_("Groundhog keeps these messages in memory only and forgets "
                                 "them when it closes.")),
                           tr(elsewhere));
  case GH_PRIVACY_STORAGE_NONE:
    return g_strdup_printf("%s %s",
                           tr(N_("Groundhog isn't saving messages on this device right now.")),
                           tr(elsewhere));
  case GH_PRIVACY_STORAGE_SAVED:
  default:
    return g_strdup(tr(saved));
  }
}

static void
add_nip17(const GhPrivacyContext *context, GhPrivacySummary *summary, GStrvBuilder *visible,
          GStrvBuilder *unprotected)
{
  const gboolean self_only = context->n_people == 0;
  const gboolean one_peer = context->n_people == 1;
  const gchar *peer = context->peer_name && *context->peer_name ? context->peer_name : NULL;

  summary->end_to_end = TRUE;
  summary->icon_name = "channel-secure-symbolic";
  summary->heading = g_strdup(tr(N_("End-to-end encrypted")));
  if (self_only)
    summary->encrypted = g_strdup(tr(N_("Only you can read these notes. Relays can't see what "
                                        "you write.")));
  else if (one_peer && peer)
    summary->encrypted = g_strdup_printf(tr(N_("Only you and %s can read these messages. "
                                               "Relays can't see what you write or who sent "
                                               "it.")), peer);
  else if (one_peer)
    summary->encrypted = g_strdup(tr(N_("Only you and the other person can read these "
                                        "messages. Relays can't see what you write or who "
                                        "sent it.")));
  else
    summary->encrypted = g_strdup(tr(N_("Only the people in this conversation can read its "
                                        "messages. Relays can't see what anyone writes or who "
                                        "sent it.")));

  /* §2.2 column "NIP-17 conversation". */
  g_strv_builder_add(visible, tr(N_("Your message relays can see that you receive messages and "
                                    "roughly when, but not who sends them or what they say.")));
  if (one_peer && peer)
    g_strv_builder_take(visible, g_strdup_printf(tr(N_("%s's message relays can see when "
                                                       "messages arrive for them, but not who "
                                                       "sent them or what they say.")), peer));
  else if (one_peer)
    g_strv_builder_add(visible, tr(N_("The other person's message relays can see when "
                                      "messages arrive for them, but not who sent them or what "
                                      "they say.")));
  else if (!self_only)
    g_strv_builder_add(visible, tr(N_("Each person's message relays can see when messages "
                                      "arrive for them, but not who sent them or what they "
                                      "say.")));
  g_strv_builder_add(visible, tr(your_ip));
  g_strv_builder_add(visible, tr(N_("Anyone can see that you accept private messages, and which "
                                    "relays receive them.")));
  if (one_peer && peer)
    g_strv_builder_take(visible, g_strdup_printf(tr(N_("%s sees what you write, your public key "
                                                       "and the time you say you sent it. "
                                                       "Groundhog never tells anyone when you "
                                                       "read or type.")), peer));
  else if (one_peer)
    g_strv_builder_add(visible, tr(N_("The other person sees what you write, your public key "
                                      "and the time you say you sent it. Groundhog never tells "
                                      "anyone when you read or type.")));
  else if (!self_only)
    g_strv_builder_add(visible, tr(N_("The people in this conversation see what you write, "
                                      "your public key and the time you say you sent it. "
                                      "Groundhog never tells anyone when you read or type.")));

  if (one_peer && peer)
    g_strv_builder_take(unprotected, g_strdup_printf(tr(N_("What %s does with your messages: "
                                                           "they can copy, forward or "
                                                           "screenshot them.")), peer));
  else if (one_peer)
    g_strv_builder_add(unprotected, tr(N_("What the other person does with your messages: they "
                                          "can copy, forward or screenshot them.")));
  else if (!self_only)
    g_strv_builder_add(unprotected, tr(N_("What the others do with your messages: they can "
                                          "copy, forward or screenshot them.")));
  g_strv_builder_add(unprotected, tr(disappearing_not_guaranteed));
  g_strv_builder_add(unprotected, tr(not_hidden_timing));
  g_strv_builder_add(unprotected, tr(not_hidden_nostr));
  g_strv_builder_add(unprotected, tr(not_hidden_malware));

  /* D7: an honest single-device copy. */
  summary->storage = storage_line(context->storage,
                                  N_("Groundhog keeps these messages encrypted on this device "
                                     "only. Private messages can be downloaded again from your "
                                     "relays with your key."),
                                  N_("Private messages can be downloaded again from your relays "
                                     "with your key."));
}

static void
add_nip29(const GhPrivacyContext *context, GhPrivacySummary *summary, GStrvBuilder *visible,
          GStrvBuilder *unprotected)
{
  const gchar *host = context->relay_host && *context->relay_host ? context->relay_host : NULL;

  summary->end_to_end = FALSE;
  summary->icon_name = "dialog-warning-symbolic";
  summary->heading = g_strdup(tr(N_("Not end-to-end encrypted")));
  summary->encrypted = host
    ? g_strdup_printf(tr(N_("Messages are kept on %s without end-to-end encryption. Its "
                            "operators can read every message.")), host)
    : g_strdup(tr(N_("Messages are kept on the group's relay without end-to-end encryption. "
                     "Its operators can read every message.")));

  /* §2.2 column "NIP-29 relay group". */
  if (host) {
    g_strv_builder_take(visible, g_strdup_printf(tr(N_("The operators of %s can read every "
                                                       "message and see the member list, roles "
                                                       "and when people post.")), host));
    g_strv_builder_take(visible, g_strdup_printf(tr(N_("%s sees your public key and IP address "
                                                       "when you connect.")), host));
  } else {
    g_strv_builder_add(visible, tr(N_("The operators of the group's relay can read every "
                                      "message and see the member list, roles and when people "
                                      "post.")));
    g_strv_builder_add(visible, tr(N_("The group's relay sees your public key and IP address "
                                      "when you connect.")));
  }
  g_strv_builder_add(visible, tr(N_("Group members see what you write and your public key.")));
  g_strv_builder_add(visible, tr(N_("Discovery relays can see which groups you look up.")));

  g_strv_builder_add(unprotected, tr(N_("Message content: the relay's operators can read it.")));
  g_strv_builder_add(unprotected, tr(N_("Membership: the relay knows everyone in the group.")));
  g_strv_builder_add(unprotected, tr(N_("What members do with your messages: they can copy, "
                                        "forward or screenshot them.")));
  g_strv_builder_add(unprotected, tr(not_hidden_timing));
  g_strv_builder_add(unprotected, tr(not_hidden_nostr));
  g_strv_builder_add(unprotected, tr(not_hidden_malware));

  summary->storage = storage_line(context->storage,
                                  N_("Groundhog keeps a copy encrypted on this device. The relay "
                                     "keeps its own copy, which its operators can read."),
                                  N_("The relay keeps its own copy, which its operators can "
                                     "read."));
}

static void
add_mls(const GhPrivacyContext *context, GhPrivacySummary *summary, GStrvBuilder *visible,
        GStrvBuilder *unprotected)
{
  (void)context;
  summary->end_to_end = TRUE;
  summary->icon_name = "channel-secure-symbolic";
  summary->heading = g_strdup(tr(N_("End-to-end encrypted")));
  summary->encrypted = g_strdup(tr(N_("Messages are encrypted end to end for the group's current "
                                      "members only. Relays can't see what anyone writes or who "
                                      "wrote it.")));

  /* §2.2 column "MLS encrypted group". */
  g_strv_builder_add(visible, tr(N_("The group's relays store only encrypted messages. They see "
                                    "an id that links the group's messages, their sizes and "
                                    "times, and the IP addresses of members who connect. They "
                                    "don't see who the members are.")));
  g_strv_builder_add(visible, tr(N_("Invitations arrive like private messages: your message "
                                    "relays see their size and time.")));
  g_strv_builder_add(visible, tr(N_("Anyone can see that you can join encrypted groups, from the "
                                    "key packages you publish.")));
  /* W24 review H1: lookups only when you invite or ask to verify. */
  g_strv_builder_add(visible, tr(N_("Discovery relays see whose published keys you look up: "
                                    "when you invite someone, or ask to verify a member. "
                                    "Groundhog never asks the group's relays.")));
  g_strv_builder_add(visible, tr(N_("Members see what you write, your public key and the "
                                    "member list.")));

  /* §1.4: one device per MLS identity in v1; D7: no history recovery. */
  g_strv_builder_add(unprotected, tr(N_("Other devices: this group works on this device only.")));
  g_strv_builder_add(unprotected, tr(N_("Lost history: if this device's data is lost, the "
                                        "group's history can't be downloaded again.")));
  g_strv_builder_add(unprotected, tr(N_("What members do with your messages: they can copy, "
                                        "forward or screenshot them.")));
  g_strv_builder_add(unprotected, tr(disappearing_not_guaranteed));
  g_strv_builder_add(unprotected, tr(not_hidden_timing));
  g_strv_builder_add(unprotected, tr(not_hidden_nostr));
  g_strv_builder_add(unprotected, tr(not_hidden_malware));

  summary->storage = storage_line(context->storage,
                                  N_("Groundhog keeps group messages and keys encrypted on this "
                                     "device only. Encrypted group history can't be downloaded "
                                     "again."),
                                  N_("Encrypted group history can't be downloaded again."));
}

static gboolean
backend_is_valid(GhPrivacyBackend backend)
{
  return backend == GH_PRIVACY_BACKEND_NIP17 || backend == GH_PRIVACY_BACKEND_NIP29 ||
         backend == GH_PRIVACY_BACKEND_MLS;
}

gchar *
gh_privacy_summary_dup_subtitle(const GhPrivacyContext *context)
{
  g_return_val_if_fail(context != NULL, NULL);
  switch (context->backend) {
  case GH_PRIVACY_BACKEND_NIP17:
    if (!context->is_request)
      return g_strdup(tr(N_("Private · end-to-end encrypted")));
    /* A request's subject is text a stranger chose: secondary, quoted. */
    if (context->subject && *context->subject)
      return g_strdup_printf(tr(N_("“%s” · Message request · end-to-end encrypted")),
                             context->subject);
    return g_strdup(tr(N_("Message request · end-to-end encrypted")));
  case GH_PRIVACY_BACKEND_NIP29:
    return g_strdup(tr(N_("Relay group · not end-to-end encrypted")));
  case GH_PRIVACY_BACKEND_MLS:
    if (context->n_people == 0)
      return g_strdup(tr(N_("Encrypted group")));
    return g_strdup_printf(g_dngettext(NULL, "Encrypted group · %u member",
                                       "Encrypted group · %u members", context->n_people),
                           context->n_people);
  default:
    return NULL;
  }
}

const gchar *
gh_privacy_summary_kind(GhPrivacyBackend backend)
{
  switch (backend) {
  case GH_PRIVACY_BACKEND_NIP17:
    return tr(N_("Private conversation"));
  case GH_PRIVACY_BACKEND_NIP29:
    return tr(N_("Relay group, not end-to-end encrypted"));
  case GH_PRIVACY_BACKEND_MLS:
    return tr(N_("Encrypted group"));
  default:
    return NULL;
  }
}

GhPrivacySummary *
gh_privacy_summary_new(const GhPrivacyContext *context)
{
  g_return_val_if_fail(context != NULL, NULL);
  if (!backend_is_valid(context->backend))
    return NULL;
  GhPrivacySummary *summary = g_new0(GhPrivacySummary, 1);
  summary->backend = context->backend;
  summary->subtitle = gh_privacy_summary_dup_subtitle(context);
  g_autoptr(GStrvBuilder) visible = g_strv_builder_new();
  g_autoptr(GStrvBuilder) unprotected = g_strv_builder_new();
  switch (context->backend) {
  case GH_PRIVACY_BACKEND_NIP17:
    add_nip17(context, summary, visible, unprotected);
    break;
  case GH_PRIVACY_BACKEND_NIP29:
    add_nip29(context, summary, visible, unprotected);
    break;
  case GH_PRIVACY_BACKEND_MLS:
    add_mls(context, summary, visible, unprotected);
    break;
  }
  summary->visible = g_strv_builder_end(visible);
  summary->unprotected = g_strv_builder_end(unprotected);
  return summary;
}

void
gh_privacy_summary_free(GhPrivacySummary *summary)
{
  if (!summary)
    return;
  g_free(summary->subtitle);
  g_free(summary->heading);
  g_free(summary->encrypted);
  g_strfreev(summary->visible);
  g_strfreev(summary->unprotected);
  g_free(summary->storage);
  g_free(summary);
}

gchar *
gh_privacy_summary_to_text(const GhPrivacySummary *summary)
{
  g_return_val_if_fail(summary != NULL, NULL);
  static const gchar *const names[] = {
    [GH_PRIVACY_BACKEND_NIP17] = "NIP-17",
    [GH_PRIVACY_BACKEND_NIP29] = "NIP-29",
    [GH_PRIVACY_BACKEND_MLS] = "MLS",
  };
  GString *text = g_string_new(NULL);
  g_string_append_printf(text, "backend: %s\n", names[summary->backend]);
  g_string_append_printf(text, "end-to-end: %s\n", summary->end_to_end ? "yes" : "no");
  g_string_append_printf(text, "icon: %s\n", summary->icon_name);
  g_string_append_printf(text, "subtitle: %s\n", summary->subtitle);
  g_string_append_printf(text, "heading: %s\n", summary->heading);
  g_string_append_printf(text, "encrypted: %s\n", summary->encrypted);
  g_string_append(text, "what others can see:\n");
  for (gchar **line = summary->visible; *line; line++)
    g_string_append_printf(text, "- %s\n", *line);
  g_string_append(text, "what Groundhog can't protect:\n");
  for (gchar **line = summary->unprotected; *line; line++)
    g_string_append_printf(text, "- %s\n", *line);
  g_string_append_printf(text, "storage: %s\n", summary->storage);
  return g_string_free(text, FALSE);
}

/* ---- Safety codes --------------------------------------------------------------------- */

#define FINGERPRINT_DOMAIN "groundhog-fingerprint-v1"
#define FINGERPRINT_GROUPS (GH_PRIVACY_FINGERPRINT_DIGITS / GH_PRIVACY_FINGERPRINT_GROUP)

G_STATIC_ASSERT(GH_PRIVACY_FINGERPRINT_DIGITS % GH_PRIVACY_FINGERPRINT_GROUP == 0);
/* Five digest bytes per group of four digits, from one SHA-512 digest. */
G_STATIC_ASSERT(GH_PRIVACY_FINGERPRINT_GROUP == 4 && FINGERPRINT_GROUPS * 5 <= 64);

static gboolean
key_bytes(const gchar *hex, guint8 out[32])
{
  if (!hex || strlen(hex) != 64)
    return FALSE;
  for (guint i = 0; i < 32; i++) {
    gint hi = g_ascii_xdigit_value(hex[2 * i]);
    gint lo = g_ascii_xdigit_value(hex[2 * i + 1]);
    if (hi < 0 || lo < 0)
      return FALSE;
    out[i] = (guint8)(hi << 4 | lo);
  }
  return TRUE;
}

gchar *
gh_privacy_fingerprint(const gchar *pubkey)
{
  guint8 key[32];
  if (!key_bytes(pubkey, key))
    return NULL;
  guint8 digest[64];
  gsize length = sizeof digest;
  g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA512);
  g_checksum_update(checksum, (const guchar *)FINGERPRINT_DOMAIN, strlen(FINGERPRINT_DOMAIN));
  g_checksum_update(checksum, key, sizeof key);
  g_checksum_get_digest(checksum, digest, &length);
  /* Iterated, as Signal's safety numbers are: each try at a look-alike key
   * costs this many hashes on top of the key itself. */
  for (guint i = 1; i < GH_PRIVACY_FINGERPRINT_ITERATIONS; i++) {
    g_checksum_reset(checksum);
    g_checksum_update(checksum, digest, sizeof digest);
    g_checksum_update(checksum, key, sizeof key);
    length = sizeof digest;
    g_checksum_get_digest(checksum, digest, &length);
  }
  GString *text = g_string_sized_new(GH_PRIVACY_FINGERPRINT_DIGITS + FINGERPRINT_GROUPS);
  for (guint group = 0; group < FINGERPRINT_GROUPS; group++) {
    guint64 value = 0;
    for (guint i = 0; i < 5; i++)
      value = value << 8 | digest[group * 5 + i];
    if (group)
      g_string_append_c(text, ' ');
    g_string_append_printf(text, "%04u", (guint)(value % 10000));
  }
  return g_string_free(text, FALSE);
}

gchar *
gh_privacy_fingerprint_spoken(const gchar *fingerprint)
{
  g_return_val_if_fail(fingerprint != NULL, NULL);
  GString *text = g_string_new(NULL);
  for (const gchar *c = fingerprint; *c; c++) {
    if (*c == ' ') {
      g_string_append_c(text, ',');
      continue;
    }
    if (text->len)
      g_string_append_c(text, ' ');
    g_string_append_c(text, *c);
  }
  return g_string_free(text, FALSE);
}

gchar *
gh_privacy_format_key(const gchar *key)
{
  g_return_val_if_fail(key != NULL, NULL);
  GString *text = g_string_new(NULL);
  const gchar *rest = key;
  if (g_str_has_prefix(key, "npub1") && key[5]) {
    g_string_append(text, "npub1");
    rest = key + 5;
  }
  for (gsize i = 0; rest[i]; i++) {
    if (text->len && i % 4 == 0)
      g_string_append_c(text, ' ');
    g_string_append_c(text, rest[i]);
  }
  return g_string_free(text, FALSE);
}
