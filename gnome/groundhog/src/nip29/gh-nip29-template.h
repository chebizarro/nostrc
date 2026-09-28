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
 * so supported kinds, parent and children survive the edit. */
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

/* kind:9009 create-invite: ["code", <code>]; @code must be non-empty. */
gchar *gh_nip29_template_create_invite(const GhNip29GroupKey *group,
                                       const GhNip29TemplateContext *context,
                                       const gchar *code,
                                       const gchar *reason,
                                       GError **error);

G_END_DECLS

#endif /* GH_NIP29_TEMPLATE_H */
