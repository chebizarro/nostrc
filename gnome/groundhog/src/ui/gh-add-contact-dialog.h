#ifndef GH_ADD_CONTACT_DIALOG_H
#define GH_ADD_CONTACT_DIALOG_H

#include <adwaita.h>
#include "gh-conversation-store.h"
#include "gh-nip05.h"
#include "gh-window.h"

G_BEGIN_DECLS

/*
 * GhAddContactDialog (nostrc-txnu, charter §7.9, PT-8): Add Contact
 * by npub, nostr: URI, hex pubkey or NIP-05 address.
 *
 * Nothing touches the network before the user acts. The parsed result is
 * shown; NIP-05 needs an explicit "Look Up This Address" row. "Add" opens
 * (or creates) an accepted DM conversation with the person, making them an
 * accepted contact in the contact directory.
 *
 * Signal "contact-added": emitted with the GhConversation* of the room
 * opened for the new contact.
 */

typedef struct {
  GhConversationStore *conversations;    /* required */
  GhNip05 *nip05;                        /* nullable: no address lookups */
  /* Nullable: cached display name for a known person. */
  const gchar *(*display_name)(gpointer data, const gchar *pubkey);
  gpointer names_data;
} GhAddContactConfig;

#define GH_TYPE_ADD_CONTACT_DIALOG (gh_add_contact_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhAddContactDialog, gh_add_contact_dialog, GH, ADD_CONTACT_DIALOG,
                     AdwDialog)

GhAddContactDialog *gh_add_contact_dialog_new(const GhAddContactConfig *config);

/* Installs win.add-contact on window, presenting the dialog. The action
 * is enabled while config->conversations has an account. */
void gh_add_contact_attach(GhWindow *window, const GhAddContactConfig *config);

G_END_DECLS
#endif
