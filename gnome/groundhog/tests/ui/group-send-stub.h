/* test-groundhog-group-ui and test-groundhog-mls-ui link this instead of
 * gh-send-ui.c (whose account stack the group UIs do not need): it records
 * what gh-group-ui.c and gh-mls-ui.c hand the send UI, so the tests drive
 * the real composer delegates on a real window. */
#ifndef GH_TEST_GROUP_SEND_STUB_H
#define GH_TEST_GROUP_SEND_STUB_H

#include "gh-send-ui.h"

/* The delegate and data set last with gh_send_ui_set_delegate() (NULL
 * delegate: none). */
const GhSendUiDelegate *group_send_stub_delegate(gpointer *out_data);
/* The delegate (set or added) that handles conversation, as the send UI
 * picks it, or NULL. */
const GhSendUiDelegate *group_send_stub_delegate_for(GhConversation *conversation,
                                                     gpointer *out_data);
/* How often gh_send_ui_refresh() was called. */
guint group_send_stub_refreshes(void);
void group_send_stub_reset(void);

#endif
