#ifndef GH_MLS_CONTEXT_H
#define GH_MLS_CONTEXT_H

#include "gh-mls-service.h"

G_BEGIN_DECLS

/* What the encrypted-group dialogs work with (nostrc-9xf5). Everything is
 * borrowed: the dialog holds references to the objects while it lives. */

/* The name Groundhog has cached for pubkey (e.g. the contact directory's),
 * or NULL. Display only, never fetched (same shape as GhGroupNameFunc). */
typedef const gchar *(*GhMlsNameFunc)(const gchar *pubkey, gpointer user_data);

typedef struct {
  GhMlsService *service;          /* required: the open store's */
  GhAccountController *accounts;  /* required: KeyPackage checks */
  GhConversationStore *model;     /* required: contacts and rooms */
  GSettings *settings;            /* nullable: discovery-relays */
  GhMlsNameFunc display_name;     /* nullable */
  gpointer names_data;
  /* New groups start with these relays (e.g. the account's own 10002 write
   * relays, a signed list: charter P1); NULL: none, the user adds them. */
  const gchar *const *default_relays;
  guint lookup_deadline;          /* seconds per lookup phase; 0: default */
} GhMlsUiContext;

G_END_DECLS
#endif
