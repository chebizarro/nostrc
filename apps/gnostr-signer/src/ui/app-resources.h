#pragma once

// Global resource path for GNostr Signer UI templates
// All GtkBuilder templates are compiled into GResource under this prefix.
// gtk_widget_class_set_template_from_resource expects a resource PATH (not URI).
#ifndef APP_RESOURCE_PATH
#define APP_RESOURCE_PATH "/org/gnostr/signer"
#endif
