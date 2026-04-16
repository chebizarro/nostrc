/* gnostr-nip51-list-picker-dialog.h — AdwDialog for picking a NIP-51
 * kind-30000 list to import as a Custom filter set.
 *
 * SPDX-License-Identifier: MIT
 *
 * The dialog loads the current user's categorized people lists via
 * gnostr_nip51_load_user_lists_async() and presents them as a single-
 * selection row list. Selecting a list closes the dialog and emits
 * the "list-selected" signal with the chosen list's identifier /
 * title so the caller can convert it to a FilterSet.
 *
 * nostrc-yg8j.8.
 */

#ifndef GNOSTR_NIP51_LIST_PICKER_DIALOG_H
#define GNOSTR_NIP51_LIST_PICKER_DIALOG_H

#include <adwaita.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_NIP51_LIST_PICKER_DIALOG \
    (gnostr_nip51_list_picker_dialog_get_type())
G_DECLARE_FINAL_TYPE(GnostrNip51ListPickerDialog,
                     gnostr_nip51_list_picker_dialog,
                     GNOSTR, NIP51_LIST_PICKER_DIALOG, AdwDialog)

/**
 * gnostr_nip51_list_picker_dialog_new:
 * @pubkey_hex: 64-hex-char pubkey whose kind-30000 lists to load
 *
 * Construct a new picker dialog. The async load kicks off from
 * constructed().
 *
 * Returns: (transfer full): a new dialog instance.
 */
GtkWidget *gnostr_nip51_list_picker_dialog_new(const gchar *pubkey_hex);

/**
 * Signals:
 *
 *   "list-selected" (const gchar *title,
 *                    const gchar *identifier,
 *                    gpointer     nostr_list_ptr)
 *     Emitted after the user picks a list. @nostr_list_ptr is a
 *     borrowed <type>NostrList*</type> owned by the dialog — valid
 *     only during the signal emission. Callers that need the list
 *     beyond the callback must convert it immediately (e.g. via
 *     gnostr_nip51_list_to_filter_set()).
 */

G_END_DECLS

#endif /* GNOSTR_NIP51_LIST_PICKER_DIALOG_H */
