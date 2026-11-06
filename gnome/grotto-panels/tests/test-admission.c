/* SPDX-License-Identifier: MIT */
#include "fake-service.h"

typedef struct { GObject parent_instance; } LegacyService;
typedef struct { GObjectClass parent_class; } LegacyServiceClass;
static void legacy_iface_init(GrottoPanelsServiceInterface *iface);
G_DEFINE_TYPE_WITH_CODE(LegacyService, legacy_service, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GROTTO_TYPE_PANELS_SERVICE, legacy_iface_init))
static gboolean legacy_available(GrottoPanelsService *service) { (void)service; return TRUE; }
static gboolean legacy_can(GrottoPanelsService *service, GrottoPanelCommand command) {
  (void)service;
  g_assert_cmpint(command, <, GROTTO_PANEL_DISMISS_BACKUP_BANNER);
  return TRUE;
}
static void legacy_iface_init(GrottoPanelsServiceInterface *iface) {
  iface->is_available = legacy_available; iface->can = legacy_can;
}
static void legacy_service_init(LegacyService *self) { (void)self; }
static void legacy_service_class_init(LegacyServiceClass *klass) { (void)klass; }
static void legacy_commands(void) {
  GrottoPanelsService *service = g_object_new(legacy_service_get_type(), NULL);
  g_assert_true(grotto_panels_service_can(service, GROTTO_PANEL_SET_CLIENT_LABEL));
  g_assert_false(grotto_panels_service_can(service, GROTTO_PANEL_DISMISS_BACKUP_BANNER));
  g_object_unref(service);
}
typedef struct { gboolean done, ok; GError *error; } Result;
static void finished(GObject *source, GAsyncResult *result, gpointer data) {
  Result *r = data;
  r->ok = grotto_panels_service_invoke_finish(GROTTO_PANELS_SERVICE(source), result, &r->error);
  r->done = TRUE;
}
static void rejected(GrottoPanelsFakeService *fake, GrottoPanelCommand command, const GrottoPanelCommandArgs *args) {
  Result r = {0};
  guint before = grotto_panels_fake_service_get_invocations(fake);
  grotto_panels_service_invoke_async(GROTTO_PANELS_SERVICE(fake), command, args, NULL, finished, &r);
  while (!r.done) g_main_context_iteration(NULL, TRUE);
  g_assert_false(r.ok); g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpstr(r.error->message, ==, "Invalid public panel data");
  g_clear_error(&r.error);
  g_assert_cmpuint(grotto_panels_fake_service_get_invocations(fake), ==, before);
}
static GrottoPanelCommandArgs valid(GrottoPanelCommand command) {
  static const GrottoPanelPermission permissions[] = {{"sign-event", "allow", 1, 0, "wss://nos.lol"}};
  static const char *const relays[] = {"wss://nos.lol", NULL};
  static const char *const methods[] = {"get_public_key", "sign_event", NULL};
  GrottoPanelCommandArgs args = {0};
  switch (command) {
    case GROTTO_PANEL_SET_CLIENT_LABEL: args.principal = "app"; args.label = "Cosmetic name"; break;
    case GROTTO_PANEL_SET_CLIENT_TRUST: args.principal = "app"; args.trust = "ask"; break;
    case GROTTO_PANEL_REVOKE_CLIENT: args.principal = "app"; break;
    case GROTTO_PANEL_SET_DEFAULT_IDENTITY: args.public_key = "npub-public"; break;
    case GROTTO_PANEL_DISMISS_BACKUP_BANNER: args.public_key = "npub-public"; break;
    case GROTTO_PANEL_SET_IDENTITY_LABEL: args.public_key = "npub-public"; args.label = "Local name"; break;
    case GROTTO_PANEL_SET_PERMISSIONS:
      args.principal = "app"; args.public_key = "npub-public"; args.permissions = permissions;
      args.n_permissions = 1; args.expected_revision = 4; break;
    case GROTTO_PANEL_CONFIGURE_REMOTE: args.remote_enabled = TRUE; args.relays = relays; args.methods = methods; break;
    default: break;
  }
  return args;
}
static const char *const credentials[] = {
  "nsec1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq",
  "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about",
  "token=private-pairing-value",
  "ABANDON ABANDON ABANDON ABANDON ABANDON ABANDON ABANDON ABANDON ABANDON ABANDON ABANDON ABOUT"
};
static void command_admission(void) {
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  grotto_panels_fake_service_set_available(fake, TRUE);
  for (guint command = 0; command < GROTTO_PANEL_N_COMMANDS; command++) {
    grotto_panels_fake_service_set_can(fake, command, TRUE);
    GrottoPanelCommandArgs baseline = valid(command);
    g_assert_true(grotto_panel_command_args_validate(command, &baseline, NULL));
    for (guint payload = 0; payload < G_N_ELEMENTS(credentials); payload++) {
      /* Every string slot, including unused slots, is checked before dispatch. */
      for (guint field = 0; field < 4; field++) {
        GrottoPanelCommandArgs args = baseline;
        const char **slots[] = {&args.principal, &args.label, &args.public_key, &args.trust};
        *slots[field] = credentials[payload]; rejected(fake, command, &args);
      }
      const char *vector[] = {credentials[payload], NULL};
      GrottoPanelCommandArgs args = baseline; args.relays = vector; rejected(fake, command, &args);
      args = baseline; args.methods = vector; rejected(fake, command, &args);
      for (guint field = 0; field < 3; field++) {
        GrottoPanelPermission permission = {"sign-event", "allow", 1, 0, "wss://nos.lol"};
        const char **slots[] = {&permission.operation, &permission.decision, &permission.relay};
        *slots[field] = credentials[payload]; args = baseline; args.permissions = &permission; args.n_permissions = 1;
        rejected(fake, command, &args);
      }
    }
    Result accepted = {0};
    grotto_panels_service_invoke_async(GROTTO_PANELS_SERVICE(fake), command, &baseline, NULL, finished, &accepted);
    while (!accepted.done) g_main_context_iteration(NULL, TRUE);
    g_assert_true(accepted.ok); g_assert_no_error(accepted.error);
    g_assert_cmpint(grotto_panels_fake_service_get_last_command(fake), ==, command);
  }
  /* Manager labels never accept tier/permission payloads. Trust stays approver-only. */
  GrottoPanelCommandArgs label = valid(GROTTO_PANEL_SET_CLIENT_LABEL); label.trust = "routine";
  rejected(fake, GROTTO_PANEL_SET_CLIENT_LABEL, &label);
  GrottoPanelCommandArgs dismiss = valid(GROTTO_PANEL_DISMISS_BACKUP_BANNER); dismiss.label = "not an acknowledgement";
  rejected(fake, GROTTO_PANEL_DISMISS_BACKUP_BANNER, &dismiss);
  g_assert_cmpint(grotto_panel_command_get_access(GROTTO_PANEL_DISMISS_BACKUP_BANNER), ==, GROTTO_PANELS_ACCESS_MANAGER);
  g_assert_cmpint(grotto_panel_command_get_access(GROTTO_PANEL_SET_CLIENT_LABEL), ==, GROTTO_PANELS_ACCESS_MANAGER);
  g_assert_cmpint(grotto_panel_command_get_access(GROTTO_PANEL_SET_CLIENT_TRUST), ==, GROTTO_PANELS_ACCESS_APPROVER);
  g_assert_cmpint(grotto_panel_command_get_access(GROTTO_PANEL_SET_PERMISSIONS), ==, GROTTO_PANELS_ACCESS_APPROVER);
}
static void metadata_admission(void) {
  g_autoptr(GrottoPanelsFakeService) fake = grotto_panels_fake_service_new();
  for (guint model = 0; model < GROTTO_PANELS_N_MODELS; model++) {
    for (guint payload = 0; payload < G_N_ELEMENTS(credentials); payload++) {
      for (guint field = 0; field < 17; field++) {
        GrottoPanelsMetadata m = {.id = "public-id", .label = "Public name"};
        const char **slots[] = {&m.id, &m.label, &m.public_key, &m.state, &m.reason, &m.access_policy,
          &m.principal, &m.verified_as, &m.transport, &m.operation, &m.outcome, &m.trust, &m.decision, &m.relay};
        const char *vector[] = {credentials[payload], NULL};
        if (field < G_N_ELEMENTS(slots)) *slots[field] = credentials[payload];
        else if (field == G_N_ELEMENTS(slots)) m.relays = vector;
        else if (field == G_N_ELEMENTS(slots) + 1) m.methods = vector;
        else m.effective_permissions = vector;
        g_autoptr(GError) error = NULL;
        g_assert_false(grotto_panels_fake_service_replace(fake, model, &m, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_assert_cmpuint(g_list_model_get_n_items(grotto_panels_service_get_model(GROTTO_PANELS_SERVICE(fake), model)), ==, 0);
      }
    }
  }
}
/* A generic GObject model can hide any payload. The public getter rejects its
 * type structurally, irrespective of property/qdata names or credential form. */
typedef struct { GObject parent; GListStore *store; } OpaqueService;
typedef struct { GObjectClass parent; } OpaqueServiceClass;
static void opaque_iface(GrottoPanelsServiceInterface *iface);
G_DEFINE_TYPE_WITH_CODE(OpaqueService, opaque_service, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GROTTO_TYPE_PANELS_SERVICE, opaque_iface))
static GListModel *opaque_model(GrottoPanelsService *self, GrottoPanelsModel model) {
  (void)model; return G_LIST_MODEL(((OpaqueService *)self)->store);
}
static void opaque_iface(GrottoPanelsServiceInterface *iface) { iface->get_model = opaque_model; }
static void opaque_dispose(GObject *object) {
  g_clear_object(&((OpaqueService *)object)->store);
  G_OBJECT_CLASS(opaque_service_parent_class)->dispose(object);
}
static void opaque_service_class_init(OpaqueServiceClass *klass) { G_OBJECT_CLASS(klass)->dispose = opaque_dispose; }
static void opaque_service_init(OpaqueService *self) { self->store = g_list_store_new(G_TYPE_OBJECT); }
static void opaque_rejected(void) {
  g_assert_true(G_TYPE_IS_FINAL(GROTTO_TYPE_PANELS_ITEM));
  OpaqueService *self = g_object_new(opaque_service_get_type(), NULL);
  const char *keys[] = {"nsec", "mnemonic", "token"};
  for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
    g_autoptr(GObject) object = g_object_new(G_TYPE_OBJECT, NULL);
    g_object_set_data_full(object, keys[i], g_strdup(credentials[i]), g_free);
    g_list_store_append(self->store, object);
    for (guint model = 0; model < GROTTO_PANELS_N_MODELS; model++)
      g_assert_null(grotto_panels_service_get_model(GROTTO_PANELS_SERVICE(self), model));
    g_list_store_remove_all(self->store);
  }
  g_object_unref(self);
}
static void profile_admission(void) {
  for (guint field = 0; field < GROTTO_PROFILE_N_FIELDS; field++) {
    for (guint i = 0; i < G_N_ELEMENTS(credentials); i++) {
      GrottoProfileInfo info = {0}; info.fields[field] = (char *)credentials[i];
      g_autoptr(GError) error = NULL; g_assert_false(grotto_profile_info_validate(&info, &error));
      g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    }
  }
  GrottoProfileInfo info = {0}; info.fields[GROTTO_PROFILE_ABOUT] = "First line\nSecond line";
  g_assert_true(grotto_profile_info_validate(&info, NULL));
  g_autofree char *large = g_strnfill(4097, 'x'); info.fields[GROTTO_PROFILE_ABOUT] = large;
  g_assert_false(grotto_profile_info_validate(&info, NULL));
  info.fields[GROTTO_PROFILE_ABOUT] = NULL;
  g_autoptr(GObject) opaque = g_object_new(G_TYPE_OBJECT, NULL);
  info.images[0] = (GdkTexture *)opaque;
  g_assert_false(grotto_profile_info_validate(&info, NULL));
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/admission/legacy-commands", legacy_commands);
  g_test_add_func("/admission/commands", command_admission);
  g_test_add_func("/admission/profile", profile_admission);
  g_test_add_func("/admission/metadata", metadata_admission);
  g_test_add_func("/admission/opaque-model", opaque_rejected);
  return g_test_run();
}
