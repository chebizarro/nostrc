#ifndef GH_MLS_NEW_GROUP_PAGE_H
#define GH_MLS_NEW_GROUP_PAGE_H

#include <adwaita.h>

#include "gh-mls-context.h"

G_BEGIN_DECLS

/*
 * GhMlsRelayRow (data/ui/gh-mls-relay-row.blp): one group relay, with
 * Remove (relay.remove, signal "remove") when removable.
 */
#define GH_TYPE_MLS_RELAY_ROW (gh_mls_relay_row_get_type())
G_DECLARE_FINAL_TYPE(GhMlsRelayRow, gh_mls_relay_row, GH, MLS_RELAY_ROW, AdwActionRow)

GhMlsRelayRow *gh_mls_relay_row_new(const gchar *url, gboolean removable);
const gchar *gh_mls_relay_row_get_url(GhMlsRelayRow *self);

/*
 * GhMlsNewGroupPage (data/ui/gh-mls-new-group-page.blp; charter §7.9;
 * nostrc-9xf5): the "Encrypted Group" page of New Group
 * (gh_new_group_dialog_add_encrypted_page()).
 *
 *  - This device's approval (GhMlsService:identity-state, followed live):
 *    "Waiting for approval in Nostr Signer…", or declined/failed with Try
 *    Again (gh_mls_service_retry_identity()); hidden once approved.
 *  - Name, optional description, members (GhMlsInviteePicker: accepted
 *    contacts, each with its KeyPackage check) and the group relays (the
 *    context's default relays to start with; add and remove, 1 to 16).
 *  - Create (mls-new.create) is sensitive only when everything is ready;
 *    otherwise the reason is under it. It calls
 *    gh_mls_service_create_group_async() and shows the status: creating,
 *    created (Open Group: "open-group" with the GhMlsGroup, then the
 *    enclosing dialog closes) or not created (gh_mls_error_copy(), Change
 *    and Try Again).
 * Actions (widget actions): mls-new.create, mls-new.open, mls-new.back,
 * mls-new.retry-identity.
 */
#define GH_TYPE_MLS_NEW_GROUP_PAGE (gh_mls_new_group_page_get_type())
G_DECLARE_FINAL_TYPE(GhMlsNewGroupPage, gh_mls_new_group_page, GH, MLS_NEW_GROUP_PAGE,
                     AdwNavigationPage)

GhMlsNewGroupPage *gh_mls_new_group_page_new(const GhMlsUiContext *context);

/* For tests and the window glue. */
struct _GhMlsInviteePicker *gh_mls_new_group_page_get_picker(GhMlsNewGroupPage *self);
void gh_mls_new_group_page_set_name(GhMlsNewGroupPage *self, const gchar *name);
/* Adds a relay as typed ("Add Relay"); FALSE with the shown reason. */
gboolean gh_mls_new_group_page_add_relay(GhMlsNewGroupPage *self, const gchar *text);
GStrv gh_mls_new_group_page_dup_relays(GhMlsNewGroupPage *self);
/* Why Create is insensitive, or NULL when it can run. */
const gchar *gh_mls_new_group_page_get_create_reason(GhMlsNewGroupPage *self);
/* What the page says of the group's format (nostrc-lf62): that it will be
 * the older one, or NULL. */
const gchar *gh_mls_new_group_page_get_format_notice(GhMlsNewGroupPage *self);
/* Whether the choice between the people of each format is offered (they
 * share none; "mls-new.keep-adopted" and "mls-new.keep-legacy" take it). */
gboolean gh_mls_new_group_page_get_format_choice(GhMlsNewGroupPage *self);
/* The identity row's title, NULL when hidden. */
const gchar *gh_mls_new_group_page_get_identity_title(GhMlsNewGroupPage *self);
const gchar *gh_mls_new_group_page_get_status_title(GhMlsNewGroupPage *self);
/* The group made by Create, once created; NULL before. */
GhMlsGroup *gh_mls_new_group_page_get_group(GhMlsNewGroupPage *self);

G_END_DECLS
#endif
