#ifndef GH_MLS_ATTACHMENT_UI_H
#define GH_MLS_ATTACHMENT_UI_H

#include "gh-attachment-ui.h"
#include "gh-mls-attachments.h"

G_BEGIN_DECLS

/* Files in the window's encrypted groups (W25, nostrc-q3a6): installs
 * files (a reference is kept) as the attachment UI's group delegate
 * (gh_attachment_ui_set_groups()), so an MLS conversation gets the attach
 * button, the sheet and the cards with the NIP-17 rules, and its files go
 * through gh_mls_attachments_send_async(). Files go where the composer's
 * text can (gh_mls_send_reason()), and the attach button follows the shown
 * group as it changes (gh_attachment_ui_groups_changed()). After
 * gh_attachment_ui_attach() and gh_mls_ui_attach(). */
void gh_mls_attachment_ui_attach(GhWindow *window, GhMlsAttachments *files);

G_END_DECLS
#endif
