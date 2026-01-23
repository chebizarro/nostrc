/* hsm_provider_pkcs11.c - PKCS#11 HSM provider implementation
 *
 * PKCS#11 provider using p11-kit for token discovery and management.
 *
 * Build requirements:
 *   - p11-kit development headers
 *   - libsecp256k1 for software signing fallback
 *
 * SPDX-License-Identifier: MIT
 */
#include "hsm_provider_pkcs11.h"

#ifdef GNOSTR_HAVE_PKCS11
#include <p11-kit/p11-kit.h>
#include <p11-kit/uri.h>
#endif

#include <string.h>
#include <time.h>

/* Include libnostr for crypto operations */
#include <keys.h>
#include <nostr/nip19/nip19.h>
#include <nostr-utils.h>

/* Module info structure */
typedef struct {
  gchar *path;
#ifdef GNOSTR_HAVE_PKCS11
  CK_FUNCTION_LIST *functions;
#else
  gpointer functions;
#endif
  gchar *description;
  gchar *manufacturer;
  gchar *version;
  gboolean is_p11kit;
} Pkcs11Module;

/* Session state per slot */
typedef struct {
  guint64 slot_id;
#ifdef GNOSTR_HAVE_PKCS11
  CK_SESSION_HANDLE session;
#else
  gulong session;
#endif
  gboolean is_logged_in;
  gchar *token_label;
} SlotSession;

struct _GnHsmProviderPkcs11 {
  GObject parent_instance;

  gboolean initialized;
  gboolean software_signing_enabled;

  GList *modules;           /* List of Pkcs11Module */
  GHashTable *sessions;     /* slot_id -> SlotSession */

  GnHsmPinCallback pin_callback;
  gpointer pin_callback_data;
  GDestroyNotify pin_callback_destroy;

  GMutex lock;
};

static void gn_hsm_provider_pkcs11_iface_init(GnHsmProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnHsmProviderPkcs11, gn_hsm_provider_pkcs11, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GN_TYPE_HSM_PROVIDER,
                                              gn_hsm_provider_pkcs11_iface_init))

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static void
pkcs11_module_free(Pkcs11Module *mod)
{
  if (!mod)
    return;
  g_free(mod->path);
  g_free(mod->description);
  g_free(mod->manufacturer);
  g_free(mod->version);
#ifdef GNOSTR_HAVE_PKCS11
  if (mod->functions && !mod->is_p11kit) {
    mod->functions->C_Finalize(NULL);
  }
#endif
  g_free(mod);
}

static void
slot_session_free(SlotSession *sess)
{
  if (!sess)
    return;
#ifdef GNOSTR_HAVE_PKCS11
  /* Session is closed when finalizing */
#endif
  g_free(sess->token_label);
  g_free(sess);
}

static gchar *
bytes_to_hex(const guint8 *bytes, gsize len)
{
  static const gchar hex_chars[] = "0123456789abcdef";
  gchar *result = g_malloc(len * 2 + 1);
  for (gsize i = 0; i < len; i++) {
    result[i * 2] = hex_chars[(bytes[i] >> 4) & 0xF];
    result[i * 2 + 1] = hex_chars[bytes[i] & 0xF];
  }
  result[len * 2] = '\0';
  return result;
}

static gchar *
generate_key_id(void)
{
  guint8 random_bytes[8];
  for (int i = 0; i < 8; i++) {
    random_bytes[i] = g_random_int_range(0, 256);
  }
  return g_base64_encode(random_bytes, 8);
}

#ifdef GNOSTR_HAVE_PKCS11

/* Convert PKCS#11 return code to string */
static const gchar *
rv_to_string(CK_RV rv)
{
  switch (rv) {
    case CKR_OK: return "CKR_OK";
    case CKR_CANCEL: return "CKR_CANCEL";
    case CKR_HOST_MEMORY: return "CKR_HOST_MEMORY";
    case CKR_SLOT_ID_INVALID: return "CKR_SLOT_ID_INVALID";
    case CKR_GENERAL_ERROR: return "CKR_GENERAL_ERROR";
    case CKR_FUNCTION_FAILED: return "CKR_FUNCTION_FAILED";
    case CKR_ARGUMENTS_BAD: return "CKR_ARGUMENTS_BAD";
    case CKR_PIN_INCORRECT: return "CKR_PIN_INCORRECT";
    case CKR_PIN_LOCKED: return "CKR_PIN_LOCKED";
    case CKR_TOKEN_NOT_PRESENT: return "CKR_TOKEN_NOT_PRESENT";
    case CKR_DEVICE_ERROR: return "CKR_DEVICE_ERROR";
    case CKR_DEVICE_REMOVED: return "CKR_DEVICE_REMOVED";
    case CKR_USER_NOT_LOGGED_IN: return "CKR_USER_NOT_LOGGED_IN";
    default: return "CKR_UNKNOWN";
  }
}

/* Map PKCS#11 error to GnHsmError */
static GnHsmError
map_pkcs11_error(CK_RV rv)
{
  switch (rv) {
    case CKR_OK: return GN_HSM_ERROR_FAILED; /* Should not happen */
    case CKR_PIN_INCORRECT: return GN_HSM_ERROR_PIN_INCORRECT;
    case CKR_PIN_LOCKED: return GN_HSM_ERROR_PIN_LOCKED;
    case CKR_TOKEN_NOT_PRESENT: return GN_HSM_ERROR_NOT_FOUND;
    case CKR_DEVICE_ERROR: return GN_HSM_ERROR_DEVICE_ERROR;
    case CKR_DEVICE_REMOVED: return GN_HSM_ERROR_DEVICE_REMOVED;
    case CKR_USER_NOT_LOGGED_IN: return GN_HSM_ERROR_PIN_REQUIRED;
    case CKR_SLOT_ID_INVALID: return GN_HSM_ERROR_NOT_FOUND;
    default: return GN_HSM_ERROR_FAILED;
  }
}

/* Trim trailing spaces from PKCS#11 fixed-size strings */
static gchar *
trim_pkcs11_string(const CK_UTF8CHAR *str, gsize len)
{
  while (len > 0 && str[len - 1] == ' ')
    len--;
  return g_strndup((const gchar *)str, len);
}

#endif /* GNOSTR_HAVE_PKCS11 */

/* ============================================================================
 * Provider Interface Implementation
 * ============================================================================ */

static const gchar *
pkcs11_get_name(GnHsmProvider *provider)
{
  (void)provider;
  return "PKCS#11";
}

static gboolean
pkcs11_is_available(GnHsmProvider *provider)
{
  (void)provider;
#ifdef GNOSTR_HAVE_PKCS11
  return TRUE;
#else
  return FALSE;
#endif
}

static gboolean
pkcs11_init_provider(GnHsmProvider *provider, GError **error)
{
  GnHsmProviderPkcs11 *self = GN_HSM_PROVIDER_PKCS11(provider);

#ifndef GNOSTR_HAVE_PKCS11
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "PKCS#11 support not compiled in");
  return FALSE;
#else
  g_mutex_lock(&self->lock);

  if (self->initialized) {
    g_mutex_unlock(&self->lock);
    return TRUE;
  }

  /* Load all PKCS#11 modules via p11-kit */
  CK_FUNCTION_LIST **modules = p11_kit_modules_load_and_initialize(0);
  if (!modules) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
                "Failed to load PKCS#11 modules via p11-kit");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  for (int i = 0; modules[i] != NULL; i++) {
    CK_INFO info;
    CK_RV rv = modules[i]->C_GetInfo(&info);
    if (rv != CKR_OK) {
      g_warning("PKCS#11: Failed to get module info: %s", rv_to_string(rv));
      continue;
    }

    Pkcs11Module *mod = g_new0(Pkcs11Module, 1);
    mod->functions = modules[i];
    mod->is_p11kit = TRUE;
    mod->description = trim_pkcs11_string(info.libraryDescription,
                                          sizeof(info.libraryDescription));
    mod->manufacturer = trim_pkcs11_string(info.manufacturerID,
                                           sizeof(info.manufacturerID));
    mod->version = g_strdup_printf("%d.%d",
                                   info.libraryVersion.major,
                                   info.libraryVersion.minor);

    gchar *name = p11_kit_module_get_name(modules[i]);
    mod->path = g_strdup(name ? name : "unknown");
    free(name);

    self->modules = g_list_prepend(self->modules, mod);
    g_message("PKCS#11: Loaded module '%s' (%s)", mod->path, mod->description);
  }

  /* Don't free modules array - p11-kit manages lifetime */
  free(modules);

  self->initialized = TRUE;
  g_mutex_unlock(&self->lock);

  g_message("PKCS#11 provider initialized with %u modules",
            g_list_length(self->modules));
  return TRUE;
#endif
}

static void
pkcs11_shutdown_provider(GnHsmProvider *provider)
{
  GnHsmProviderPkcs11 *self = GN_HSM_PROVIDER_PKCS11(provider);

  g_mutex_lock(&self->lock);

  if (!self->initialized) {
    g_mutex_unlock(&self->lock);
    return;
  }

#ifdef GNOSTR_HAVE_PKCS11
  /* Close all sessions */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->sessions);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    SlotSession *sess = (SlotSession *)value;
    /* Find the module for this slot and close session */
    for (GList *l = self->modules; l != NULL; l = l->next) {
      Pkcs11Module *mod = (Pkcs11Module *)l->data;
      if (mod->functions && sess->session) {
        mod->functions->C_CloseSession(sess->session);
        break;
      }
    }
  }
  g_hash_table_remove_all(self->sessions);

  /* Unload modules managed by p11-kit */
  GList *p11kit_modules = NULL;
  for (GList *l = self->modules; l != NULL; l = l->next) {
    Pkcs11Module *mod = (Pkcs11Module *)l->data;
    if (mod->is_p11kit && mod->functions) {
      p11kit_modules = g_list_prepend(p11kit_modules, mod->functions);
    }
  }

  if (p11kit_modules) {
    /* Convert to array for p11_kit_modules_finalize_and_release */
    guint n = g_list_length(p11kit_modules);
    CK_FUNCTION_LIST **arr = g_new0(CK_FUNCTION_LIST *, n + 1);
    guint i = 0;
    for (GList *l = p11kit_modules; l != NULL; l = l->next) {
      arr[i++] = (CK_FUNCTION_LIST *)l->data;
    }
    p11_kit_modules_finalize_and_release(arr);
    g_list_free(p11kit_modules);
  }
#endif

  g_list_free_full(self->modules, (GDestroyNotify)pkcs11_module_free);
  self->modules = NULL;
  self->initialized = FALSE;

  g_mutex_unlock(&self->lock);
  g_message("PKCS#11 provider shut down");
}

static GPtrArray *
pkcs11_detect_devices(GnHsmProvider *provider, GError **error)
{
  GnHsmProviderPkcs11 *self = GN_HSM_PROVIDER_PKCS11(provider);

#ifndef GNOSTR_HAVE_PKCS11
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "PKCS#11 support not compiled in");
  return NULL;
#else
  g_mutex_lock(&self->lock);

  if (!self->initialized) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_INITIALIZED,
                "Provider not initialized");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  GPtrArray *devices = g_ptr_array_new_with_free_func(
    (GDestroyNotify)gn_hsm_device_info_free);

  for (GList *l = self->modules; l != NULL; l = l->next) {
    Pkcs11Module *mod = (Pkcs11Module *)l->data;
    if (!mod->functions)
      continue;

    CK_ULONG slot_count = 0;
    CK_RV rv = mod->functions->C_GetSlotList(CK_TRUE, NULL, &slot_count);
    if (rv != CKR_OK || slot_count == 0)
      continue;

    CK_SLOT_ID *slots = g_new(CK_SLOT_ID, slot_count);
    rv = mod->functions->C_GetSlotList(CK_TRUE, slots, &slot_count);
    if (rv != CKR_OK) {
      g_free(slots);
      continue;
    }

    for (CK_ULONG i = 0; i < slot_count; i++) {
      CK_SLOT_INFO slot_info;
      rv = mod->functions->C_GetSlotInfo(slots[i], &slot_info);
      if (rv != CKR_OK)
        continue;

      CK_TOKEN_INFO token_info;
      rv = mod->functions->C_GetTokenInfo(slots[i], &token_info);
      if (rv != CKR_OK)
        continue;

      GnHsmDeviceInfo *info = g_new0(GnHsmDeviceInfo, 1);
      info->slot_id = slots[i];
      info->label = trim_pkcs11_string(token_info.label,
                                       sizeof(token_info.label));
      info->manufacturer = trim_pkcs11_string(token_info.manufacturerID,
                                              sizeof(token_info.manufacturerID));
      info->model = trim_pkcs11_string(token_info.model,
                                       sizeof(token_info.model));
      info->serial = trim_pkcs11_string(token_info.serialNumber,
                                        sizeof(token_info.serialNumber));
      info->flags = token_info.flags;
      info->is_token_present = (slot_info.flags & CKF_TOKEN_PRESENT) != 0;
      info->is_initialized = (token_info.flags & CKF_TOKEN_INITIALIZED) != 0;
      info->needs_pin = (token_info.flags & CKF_LOGIN_REQUIRED) != 0;

      g_ptr_array_add(devices, info);
    }

    g_free(slots);
  }

  g_mutex_unlock(&self->lock);
  return devices;
#endif
}

static GPtrArray *
pkcs11_list_keys(GnHsmProvider *provider, guint64 slot_id, GError **error)
{
  GnHsmProviderPkcs11 *self = GN_HSM_PROVIDER_PKCS11(provider);

#ifndef GNOSTR_HAVE_PKCS11
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "PKCS#11 support not compiled in");
  return NULL;
#else
  g_mutex_lock(&self->lock);

  if (!self->initialized) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_INITIALIZED,
                "Provider not initialized");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  GPtrArray *keys = g_ptr_array_new_with_free_func(
    (GDestroyNotify)gn_hsm_key_info_free);

  /* Find module and get session for this slot */
  for (GList *l = self->modules; l != NULL; l = l->next) {
    Pkcs11Module *mod = (Pkcs11Module *)l->data;
    if (!mod->functions)
      continue;

    /* Try to open a session */
    CK_SESSION_HANDLE session;
    CK_RV rv = mod->functions->C_OpenSession(slot_id,
                                             CKF_SERIAL_SESSION | CKF_RW_SESSION,
                                             NULL, NULL, &session);
    if (rv != CKR_OK)
      continue;

    /* Search for private keys */
    CK_OBJECT_CLASS key_class = CKO_PRIVATE_KEY;
    CK_ATTRIBUTE template[] = {
      {CKA_CLASS, &key_class, sizeof(key_class)}
    };

    rv = mod->functions->C_FindObjectsInit(session, template, 1);
    if (rv != CKR_OK) {
      mod->functions->C_CloseSession(session);
      continue;
    }

    CK_OBJECT_HANDLE objects[64];
    CK_ULONG object_count;

    while (mod->functions->C_FindObjects(session, objects, 64, &object_count) == CKR_OK
           && object_count > 0) {
      for (CK_ULONG i = 0; i < object_count; i++) {
        CK_BYTE id[256];
        CK_BYTE label[256];
        CK_KEY_TYPE key_type;
        CK_ATTRIBUTE attrs[] = {
          {CKA_ID, id, sizeof(id)},
          {CKA_LABEL, label, sizeof(label)},
          {CKA_KEY_TYPE, &key_type, sizeof(key_type)}
        };

        rv = mod->functions->C_GetAttributeValue(session, objects[i], attrs, 3);
        if (rv != CKR_OK)
          continue;

        /* Only interested in EC keys for now */
        if (key_type != CKK_EC)
          continue;

        GnHsmKeyInfo *info = g_new0(GnHsmKeyInfo, 1);
        info->key_id = g_base64_encode(id, attrs[0].ulValueLen);
        info->label = g_strndup((gchar *)label, attrs[1].ulValueLen);
        info->key_type = GN_HSM_KEY_TYPE_SECP256K1; /* Assume for now */
        info->slot_id = slot_id;
        info->can_sign = TRUE;
        info->is_extractable = FALSE;

        /* Try to get public key */
        /* This is simplified - real implementation would extract EC point */
        info->npub = NULL;
        info->pubkey_hex = NULL;

        g_ptr_array_add(keys, info);
      }
    }

    mod->functions->C_FindObjectsFinal(session);
    mod->functions->C_CloseSession(session);
    break; /* Found the right module */
  }

  g_mutex_unlock(&self->lock);
  return keys;
#endif
}

static GnHsmKeyInfo *
pkcs11_get_public_key(GnHsmProvider *provider,
                      guint64 slot_id,
                      const gchar *key_id,
                      GError **error)
{
  GnHsmProviderPkcs11 *self = GN_HSM_PROVIDER_PKCS11(provider);
  (void)slot_id;
  (void)key_id;

#ifndef GNOSTR_HAVE_PKCS11
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "PKCS#11 support not compiled in");
  return NULL;
#else
  g_mutex_lock(&self->lock);

  if (!self->initialized) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_INITIALIZED,
                "Provider not initialized");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* TODO: Implement actual key lookup */
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
              "Key lookup not yet implemented");
  g_mutex_unlock(&self->lock);
  return NULL;
#endif
}

static gboolean
pkcs11_sign_hash(GnHsmProvider *provider,
                 guint64 slot_id,
                 const gchar *key_id,
                 const guint8 *hash,
                 gsize hash_len,
                 guint8 *signature,
                 gsize *signature_len,
                 GError **error)
{
  GnHsmProviderPkcs11 *self = GN_HSM_PROVIDER_PKCS11(provider);
  (void)slot_id;
  (void)key_id;
  (void)hash;
  (void)hash_len;
  (void)signature;
  (void)signature_len;

#ifndef GNOSTR_HAVE_PKCS11
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "PKCS#11 support not compiled in");
  return FALSE;
#else
  g_mutex_lock(&self->lock);

  if (!self->initialized) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_INITIALIZED,
                "Provider not initialized");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* TODO: Implement actual signing */
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
              "Signing not yet implemented");
  g_mutex_unlock(&self->lock);
  return FALSE;
#endif
}

static gchar *
pkcs11_sign_event(GnHsmProvider *provider,
                  guint64 slot_id,
                  const gchar *key_id,
                  const gchar *event_json,
                  GError **error)
{
  GnHsmProviderPkcs11 *self = GN_HSM_PROVIDER_PKCS11(provider);
  (void)slot_id;
  (void)key_id;
  (void)event_json;

#ifndef GNOSTR_HAVE_PKCS11
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "PKCS#11 support not compiled in");
  return NULL;
#else
  g_mutex_lock(&self->lock);

  if (!self->initialized) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_INITIALIZED,
                "Provider not initialized");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* TODO: Implement actual event signing */
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
              "Event signing not yet implemented");
  g_mutex_unlock(&self->lock);
  return NULL;
#endif
}

static GnHsmKeyInfo *
pkcs11_generate_key(GnHsmProvider *provider,
                    guint64 slot_id,
                    const gchar *label,
                    GnHsmKeyType key_type,
                    GError **error)
{
  GnHsmProviderPkcs11 *self = GN_HSM_PROVIDER_PKCS11(provider);
  (void)slot_id;
  (void)label;
  (void)key_type;

#ifndef GNOSTR_HAVE_PKCS11
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "PKCS#11 support not compiled in");
  return NULL;
#else
  g_mutex_lock(&self->lock);

  if (!self->initialized) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_INITIALIZED,
                "Provider not initialized");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* TODO: Implement actual key generation */
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_KEY_GENERATION_FAILED,
              "Key generation not yet implemented");
  g_mutex_unlock(&self->lock);
  return NULL;
#endif
}

static gboolean
pkcs11_import_key(GnHsmProvider *provider,
                  guint64 slot_id,
                  const gchar *label,
                  const guint8 *private_key,
                  gsize key_len,
                  GnHsmKeyInfo **out_info,
                  GError **error)
{
  GnHsmProviderPkcs11 *self = GN_HSM_PROVIDER_PKCS11(provider);
  (void)slot_id;
  (void)label;
  (void)private_key;
  (void)key_len;
  (void)out_info;

#ifndef GNOSTR_HAVE_PKCS11
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "PKCS#11 support not compiled in");
  return FALSE;
#else
  g_mutex_lock(&self->lock);

  if (!self->initialized) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_INITIALIZED,
                "Provider not initialized");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* TODO: Implement actual key import */
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
              "Key import not yet implemented");
  g_mutex_unlock(&self->lock);
  return FALSE;
#endif
}

static gboolean
pkcs11_delete_key(GnHsmProvider *provider,
                  guint64 slot_id,
                  const gchar *key_id,
                  GError **error)
{
  GnHsmProviderPkcs11 *self = GN_HSM_PROVIDER_PKCS11(provider);
  (void)slot_id;
  (void)key_id;

#ifndef GNOSTR_HAVE_PKCS11
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "PKCS#11 support not compiled in");
  return FALSE;
#else
  g_mutex_lock(&self->lock);

  if (!self->initialized) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_INITIALIZED,
                "Provider not initialized");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* TODO: Implement actual key deletion */
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
              "Key deletion not yet implemented");
  g_mutex_unlock(&self->lock);
  return FALSE;
#endif
}

static gboolean
pkcs11_login(GnHsmProvider *provider, guint64 slot_id, const gchar *pin, GError **error)
{
  GnHsmProviderPkcs11 *self = GN_HSM_PROVIDER_PKCS11(provider);

#ifndef GNOSTR_HAVE_PKCS11
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "PKCS#11 support not compiled in");
  return FALSE;
#else
  g_mutex_lock(&self->lock);

  if (!self->initialized) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_INITIALIZED,
                "Provider not initialized");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Find the right module for this slot */
  for (GList *l = self->modules; l != NULL; l = l->next) {
    Pkcs11Module *mod = (Pkcs11Module *)l->data;
    if (!mod->functions)
      continue;

    /* Try to open a session */
    CK_SESSION_HANDLE session;
    CK_RV rv = mod->functions->C_OpenSession(slot_id,
                                             CKF_SERIAL_SESSION | CKF_RW_SESSION,
                                             NULL, NULL, &session);
    if (rv != CKR_OK)
      continue;

    /* Try to login */
    rv = mod->functions->C_Login(session, CKU_USER,
                                 (CK_UTF8CHAR_PTR)(pin ? pin : ""),
                                 pin ? strlen(pin) : 0);
    if (rv != CKR_OK && rv != CKR_USER_ALREADY_LOGGED_IN) {
      mod->functions->C_CloseSession(session);
      g_set_error(error, GN_HSM_ERROR, map_pkcs11_error(rv),
                  "Login failed: %s", rv_to_string(rv));
      g_mutex_unlock(&self->lock);
      return FALSE;
    }

    /* Store session */
    SlotSession *sess = g_new0(SlotSession, 1);
    sess->slot_id = slot_id;
    sess->session = session;
    sess->is_logged_in = TRUE;

    g_hash_table_insert(self->sessions, GUINT_TO_POINTER((guint)slot_id), sess);

    g_mutex_unlock(&self->lock);
    return TRUE;
  }

  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
              "No module found for slot %" G_GUINT64_FORMAT, slot_id);
  g_mutex_unlock(&self->lock);
  return FALSE;
#endif
}

static void
pkcs11_logout(GnHsmProvider *provider, guint64 slot_id)
{
  GnHsmProviderPkcs11 *self = GN_HSM_PROVIDER_PKCS11(provider);

#ifdef GNOSTR_HAVE_PKCS11
  g_mutex_lock(&self->lock);

  SlotSession *sess = g_hash_table_lookup(self->sessions,
                                          GUINT_TO_POINTER((guint)slot_id));
  if (sess && sess->session) {
    for (GList *l = self->modules; l != NULL; l = l->next) {
      Pkcs11Module *mod = (Pkcs11Module *)l->data;
      if (mod->functions) {
        mod->functions->C_Logout(sess->session);
        mod->functions->C_CloseSession(sess->session);
        break;
      }
    }
    g_hash_table_remove(self->sessions, GUINT_TO_POINTER((guint)slot_id));
  }

  g_mutex_unlock(&self->lock);
#else
  (void)self;
  (void)slot_id;
#endif
}

/* ============================================================================
 * Interface Setup
 * ============================================================================ */

static void
gn_hsm_provider_pkcs11_iface_init(GnHsmProviderInterface *iface)
{
  iface->get_name = pkcs11_get_name;
  iface->is_available = pkcs11_is_available;
  iface->init_provider = pkcs11_init_provider;
  iface->shutdown_provider = pkcs11_shutdown_provider;
  iface->detect_devices = pkcs11_detect_devices;
  iface->list_keys = pkcs11_list_keys;
  iface->get_public_key = pkcs11_get_public_key;
  iface->sign_hash = pkcs11_sign_hash;
  iface->sign_event = pkcs11_sign_event;
  iface->generate_key = pkcs11_generate_key;
  iface->import_key = pkcs11_import_key;
  iface->delete_key = pkcs11_delete_key;
  iface->login = pkcs11_login;
  iface->logout = pkcs11_logout;
}

/* ============================================================================
 * GObject Implementation
 * ============================================================================ */

static void
gn_hsm_provider_pkcs11_finalize(GObject *object)
{
  GnHsmProviderPkcs11 *self = GN_HSM_PROVIDER_PKCS11(object);

  if (self->initialized) {
    pkcs11_shutdown_provider(GN_HSM_PROVIDER(self));
  }

  if (self->pin_callback_destroy && self->pin_callback_data) {
    self->pin_callback_destroy(self->pin_callback_data);
  }

  g_hash_table_unref(self->sessions);
  g_mutex_clear(&self->lock);

  G_OBJECT_CLASS(gn_hsm_provider_pkcs11_parent_class)->finalize(object);
}

static void
gn_hsm_provider_pkcs11_class_init(GnHsmProviderPkcs11Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gn_hsm_provider_pkcs11_finalize;
}

static void
gn_hsm_provider_pkcs11_init(GnHsmProviderPkcs11 *self)
{
  g_mutex_init(&self->lock);
  self->initialized = FALSE;
  self->software_signing_enabled = TRUE;
  self->modules = NULL;
  self->sessions = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                         NULL, (GDestroyNotify)slot_session_free);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GnHsmProviderPkcs11 *
gn_hsm_provider_pkcs11_new(void)
{
#ifdef GNOSTR_HAVE_PKCS11
  return g_object_new(GN_TYPE_HSM_PROVIDER_PKCS11, NULL);
#else
  g_warning("PKCS#11 support not compiled in");
  return NULL;
#endif
}

gboolean
gn_hsm_provider_pkcs11_is_supported(void)
{
#ifdef GNOSTR_HAVE_PKCS11
  return TRUE;
#else
  return FALSE;
#endif
}

gboolean
gn_hsm_provider_pkcs11_add_module(GnHsmProviderPkcs11 *self,
                                  const gchar *module_path,
                                  GError **error)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER_PKCS11(self), FALSE);
  g_return_val_if_fail(module_path != NULL, FALSE);

#ifndef GNOSTR_HAVE_PKCS11
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "PKCS#11 support not compiled in");
  return FALSE;
#else
  /* TODO: Implement manual module loading */
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
              "Manual module loading not yet implemented");
  return FALSE;
#endif
}

void
gn_hsm_provider_pkcs11_remove_module(GnHsmProviderPkcs11 *self,
                                     const gchar *module_path)
{
  g_return_if_fail(GN_IS_HSM_PROVIDER_PKCS11(self));
  g_return_if_fail(module_path != NULL);

  /* TODO: Implement module removal */
}

gboolean
gn_hsm_provider_pkcs11_get_module_info(GnHsmProviderPkcs11 *self,
                                       const gchar *module_path,
                                       gchar **out_description,
                                       gchar **out_manufacturer,
                                       gchar **out_version)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER_PKCS11(self), FALSE);
  g_return_val_if_fail(module_path != NULL, FALSE);

  g_mutex_lock(&self->lock);

  for (GList *l = self->modules; l != NULL; l = l->next) {
    Pkcs11Module *mod = (Pkcs11Module *)l->data;
    if (g_strcmp0(mod->path, module_path) == 0) {
      if (out_description)
        *out_description = g_strdup(mod->description);
      if (out_manufacturer)
        *out_manufacturer = g_strdup(mod->manufacturer);
      if (out_version)
        *out_version = g_strdup(mod->version);
      g_mutex_unlock(&self->lock);
      return TRUE;
    }
  }

  g_mutex_unlock(&self->lock);
  return FALSE;
}

void
gn_hsm_provider_pkcs11_set_pin_callback(GnHsmProviderPkcs11 *self,
                                        GnHsmPinCallback callback,
                                        gpointer user_data,
                                        GDestroyNotify destroy)
{
  g_return_if_fail(GN_IS_HSM_PROVIDER_PKCS11(self));

  g_mutex_lock(&self->lock);

  if (self->pin_callback_destroy && self->pin_callback_data) {
    self->pin_callback_destroy(self->pin_callback_data);
  }

  self->pin_callback = callback;
  self->pin_callback_data = user_data;
  self->pin_callback_destroy = destroy;

  g_mutex_unlock(&self->lock);
}

gboolean
gn_hsm_provider_pkcs11_has_secp256k1_support(GnHsmProviderPkcs11 *self,
                                              guint64 slot_id)
{
  g_return_val_if_fail(GN_IS_HSM_PROVIDER_PKCS11(self), FALSE);
  (void)slot_id;

  /* Most PKCS#11 tokens don't support secp256k1 natively */
  /* TODO: Probe the token for EC mechanism support */
  return FALSE;
}

void
gn_hsm_provider_pkcs11_enable_software_signing(GnHsmProviderPkcs11 *self,
                                               gboolean enable)
{
  g_return_if_fail(GN_IS_HSM_PROVIDER_PKCS11(self));

  g_mutex_lock(&self->lock);
  self->software_signing_enabled = enable;
  g_mutex_unlock(&self->lock);
}
