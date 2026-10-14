#pragma once
#include <gio/gio.h>
G_BEGIN_DECLS
#define GROTTO_TYPE_HOME_MODEL (grotto_home_model_get_type())
G_DECLARE_FINAL_TYPE(GrottoHomeModel, grotto_home_model, GROTTO, HOME_MODEL, GObject)
GrottoHomeModel *grotto_home_model_new(void);
void grotto_home_model_start(GrottoHomeModel *self);
void grotto_home_model_stop(GrottoHomeModel *self);
void grotto_home_model_refresh(GrottoHomeModel *self);
/* Borrowed daemon snapshots, invalidated together on owner loss. */
GVariant *grotto_home_model_status(GrottoHomeModel *self);
GVariant *grotto_home_model_clients(GrottoHomeModel *self);
GVariant *grotto_home_model_requests(GrottoHomeModel *self);
GVariant *grotto_home_model_activity(GrottoHomeModel *self);
GDBusProxy *grotto_home_model_proxy(GrottoHomeModel *self);
gboolean grotto_home_model_writable(GrottoHomeModel *self);
const gchar *grotto_home_model_error(GrottoHomeModel *self);
G_END_DECLS
