#include "gh-nip17-inbox.h"
#include "gh-identity.h"
#include "gh-signer.h"
#include "nostr-event.h"
#include "nostr-tag.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

G_DEFINE_QUARK(gh-nip17-inbox-error-quark, gh_nip17_inbox_error)

typedef struct {
  guint64 generation;
  gchar *account_npub;
  gchar *account;     /* lowercase hex */
  gchar *wrap_id;
  gchar *seal_pubkey;
  gint64 wrap_expiration;
  gint64 seal_expiration;
} Unwrap;

static void
unwrap_free(gpointer data)
{
  Unwrap *unwrap = data;
  g_free(unwrap->account_npub);
  g_free(unwrap->account);
  g_free(unwrap->wrap_id);
  g_free(unwrap->seal_pubkey);
  g_free(unwrap);
}

void
gh_nip17_message_free(GhNip17Message *message)
{
  if (!message) return;
  g_free(message->account_pubkey);
  g_free(message->wrap_id);
  g_free(message->rumor_id);
  g_free(message->rumor_json);
  g_free(message->sender_pubkey);
  g_strfreev(message->recipients);
  g_free(message);
}

static gboolean
lower_hex64(const gchar *value)
{
  if (!value || strlen(value) != 64) return FALSE;
  for (const gchar *p = value; *p; p++)
    if (!g_ascii_isdigit(*p) && (*p < 'a' || *p > 'f')) return FALSE;
  return TRUE;
}

static gboolean
bounded(const gchar *text, gsize max)
{
  return text && strnlen(text, max + 1) <= max;
}

static gboolean
current(GTask *task)
{
  GhAccountController *accounts = g_task_get_source_object(task);
  Unwrap *unwrap = g_task_get_task_data(task);
  GCancellable *cancel = g_task_get_cancellable(task);
  return (!cancel || !g_cancellable_is_cancelled(cancel)) &&
         gh_account_controller_is_current(accounts, unwrap->generation) &&
         g_strcmp0(gh_account_controller_get_active_npub(accounts),
                   unwrap->account_npub) == 0;
}

/* Any failure observed after the account or request went away is reported as
 * a cancellation, so a stale result can never be mistaken for a verdict. */
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
reject(GTask *task, GhNip17InboxError code, const gchar *message)
{
  fail(task, g_error_new_literal(GH_NIP17_INBOX_ERROR, code, message));
}

static void
fail_cancelled(GTask *task)
{
  fail(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                 "NIP-17 account generation or request was cancelled"));
}

/* Counts p tags and returns the value of the first one. */
static guint
p_tags(const NostrEvent *event, const gchar **first)
{
  NostrTags *tags = nostr_event_get_tags(event);
  guint count = 0;
  *first = NULL;
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag || nostr_tag_size(tag) < 1 || g_strcmp0(nostr_tag_get(tag, 0), "p") != 0)
      continue;
    if (count++ == 0)
      *first = nostr_tag_size(tag) >= 2 ? nostr_tag_get(tag, 1) : NULL;
  }
  return count;
}

/* Parses a NIP-40 value: canonical decimal unix seconds in
 * [1, GH_NIP17_MAX_EXPIRATION], i.e. digits only, no sign or leading zero. */
static gboolean
parse_expiration(const gchar *text, gint64 *out)
{
  if (!text || *text < '1' || *text > '9') return FALSE;
  gint64 value = 0;
  for (const gchar *p = text; *p; p++) {
    if (!g_ascii_isdigit(*p)) return FALSE;
    value = value * 10 + (*p - '0');
    if (value > GH_NIP17_MAX_EXPIRATION) return FALSE;
  }
  *out = value;
  return TRUE;
}

/* Reads the event's expiration into *out (0 when absent). Fails if there is
 * more than one expiration tag or one is not exactly ["expiration", value]
 * with a valid value. With only_expiration, any other tag also fails: this
 * is the seal rule, where NIP-59's empty-tags requirement admits only the
 * expiration that NIP-17 asks senders to copy onto the seal. */
static gboolean
expiration_tag(const NostrEvent *event, gboolean only_expiration, gint64 *out)
{
  NostrTags *tags = nostr_event_get_tags(event);
  gboolean found = FALSE;
  *out = 0;
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    const gchar *key = tag && nostr_tag_size(tag) >= 1 ? nostr_tag_get(tag, 0) : NULL;
    if (g_strcmp0(key, "expiration") != 0) {
      if (only_expiration) return FALSE;
      continue;
    }
    if (found || nostr_tag_size(tag) != 2 || !parse_expiration(nostr_tag_get(tag, 1), out)) {
      *out = 0;
      return FALSE;
    }
    found = TRUE;
  }
  return TRUE;
}

static NostrEvent *
parse_signed(const gchar *json, int kind, gchar canonical_id[65])
{
  NostrEvent *event = nostr_event_new();
  if (!event) return NULL;
  if (nostr_event_deserialize_signed(event, json, NULL) != NOSTR_EVENT_VALIDATION_OK ||
      nostr_event_validate(event, canonical_id) != NOSTR_EVENT_VALIDATION_OK ||
      nostr_event_get_kind(event) != kind ||
      !lower_hex64(nostr_event_get_pubkey(event)) ||
      !nostr_event_get_content(event) || !*nostr_event_get_content(event)) {
    nostr_event_free(event);
    return NULL;
  }
  return event;
}

static void wrap_decrypted(GObject *source, GAsyncResult *result, gpointer data);
static void seal_decrypted(GObject *source, GAsyncResult *result, gpointer data);

void
gh_nip17_unwrap_async(GhAccountController *accounts, const gchar *wrap_json,
                      GCancellable *cancellable, GAsyncReadyCallback callback,
                      gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts));
  GTask *task = g_task_new(accounts, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_nip17_unwrap_async);
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
  const gchar *npub = gh_account_controller_get_active_npub(accounts);
  gchar *account = npub ? gh_identity_pubkey_hex(npub) : NULL;
  if (!account) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                            "No active account to receive NIP-17 messages");
    g_object_unref(task);
    return;
  }
  Unwrap *unwrap = g_new0(Unwrap, 1);
  unwrap->generation = gh_account_controller_get_generation(accounts);
  unwrap->account_npub = g_strdup(npub);
  unwrap->account = g_ascii_strdown(account, -1);
  g_free(account);
  g_task_set_task_data(task, unwrap, unwrap_free);

  if (!bounded(wrap_json, GH_NIP17_MAX_WRAP_JSON)) {
    reject(task, GH_NIP17_INBOX_ERROR_TOO_LARGE, "NIP-17 gift wrap is missing or too large");
    return;
  }
  gchar wrap_id[65] = { 0 };
  NostrEvent *wrap = parse_signed(wrap_json, 1059, wrap_id);
  if (!wrap) {
    reject(task, GH_NIP17_INBOX_ERROR_INVALID_WRAP,
           "NIP-17 gift wrap is malformed or its id or signature is invalid");
    return;
  }
  const gchar *p = NULL;
  if (p_tags(wrap, &p) != 1 || g_strcmp0(p, unwrap->account) != 0) {
    nostr_event_free(wrap);
    reject(task, GH_NIP17_INBOX_ERROR_WRONG_RECIPIENT,
           "NIP-17 gift wrap is not addressed to the active account");
    return;
  }
  if (!expiration_tag(wrap, FALSE, &unwrap->wrap_expiration)) {
    nostr_event_free(wrap);
    reject(task, GH_NIP17_INBOX_ERROR_INVALID_WRAP,
           "NIP-17 gift wrap has a malformed or repeated expiration tag");
    return;
  }
  unwrap->wrap_id = g_strdup(wrap_id);
  gh_account_controller_nip44_decrypt_with_cancellable_async(
    accounts, nostr_event_get_content(wrap), nostr_event_get_pubkey(wrap),
    cancellable, wrap_decrypted, task);
  nostr_event_free(wrap);
}

static void
wrap_decrypted(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  Unwrap *unwrap = g_task_get_task_data(task);
  GError *error = NULL;
  g_autofree gchar *seal_json = gh_account_controller_nip44_finish(result, &error);
  (void)source;
  if (error) { fail(task, error); return; }
  if (!current(task)) { fail_cancelled(task); return; }
  if (!bounded(seal_json, GH_NIP17_MAX_PLAINTEXT)) {
    reject(task, GH_NIP17_INBOX_ERROR_TOO_LARGE, "NIP-17 seal is missing or too large");
    return;
  }
  NostrEvent *seal = parse_signed(seal_json, 13, NULL);
  if (!seal || !expiration_tag(seal, TRUE, &unwrap->seal_expiration)) {
    if (seal) nostr_event_free(seal);
    reject(task, GH_NIP17_INBOX_ERROR_INVALID_SEAL,
           "NIP-17 seal is malformed, has a tag other than one valid expiration, "
           "or its id or signature is invalid");
    return;
  }
  unwrap->seal_pubkey = g_strdup(nostr_event_get_pubkey(seal));
  gh_account_controller_nip44_decrypt_with_cancellable_async(
    g_task_get_source_object(task), nostr_event_get_content(seal), unwrap->seal_pubkey,
    g_task_get_cancellable(task), seal_decrypted, task);
  nostr_event_free(seal);
}

/* Returns the unique lowercase p-tag pubkeys, or NULL if any is malformed or
 * there are none or too many. */
static gchar **
rumor_recipients(const NostrEvent *rumor)
{
  NostrTags *tags = nostr_event_get_tags(rumor);
  g_autoptr(GPtrArray) out = g_ptr_array_new_with_free_func(g_free);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag || nostr_tag_size(tag) < 1 || g_strcmp0(nostr_tag_get(tag, 0), "p") != 0)
      continue;
    const gchar *pubkey = nostr_tag_size(tag) >= 2 ? nostr_tag_get(tag, 1) : NULL;
    if (!lower_hex64(pubkey)) return NULL;
    gboolean seen = FALSE;
    for (guint j = 0; j < out->len && !seen; j++)
      seen = g_str_equal(g_ptr_array_index(out, j), pubkey);
    if (seen) continue;
    if (out->len >= GH_NIP17_MAX_RECIPIENTS) return NULL;
    g_ptr_array_add(out, g_strdup(pubkey));
  }
  if (out->len == 0) return NULL;
  g_ptr_array_add(out, NULL);
  return (gchar **)g_ptr_array_free(g_steal_pointer(&out), FALSE);
}

static void
seal_decrypted(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  Unwrap *unwrap = g_task_get_task_data(task);
  GError *error = NULL;
  g_autofree gchar *rumor_json = gh_account_controller_nip44_finish(result, &error);
  (void)source;
  if (error) { fail(task, error); return; }
  if (!current(task)) { fail_cancelled(task); return; }
  if (!bounded(rumor_json, GH_NIP17_MAX_PLAINTEXT)) {
    reject(task, GH_NIP17_INBOX_ERROR_TOO_LARGE, "NIP-17 rumor is missing or too large");
    return;
  }
  NostrEvent *rumor = nostr_event_new();
  if (!rumor ||
      nostr_event_deserialize_unsigned(rumor, rumor_json, NULL) != NOSTR_EVENT_VALIDATION_OK ||
      rumor->sig != NULL || nostr_event_get_created_at(rumor) <= 0 ||
      !nostr_event_get_content(rumor)) {
    if (rumor) nostr_event_free(rumor);
    reject(task, GH_NIP17_INBOX_ERROR_INVALID_RUMOR, "NIP-17 rumor is malformed or signed");
    return;
  }
  if (nostr_event_get_kind(rumor) != 14) {
    nostr_event_free(rumor);
    reject(task, GH_NIP17_INBOX_ERROR_UNSUPPORTED_KIND,
           "Only kind-14 NIP-17 chat messages are accepted");
    return;
  }
  if (g_strcmp0(nostr_event_get_pubkey(rumor), unwrap->seal_pubkey) != 0) {
    nostr_event_free(rumor);
    reject(task, GH_NIP17_INBOX_ERROR_SENDER_MISMATCH,
           "NIP-17 rumor author differs from the seal signer");
    return;
  }
  gchar rumor_id[65] = { 0 };
  NostrEventValidationStatus id_status = rumor->id ?
    nostr_event_validate_id(rumor, rumor_id) : nostr_event_compute_id(rumor, rumor_id);
  gchar **recipients = id_status == NOSTR_EVENT_VALIDATION_OK ?
    rumor_recipients(rumor) : NULL;
  gint64 rumor_expiration = 0;
  if (!recipients || !expiration_tag(rumor, FALSE, &rumor_expiration)) {
    g_strfreev(recipients);
    nostr_event_free(rumor);
    reject(task, GH_NIP17_INBOX_ERROR_INVALID_RUMOR,
           "NIP-17 rumor id, recipient or expiration tags are invalid");
    return;
  }
  gboolean self_copy = g_str_equal(unwrap->seal_pubkey, unwrap->account);
  if (!self_copy && !g_strv_contains((const gchar *const *)recipients, unwrap->account)) {
    g_strfreev(recipients);
    nostr_event_free(rumor);
    reject(task, GH_NIP17_INBOX_ERROR_WRONG_RECIPIENT,
           "NIP-17 rumor does not include the active account");
    return;
  }
  free(rumor->id);
  rumor->id = strdup(rumor_id);
  char *canonical = rumor->id ? nostr_event_serialize_compact(rumor) : NULL;
  gint64 created_at = nostr_event_get_created_at(rumor);
  nostr_event_free(rumor);
  if (!canonical) {
    g_strfreev(recipients);
    reject(task, GH_NIP17_INBOX_ERROR_INVALID_RUMOR, "Could not serialize NIP-17 rumor");
    return;
  }
  if (!current(task)) {
    free(canonical);
    g_strfreev(recipients);
    fail_cancelled(task);
    return;
  }
  GhNip17Message *message = g_new0(GhNip17Message, 1);
  message->account_pubkey = g_strdup(unwrap->account);
  message->wrap_id = g_strdup(unwrap->wrap_id);
  message->rumor_id = g_strdup(rumor_id);
  message->rumor_json = g_strdup(canonical);
  message->sender_pubkey = g_strdup(unwrap->seal_pubkey);
  message->recipients = recipients;
  message->created_at = created_at;
  message->self_copy = self_copy;
  message->rumor_expiration = rumor_expiration;
  message->seal_expiration = unwrap->seal_expiration;
  message->wrap_expiration = unwrap->wrap_expiration;
  message->expires_at = rumor_expiration ? rumor_expiration :
                        unwrap->seal_expiration ? unwrap->seal_expiration :
                        unwrap->wrap_expiration;
  free(canonical);
  g_task_return_pointer(task, message, (GDestroyNotify)gh_nip17_message_free);
  g_object_unref(task);
}

GhNip17Message *
gh_nip17_unwrap_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == gh_nip17_unwrap_async, NULL);
  GhNip17Message *message = g_task_propagate_pointer(G_TASK(result), error);
  if (message && !current(G_TASK(result))) {
    gh_nip17_message_free(message);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "NIP-17 account generation or request was cancelled");
    return NULL;
  }
  return message;
}

/* ---- restart-safe seen-set ------------------------------------------------ */

#define SEEN_MAGIC "groundhog-nip17-seen 1 "
#define SEEN_LINE 67 /* "w " or "r " + 64 hex + "\n" */

struct _GhNip17Seen {
  gchar *path;
  gchar *account;
  guint capacity;
  GHashTable *keys; /* "w<id>" / "r<id>" -> same string in order */
  GQueue order;     /* oldest first; owns the key strings */
  guint file_lines; /* entry lines currently in the file */
  gboolean needs_rewrite; /* a torn tail must never be appended to */
};

void
gh_nip17_seen_free(GhNip17Seen *seen)
{
  if (!seen) return;
  g_hash_table_unref(seen->keys);
  g_queue_clear_full(&seen->order, g_free);
  g_free(seen->path);
  g_free(seen->account);
  g_free(seen);
}

static void
seen_insert(GhNip17Seen *seen, gchar type, const gchar *id)
{
  gchar *key = g_strdup_printf("%c%s", type, id);
  if (g_hash_table_contains(seen->keys, key)) { g_free(key); return; }
  g_hash_table_add(seen->keys, key);
  g_queue_push_tail(&seen->order, key);
  while (seen->order.length > seen->capacity) {
    gchar *old = g_queue_pop_head(&seen->order);
    g_hash_table_remove(seen->keys, old);
    g_free(old);
  }
}

GhNip17Seen *
gh_nip17_seen_open(const gchar *path, const gchar *account_pubkey_hex, guint capacity,
                   GError **error)
{
  g_return_val_if_fail(path != NULL, NULL);
  if (!lower_hex64(account_pubkey_hex) || capacity == 0 || capacity > 1u << 20) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A lowercase account pubkey and a bounded capacity are required");
    return NULL;
  }
  GhNip17Seen *seen = g_new0(GhNip17Seen, 1);
  seen->path = g_strdup(path);
  seen->account = g_strdup(account_pubkey_hex);
  seen->capacity = capacity;
  seen->keys = g_hash_table_new(g_str_hash, g_str_equal);
  g_queue_init(&seen->order);

  g_autofree gchar *contents = NULL;
  gsize length = 0;
  GError *read_error = NULL;
  if (!g_file_get_contents(path, &contents, &length, &read_error)) {
    if (g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_clear_error(&read_error);
      return seen; /* nothing recorded yet */
    }
    g_propagate_error(error, read_error);
    gh_nip17_seen_free(seen);
    return NULL;
  }
  g_autofree gchar *header = g_strconcat(SEEN_MAGIC, seen->account, "\n", NULL);
  gsize header_len = strlen(header);
  /* The log is compacted at twice the capacity; allow one torn tail line. */
  gsize max_len = header_len + ((gsize)capacity * 2 + 1) * SEEN_LINE;
  if (length < header_len || length > max_len || memchr(contents, '\0', length) ||
      memcmp(contents, header, header_len) != 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "%s is not a NIP-17 seen-set for this account", path);
    gh_nip17_seen_free(seen);
    return NULL;
  }
  const gchar *line = contents + header_len;
  const gchar *end = contents + length;
  while (line < end) {
    const gchar *newline = memchr(line, '\n', end - line);
    if (!newline) {
      /* A torn final append is ignored, and the next record compacts the
       * file so that no entry is ever glued onto the fragment. */
      seen->needs_rewrite = TRUE;
      break;
    }
    g_autofree gchar *id = g_strndup(line + 2, MAX(newline - line - 2, 0));
    if (newline - line != SEEN_LINE - 1 || (line[0] != 'w' && line[0] != 'r') ||
        line[1] != ' ' || !lower_hex64(id)) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "%s contains a malformed entry", path);
      gh_nip17_seen_free(seen);
      return NULL;
    }
    seen_insert(seen, line[0], id);
    seen->file_lines++;
    line = newline + 1;
  }
  return seen;
}

static gboolean
seen_has(GhNip17Seen *seen, gchar type, const gchar *id)
{
  if (!seen || !lower_hex64(id)) return FALSE;
  gchar key[66];
  key[0] = type;
  memcpy(key + 1, id, 65);
  return g_hash_table_contains(seen->keys, key);
}

gboolean
gh_nip17_seen_has_wrap(GhNip17Seen *seen, const gchar *wrap_id)
{
  return seen_has(seen, 'w', wrap_id);
}

gboolean
gh_nip17_seen_has_rumor(GhNip17Seen *seen, const gchar *rumor_id)
{
  return seen_has(seen, 'r', rumor_id);
}

static gboolean
seen_rewrite(GhNip17Seen *seen, GError **error)
{
  GString *out = g_string_new(SEEN_MAGIC);
  g_string_append_printf(out, "%s\n", seen->account);
  for (GList *l = seen->order.head; l; l = l->next) {
    const gchar *key = l->data;
    g_string_append_printf(out, "%c %s\n", key[0], key + 1);
  }
  gboolean ok = g_file_set_contents_full(seen->path, out->str, out->len,
                                         G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
  g_string_free(out, TRUE);
  if (ok) {
    seen->file_lines = seen->order.length;
    seen->needs_rewrite = FALSE;
  }
  return ok;
}

static gboolean
seen_append(GhNip17Seen *seen, const gchar *lines, guint count, GError **error)
{
  /* Owner-only: the seen-set reveals per-account message metadata. fchmod
   * also narrows a file that an older build created with the umask mode. */
  int fd = open(seen->path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
  FILE *file = NULL;
  if (fd >= 0 && fchmod(fd, 0600) == 0)
    file = fdopen(fd, "ab");
  int saved = errno;
  if (!file && fd >= 0) close(fd);
  gboolean ok = file && fwrite(lines, 1, strlen(lines), file) == strlen(lines) &&
                fflush(file) == 0 && fsync(fileno(file)) == 0;
  if (file) saved = errno;
  if (file && fclose(file) != 0 && ok) { ok = FALSE; saved = errno; }
  if (!ok) {
    /* A failed write (ENOSPC, EIO) may leave a partial line; the next record
     * must rewrite from memory rather than append onto it. */
    seen->needs_rewrite = TRUE;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved),
                "Could not record NIP-17 seen-set entry: %s", g_strerror(saved));
    return FALSE;
  }
  seen->file_lines += count;
  return TRUE;
}

gboolean
gh_nip17_seen_record(GhNip17Seen *seen, const GhNip17Message *message, GError **error)
{
  g_return_val_if_fail(seen != NULL && message != NULL, FALSE);
  if (g_strcmp0(message->account_pubkey, seen->account) != 0 ||
      !lower_hex64(message->wrap_id) || !lower_hex64(message->rumor_id)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Message does not belong to this account's seen-set");
    return FALSE;
  }
  GString *lines = g_string_new(NULL);
  guint count = 0;
  if (!gh_nip17_seen_has_wrap(seen, message->wrap_id)) {
    g_string_append_printf(lines, "w %s\n", message->wrap_id);
    count++;
  }
  if (!gh_nip17_seen_has_rumor(seen, message->rumor_id)) {
    g_string_append_printf(lines, "r %s\n", message->rumor_id);
    count++;
  }
  gboolean ok = TRUE;
  if (count) {
    gboolean exists = g_file_test(seen->path, G_FILE_TEST_EXISTS);
    seen_insert(seen, 'w', message->wrap_id);
    seen_insert(seen, 'r', message->rumor_id);
    if (!exists || seen->needs_rewrite || seen->file_lines + count > seen->capacity * 2)
      ok = seen_rewrite(seen, error);
    else
      ok = seen_append(seen, lines->str, count, error);
  }
  g_string_free(lines, TRUE);
  return ok;
}
