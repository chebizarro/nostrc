/* sheet-online-accounts.h - GNOME Online Accounts onboarding wizard
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SHEET_ONLINE_ACCOUNTS_H
#define SHEET_ONLINE_ACCOUNTS_H

#include <adwaita.h>

G_BEGIN_DECLS

#define SHEET_TYPE_ONLINE_ACCOUNTS (sheet_online_accounts_get_type())
G_DECLARE_FINAL_TYPE(SheetOnlineAccounts, sheet_online_accounts,
                     SHEET, ONLINE_ACCOUNTS, AdwDialog)

SheetOnlineAccounts *sheet_online_accounts_new(void);

G_END_DECLS
#endif /* SHEET_ONLINE_ACCOUNTS_H */
