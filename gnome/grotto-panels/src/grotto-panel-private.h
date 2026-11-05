/* SPDX-License-Identifier: MIT */
#pragma once
#include "grotto-panels.h"
G_GNUC_INTERNAL void grotto_panel_bind_header_bar(GrottoPanel *self, AdwHeaderBar *header);
G_GNUC_INTERNAL void grotto_panel_forward_command_async(GrottoPanel *self,
  GrottoPanelCommand command, const GrottoPanelCommandArgs *args,
  GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data);
