#pragma once
#include <adwaita.h>
#include "../home-model.h"
#define GROTTO_TYPE_CLIENT_DETAIL (grotto_client_detail_get_type())
G_DECLARE_FINAL_TYPE(GrottoClientDetail, grotto_client_detail, GROTTO, CLIENT_DETAIL, AdwNavigationPage)
GtkWidget *grotto_client_detail_new(GrottoHomeModel *, const gchar *principal);
