#ifndef GH_BADGE_TYPES_H
#define GH_BADGE_TYPES_H
#include <gdk/gdk.h>
#include <gio/gio.h>
G_BEGIN_DECLS
/* One verified NIP-58 badge: the profile list's a/e pair, the award by the
 * definition's author naming the displayed account, and that definition
 * (plan §3.12). Artwork addresses are carried, never fetched here. */
typedef struct {
  gchar *address;     /* "30009:<issuer>:<identifier>" */
  gchar *award_id;    /* 64 lowercase hex */
  gchar *issuer;      /* definition author == award author, 64 lowercase hex */
  gchar *identifier;  /* the definition's d */
  gchar *name;        /* nullable */
  gchar *description; /* nullable */
  gchar *image_url;   /* nullable; inert until Load Remote Images consent */
  gchar *thumb_url;   /* nullable; same */
  gchar *award_relay; /* the list's e relay hint, nullable; never contacted unasked */
  gboolean verified;  /* the whole chain checked out; FALSE: a chosen pair whose
                       * award or definition is not known (no "Awarded by") */
} GhBadge;
static inline void
gh_badge_free(GhBadge *badge)
{
  if (!badge) return;
  g_free(badge->address); g_free(badge->award_id); g_free(badge->issuer);
  g_free(badge->identifier); g_free(badge->name); g_free(badge->description);
  g_free(badge->image_url); g_free(badge->thumb_url); g_free(badge->award_relay);
  g_free(badge);
}
static inline GhBadge *
gh_badge_copy(const GhBadge *badge)
{
  GhBadge *copy = g_new0(GhBadge, 1);
  copy->address = g_strdup(badge->address); copy->award_id = g_strdup(badge->award_id);
  copy->issuer = g_strdup(badge->issuer); copy->identifier = g_strdup(badge->identifier);
  copy->name = g_strdup(badge->name); copy->description = g_strdup(badge->description);
  copy->image_url = g_strdup(badge->image_url); copy->thumb_url = g_strdup(badge->thumb_url);
  copy->award_relay = g_strdup(badge->award_relay);
  copy->verified = badge->verified;
  return copy;
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhBadge, gh_badge_free)

/* Which profile list a displayed account's badges come from. */
typedef enum {
  GH_BADGE_LIST_UNKNOWN,   /* never looked up */
  GH_BADGE_LIST_NONE,      /* looked up; no list found */
  GH_BADGE_LIST_CANONICAL, /* kind 10008 (wins even when empty) */
  GH_BADGE_LIST_LEGACY     /* kind 30008 d=profile_badges */
} GhBadgeListKind;
/* Where the own account's selection comes from (§3.12 legacy migration). */
typedef enum {
  GH_BADGE_SELECTION_LOCAL,     /* the local kind-10008 document */
  GH_BADGE_SELECTION_CANONICAL, /* a looked-up 10008 not yet saved locally */
  GH_BADGE_SELECTION_LEGACY,    /* a looked-up legacy 30008 d=profile_badges */
  GH_BADGE_SELECTION_EMPTY      /* nothing known */
} GhBadgeSelectionSource;

/* Copied backend; owner is referenced by each consumer and emits "changed"
 * after any lookup, selection or binding change. Reads are cached and
 * offline; only lookup_async and publish_async touch the network, and the
 * UI asks the user before calling either. No UI object borrows a GhStore. */
typedef struct {
  GObject *owner;
  guint64 (*generation)(GObject *owner);
  const gchar *(*account)(GObject *owner); /* NULL while no store is bound */
  /* Verified badges of pubkey, from cache; NULL when never looked up. */
  GPtrArray *(*badges)(GObject *owner, const gchar *pubkey);
  GhBadgeListKind (*list_kind)(GObject *owner, const gchar *pubkey);
  /* Relays the list hinted at that no lookup contacted. */
  GStrv (*dup_hints)(GObject *owner, const gchar *pubkey);
  /* The discovery relays a lookup contacts by default. */
  GStrv (*dup_sources)(GObject *owner);
  void (*lookup_async)(GObject *owner, const gchar *pubkey, const gchar *const *approved_relays,
                       GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
  gboolean (*lookup_finish)(GObject *owner, GAsyncResult *result, GError **error);
  /* Own account: verified received awards (NULL when never looked up), the
   * ordered selection and its origin, and explicit publication. */
  GPtrArray *(*awards)(GObject *owner);
  GPtrArray *(*selection)(GObject *owner, GhBadgeSelectionSource *source, GError **error);
  gboolean (*set_selection)(GObject *owner, GPtrArray *badges, GError **error);
  GStrv (*dup_targets)(GObject *owner);
  void (*publish_async)(GObject *owner, GCancellable *cancellable, GAsyncReadyCallback callback,
                        gpointer user_data);
  gboolean (*publish_finish)(GObject *owner, GAsyncResult *result, GError **error);
  /* Artwork under the Load Remote Images consent only (PD-2). */
  gboolean (*artwork_enabled)(GObject *owner);
  void (*artwork_async)(GObject *owner, const gchar *url, GCancellable *cancellable,
                        GAsyncReadyCallback callback, gpointer user_data);
  GdkTexture *(*artwork_finish)(GObject *owner, GAsyncResult *result, GError **error);
} GhBadgesBackend;
G_END_DECLS
#endif
