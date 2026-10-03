#include "gh-nip17-envelope.h"
#include "gh-identity.h"
#include "gh-nip17-inbox.h"
#include "gh-signer.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip17/nip17.h"
#include "nostr/nip59/nip59.h"
#include "secure_buf.h"

#include <stdlib.h>
#include <string.h>

/* Bounded redraws of a random value that repeated one already used by this
 * envelope (a seal or wrap created_at, an ephemeral key): unlinkability is
 * checked, not left to chance (W17). */
#define MAX_REDRAWS 16

typedef struct {
  guint64 generation;
  gchar *sender;
  gchar *sender_npub;
  GStrv recipients;       /* receivers besides the sender; empty for a note to self */
  guint n_recipients;
  gchar *rumor_json;
  gchar *ciphertext;
  GPtrArray *wraps;       /* gchar *: the finished destinations' signed wraps */
  GHashTable *outer_keys; /* the ephemeral pubkeys those wraps use */
  GArray *seal_times;     /* gint64: their seals' created_at */
  GArray *wrap_times;     /* gint64: their created_at */
  int64_t seal_at;
  /* 0 .. n_recipients - 1: that recipient; n_recipients: the sender's own
   * copy (a note to self's only one). */
  guint destination;
  gboolean expiring;      /* the rumor expires: outer holds the layers' expirations */
  GhNip17RoomExpiration outer;
  gboolean no_self_copy;  /* a Welcome (kind 444): the recipient's wrap only */
} Build;

static void
build_free(gpointer data)
{
  Build *build = data;
  g_free(build->sender);
  g_free(build->sender_npub);
  g_strfreev(build->recipients);
  g_free(build->rumor_json);
  g_free(build->ciphertext);
  g_ptr_array_unref(build->wraps);
  g_hash_table_unref(build->outer_keys);
  g_array_unref(build->seal_times);
  g_array_unref(build->wrap_times);
  g_free(build);
}

void
gh_nip17_envelope_free(GhNip17Envelope *envelope)
{
  if (!envelope) return;
  g_free(envelope->rumor_json);
  g_free(envelope->recipient_wrap_json);
  g_free(envelope->sender_wrap_json);
  g_strfreev(envelope->recipients);
  g_strfreev(envelope->recipient_wraps);
  g_free(envelope);
}

static gboolean
hex64(const gchar *value)
{
  if (!value || strlen(value) != 64) return FALSE;
  for (const gchar *p = value; *p; p++)
    if (!g_ascii_isxdigit(*p)) return FALSE;
  return TRUE;
}

static const gchar *
destination(Build *build)
{
  return build->destination < build->n_recipients ? build->recipients[build->destination]
                                                  : build->sender;
}

/* The current destination's seal and wrap expirations (0: none). */
static GhNip17LayerExpiration
layer_expiration(Build *build)
{
  if (!build->expiring)
    return (GhNip17LayerExpiration){ 0, 0 };
  return build->destination < build->n_recipients ? build->outer.recipients[build->destination]
                                                  : build->outer.self_copy;
}

static gboolean
time_used(GArray *times, gint64 value)
{
  for (guint i = 0; i < times->len; i++)
    if (g_array_index(times, gint64, i) == value)
      return TRUE;
  return FALSE;
}

static NostrTag *
expiration_tag_new(gint64 expiration)
{
  gchar value[24];
  g_snprintf(value, sizeof value, "%" G_GINT64_FORMAT, expiration);
  return nostr_tag_new("expiration", value, NULL);
}

/* A canonical NIP-40 value: decimal digits, no sign or leading zero, in
 * [1, GH_NIP17_MAX_EXPIRATION] (the inbox's rule). */
static gboolean
parse_expiration(const gchar *text, gint64 *out)
{
  guint64 value = 0;
  if (!text || !g_ascii_isdigit(text[0]) || text[0] == '0')
    return FALSE;
  for (const gchar *p = text; *p; p++)
    if (!g_ascii_isdigit(*p))
      return FALSE;
  if (!g_ascii_string_to_unsigned(text, 10, 1, GH_NIP17_MAX_EXPIRATION, &value, NULL))
    return FALSE;
  *out = (gint64)value;
  return TRUE;
}

/* The event's single expiration tag (0 when it has none). FALSE for a
 * second one or one that is not exactly ["expiration", value]; with
 * only_expiration, also for any other tag (the seal's rule). */
static gboolean
event_expiration(const NostrEvent *event, gboolean only_expiration, gint64 *out)
{
  NostrTags *tags = nostr_event_get_tags(event);
  gint64 found = 0;
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get_key(tag), "expiration") != 0) {
      if (only_expiration)
        return FALSE;
      continue;
    }
    if (found || nostr_tag_size(tag) != 2 || !parse_expiration(nostr_tag_get(tag, 1), &found))
      return FALSE;
  }
  *out = found;
  return TRUE;
}

static gboolean
current(GTask *task)
{
  GhAccountController *accounts = g_task_get_source_object(task);
  Build *build = g_task_get_task_data(task);
  GCancellable *cancel = g_task_get_cancellable(task);
  return (!cancel || !g_cancellable_is_cancelled(cancel)) &&
         gh_account_controller_is_current(accounts, build->generation) &&
         g_strcmp0(gh_account_controller_get_active_npub(accounts),
                   build->sender_npub) == 0;
}

static void
fail(GTask *task, GError *error)
{
  if (!current(task)) {
    g_clear_error(&error);
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "NIP-17 account generation or request was cancelled");
  }
  g_task_return_error(task, error);
  g_object_unref(task);
}

static void
fail_local(GTask *task, const gchar *message)
{
  fail(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, message));
}

static void encrypt_done(GObject *source, GAsyncResult *result, gpointer data);
static void sign_done(GObject *source, GAsyncResult *result, gpointer data);

static void
begin_encrypt(GTask *task)
{
  GhAccountController *accounts = g_task_get_source_object(task);
  Build *build = g_task_get_task_data(task);
  if (!current(task)) {
    fail_local(task, "NIP-17 account changed");
    return;
  }
  gh_account_controller_nip44_encrypt_with_cancellable_async(
    accounts, build->rumor_json, destination(build), g_task_get_cancellable(task),
    encrypt_done, task);
}

/* A randomized seal created_at (NIP-59) that no earlier seal of this
 * envelope has. */
static gboolean
draw_seal_time(Build *build)
{
  for (guint i = 0; i < MAX_REDRAWS; i++) {
    if (nostr_nip59_randomize_timestamp(0, 0, &build->seal_at) != NIP59_OK)
      return FALSE;
    if (!time_used(build->seal_times, build->seal_at))
      return TRUE;
  }
  return FALSE;
}

static void
encrypt_done(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  Build *build = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  gchar *ciphertext = gh_account_controller_nip44_finish(result, &error);
  (void)source;
  if (error) { fail(task, g_steal_pointer(&error)); return; }
  if (!current(task)) { g_free(ciphertext); fail_local(task, "NIP-17 account changed"); return; }
  if (!ciphertext || !draw_seal_time(build)) {
    g_free(ciphertext);
    fail_local(task, "Could not create private NIP-17 seal");
    return;
  }
  g_free(build->ciphertext);
  build->ciphertext = ciphertext;

  NostrEvent *seal = nostr_event_new();
  if (!seal) { fail_local(task, "Could not allocate NIP-17 seal"); return; }
  nostr_event_set_kind(seal, 13);
  nostr_event_set_pubkey(seal, build->sender);
  nostr_event_set_content(seal, build->ciphertext);
  nostr_event_set_created_at(seal, build->seal_at);
  gint64 seal_expiration = layer_expiration(build).seal;
  nostr_event_set_tags(seal, seal_expiration ? nostr_tags_new(1, expiration_tag_new(seal_expiration))
                                             : nostr_tags_new(0));
  char *unsigned_json = nostr_event_serialize_compact(seal);
  nostr_event_free(seal);
  if (!unsigned_json) { fail_local(task, "Could not serialize NIP-17 seal"); return; }
  if (!current(task)) {
    free(unsigned_json);
    fail_local(task, "NIP-17 account changed");
    return;
  }
  GhAccountController *accounts = g_task_get_source_object(task);
  gh_account_controller_sign_with_cancellable_async(
    accounts, unsigned_json, g_task_get_cancellable(task), sign_done, task);
  free(unsigned_json);
}

/* One gift wrap of seal to receiver under a fresh ephemeral key (NIP-59:
 * never reused) and a randomized created_at, with the wrap expiration when
 * the message disappears. */
static NostrEvent *
wrap_once(NostrEvent *seal, const gchar *receiver, gint64 wrap_expiration)
{
  if (!wrap_expiration)
    return nostr_nip59_wrap(seal, receiver, NULL);
  /* The outer expiration is part of what the ephemeral key signs, so this
   * wrap uses a key of its own and is signed again with the tag added. */
  NostrEvent *wrap = NULL;
  char *ephemeral_sk = NULL, *ephemeral_pk = NULL;
  if (nostr_nip59_create_ephemeral_key(&ephemeral_sk, &ephemeral_pk) == NIP59_OK)
    wrap = nostr_nip59_wrap(seal, receiver, ephemeral_sk);
  if (wrap) {
    nostr_tags_append(nostr_event_get_tags(wrap), expiration_tag_new(wrap_expiration));
    if (nostr_event_sign(wrap, ephemeral_sk) != 0)
      g_clear_pointer(&wrap, nostr_event_free);
  }
  if (ephemeral_sk) {
    secure_wipe(ephemeral_sk, strlen(ephemeral_sk));
    free(ephemeral_sk);
  }
  free(ephemeral_pk);
  return wrap;
}

/* Whether a wrap could be linked to an earlier one of this message by its
 * outer key or created_at, or shows the sender. */
static gboolean
wrap_linkable(Build *build, NostrEvent *wrap)
{
  const gchar *pubkey = nostr_event_get_pubkey(wrap);
  return !pubkey || g_strcmp0(pubkey, build->sender) == 0 ||
         g_hash_table_contains(build->outer_keys, pubkey) ||
         time_used(build->wrap_times, nostr_event_get_created_at(wrap));
}

static void
envelope_return(GTask *task)
{
  Build *build = g_task_get_task_data(task);
  guint n = build->n_recipients;
  GhNip17Envelope *envelope = g_new0(GhNip17Envelope, 1);
  envelope->rumor_json = g_steal_pointer(&build->rumor_json);
  envelope->recipients = g_strdupv(build->recipients);
  envelope->recipient_wraps = g_new0(gchar *, n + 1);
  for (guint i = 0; i < n; i++)
    envelope->recipient_wraps[i] = g_strdup(g_ptr_array_index(build->wraps, i));
  envelope->recipient_wrap_json = n ? g_strdup(envelope->recipient_wraps[0]) : NULL;
  envelope->sender_wrap_json = build->wraps->len > n ? g_strdup(g_ptr_array_index(build->wraps, n))
                                                     : NULL;
  g_task_return_pointer(task, envelope, (GDestroyNotify)gh_nip17_envelope_free);
  g_object_unref(task);
}

static void
sign_done(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  Build *build = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json = gh_account_controller_sign_finish(result, &error);
  (void)source;
  if (error) { fail(task, g_steal_pointer(&error)); return; }
  if (!current(task)) { fail_local(task, "NIP-17 account changed"); return; }

  const GhNip17LayerExpiration expiration = layer_expiration(build);
  gint64 signed_expiration = -1;
  NostrEvent *seal = nostr_event_new();
  if (!seal || nostr_event_deserialize_compact(seal, signed_json, NULL) != 1 ||
      nostr_event_validate(seal, NULL) != NOSTR_EVENT_VALIDATION_OK ||
      nostr_event_get_kind(seal) != 13 ||
      g_strcmp0(nostr_event_get_pubkey(seal), build->sender) != 0 ||
      g_strcmp0(nostr_event_get_content(seal), build->ciphertext) != 0 ||
      nostr_event_get_created_at(seal) != build->seal_at ||
      !event_expiration(seal, TRUE, &signed_expiration) ||
      signed_expiration != expiration.seal) {
    if (seal) nostr_event_free(seal);
    fail(task, g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_INVALID_RESULT,
                                   "Signer returned an invalid NIP-17 seal"));
    return;
  }
  NostrEvent *wrap = NULL;
  for (guint i = 0; i < MAX_REDRAWS && !wrap; i++) {
    wrap = wrap_once(seal, destination(build), expiration.wrap);
    if (wrap && wrap_linkable(build, wrap))
      g_clear_pointer(&wrap, nostr_event_free);
  }
  nostr_event_free(seal);
  char *wrapped_for = wrap ? nostr_nip59_get_recipient(wrap) : NULL;
  gboolean correct_recipient = g_strcmp0(wrapped_for, destination(build)) == 0;
  free(wrapped_for);
  if (!wrap || !nostr_nip59_validate_gift_wrap(wrap) || !correct_recipient) {
    if (wrap) nostr_event_free(wrap);
    fail_local(task, "Could not create private NIP-17 gift wrap");
    return;
  }
  char *json = nostr_event_serialize_compact(wrap);
  if (!json || !current(task)) {
    nostr_event_free(wrap);
    free(json);
    fail_local(task, "NIP-17 gift wrap was cancelled or could not serialize");
    return;
  }
  gint64 seal_at = build->seal_at, wrap_at = nostr_event_get_created_at(wrap);
  g_array_append_val(build->seal_times, seal_at);
  g_array_append_val(build->wrap_times, wrap_at);
  g_hash_table_add(build->outer_keys, g_strdup(nostr_event_get_pubkey(wrap)));
  nostr_event_free(wrap);
  g_ptr_array_add(build->wraps, g_strdup(json));
  free(json);
  if (build->destination < build->n_recipients) {
    build->destination++; /* the next recipient, then the self-copy */
    if (build->destination < build->n_recipients || !build->no_self_copy) {
      begin_encrypt(task);
      return;
    }
  }
  envelope_return(task);
}

/* G21: a kind-15 file message (gh-nip17-file.h) is built, sealed and sent
 * exactly like a kind-14 chat message, to one person or a room. */
static gboolean
dm_kind(gint kind)
{
  return kind == 14 || kind == GH_NIP17_FILE_KIND || kind == 7 || kind == 5;
}

/* A canonical rumor of kind (14 or 15) with its id, one "p" tag per
 * recipient (lowercase, validated), then extra (nullable: (key, value)
 * GStrv pairs, e.g. a file's tags), then the expiration; created_at 0 means
 * now, expires_at 0 never. */
static gchar *
rumor_new_kind(const gchar *sender, const gchar *const *recipients, gint kind,
               const gchar *content, GPtrArray *extra, gint64 created_at, gint64 expires_at,
               gchar **out_id, GError **error)
{
  NostrEvent *rumor = nostr_nip17_create_rumor(sender, recipients[0], content, created_at);
  if (!rumor) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not create NIP-17 rumor");
    return NULL;
  }
  nostr_event_set_kind(rumor, kind);
  for (guint i = 1; recipients[i]; i++)
    nostr_tags_append(nostr_event_get_tags(rumor), nostr_tag_new("p", recipients[i], NULL));
  for (guint i = 0; extra && i < extra->len; i++) {
    const gchar *const *pair = g_ptr_array_index(extra, i);
    nostr_tags_append(nostr_event_get_tags(rumor), nostr_tag_new(pair[0], pair[1], NULL));
  }
  if (expires_at)
    nostr_tags_append(nostr_event_get_tags(rumor), expiration_tag_new(expires_at));
  rumor->id = nostr_event_get_id(rumor);
  if (!rumor->id || nostr_event_validate_id(rumor, NULL) != NOSTR_EVENT_VALIDATION_OK ||
      rumor->sig != NULL) {
    nostr_event_free(rumor);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Could not canonicalize NIP-17 rumor");
    return NULL;
  }
  char *json = nostr_event_serialize_compact(rumor);
  if (out_id && json)
    *out_id = g_strdup(rumor->id);
  nostr_event_free(rumor);
  if (!json) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not serialize NIP-17 rumor");
    return NULL;
  }
  gchar *copy = g_strdup(json);
  free(json);
  return copy;
}

/* A canonical kind-14 rumor (see rumor_new_kind()). */
static gchar *
rumor_new(const gchar *sender, const gchar *const *recipients, const gchar *content,
          gint64 created_at, gint64 expires_at, gchar **out_id, GError **error)
{
  return rumor_new_kind(sender, recipients, 14, content, NULL, created_at, expires_at, out_id,
                        error);
}

/* The recipients of a room as rumor "p" tags: 1 to the limit of distinct
 * lowercase keys, the sender only alone; NULL (and a reason) otherwise. */
static GStrv
room_recipients(const gchar *sender, const gchar *const *recipients, const gchar **reason)
{
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  guint n = 0;
  gboolean has_sender = FALSE;
  for (; recipients && recipients[n]; n++) {
    if (!hex64(recipients[n])) {
      *reason = "Sender and recipient pubkeys, UTF-8 content and a time are required";
      return NULL;
    }
    gchar *lower = g_ascii_strdown(recipients[n], -1);
    has_sender |= g_strcmp0(lower, sender) == 0;
    if (!g_hash_table_add(seen, lower)) {
      *reason = "A NIP-17 message names each recipient once";
      return NULL;
    }
    g_strv_builder_add(builder, lower);
  }
  if (n == 0 || n > GH_NIP17_MAX_SEND_RECIPIENTS || (has_sender && n > 1)) {
    *reason = n == 0 ? "Sender and recipient pubkeys, UTF-8 content and a time are required"
                     : "A NIP-17 message goes to 1 to 10 other people, or only to yourself";
    return NULL;
  }
  return g_strv_builder_end(builder);
}

gchar *
gh_nip17_rumor_new(const gchar *sender_pubkey_hex, const gchar *recipient_pubkey_hex,
                   const gchar *content, gint64 created_at, gchar **out_rumor_id,
                   GError **error)
{
  return gh_nip17_rumor_new_expiring(sender_pubkey_hex, recipient_pubkey_hex, content,
                                     created_at, 0, out_rumor_id, error);
}

gchar *
gh_nip17_rumor_new_expiring(const gchar *sender_pubkey_hex, const gchar *recipient_pubkey_hex,
                            const gchar *content, gint64 created_at, gint64 expires_at,
                            gchar **out_rumor_id, GError **error)
{
  const gchar *const recipients[] = { recipient_pubkey_hex, NULL };
  return gh_nip17_rumor_new_room(sender_pubkey_hex, recipient_pubkey_hex ? recipients : NULL,
                                 content, created_at, expires_at, out_rumor_id, error);
}

/* The checks every room rumor passes, kind 14 or 15: a sender, 1 to the
 * limit of recipients (room_recipients()), UTF-8 content, a time and an
 * expiration after it. The lowercase sender and recipients, or NULL. */
static GStrv
room_rumor_check(const gchar *sender_pubkey_hex, const gchar *const *recipients,
                 const gchar *content, gint64 created_at, gint64 expires_at,
                 gchar **out_sender, GError **error)
{
  if (!hex64(sender_pubkey_hex) || !recipients || !recipients[0] || !content || !*content ||
      !g_utf8_validate(content, -1, NULL) || created_at <= 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Sender and recipient pubkeys, UTF-8 content and a time are required");
    return NULL;
  }
  if (expires_at != 0 && (expires_at <= created_at || expires_at > GH_NIP17_MAX_EXPIRATION)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A NIP-17 rumor expires after it was created");
    return NULL;
  }
  g_autofree gchar *sender = g_ascii_strdown(sender_pubkey_hex, -1);
  const gchar *reason = NULL;
  GStrv room = room_recipients(sender, recipients, &reason);
  if (!room) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, reason);
    return NULL;
  }
  *out_sender = g_steal_pointer(&sender);
  return room;
}

gchar *
gh_nip17_rumor_new_room(const gchar *sender_pubkey_hex, const gchar *const *recipients,
                        const gchar *content, gint64 created_at, gint64 expires_at,
                        gchar **out_rumor_id, GError **error)
{
  g_autofree gchar *sender = NULL;
  g_auto(GStrv) room = room_rumor_check(sender_pubkey_hex, recipients, content, created_at,
                                        expires_at, &sender, error);
  if (!room)
    return NULL;
  return rumor_new(sender, (const gchar *const *)room, content, created_at, expires_at,
                   out_rumor_id, error);
}

gchar *
gh_nip17_rumor_new_file_room(const gchar *sender_pubkey_hex, const gchar *const *recipients,
                             const GhNip17File *file, gint64 created_at, gint64 expires_at,
                             gchar **out_rumor_id, GError **error)
{
  g_autoptr(GPtrArray) tags = file ? gh_nip17_file_dup_tags(file, error) : NULL;
  if (!tags) {
    if (!file)
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                          "A file message needs a complete encrypted file");
    return NULL;
  }
  g_autofree gchar *sender = NULL;
  g_auto(GStrv) room = room_rumor_check(sender_pubkey_hex, recipients, file->url, created_at,
                                        expires_at, &sender, error);
  if (!room)
    return NULL;
  return rumor_new_kind(sender, (const gchar *const *)room, GH_NIP17_FILE_KIND, file->url, tags,
                        created_at, expires_at, out_rumor_id, error);
}

gchar *
gh_nip17_rumor_new_reaction_room(const gchar *sender_pubkey_hex,
                                 const gchar *const *recipients,
                                 const gchar *emoji,
                                 const gchar *target_rumor_id,
                                 const gchar *target_kind_str,
                                 gint64 created_at, gchar **out_rumor_id,
                                 GError **error)
{
  if (!emoji || !*emoji || !g_utf8_validate(emoji, -1, NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A reaction needs a non-empty UTF-8 emoji");
    return NULL;
  }
  if (!target_rumor_id || !*target_rumor_id) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A reaction needs a target event id");
    return NULL;
  }
  g_autofree gchar *sender = NULL;
  g_auto(GStrv) room = room_rumor_check(sender_pubkey_hex, recipients, emoji, created_at,
                                        0, &sender, error);
  if (!room)
    return NULL;
  g_autoptr(GPtrArray) extra = g_ptr_array_new();
  const gchar *e_pair[] = { "e", target_rumor_id, NULL };
  g_ptr_array_add(extra, (gpointer)e_pair);
  if (target_kind_str && *target_kind_str) {
    const gchar *k_pair[] = { "k", target_kind_str, NULL };
    g_ptr_array_add(extra, (gpointer)k_pair);
  }
  return rumor_new_kind(sender, (const gchar *const *)room, 7, emoji, extra,
                        created_at, 0, out_rumor_id, error);
}

gchar *
gh_nip17_rumor_new_deletion_room(const gchar *sender_pubkey_hex,
                                 const gchar *const *recipients,
                                 const gchar *target_event_id,
                                 gint64 created_at, gchar **out_rumor_id,
                                 GError **error)
{
  if (!target_event_id || !*target_event_id) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A deletion needs a target event id");
    return NULL;
  }
  /* kind-5 content is a reason (NIP-09), may be empty; room_rumor_check
   * requires non-empty content, so pass a placeholder. */
  g_autofree gchar *sender = NULL;
  g_auto(GStrv) room = room_rumor_check(sender_pubkey_hex, recipients, "delete", created_at,
                                        0, &sender, error);
  if (!room)
    return NULL;
  g_autoptr(GPtrArray) extra = g_ptr_array_new();
  const gchar *e_pair[] = { "e", target_event_id, NULL };
  g_ptr_array_add(extra, (gpointer)e_pair);
  return rumor_new_kind(sender, (const gchar *const *)room, 5, "", extra,
                        created_at, 0, out_rumor_id, error);
}

gboolean
gh_nip17_rumor_get_expiration(const gchar *rumor_json, gint64 *out_created_at,
                              gint64 *out_expires_at)
{
  if (!rumor_json || !g_utf8_validate(rumor_json, -1, NULL))
    return FALSE;
  NostrEvent *rumor = nostr_event_new();
  gint64 expires_at = 0;
  gboolean ok = rumor && nostr_event_deserialize_compact(rumor, rumor_json, NULL) == 1 &&
                dm_kind(nostr_event_get_kind(rumor)) && !rumor->sig && rumor->id &&
                nostr_event_validate_id(rumor, NULL) == NOSTR_EVENT_VALIDATION_OK &&
                nostr_event_get_created_at(rumor) > 0 &&
                event_expiration(rumor, FALSE, &expires_at);
  if (ok && out_created_at)
    *out_created_at = nostr_event_get_created_at(rumor);
  if (ok && out_expires_at)
    *out_expires_at = expires_at;
  if (rumor)
    nostr_event_free(rumor);
  return ok;
}

/* Validates the active account and recipients, then encrypts rumor_json
 * (owned) towards each recipient and the sender (only to the sender for a
 * note to self); outer (nullable) holds the outer layers' expirations of a
 * rumor that expires. */
static void
start_with_rumor(GTask *task, const gchar *const *recipients, gboolean self_only,
                 gchar *rumor_json, const GhNip17RoomExpiration *outer, gboolean no_self_copy)
{
  GhAccountController *accounts = g_task_get_source_object(task);
  const gchar *npub = gh_account_controller_get_active_npub(accounts);
  g_autofree gchar *sender = npub ? gh_identity_pubkey_hex(npub) : NULL;
  const gchar *invalid = NULL;
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  guint n = 0;
  if (!sender)
    invalid = "Active account, recipient pubkey and UTF-8 content are required";
  for (; !invalid && !self_only && recipients && recipients[n]; n++) {
    gchar *lower = hex64(recipients[n]) ? g_ascii_strdown(recipients[n], -1) : NULL;
    if (!lower)
      invalid = "Active account, recipient pubkey and UTF-8 content are required";
    else if (g_strcmp0(sender, lower) == 0)
      invalid = "Recipient must differ from the selected sender";
    else if (g_hash_table_contains(seen, lower))
      invalid = "A NIP-17 message names each recipient once";
    else
      g_strv_builder_add(builder, lower);
    if (lower && !invalid)
      g_hash_table_add(seen, lower);
    else
      g_free(lower);
  }
  if (!invalid && !self_only && (n == 0 || n > GH_NIP17_MAX_SEND_RECIPIENTS))
    invalid = "A NIP-17 message goes to 1 to 10 other people, or only to yourself";
  if (!invalid && !rumor_json)
    invalid = "Active account, recipient pubkey and UTF-8 content are required";
  if (invalid) {
    g_free(rumor_json);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "%s", invalid);
    g_object_unref(task);
    return;
  }
  Build *build = g_new0(Build, 1);
  build->generation = gh_account_controller_get_generation(accounts);
  build->sender = g_strdup(sender);
  build->sender_npub = g_strdup(npub);
  build->recipients = g_strv_builder_end(builder); /* empty for a note to self */
  build->n_recipients = g_strv_length(build->recipients);
  build->wraps = g_ptr_array_new_with_free_func(g_free);
  build->outer_keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  build->seal_times = g_array_new(FALSE, FALSE, sizeof(gint64));
  build->wrap_times = g_array_new(FALSE, FALSE, sizeof(gint64));
  /* The recipients first, then the self-copy (a note to self's only wrap). */
  build->destination = 0;
  build->rumor_json = rumor_json;
  build->expiring = outer != NULL;
  if (outer)
    build->outer = *outer;
  build->no_self_copy = no_self_copy && !self_only;
  g_task_set_task_data(task, build, build_free);
  begin_encrypt(task);
}

static void
build_start(GhAccountController *accounts, const gchar *recipient_pubkey_hex,
            gboolean self_only, const gchar *content, GCancellable *cancellable,
            GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new(accounts, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_nip17_envelope_build_async);
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
  const gchar *npub = gh_account_controller_get_active_npub(accounts);
  g_autofree gchar *sender = npub ? gh_identity_pubkey_hex(npub) : NULL;
  const gchar *recipient = self_only ? sender : recipient_pubkey_hex;
  if (!sender || !hex64(recipient) || !content || !*content ||
      !g_utf8_validate(content, -1, NULL)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Active account, recipient pubkey and UTF-8 content are required");
    g_object_unref(task);
    return;
  }
  g_autofree gchar *lower = g_ascii_strdown(recipient, -1);
  const gchar *const recipients[] = { lower, NULL };
  gchar *rumor_json = NULL;
  if (self_only || g_strcmp0(sender, lower) != 0) {
    g_autoptr(GError) error = NULL;
    rumor_json = rumor_new(sender, recipients, content, 0, 0, NULL, &error);
    if (!rumor_json) {
      g_task_return_error(task, g_steal_pointer(&error));
      g_object_unref(task);
      return;
    }
  }
  /* Without a rumor (recipient == sender) this reports that recipient. */
  start_with_rumor(task, recipients, self_only, rumor_json, NULL, FALSE);
}

gchar *
gh_nip17_rumor_get_recipient(const gchar *rumor_json, const gchar *sender)
{
  g_auto(GStrv) recipients = gh_nip17_rumor_dup_recipients(rumor_json, sender);
  return recipients && g_strv_length(recipients) == 1 ? g_strdup(recipients[0]) : NULL;
}

GStrv
gh_nip17_rumor_dup_recipients(const gchar *rumor_json, const gchar *sender)
{
  if (!rumor_json || !hex64(sender) || !g_utf8_validate(rumor_json, -1, NULL))
    return NULL;
  NostrEvent *rumor = nostr_event_new();
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gboolean ok = rumor && nostr_event_deserialize_compact(rumor, rumor_json, NULL) == 1 &&
                dm_kind(nostr_event_get_kind(rumor)) && !rumor->sig && rumor->id &&
                nostr_event_validate_id(rumor, NULL) == NOSTR_EVENT_VALIDATION_OK &&
                g_strcmp0(nostr_event_get_pubkey(rumor), sender) == 0;
  gboolean has_sender = FALSE;
  guint n = 0;
  NostrTags *tags = ok ? nostr_event_get_tags(rumor) : NULL;
  for (size_t i = 0; ok && tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get_key(tag), "p") != 0)
      continue;
    const gchar *value = nostr_tag_get_value(tag);
    gchar *lower = hex64(value) ? g_ascii_strdown(value, -1) : NULL;
    ok = lower && !g_hash_table_contains(seen, lower) && ++n <= GH_NIP17_MAX_SEND_RECIPIENTS;
    if (!ok) {
      g_free(lower);
      break;
    }
    has_sender |= g_strcmp0(lower, sender) == 0;
    g_strv_builder_add(builder, lower);
    g_hash_table_add(seen, lower);
  }
  if (rumor)
    nostr_event_free(rumor);
  if (!ok || n == 0 || (has_sender && n > 1))
    return NULL;
  return g_strv_builder_end(builder);
}

/* Whether outer suits a rumor to n_recipients (0: a note to self) expiring
 * at expires_at (0: never). */
static gboolean
outer_valid(const GhNip17RoomExpiration *outer, gint64 expires_at, guint n_recipients)
{
  if (!expires_at || !outer)
    return !expires_at && !outer;
  if (outer->n_recipients != n_recipients)
    return FALSE;
  for (guint i = 0; i <= n_recipients; i++) {
    const GhNip17LayerExpiration *layer =
      i < n_recipients ? &outer->recipients[i] : &outer->self_copy;
    if (layer->seal < expires_at || layer->wrap < expires_at ||
        layer->seal > GH_NIP17_MAX_EXPIRATION || layer->wrap > GH_NIP17_MAX_EXPIRATION)
      return FALSE;
  }
  return TRUE;
}

/* Seals rumor_json to its recipients; with single, the rumor must name
 * exactly one (gh_nip17_envelope_seal_async()'s historic contract). */
static void
seal_start(GhAccountController *accounts, const gchar *rumor_json,
           const GhNip17RoomExpiration *outer, gboolean single, GCancellable *cancellable,
           GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new(accounts, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_nip17_envelope_build_async);
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
  const gchar *npub = gh_account_controller_get_active_npub(accounts);
  g_autofree gchar *sender = npub ? gh_identity_pubkey_hex(npub) : NULL;
  g_auto(GStrv) recipients = sender ? gh_nip17_rumor_dup_recipients(rumor_json, sender) : NULL;
  gint64 expires_at = 0;
  if (!recipients || (single && g_strv_length(recipients) != 1) ||
      !gh_nip17_rumor_get_expiration(rumor_json, NULL, &expires_at)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Not a canonical NIP-17 rumor of the active account");
    g_object_unref(task);
    return;
  }
  gboolean self_only = g_strcmp0(recipients[0], sender) == 0;
  if (!outer_valid(outer, expires_at, self_only ? 0 : g_strv_length(recipients))) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            expires_at ? "A disappearing NIP-17 message needs outer "
                                         "expirations no earlier than its own"
                                       : "Only a disappearing NIP-17 message has outer "
                                         "expirations");
    g_object_unref(task);
    return;
  }
  start_with_rumor(task, (const gchar *const *)recipients, self_only, g_strdup(rumor_json),
                   outer, FALSE);
}

void
gh_nip17_envelope_seal_async(GhAccountController *accounts, const gchar *rumor_json,
                             GCancellable *cancellable, GAsyncReadyCallback callback,
                             gpointer user_data)
{
  gh_nip17_envelope_seal_expiring_async(accounts, rumor_json, NULL, cancellable, callback,
                                        user_data);
}

void
gh_nip17_envelope_seal_expiring_async(GhAccountController *accounts, const gchar *rumor_json,
                                      const GhNip17OuterExpiration *outer,
                                      GCancellable *cancellable, GAsyncReadyCallback callback,
                                      gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts));
  GhNip17RoomExpiration room = { 0 };
  if (outer) {
    /* A one-to-one rumor: n_recipients 1, or 0 for a note to self. */
    const gchar *npub = gh_account_controller_get_active_npub(accounts);
    g_autofree gchar *sender = npub ? gh_identity_pubkey_hex(npub) : NULL;
    g_autofree gchar *recipient = sender ? gh_nip17_rumor_get_recipient(rumor_json, sender)
                                         : NULL;
    room.n_recipients = recipient && g_strcmp0(recipient, sender) != 0 ? 1 : 0;
    room.recipients[0] = outer->recipient;
    room.self_copy = outer->self_copy;
  }
  seal_start(accounts, rumor_json, outer ? &room : NULL, TRUE, cancellable, callback,
             user_data);
}

void
gh_nip17_envelope_seal_room_async(GhAccountController *accounts, const gchar *rumor_json,
                                  const GhNip17RoomExpiration *outer,
                                  GCancellable *cancellable, GAsyncReadyCallback callback,
                                  gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts));
  seal_start(accounts, rumor_json, outer, FALSE, cancellable, callback, user_data);
}

void
gh_nip17_envelope_seal_welcome_async(GhAccountController *accounts, const gchar *rumor_json,
                                     const gchar *recipient_pubkey_hex,
                                     GCancellable *cancellable, GAsyncReadyCallback callback,
                                     gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts));
  GTask *task = g_task_new(accounts, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_nip17_envelope_build_async);
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
  const gchar *npub = gh_account_controller_get_active_npub(accounts);
  g_autofree gchar *sender = npub ? gh_identity_pubkey_hex(npub) : NULL;
  g_autofree gchar *recipient = hex64(recipient_pubkey_hex)
                                  ? g_ascii_strdown(recipient_pubkey_hex, -1) : NULL;
  NostrEvent *rumor = nostr_event_new();
  gboolean ok = sender && recipient && g_strcmp0(sender, recipient) != 0 && rumor_json &&
                strnlen(rumor_json, GH_NIP17_MAX_PLAINTEXT + 1) <= GH_NIP17_MAX_PLAINTEXT &&
                rumor && nostr_event_deserialize_compact(rumor, rumor_json, NULL) == 1 &&
                nostr_event_get_kind(rumor) == GH_NIP17_WELCOME_KIND && !rumor->sig &&
                rumor->id && nostr_event_validate_id(rumor, NULL) == NOSTR_EVENT_VALIDATION_OK &&
                g_strcmp0(nostr_event_get_pubkey(rumor), sender) == 0 &&
                nostr_event_get_content(rumor) && *nostr_event_get_content(rumor);
  if (rumor)
    nostr_event_free(rumor);
  if (!ok) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Not a canonical kind-444 Welcome rumor of the active account "
                            "for another person");
    g_object_unref(task);
    return;
  }
  const gchar *const recipients[] = { recipient, NULL };
  start_with_rumor(task, recipients, FALSE, g_strdup(rumor_json), NULL, TRUE);
}

void
gh_nip17_envelope_build_async(GhAccountController *accounts,
                                    const gchar *recipient_pubkey_hex,
                                    const gchar *content,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts));
  build_start(accounts, recipient_pubkey_hex, FALSE, content, cancellable,
              callback, user_data);
}

void
gh_nip17_envelope_build_self_async(GhAccountController *accounts,
                                   const gchar *content,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts));
  build_start(accounts, NULL, TRUE, content, cancellable, callback, user_data);
}

GhNip17Envelope *
gh_nip17_envelope_build_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) ==
                       gh_nip17_envelope_build_async, NULL);
  GhNip17Envelope *envelope = g_task_propagate_pointer(G_TASK(result), error);
  if (envelope && !current(G_TASK(result))) {
    gh_nip17_envelope_free(envelope);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "NIP-17 account generation or request was cancelled");
    return NULL;
  }
  return envelope;
}
