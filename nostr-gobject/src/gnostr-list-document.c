#include "gnostr-list-private.h"
#include <string.h>
#include <stdlib.h>

G_DEFINE_TYPE(GNostrListDocument, gnostr_list_document, G_TYPE_OBJECT)
G_DEFINE_BOXED_TYPE(GNostrListOperation, gnostr_list_operation,
                    gnostr_list_operation_ref, gnostr_list_operation_unref)

static void gnostr_list_document_finalize(GObject *object) {
  GNostrListDocument *s = GNOSTR_LIST_DOCUMENT(object);
  nostr_nip51_document_free(s->codec);
  g_free(s->author); g_free(s->event_id); g_free(s->source_json);
  g_free(s->error); g_free(s->sources); g_free(s->targets);
  G_OBJECT_CLASS(gnostr_list_document_parent_class)->finalize(object);
}
static void gnostr_list_document_class_init(GNostrListDocumentClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = gnostr_list_document_finalize;
}
static void gnostr_list_document_init(GNostrListDocument *s) { (void)s; }

gboolean list_fail(GError **error, GIOErrorEnum code, const char *message) {
  g_set_error_literal(error, G_IO_ERROR, code, message); return FALSE;
}
char *list_json_dump(json_t *j) { return json_dumps(j, JSON_COMPACT | JSON_ENCODE_ANY); }
static NostrListTag *tag_copy(const NostrListTag *tag) {
  size_t n = nostr_nip51_tag_size(tag);
  if (!tag || n > NOSTR_LIST_MAX_FIELDS) return NULL;
  const char *fields[NOSTR_LIST_MAX_FIELDS];
  for (size_t i = 0; i < n; i++) fields[i] = nostr_nip51_tag_field(tag, i);
  return nostr_nip51_tag_new(fields, n);
}
NostrListDocument *list_codec_copy(const NostrListDocument *src) {
  int kind = nostr_nip51_document_kind(src);
  const NostrListSchema *schema = nostr_nip51_schema_lookup(kind);
  NostrListDocument *d = nostr_nip51_document_new(kind,
    nostr_nip51_schema_requires_identifier(schema) ? nostr_nip51_document_identifier(src) : NULL);
  if (!d) return NULL;
  while (nostr_nip51_document_tag_count(d, FALSE)) nostr_nip51_document_remove(d, FALSE, 0);
  for (int p = 0; p < 2; p++)
    for (size_t i = 0; i < nostr_nip51_document_tag_count(src, p); i++)
      if (nostr_nip51_document_append(d, p, nostr_nip51_document_tag(src, p, i))) goto fail;
  if (nostr_nip51_document_private_state(src) == NOSTR_LIST_PRIVATE_UNREADABLE)
    nostr_nip51_document_mark_unreadable(d);
  return d;
fail:
  nostr_nip51_document_free(d); return NULL;
}
GNostrListDocument *list_snapshot_new(const char *account, const NostrListDocument *codec) {
  GNostrListDocument *s = g_object_new(GNOSTR_TYPE_LIST_DOCUMENT, NULL);
  s->author = g_strdup(account); s->codec = list_codec_copy(codec);
  s->sources = g_strdup("{}"); s->targets = g_strdup("{}");
  if (!s->codec) { g_object_unref(s); return NULL; }
  return s;
}
const char *gnostr_list_document_get_author(GNostrListDocument *s) { return s->author; }
int gnostr_list_document_get_kind(GNostrListDocument *s) { return nostr_nip51_document_kind(s->codec); }
const char *gnostr_list_document_get_identifier(GNostrListDocument *s) { return nostr_nip51_document_identifier(s->codec); }
const char *gnostr_list_document_get_event_id(GNostrListDocument *s) { return s->event_id; }
const char *gnostr_list_document_get_source_json(GNostrListDocument *s) { return s->source_json; }
gint64 gnostr_list_document_get_created_at(GNostrListDocument *s) { return s->created_at; }
guint64 gnostr_list_document_get_local_revision(GNostrListDocument *s) { return s->revision; }
gboolean gnostr_list_document_get_dirty(GNostrListDocument *s) { return s->dirty; }
GNostrListPrivateState gnostr_list_document_get_private_state(GNostrListDocument *s) { return (GNostrListPrivateState)nostr_nip51_document_private_state(s->codec); }
const NostrListDocument *gnostr_list_document_get_codec(GNostrListDocument *s) { return s->codec; }
const char *gnostr_list_document_get_error(GNostrListDocument *s) { return s->error; }
const char *gnostr_list_document_get_source_outcomes(GNostrListDocument *s) { return s->sources; }
const char *gnostr_list_document_get_target_outcomes(GNostrListDocument *s) { return s->targets; }

GNostrListOperation *gnostr_list_operation_ref(GNostrListOperation *s) { g_atomic_ref_count_inc(&s->refs); return s; }
void gnostr_list_operation_unref(GNostrListOperation *s) {
  if (s && g_atomic_ref_count_dec(&s->refs)) { nostr_nip51_tag_free(s->tag); g_free(s); }
}
static GNostrListOperation *operation_new(ListEdit type, gboolean p, const NostrListTag *tag, guint occurrence, guint position) {
  if (type == LIST_METADATA && p) return NULL;
  GNostrListOperation *o = g_new0(GNostrListOperation, 1);
  g_atomic_ref_count_init(&o->refs); o->type = type; o->private_tag = !!p;
  o->tag = tag_copy(tag); o->occurrence = occurrence; o->position = position;
  if (!o->tag) { gnostr_list_operation_unref(o); return NULL; } return o;
}
GNostrListOperation *gnostr_list_operation_new(GNostrListEditType type, gboolean private_tag,
  const char *const *fields, gsize n_fields, guint occurrence, guint position) {
  if (type < GNOSTR_LIST_EDIT_APPEND || type > GNOSTR_LIST_EDIT_METADATA) return NULL;
  NostrListTag *tag = nostr_nip51_tag_new(fields, n_fields);
  GNostrListOperation *op = tag ? operation_new((ListEdit)type, private_tag, tag, occurrence, position) : NULL;
  nostr_nip51_tag_free(tag); return op;
}
GNostrListOperation *gnostr_list_operation_append(gboolean p, const NostrListTag *t) { return operation_new(LIST_APPEND, p, t, 0, 0); }
GNostrListOperation *gnostr_list_operation_remove(gboolean p, const NostrListTag *t, guint n) { return operation_new(LIST_REMOVE, p, t, n, 0); }
GNostrListOperation *gnostr_list_operation_move(gboolean p, const NostrListTag *t, guint n, guint to) { return operation_new(LIST_MOVE, p, t, n, to); }
GNostrListOperation *gnostr_list_operation_metadata(const NostrListTag *t, guint n) { return operation_new(LIST_METADATA, FALSE, t, n, 0); }
static gboolean tags_equal(const NostrListTag *a, const NostrListTag *b) {
  size_t n = nostr_nip51_tag_size(a);
  if (n != nostr_nip51_tag_size(b)) return FALSE;
  for (size_t i = 0; i < n; i++) if (g_strcmp0(nostr_nip51_tag_field(a, i), nostr_nip51_tag_field(b, i))) return FALSE;
  return TRUE;
}
gboolean list_operation_apply(NostrListDocument *d, GNostrListOperation *o, GError **error) {
  const char *name = nostr_nip51_tag_field(o->tag, 0);
  const NostrListSchema *schema = nostr_nip51_schema_lookup(nostr_nip51_document_kind(d));
  if (!name || !strcmp(name, "d") || nostr_nip51_validate_item(schema, o->tag) == NOSTR_LIST_ITEM_INVALID)
    return list_fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Invalid list operation");
  if (o->type == LIST_METADATA && strcmp(name, "title") && strcmp(name, "image") && strcmp(name, "description"))
    return list_fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Invalid metadata operation");
  int rc = 0;
  if (o->type == LIST_APPEND) rc = nostr_nip51_document_append(d, o->private_tag, o->tag);
  else {
    guint seen = 0;
    gboolean found = FALSE;
    for (size_t i = 0; i < nostr_nip51_document_tag_count(d, o->private_tag);) {
      const NostrListTag *t = nostr_nip51_document_tag(d, o->private_tag, i);
      gboolean match = o->type == LIST_METADATA ? !g_strcmp0(name, nostr_nip51_tag_field(t, 0)) : tags_equal(t, o->tag);
      if (!match || (o->occurrence != G_MAXUINT && seen++ != o->occurrence)) { i++; continue; }
      found = TRUE;
      if (o->type == LIST_METADATA) rc = nostr_nip51_document_replace(d, FALSE, i, o->tag);
      else if (o->type == LIST_REMOVE) rc = nostr_nip51_document_remove(d, o->private_tag, i);
      else {
        /* Rebuild just this ordered vector; other visibility and unknown tags survive. */
        GPtrArray *tags = g_ptr_array_new_with_free_func((GDestroyNotify)nostr_nip51_tag_free);
        for (size_t j = 0; j < nostr_nip51_document_tag_count(d, o->private_tag); j++)
          if (j != i) g_ptr_array_add(tags, tag_copy(nostr_nip51_document_tag(d, o->private_tag, j)));
        g_ptr_array_insert(tags, MIN(o->position, tags->len), tag_copy(t));
        while (nostr_nip51_document_tag_count(d, o->private_tag)) nostr_nip51_document_remove(d, o->private_tag, 0);
        for (guint j = 0; !rc && j < tags->len; j++) rc = nostr_nip51_document_append(d, o->private_tag, tags->pdata[j]);
        g_ptr_array_unref(tags);
      }
      if (rc || o->occurrence != G_MAXUINT || o->type != LIST_REMOVE) break;
    }
    if (!found && o->type == LIST_METADATA) rc = nostr_nip51_document_append(d, FALSE, o->tag);
  }
  if (rc || nostr_nip51_document_validate(d)) return list_fail(error, G_IO_ERROR_INVALID_DATA, "List edit exceeds codec bounds or schema");
  return TRUE;
}
json_t *list_operation_json(GNostrListOperation *o) {
  json_t *a = json_array();
  for (size_t i = 0; i < nostr_nip51_tag_size(o->tag); i++) json_array_append_new(a, json_string(nostr_nip51_tag_field(o->tag, i)));
  return json_pack("{s:i,s:b,s:I,s:I,s:o}", "type", o->type, "private", o->private_tag,
    "occurrence", (json_int_t)o->occurrence, "position", (json_int_t)o->position, "tag", a);
}
GNostrListOperation *list_operation_parse(json_t *j) {
  json_t *a = json_object_get(j, "tag"), *p = json_object_get(j, "private");
  json_int_t type = json_integer_value(json_object_get(j, "type"));
  json_int_t n = json_integer_value(json_object_get(j, "occurrence")), pos = json_integer_value(json_object_get(j, "position"));
  if (!json_is_array(a) || !json_is_boolean(p) || type < LIST_APPEND || type > LIST_METADATA || n < 0 || n > G_MAXUINT || pos < 0 || pos > G_MAXUINT || json_array_size(a) > NOSTR_LIST_MAX_FIELDS) return NULL;
  const char *fields[NOSTR_LIST_MAX_FIELDS];
  for (size_t i = 0; i < json_array_size(a); i++) {
    fields[i] = json_string_value(json_array_get(a, i));
    if (!fields[i] || strlen(fields[i]) != json_string_length(json_array_get(a, i))) return NULL;
  }
  NostrListTag *t = nostr_nip51_tag_new(fields, json_array_size(a));
  GNostrListOperation *o = t ? operation_new(type, json_is_true(p), t, n, pos) : NULL;
  nostr_nip51_tag_free(t); return o;
}
