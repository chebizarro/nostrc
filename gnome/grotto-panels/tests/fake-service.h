/* SPDX-License-Identifier: MIT */
#pragma once
#include <grotto-panels.h>
#define GROTTO_TYPE_PANELS_FAKE_SERVICE (grotto_panels_fake_service_get_type())
G_DECLARE_FINAL_TYPE(GrottoPanelsFakeService, grotto_panels_fake_service, GROTTO, PANELS_FAKE_SERVICE, GObject)
GrottoPanelsFakeService *grotto_panels_fake_service_new(void);
void grotto_panels_fake_service_set_available(GrottoPanelsFakeService *self, gboolean available);
void grotto_panels_fake_service_set_can(GrottoPanelsFakeService *self, GrottoPanelCommand command, gboolean can);
void grotto_panels_fake_service_replace(GrottoPanelsFakeService *self, GrottoPanelsModel model, const GrottoPanelsMetadata *metadata);
