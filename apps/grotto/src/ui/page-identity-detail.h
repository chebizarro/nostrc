#pragma once
#include <adwaita.h>
G_BEGIN_DECLS
#define GROTTO_TYPE_IDENTITY_DETAIL (grotto_identity_detail_get_type())
G_DECLARE_FINAL_TYPE(GrottoIdentityDetail, grotto_identity_detail, GROTTO, IDENTITY_DETAIL, AdwNavigationPage)
/* NULL identity offers the daemon's signable inventory; npub pins a detail page. */
GtkWidget *page_identity_detail_new(const gchar *npub);
void page_identity_detail_present(GtkWidget *parent);
void page_identity_detail_present_identity(GtkWidget *parent, const gchar *npub);
G_END_DECLS
