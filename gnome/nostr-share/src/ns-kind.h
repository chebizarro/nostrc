/* ns-kind.h - Map "whatever GNOME handed us" to a Nostr event kind
 *
 * SPDX-License-Identifier: MIT
 *
 * Pure (no I/O). Two steps:
 *
 *   1. ns_kind_classify() turns a MIME type / filename / directory flag
 *      into an NsInputClass.
 *   2. ns_kind_resolve() turns a class (+ text length, + optional
 *      --kind override) into an NsAction, rejecting overrides that make
 *      no sense for the input (e.g. --kind 1063 on plain text).
 *
 * The full table lives in gnome/nostr-share/README.md ("Kind mapping").
 */
#ifndef NS_KIND_H
#define NS_KIND_H

#include <glib.h>

G_BEGIN_DECLS

#define NS_ERROR (ns_error_quark())
GQuark ns_error_quark(void);

typedef enum {
  NS_ERROR_BAD_INPUT = 1,     /* unreadable / unsupported input */
  NS_ERROR_BAD_KIND,          /* --kind not applicable to this input */
  NS_ERROR_METADATA,          /* media metadata could not be stripped */
  NS_ERROR_TOO_LARGE,
  NS_ERROR_NO_SIGNER,
  NS_ERROR_NO_RELAYS,
  NS_ERROR_NO_SERVERS,
  NS_ERROR_UPLOAD,
  NS_ERROR_PUBLISH,
  NS_ERROR_DAV,
  NS_ERROR_GIT,
  /* Held by the session relay, upstream delivery not confirmed yet
   * (nostrc-t24q): it is still being delivered; sharing again duplicates. */
  NS_ERROR_QUEUED,
} NsError;

typedef enum {
  NS_CLASS_TEXT = 0,    /* plain text (argument or text/plain file) */
  NS_CLASS_MARKDOWN,    /* text/markdown, *.md */
  NS_CLASS_URL,         /* http(s) URL(s), text/uri-list */
  NS_CLASS_MEDIA,       /* image/…, video/…, audio/… */
  NS_CLASS_CALENDAR,    /* text/calendar, *.ics */
  NS_CLASS_CONTACT,     /* text/vcard, *.vcf */
  NS_CLASS_GIT_REPO,    /* directory containing a git repository */
  NS_CLASS_DIRECTORY,   /* any other directory: unsupported */
  NS_CLASS_OTHER_FILE,  /* application/pdf and everything else */
} NsInputClass;

typedef enum {
  NS_ACTION_NOTE = 0,       /* kind 1 (+ r tags for URLs) */
  NS_ACTION_ARTICLE,        /* kind 30023 (NIP-23) */
  NS_ACTION_MEDIA_NOTE,     /* Blossom upload + kind 1 with imeta (NIP-92) */
  NS_ACTION_FILE_METADATA,  /* Blossom upload + kind 1063 (NIP-94) */
  NS_ACTION_GIT_REPO,       /* kind 30617 (NIP-34) */
  NS_ACTION_DAV_CALENDAR,   /* staged into nostr-dav (it owns NIP-52) */
  NS_ACTION_DAV_CONTACT,    /* staged into nostr-dav (it owns kind 30085) */
  NS_ACTION_PRIVATE_MESSAGE,/* NIP-17 kind-14 rumor, gift-wrapped (--private) */
  NS_ACTION_PRIVATE_FILE,   /* NIP-17 kind-15 rumor + encrypted Blossom blob */
} NsAction;

#define NS_KIND_NOTE          1
#define NS_KIND_GROUP_CHAT    9      /* NIP-29 group chat message */
#define NS_KIND_FILE_METADATA 1063
#define NS_KIND_ARTICLE       30023
#define NS_KIND_GIT_REPO      30617

NsInputClass ns_kind_classify(const gchar *mime,
                              const gchar *filename,
                              gboolean     is_dir,
                              gboolean     is_git_repo);

/* @forced_kind: 0 for "pick the default". Plain text is always a
 * kind-1 note by default, whatever its length: promoting it to 30023
 * would give it replaceable (d-tag) semantics nobody asked for. */
gboolean ns_kind_resolve(NsInputClass  cls,
                         gint          forced_kind,
                         NsAction     *out_action,
                         GError      **error);

/* Event kind produced by @action; 0 for the nostr-dav hand-off actions
 * (nostr-dav chooses the calendar/contact kind itself). */
gint ns_action_kind(NsAction action);

/* The kinds the dialog's picker should offer for @cls, default first.
 * Returns the count written to @out_kinds (<= 3). */
guint ns_kind_choices(NsInputClass cls, gint out_kinds[3]);

const gchar *ns_class_name(NsInputClass cls);
const gchar *ns_kind_label(gint kind);

G_END_DECLS

#endif /* NS_KIND_H */
