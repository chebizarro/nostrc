/* nn-core.h - nostr-nautilus menu gating and argv construction
 *
 * SPDX-License-Identifier: MIT
 *
 * Pure (no I/O except nn_dir_is_git_repo()): the Nautilus provider turns
 * each NautilusFileInfo into an NnFile, asks which NnActions apply to the
 * selection, and turns the chosen action into the argv(s) to launch.
 * Everything that knows about Nautilus lives in nostr-nautilus.c so this
 * file (and its test) builds on hosts without libnautilus-extension.
 */
#ifndef NN_CORE_H
#define NN_CORE_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define NN_SEALED_MIME   "application/vnd.nostr.sealed"
#define NN_SEALED_SUFFIX ".nsealed"

/* One selected file, as the provider saw it. Strings are borrowed. */
typedef struct {
  const gchar *name;        /* basename (for extension checks) */
  const gchar *mime;        /* Nautilus' MIME type; may be NULL */
  GFileType    type;        /* G_FILE_TYPE_REGULAR / _DIRECTORY / … */
  const gchar *path;        /* local path, or NULL for non-native (GVfs) files */
  const gchar *uri;         /* always set */
  gboolean     is_git_repo; /* directory with a .git entry (nn_dir_is_git_repo) */
} NnFile;

/* The installed tools. A NULL member hides the actions that need it. */
typedef struct {
  const gchar        *share_exe;    /* nostr-share (from org.nostr.Share.desktop) */
  const gchar *const *share_mimes;  /* that desktop file's MimeType= list */
  const gchar        *seal_exe;     /* nostr-seal */
  const gchar        *seal_gtk_exe; /* nostr-seal-gtk (preferred for Decrypt) */
  const gchar        *seal_helper;  /* libexec/nostr-nautilus-seal (Encrypt dialog) */
} NnTools;

typedef enum {
  NN_ACTION_SHARE = 0,  /* nostr-share FILE… */
  NN_ACTION_UPLOAD,     /* nostr-share --kind 1063 FILE… */
  NN_ACTION_ENCRYPT,    /* nostr-nautilus-seal FILE… → nostr-seal encrypt --to … FILE */
  NN_ACTION_DECRYPT,    /* nostr-seal-gtk FILE… | nostr-seal decrypt FILE (each) */
  NN_N_ACTIONS
} NnAction;

/* TRUE if @mime is covered by one of @patterns (exact, an "image/<star>" wildcard, or a
 * shared-mime-info subclass such as text/x-csrc ⊂ text/plain). */
gboolean  nn_mime_in_list (const gchar *mime, const gchar *const *patterns);

gboolean  nn_file_is_sealed (const NnFile *file);

/* Does @action apply to every file of the selection with these tools? */
gboolean  nn_action_applies (NnAction action, const NnFile *files, guint n_files,
                             const NnTools *tools);

/* The argv(s) to launch, as a GPtrArray of GStrv (free func set), or NULL
 * when the action does not apply. Usually one argv; the CLI decrypt
 * fallback yields one per file. */
GPtrArray *nn_action_argvs (NnAction action, const NnFile *files, guint n_files,
                            const NnTools *tools);

/* A desktop-entry Exec= line that expands back to exactly @argv:
 * each argument shell-quoted, every '%' doubled (field-code escape). */
gchar    *nn_exec_line (const gchar *const *argv);

/* The directory test nostr-share itself uses (ns-git.c): DIR/.git exists. */
gboolean  nn_dir_is_git_repo (const gchar *path);

/* --- Encrypt helper ------------------------------------------------------ */

/* Split @text on whitespace / ',' / ';', strip "nostr:" prefixes, accept
 * npub1… (bech32 charset, 63 chars) and 64-hex keys, drop duplicates.
 * An empty @text yields an empty (non-NULL) vector. */
gboolean  nn_parse_recipients (const gchar *text, GStrv *out, GError **error);

/* nostr-seal encrypt [--to R]… [--to-self] PATH */
GStrv     nn_seal_encrypt_argv (const gchar *seal_exe, const gchar *const *recipients,
                                gboolean to_self, const gchar *path);

G_END_DECLS

#endif /* NN_CORE_H */
