/* test-groundhog-group-ui links this instead of gh-send-ui.c (whose account
 * stack the group UI does not need): it records what gh-group-ui.c hands the
 * send UI, so the test drives the real composer delegate on a real window. */
#ifndef GH_TEST_GROUP_SEND_STUB_H
#define GH_TEST_GROUP_SEND_STUB_H

#include "gh-send-ui.h"

/* The delegate and data set last (NULL delegate: none). */
const GhSendUiDelegate *group_send_stub_delegate(gpointer *out_data);
/* How often gh_send_ui_refresh() was called. */
guint group_send_stub_refreshes(void);
void group_send_stub_reset(void);

#endif
