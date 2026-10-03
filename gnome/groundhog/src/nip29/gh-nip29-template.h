#ifndef GH_NIP29_TEMPLATE_H
#define GH_NIP29_TEMPLATE_H

#include "gh-nip29-group.h"

G_BEGIN_DECLS

/* NIP-29 "Timeline references": a `previous` tag cites the first 8
 * characters of any of the last 50 events the user saw on the group's relay,
 * excluding the user's own events. */
#define GH_NIP29_PREVIOUS_WINDOW 50
#define GH_NIP29_PREVIOUS_PREFIX_LEN 8
/* The spec recommends at least three references; this core cites the three
 * newest eligible events (fewer when fewer are eligible). */
#define GH_NIP29_PREVIOUS_REFS 3

typedef struct {
  const gchar *event_id; /* 64-char lowercase hex */
  const gchar *pubkey;   /* its author, 64-char lowercase hex */
} GhNip29TimelineRef;

typedef struct {
  const gchar *author_pubkey; /* the signing account, 64-char lowercase hex */
  gint64 created_at;          /* > 0 */
  /* Events seen from the group's relay, newest first; may be empty. */
  const GhNip29TimelineRef *recent;
  gsize n_recent;
} GhNip29TemplateContext;

/* ---- Timeline ring (qp24.12.2) --------------------------------------------
 * The source of GhNip29TemplateContext.recent: the newest
 * GH_NIP29_PREVIOUS_WINDOW events seen from one group's relay (its h-tagged
 * timeline, own events included, since they occupy window slots), ordered
 * newest first by created_at and then by id, each id once. The service feeds
 * it from the group's EOSE-aware subscription (backfill and live alike) and
 * persists it with the group, so the first message after a restart can cite
 * events before the relay replays anything. Refs are scoped to one group on
 * one relay: relays that enforce timeline references check them against that
 * group's recent history, and a fork on another relay never sees them. */
typedef struct {
  gchar event_id[65];
  gchar pubkey[65];
  gint64 created_at;
} GhNip29TimelineEntry;

typedef struct _GhNip29Timeline GhNip29Timeline;

GhNip29Timeline *gh_nip29_timeline_new(void);
void gh_nip29_timeline_free(GhNip29Timeline *timeline);
/* Records one event (64-char lowercase hex id and author, created_at >= 0).
 * TRUE when the ring changed: a known id, or an event older than the full
 * window's oldest, changes nothing. Malformed input is refused (FALSE). */
gboolean gh_nip29_timeline_add(GhNip29Timeline *timeline, const gchar *event_id,
                               const gchar *pubkey, gint64 created_at);
/* Drops an event (e.g. deleted by kind:9005); TRUE when it was held. */
gboolean gh_nip29_timeline_remove(GhNip29Timeline *timeline, const gchar *event_id);
guint gh_nip29_timeline_get_length(const GhNip29Timeline *timeline);
/* Newest first; index < length. */
const GhNip29TimelineEntry *gh_nip29_timeline_get_entry(const GhNip29Timeline *timeline,
                                                        guint index);
/* Newest-first refs for GhNip29TemplateContext.recent, valid until the next
 * change of the ring. */
const GhNip29TimelineRef *gh_nip29_timeline_get_refs(GhNip29Timeline *timeline,
                                                     gsize *n_refs);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhNip29Timeline, gh_nip29_timeline_free)

/* Up to GH_NIP29_PREVIOUS_REFS distinct prefixes chosen newest-first from
 * the first GH_NIP29_PREVIOUS_WINDOW entries of @recent, skipping events by
 * @author_pubkey. The author's own events still occupy window slots: that
 * reading of "the last 50 events seen ... excluding events by themselves"
 * never cites an event outside the relay's last 50, whichever way a relay
 * interprets it. Every entry in the window must be well formed. Returns an
 * empty vector when nothing is eligible. */
GStrv gh_nip29_select_previous(const gchar *author_pubkey,
                               const GhNip29TimelineRef *recent,
                               gsize n_recent,
                               GError **error);

/* Unsigned event templates as compact JSON {pubkey, created_at, kind, tags,
 * content} for the account signer. Every template carries ["h", <group id>]
 * first, then its operation tags, then one ["previous", ...] tag when any
 * reference is eligible. Optional reasons become the content ("" if NULL). */

/* kind:9 chat message; @text must be non-empty. */
gchar *gh_nip29_template_chat(const GhNip29GroupKey *group,
                              const GhNip29TemplateContext *context,
                              const gchar *text,
                              GError **error);

/* kind:9021 join request; a NULL or empty @invite_code adds no code tag. */
gchar *gh_nip29_template_join_request(const GhNip29GroupKey *group,
                                      const GhNip29TemplateContext *context,
                                      const gchar *reason,
                                      const gchar *invite_code,
                                      GError **error);

/* kind:9022 leave request. */
gchar *gh_nip29_template_leave_request(const GhNip29GroupKey *group,
                                       const GhNip29TemplateContext *context,
                                       const gchar *reason,
                                       GError **error);

/* kind:9000 put-user: ["p", <pubkey>, <role>...]. @roles is NULL-terminated
 * and nullable; duplicates are dropped. */
gchar *gh_nip29_template_put_user(const GhNip29GroupKey *group,
                                  const GhNip29TemplateContext *context,
                                  const gchar *pubkey,
                                  const gchar *const *roles,
                                  const gchar *reason,
                                  GError **error);

/* kind:9001 remove-user: ["p", <pubkey>]. */
gchar *gh_nip29_template_remove_user(const GhNip29GroupKey *group,
                                     const GhNip29TemplateContext *context,
                                     const gchar *pubkey,
                                     const gchar *reason,
                                     GError **error);

/* kind:9002 edit-metadata carries the complete desired metadata (NIP-29
 * edits replace, they do not patch): start from gh_nip29_group_dup_metadata()
 * so supported kinds, parent, children and the unmodelled extra_tags survive
 * the edit. Extra tags follow the modelled ones in their order; each must be
 * a non-empty UTF-8 name that the model does not own, within the
 * GH_NIP29_MAX_EXTRA_* bounds. */
gchar *gh_nip29_template_edit_metadata(const GhNip29GroupKey *group,
                                       const GhNip29TemplateContext *context,
                                       const GhNip29Metadata *metadata,
                                       const gchar *reason,
                                       GError **error);

/* kind:9005 delete-event: ["e", <event id>]. */
gchar *gh_nip29_template_delete_event(const GhNip29GroupKey *group,
                                      const GhNip29TemplateContext *context,
                                      const gchar *event_id,
                                      const gchar *reason,
                                      GError **error);

/* W26 slice B review fix (F2): kind:5 author deletion (NIP-09) for a NIP-29
 * group. Regular members use this to delete their own events (e.g. reactions).
 * Unlike kind:9005 (admin delete-event), no admin permission is needed. */
gchar *gh_nip29_template_deletion(const GhNip29GroupKey *group,
                                  const GhNip29TemplateContext *context,
                                  const gchar *event_id,
                                  GError **error);

/* W26 slice B (nostrc-191r): kind:7 reaction (NIP-25): ["e", <target event
 * id>], ["p", <target author pubkey>], ["k", <target kind as string>];
 * @emoji must be non-empty UTF-8 and becomes the content. */
gchar *gh_nip29_template_reaction(const GhNip29GroupKey *group,
                                  const GhNip29TemplateContext *context,
                                  const gchar *target_event_id,
                                  const gchar *target_pubkey,
                                  const gchar *target_kind_str,
                                  const gchar *emoji,
                                  GError **error);

/* kind:9009 create-invite: ["code", <code>]; @code must be non-empty. */
gchar *gh_nip29_template_create_invite(const GhNip29GroupKey *group,
                                       const GhNip29TemplateContext *context,
                                       const gchar *code,
                                       const gchar *reason,
                                       GError **error);

/* kind:9007 create-group (G20b): asks the relay to create a group with the
 * id of @group (chosen by the client); a relay that lets the account create
 * it makes the account its admin. Most relays refuse it unless configured
 * to allow group creation. */
gchar *gh_nip29_template_create_group(const GhNip29GroupKey *group,
                                      const GhNip29TemplateContext *context,
                                      const gchar *reason,
                                      GError **error);

G_END_DECLS

#endif /* GH_NIP29_TEMPLATE_H */
