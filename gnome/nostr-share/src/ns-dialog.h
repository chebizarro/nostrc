/* ns-dialog.h - libadwaita "Share to Nostr" window
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NS_DIALOG_H
#define NS_DIALOG_H

#include "ns-share.h"

G_BEGIN_DECLS

/* Runs the dialog to completion. Takes ownership of @share (may be NULL
 * when @load_error explains why nothing could be loaded). Returns the
 * process exit status. */
int ns_dialog_run(NsShare *share, const GError *load_error);

G_END_DECLS

#endif /* NS_DIALOG_H */
