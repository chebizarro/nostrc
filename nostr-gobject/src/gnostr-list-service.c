#include "gnostr-list-private.h"
#include "nostr-kinds.h"
#include "json.h"
#include "nostr-tag.h"
#include <string.h>
#include <stdlib.h>

/* Slots are stable until finalization, including entries removed from a plan;
 * asynchronous work never borrows a plan entry or application store. */
typedef struct {
  GNostrListReadPlanEntry plan;
  gpointer subscription;
  gboolean active, loaded, unreadable, waiting, failed, decrypt_pending, retry_signed, resume_pending, subscription_failed;
  gsize stored_bytes;
  NostrListDocument *base;
  char *base_json, *base_id, *blocked_json, *blocked_id;
  gint64 base_time, blocked_time;
  guint64 revision, remote_serial;
  GPtrArray *operations;
  GNostrListDocument *snapshot;
  json_t *sources, *outcomes;
  char *signed_json, *signed_base_id, *signed_fingerprint, *signed_private;
  guint signed_operations, included_operations;
  char **signed_targets;
  guint64 signed_revision;
  GNostrListApproval signed_approval;
  gpointer pipeline;
} ListSlot;
typedef enum { WORK_DECRYPT, WORK_ENCRYPT, WORK_SIGN, WORK_PUBLISH } WorkType;
typedef struct {
  GNostrListService *service;
  ListSlot *slot;
  WorkType type;
  guint64 revision, read_serial, remote_serial, policy_serial;
  char *base_id, *unsigned_json, *signed_json, *input;
  char **targets;
  NostrListDocument *codec;
  GNostrListApproval approval;
  GCancellable *cancel, *caller_cancel;
  gulong cancel_handler;
  GTask *task;
  GAsyncResult *result;
} ListWork;
struct _GNostrListService {
  GObject parent_instance;
  char *account;
  guint64 generation, read_serial, policy_serial;
  GNostrListAdapter adapter;
  gpointer data;
  GDestroyNotify destroy;
  GMainContext *context;
  GThread *thread;
  GPtrArray *slots, *works;
  GQueue crypto_queue;
  guint crypto_active;
  gboolean closed, sync_enabled, started, wake_pending;
};
static void list_model_init(GListModelInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GNostrListService, gnostr_list_service, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, list_model_init))
static guint signals[2];
static void wake(GNostrListService *s);
static void crypto_drain(GNostrListService *s);
static void publish_begin(GNostrListService *s, ListSlot *slot, GNostrListApproval approval, GTask *task);
static void accept_event(GNostrListService *s, ListSlot *slot, const char *wire, gboolean retry);
static char *fingerprint(const NostrListDocument *codec);
static void close_slot(GNostrListService *s, ListSlot *slot);

static gboolean owner(GNostrListService *s) { return g_thread_self() == s->thread; }
static gboolean valid(GNostrListService *s) {
  return !s->closed && s->adapter.generation_valid(s->data, s->account, s->generation);
}
static gint64 now(GNostrListService *s) { return s->adapter.now ? s->adapter.now(s->data) : g_get_real_time() / G_USEC_PER_SEC; }
static void dispatch(GNostrListService *s, GSourceFunc func, gpointer data, GDestroyNotify destroy) {
  GSource *source = g_idle_source_new();
  g_source_set_callback(source, func, data, destroy);
  g_source_attach(source, s->context); g_source_unref(source);
}
static ListSlot *find_slot(GNostrListService *s, int kind, const char *id) {
  for (guint i = 0; i < s->slots->len; i++) {
    ListSlot *d = s->slots->pdata[i];
    if (d->plan.kind == kind && !g_strcmp0(d->plan.identifier, id ? id : "")) return d;
  }
  return NULL;
}
static void plan_clear(GNostrListReadPlanEntry *p) {
  g_free((char *)p->identifier); g_strfreev((char **)p->sources); g_strfreev((char **)p->targets);
  g_free((char *)p->read_purpose); g_free((char *)p->publish_purpose);
}
static void plan_copy(GNostrListReadPlanEntry *to, const GNostrListReadPlanEntry *from) {
  *to = *from; to->identifier = g_strdup(from->identifier ? from->identifier : "");
  to->sources = (const char *const *)g_strdupv((char **)from->sources);
  to->targets = (const char *const *)g_strdupv((char **)from->targets);
  to->read_purpose = g_strdup(from->read_purpose); to->publish_purpose = g_strdup(from->publish_purpose);
}
static guint strv_len(const char *const *v) { return v ? g_strv_length((char **)v) : 0; }
static gboolean contains(const char *const *v, const char *str) {
  for (guint i = 0; v && v[i]; i++) if (!g_strcmp0(v[i], str)) return TRUE;
  return FALSE;
}
static void slot_free(gpointer data) {
  ListSlot *d = data; plan_clear(&d->plan);
  nostr_nip51_document_free(d->base); g_free(d->base_json); g_free(d->base_id);
  g_free(d->blocked_json); g_free(d->blocked_id); g_free(d->signed_json);
  g_free(d->signed_base_id); g_free(d->signed_fingerprint); g_free(d->signed_private);
  g_strfreev(d->signed_targets); g_ptr_array_unref(d->operations); g_clear_object(&d->snapshot);
  json_decref(d->sources); json_decref(d->outcomes); g_free(d);
}
static void gnostr_list_service_finalize(GObject *object) {
  GNostrListService *s = GNOSTR_LIST_SERVICE(object);
  gnostr_list_service_close(s);
  g_ptr_array_unref(s->slots); g_ptr_array_unref(s->works);
  if (s->destroy) s->destroy(s->data);
  g_main_context_unref(s->context); g_free(s->account);
  G_OBJECT_CLASS(gnostr_list_service_parent_class)->finalize(object);
}
static void gnostr_list_service_class_init(GNostrListServiceClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = gnostr_list_service_finalize;
  signals[0] = g_signal_new("document-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, GNOSTR_TYPE_LIST_DOCUMENT);
  signals[1] = g_signal_new("state-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void gnostr_list_service_init(GNostrListService *s) {
  s->context = g_main_context_ref_thread_default(); s->thread = g_thread_self();
  s->slots = g_ptr_array_new_with_free_func(slot_free); s->works = g_ptr_array_new(); g_queue_init(&s->crypto_queue);
}
static GType model_type(GListModel *model) { (void)model; return GNOSTR_TYPE_LIST_DOCUMENT; }
static guint model_count(GListModel *model) { return GNOSTR_LIST_SERVICE(model)->slots->len; }
static gpointer model_item(GListModel *model, guint position) {
  GNostrListService *s = GNOSTR_LIST_SERVICE(model);
  return position < s->slots->len ? g_object_ref(((ListSlot *)s->slots->pdata[position])->snapshot) : NULL;
}
static void list_model_init(GListModelInterface *iface) { iface->get_item_type = model_type; iface->get_n_items = model_count; iface->get_item = model_item; }
GNostrListService *gnostr_list_service_new(const char *account, guint64 generation, const GNostrListAdapter *a, gpointer data, GDestroyNotify destroy) {
  if (!account || strlen(account) != 64 || !a || !a->generation_valid || !a->load || !a->commit) return NULL;
  for (guint i = 0; i < 64; i++) if (!g_ascii_isxdigit(account[i]) || g_ascii_isupper(account[i])) return NULL;
  GNostrListService *s = g_object_new(GNOSTR_TYPE_LIST_SERVICE, NULL);
  s->account = g_strdup(account); s->generation = generation; s->adapter = *a; s->data = data; s->destroy = destroy;
  return s;
}
static gboolean sources_ready(ListSlot *d) {
  guint total = strv_len(d->plan.sources), eose = 0;
  for (guint i = 0; i < total; i++) if (json_integer_value(json_object_get(d->sources, d->plan.sources[i])) == GNOSTR_LIST_EOSE + 1) eose++;
  return total && (d->plan.required_sources == GNOSTR_LIST_SOURCES_ALL ? eose == total : eose > total / 2);
}
GNostrListState gnostr_list_service_get_state(GNostrListService *s, int kind, const char *id) {
  ListSlot *d = find_slot(s, kind, id);
  if (!d || !valid(s) || !d->active) return GNOSTR_LIST_INACTIVE;
  if (d->unreadable) return GNOSTR_LIST_UNREADABLE;
  if (d->pipeline) return GNOSTR_LIST_PUBLISHING;
  if (d->decrypt_pending) return GNOSTR_LIST_READING;
  if (d->plan.waiting_for_dependency || !sources_ready(d)) {
    if (d->plan.waiting_for_dependency) return GNOSTR_LIST_WAITING_FOR_DEPENDENCY;
    if (!strv_len(d->plan.sources)) return GNOSTR_LIST_LOCAL_ONLY;
    return json_object_size(d->sources) ? GNOSTR_LIST_PARTIAL : GNOSTR_LIST_READING;
  }
  if (d->waiting) return GNOSTR_LIST_WAITING_FOR_SIGNER;
  if (d->failed) return GNOSTR_LIST_FAILED;
  if (d->operations->len) return s->sync_enabled || d->plan.publish_policy == GNOSTR_LIST_AUTO_ALWAYS ? GNOSTR_LIST_DIRTY : GNOSTR_LIST_LOCAL_ONLY;
  return GNOSTR_LIST_SYNCED;
}
guint64 gnostr_list_service_get_read_serial(GNostrListService *s) { return s->read_serial; }
GNostrListDocument *gnostr_list_service_lookup(GNostrListService *s, int kind, const char *id) {
  g_return_val_if_fail(owner(s), NULL); ListSlot *d = find_slot(s, kind, id);
  return d ? g_object_ref(d->snapshot) : NULL;
}
static NostrListDocument *projection(ListSlot *d, GPtrArray *ops, GError **error) {
  NostrListDocument *codec = list_codec_copy(d->base);
  if (!codec) { list_fail(error, G_IO_ERROR_NO_SPACE, "Cannot copy list document"); return NULL; }
  for (guint i = d->included_operations; i < ops->len; i++) if (!list_operation_apply(codec, ops->pdata[i], error)) { nostr_nip51_document_free(codec); return NULL; }
  return codec;
}
static void refresh(GNostrListService *s, ListSlot *d, const char *failure) {
  GError *error = NULL;
  NostrListDocument *codec = projection(d, d->operations, &error);
  if (!codec) { g_clear_error(&error); d->failed = TRUE; return; }
  GNostrListDocument *snap = list_snapshot_new(s->account, codec);
  nostr_nip51_document_free(codec);
  if (!snap) { d->failed = TRUE; return; }
  snap->event_id = g_strdup(d->base_id); snap->source_json = g_strdup(d->base_json);
  snap->created_at = d->base_time; snap->revision = d->revision; snap->dirty = d->operations->len > 0;
  snap->error = g_strdup(failure);
  if (d->unreadable) nostr_nip51_document_mark_unreadable(snap->codec);
  g_free(snap->sources); snap->sources = list_json_dump(d->sources);
  g_free(snap->targets); snap->targets = list_json_dump(d->outcomes);
  g_set_object(&d->snapshot, snap);
  guint position = 0; g_ptr_array_find(s->slots, d, &position);
  g_list_model_items_changed(G_LIST_MODEL(s), position, 1, 1);
  g_signal_emit(s, signals[0], 0, snap); g_object_unref(snap);
  g_signal_emit(s, signals[1], 0);
}
static json_t *strings_json(char **v) {
  json_t *a = json_array(); for (guint i = 0; v && v[i]; i++) json_array_append_new(a, json_string(v[i])); return a;
}
static char **strings_parse(json_t *a) {
  if (!json_is_array(a) || json_array_size(a) > 16) return NULL;
  char **v = g_new0(char *, json_array_size(a) + 1);
  for (guint i = 0; i < json_array_size(a); i++) {
    const char *str = json_string_value(json_array_get(a, i));
    if (!str || strlen(str) > 4096) { g_strfreev(v); return NULL; }
    v[i] = g_strdup(str);
  }
  return v;
}
static gboolean persist(GNostrListService *s, ListSlot *d, GError **error) {
  if (!valid(s)) return list_fail(error, G_IO_ERROR_CANCELLED, "List account generation revoked");
  json_t *ops = json_array();
  for (guint i = 0; i < d->operations->len; i++) json_array_append_new(ops, list_operation_json(d->operations->pdata[i]));
  char *plain = nostr_nip51_document_serialize_private_tags(d->base);
  json_t *j = json_pack("{s:i,s:s,s:s,s:s,s:I,s:o,s:s,s:s,s:I,s:i,s:o,s:O}",
    "version", 1, "account", s->account, "base", d->base_json ? d->base_json : "",
    "private", plain ? plain : "[]", "revision", (json_int_t)d->revision,
    "operations", ops, "blocked", d->blocked_json ? d->blocked_json : "",
    "signed", d->signed_json ? d->signed_json : "", "signed_revision", (json_int_t)d->signed_revision,
    "approval", d->signed_approval, "targets", strings_json(d->signed_targets), "outcomes", d->outcomes);
  free(plain);
  json_object_set_new(j, "signed_base_id", json_string(d->signed_base_id ? d->signed_base_id : ""));
  json_object_set_new(j, "signed_fingerprint", json_string(d->signed_fingerprint ? d->signed_fingerprint : ""));
  json_object_set_new(j, "signed_private", json_string(d->signed_private ? d->signed_private : "[]"));
  json_object_set_new(j, "signed_operations", json_integer(d->signed_operations));
  json_object_set_new(j, "included_operations", json_integer(d->included_operations));
  json_object_set_new(j, "blocked_id", json_string(d->blocked_id ? d->blocked_id : ""));
  json_object_set_new(j, "blocked_time", json_integer(d->blocked_time));
  char *wire = list_json_dump(j); json_decref(j);
  gsize bytes = wire ? strlen(wire) : 0, total = bytes;
  for (guint i = 0; i < s->slots->len; i++) { ListSlot *other = s->slots->pdata[i]; if (other != d) total += other->stored_bytes; }
  if (!wire || total > 32u * 1024u * 1024u) { free(wire); return list_fail(error, G_IO_ERROR_NO_SPACE, "List account state exceeds storage bound"); }
  gboolean ok = s->adapter.commit(s->data, d->plan.kind, d->plan.identifier, wire, error);
  if (ok) d->stored_bytes = bytes;
  free(wire); return ok;
}
static NostrEvent *verified(GNostrListService *s, ListSlot *d, const char *wire) {
  if (!wire || strlen(wire) > NOSTR_LIST_MAX_FOLLOW_BYTES) return NULL;
  NostrEvent *event = nostr_event_new();
  if (nostr_event_deserialize_signed(event, wire, NULL) != NOSTR_EVENT_VALIDATION_OK ||
      !nostr_event_check_signature(event) || g_strcmp0(nostr_event_get_pubkey(event), s->account) ||
      nostr_event_get_kind(event) != d->plan.kind || nostr_event_get_created_at(event) < 0 ||
      nostr_event_get_created_at(event) > now(s) + 600) { nostr_event_free(event); return NULL; }
  /* Address validation uses the lossless codec, not an independent tag parser. */
  NostrListDocument *codec = nostr_nip51_document_parse_event(event);
  gboolean match = codec && !nostr_nip51_document_validate(codec) && !g_strcmp0(nostr_nip51_document_identifier(codec), d->plan.identifier);
  nostr_nip51_document_free(codec);
  if (!match) { nostr_event_free(event); return NULL; }
  return event;
}
static gboolean newer(gint64 time, const char *id, gint64 old_time, const char *old_id) {
  return !old_id || time > old_time || (time == old_time && strcmp(id, old_id) < 0);
}
static void clear_signed(ListSlot *d) {
  g_clear_pointer(&d->signed_json, g_free); g_clear_pointer(&d->signed_targets, g_strfreev);
  g_clear_pointer(&d->signed_base_id, g_free); g_clear_pointer(&d->signed_fingerprint, g_free); g_clear_pointer(&d->signed_private, g_free);
  d->signed_operations = 0;
  d->signed_revision = 0; d->signed_approval = GNOSTR_LIST_APPROVE_NONE;
}
static gboolean restore(GNostrListService *s, ListSlot *d, GError **error) {
  if (!valid(s)) return list_fail(error, G_IO_ERROR_CANCELLED, "List account generation revoked");
  char *wire = s->adapter.load(s->data, d->plan.kind, d->plan.identifier, error);
  if (!wire) return !error || !*error;
  if (strlen(wire) > 32u * 1024u * 1024u) { g_free(wire); return list_fail(error, G_IO_ERROR_INVALID_DATA, "Stored list exceeds bound"); }
  d->stored_bytes = strlen(wire);
  json_t *j = json_loads(wire, JSON_REJECT_DUPLICATES, NULL); g_free(wire);
  const char *base = json_string_value(json_object_get(j, "base"));
  const char *plain = json_string_value(json_object_get(j, "private"));
  json_t *ops = json_object_get(j, "operations");
  gboolean ok = j && json_integer_value(json_object_get(j, "version")) == 1 &&
    !g_strcmp0(json_string_value(json_object_get(j, "account")), s->account) && base && plain && json_is_array(ops) && json_array_size(ops) <= NOSTR_LIST_MAX_TAGS;
  if (ok && *base) {
    NostrEvent *ev = verified(s, d, base); ok = ev != NULL;
    if (ev) {
      NostrListDocument *codec = nostr_nip51_document_parse_event(ev);
      ok = codec && !nostr_nip51_document_parse_private_tags(codec, plain, strlen(plain));
      if (ok) { nostr_nip51_document_free(d->base); d->base = codec; d->base_json = g_strdup(base); d->base_id = nostr_event_get_id(ev); d->base_time = nostr_event_get_created_at(ev); }
      else nostr_nip51_document_free(codec);
      nostr_event_free(ev);
    }
  }
  for (guint i = 0; ok && i < json_array_size(ops); i++) {
    GNostrListOperation *op = list_operation_parse(json_array_get(ops, i));
    if (!op) ok = FALSE; else g_ptr_array_add(d->operations, op);
  }
  json_int_t rev = json_integer_value(json_object_get(j, "revision"));
  if (rev < 0) ok = FALSE; else d->revision = rev;
  json_int_t included = json_integer_value(json_object_get(j, "included_operations"));
  if (included < 0 || (guint64)included > d->operations->len) ok = FALSE; else d->included_operations = included;
  NostrListDocument *check = ok ? projection(d, d->operations, NULL) : NULL;
  if (!check) ok = FALSE; nostr_nip51_document_free(check);
  const char *blocked = json_string_value(json_object_get(j, "blocked"));
  if (ok && blocked && *blocked) {
    NostrEvent *ev = verified(s, d, blocked);
    if (ev) { d->blocked_json = g_strdup(blocked); d->blocked_id = nostr_event_get_id(ev); d->blocked_time = nostr_event_get_created_at(ev); d->unreadable = TRUE; nostr_event_free(ev); }
    else ok = FALSE;
  }
  const char *signed_json = json_string_value(json_object_get(j, "signed"));
  if (ok && signed_json && *signed_json) {
    /* Exact payload and targets are checked again immediately before sending,
     * once current settings and discovery have been installed. */
    d->signed_json = g_strdup(signed_json); d->resume_pending = TRUE;
    d->signed_targets = strings_parse(json_object_get(j, "targets"));
    d->signed_revision = json_integer_value(json_object_get(j, "signed_revision"));
    d->signed_approval = json_integer_value(json_object_get(j, "approval"));
    d->signed_base_id = g_strdup(json_string_value(json_object_get(j, "signed_base_id")));
    d->signed_fingerprint = g_strdup(json_string_value(json_object_get(j, "signed_fingerprint")));
    d->signed_private = g_strdup(json_string_value(json_object_get(j, "signed_private")));
    json_int_t n = json_integer_value(json_object_get(j, "signed_operations"));
    if (n < 0 || n > NOSTR_LIST_MAX_TAGS) ok = FALSE; else d->signed_operations = n;
  }
  const char *blocked_id = json_string_value(json_object_get(j, "blocked_id"));
  if (ok && !d->blocked_id && blocked_id && strlen(blocked_id) == 64) {
    d->blocked_id = g_strdup(blocked_id); d->blocked_time = json_integer_value(json_object_get(j, "blocked_time")); d->unreadable = TRUE;
  }
  json_t *outcomes = json_object_get(j, "outcomes");
  if (json_is_object(outcomes)) { json_decref(d->outcomes); d->outcomes = json_deep_copy(outcomes); }
  json_decref(j);
  return ok ? TRUE : list_fail(error, G_IO_ERROR_INVALID_DATA, "Invalid stored list state");
}

static gboolean plan_valid(const GNostrListReadPlanEntry *p) {
  const NostrListSchema *schema = nostr_nip51_schema_lookup(p->kind);
  if (!schema || p->publish_policy > GNOSTR_LIST_READ_ONLY || p->publish_policy < 0 ||
      p->required_sources > GNOSTR_LIST_SOURCES_MAJORITY || p->required_sources < 0 ||
      strv_len(p->sources) > 16 || strv_len(p->targets) > 16 ||
      (p->publish_policy == GNOSTR_LIST_AUTO_ALWAYS && p->kind != NOSTR_KIND_SIMPLE_GROUP_LIST) ||
      (p->kind == NOSTR_KIND_SIMPLE_GROUP_LIST && p->required_sources != GNOSTR_LIST_SOURCES_ALL) ||
      (nostr_nip51_schema_read_only(schema) && p->publish_policy != GNOSTR_LIST_READ_ONLY)) return FALSE;
  NostrListDocument *d = nostr_nip51_document_new(p->kind,
    nostr_nip51_schema_requires_identifier(schema) ? (p->identifier ? p->identifier : "") : NULL);
  gboolean ok = d && (!p->identifier || nostr_nip51_schema_requires_identifier(schema) || !*p->identifier);
  nostr_nip51_document_free(d);
  for (guint i = 0; ok && p->sources && p->sources[i]; i++) {
    if (!*p->sources[i] || strlen(p->sources[i]) > 4096) return FALSE;
    for (guint j = 0; j < i; j++) if (!strcmp(p->sources[i], p->sources[j])) return FALSE;
  }
  return ok;
}
gboolean gnostr_list_service_update_read_plan(GNostrListService *s, const GNostrListReadPlanEntry *entries, gsize count, GError **error) {
  g_return_val_if_fail(owner(s), FALSE);
  if (!valid(s)) return list_fail(error, G_IO_ERROR_CANCELLED, "List account generation revoked");
  if (count > 256 || (!entries && count)) return list_fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Invalid list read plan");
  for (gsize i = 0; i < count; i++) {
    if (!plan_valid(&entries[i])) return list_fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Invalid list policy or address");
    for (gsize j = 0; j < i; j++) if (entries[j].kind == entries[i].kind && !g_strcmp0(entries[j].identifier, entries[i].identifier))
      return list_fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Duplicate list address");
  }
  s->read_serial++; s->policy_serial++;
  for (guint i = 0; i < s->slots->len; i++) { ListSlot *d = s->slots->pdata[i]; close_slot(s, d); d->active = FALSE; }
  gboolean success = TRUE;
  for (gsize i = 0; i < count && valid(s); i++) {
    ListSlot *d = find_slot(s, entries[i].kind, entries[i].identifier);
    if (!d) {
      d = g_new0(ListSlot, 1); plan_copy(&d->plan, &entries[i]);
      d->operations = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
      d->sources = json_object(); d->outcomes = json_object();
      const NostrListSchema *schema = nostr_nip51_schema_lookup(d->plan.kind);
      d->base = nostr_nip51_document_new(d->plan.kind, nostr_nip51_schema_requires_identifier(schema) ? d->plan.identifier : NULL);
      d->snapshot = list_snapshot_new(s->account, d->base);
      g_ptr_array_add(s->slots, d); g_list_model_items_changed(G_LIST_MODEL(s), s->slots->len - 1, 0, 1);
    } else { plan_clear(&d->plan); plan_copy(&d->plan, &entries[i]); }
    d->active = TRUE; json_object_clear(d->sources);
    if (!d->loaded) {
      d->loaded = TRUE;
      GError *load_error = NULL;
      if (!restore(s, d, &load_error)) { d->failed = d->unreadable = TRUE; success = FALSE; if (error && !*error) *error = g_steal_pointer(&load_error); g_clear_error(&load_error); }
    }
    if (d->subscription_failed) { d->failed = d->subscription_failed = FALSE; }
    if (!d->plan.waiting_for_dependency && strv_len(d->plan.sources)) {
      if (!s->adapter.open_subscription || !s->adapter.close_subscription) {
        d->failed = TRUE; success = FALSE; list_fail(error && !*error ? error : NULL, G_IO_ERROR_NOT_SUPPORTED, "List subscription adapter unavailable");
      } else if (valid(s)) {
        GError *open_error = NULL;
        d->subscription = s->adapter.open_subscription(s->data, s, s->account, s->generation, s->read_serial, &d->plan, &open_error);
        if (!d->subscription) { d->failed = d->subscription_failed = TRUE; success = FALSE; if (error && !*error) *error = open_error; else g_clear_error(&open_error); }
      }
    }
    refresh(s, d, d->failed ? "List adapter failed" : NULL);
  }
  s->started = TRUE; wake(s); return success;
}
gboolean gnostr_list_service_start(GNostrListService *s, const GNostrListReadPlanEntry *entries, gsize count, GError **error) {
  return gnostr_list_service_update_read_plan(s, entries, count, error);
}
GNostrListDocument *gnostr_list_service_apply(GNostrListService *s, int kind, const char *id, guint64 expected, GPtrArray *operations, GError **error) {
  g_return_val_if_fail(owner(s), NULL);
  ListSlot *d = find_slot(s, kind, id);
  if (!valid(s) || !d || !d->active) { list_fail(error, G_IO_ERROR_CLOSED, "List service is inactive"); return NULL; }
  if (d->plan.publish_policy == GNOSTR_LIST_READ_ONLY || nostr_nip51_schema_read_only(nostr_nip51_schema_lookup(kind))) { list_fail(error, G_IO_ERROR_PERMISSION_DENIED, "List is read only"); return NULL; }
  if (d->revision != expected) { list_fail(error, G_IO_ERROR_WRONG_ETAG, "List local revision changed"); return NULL; }
  if (!operations || !operations->len) return g_object_ref(d->snapshot);
  if (operations->len + d->operations->len > NOSTR_LIST_MAX_TAGS || d->revision == G_MAXINT64) { list_fail(error, G_IO_ERROR_NO_SPACE, "Too many pending list operations"); return NULL; }
  GPtrArray *next = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  for (guint i = 0; i < d->operations->len; i++) g_ptr_array_add(next, gnostr_list_operation_ref(d->operations->pdata[i]));
  for (guint i = 0; i < operations->len; i++) {
    if (!operations->pdata[i]) { g_ptr_array_unref(next); list_fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Missing list operation"); return NULL; }
    g_ptr_array_add(next, gnostr_list_operation_ref(operations->pdata[i]));
  }
  NostrListDocument *check = projection(d, next, error);
  if (!check) { g_ptr_array_unref(next); return NULL; } nostr_nip51_document_free(check);
  GPtrArray *old = d->operations; d->operations = next; d->revision++;
  if (!persist(s, d, error)) { d->revision--; d->operations = old; g_ptr_array_unref(next); return NULL; }
  g_ptr_array_unref(old); d->failed = d->waiting = FALSE;
  refresh(s, d, NULL); wake(s); return g_object_ref(d->snapshot);
}

static void work_free(ListWork *w) {
  if (w->caller_cancel && w->cancel_handler) g_cancellable_disconnect(w->caller_cancel, w->cancel_handler);
  g_clear_object(&w->caller_cancel); g_clear_object(&w->cancel); g_clear_object(&w->task); g_clear_object(&w->result);
  g_free(w->base_id); g_free(w->unsigned_json); g_free(w->signed_json); g_free(w->input);
  g_strfreev(w->targets); nostr_nip51_document_free(w->codec);
  g_ptr_array_remove_fast(w->service->works, w);
  g_object_unref(w->service); g_free(w);
}
static ListWork *work_new(GNostrListService *s, ListSlot *d, WorkType type) {
  ListWork *w = g_new0(ListWork, 1); w->service = g_object_ref(s); w->slot = d; w->type = type;
  w->revision = d->revision; w->base_id = g_strdup(d->base_id); w->read_serial = s->read_serial;
  w->remote_serial = d->remote_serial; w->policy_serial = s->policy_serial;
  w->cancel = g_cancellable_new(); g_ptr_array_add(s->works, w); return w;
}
static gboolean work_valid(ListWork *w) {
  GNostrListService *s = w->service; ListSlot *d = w->slot;
  return valid(s) && d->active && !g_cancellable_is_cancelled(w->cancel) && w->read_serial == s->read_serial &&
    (w->type == WORK_DECRYPT ? w->remote_serial == d->remote_serial :
     w->revision == d->revision && w->remote_serial == d->remote_serial && !d->blocked_id &&
     w->policy_serial == s->policy_serial && !g_strcmp0(w->base_id, d->base_id));
}
static void work_end(ListWork *w, GError *error) {
  GNostrListService *s = w->service; ListSlot *d = w->slot;
  if (d->pipeline == w) d->pipeline = NULL;
  if (w->task) {
    if (error) g_task_return_error(w->task, error);
    else g_task_return_new_error(w->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "List publication superseded");
  } else g_clear_error(&error);
  if (valid(s)) { refresh(s, d, d->failed ? "List operation failed" : NULL); wake(s); }
  work_free(w);
}
static gboolean work_result(gpointer data);
static void completed(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; ListWork *w = data; w->result = g_object_ref(result);
  dispatch(w->service, work_result, w, NULL);
}
static void work_start(ListWork *w) {
  GNostrListService *s = w->service;
  if (!work_valid(w)) { work_end(w, NULL); return; }
  if (w->type == WORK_DECRYPT) {
    s->crypto_active++;
    s->adapter.decrypt_async(s->data, s->account, w->input,
      nostr_nip51_document_private_state(w->codec) == NOSTR_LIST_PRIVATE_LEGACY_NIP04,
      w->cancel, completed, w);
  } else if (w->type == WORK_ENCRYPT) {
    s->crypto_active++;
    s->adapter.encrypt_async(s->data, s->account, w->input, w->cancel, completed, w);
  } else if (w->type == WORK_SIGN) {
    s->adapter.sign_async(s->data, w->unsigned_json, w->cancel, completed, w);
  } else {
    s->adapter.publish_async(s->data, w->signed_json, (const char *const *)w->targets,
      w->slot->plan.publish_purpose, w->cancel, completed, w);
  }
}
static void crypto_drain(GNostrListService *s) {
  while (s->crypto_active < 16 && !g_queue_is_empty(&s->crypto_queue)) work_start(g_queue_pop_head(&s->crypto_queue));
}
static void crypto_enqueue(ListWork *w) {
  GNostrListService *s = w->service;
  g_queue_push_tail(&s->crypto_queue, w); crypto_drain(s);
}
static void publish_fail(ListWork *w, GError *error, gboolean waiting) {
  if (work_valid(w)) { w->slot->waiting = waiting; w->slot->failed = !waiting; }
  if (!error) error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "List adapter failed");
  work_end(w, error);
}
static void install_remote(GNostrListService *s, ListSlot *d, NostrListDocument *codec, const char *wire, const char *id, gint64 at, guint included) {
  NostrListDocument *old_base = d->base;
  guint old_included = d->included_operations; d->included_operations = included;
  char *old_json = d->base_json, *old_id = d->base_id;
  gint64 old_time = d->base_time, old_blocked_time = d->blocked_time;
  char *old_blocked_json = d->blocked_json, *old_blocked_id = d->blocked_id;
  d->blocked_json = d->blocked_id = NULL; d->blocked_time = 0;
  d->base = codec; d->base_json = g_strdup(wire); d->base_id = g_strdup(id); d->base_time = at;
  GError *error = NULL;
  NostrListDocument *check = projection(d, d->operations, &error);
  gboolean ok = check && persist(s, d, &error);
  nostr_nip51_document_free(check);
  if (!ok) {
    nostr_nip51_document_free(d->base); g_free(d->base_json); g_free(d->base_id);
    d->base = old_base; d->base_json = old_json; d->base_id = old_id; d->base_time = old_time;
    d->blocked_json = old_blocked_json; d->blocked_id = old_blocked_id; d->blocked_time = old_blocked_time;
    d->included_operations = old_included;
    d->failed = TRUE;
  } else {
    nostr_nip51_document_free(old_base); g_free(old_json); g_free(old_id);
    d->unreadable = d->waiting = d->failed = FALSE;
    g_free(old_blocked_json); g_free(old_blocked_id);
  }
  g_clear_error(&error); refresh(s, d, ok ? NULL : "Cannot commit remote list"); wake(s);
}
static void unreadable(GNostrListService *s, ListSlot *d, const char *wire, const char *id, gint64 at, gboolean pending) {
  d->unreadable = !pending; d->decrypt_pending = pending; d->waiting = FALSE;
  g_free(d->blocked_json); d->blocked_json = g_strdup(wire);
  g_free(d->blocked_id); d->blocked_id = g_strdup(id); d->blocked_time = at;
  GError *error = NULL;
  if (!persist(s, d, &error)) d->failed = TRUE;
  g_clear_error(&error); refresh(s, d, "Unreadable list revision");
}
static void accept_event(GNostrListService *s, ListSlot *d, const char *wire, gboolean retry) {
  if (!valid(s) || !d->active) return;
  NostrEvent *ev = verified(s, d, wire);
  if (!ev) {
    /* The codec deliberately refuses oversized/invalid documents. Authenticate
     * the envelope and its routing address only, then retain a bounded head
     * record rather than letting an older usable revision overwrite it. */
    NostrEvent *raw = nostr_event_new();
    gboolean authenticated = wire && strlen(wire) <= NOSTR_LIST_MAX_FOLLOW_BYTES &&
      nostr_event_deserialize_signed(raw, wire, NULL) == NOSTR_EVENT_VALIDATION_OK && nostr_event_check_signature(raw) &&
      !g_strcmp0(nostr_event_get_pubkey(raw), s->account) && nostr_event_get_kind(raw) == d->plan.kind &&
      nostr_event_get_created_at(raw) >= 0 && nostr_event_get_created_at(raw) <= now(s) + 600;
    const NostrListSchema *schema = nostr_nip51_schema_lookup(d->plan.kind);
    const char *identifier = "";
    if (authenticated && nostr_nip51_schema_requires_identifier(schema)) {
      identifier = NULL;
      NostrTags *tags = nostr_event_get_tags(raw);
      for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (!g_strcmp0(nostr_tag_get(tag, 0), "d")) { if (identifier) { authenticated = FALSE; break; } identifier = nostr_tag_get(tag, 1); }
      }
    }
    char *id = authenticated ? nostr_event_get_id(raw) : NULL;
    gint64 at = nostr_event_get_created_at(raw);
    if (authenticated && !g_strcmp0(identifier, d->plan.identifier) && newer(at, id, d->base_time, d->base_id) &&
        newer(at, id, d->blocked_time, d->blocked_id)) {
      d->remote_serial++; d->decrypt_pending = FALSE;
      unreadable(s, d, NULL, id, at, FALSE);
    }
    free(id); nostr_event_free(raw);
    refresh(s, d, "Invalid or oversized list revision"); return;
  }
  char *id = nostr_event_get_id(ev); gint64 at = nostr_event_get_created_at(ev);
  if (!newer(at, id, d->base_time, d->base_id) ||
      (!retry && d->blocked_id && !newer(at, id, d->blocked_time, d->blocked_id))) goto done;
  d->remote_serial++; d->decrypt_pending = FALSE;
  NostrListDocument *codec = nostr_nip51_document_parse_event(ev);
  NostrListPrivateState state = nostr_nip51_document_private_state(codec);
  NostrEvent *stored_signed = d->signed_json ? verified(s, d, d->signed_json) : NULL;
  char *stored_id = stored_signed ? nostr_event_get_id(stored_signed) : NULL;
  gboolean own_revision = stored_id && !strcmp(id, stored_id);
  free(stored_id); nostr_event_free(stored_signed);
  if (own_revision && d->signed_private &&
      !nostr_nip51_document_parse_private_tags(codec, d->signed_private, strlen(d->signed_private))) {
    char *hash = fingerprint(codec);
    gboolean same = hash && !g_strcmp0(hash, d->signed_fingerprint); g_free(hash);
    if (same) { install_remote(s, d, codec, wire, id, at, MIN(d->signed_operations, d->operations->len)); goto done; }
  }
  if (state == NOSTR_LIST_PRIVATE_EMPTY) install_remote(s, d, codec, wire, id, at, 0);
  else if (!s->adapter.decrypt_async || !s->adapter.decrypt_finish) {
    nostr_nip51_document_free(codec); unreadable(s, d, wire, id, at, FALSE); d->waiting = TRUE;
  } else {
    /* Remember the head before decrypting: older events cannot replace it while
     * crypto is queued. Failed decryption retains the last usable projection. */
    unreadable(s, d, wire, id, at, TRUE);
    ListWork *w = work_new(s, d, WORK_DECRYPT); w->codec = codec; w->signed_json = g_strdup(wire);
    d->decrypt_pending = TRUE; d->unreadable = FALSE;
    w->input = g_strdup(nostr_event_get_content(ev)); crypto_enqueue(w);
  }
done:
  free(id); nostr_event_free(ev);
}
static gboolean same_unsigned(const char *expected, NostrEvent *signed_event) {
  NostrEvent *e = nostr_event_new(); char a[65], b[65];
  gboolean ok = nostr_event_deserialize_compact(e, expected, NULL) == 1 &&
    nostr_event_compute_id(e, a) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_compute_id(signed_event, b) == NOSTR_EVENT_VALIDATION_OK && !strcmp(a, b);
  nostr_event_free(e); return ok;
}
static char *fingerprint(const NostrListDocument *codec) {
  json_t *root = json_array();
  for (int p = 0; p < 2; p++) {
    json_t *tags = json_array();
    for (size_t i = 0; i < nostr_nip51_document_tag_count(codec, p); i++) {
      const NostrListTag *tag = nostr_nip51_document_tag(codec, p, i); json_t *row = json_array();
      for (size_t j = 0; j < nostr_nip51_tag_size(tag); j++) json_array_append_new(row, json_string(nostr_nip51_tag_field(tag, j)));
      json_array_append_new(tags, row);
    }
    json_array_append_new(root, tags);
  }
  char *wire = list_json_dump(root); json_decref(root);
  char *hash = wire ? g_compute_checksum_for_string(G_CHECKSUM_SHA256, wire, -1) : NULL; free(wire); return hash;
}
static gboolean resume_valid(GNostrListService *s, ListSlot *d) {
  if (!d->signed_json || d->signed_revision != d->revision || d->unreadable ||
      !strv_len((const char *const *)d->signed_targets)) return FALSE;
  for (guint i = 0; d->signed_targets[i]; i++) if (!contains(d->plan.targets, d->signed_targets[i])) return FALSE;
  NostrEvent *ev = verified(s, d, d->signed_json);
  if (!ev) return FALSE;
  char *id = nostr_event_get_id(ev);
  gboolean ok = !g_strcmp0(id, d->base_id) || newer(nostr_event_get_created_at(ev), id, d->base_time, d->base_id);
  NostrListDocument *codec = projection(d, d->operations, NULL);
  char *hash = codec ? fingerprint(codec) : NULL;
  ok = ok && hash && !g_strcmp0(hash, d->signed_fingerprint) &&
    (!g_strcmp0(id, d->base_id) || !g_strcmp0(d->signed_base_id, d->base_id ? d->base_id : ""));
  g_free(hash);
  NostrEvent *expected = codec ? nostr_nip51_document_build_unsigned(codec, s->account,
    nostr_event_get_created_at(ev), nostr_event_get_content(ev)) : NULL;
  char *json = expected ? nostr_event_serialize(expected) : NULL;
  ok = ok && json && same_unsigned(json, ev);
  free(json); free(id); nostr_event_free(expected); nostr_nip51_document_free(codec); nostr_event_free(ev);
  return ok;
}
static gboolean work_result(gpointer data) {
  ListWork *w = data; GNostrListService *s = w->service; ListSlot *d = w->slot;
  GError *error = NULL; char *text = NULL; GVariant *outcomes = NULL;
  /* Always drain finish(), including revoked/cancelled results, before dropping
   * adapter data. No effects are allowed until the captured tokens are checked. */
  if (w->type == WORK_DECRYPT) text = s->adapter.decrypt_finish(s->data, w->result, &error);
  else if (w->type == WORK_ENCRYPT) text = s->adapter.encrypt_finish(s->data, w->result, &error);
  else if (w->type == WORK_SIGN) text = s->adapter.sign_finish(s->data, w->result, &error);
  else outcomes = s->adapter.publish_finish(s->data, w->result, &error);
  g_clear_object(&w->result);
  if (w->type == WORK_DECRYPT || w->type == WORK_ENCRYPT) { s->crypto_active--; crypto_drain(s); }
  gboolean publication_current = w->type == WORK_PUBLISH && valid(s) && d->active &&
    !g_cancellable_is_cancelled(w->cancel) && !g_strcmp0(w->signed_json, d->signed_json);
  if (!work_valid(w) && !publication_current) { g_free(text); g_clear_pointer(&outcomes, g_variant_unref); g_clear_error(&error); work_end(w, NULL); return G_SOURCE_REMOVE; }
  if (w->type == WORK_DECRYPT) {
    d->decrypt_pending = FALSE;
    if (!error && text && !nostr_nip51_document_parse_private_tags(w->codec, text, strlen(text)) && !nostr_nip51_document_validate(w->codec)) {
      NostrEvent *ev = verified(s, d, w->signed_json);
      if (ev) {
        char *id = nostr_event_get_id(ev); NostrListDocument *codec = w->codec; w->codec = NULL;
        install_remote(s, d, codec, w->signed_json, id, nostr_event_get_created_at(ev), 0);
        free(id); nostr_event_free(ev);
      }
    } else { d->unreadable = TRUE; refresh(s, d, "Unreadable private list"); }
    g_free(text); g_clear_error(&error); work_free(w); return G_SOURCE_REMOVE;
  }
  if (error || (w->type != WORK_PUBLISH && (!text || !*text))) {
    g_free(text); g_clear_pointer(&outcomes, g_variant_unref);
    publish_fail(w, error, w->type != WORK_PUBLISH); return G_SOURCE_REMOVE;
  }
  if (w->type == WORK_ENCRYPT) {
    NostrEvent *ev = nostr_nip51_document_build_unsigned(w->codec, s->account, MAX(now(s), d->base_time + 1), text);
    g_free(text);
    if (!ev) { publish_fail(w, NULL, FALSE); return G_SOURCE_REMOVE; }
    w->unsigned_json = nostr_event_serialize(ev); nostr_event_free(ev); w->type = WORK_SIGN;
    if (!w->unsigned_json) { publish_fail(w, NULL, FALSE); return G_SOURCE_REMOVE; }
    work_start(w); return G_SOURCE_REMOVE;
  }
  if (w->type == WORK_SIGN) {
    NostrEvent *ev = verified(s, d, text);
    if (!ev || !same_unsigned(w->unsigned_json, ev)) {
      nostr_event_free(ev); g_free(text);
      publish_fail(w, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Signer changed the approved list event"), FALSE); return G_SOURCE_REMOVE;
    }
    nostr_event_free(ev); clear_signed(d); json_object_clear(d->outcomes);
    d->signed_json = g_strdup(text); d->signed_targets = g_strdupv(w->targets);
    d->signed_revision = w->revision; d->signed_approval = w->approval;
    d->signed_base_id = g_strdup(w->base_id ? w->base_id : "");
    d->signed_fingerprint = fingerprint(w->codec);
    d->signed_private = nostr_nip51_document_serialize_private_tags(w->codec);
    d->signed_operations = d->operations->len;
    if (!persist(s, d, &error)) { clear_signed(d); g_free(text); publish_fail(w, error, FALSE); return G_SOURCE_REMOVE; }
    w->signed_json = text; w->type = WORK_PUBLISH; work_start(w); return G_SOURCE_REMOVE;
  }
  if (!outcomes || !g_variant_is_of_type(outcomes, G_VARIANT_TYPE("a{si}"))) {
    g_clear_pointer(&outcomes, g_variant_unref); publish_fail(w, NULL, FALSE); return G_SOURCE_REMOVE;
  }
  gboolean accepted = FALSE;
  gboolean all_accepted = TRUE;
  for (guint i = 0; w->targets && w->targets[i]; i++) {
    gint outcome = GNOSTR_LIST_TRANSPORT_FAILURE;
    g_variant_lookup(outcomes, w->targets[i], "i", &outcome);
    if (outcome < GNOSTR_LIST_ACCEPTED || outcome > GNOSTR_LIST_CANCELLED) outcome = GNOSTR_LIST_TRANSPORT_FAILURE;
    json_object_set_new(d->outcomes, w->targets[i], json_integer(outcome));
    accepted |= outcome == GNOSTR_LIST_ACCEPTED || outcome == GNOSTR_LIST_DUPLICATE;
  }
  for (guint i = 0; d->signed_targets && d->signed_targets[i]; i++) {
    json_t *value = json_object_get(d->outcomes, d->signed_targets[i]);
    int outcome = value ? json_integer_value(value) : GNOSTR_LIST_TRANSPORT_FAILURE;
    gboolean ok = outcome == GNOSTR_LIST_ACCEPTED || outcome == GNOSTR_LIST_DUPLICATE;
    accepted |= ok; all_accepted &= ok;
  }
  if (accepted) {
    NostrEvent *ev = verified(s, d, w->signed_json);
    if (!ev) { g_variant_unref(outcomes); publish_fail(w, NULL, FALSE); return G_SOURCE_REMOVE; }
    char *accepted_id = nostr_event_get_id(ev);
    gboolean current_head = (!d->base_id || !g_strcmp0(accepted_id, d->base_id) ||
      newer(nostr_event_get_created_at(ev), accepted_id, d->base_time, d->base_id)) &&
      (!d->blocked_id || newer(nostr_event_get_created_at(ev), accepted_id, d->blocked_time, d->blocked_id));
    free(accepted_id);
    if (!current_head) { nostr_event_free(ev); goto acceptance_recorded; }
    NostrListDocument *old_base = d->base;
    char *old_json = d->base_json, *old_id = d->base_id;
    gint64 old_time = d->base_time;
    GPtrArray *old_ops = d->operations;
    d->base = list_codec_copy(w->codec ? w->codec : d->snapshot->codec);
    d->base_json = g_strdup(w->signed_json); d->base_id = nostr_event_get_id(ev); d->base_time = nostr_event_get_created_at(ev);
    guint old_included = d->included_operations; d->included_operations = 0;
    d->operations = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
    /* Retire only operations represented by this immutable signed event. A
     * newer local tail remains dirty and is rebased onto the accepted head. */
    for (guint i = MIN(d->signed_operations, old_ops->len); i < old_ops->len; i++)
      g_ptr_array_add(d->operations, gnostr_list_operation_ref(old_ops->pdata[i]));
    guint old_signed_operations = d->signed_operations; d->signed_operations = 0;
    if (!persist(s, d, &error)) {
      nostr_nip51_document_free(d->base); g_free(d->base_json); g_free(d->base_id); g_ptr_array_unref(d->operations);
      d->base = old_base; d->base_json = old_json; d->base_id = old_id; d->base_time = old_time; d->operations = old_ops;
      d->included_operations = old_included; d->signed_operations = old_signed_operations;
      nostr_event_free(ev); g_variant_unref(outcomes); publish_fail(w, error, FALSE); return G_SOURCE_REMOVE;
    }
    nostr_event_free(ev); nostr_nip51_document_free(old_base); g_free(old_json); g_free(old_id); g_ptr_array_unref(old_ops);
acceptance_recorded:
    if (all_accepted) clear_signed(d);
    d->failed = d->waiting = FALSE;
  } else d->failed = TRUE;
  if (!persist(s, d, &error)) { g_variant_unref(outcomes); publish_fail(w, error, FALSE); return G_SOURCE_REMOVE; }
  d->pipeline = NULL;
  if (w->task) { g_task_return_pointer(w->task, g_variant_ref(outcomes), (GDestroyNotify)g_variant_unref); g_clear_object(&w->task); }
  g_variant_unref(outcomes); refresh(s, d, accepted ? NULL : "No relay accepted the list"); work_free(w);
  return G_SOURCE_REMOVE;
}
static void forward_cancel(GCancellable *caller, gpointer data) { (void)caller; g_cancellable_cancel(data); }
static gboolean allowed(GNostrListService *s, ListSlot *d, GNostrListApproval approval) {
  if (!valid(s) || !d->active || d->plan.waiting_for_dependency || d->unreadable || d->blocked_id || d->failed ||
      d->plan.publish_policy == GNOSTR_LIST_READ_ONLY || nostr_nip51_schema_read_only(nostr_nip51_schema_lookup(d->plan.kind)) ||
      (d->plan.publish_policy == GNOSTR_LIST_AUTO_WHEN_SYNC_ENABLED && !s->sync_enabled) ||
      !strv_len(d->plan.targets) || (!sources_ready(d) && !(approval & GNOSTR_LIST_APPROVE_PARTIAL))) return FALSE;
  for (guint i = 0; i < d->operations->len; i++) {
    GNostrListOperation *op = d->operations->pdata[i];
    if (!(approval & (op->private_tag ? GNOSTR_LIST_APPROVE_PRIVATE : GNOSTR_LIST_APPROVE_PUBLIC))) return FALSE;
  }
  return TRUE;
}
static void publish_begin(GNostrListService *s, ListSlot *d, GNostrListApproval approval, GTask *task) {
  if (d->pipeline || !allowed(s, d, approval)) {
    if (task) g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "List publication needs permission, discovery or dependencies");
    return;
  }
  if (s->adapter.derive_operations && valid(s)) {
    GError *error = NULL;
    GPtrArray *ops = s->adapter.derive_operations(s->data, d->snapshot, &error);
    GNostrListDocument *saved = !error && ops && ops->len ? gnostr_list_service_apply(s, d->plan.kind, d->plan.identifier, d->revision, ops, &error) : NULL;
    g_clear_object(&saved); if (ops) g_ptr_array_unref(ops);
    if (error) { d->failed = TRUE; if (task) g_task_return_error(task, error); else g_error_free(error); refresh(s, d, "Cannot derive list operations"); return; }
    if (!allowed(s, d, approval)) { if (task) g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Derived list needs publication approval"); return; }
  }
  if (!d->operations->len && !d->signed_json) {
    if (task) { GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("a{si}")); g_task_return_pointer(task, g_variant_ref_sink(g_variant_builder_end(&b)), (GDestroyNotify)g_variant_unref); }
    return;
  }
  if (!s->adapter.sign_async || !s->adapter.sign_finish || !s->adapter.publish_async || !s->adapter.publish_finish) {
    d->waiting = TRUE; if (task) g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "List signer or publisher unavailable"); refresh(s, d, "Waiting for list signer"); return;
  }
  ListWork *w = work_new(s, d, WORK_SIGN); w->approval = approval; w->task = task ? g_object_ref(task) : NULL;
  w->targets = g_strdupv((char **)d->plan.targets); w->codec = projection(d, d->operations, NULL);
  if (task && g_task_get_cancellable(task)) {
    w->caller_cancel = g_object_ref(g_task_get_cancellable(task));
    w->cancel_handler = g_cancellable_connect(w->caller_cancel, G_CALLBACK(forward_cancel), g_object_ref(w->cancel), g_object_unref);
  }
  d->pipeline = w; d->resume_pending = d->retry_signed = FALSE;
  if (d->signed_json && resume_valid(s, d) && d->signed_approval == approval) {
    w->signed_json = g_strdup(d->signed_json); g_strfreev(w->targets); w->targets = g_strdupv(d->signed_targets);
    /* Already accepted targets are immutable outcomes; retries address only
     * unfinished targets of this exact signed revision. */
    guint n = 0;
    for (guint i = 0; w->targets[i]; i++) {
      json_t *value = json_object_get(d->outcomes, w->targets[i]);
      int result = value ? json_integer_value(value) : GNOSTR_LIST_TRANSPORT_FAILURE;
      if (result == GNOSTR_LIST_ACCEPTED || result == GNOSTR_LIST_DUPLICATE) g_free(w->targets[i]);
      else w->targets[n++] = w->targets[i];
    }
    w->targets[n] = NULL;
    if (!n) { clear_signed(d); work_end(w, NULL); return; }
    w->type = WORK_PUBLISH; work_start(w); return;
  }
  if (d->signed_json) { clear_signed(d); GError *error = NULL; if (!persist(s, d, &error)) { publish_fail(w, error, FALSE); return; } }
  gboolean private_content = nostr_nip51_document_tag_count(w->codec, TRUE) > 0;
  if (private_content) {
    if (!s->adapter.encrypt_async || !s->adapter.encrypt_finish) { publish_fail(w, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "NIP-44 encryption unavailable"), TRUE); return; }
    w->input = nostr_nip51_document_serialize_private_tags(w->codec); w->type = WORK_ENCRYPT;
    if (!w->input) { publish_fail(w, NULL, FALSE); return; }
    crypto_enqueue(w);
  } else {
    NostrEvent *ev = nostr_nip51_document_build_unsigned(w->codec, s->account, MAX(now(s), d->base_time + 1), "");
    w->unsigned_json = ev ? nostr_event_serialize(ev) : NULL; nostr_event_free(ev);
    if (!w->unsigned_json) { publish_fail(w, NULL, FALSE); return; }
    work_start(w);
  }
  refresh(s, d, NULL);
}
void gnostr_list_service_publish_async(GNostrListService *s, int kind, const char *id, GNostrListApproval approval, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
  g_return_if_fail(owner(s));
  GTask *task = g_task_new(s, cancel, callback, data);
  ListSlot *d = find_slot(s, kind, id);
  if (!d) g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "List address not found");
  else publish_begin(s, d, approval, task);
  g_object_unref(task);
}
GVariant *gnostr_list_service_publish_finish(GNostrListService *s, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, s), NULL); return g_task_propagate_pointer(G_TASK(result), error);
}
static gboolean wake_dispatch(gpointer data) {
  GNostrListService *s = data; s->wake_pending = FALSE;
  for (guint i = 0; valid(s) && i < s->slots->len; i++) {
    ListSlot *d = s->slots->pdata[i];
    if (!d->pipeline && !d->waiting && !d->failed && d->signed_json &&
        (d->retry_signed || (d->resume_pending && (s->sync_enabled || d->plan.publish_policy == GNOSTR_LIST_AUTO_ALWAYS)))) {
      publish_begin(s, d, d->signed_approval, NULL);
    } else if (!d->pipeline && !d->waiting && !d->failed &&
        (d->operations->len || s->adapter.derive_operations) &&
        (d->plan.publish_policy == GNOSTR_LIST_AUTO_ALWAYS ||
         (d->plan.publish_policy == GNOSTR_LIST_AUTO_WHEN_SYNC_ENABLED && s->sync_enabled)))
      publish_begin(s, d, GNOSTR_LIST_APPROVE_PRIVATE, NULL);
  }
  return G_SOURCE_REMOVE;
}
static void wake(GNostrListService *s) {
  if (s->wake_pending || !valid(s)) return;
  s->wake_pending = TRUE; dispatch(s, wake_dispatch, g_object_ref(s), g_object_unref);
}
typedef struct { GNostrListService *s; guint64 serial; int kind; char *id, *source, *payload; GNostrListReadNotification type; } Notification;
static void notification_free(gpointer data) {
  Notification *n = data; g_object_unref(n->s); g_free(n->id); g_free(n->source); g_free(n->payload); g_free(n);
}
static gboolean notification_dispatch(gpointer data) {
  Notification *n = data; GNostrListService *s = n->s; ListSlot *d = find_slot(s, n->kind, n->id);
  if (!valid(s) || n->serial != s->read_serial || !d || !d->active || !contains(d->plan.sources, n->source)) return G_SOURCE_REMOVE;
  if (n->type == GNOSTR_LIST_EVENT) accept_event(s, d, n->payload, FALSE);
  else {
    /* Duplicate EOSE is idempotent. Reconnect resets only that source's boundary. */
    json_object_set_new(d->sources, n->source, json_integer(n->type + 1));
    refresh(s, d, NULL); wake(s);
  }
  return G_SOURCE_REMOVE;
}
void gnostr_list_service_notify(GNostrListService *s, guint64 serial, int kind, const char *id, const char *source, GNostrListReadNotification type, const char *payload) {
  if (!source || type < GNOSTR_LIST_EVENT || type > GNOSTR_LIST_RECONNECTED ||
      (payload && strlen(payload) > NOSTR_LIST_MAX_FOLLOW_BYTES)) return;
  Notification *n = g_new0(Notification, 1); n->s = g_object_ref(s); n->serial = serial; n->kind = kind;
  n->id = g_strdup(id ? id : ""); n->source = g_strdup(source); n->payload = g_strdup(payload); n->type = type;
  dispatch(s, notification_dispatch, n, notification_free);
}
static void close_slot(GNostrListService *s, ListSlot *d) {
  if (d->subscription) { gpointer sub = d->subscription; d->subscription = NULL; s->adapter.close_subscription(s->data, sub); }
  for (guint i = 0; i < s->works->len; i++) {
    ListWork *w = s->works->pdata[i]; if (w->slot == d) g_cancellable_cancel(w->cancel);
  }
  d->decrypt_pending = FALSE;
  if (d->blocked_id) d->unreadable = TRUE;
}
void gnostr_list_service_set_private_sync_enabled(GNostrListService *s, gboolean enabled) {
  g_return_if_fail(owner(s)); if (!valid(s) || s->sync_enabled == !!enabled) return;
  s->sync_enabled = !!enabled; s->policy_serial++;
  for (guint i = 0; valid(s) && i < s->slots->len; i++) {
    ListSlot *d = s->slots->pdata[i];
    if (d->plan.publish_policy != GNOSTR_LIST_AUTO_ALWAYS && d->pipeline) g_cancellable_cancel(((ListWork *)d->pipeline)->cancel);
    if (!enabled && d->plan.publish_policy != GNOSTR_LIST_AUTO_ALWAYS && d->signed_json) {
      clear_signed(d); GError *error = NULL; if (!persist(s, d, &error)) d->failed = TRUE; g_clear_error(&error);
    }
    refresh(s, d, NULL);
  }
  wake(s);
}
void gnostr_list_service_retry(GNostrListService *s, int kind, const char *id) {
  g_return_if_fail(owner(s)); ListSlot *d = find_slot(s, kind, id);
  if (!valid(s) || !d) return;
  gboolean reopen = d->subscription_failed;
  d->failed = d->waiting = FALSE; d->retry_signed = d->signed_json != NULL;
  if (reopen && !d->plan.waiting_for_dependency && s->adapter.open_subscription && valid(s)) {
    json_object_clear(d->sources);
    d->subscription = s->adapter.open_subscription(s->data, s, s->account, s->generation, s->read_serial, &d->plan, NULL);
    d->subscription_failed = !d->subscription; d->failed = d->subscription_failed;
    refresh(s, d, d->failed ? "List subscription unavailable" : NULL);
  }
  if (d->blocked_json) { char *wire = g_strdup(d->blocked_json); accept_event(s, d, wire, TRUE); g_free(wire); }
  wake(s);
}
void gnostr_list_service_close(GNostrListService *s) {
  g_return_if_fail(owner(s)); if (s->closed) return;
  s->closed = TRUE; s->read_serial++; s->policy_serial++;
  for (guint i = 0; i < s->slots->len; i++) close_slot(s, s->slots->pdata[i]);
  for (guint i = 0; i < s->works->len; i++) g_cancellable_cancel(((ListWork *)s->works->pdata[i])->cancel);
  while (!g_queue_is_empty(&s->crypto_queue)) work_end(g_queue_pop_head(&s->crypto_queue), NULL);
  g_signal_emit(s, signals[1], 0);
}
