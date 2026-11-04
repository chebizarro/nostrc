/* SPDX-License-Identifier: MIT */
#pragma once
#include <grotto-panels.h>
#include "home-model.h"
#define GROTTO_TYPE_IDENTITY_SERVICE (grotto_identity_service_get_type())
G_DECLARE_FINAL_TYPE(GrottoIdentityService, grotto_identity_service, GROTTO, IDENTITY_SERVICE, GObject)
GrottoPanelsService *grotto_identity_service_new(GrottoHomeModel *control);
GrottoPanelsService *grotto_identity_service_get_control(GrottoIdentityService *self);
