/* SPDX-License-Identifier: MIT */
#pragma once
#include "grotto-panels.h"
#define GROTTO_TYPE_PROFILE_VIEW (grotto_profile_view_get_type())
G_GNUC_INTERNAL G_DECLARE_FINAL_TYPE(GrottoProfileView, grotto_profile_view, GROTTO, PROFILE_VIEW, GtkBox)
G_GNUC_INTERNAL GtkWidget *grotto_profile_view_new(GrottoPanel *panel);
G_GNUC_INTERNAL void grotto_profile_view_set_identity(GrottoProfileView *self,
  const char *key, const char *label, gboolean editable);
