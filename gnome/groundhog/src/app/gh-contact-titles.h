#ifndef GH_CONTACT_TITLES_H
#define GH_CONTACT_TITLES_H

#include "gh-contact-directory.h"

G_BEGIN_DECLS

/*
 * GhContactTitles (nostrc-qp24.66; privacy charter G10, PT-8, PD-2): titles
 * the model's accepted private (NIP-17) conversations with their peers'
 * display names from the contact directory's cache
 * (gh_contact_directory_dup_conversation_title(), set with
 * gh_conversation_set_contact_title()), so the conversation list, the
 * conversation header, search and notifications show "Alice" instead of an
 * npub. It follows the directory's "profile-changed", conversations that
 * are listed and requests that are accepted, live. It never asks for
 * anything: the directory refreshes accepted contacts only, and a message
 * request has no name here (its title stays its npubs). No pictures.
 * Main context only.
 */
#define GH_TYPE_CONTACT_TITLES (gh_contact_titles_get_type())
G_DECLARE_FINAL_TYPE(GhContactTitles, gh_contact_titles, GH, CONTACT_TITLES, GObject)

/* Both are referenced. */
GhContactTitles *gh_contact_titles_new(GhConversationStore *model,
                                       GhContactDirectory *directory);

G_END_DECLS
#endif
