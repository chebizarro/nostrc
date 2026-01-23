/* hw_wallet_ledger.c - Ledger hardware wallet provider implementation
 *
 * Implements USB HID communication with Ledger devices using APDU protocol.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hw_wallet_ledger.h"
#include <string.h>
#include <stdio.h>

#ifdef GNOSTR_HAVE_HIDAPI
#include <hidapi/hidapi.h>
#endif

/* ============================================================================
 * Internal Structures
 * ============================================================================ */

/* Open device handle */
typedef struct {
  gchar *device_id;
#ifdef GNOSTR_HAVE_HIDAPI
  hid_device *handle;
#else
  gpointer handle;
#endif
  GnHwWalletState state;
  gchar *app_name;
  gchar *app_version;
} LedgerDeviceHandle;

struct _GnHwWalletLedgerProvider {
  GObject parent_instance;
  GHashTable *open_devices; /* device_id -> LedgerDeviceHandle */
  GMutex lock;
};

static void gn_hw_wallet_provider_iface_init(GnHwWalletProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnHwWalletLedgerProvider, gn_hw_wallet_ledger_provider, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GN_TYPE_HW_WALLET_PROVIDER,
                                              gn_hw_wallet_provider_iface_init))

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

static void
ledger_device_handle_free(LedgerDeviceHandle *handle)
{
  if (!handle)
    return;
#ifdef GNOSTR_HAVE_HIDAPI
  if (handle->handle)
    hid_close(handle->handle);
#endif
  g_free(handle->device_id);
  g_free(handle->app_name);
  g_free(handle->app_version);
  g_free(handle);
}

/* Determine Ledger device type from USB product ID */
static GnHwWalletType
ledger_pid_to_type(unsigned short pid)
{
  switch (pid) {
    case GN_HW_WALLET_LEDGER_NANO_S_PID:
      return GN_HW_WALLET_TYPE_LEDGER_NANO_S;
    case GN_HW_WALLET_LEDGER_NANO_X_PID:
      return GN_HW_WALLET_TYPE_LEDGER_NANO_X;
    case GN_HW_WALLET_LEDGER_NANO_S_PLUS_PID:
      return GN_HW_WALLET_TYPE_LEDGER_NANO_S_PLUS;
    default:
      return GN_HW_WALLET_TYPE_UNKNOWN;
  }
}

#ifdef GNOSTR_HAVE_HIDAPI

/* Build APDU command buffer */
static gsize
ledger_build_apdu(guint8 *buffer, guint8 cla, guint8 ins, guint8 p1, guint8 p2,
                  const guint8 *data, gsize data_len)
{
  gsize offset = 0;

  buffer[offset++] = cla;
  buffer[offset++] = ins;
  buffer[offset++] = p1;
  buffer[offset++] = p2;

  if (data && data_len > 0) {
    buffer[offset++] = (guint8)data_len;
    memcpy(buffer + offset, data, data_len);
    offset += data_len;
  } else {
    buffer[offset++] = 0;
  }

  return offset;
}

/* Wrap APDU for HID transport (Ledger proprietary framing) */
static gsize
ledger_wrap_apdu(guint8 *output, const guint8 *apdu, gsize apdu_len, guint16 channel_id)
{
  gsize offset = 0;
  gsize apdu_offset = 0;
  guint8 seq_idx = 0;

  while (apdu_offset < apdu_len) {
    gsize chunk_size;
    guint8 packet[LEDGER_HID_PACKET_SIZE];
    memset(packet, 0, sizeof(packet));

    packet[0] = (channel_id >> 8) & 0xFF;
    packet[1] = channel_id & 0xFF;
    packet[2] = 0x05; /* TAG_APDU */
    packet[3] = (seq_idx >> 8) & 0xFF;
    packet[4] = seq_idx & 0xFF;

    if (seq_idx == 0) {
      /* First packet includes length */
      packet[5] = (apdu_len >> 8) & 0xFF;
      packet[6] = apdu_len & 0xFF;
      chunk_size = MIN(apdu_len - apdu_offset, LEDGER_HID_PACKET_SIZE - 7);
      memcpy(packet + 7, apdu + apdu_offset, chunk_size);
    } else {
      chunk_size = MIN(apdu_len - apdu_offset, LEDGER_HID_PACKET_SIZE - 5);
      memcpy(packet + 5, apdu + apdu_offset, chunk_size);
    }

    memcpy(output + offset, packet, LEDGER_HID_PACKET_SIZE);
    offset += LEDGER_HID_PACKET_SIZE;
    apdu_offset += chunk_size;
    seq_idx++;
  }

  return offset;
}

/* Unwrap HID response to extract APDU response */
static gsize
ledger_unwrap_response(const guint8 *input, gsize input_len, guint8 *output,
                       gsize output_max, guint16 channel_id)
{
  if (input_len < LEDGER_HID_PACKET_SIZE)
    return 0;

  /* Verify channel and tag */
  guint16 recv_channel = (input[0] << 8) | input[1];
  if (recv_channel != channel_id)
    return 0;
  if (input[2] != 0x05) /* TAG_APDU */
    return 0;

  /* First packet contains length */
  guint16 resp_len = (input[5] << 8) | input[6];
  if (resp_len > output_max)
    return 0;

  gsize offset = 0;
  gsize packet_offset = 0;
  guint8 seq_idx = 0;

  while (offset < resp_len && packet_offset < input_len) {
    gsize chunk_start, chunk_size;

    if (seq_idx == 0) {
      chunk_start = 7;
      chunk_size = MIN(resp_len - offset, LEDGER_HID_PACKET_SIZE - 7);
    } else {
      chunk_start = 5;
      chunk_size = MIN(resp_len - offset, LEDGER_HID_PACKET_SIZE - 5);
    }

    memcpy(output + offset, input + packet_offset + chunk_start, chunk_size);
    offset += chunk_size;
    packet_offset += LEDGER_HID_PACKET_SIZE;
    seq_idx++;
  }

  return offset;
}

/* Exchange APDU with device */
static gboolean
ledger_exchange(hid_device *handle, const guint8 *apdu, gsize apdu_len,
                guint8 *response, gsize *response_len, guint16 *sw, GError **error)
{
  guint16 channel_id = 0x0101;

  /* Wrap APDU for HID */
  guint8 wrapped[1024];
  gsize wrapped_len = ledger_wrap_apdu(wrapped, apdu, apdu_len, channel_id);

  /* Send packets */
  for (gsize i = 0; i < wrapped_len; i += LEDGER_HID_PACKET_SIZE) {
    int ret = hid_write(handle, wrapped + i, LEDGER_HID_PACKET_SIZE);
    if (ret < 0) {
      g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
                  "Failed to write to device");
      return FALSE;
    }
  }

  /* Read response */
  guint8 recv_buffer[1024];
  gsize recv_len = 0;

  /* Read packets until we have complete response */
  while (recv_len < sizeof(recv_buffer)) {
    guint8 packet[LEDGER_HID_PACKET_SIZE];
    int ret = hid_read_timeout(handle, packet, LEDGER_HID_PACKET_SIZE, 30000);
    if (ret < 0) {
      g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
                  "Failed to read from device");
      return FALSE;
    }
    if (ret == 0) {
      g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_TIMEOUT,
                  "Device timeout");
      return FALSE;
    }

    memcpy(recv_buffer + recv_len, packet, ret);
    recv_len += ret;

    /* Check if we have the complete response */
    if (recv_len >= LEDGER_HID_PACKET_SIZE) {
      guint16 expected_len = (recv_buffer[5] << 8) | recv_buffer[6];
      gsize packets_needed = (expected_len + 7 + LEDGER_HID_PACKET_SIZE - 1) / LEDGER_HID_PACKET_SIZE;
      if (recv_len >= packets_needed * LEDGER_HID_PACKET_SIZE)
        break;
    }
  }

  /* Unwrap response */
  guint8 unwrapped[512];
  gsize unwrapped_len = ledger_unwrap_response(recv_buffer, recv_len, unwrapped,
                                                sizeof(unwrapped), channel_id);

  if (unwrapped_len < 2) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
                "Invalid response from device");
    return FALSE;
  }

  /* Extract status word from end of response */
  *sw = (unwrapped[unwrapped_len - 2] << 8) | unwrapped[unwrapped_len - 1];
  *response_len = unwrapped_len - 2;
  if (*response_len > 0)
    memcpy(response, unwrapped, *response_len);

  return TRUE;
}

/* Parse derivation path string to bytes */
static gboolean
ledger_parse_path(const gchar *path, guint8 *output, gsize *output_len)
{
  /* Path format: m/44'/1237'/0'/0/0 */
  if (!path || path[0] != 'm')
    return FALSE;

  guint32 components[10];
  gsize count = 0;
  const gchar *p = path + 1;

  while (*p && count < 10) {
    if (*p == '/')
      p++;

    guint32 val = 0;
    gboolean hardened = FALSE;

    while (*p >= '0' && *p <= '9') {
      val = val * 10 + (*p - '0');
      p++;
    }

    if (*p == '\'' || *p == 'h' || *p == 'H') {
      hardened = TRUE;
      p++;
    }

    if (hardened)
      val |= 0x80000000;

    components[count++] = val;
  }

  /* Serialize path */
  output[0] = (guint8)count;
  gsize offset = 1;
  for (gsize i = 0; i < count; i++) {
    output[offset++] = (components[i] >> 24) & 0xFF;
    output[offset++] = (components[i] >> 16) & 0xFF;
    output[offset++] = (components[i] >> 8) & 0xFF;
    output[offset++] = components[i] & 0xFF;
  }

  *output_len = offset;
  return TRUE;
}

#endif /* GNOSTR_HAVE_HIDAPI */

/* ============================================================================
 * GnHwWalletProvider Interface Implementation
 * ============================================================================ */

static GnHwWalletType
ledger_get_device_type(GnHwWalletProvider *provider)
{
  (void)provider;
  /* Return generic Ledger type - specific type determined per-device */
  return GN_HW_WALLET_TYPE_LEDGER_NANO_S;
}

static GPtrArray *
ledger_enumerate_devices(GnHwWalletProvider *provider, GError **error)
{
  GnHwWalletLedgerProvider *self = GN_HW_WALLET_LEDGER_PROVIDER(provider);
  GPtrArray *devices = g_ptr_array_new_with_free_func(
    (GDestroyNotify)gn_hw_wallet_device_info_free);

#ifdef GNOSTR_HAVE_HIDAPI
  struct hid_device_info *devs = hid_enumerate(GN_HW_WALLET_LEDGER_VID, 0);
  struct hid_device_info *cur = devs;

  while (cur) {
    GnHwWalletType type = ledger_pid_to_type(cur->product_id);
    if (type != GN_HW_WALLET_TYPE_UNKNOWN) {
      GnHwWalletDeviceInfo *info = g_new0(GnHwWalletDeviceInfo, 1);
      info->device_id = g_strdup(cur->path);
      info->type = type;
      info->manufacturer = cur->manufacturer_string ?
        g_utf16_to_utf8((gunichar2 *)cur->manufacturer_string, -1, NULL, NULL, NULL) :
        g_strdup("Ledger");
      info->product = cur->product_string ?
        g_utf16_to_utf8((gunichar2 *)cur->product_string, -1, NULL, NULL, NULL) :
        g_strdup(gn_hw_wallet_type_to_string(type));
      info->serial = cur->serial_number ?
        g_utf16_to_utf8((gunichar2 *)cur->serial_number, -1, NULL, NULL, NULL) :
        NULL;
      info->state = GN_HW_WALLET_STATE_CONNECTED;
      info->needs_pin = TRUE;
      info->has_nostr_app = FALSE; /* Will be determined when opened */

      g_ptr_array_add(devices, info);
    }
    cur = cur->next;
  }

  hid_free_enumeration(devs);
#else
  (void)self;
  g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_UNSUPPORTED,
              "hidapi not available");
#endif

  return devices;
}

static gboolean
ledger_open_device(GnHwWalletProvider *provider, const gchar *device_id, GError **error)
{
  GnHwWalletLedgerProvider *self = GN_HW_WALLET_LEDGER_PROVIDER(provider);

#ifdef GNOSTR_HAVE_HIDAPI
  g_mutex_lock(&self->lock);

  /* Check if already open */
  if (g_hash_table_contains(self->open_devices, device_id)) {
    g_mutex_unlock(&self->lock);
    return TRUE;
  }

  hid_device *handle = hid_open_path(device_id);
  if (!handle) {
    g_mutex_unlock(&self->lock);
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
                "Failed to open device: %ls", hid_error(NULL));
    return FALSE;
  }

  /* Set non-blocking for timeouts to work */
  hid_set_nonblocking(handle, 0);

  LedgerDeviceHandle *dev = g_new0(LedgerDeviceHandle, 1);
  dev->device_id = g_strdup(device_id);
  dev->handle = handle;
  dev->state = GN_HW_WALLET_STATE_CONNECTED;

  /* Try to get app name to check if Nostr app is open */
  guint8 apdu[5];
  gsize apdu_len = ledger_build_apdu(apdu, LEDGER_NOSTR_CLA, LEDGER_NOSTR_INS_GET_APP_NAME,
                                     0, 0, NULL, 0);

  guint8 response[256];
  gsize response_len;
  guint16 sw;

  if (ledger_exchange(handle, apdu, apdu_len, response, &response_len, &sw, NULL)) {
    if (sw == LEDGER_SW_OK && response_len > 0) {
      dev->app_name = g_strndup((gchar *)response, response_len);
      if (g_str_has_prefix(dev->app_name, "Nostr") ||
          g_str_has_prefix(dev->app_name, "Bitcoin")) {
        dev->state = GN_HW_WALLET_STATE_READY;
      }
    } else if (sw == LEDGER_SW_APP_NOT_OPEN || sw == LEDGER_SW_CLA_NOT_SUPPORTED) {
      dev->state = GN_HW_WALLET_STATE_APP_CLOSED;
    } else if (sw == LEDGER_SW_LOCKED) {
      dev->state = GN_HW_WALLET_STATE_CONNECTED;
    }
  }

  g_hash_table_insert(self->open_devices, g_strdup(device_id), dev);
  g_mutex_unlock(&self->lock);

  return TRUE;
#else
  (void)self;
  (void)device_id;
  g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_UNSUPPORTED,
              "hidapi not available");
  return FALSE;
#endif
}

static void
ledger_close_device(GnHwWalletProvider *provider, const gchar *device_id)
{
  GnHwWalletLedgerProvider *self = GN_HW_WALLET_LEDGER_PROVIDER(provider);

  g_mutex_lock(&self->lock);
  g_hash_table_remove(self->open_devices, device_id);
  g_mutex_unlock(&self->lock);
}

static GnHwWalletState
ledger_get_device_state(GnHwWalletProvider *provider, const gchar *device_id)
{
  GnHwWalletLedgerProvider *self = GN_HW_WALLET_LEDGER_PROVIDER(provider);

  g_mutex_lock(&self->lock);
  LedgerDeviceHandle *dev = g_hash_table_lookup(self->open_devices, device_id);
  GnHwWalletState state = dev ? dev->state : GN_HW_WALLET_STATE_DISCONNECTED;
  g_mutex_unlock(&self->lock);

  return state;
}

static gboolean
ledger_get_public_key(GnHwWalletProvider *provider, const gchar *device_id,
                      const gchar *derivation_path, guint8 *pubkey_out,
                      gsize *pubkey_len, gboolean confirm_on_device, GError **error)
{
  GnHwWalletLedgerProvider *self = GN_HW_WALLET_LEDGER_PROVIDER(provider);

#ifdef GNOSTR_HAVE_HIDAPI
  g_mutex_lock(&self->lock);
  LedgerDeviceHandle *dev = g_hash_table_lookup(self->open_devices, device_id);
  if (!dev) {
    g_mutex_unlock(&self->lock);
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_DEVICE_NOT_FOUND,
                "Device not open");
    return FALSE;
  }

  /* Parse derivation path */
  guint8 path_data[64];
  gsize path_len;
  if (!ledger_parse_path(derivation_path, path_data, &path_len)) {
    g_mutex_unlock(&self->lock);
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_FAILED,
                "Invalid derivation path");
    return FALSE;
  }

  /* Build GET_PUBLIC_KEY APDU */
  guint8 apdu[256];
  gsize apdu_len = ledger_build_apdu(apdu, LEDGER_NOSTR_CLA, LEDGER_NOSTR_INS_GET_PUBLIC_KEY,
                                     confirm_on_device ? LEDGER_P1_CONFIRM_ON : LEDGER_P1_CONFIRM_OFF,
                                     LEDGER_P2_UNUSED, path_data, path_len);

  /* Exchange with device */
  guint8 response[256];
  gsize response_len;
  guint16 sw;

  gboolean ok = ledger_exchange(dev->handle, apdu, apdu_len, response, &response_len, &sw, error);
  g_mutex_unlock(&self->lock);

  if (!ok)
    return FALSE;

  if (sw == LEDGER_SW_USER_REJECTED) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_USER_REJECTED,
                "User rejected on device");
    return FALSE;
  }

  if (sw == LEDGER_SW_APP_NOT_OPEN || sw == LEDGER_SW_CLA_NOT_SUPPORTED) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_APP_NOT_OPEN,
                "Nostr app not open on device");
    return FALSE;
  }

  if (sw != LEDGER_SW_OK) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_FAILED,
                "Device returned error: 0x%04X", sw);
    return FALSE;
  }

  /* Response format: [pubkey_len][pubkey][optional chain code] */
  if (response_len < 33) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
                "Invalid public key response");
    return FALSE;
  }

  guint8 pk_len = response[0];
  if (pk_len > response_len - 1) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
                "Invalid public key length");
    return FALSE;
  }

  /* For Nostr, we want the x-only pubkey (32 bytes) */
  if (pk_len == 33) {
    /* Compressed pubkey - extract x coordinate */
    memcpy(pubkey_out, response + 2, 32);
    *pubkey_len = 32;
  } else if (pk_len == 32) {
    /* Already x-only */
    memcpy(pubkey_out, response + 1, 32);
    *pubkey_len = 32;
  } else {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
                "Unexpected public key format");
    return FALSE;
  }

  return TRUE;
#else
  (void)self;
  (void)device_id;
  (void)derivation_path;
  (void)pubkey_out;
  (void)pubkey_len;
  (void)confirm_on_device;
  g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_UNSUPPORTED,
              "hidapi not available");
  return FALSE;
#endif
}

static gboolean
ledger_sign_hash(GnHwWalletProvider *provider, const gchar *device_id,
                 const gchar *derivation_path, const guint8 *hash, gsize hash_len,
                 guint8 *signature_out, gsize *signature_len, GError **error)
{
  GnHwWalletLedgerProvider *self = GN_HW_WALLET_LEDGER_PROVIDER(provider);

  if (hash_len != 32) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_FAILED,
                "Hash must be 32 bytes");
    return FALSE;
  }

#ifdef GNOSTR_HAVE_HIDAPI
  g_mutex_lock(&self->lock);
  LedgerDeviceHandle *dev = g_hash_table_lookup(self->open_devices, device_id);
  if (!dev) {
    g_mutex_unlock(&self->lock);
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_DEVICE_NOT_FOUND,
                "Device not open");
    return FALSE;
  }

  /* Parse derivation path */
  guint8 path_data[64];
  gsize path_len;
  if (!ledger_parse_path(derivation_path, path_data, &path_len)) {
    g_mutex_unlock(&self->lock);
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_FAILED,
                "Invalid derivation path");
    return FALSE;
  }

  /* Build SIGN_HASH APDU: [path][hash] */
  guint8 sign_data[256];
  memcpy(sign_data, path_data, path_len);
  memcpy(sign_data + path_len, hash, hash_len);

  guint8 apdu[256];
  gsize apdu_len = ledger_build_apdu(apdu, LEDGER_NOSTR_CLA, LEDGER_NOSTR_INS_SIGN_HASH,
                                     LEDGER_P1_CONFIRM_ON, /* Always confirm signing */
                                     LEDGER_P2_UNUSED, sign_data, path_len + hash_len);

  /* Exchange with device */
  guint8 response[256];
  gsize response_len;
  guint16 sw;

  gboolean ok = ledger_exchange(dev->handle, apdu, apdu_len, response, &response_len, &sw, error);
  g_mutex_unlock(&self->lock);

  if (!ok)
    return FALSE;

  if (sw == LEDGER_SW_USER_REJECTED) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_USER_REJECTED,
                "User rejected signing on device");
    return FALSE;
  }

  if (sw == LEDGER_SW_APP_NOT_OPEN || sw == LEDGER_SW_CLA_NOT_SUPPORTED) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_APP_NOT_OPEN,
                "Nostr app not open on device");
    return FALSE;
  }

  if (sw != LEDGER_SW_OK) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_FAILED,
                "Device returned error: 0x%04X", sw);
    return FALSE;
  }

  /* Schnorr signature is 64 bytes */
  if (response_len < 64) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
                "Invalid signature response");
    return FALSE;
  }

  memcpy(signature_out, response, 64);
  *signature_len = 64;

  return TRUE;
#else
  (void)self;
  (void)device_id;
  (void)derivation_path;
  (void)hash;
  (void)signature_out;
  (void)signature_len;
  g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_UNSUPPORTED,
              "hidapi not available");
  return FALSE;
#endif
}

/* ============================================================================
 * GObject Implementation
 * ============================================================================ */

static void
gn_hw_wallet_ledger_provider_finalize(GObject *object)
{
  GnHwWalletLedgerProvider *self = GN_HW_WALLET_LEDGER_PROVIDER(object);

  g_mutex_lock(&self->lock);
  g_hash_table_unref(self->open_devices);
  g_mutex_unlock(&self->lock);
  g_mutex_clear(&self->lock);

  G_OBJECT_CLASS(gn_hw_wallet_ledger_provider_parent_class)->finalize(object);
}

static void
gn_hw_wallet_provider_iface_init(GnHwWalletProviderInterface *iface)
{
  iface->get_device_type = ledger_get_device_type;
  iface->enumerate_devices = ledger_enumerate_devices;
  iface->open_device = ledger_open_device;
  iface->close_device = ledger_close_device;
  iface->get_device_state = ledger_get_device_state;
  iface->get_public_key = ledger_get_public_key;
  iface->sign_hash = ledger_sign_hash;
}

static void
gn_hw_wallet_ledger_provider_class_init(GnHwWalletLedgerProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gn_hw_wallet_ledger_provider_finalize;
}

static void
gn_hw_wallet_ledger_provider_init(GnHwWalletLedgerProvider *self)
{
  g_mutex_init(&self->lock);
  self->open_devices = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, (GDestroyNotify)ledger_device_handle_free);
}

GnHwWalletProvider *
gn_hw_wallet_ledger_provider_new(void)
{
  return GN_HW_WALLET_PROVIDER(g_object_new(GN_TYPE_HW_WALLET_LEDGER_PROVIDER, NULL));
}
