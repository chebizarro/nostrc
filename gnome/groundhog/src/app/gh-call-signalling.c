#include "gh-call-signalling.h"
#include "gh-auth-policy.h"
#include "gh-identity.h"
#include "gh-relay-deny-policy.h"
#include "gh-relay-publish.h"
#include <nostr/nip59/nip59.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Every asynchronous leg owns its job; cancellation removes the queue's
 * ownership but cannot free a job underneath a late signer completion. */
typedef struct {
  grefcount refs;
  GhCallSignalling *owner;
  guint64 generation;
  gchar *id, *peer, *destination, *unsigned_json, *signed_json;
  GCancellable *cancel;
  GhRelayPublish *publish;
  GhRelayScope *lookup;
  GPtrArray *fallback;
  GHashTable *sources;
  gint64 newest;
  guint expiry;
  gboolean self_copy, finished, accepted;
  int kind;
} Send;

struct _GhCallSignalling {
  GObject parent_instance;
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GhInboxResolver *resolver;
  GSettings *settings;
  GhRelayScope *scope;
  GCancellable *receive_cancel;
  GPtrArray *jobs;
  GQueue queue, incoming;
  guint64 generation;
  gchar *pubkey;
  gchar *trace_path; /* GROUNDHOG_CALL_TRACE at construction; NULL: no trace */
  gboolean decrypting, stopped, paused;
};
G_DEFINE_TYPE(GhCallSignalling, gh_call_signalling, G_TYPE_OBJECT)

/* Interop capture (docs/groundhog-call-interop-runbook.md): with
 * GROUNDHOG_CALL_TRACE=<file> in the environment when the service starts,
 * every validated inner call event (25050-25055: SDP offers and answers, ICE
 * candidates, hangups and rejects) is appended to that file as one JSON
 * line, {"ts":<unix microseconds>,"dir":"out"|"in","event":<signed inner
 * event>}, after it was signed (out) or decrypted and validated (in). Unset,
 * the default, nothing is written and nothing else changes. The file holds
 * decrypted SDP with addresses: for the owner's own test runs only, so it
 * is created readable by this user only (0600, as gh-diagnostics.c keeps
 * its files) and never through a symbolic link; a path that already exists
 * as anything but a regular file of this user with no group or other
 * permissions is refused, with one warning, and tracing stays off for the
 * rest of the session rather than appending plaintext to it. */
static void trace(GhCallSignalling *self, const gchar *direction, const gchar *inner_json)
{
  if (!self->trace_path) return;
  struct stat st;
  const gchar *refused = NULL;
  int fd = open(self->trace_path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) refused = errno == ELOOP ? "it is a symbolic link" : g_strerror(errno);
  else if (fstat(fd, &st) < 0) refused = g_strerror(errno);
  else if (!S_ISREG(st.st_mode)) refused = "it is not a regular file";
  else if (st.st_uid != getuid()) refused = "it is not owned by you";
  else if (st.st_mode & 077) refused = "it is readable or writable by others";
  if (refused) {
    g_warning("GROUNDHOG_CALL_TRACE: cannot append to %s: %s; call tracing is off for this session",
              self->trace_path, refused);
    if (fd >= 0) close(fd);
    g_clear_pointer(&self->trace_path, g_free);
    return;
  }
  g_autofree gchar *line = g_strdup_printf("{\"ts\":%" G_GINT64_FORMAT ",\"dir\":\"%s\",\"event\":%s}\n",
                                           g_get_real_time(), direction, inner_json);
  for (const gchar *p = line, *end = line + strlen(line); p < end;) {
    ssize_t n = write(fd, p, (size_t)(end - p));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      g_warning("GROUNDHOG_CALL_TRACE: cannot append to %s: %s", self->trace_path, g_strerror(errno));
      break;
    }
    p += n;
  }
  close(fd);
}

static void pump(GhCallSignalling *self);
static void receive_next(GhCallSignalling *self);
static void rebuild(GhCallSignalling *self);
static Send *send_ref(Send *s) { g_ref_count_inc(&s->refs); return s; }
static void clear_scope(GhRelayScope **p)
{
  if (*p) { gh_relay_scope_cancel(*p); gh_relay_scope_unref(*p); *p = NULL; }
}
static void send_unref(Send *s)
{
  if (!g_ref_count_dec(&s->refs)) return;
  clear_scope(&s->lookup);
  if (s->publish) gh_relay_publish_unref(s->publish);
  g_clear_pointer(&s->fallback, g_ptr_array_unref);
  g_clear_pointer(&s->sources, g_hash_table_unref);
  g_object_unref(s->cancel);
  g_free(s->id); g_free(s->peer); g_free(s->destination);
  g_free(s->unsigned_json); g_free(s->signed_json);
  g_object_unref(s->owner); g_free(s);
}
static gboolean current(Send *s)
{
  return !s->finished && !s->owner->stopped && !g_cancellable_is_cancelled(s->cancel) &&
    gh_account_controller_is_current(s->owner->accounts, s->generation);
}
static void finish(Send *s, gboolean success)
{
  if (s->finished) return;
  send_ref(s);
  s->finished = TRUE;
  g_cancellable_cancel(s->cancel);
  clear_scope(&s->lookup);
  if (s->publish) gh_relay_publish_cancel(s->publish);
  if (s->expiry) { guint source = s->expiry; s->expiry = 0; g_source_remove(source); }
  GhCallSignalling *self = s->owner;
  gboolean head = g_queue_peek_head(&self->queue) == s;
  g_queue_remove(&self->queue, s);
  if (!success && !self->stopped && s->generation == self->generation)
    g_signal_emit_by_name(self, "failed", s->id);
  g_ptr_array_remove(self->jobs, s);
  if (head) pump(self);
  send_unref(s);
}
static gboolean expire(gpointer data)
{
  Send *s = data; s->expiry = 0; finish(s, FALSE); return G_SOURCE_REMOVE;
}
static void advance(Send *s)
{
  GhCallSignalling *self = s->owner;
  if (g_queue_peek_head(&self->queue) == s) {
    g_queue_pop_head(&self->queue); pump(self);
  }
}

static void published(GhRelayPublish *p, const GhRelayPublishSummary *summary, gpointer data)
{
  (void)p;
  Send *s = send_ref(data);
  s->accepted = summary->any_accepted;
  finish(s, s->accepted);
  send_unref(s);
}
static void publish_to(Send *s, const gchar *const *urls)
{
  if (!current(s)) { finish(s, FALSE); return; }
  NostrEvent *inner = gh_call_event_parse(s->signed_json, NULL, g_get_real_time()/G_USEC_PER_SEC, NULL);
  NostrEvent *wrap = inner ? nostr_nip59_wrap_kind(inner, s->destination,
                                     NOSTR_KIND_EPHEMERAL_GIFT_WRAP, NULL) : NULL;
  if (inner) nostr_event_free(inner);
  g_autofree gchar *json = wrap ? nostr_event_serialize_compact(wrap) : NULL;
  if (wrap) nostr_event_free(wrap);
  s->publish = json ? gh_relay_publish_new(s->generation, json, NULL, published, s, NULL) : NULL;
  if (!s->publish) { finish(s, FALSE); return; }
  GhAuthPolicy *auth = gh_auth_policy_get_for_accounts(s->owner->accounts);
  guint added = 0;
  for (guint i = 0; urls && urls[i] && i < 16; i++) {
    if (!gh_relay_deny_policy_check(s->generation, urls[i], FALSE, NULL)) continue;
    if (gh_relay_publish_add_url(s->publish, urls[i], NULL) &&
        gh_auth_policy_apply_publish(auth, s->publish,
          s->self_copy ? GH_AUTH_PURPOSE_SELF_WRAP : GH_AUTH_PURPOSE_RECIPIENT_WRAP, urls[i], NULL)) added++;
  }
  gh_relay_publish_set_deadline(s->publish, 8);
  gh_relay_publish_set_retries(s->publish, 1, 250);
  /* start may complete synchronously; preserve s through its done callback. */
  send_ref(s);
  advance(s);
  if (!added || !gh_relay_publish_start(s->publish, NULL)) finish(s, FALSE);
  send_unref(s);
}

static void lookup_update(GhRelayScope *scope, const GhRelayUpdate *u, gpointer data)
{
  (void)scope;
  Send *s = data;
  if (!current(s)) return;
  if (u->notice == GH_RELAY_NOTICE_EVENT) {
    NostrEvent *e = nostr_event_new();
    if (nostr_event_deserialize_signed(e, u->event_json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
        nostr_event_validate(e, NULL) == NOSTR_EVENT_VALIDATION_OK &&
        nostr_event_get_kind(e) == NOSTR_KIND_RELAY_LIST_METADATA &&
        !g_strcmp0(nostr_event_get_pubkey(e), s->destination) &&
        nostr_event_get_created_at(e) <= g_get_real_time()/G_USEC_PER_SEC + 60 &&
        nostr_event_get_created_at(e) > s->newest) {
      s->newest = nostr_event_get_created_at(e);
      g_ptr_array_set_size(s->fallback, 0);
      NostrTags *tags = nostr_event_get_tags(e);
      for (gsize i = 0; tags && i < nostr_tags_size(tags) && s->fallback->len < 16; i++) {
        NostrTag *t = nostr_tags_get(tags, i);
        const gchar *url = nostr_tag_get(t, 1), *mode = nostr_tag_get(t, 2);
        if (!g_strcmp0(nostr_tag_get_key(t), "r") &&
            (!mode || !g_strcmp0(mode, "read")) && gh_relay_url_validate(url, NULL))
          g_ptr_array_add(s->fallback, g_strdup(url));
      }
    }
    nostr_event_free(e);
  } else if (u->notice == GH_RELAY_NOTICE_EOSE || u->notice == GH_RELAY_NOTICE_CLOSED ||
             u->notice == GH_RELAY_NOTICE_ERROR || u->notice == GH_RELAY_NOTICE_DISCONNECTED) {
    g_hash_table_remove(s->sources, u->url);
    if (!g_hash_table_size(s->sources)) {
      send_ref(s);
      clear_scope(&s->lookup);
      g_ptr_array_add(s->fallback, NULL);
      publish_to(s, (const gchar *const *)s->fallback->pdata);
      send_unref(s);
    }
  }
}
static void fallback(Send *s)
{
  /* Only authenticated absence of 10050 permits NIP-65 fallback; an empty
   * 10050 is an explicit refusal, and a failed lookup is not absence. */
  NostrFilter *f = nostr_filter_new();
  int kind = NOSTR_KIND_RELAY_LIST_METADATA;
  nostr_filter_set_kinds(f, &kind, 1);
  const gchar *authors[] = { s->destination };
  nostr_filter_set_authors(f, authors, 1);
  nostr_filter_set_limit(f, 1);
  NostrFilters *filters = nostr_filters_new(); nostr_filters_add(filters, f); nostr_filter_free(f);
  s->lookup = gh_relay_scope_new(s->generation, filters, lookup_update, s);
  s->fallback = g_ptr_array_new_with_free_func(g_free);
  s->sources = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_auto(GStrv) urls = g_settings_get_strv(s->owner->settings, "discovery-relays");
  GhAuthPolicy *auth = gh_auth_policy_get_for_accounts(s->owner->accounts);
  for (guint i = 0; urls && urls[i] && i < 16; i++)
    if (gh_relay_deny_policy_check(s->generation, urls[i], FALSE, NULL) &&
        gh_relay_scope_add_url(s->lookup, urls[i], NULL) &&
        gh_auth_policy_apply_scope(auth, s->lookup, GH_AUTH_PURPOSE_CONTACT_DIRECTORY, urls[i], NULL))
      g_hash_table_add(s->sources, g_strdup(urls[i]));
  if (!g_hash_table_size(s->sources)) { finish(s, FALSE); return; }
  gh_relay_scope_start(s->lookup);
}
static void resolved(GObject *source, GAsyncResult *result, gpointer data)
{
  Send *s = data;
  g_autoptr(GhInboxResult) inbox = gh_inbox_resolver_resolve_finish(GH_INBOX_RESOLVER(source), result, NULL);
  if (!current(s)) finish(s, FALSE);
  else if (inbox && inbox->status == GH_INBOX_FOUND) publish_to(s, (const gchar *const *)inbox->relays);
  else if (inbox && inbox->status == GH_INBOX_NOT_FOUND && !s->self_copy) fallback(s);
  else finish(s, FALSE);
  send_unref(s);
}
static void resolve(Send *s)
{
  if (s->self_copy) {
    publish_to(s, gh_account_relays_get_inbox_relays(s->owner->relays));
    return;
  }
  gh_inbox_resolver_resolve_async(s->owner->resolver, s->destination, s->cancel,
                                 resolved, send_ref(s));
}
static Send *job_new(GhCallSignalling *self, const gchar *peer, const gchar *id, int kind)
{
  Send *s = g_new0(Send, 1); g_ref_count_init(&s->refs);
  s->owner = g_object_ref(self); s->generation = self->generation;
  s->peer = g_strdup(peer); s->destination = g_strdup(peer); s->id = g_strdup(id); s->kind = kind;
  s->cancel = g_cancellable_new();
  g_ptr_array_add(self->jobs, s);
  s->expiry = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 60, expire, send_ref(s), (GDestroyNotify)send_unref);
  return s;
}
static void signed_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Send *s = data;
  g_autofree gchar *json = gh_account_controller_sign_finish(result, NULL);
  NostrEvent *e = json ? gh_call_event_parse(json, s->peer, g_get_real_time()/G_USEC_PER_SEC, NULL) : NULL;
  NostrEvent *original = nostr_event_new();
  gchar expected[65] = {0}, actual[65] = {0};
  gboolean valid = e && nostr_event_deserialize_unsigned(original, s->unsigned_json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_compute_id(original, expected) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_compute_id(e, actual) == NOSTR_EVENT_VALIDATION_OK && !strcmp(expected, actual);
  nostr_event_free(original); if (e) nostr_event_free(e);
  if (!current(s) || !valid) finish(s, FALSE);
  else {
    s->signed_json = g_steal_pointer(&json);
    trace(s->owner, "out", s->signed_json);
    if (s->kind == NOSTR_KIND_CALL_ANSWER || s->kind == NOSTR_KIND_CALL_REJECT) {
      Send *copy = job_new(s->owner, s->peer, s->id, s->kind);
      copy->self_copy = TRUE;
      g_free(copy->destination); copy->destination = g_strdup(s->owner->pubkey);
      copy->signed_json = g_strdup(s->signed_json);
      g_queue_push_tail(&s->owner->queue, copy);
    }
    resolve(s);
  }
  send_unref(s);
}
static void pump(GhCallSignalling *self)
{
  Send *s = g_queue_peek_head(&self->queue);
  if (!s || self->stopped || self->paused) return;
  if (s->signed_json) resolve(s);
  else gh_account_controller_sign_with_cancellable_async(self->accounts, s->unsigned_json,
                                                         s->cancel, signed_done, send_ref(s));
}

gboolean gh_call_signalling_send(GhCallSignalling *self, const gchar *peer,
    const gchar *id, int kind, GhCallType type, const gchar *content)
{
  if (self->stopped || !self->pubkey || self->jobs->len >= GH_CALL_MAX_PENDING ||
      !peer || !gh_call_id_valid(id)) return FALSE;
  NostrEvent *e = gh_call_event_new(self->pubkey, peer, id, kind, type, content,
                                   g_get_real_time()/G_USEC_PER_SEC);
  if (!e) return FALSE;
  Send *s = job_new(self, peer, id, kind);
  s->unsigned_json = nostr_event_serialize_compact(e); nostr_event_free(e);
  g_queue_push_tail(&self->queue, s);
  if (self->queue.length == 1) pump(self);
  return TRUE;
}
void gh_call_signalling_cancel_call(GhCallSignalling *self, const gchar *id)
{
  gboolean was_paused = self->paused;
  self->paused = TRUE;
  gboolean removed_head = FALSE;
  for (guint i = self->jobs->len; i > 0; i--) {
    Send *s = g_ptr_array_index(self->jobs, i-1);
    if (!g_strcmp0(s->id, id)) {
      removed_head |= g_queue_peek_head(&self->queue) == s;
      finish(s, TRUE);
    }
  }
  self->paused = was_paused;
  if (removed_head && !was_paused) pump(self);
}

typedef struct { GhCallSignalling *self; guint64 generation; } Receive;
static void decrypted(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Receive *r = data; GhCallSignalling *self = r->self;
  g_autofree gchar *plain = gh_account_controller_nip44_finish(result, NULL);
  if (!self->stopped && r->generation == self->generation) {
    NostrEvent *inner = plain ? gh_call_event_parse(plain, NULL, g_get_real_time()/G_USEC_PER_SEC, NULL) : NULL;
    if (inner) {
      trace(self, "in", plain);
      g_signal_emit_by_name(self, "event", plain); nostr_event_free(inner);
    }
    self->decrypting = FALSE; receive_next(self);
  }
  g_object_unref(self); g_free(r);
}
static void receive_next(GhCallSignalling *self)
{
  if (self->decrypting || self->stopped || !self->pubkey) return;
  g_autofree gchar *json = g_queue_pop_head(&self->incoming);
  if (!json) return;
  NostrEvent *outer = nostr_event_new();
  gboolean valid = nostr_event_deserialize_signed(outer, json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_nip59_validate_gift_wrap_kind(outer, NOSTR_KIND_EPHEMERAL_GIFT_WRAP) &&
    !g_strcmp0(gh_call_event_tag(outer, "p"), self->pubkey);
  if (valid) {
    Receive *r = g_new0(Receive, 1); r->self = g_object_ref(self); r->generation = self->generation;
    self->decrypting = TRUE;
    gh_account_controller_nip44_decrypt_with_cancellable_async(self->accounts,
      nostr_event_get_content(outer), nostr_event_get_pubkey(outer), self->receive_cancel, decrypted, r);
  }
  nostr_event_free(outer);
  if (!valid) receive_next(self);
}
static void received(GhRelayScope *scope, const GhRelayUpdate *u, gpointer data)
{
  (void)scope;
  GhCallSignalling *self = data;
  if (u->notice != GH_RELAY_NOTICE_EVENT) {
    /* EOSE is the catch-up/live boundary, not a timer. Scope owns AUTH and
     * reconnect; expose terminal CLOSED/error reasons instead of swallowing
     * them or claiming that a live subscription exists. No status is stored. */
    g_signal_emit_by_name(self, "relay-status", u->notice, u->url, u->detail);
    return;
  }
  if (self->incoming.length >= 64 || strlen(u->event_json) > 131072) return;
  g_queue_push_tail(&self->incoming, g_strdup(u->event_json)); receive_next(self);
}
static void reset(GhCallSignalling *self)
{
  clear_scope(&self->scope);
  if (self->receive_cancel) g_cancellable_cancel(self->receive_cancel);
  g_clear_object(&self->receive_cancel);
  g_queue_clear_full(&self->incoming, g_free); self->decrypting = FALSE;
  gboolean stopped = self->stopped; self->stopped = TRUE;
  while (self->jobs->len) finish(g_ptr_array_index(self->jobs, self->jobs->len-1), TRUE);
  self->stopped = stopped;
}
static void rebuild(GhCallSignalling *self)
{
  if (self->stopped) return;
  guint64 generation = gh_account_controller_get_generation(self->accounts);
  if (generation != self->generation) {
    reset(self); self->generation = generation;
    g_free(self->pubkey); self->pubkey = gh_identity_pubkey_hex(gh_account_controller_get_active_npub(self->accounts));
    self->receive_cancel = g_cancellable_new();
    g_signal_emit_by_name(self, "reset");
  }
  clear_scope(&self->scope);
  if (!self->pubkey) return;
  NostrFilter *f = nostr_filter_new(); int kind = NOSTR_KIND_EPHEMERAL_GIFT_WRAP;
  nostr_filter_set_kinds(f, &kind, 1); nostr_filter_tags_append(f, "p", self->pubkey, NULL);
  nostr_filter_set_since(f, g_get_real_time()/G_USEC_PER_SEC - 60); nostr_filter_set_limit(f, 128);
  NostrFilters *filters = nostr_filters_new(); nostr_filters_add(filters, f); nostr_filter_free(f);
  self->scope = gh_relay_scope_new(self->generation, filters, received, self);
  const gchar *const *urls = gh_account_relays_get_inbox_relays(self->relays);
  GhAuthPolicy *auth = gh_auth_policy_get_for_accounts(self->accounts);
  for (guint i = 0; urls && urls[i] && i < 16; i++)
    if (gh_relay_deny_policy_check(self->generation, urls[i], FALSE, NULL) &&
        gh_relay_scope_add_url(self->scope, urls[i], NULL))
      gh_auth_policy_apply_scope(auth, self->scope, GH_AUTH_PURPOSE_OWN_INBOX_READ, urls[i], NULL);
  gh_relay_scope_start(self->scope);
}
static void changed(GObject *source, gpointer data) { (void)source; rebuild(data); }
static void deny_changed(GObject *source, gpointer data)
{
  (void)source; GhCallSignalling *self = data;
  /* Revoke already-open writes too, even for a non-network test transport.
   * Snapshot refs because a failure observer can cancel other jobs. */
  GPtrArray *jobs = g_ptr_array_new_with_free_func((GDestroyNotify)send_unref);
  for (guint i = 0; i < self->jobs->len; i++)
    g_ptr_array_add(jobs, send_ref(g_ptr_array_index(self->jobs, i)));
  self->paused = TRUE;
  for (guint i = 0; i < jobs->len; i++) finish(jobs->pdata[i], FALSE);
  self->paused = FALSE; g_ptr_array_unref(jobs);
  rebuild(self);
}
void gh_call_signalling_stop(GhCallSignalling *self)
{
  self->stopped = TRUE; reset(self);
  if (self->accounts) g_signal_handlers_disconnect_by_data(self->accounts, self);
  if (self->relays) g_signal_handlers_disconnect_by_data(self->relays, self);
  g_signal_handlers_disconnect_by_data(gh_relay_deny_policy_get_default(), self);
}
const gchar *gh_call_signalling_get_pubkey(GhCallSignalling *self) { return self->pubkey; }
static void dispose(GObject *object)
{
  GhCallSignalling *self = GH_CALL_SIGNALLING(object);
  gh_call_signalling_stop(self);
  g_clear_object(&self->accounts); g_clear_object(&self->relays);
  g_clear_object(&self->resolver); g_clear_object(&self->settings);
  G_OBJECT_CLASS(gh_call_signalling_parent_class)->dispose(object);
}
static void finalize(GObject *object)
{
  GhCallSignalling *self = GH_CALL_SIGNALLING(object);
  g_ptr_array_unref(self->jobs); g_free(self->pubkey); g_free(self->trace_path);
  G_OBJECT_CLASS(gh_call_signalling_parent_class)->finalize(object);
}
static void gh_call_signalling_init(GhCallSignalling *self)
{ self->jobs = g_ptr_array_new_with_free_func((GDestroyNotify)send_unref); }
static void gh_call_signalling_class_init(GhCallSignallingClass *klass)
{
  GObjectClass *object = G_OBJECT_CLASS(klass); object->dispose = dispose; object->finalize = finalize;
  g_signal_new("event", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  g_signal_new("relay-status", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 3, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
  g_signal_new("reset", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  g_signal_new("failed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}
GhCallSignalling *gh_call_signalling_new(GhAccountController *accounts, GhAccountRelays *relays,
                                       GhInboxResolver *resolver, GSettings *settings)
{
  GhCallSignalling *self = g_object_new(GH_TYPE_CALL_SIGNALLING, NULL);
  self->accounts = g_object_ref(accounts); self->relays = g_object_ref(relays);
  self->resolver = g_object_ref(resolver); self->settings = g_object_ref(settings);
  const gchar *trace_path = g_getenv("GROUNDHOG_CALL_TRACE");
  if (trace_path && *trace_path) self->trace_path = g_strdup(trace_path);
  g_signal_connect(accounts, "changed", G_CALLBACK(changed), self);
  g_signal_connect(relays, "changed", G_CALLBACK(changed), self);
  g_signal_connect(gh_relay_deny_policy_get_default(), "changed", G_CALLBACK(deny_changed), self);
  rebuild(self); return self;
}
