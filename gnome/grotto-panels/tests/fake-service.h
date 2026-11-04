/* SPDX-License-Identifier: MIT */
#pragma once
#include <grotto-panels.h>
#define GROTTO_TYPE_PANELS_FAKE_SERVICE (grotto_panels_fake_service_get_type())
G_DECLARE_FINAL_TYPE(GrottoPanelsFakeService, grotto_panels_fake_service, GROTTO, PANELS_FAKE_SERVICE, GObject)
GrottoPanelsFakeService *grotto_panels_fake_service_new(void);
void grotto_panels_fake_service_set_available(GrottoPanelsFakeService *self, gboolean available);
void grotto_panels_fake_service_set_can(GrottoPanelsFakeService *self, GrottoPanelCommand command, gboolean can);
gboolean grotto_panels_fake_service_replace(GrottoPanelsFakeService *self, GrottoPanelsModel model,
  const GrottoPanelsMetadata *metadata, GError **error);
/* G_IO_ERROR contract matches Control1 normalization. -1 means success.
 * Deferred completion gives deterministic in-flight latency, without sleeps.
 * Cancellation completes pending tasks; owner loss cancels and clears state. */
void grotto_panels_fake_service_set_query_result(GrottoPanelsFakeService *self, GrottoPanelsQuery query,
  const GrottoPanelsMetadata *rows, gsize length, gint failure);
void grotto_panels_fake_service_set_failure(GrottoPanelsFakeService *self, GrottoPanelCommand command, gint code);
void grotto_panels_fake_service_set_deferred(GrottoPanelsFakeService *self, gboolean deferred);
gboolean grotto_panels_fake_service_complete_next(GrottoPanelsFakeService *self);
guint grotto_panels_fake_service_get_pending(GrottoPanelsFakeService *self);
guint grotto_panels_fake_service_get_invocations(GrottoPanelsFakeService *self);
GrottoPanelCommand grotto_panels_fake_service_get_last_command(GrottoPanelsFakeService *self);
const GrottoPanelCommandArgs *grotto_panels_fake_service_get_last_args(GrottoPanelsFakeService *self);
