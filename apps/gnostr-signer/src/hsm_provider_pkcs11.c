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
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostr-gobject-1.0/nostr_keys.h>
#include <keys.h>       /* nostr_key_generate_private() - no GObject equivalent */
#include <nostr-event.h>

#ifdef GNOSTR_HAVE_PKCS11
/* OID for secp256k1 curve (1.3.132.0.10) */
static const CK_BYTE SECP256K1_OID[] = {0x06, 0x05, 0x2B, 0x81, 0x04, 0x00, 0x0A};
/* OID for prime256v1/secp256r1 curve (1.2.840.10045.3.1.7) for comparison */
static const CK_BYTE SECP256R1_OID[] = {0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
#endif

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

/* Helper to convert hex to bytes */
static gboolean
hex_to_bytes(const gchar *hex, guint8 *out, gsize expected_len)
{
  gsize hex_len = strlen(hex);
  if (hex_len != expected_len * 2)
    return FALSE;

  for (gsize i = 0; i < expected_len; i++) {
    gchar high = hex[i * 2];
    gchar low = hex[i * 2 + 1];
    guint8 val = 0;

    if (high >= '0' && high <= '9')
      val = (high - '0') << 4;
    else if (high >= 'a' && high <= 'f')
      val = (high - 'a' + 10) << 4;
    else if (high >= 'A' && high <= 'F')
      val = (high - 'A' + 10) << 4;
    else
      return FALSE;

    if (low >= '0' && low <= '9')
      val |= low - '0';
    else if (low >= 'a' && low <= 'f')
      val |= low - 'a' + 10;
    else if (low >= 'A' && low <= 'F')
      val |= low - 'A' + 10;
    else
      return FALSE;

    out[i] = val;
  }
  return TRUE;
}

/* Find a module that has the specified slot */
static Pkcs11Module *
find_module_for_slot(GnHsmProviderPkcs11 *self, guint64 slot_id)
{
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
      if (slots[i] == (CK_SLOT_ID)slot_id) {
        g_free(slots);
        return mod;
      }
    }
    g_free(slots);
  }
  return NULL;
}

/* Find a key object by key_id (base64 encoded CKA_ID) */
static CK_OBJECT_HANDLE
find_key_object(CK_FUNCTION_LIST *funcs, CK_SESSION_HANDLE session,
                const gchar *key_id, CK_OBJECT_CLASS key_class)
{
  gsize id_len = 0;
  guchar *id_bytes = g_base64_decode(key_id, &id_len);
  if (!id_bytes)
    return CK_INVALID_HANDLE;

  CK_ATTRIBUTE template[] = {
    {CKA_CLASS, &key_class, sizeof(key_class)},
    {CKA_ID, id_bytes, id_len}
  };

  CK_RV rv = funcs->C_FindObjectsInit(session, template, 2);
  g_free(id_bytes);

  if (rv != CKR_OK)
    return CK_INVALID_HANDLE;

  CK_OBJECT_HANDLE obj = CK_INVALID_HANDLE;
  CK_ULONG count = 0;
  funcs->C_FindObjects(session, &obj, 1, &count);
  funcs->C_FindObjectsFinal(session);

  return (count > 0) ? obj : CK_INVALID_HANDLE;
}

/* Extract x-only public key (32 bytes) from EC point attribute */
static gboolean
extract_xonly_pubkey(const guint8 *ec_point, gsize ec_point_len, guint8 *xonly_out)
{
  /* EC point is typically DER encoded: 0x04 || x || y for uncompressed
   * or wrapped in OCTET STRING: 0x04 <len> 0x04 || x || y
   * We want the x coordinate (32 bytes for secp256k1) */

  if (ec_point_len == 0)
    return FALSE;

  const guint8 *point_data = ec_point;
  gsize point_len = ec_point_len;

  /* Check for OCTET STRING wrapper (0x04 is tag for OCTET STRING in DER) */
  if (point_data[0] == 0x04 && point_len > 2) {
    /* Could be OCTET STRING wrapping, or uncompressed point directly */
    /* If second byte is length and matches remaining data, it's wrapped */
    if (point_data[1] == point_len - 2) {
      /* Skip OCTET STRING tag and length */
      point_data += 2;
      point_len -= 2;
    }
  }

  /* Now we should have: 0x04 || x (32 bytes) || y (32 bytes) for uncompressed */
  if (point_len == 65 && point_data[0] == 0x04) {
    /* Uncompressed point - extract x coordinate */
    memcpy(xonly_out, point_data + 1, 32);
    return TRUE;
  }

  /* Or compressed: 0x02/0x03 || x (32 bytes) */
  if (point_len == 33 && (point_data[0] == 0x02 || point_data[0] == 0x03)) {
    memcpy(xonly_out, point_data + 1, 32);
    return TRUE;
  }

  /* Raw 32-byte x coordinate */
  if (point_len == 32) {
    memcpy(xonly_out, point_data, 32);
    return TRUE;
  }

  return FALSE;
}

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

#ifndef GNOSTR_HAVE_PKCS11
  (void)slot_id;
  (void)key_id;
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

  /* Find the module for this slot */
  Pkcs11Module *mod = find_module_for_slot(self, slot_id);
  if (!mod) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "No module found for slot %" G_GUINT64_FORMAT, slot_id);
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* Open a session */
  CK_SESSION_HANDLE session;
  CK_RV rv = mod->functions->C_OpenSession(slot_id,
                                           CKF_SERIAL_SESSION,
                                           NULL, NULL, &session);
  if (rv != CKR_OK) {
    g_set_error(error, GN_HSM_ERROR, map_pkcs11_error(rv),
                "Failed to open session: %s", rv_to_string(rv));
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* Find the public key object */
  CK_OBJECT_CLASS pub_class = CKO_PUBLIC_KEY;
  CK_OBJECT_HANDLE pub_obj = find_key_object(mod->functions, session, key_id, pub_class);

  if (pub_obj == CK_INVALID_HANDLE) {
    /* Try finding the private key and extracting public from it */
    CK_OBJECT_CLASS priv_class = CKO_PRIVATE_KEY;
    CK_OBJECT_HANDLE priv_obj = find_key_object(mod->functions, session, key_id, priv_class);
    if (priv_obj == CK_INVALID_HANDLE) {
      mod->functions->C_CloseSession(session);
      g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                  "Key '%s' not found in slot %" G_GUINT64_FORMAT, key_id, slot_id);
      g_mutex_unlock(&self->lock);
      return NULL;
    }
    /* Use the private key object - we can get CKA_ID to find matching public */
    pub_obj = priv_obj;
  }

  /* Get key attributes */
  CK_BYTE label[256] = {0};
  CK_BYTE ec_point[256] = {0};
  CK_KEY_TYPE key_type = 0;
  CK_ATTRIBUTE attrs[] = {
    {CKA_LABEL, label, sizeof(label)},
    {CKA_EC_POINT, ec_point, sizeof(ec_point)},
    {CKA_KEY_TYPE, &key_type, sizeof(key_type)}
  };

  rv = mod->functions->C_GetAttributeValue(session, pub_obj, attrs, 3);

  /* If we couldn't get EC_POINT from private key, try finding the public key */
  if (rv != CKR_OK || attrs[1].ulValueLen == CK_UNAVAILABLE_INFORMATION) {
    /* Need to find the matching public key */
    gsize id_len = 0;
    guchar *id_bytes = g_base64_decode(key_id, &id_len);
    if (id_bytes) {
      CK_OBJECT_CLASS pub_class2 = CKO_PUBLIC_KEY;
      CK_ATTRIBUTE find_template[] = {
        {CKA_CLASS, &pub_class2, sizeof(pub_class2)},
        {CKA_ID, id_bytes, id_len}
      };

      rv = mod->functions->C_FindObjectsInit(session, find_template, 2);
      if (rv == CKR_OK) {
        CK_OBJECT_HANDLE found_pub = CK_INVALID_HANDLE;
        CK_ULONG count = 0;
        mod->functions->C_FindObjects(session, &found_pub, 1, &count);
        mod->functions->C_FindObjectsFinal(session);

        if (count > 0) {
          pub_obj = found_pub;
          attrs[1].ulValueLen = sizeof(ec_point);
          rv = mod->functions->C_GetAttributeValue(session, pub_obj, attrs, 3);
        }
      }
      g_free(id_bytes);
    }
  }

  mod->functions->C_CloseSession(session);

  if (rv != CKR_OK) {
    g_set_error(error, GN_HSM_ERROR, map_pkcs11_error(rv),
                "Failed to get key attributes: %s", rv_to_string(rv));
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* Extract the x-only public key from EC point */
  guint8 xonly_pk[32];
  if (!extract_xonly_pubkey(ec_point, attrs[1].ulValueLen, xonly_pk)) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Failed to extract public key from EC point");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* Create key info */
  GnHsmKeyInfo *info = g_new0(GnHsmKeyInfo, 1);
  info->key_id = g_strdup(key_id);
  info->label = g_strndup((gchar *)label, attrs[0].ulValueLen);
  info->key_type = GN_HSM_KEY_TYPE_SECP256K1;
  info->slot_id = slot_id;
  info->can_sign = TRUE;
  info->is_extractable = FALSE;

  /* Convert public key to hex */
  info->pubkey_hex = bytes_to_hex(xonly_pk, 32);

  /* Generate npub */
  GNostrNip19 *nip19 = gnostr_nip19_encode_npub(info->pubkey_hex, NULL);
  if (nip19) {
    info->npub = g_strdup(gnostr_nip19_get_bech32(nip19));
    g_object_unref(nip19);
  } else {
    info->npub = g_strdup_printf("npub1%s", info->pubkey_hex);
  }

  /* Get creation time if available (use current time as fallback) */
  info->created_at = g_strdup_printf("%" G_GINT64_FORMAT, (gint64)time(NULL));

  g_mutex_unlock(&self->lock);
  return info;
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

#ifndef GNOSTR_HAVE_PKCS11
  (void)slot_id;
  (void)key_id;
  (void)hash;
  (void)hash_len;
  (void)signature;
  (void)signature_len;
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "PKCS#11 support not compiled in");
  return FALSE;
#else
  if (hash_len != 32) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Hash must be 32 bytes, got %" G_GSIZE_FORMAT, hash_len);
    return FALSE;
  }

  if (*signature_len < 64) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Signature buffer too small (need 64, got %" G_GSIZE_FORMAT ")",
                *signature_len);
    return FALSE;
  }

  g_mutex_lock(&self->lock);

  if (!self->initialized) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_INITIALIZED,
                "Provider not initialized");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Find the module for this slot */
  Pkcs11Module *mod = find_module_for_slot(self, slot_id);
  if (!mod) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "No module found for slot %" G_GUINT64_FORMAT, slot_id);
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Check for existing session or open a new one */
  SlotSession *sess = g_hash_table_lookup(self->sessions,
                                          GUINT_TO_POINTER((guint)slot_id));
  CK_SESSION_HANDLE session;
  gboolean own_session = FALSE;

  if (sess && sess->session) {
    session = sess->session;
  } else {
    CK_RV rv = mod->functions->C_OpenSession(slot_id,
                                             CKF_SERIAL_SESSION | CKF_RW_SESSION,
                                             NULL, NULL, &session);
    if (rv != CKR_OK) {
      g_set_error(error, GN_HSM_ERROR, map_pkcs11_error(rv),
                  "Failed to open session: %s", rv_to_string(rv));
      g_mutex_unlock(&self->lock);
      return FALSE;
    }
    own_session = TRUE;
  }

  /* Find the private key object */
  CK_OBJECT_CLASS priv_class = CKO_PRIVATE_KEY;
  CK_OBJECT_HANDLE priv_key = find_key_object(mod->functions, session, key_id, priv_class);

  if (priv_key == CK_INVALID_HANDLE) {
    if (own_session)
      mod->functions->C_CloseSession(session);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "Private key '%s' not found", key_id);
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Check if software signing is enabled and needed */
  gboolean use_software_signing = self->software_signing_enabled;

  if (use_software_signing) {
    /* Try to extract the private key for software signing (if extractable) */
    /* Note: Most HSM keys are NOT extractable, which is the point.
     * For tokens without secp256k1 support, we need to store the key
     * in a way that allows extraction, or use a wrapped key approach.
     * For now, we'll try PKCS#11 ECDSA and fall back to error if not supported. */

    /* First, try hardware signing with ECDSA mechanism */
    CK_MECHANISM mechanism = {CKM_ECDSA, NULL, 0};

    CK_RV rv = mod->functions->C_SignInit(session, &mechanism, priv_key);
    if (rv == CKR_OK) {
      /* Hardware supports ECDSA - use it */
      CK_BYTE raw_sig[72]; /* DER encoded ECDSA signature max size */
      CK_ULONG raw_sig_len = sizeof(raw_sig);

      rv = mod->functions->C_Sign(session, (CK_BYTE_PTR)hash, hash_len,
                                  raw_sig, &raw_sig_len);

      if (rv == CKR_OK) {
        /* PKCS#11 ECDSA returns raw (r,s) concatenated for secp256k1 (64 bytes)
         * or DER encoded for some implementations */
        if (raw_sig_len == 64) {
          /* Raw r||s format - exactly what we need for Schnorr-style */
          memcpy(signature, raw_sig, 64);
          *signature_len = 64;
        } else if (raw_sig_len > 64) {
          /* Likely DER encoded - need to decode */
          /* DER: 0x30 <len> 0x02 <r_len> <r> 0x02 <s_len> <s> */
          if (raw_sig[0] == 0x30) {
            gsize offset = 2; /* Skip 0x30 and length */
            if (raw_sig[1] & 0x80) offset++; /* Long form length */

            /* Extract r */
            if (raw_sig[offset] == 0x02) {
              offset++;
              gsize r_len = raw_sig[offset++];
              const guint8 *r_ptr = &raw_sig[offset];
              if (r_len > 32 && r_ptr[0] == 0x00) {
                r_ptr++;
                r_len--;
              }
              gsize r_pad = (r_len < 32) ? 32 - r_len : 0;
              memset(signature, 0, r_pad);
              memcpy(signature + r_pad, r_ptr, (r_len > 32) ? 32 : r_len);
              offset += raw_sig[offset - 1 - (r_ptr - &raw_sig[offset])];

              /* Extract s */
              offset = 2 + (raw_sig[1] & 0x80 ? 1 : 0) + 2 + raw_sig[3 + (raw_sig[1] & 0x80 ? 1 : 0)];
              if (raw_sig[offset] == 0x02) {
                offset++;
                gsize s_len = raw_sig[offset++];
                const guint8 *s_ptr = &raw_sig[offset];
                if (s_len > 32 && s_ptr[0] == 0x00) {
                  s_ptr++;
                  s_len--;
                }
                gsize s_pad = (s_len < 32) ? 32 - s_len : 0;
                memset(signature + 32, 0, s_pad);
                memcpy(signature + 32 + s_pad, s_ptr, (s_len > 32) ? 32 : s_len);
              }
            }
            *signature_len = 64;
          } else {
            /* Unknown format */
            if (own_session)
              mod->functions->C_CloseSession(session);
            g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                        "Unknown signature format from HSM");
            g_mutex_unlock(&self->lock);
            return FALSE;
          }
        } else {
          if (own_session)
            mod->functions->C_CloseSession(session);
          g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                      "Unexpected signature length: %lu", (unsigned long)raw_sig_len);
          g_mutex_unlock(&self->lock);
          return FALSE;
        }

        if (own_session)
          mod->functions->C_CloseSession(session);
        g_mutex_unlock(&self->lock);
        return TRUE;
      }
    }

    /* Hardware signing failed - check if key is extractable for software fallback */
    CK_BBOOL extractable = CK_FALSE;
    CK_ATTRIBUTE ext_attr = {CKA_EXTRACTABLE, &extractable, sizeof(extractable)};
    mod->functions->C_GetAttributeValue(session, priv_key, &ext_attr, 1);

    if (extractable == CK_TRUE) {
      /* Extract private key for software signing */
      CK_BYTE priv_value[32];
      CK_ATTRIBUTE val_attr = {CKA_VALUE, priv_value, sizeof(priv_value)};

      rv = mod->functions->C_GetAttributeValue(session, priv_key, &val_attr, 1);
      if (rv == CKR_OK && val_attr.ulValueLen == 32) {
        /* Use libnostr for Schnorr signing */
        gchar *sk_hex = bytes_to_hex(priv_value, 32);
        gchar *hash_hex = bytes_to_hex(hash, 32);

        /* Create a temporary event to use nostr_event_sign */
        NostrEvent *temp_event = nostr_event_new();
        if (temp_event) {
          /* Get public key */
          GNostrKeys *gkeys = gnostr_keys_new_from_hex(sk_hex, NULL);
          if (gkeys) {
            const gchar *pk_hex = gnostr_keys_get_pubkey(gkeys);
            nostr_event_set_pubkey(temp_event, pk_hex);
            nostr_event_set_kind(temp_event, 1);
            nostr_event_set_created_at(temp_event, time(NULL));
            nostr_event_set_content(temp_event, "");
            temp_event->id = g_strdup(hash_hex);

            int sign_result = nostr_event_sign(temp_event, sk_hex);
            if (sign_result == 0) {
              const char *sig_hex = nostr_event_get_sig(temp_event);
              if (sig_hex && hex_to_bytes(sig_hex, signature, 64)) {
                *signature_len = 64;
                nostr_event_free(temp_event);
                memset(priv_value, 0, sizeof(priv_value));
                memset(sk_hex, 0, strlen(sk_hex));
                g_free(sk_hex);
                g_free(hash_hex);
                g_object_unref(gkeys);
                if (own_session)
                  mod->functions->C_CloseSession(session);
                g_mutex_unlock(&self->lock);
                return TRUE;
              }
            }
            g_object_unref(gkeys);
          }
          nostr_event_free(temp_event);
        }

        memset(priv_value, 0, sizeof(priv_value));
        memset(sk_hex, 0, strlen(sk_hex));
        g_free(sk_hex);
        g_free(hash_hex);
      }
    }
  }

  /* No supported signing method worked */
  if (own_session)
    mod->functions->C_CloseSession(session);

  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
              "Token does not support secp256k1 signing and software fallback failed");
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

#ifndef GNOSTR_HAVE_PKCS11
  (void)slot_id;
  (void)key_id;
  (void)event_json;
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

  /* Parse the event JSON */
  NostrEvent *event = nostr_event_new();
  if (!event) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Failed to create event for signing");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  if (!nostr_event_deserialize_compact(event, event_json, NULL)) {
    nostr_event_free(event);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Failed to parse event JSON");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* Get the event ID (hash) */
  gchar *event_id = nostr_event_get_id(event);
  if (!event_id) {
    nostr_event_free(event);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Failed to compute event ID");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* Convert event ID hex to bytes */
  guint8 hash[32];
  if (!hex_to_bytes(event_id, hash, 32)) {
    g_free(event_id);
    nostr_event_free(event);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Invalid event ID format");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* Sign the hash */
  guint8 signature[64];
  gsize signature_len = sizeof(signature);

  /* Temporarily unlock for the sign_hash call to avoid deadlock */
  g_mutex_unlock(&self->lock);

  gboolean signed_ok = pkcs11_sign_hash(provider, slot_id, key_id,
                                        hash, 32, signature, &signature_len, error);

  if (!signed_ok) {
    g_free(event_id);
    nostr_event_free(event);
    return NULL;
  }

  /* Set the event ID and signature */
  event->id = event_id; /* Transfer ownership */
  gchar *sig_hex = bytes_to_hex(signature, 64);
  nostr_event_set_sig(event, sig_hex);
  g_free(sig_hex);

  /* Serialize the signed event */
  gchar *signed_json = nostr_event_serialize_compact(event);
  nostr_event_free(event);

  if (!signed_json) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_SIGNING_FAILED,
                "Failed to serialize signed event");
    return NULL;
  }

  return signed_json;
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

#ifndef GNOSTR_HAVE_PKCS11
  (void)slot_id;
  (void)label;
  (void)key_type;
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "PKCS#11 support not compiled in");
  return NULL;
#else
  if (key_type != GN_HSM_KEY_TYPE_SECP256K1) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Only secp256k1 keys are supported");
    return NULL;
  }

  g_mutex_lock(&self->lock);

  if (!self->initialized) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_INITIALIZED,
                "Provider not initialized");
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* Find the module for this slot */
  Pkcs11Module *mod = find_module_for_slot(self, slot_id);
  if (!mod) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "No module found for slot %" G_GUINT64_FORMAT, slot_id);
    g_mutex_unlock(&self->lock);
    return NULL;
  }

  /* Check for existing session or open a new one */
  SlotSession *sess = g_hash_table_lookup(self->sessions,
                                          GUINT_TO_POINTER((guint)slot_id));
  CK_SESSION_HANDLE session;
  gboolean own_session = FALSE;

  if (sess && sess->session) {
    session = sess->session;
  } else {
    CK_RV rv = mod->functions->C_OpenSession(slot_id,
                                             CKF_SERIAL_SESSION | CKF_RW_SESSION,
                                             NULL, NULL, &session);
    if (rv != CKR_OK) {
      g_set_error(error, GN_HSM_ERROR, map_pkcs11_error(rv),
                  "Failed to open session: %s", rv_to_string(rv));
      g_mutex_unlock(&self->lock);
      return NULL;
    }
    own_session = TRUE;
  }

  /* Generate a unique key ID */
  gchar *key_id_str = generate_key_id();
  gsize key_id_len = 0;
  guchar *key_id_bytes = g_base64_decode(key_id_str, &key_id_len);

  /* Try to generate key on the HSM first */
  CK_MECHANISM mechanism = {CKM_EC_KEY_PAIR_GEN, NULL, 0};
  CK_BBOOL ck_true = CK_TRUE;
  CK_BBOOL ck_false = CK_FALSE;

  /* Check if token supports secp256k1 */
  gboolean has_secp256k1 = gn_hsm_provider_pkcs11_has_secp256k1_support(self, slot_id);

  CK_OBJECT_HANDLE pub_key = CK_INVALID_HANDLE;
  CK_OBJECT_HANDLE priv_key = CK_INVALID_HANDLE;
  guint8 public_key[32] = {0};
  guint8 private_key[32] = {0};
  gboolean generated_in_software = FALSE;

  if (has_secp256k1) {
    /* Generate on HSM with secp256k1 curve */
    CK_ATTRIBUTE pub_template[] = {
      {CKA_TOKEN, &ck_true, sizeof(ck_true)},
      {CKA_VERIFY, &ck_true, sizeof(ck_true)},
      {CKA_EC_PARAMS, (CK_VOID_PTR)SECP256K1_OID, sizeof(SECP256K1_OID)},
      {CKA_LABEL, (CK_VOID_PTR)label, strlen(label)},
      {CKA_ID, key_id_bytes, key_id_len}
    };

    CK_ATTRIBUTE priv_template[] = {
      {CKA_TOKEN, &ck_true, sizeof(ck_true)},
      {CKA_PRIVATE, &ck_true, sizeof(ck_true)},
      {CKA_SENSITIVE, &ck_true, sizeof(ck_true)},
      {CKA_EXTRACTABLE, &ck_false, sizeof(ck_false)},
      {CKA_SIGN, &ck_true, sizeof(ck_true)},
      {CKA_LABEL, (CK_VOID_PTR)label, strlen(label)},
      {CKA_ID, key_id_bytes, key_id_len}
    };

    CK_RV rv = mod->functions->C_GenerateKeyPair(session, &mechanism,
                                                  pub_template, 5,
                                                  priv_template, 7,
                                                  &pub_key, &priv_key);

    if (rv == CKR_OK) {
      /* Extract public key */
      CK_BYTE ec_point[256];
      CK_ATTRIBUTE ec_attr = {CKA_EC_POINT, ec_point, sizeof(ec_point)};
      rv = mod->functions->C_GetAttributeValue(session, pub_key, &ec_attr, 1);
      if (rv == CKR_OK) {
        extract_xonly_pubkey(ec_point, ec_attr.ulValueLen, public_key);
      }
    } else {
      g_message("PKCS#11: Hardware key generation failed (%s), trying software",
                rv_to_string(rv));
    }
  }

  if (pub_key == CK_INVALID_HANDLE) {
    /* Generate in software and store on token */
    gchar *sk_hex = nostr_key_generate_private();
    if (!sk_hex) {
      g_free(key_id_bytes);
      g_free(key_id_str);
      if (own_session)
        mod->functions->C_CloseSession(session);
      g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_KEY_GENERATION_FAILED,
                  "Failed to generate key in software");
      g_mutex_unlock(&self->lock);
      return NULL;
    }

    GNostrKeys *gkeys = gnostr_keys_new_from_hex(sk_hex, NULL);
    if (!gkeys) {
      memset(sk_hex, 0, strlen(sk_hex));
      g_free(sk_hex);
      g_free(key_id_bytes);
      g_free(key_id_str);
      if (own_session)
        mod->functions->C_CloseSession(session);
      g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_KEY_GENERATION_FAILED,
                  "Failed to derive public key");
      g_mutex_unlock(&self->lock);
      return NULL;
    }

    const gchar *pk_hex = gnostr_keys_get_pubkey(gkeys);
    hex_to_bytes(sk_hex, private_key, 32);
    hex_to_bytes(pk_hex, public_key, 32);
    memset(sk_hex, 0, strlen(sk_hex));
    g_free(sk_hex);
    g_object_unref(gkeys);
    generated_in_software = TRUE;

    /* Build EC point (uncompressed: 0x04 || x || y) */
    /* For simplicity, store as 0x04 || x || x (we only have x, but some tokens need 65 bytes) */
    /* Better approach: compute y from x for secp256k1, but for now store as extractable key */
    CK_BYTE ec_point[67];
    ec_point[0] = 0x04; /* OCTET STRING tag */
    ec_point[1] = 65;   /* Length */
    ec_point[2] = 0x04; /* Uncompressed point marker */
    memcpy(&ec_point[3], public_key, 32); /* x coordinate */
    memset(&ec_point[35], 0, 32); /* y placeholder - not used for x-only keys */

    /* Create private key object (extractable for software signing) */
    CK_OBJECT_CLASS priv_class = CKO_PRIVATE_KEY;
    CK_KEY_TYPE key_type_ec = CKK_EC;
    CK_ATTRIBUTE priv_template[] = {
      {CKA_CLASS, &priv_class, sizeof(priv_class)},
      {CKA_KEY_TYPE, &key_type_ec, sizeof(key_type_ec)},
      {CKA_TOKEN, &ck_true, sizeof(ck_true)},
      {CKA_PRIVATE, &ck_true, sizeof(ck_true)},
      {CKA_SENSITIVE, &ck_false, sizeof(ck_false)}, /* Allow extraction for software signing */
      {CKA_EXTRACTABLE, &ck_true, sizeof(ck_true)},
      {CKA_SIGN, &ck_true, sizeof(ck_true)},
      {CKA_EC_PARAMS, (CK_VOID_PTR)SECP256K1_OID, sizeof(SECP256K1_OID)},
      {CKA_VALUE, private_key, 32},
      {CKA_LABEL, (CK_VOID_PTR)label, strlen(label)},
      {CKA_ID, key_id_bytes, key_id_len}
    };

    CK_RV rv = mod->functions->C_CreateObject(session, priv_template, 11, &priv_key);

    /* Clear private key from memory */
    memset(private_key, 0, sizeof(private_key));

    if (rv != CKR_OK) {
      g_free(key_id_bytes);
      g_free(key_id_str);
      if (own_session)
        mod->functions->C_CloseSession(session);
      g_set_error(error, GN_HSM_ERROR, map_pkcs11_error(rv),
                  "Failed to store private key: %s", rv_to_string(rv));
      g_mutex_unlock(&self->lock);
      return NULL;
    }

    /* Create public key object */
    CK_OBJECT_CLASS pub_class = CKO_PUBLIC_KEY;
    CK_ATTRIBUTE pub_template[] = {
      {CKA_CLASS, &pub_class, sizeof(pub_class)},
      {CKA_KEY_TYPE, &key_type_ec, sizeof(key_type_ec)},
      {CKA_TOKEN, &ck_true, sizeof(ck_true)},
      {CKA_VERIFY, &ck_true, sizeof(ck_true)},
      {CKA_EC_PARAMS, (CK_VOID_PTR)SECP256K1_OID, sizeof(SECP256K1_OID)},
      {CKA_EC_POINT, ec_point, sizeof(ec_point)},
      {CKA_LABEL, (CK_VOID_PTR)label, strlen(label)},
      {CKA_ID, key_id_bytes, key_id_len}
    };

    rv = mod->functions->C_CreateObject(session, pub_template, 8, &pub_key);
    /* Public key creation failure is not fatal - private key is stored */
    if (rv != CKR_OK) {
      g_message("PKCS#11: Failed to store public key object: %s", rv_to_string(rv));
    }
  }

  g_free(key_id_bytes);

  if (own_session)
    mod->functions->C_CloseSession(session);

  /* Create return info */
  GnHsmKeyInfo *info = g_new0(GnHsmKeyInfo, 1);
  info->key_id = key_id_str;
  info->label = g_strdup(label);
  info->key_type = GN_HSM_KEY_TYPE_SECP256K1;
  info->slot_id = slot_id;
  info->can_sign = TRUE;
  info->is_extractable = generated_in_software;

  /* Convert public key to hex */
  info->pubkey_hex = bytes_to_hex(public_key, 32);

  /* Generate npub */
  GNostrNip19 *nip19 = gnostr_nip19_encode_npub(info->pubkey_hex, NULL);
  if (nip19) {
    info->npub = g_strdup(gnostr_nip19_get_bech32(nip19));
    g_object_unref(nip19);
  } else {
    info->npub = g_strdup_printf("npub1%s", info->pubkey_hex);
  }

  info->created_at = g_strdup_printf("%" G_GINT64_FORMAT, (gint64)time(NULL));

  g_mutex_unlock(&self->lock);
  return info;
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

#ifndef GNOSTR_HAVE_PKCS11
  (void)slot_id;
  (void)label;
  (void)private_key;
  (void)key_len;
  (void)out_info;
  g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_AVAILABLE,
              "PKCS#11 support not compiled in");
  return FALSE;
#else
  if (key_len != 32) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Private key must be 32 bytes");
    return FALSE;
  }

  g_mutex_lock(&self->lock);

  if (!self->initialized) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_INITIALIZED,
                "Provider not initialized");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Find the module for this slot */
  Pkcs11Module *mod = find_module_for_slot(self, slot_id);
  if (!mod) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "No module found for slot %" G_GUINT64_FORMAT, slot_id);
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Derive public key from private key */
  gchar *sk_hex = bytes_to_hex(private_key, 32);
  GNostrKeys *gkeys = gnostr_keys_new_from_hex(sk_hex, NULL);

  if (!gkeys) {
    memset(sk_hex, 0, strlen(sk_hex));
    g_free(sk_hex);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Failed to derive public key");
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  const gchar *pk_hex = gnostr_keys_get_pubkey(gkeys);
  guint8 public_key[32];
  hex_to_bytes(pk_hex, public_key, 32);
  memset(sk_hex, 0, strlen(sk_hex));
  g_free(sk_hex);
  g_object_unref(gkeys);

  /* Check for existing session or open a new one */
  SlotSession *sess = g_hash_table_lookup(self->sessions,
                                          GUINT_TO_POINTER((guint)slot_id));
  CK_SESSION_HANDLE session;
  gboolean own_session = FALSE;

  if (sess && sess->session) {
    session = sess->session;
  } else {
    CK_RV rv = mod->functions->C_OpenSession(slot_id,
                                             CKF_SERIAL_SESSION | CKF_RW_SESSION,
                                             NULL, NULL, &session);
    if (rv != CKR_OK) {
      g_set_error(error, GN_HSM_ERROR, map_pkcs11_error(rv),
                  "Failed to open session: %s", rv_to_string(rv));
      g_mutex_unlock(&self->lock);
      return FALSE;
    }
    own_session = TRUE;
  }

  /* Generate a unique key ID */
  gchar *key_id_str = generate_key_id();
  gsize key_id_len = 0;
  guchar *key_id_bytes = g_base64_decode(key_id_str, &key_id_len);

  /* Build EC point (uncompressed: 0x04 || x || y) */
  CK_BYTE ec_point[67];
  ec_point[0] = 0x04; /* OCTET STRING tag */
  ec_point[1] = 65;   /* Length */
  ec_point[2] = 0x04; /* Uncompressed point marker */
  memcpy(&ec_point[3], public_key, 32); /* x coordinate */
  memset(&ec_point[35], 0, 32); /* y placeholder */

  /* Create private key object */
  CK_OBJECT_CLASS priv_class = CKO_PRIVATE_KEY;
  CK_KEY_TYPE key_type_ec = CKK_EC;
  CK_BBOOL ck_true = CK_TRUE;
  CK_BBOOL ck_false = CK_FALSE;

  CK_ATTRIBUTE priv_template[] = {
    {CKA_CLASS, &priv_class, sizeof(priv_class)},
    {CKA_KEY_TYPE, &key_type_ec, sizeof(key_type_ec)},
    {CKA_TOKEN, &ck_true, sizeof(ck_true)},
    {CKA_PRIVATE, &ck_true, sizeof(ck_true)},
    {CKA_SENSITIVE, &ck_false, sizeof(ck_false)}, /* Allow extraction for software signing */
    {CKA_EXTRACTABLE, &ck_true, sizeof(ck_true)},
    {CKA_SIGN, &ck_true, sizeof(ck_true)},
    {CKA_EC_PARAMS, (CK_VOID_PTR)SECP256K1_OID, sizeof(SECP256K1_OID)},
    {CKA_VALUE, (CK_VOID_PTR)private_key, 32},
    {CKA_LABEL, (CK_VOID_PTR)label, strlen(label)},
    {CKA_ID, key_id_bytes, key_id_len}
  };

  CK_OBJECT_HANDLE priv_key = CK_INVALID_HANDLE;
  CK_RV rv = mod->functions->C_CreateObject(session, priv_template, 11, &priv_key);

  if (rv != CKR_OK) {
    g_free(key_id_bytes);
    g_free(key_id_str);
    if (own_session)
      mod->functions->C_CloseSession(session);
    g_set_error(error, GN_HSM_ERROR, map_pkcs11_error(rv),
                "Failed to import private key: %s", rv_to_string(rv));
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Create public key object */
  CK_OBJECT_CLASS pub_class = CKO_PUBLIC_KEY;
  CK_ATTRIBUTE pub_template[] = {
    {CKA_CLASS, &pub_class, sizeof(pub_class)},
    {CKA_KEY_TYPE, &key_type_ec, sizeof(key_type_ec)},
    {CKA_TOKEN, &ck_true, sizeof(ck_true)},
    {CKA_VERIFY, &ck_true, sizeof(ck_true)},
    {CKA_EC_PARAMS, (CK_VOID_PTR)SECP256K1_OID, sizeof(SECP256K1_OID)},
    {CKA_EC_POINT, ec_point, sizeof(ec_point)},
    {CKA_LABEL, (CK_VOID_PTR)label, strlen(label)},
    {CKA_ID, key_id_bytes, key_id_len}
  };

  CK_OBJECT_HANDLE pub_key = CK_INVALID_HANDLE;
  rv = mod->functions->C_CreateObject(session, pub_template, 8, &pub_key);
  /* Public key creation failure is not fatal */
  if (rv != CKR_OK) {
    g_message("PKCS#11: Failed to store public key object: %s", rv_to_string(rv));
  }

  g_free(key_id_bytes);

  if (own_session)
    mod->functions->C_CloseSession(session);

  /* Create return info if requested */
  if (out_info) {
    GnHsmKeyInfo *info = g_new0(GnHsmKeyInfo, 1);
    info->key_id = key_id_str;
    info->label = g_strdup(label);
    info->key_type = GN_HSM_KEY_TYPE_SECP256K1;
    info->slot_id = slot_id;
    info->can_sign = TRUE;
    info->is_extractable = TRUE;

    info->pubkey_hex = bytes_to_hex(public_key, 32);

    GNostrNip19 *nip19 = gnostr_nip19_encode_npub(info->pubkey_hex, NULL);
    if (nip19) {
      info->npub = g_strdup(gnostr_nip19_get_bech32(nip19));
      g_object_unref(nip19);
    } else {
      info->npub = g_strdup_printf("npub1%s", info->pubkey_hex);
    }

    info->created_at = g_strdup_printf("%" G_GINT64_FORMAT, (gint64)time(NULL));
    *out_info = info;
  } else {
    g_free(key_id_str);
  }

  g_mutex_unlock(&self->lock);
  return TRUE;
#endif
}

static gboolean
pkcs11_delete_key(GnHsmProvider *provider,
                  guint64 slot_id,
                  const gchar *key_id,
                  GError **error)
{
  GnHsmProviderPkcs11 *self = GN_HSM_PROVIDER_PKCS11(provider);

#ifndef GNOSTR_HAVE_PKCS11
  (void)slot_id;
  (void)key_id;
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

  /* Find the module for this slot */
  Pkcs11Module *mod = find_module_for_slot(self, slot_id);
  if (!mod) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "No module found for slot %" G_GUINT64_FORMAT, slot_id);
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Check for existing session or open a new one */
  SlotSession *sess = g_hash_table_lookup(self->sessions,
                                          GUINT_TO_POINTER((guint)slot_id));
  CK_SESSION_HANDLE session;
  gboolean own_session = FALSE;

  if (sess && sess->session) {
    session = sess->session;
  } else {
    CK_RV rv = mod->functions->C_OpenSession(slot_id,
                                             CKF_SERIAL_SESSION | CKF_RW_SESSION,
                                             NULL, NULL, &session);
    if (rv != CKR_OK) {
      g_set_error(error, GN_HSM_ERROR, map_pkcs11_error(rv),
                  "Failed to open session: %s", rv_to_string(rv));
      g_mutex_unlock(&self->lock);
      return FALSE;
    }
    own_session = TRUE;
  }

  gboolean deleted_any = FALSE;

  /* Delete private key object */
  CK_OBJECT_CLASS priv_class = CKO_PRIVATE_KEY;
  CK_OBJECT_HANDLE priv_key = find_key_object(mod->functions, session, key_id, priv_class);

  if (priv_key != CK_INVALID_HANDLE) {
    CK_RV rv = mod->functions->C_DestroyObject(session, priv_key);
    if (rv == CKR_OK) {
      deleted_any = TRUE;
    } else {
      g_message("PKCS#11: Failed to delete private key: %s", rv_to_string(rv));
    }
  }

  /* Delete public key object */
  CK_OBJECT_CLASS pub_class = CKO_PUBLIC_KEY;
  CK_OBJECT_HANDLE pub_key = find_key_object(mod->functions, session, key_id, pub_class);

  if (pub_key != CK_INVALID_HANDLE) {
    CK_RV rv = mod->functions->C_DestroyObject(session, pub_key);
    if (rv == CKR_OK) {
      deleted_any = TRUE;
    } else {
      g_message("PKCS#11: Failed to delete public key: %s", rv_to_string(rv));
    }
  }

  if (own_session)
    mod->functions->C_CloseSession(session);

  if (!deleted_any) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_NOT_FOUND,
                "Key '%s' not found in slot %" G_GUINT64_FORMAT, key_id, slot_id);
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  g_mutex_unlock(&self->lock);
  return TRUE;
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

  g_clear_pointer(&self->sessions, g_hash_table_unref);
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
  g_mutex_lock(&self->lock);

  /* Check if module is already loaded */
  for (GList *l = self->modules; l != NULL; l = l->next) {
    Pkcs11Module *mod = (Pkcs11Module *)l->data;
    if (g_strcmp0(mod->path, module_path) == 0) {
      g_mutex_unlock(&self->lock);
      /* Already loaded - not an error */
      return TRUE;
    }
  }

  /* Load the module using p11-kit */
  CK_FUNCTION_LIST *funcs = p11_kit_module_load(module_path, 0);
  if (!funcs) {
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Failed to load PKCS#11 module: %s", module_path);
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Initialize the module */
  CK_RV rv = funcs->C_Initialize(NULL);
  if (rv != CKR_OK && rv != CKR_CRYPTOKI_ALREADY_INITIALIZED) {
    p11_kit_module_release(funcs);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Failed to initialize PKCS#11 module: %s", rv_to_string(rv));
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Get module info */
  CK_INFO info;
  rv = funcs->C_GetInfo(&info);
  if (rv != CKR_OK) {
    funcs->C_Finalize(NULL);
    p11_kit_module_release(funcs);
    g_set_error(error, GN_HSM_ERROR, GN_HSM_ERROR_FAILED,
                "Failed to get module info: %s", rv_to_string(rv));
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Create module entry */
  Pkcs11Module *mod = g_new0(Pkcs11Module, 1);
  mod->path = g_strdup(module_path);
  mod->functions = funcs;
  mod->is_p11kit = FALSE; /* Manually loaded, not via p11-kit discovery */
  mod->description = trim_pkcs11_string(info.libraryDescription,
                                        sizeof(info.libraryDescription));
  mod->manufacturer = trim_pkcs11_string(info.manufacturerID,
                                         sizeof(info.manufacturerID));
  mod->version = g_strdup_printf("%d.%d",
                                 info.libraryVersion.major,
                                 info.libraryVersion.minor);

  self->modules = g_list_prepend(self->modules, mod);

  g_mutex_unlock(&self->lock);

  g_message("PKCS#11: Loaded module '%s' (%s)", module_path, mod->description);
  return TRUE;
#endif
}

void
gn_hsm_provider_pkcs11_remove_module(GnHsmProviderPkcs11 *self,
                                     const gchar *module_path)
{
  g_return_if_fail(GN_IS_HSM_PROVIDER_PKCS11(self));
  g_return_if_fail(module_path != NULL);

#ifdef GNOSTR_HAVE_PKCS11
  g_mutex_lock(&self->lock);

  for (GList *l = self->modules; l != NULL; l = l->next) {
    Pkcs11Module *mod = (Pkcs11Module *)l->data;
    if (g_strcmp0(mod->path, module_path) == 0) {
      /* Don't remove modules loaded via p11-kit discovery */
      if (mod->is_p11kit) {
        g_warning("Cannot remove p11-kit managed module: %s", module_path);
        g_mutex_unlock(&self->lock);
        return;
      }

      /* Close any sessions using this module */
      if (mod->functions) {
        CK_ULONG slot_count = 0;
        CK_RV rv = mod->functions->C_GetSlotList(CK_TRUE, NULL, &slot_count);
        if (rv == CKR_OK && slot_count > 0) {
          CK_SLOT_ID *slots = g_new(CK_SLOT_ID, slot_count);
          rv = mod->functions->C_GetSlotList(CK_TRUE, slots, &slot_count);
          if (rv == CKR_OK) {
            for (CK_ULONG i = 0; i < slot_count; i++) {
              SlotSession *sess = g_hash_table_lookup(self->sessions,
                                                      GUINT_TO_POINTER((guint)slots[i]));
              if (sess) {
                mod->functions->C_CloseSession(sess->session);
                g_hash_table_remove(self->sessions, GUINT_TO_POINTER((guint)slots[i]));
              }
            }
          }
          g_free(slots);
        }

        /* Finalize and release the module */
        mod->functions->C_Finalize(NULL);
        p11_kit_module_release(mod->functions);
        mod->functions = NULL;
      }

      /* Remove from list */
      self->modules = g_list_remove(self->modules, mod);

      /* Free module structure (but don't call C_Finalize again) */
      g_free(mod->path);
      g_free(mod->description);
      g_free(mod->manufacturer);
      g_free(mod->version);
      g_free(mod);

      g_message("PKCS#11: Removed module '%s'", module_path);
      break;
    }
  }

  g_mutex_unlock(&self->lock);
#endif
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

#ifndef GNOSTR_HAVE_PKCS11
  (void)slot_id;
  return FALSE;
#else
  g_mutex_lock(&self->lock);

  if (!self->initialized) {
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Find the module for this slot */
  Pkcs11Module *mod = find_module_for_slot(self, slot_id);
  if (!mod) {
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Get the list of mechanisms supported by this token */
  CK_ULONG mech_count = 0;
  CK_RV rv = mod->functions->C_GetMechanismList(slot_id, NULL, &mech_count);
  if (rv != CKR_OK || mech_count == 0) {
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  CK_MECHANISM_TYPE *mechs = g_new(CK_MECHANISM_TYPE, mech_count);
  rv = mod->functions->C_GetMechanismList(slot_id, mechs, &mech_count);
  if (rv != CKR_OK) {
    g_free(mechs);
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* Check if ECDSA is supported */
  gboolean has_ecdsa = FALSE;
  gboolean has_ec_key_gen = FALSE;

  for (CK_ULONG i = 0; i < mech_count; i++) {
    if (mechs[i] == CKM_ECDSA) {
      has_ecdsa = TRUE;
    } else if (mechs[i] == CKM_EC_KEY_PAIR_GEN) {
      has_ec_key_gen = TRUE;
    }
  }
  g_free(mechs);

  if (!has_ecdsa) {
    g_mutex_unlock(&self->lock);
    return FALSE;
  }

  /* ECDSA is supported, but we need to check if secp256k1 curve is supported.
   * Most tokens support NIST curves (P-256, P-384, P-521) but not secp256k1.
   *
   * Unfortunately, PKCS#11 doesn't provide a direct way to query supported curves.
   * The best we can do is try to create a key with secp256k1 OID and see if it fails.
   *
   * For now, we'll do a simple heuristic:
   * - If the token supports EC key generation, try to generate a temporary keypair
   * - If it succeeds with secp256k1 OID, the curve is supported
   * - Otherwise, assume it's not supported
   */

  if (has_ec_key_gen) {
    /* Open a session to test */
    CK_SESSION_HANDLE session;
    rv = mod->functions->C_OpenSession(slot_id,
                                       CKF_SERIAL_SESSION | CKF_RW_SESSION,
                                       NULL, NULL, &session);
    if (rv == CKR_OK) {
      /* Try to get mechanism info for ECDSA */
      CK_MECHANISM_INFO mech_info;
      rv = mod->functions->C_GetMechanismInfo(slot_id, CKM_ECDSA, &mech_info);

      if (rv == CKR_OK) {
        /* Check if the mechanism supports key sizes compatible with secp256k1
         * secp256k1 uses 256-bit keys */
        if (mech_info.ulMinKeySize <= 256 && mech_info.ulMaxKeySize >= 256) {
          /* Key size is compatible. Now try to verify by attempting key generation.
           * We won't actually keep the key - just test if the OID is accepted. */

          CK_MECHANISM mechanism = {CKM_EC_KEY_PAIR_GEN, NULL, 0};
          CK_BBOOL ck_false = CK_FALSE;
          CK_BBOOL ck_true = CK_TRUE;

          /* Create templates for a session-only (non-persistent) test key */
          CK_ATTRIBUTE pub_template[] = {
            {CKA_TOKEN, &ck_false, sizeof(ck_false)}, /* Session object - auto-deleted */
            {CKA_EC_PARAMS, (CK_VOID_PTR)SECP256K1_OID, sizeof(SECP256K1_OID)}
          };

          CK_ATTRIBUTE priv_template[] = {
            {CKA_TOKEN, &ck_false, sizeof(ck_false)},
            {CKA_SIGN, &ck_true, sizeof(ck_true)}
          };

          CK_OBJECT_HANDLE test_pub = CK_INVALID_HANDLE;
          CK_OBJECT_HANDLE test_priv = CK_INVALID_HANDLE;

          rv = mod->functions->C_GenerateKeyPair(session, &mechanism,
                                                  pub_template, 2,
                                                  priv_template, 2,
                                                  &test_pub, &test_priv);

          if (rv == CKR_OK) {
            /* secp256k1 is supported! Clean up the test keys. */
            mod->functions->C_DestroyObject(session, test_pub);
            mod->functions->C_DestroyObject(session, test_priv);
            mod->functions->C_CloseSession(session);
            g_mutex_unlock(&self->lock);
            return TRUE;
          }
        }
      }

      mod->functions->C_CloseSession(session);
    }
  }

  g_mutex_unlock(&self->lock);
  return FALSE;
#endif
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
