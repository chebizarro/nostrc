#ifndef GH_ADD_CONTACT_PAGE_H
#define GH_ADD_CONTACT_PAGE_H

#include <adwaita.h>
#include "gh-conversation-store.h"
#include "gh-nip05.h"
#include "gh-window.h"

G_BEGIN_DECLS

/*
 * GhAddContactPage (nostrc-txnu, charter §7.9, PT-8;
 * data/ui/gh-add-contact-page.blp): Add Contact by npub, nostr: URI, hex
 * pubkey or NIP-05 address, a sidebar panel pushed over New Message
 * (gh_window_push_flow()). Back or Escape returns to New Message; the field
 * takes focus when the page shows; a lookup still running is cancelled when
 * the page leaves the window.
 *
 * Nothing touches the network before the user acts. The parsed result is
 * shown; NIP-05 needs an explicit "Look Up This Address" row. "Add" opens
 * (or creates) an accepted DM conversation with the person, making them an
 * accepted contact in the contact directory.
 *
 * Signal "contact-added": emitted with the GhConversation* of the room
 * opened for the new contact (the page stays; its owner closes the panels).
 */

typedef struct {
  GhConversationStore *conversations;    /* required */
  GhNip05 *nip05;                        /* nullable: no address lookups */
  /* Nullable: cached display name for a known person. */
  const gchar *(*display_name)(gpointer data, const gchar *pubkey);
  gpointer names_data;
} GhAddContactConfig;

#define GH_TYPE_ADD_CONTACT_PAGE (gh_add_contact_page_get_type())
G_DECLARE_FINAL_TYPE(GhAddContactPage, gh_add_contact_page, GH, ADD_CONTACT_PAGE,
                     AdwNavigationPage)

GhAddContactPage *gh_add_contact_page_new(const GhAddContactConfig *config);

/* Back: pops the page (and anything over it) from its navigation view. */
void gh_add_contact_page_close(GhAddContactPage *self);

/* Installs win.add-contact on window: pushes the page over New Message
 * (gh_window_push_flow()); once added, the sidebar returns to the list and
 * the conversation opens, focused. The action is enabled while
 * config->conversations has an account. */
void gh_add_contact_attach(GhWindow *window, const GhAddContactConfig *config);

G_END_DECLS
#endif
