/* SPDX-License-Identifier: MIT */
#pragma once
#include <grotto-panels.h>
/* Trusted Grotto host bridge. The panel never calls D-Bus or creates offers. */
void grotto_connection_link_bind(GrottoRemotePanel *panel, GDBusConnection *bus);
