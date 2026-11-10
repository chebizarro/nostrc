#ifndef GH_DRAFTS_PAGE_H
#define GH_DRAFTS_PAGE_H

#include <adwaita.h>

G_BEGIN_DECLS

/* GhDraftsPage: Preferences › Messages › Drafts (plan §3.13, nostrc-9lwl7.8):
 * the draft synchronization status with Retry when it stopped or failed,
 * and the conflict copies (text a newer draft from another device replaced,
 * or kept here when one arrived while writing), each with Restore and
 * Discard (undone from its toast). Backed by a copied table whose owner is
 * referenced; the owner emits "changed" whenever the status or the copies
 * change. No UI object ever borrows a GhStore. Account switches (a changed
 * generation) dismiss the page. */

typedef struct {
  gint64 id;
  gchar *text;
  GStrv npubs;       /* the other participants; the account itself for a note to self */
  gint64 saved_at;   /* unix seconds */
  gchar *room_key;   /* opaque here; handed back to keep() */
} GhDraftsConflict;
void gh_drafts_conflict_free(GhDraftsConflict *conflict);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhDraftsConflict, gh_drafts_conflict_free)

typedef struct {
  GObject *owner;
  guint64 (*generation)(GObject *owner);
  gboolean (*enabled)(GObject *owner);        /* the sync switch is on */
  const gchar *(*status)(GObject *owner);     /* NULL: nothing to say */
  gboolean (*can_retry)(GObject *owner);
  void (*retry)(GObject *owner);
  GPtrArray *(*list_conflicts)(GObject *owner, GError **error); /* GhDraftsConflict, newest first */
  gboolean (*restore)(GObject *owner, gint64 id, GError **error);
  gboolean (*discard)(GObject *owner, gint64 id, GError **error);
  gboolean (*keep)(GObject *owner, const gchar *room_key, const gchar *text, GError **error);
} GhDraftsBackend;

#define GH_TYPE_DRAFTS_PAGE (gh_drafts_page_get_type())
G_DECLARE_FINAL_TYPE(GhDraftsPage, gh_drafts_page, GH, DRAFTS_PAGE, AdwNavigationPage)

GhDraftsPage *gh_drafts_page_new(const GhDraftsBackend *backend);

/* The Messages page row's subtitle for a backend: its status, else the
 * number of conflict copies, else "Up to date" (transfer full). */
gchar *gh_drafts_page_describe(const GhDraftsBackend *backend);

G_END_DECLS
#endif
