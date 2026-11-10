#ifndef GNOSTR_LIST_DOCUMENT_H
#define GNOSTR_LIST_DOCUMENT_H

#include "nostr-gobject-export.h"
#include <gio/gio.h>
#include <nostr/nip51/nip51.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_LIST_DOCUMENT (gnostr_list_document_get_type())
NOSTR_GOBJECT_API G_DECLARE_FINAL_TYPE(GNostrListDocument, gnostr_list_document, GNOSTR, LIST_DOCUMENT, GObject)

/* Immutable snapshots. All returned strings/tags are borrowed for the lifetime
 * of the snapshot, not the service. Source JSON is authenticated by the service;
 * locally created snapshots have no source revision. */
NOSTR_GOBJECT_API const char *gnostr_list_document_get_author(GNostrListDocument *self);
NOSTR_GOBJECT_API int gnostr_list_document_get_kind(GNostrListDocument *self);
NOSTR_GOBJECT_API const char *gnostr_list_document_get_identifier(GNostrListDocument *self);
NOSTR_GOBJECT_API const char *gnostr_list_document_get_event_id(GNostrListDocument *self);
NOSTR_GOBJECT_API const char *gnostr_list_document_get_source_json(GNostrListDocument *self);
NOSTR_GOBJECT_API gint64 gnostr_list_document_get_created_at(GNostrListDocument *self);
NOSTR_GOBJECT_API guint64 gnostr_list_document_get_local_revision(GNostrListDocument *self);
NOSTR_GOBJECT_API gboolean gnostr_list_document_get_dirty(GNostrListDocument *self);
typedef enum {
  GNOSTR_LIST_PRIVATE_EMPTY, GNOSTR_LIST_PRIVATE_ENCRYPTED_NIP44,
  GNOSTR_LIST_PRIVATE_LEGACY_NIP04, GNOSTR_LIST_PRIVATE_DECODED, GNOSTR_LIST_PRIVATE_UNREADABLE
} GNostrListPrivateState;
NOSTR_GOBJECT_API GNostrListPrivateState gnostr_list_document_get_private_state(GNostrListDocument *self);
/** gnostr_list_document_get_codec: (skip)
 * C-only borrowed portable codec; introspected callers use snapshot accessors.
 */
NOSTR_GOBJECT_API const NostrListDocument *gnostr_list_document_get_codec(GNostrListDocument *self);
NOSTR_GOBJECT_API const char *gnostr_list_document_get_error(GNostrListDocument *self);
/* JSON objects keyed by destination; no list contents or signer payloads. */
NOSTR_GOBJECT_API const char *gnostr_list_document_get_source_outcomes(GNostrListDocument *self);
NOSTR_GOBJECT_API const char *gnostr_list_document_get_target_outcomes(GNostrListDocument *self);

/* Immutable, reference-counted edit descriptions. Tags are copied. Remove/move
 * identify the nth equal full tag (including empty and trailing fields), not a
 * value alone. G_MAXUINT removes all equal tags. Moves insert at a vector index.
 * Metadata edits replace one public occurrence of the named metadata tag.
 * Operations never change a document's d/address. */
typedef struct _GNostrListOperation GNostrListOperation;
#define GNOSTR_TYPE_LIST_OPERATION (gnostr_list_operation_get_type())
NOSTR_GOBJECT_API GType gnostr_list_operation_get_type(void);
NOSTR_GOBJECT_API GNostrListOperation *gnostr_list_operation_ref(GNostrListOperation *self);
NOSTR_GOBJECT_API void gnostr_list_operation_unref(GNostrListOperation *self);
/** gnostr_list_operation_append: (skip) */
NOSTR_GOBJECT_API GNostrListOperation *gnostr_list_operation_append(gboolean private_tag, const NostrListTag *tag);
/** gnostr_list_operation_remove: (skip) */
NOSTR_GOBJECT_API GNostrListOperation *gnostr_list_operation_remove(gboolean private_tag, const NostrListTag *tag, guint occurrence);
/** gnostr_list_operation_move: (skip) */
NOSTR_GOBJECT_API GNostrListOperation *gnostr_list_operation_move(gboolean private_tag, const NostrListTag *tag, guint occurrence, guint position);
/** gnostr_list_operation_metadata: (skip) */
NOSTR_GOBJECT_API GNostrListOperation *gnostr_list_operation_metadata(const NostrListTag *tag, guint occurrence);

typedef enum {
  GNOSTR_LIST_EDIT_APPEND, GNOSTR_LIST_EDIT_REMOVE, GNOSTR_LIST_EDIT_MOVE, GNOSTR_LIST_EDIT_METADATA
} GNostrListEditType;
/**
 * gnostr_list_operation_new:
 * @type: edit type
 * @private_tag: whether to edit private tags
 * @fields: (array length=n_fields) (element-type utf8): complete tag fields, copied
 * @n_fields: number of fields
 * @occurrence: matching occurrence, or G_MAXUINT for all removes
 * @position: destination index for a move
 *
 * Returns: (transfer full) (nullable): an immutable edit
 */
NOSTR_GOBJECT_API GNostrListOperation *gnostr_list_operation_new(GNostrListEditType type, gboolean private_tag,
  const char *const *fields, gsize n_fields, guint occurrence, guint position);

G_END_DECLS
#endif
