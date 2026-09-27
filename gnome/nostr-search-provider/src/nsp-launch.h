/*
 * nsp-launch — LaunchSearch argv construction (nostrc-prqu.15 contract,
 * see README "Activation"). Pure: no GDesktopAppInfo, no spawning, so the
 * rules are unit-tested on every platform (tests/test_nsp_launch.c).
 *
 * A Nostr app declares its search entry point in its desktop entry:
 *   X-Nostr-Search-Arg=--search      the option that takes the terms
 *   Actions=search;                  optional [Desktop Action search]
 *   [Desktop Action search]
 *   Exec=gnostr --search ""
 */
#ifndef NSP_LAUNCH_H
#define NSP_LAUNCH_H

#include <glib.h>

G_BEGIN_DECLS

#define NSP_SEARCH_ACTION "search"
#define NSP_SEARCH_ARG_KEY "X-Nostr-Search-Arg"
#define NSP_SEARCH_TERMS_MAX 1024 /* bytes, after joining */

/* From a parsed desktop entry: the declared search option (NULL: the app
 * has no search entry point) and the Exec line to build the command on —
 * [Desktop Action search]'s when the entry lists that action (Flatpak
 * exports rewrite both lines), else [Desktop Entry]'s. @out_has_action
 * tells whether the `search` action exists. Returns FALSE when there is no
 * usable search entry point. */
gboolean nsp_search_entry_from_keyfile(GKeyFile *kf, char **out_search_arg,
                                       char **out_exec, gboolean *out_has_action);

/* argv (no shell) that runs @exec with the terms given to @search_arg:
 *  - field codes (%u %U %f %F %i %c %k …) and Flatpak's @@ / @@u / @@f
 *    file-forwarding markers are dropped, %% becomes %; an argument with
 *    any other embedded field code is refused;
 *  - an existing "@search_arg VALUE" / "@search_arg=VALUE" in @exec (the
 *    action's empty-terms form) is replaced, otherwise the option is
 *    appended;
 *  - the terms are joined with single spaces and passed as ONE argument,
 *    "--opt=TERMS" for a long option (so terms starting with '-' cannot be
 *    taken for options), else "-o" "TERMS";
 *  - terms must be UTF-8 without control characters, at most
 *    NSP_SEARCH_TERMS_MAX bytes joined. */
char **nsp_search_argv(const char *exec, const char *search_arg,
                       const char *const *terms, GError **error);

/* Terms joined with single spaces (empty terms skipped); never NULL. */
char *nsp_search_join_terms(const char *const *terms);

G_END_DECLS

#endif /* NSP_LAUNCH_H */
