#ifndef GNOSTR_LIST_PRIVATE_H
#define GNOSTR_LIST_PRIVATE_H
#include "gnostr-list-service.h"
#include <jansson.h>

struct _GNostrListDocument {
  GObject parent_instance;
  NostrListDocument *codec;
  char *author, *event_id, *source_json, *error, *sources, *targets;
  gint64 created_at;
  guint64 revision;
  gboolean dirty;
};
typedef enum { LIST_APPEND, LIST_REMOVE, LIST_MOVE, LIST_METADATA } ListEdit;
struct _GNostrListOperation {
  gatomicrefcount refs;
  ListEdit type;
  gboolean private_tag;
  NostrListTag *tag;
  guint occurrence, position;
};
NostrListDocument *list_codec_copy(const NostrListDocument *codec);
GNostrListDocument *list_snapshot_new(const char *account, const NostrListDocument *codec);
gboolean list_operation_apply(NostrListDocument *codec, GNostrListOperation *op, GError **error);
json_t *list_operation_json(GNostrListOperation *op);
GNostrListOperation *list_operation_parse(json_t *json);
char *list_json_dump(json_t *json);
gboolean list_fail(GError **error, GIOErrorEnum code, const char *message);
#endif
