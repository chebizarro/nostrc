/*
 * nsp-avatar — kind-0 `picture` avatars for result icons.
 *
 * GetResultMetas must never wait on the network, so a lookup only ever
 * checks the on-disk cache ($XDG_CACHE_HOME/nostr-search/avatars/). On a
 * miss the caller shows a themed fallback icon and the URL is queued for
 * a background download; the next query that shows the profile gets the
 * real avatar.
 *
 * Downloads (nsp-http: https only, public peers only, 1 MiB cap, at most
 * two at a time, 32 queued) are DECODED HERE and re-encoded as a 64x64
 * PNG before they are cached. GNOME Shell therefore only ever decodes a
 * small PNG we wrote, never attacker-supplied image bytes; a hostile
 * image can at worst crash this helper, never the compositor. The cache
 * is pruned oldest-first to NSP_AVATAR_CACHE_MAX_BYTES / _FILES.
 * Without gdk-pixbuf at build time, avatars are disabled entirely.
 */
#ifndef NSP_AVATAR_H
#define NSP_AVATAR_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define NSP_AVATAR_SIZE 64
#define NSP_AVATAR_MAX_DOWNLOAD (1024 * 1024)
#define NSP_AVATAR_CACHE_MAX_BYTES (8 * 1024 * 1024)
#define NSP_AVATAR_CACHE_MAX_FILES 2000

typedef struct _NspAvatars NspAvatars;

/* @app (nullable) is held while downloads run so the service does not
 * idle-exit mid-transfer. */
NspAvatars *nsp_avatars_new(const char *cache_dir, gboolean fetch_enabled, GApplication *app);
void nsp_avatars_free(NspAvatars *a);

/* Cached avatar for @url as a GFileIcon, else NULL (and, if enabled, the
 * download is queued). Never blocks. */
GIcon *nsp_avatars_lookup(NspAvatars *a, const char *url);

/* Cache file path for @url (exists or not). */
char *nsp_avatars_path(NspAvatars *a, const char *url);

/* Number of queued + running downloads (tests). */
guint nsp_avatars_pending(NspAvatars *a);

/* Delete oldest files until @dir holds <= 80 % of both limits whenever
 * either is exceeded. Returns the number removed. */
guint nsp_avatars_prune(const char *dir, guint64 max_bytes, guint max_files);

/* Decode @image and write a square NSP_AVATAR_SIZE PNG to @out_path
 * (atomically). FALSE if gdk-pixbuf is unavailable or the image is bad. */
gboolean nsp_avatar_transcode(GBytes *image, const char *out_path, GError **error);

G_END_DECLS

#endif /* NSP_AVATAR_H */
