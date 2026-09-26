/*
 * nd-registry — kind -> handler resolution (mimeapps.list-style).
 *
 * Sources, highest precedence first:
 *   1. handlers.list files, in order:
 *        $XDG_CONFIG_HOME/nostr/handlers.list      (user)
 *        $XDG_CONFIG_DIRS/nostr/handlers.list       (e.g. /etc/xdg)
 *        $XDG_DATA_DIRS/nostr/handlers.list         (e.g. /usr/share)
 *      `[Default Handlers]` keys are kind tokens (N, A-B; see nd-kinds.h);
 *      values are ';'-separated desktop ids, first installed one wins.
 *      Within one file the exact key beats the narrowest containing range
 *      (ties: first key in file order). The FIRST FILE with a usable hit
 *      wins outright, so a user range beats a system exact key — user
 *      configuration always wins, exactly like mimeapps.list's per-file
 *      precedence.
 *   2. X-Nostr-Kinds= declarations of installed desktop entries: exact
 *      beats narrowest range; ties broken by desktop id (byte order), so
 *      the result is deterministic. `*` is not considered here.
 *   3. NIP-89 discovery hook (stub; nd-nip89.c).
 *   4. Fallback (also the ONLY step for links whose kind is unknown):
 *      the `*` key of `[Default Handlers]` (file order), then installed
 *      apps declaring `*` (desktop id order).
 *
 * `[Removed Handlers]` (any file) takes kind-token keys and desktop-id
 * values and hides those apps' X-Nostr-Kinds declarations for the covered
 * kinds (`*` key = all kinds) — the equivalent of mimeapps.list's
 * [Removed Associations]. It does not affect explicit defaults.
 *
 * `[Dispatcher] fetch-relay-hints=false` (first file that sets it wins)
 * disables contacting relays named in links.
 *
 * The dispatcher's own desktop id is never returned (loop guard).
 */
#ifndef ND_REGISTRY_H
#define ND_REGISTRY_H

#include <glib.h>

G_BEGIN_DECLS

#define ND_SELF_DESKTOP_ID "org.nostr.Dispatcher.desktop"

typedef struct {
  char *desktop_id;
  GArray *kinds;       /* NdKindRange; NULL if the key is absent */
} NdAppDecl;

NdAppDecl *nd_app_decl_new(const char *desktop_id, const char *kinds_spec);
void nd_app_decl_free(gpointer decl);

typedef enum {
  ND_SOURCE_NONE = 0,
  ND_SOURCE_HANDLERS_LIST,
  ND_SOURCE_DECLARED,
  ND_SOURCE_NIP89,
  ND_SOURCE_FALLBACK_LIST,
  ND_SOURCE_FALLBACK_DECLARED,
} NdSource;

const char *nd_source_name(NdSource s);

typedef struct NdRegistry NdRegistry;

/* @config_files: highest precedence first; missing files are skipped.
 * @apps: GPtrArray of NdAppDecl (every installed app, declaring or not);
 *        ownership is taken. */
NdRegistry *nd_registry_new(const char *const *config_files, GPtrArray *apps);

/* XDG config paths + g_app_info_get_all(). */
NdRegistry *nd_registry_new_default(void);

void nd_registry_free(NdRegistry *reg);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NdRegistry, nd_registry_free)

/* Desktop id handling @kind (kind < 0: unknown kind -> fallback only), or
 * NULL. */
char *nd_registry_choose(NdRegistry *reg, gint kind, NdSource *out_source);

gboolean nd_registry_fetch_relay_hints(NdRegistry *reg);

/* handlers.list search path, highest precedence first. */
char **nd_registry_config_files(void);
char *nd_registry_user_config_file(void);

/* `^[A-Za-z0-9_.-]+\.desktop$` and not the dispatcher itself. */
gboolean nd_desktop_id_valid(const char *desktop_id);

/* Write `[Default Handlers] <key>=<desktop_id>;` into @path, preserving
 * the rest of the file (comments included). @key is a kind token. */
gboolean nd_registry_set_default(const char *path, const char *key,
                                 const char *desktop_id, GError **error);

G_END_DECLS

#endif /* ND_REGISTRY_H */
