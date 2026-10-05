/* hw_wallet_trezor.c - Trezor hardware wallet provider implementation
 *
 * Implements USB HID communication with Trezor devices using the wire protocol.
 *
 * SPDX-License-Identifier: MIT
 */
#include "hw_wallet_trezor.h"
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
  gchar *label;
  gchar *device_version;
  gboolean initialized;
  gboolean pin_protection;
  gboolean passphrase_protection;
} TrezorDeviceHandle;

struct _GnHwWalletTrezorProvider {
  GObject parent_instance;
  GHashTable *open_devices; /* device_id -> TrezorDeviceHandle */
  GMutex lock;
};

static void gn_hw_wallet_provider_iface_init(GnHwWalletProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnHwWalletTrezorProvider, gn_hw_wallet_trezor_provider, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GN_TYPE_HW_WALLET_PROVIDER,
                                              gn_hw_wallet_provider_iface_init))

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

static void
trezor_device_handle_free(TrezorDeviceHandle *handle)
{
  if (!handle)
    return;
#ifdef GNOSTR_HAVE_HIDAPI
  if (handle->handle)
    hid_close(handle->handle);
#endif
  g_free(handle->device_id);
  g_free(handle->label);
  g_free(handle->device_version);
  g_free(handle);
}

/* Determine Trezor device type from USB product ID */
static GnHwWalletType
trezor_pid_to_type(unsigned short vid, unsigned short pid)
{
  if (vid == GN_HW_WALLET_TREZOR_VID) {
    switch (pid) {
      case GN_HW_WALLET_TREZOR_ONE_PID:
        return GN_HW_WALLET_TYPE_TREZOR_ONE;
      case GN_HW_WALLET_TREZOR_T_PID:
        return GN_HW_WALLET_TYPE_TREZOR_T;
      /* Add more PIDs as needed */
    }
  }
  return GN_HW_WALLET_TYPE_UNKNOWN;
}

#ifdef GNOSTR_HAVE_HIDAPI

/* ============================================================================
 * Protobuf Wire Format Helpers
 *
 * Minimal protobuf decoder for Trezor response messages. Handles varint
 * encoding for tags and lengths, with bounds checking throughout.
 *
 * Protobuf wire types:
 *   0 = varint, 1 = 64-bit fixed, 2 = length-delimited, 5 = 32-bit fixed
 *
 * Field numbers for Trezor messages (from trezor-common/protob/):
 *   Features:        vendor=1, major=2, minor=3, patch=4, device_id=6,
 *                    pin_protection=7, passphrase_protection=8, label=10,
 *                    initialized=12
 *   PublicKey:       node=1, xpub=2
 *   HDNodeType:      depth=1, fingerprint=2, child_num=3, chain_code=4,
 *                    private_key=5, public_key=6
 *   MessageSignature: address=1, signature=2
 * ============================================================================ */

/**
 * Read a protobuf varint from buf[*pos]. Advances *pos past the varint.
 * Returns FALSE if the buffer is exhausted or the varint exceeds 64 bits.
 */
static gboolean
pb_read_varint(const guint8 *buf, gsize buf_len, gsize *pos, guint64 *out)
{
  guint64 val = 0;
  guint shift = 0;

  while (*pos < buf_len) {
    guint8 b = buf[(*pos)++];
    val |= (guint64)(b & 0x7F) << shift;
    if ((b & 0x80) == 0) {
      if (out) *out = val;
      return TRUE;
    }
    shift += 7;
    if (shift >= 64) return FALSE; /* overflow */
  }
  return FALSE; /* truncated */
}

/**
 * Read a protobuf field tag and decode field number + wire type.
 */
static gboolean
pb_read_tag(const guint8 *buf, gsize buf_len, gsize *pos,
            guint32 *field_num, guint8 *wire_type)
{
  guint64 tag;
  if (!pb_read_varint(buf, buf_len, pos, &tag))
    return FALSE;
  if (field_num) *field_num = (guint32)(tag >> 3);
  if (wire_type) *wire_type = (guint8)(tag & 0x07);
  return TRUE;
}

/**
 * Skip a protobuf field value based on its wire type. Advances *pos.
 */
static gboolean
pb_skip_field(const guint8 *buf, gsize buf_len, gsize *pos, guint8 wire_type)
{
  switch (wire_type) {
    case 0: { /* varint */
      guint64 dummy;
      return pb_read_varint(buf, buf_len, pos, &dummy);
    }
    case 1: /* 64-bit fixed */
      if (*pos + 8 > buf_len) return FALSE;
      *pos += 8;
      return TRUE;
    case 2: { /* length-delimited */
      guint64 len;
      if (!pb_read_varint(buf, buf_len, pos, &len)) return FALSE;
      if (*pos + len > buf_len) return FALSE;
      *pos += (gsize)len;
      return TRUE;
    }
    case 5: /* 32-bit fixed */
      if (*pos + 4 > buf_len) return FALSE;
      *pos += 4;
      return TRUE;
    default:
      return FALSE; /* unknown wire type */
  }
}

/**
 * Read a length-delimited field value. Returns pointer into buf and length.
 * Does NOT advance *pos past the data (caller decides how to process).
 */
static gboolean
pb_read_bytes(const guint8 *buf, gsize buf_len, gsize *pos,
              const guint8 **out_data, gsize *out_len)
{
  guint64 len;
  if (!pb_read_varint(buf, buf_len, pos, &len)) return FALSE;
  if (*pos + len > buf_len) return FALSE;
  if (out_data) *out_data = buf + *pos;
  if (out_len) *out_len = (gsize)len;
  *pos += (gsize)len;
  return TRUE;
}

/* Build Trezor wire protocol message */
static gsize
trezor_build_message(guint8 *buffer, guint16 msg_type, const guint8 *data, gsize data_len)
{
  /* Trezor message format:
   * Byte 0: '?' (0x3F) or '#' (0x23) - Report ID / Magic
   * Byte 1-2: Message type (big endian)
   * Byte 3-6: Data length (big endian)
   * Byte 7+: Data
   */
  gsize offset = 0;

  /* First packet header */
  buffer[offset++] = TREZOR_MAGIC_V2;
  buffer[offset++] = TREZOR_MAGIC_V2;
  buffer[offset++] = (msg_type >> 8) & 0xFF;
  buffer[offset++] = msg_type & 0xFF;
  buffer[offset++] = (data_len >> 24) & 0xFF;
  buffer[offset++] = (data_len >> 16) & 0xFF;
  buffer[offset++] = (data_len >> 8) & 0xFF;
  buffer[offset++] = data_len & 0xFF;

  if (data && data_len > 0) {
    gsize copy_len = MIN(data_len, TREZOR_HID_PACKET_SIZE - 8);
    memcpy(buffer + offset, data, copy_len);
    offset += copy_len;
  }

  /* Pad to packet size */
  while (offset < TREZOR_HID_PACKET_SIZE) {
    buffer[offset++] = 0;
  }

  return offset;
}

/* Send and receive Trezor message */
static gboolean
trezor_exchange(hid_device *handle, guint16 send_type, const guint8 *send_data,
                gsize send_len, guint16 *recv_type, guint8 *recv_data,
                gsize *recv_len, gsize recv_max, GError **error)
{
  /* Build and send message */
  guint8 send_packet[TREZOR_HID_PACKET_SIZE];
  memset(send_packet, 0, sizeof(send_packet));

  /* Trezor expects report ID as first byte */
  send_packet[0] = '?';

  /* Build header after report ID */
  send_packet[1] = '#';
  send_packet[2] = '#';
  send_packet[3] = (send_type >> 8) & 0xFF;
  send_packet[4] = send_type & 0xFF;
  send_packet[5] = (send_len >> 24) & 0xFF;
  send_packet[6] = (send_len >> 16) & 0xFF;
  send_packet[7] = (send_len >> 8) & 0xFF;
  send_packet[8] = send_len & 0xFF;

  gsize offset = 9;
  gsize data_offset = 0;

  /* Copy initial data */
  if (send_data && send_len > 0) {
    gsize copy_len = MIN(send_len, TREZOR_HID_PACKET_SIZE - 9);
    memcpy(send_packet + offset, send_data, copy_len);
    data_offset = copy_len;
  }

  /* Send first packet */
  int ret = hid_write(handle, send_packet, TREZOR_HID_PACKET_SIZE);
  if (ret < 0) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
                "Failed to write to device");
    return FALSE;
  }

  /* Send continuation packets if needed */
  while (data_offset < send_len) {
    guint8 cont_packet[TREZOR_HID_PACKET_SIZE];
    memset(cont_packet, 0, sizeof(cont_packet));
    cont_packet[0] = '?';

    gsize copy_len = MIN(send_len - data_offset, TREZOR_HID_PACKET_SIZE - 1);
    memcpy(cont_packet + 1, send_data + data_offset, copy_len);
    data_offset += copy_len;

    ret = hid_write(handle, cont_packet, TREZOR_HID_PACKET_SIZE);
    if (ret < 0) {
      g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
                  "Failed to write continuation packet");
      return FALSE;
    }
  }

  /* Read response */
  guint8 recv_packet[TREZOR_HID_PACKET_SIZE];
  ret = hid_read_timeout(handle, recv_packet, TREZOR_HID_PACKET_SIZE, 60000);
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

  /* Parse response header */
  if (recv_packet[0] != '#' || recv_packet[1] != '#') {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
                "Invalid response magic");
    return FALSE;
  }

  *recv_type = (recv_packet[2] << 8) | recv_packet[3];
  guint32 total_len = (recv_packet[4] << 24) | (recv_packet[5] << 16) |
                      (recv_packet[6] << 8) | recv_packet[7];

  if (total_len > recv_max) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
                "Response too large");
    return FALSE;
  }

  /* Copy initial data */
  gsize copy_offset = 0;
  gsize initial_data_len = MIN(total_len, TREZOR_HID_PACKET_SIZE - 8);
  if (recv_data && initial_data_len > 0) {
    memcpy(recv_data, recv_packet + 8, initial_data_len);
    copy_offset = initial_data_len;
  }

  /* Read continuation packets */
  while (copy_offset < total_len) {
    ret = hid_read_timeout(handle, recv_packet, TREZOR_HID_PACKET_SIZE, 30000);
    if (ret <= 0) {
      g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
                  "Failed to read continuation packet");
      return FALSE;
    }

    gsize copy_len = MIN(total_len - copy_offset, TREZOR_HID_PACKET_SIZE - 1);
    if (recv_data)
      memcpy(recv_data + copy_offset, recv_packet + 1, copy_len);
    copy_offset += copy_len;
  }

  *recv_len = total_len;
  return TRUE;
}

/* Handle button request from device */
static gboolean
trezor_handle_button_request(hid_device *handle, guint16 *recv_type,
                              guint8 *recv_data, gsize *recv_len, gsize recv_max,
                              GError **error)
{
  /* Send ButtonAck */
  guint16 ack_type;
  if (!trezor_exchange(handle, TREZOR_MSG_BUTTON_ACK, NULL, 0,
                       &ack_type, recv_data, recv_len, recv_max, error)) {
    return FALSE;
  }

  *recv_type = ack_type;
  return TRUE;
}

/* Encode derivation path for Trezor protobuf */
static gsize
trezor_encode_path(const gchar *path, guint8 *output)
{
  /* Path format: m/44'/1237'/0'/0/0 -> array of uint32 */
  if (!path || path[0] != 'm')
    return 0;

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

  /* Simple protobuf-like encoding (field 1, repeated uint32) */
  gsize offset = 0;
  for (gsize i = 0; i < count; i++) {
    /* Field tag: (1 << 3) | 0 = 8 for packed, or 8 for each element */
    output[offset++] = 0x08;  /* Field 1, varint */

    /* Varint encoding */
    guint32 v = components[i];
    while (v >= 0x80) {
      output[offset++] = (v & 0x7F) | 0x80;
      v >>= 7;
    }
    output[offset++] = v;
  }

  return offset;
}

#endif /* GNOSTR_HAVE_HIDAPI */

/* ============================================================================
 * GnHwWalletProvider Interface Implementation
 * ============================================================================ */

static GnHwWalletType
trezor_get_device_type(GnHwWalletProvider *provider)
{
  (void)provider;
  return GN_HW_WALLET_TYPE_TREZOR_ONE;
}

static GPtrArray *
trezor_enumerate_devices(GnHwWalletProvider *provider, GError **error)
{
  GnHwWalletTrezorProvider *self = GN_HW_WALLET_TREZOR_PROVIDER(provider);
  GPtrArray *devices = g_ptr_array_new_with_free_func(
    (GDestroyNotify)gn_hw_wallet_device_info_free);

#ifdef GNOSTR_HAVE_HIDAPI
  /* Enumerate Trezor devices */
  struct hid_device_info *devs = hid_enumerate(GN_HW_WALLET_TREZOR_VID, 0);
  struct hid_device_info *cur = devs;

  while (cur) {
    GnHwWalletType type = trezor_pid_to_type(cur->vendor_id, cur->product_id);
    if (type != GN_HW_WALLET_TYPE_UNKNOWN) {
      /* Trezor uses interface 0 for the wire protocol */
      if (cur->interface_number == 0 || cur->interface_number == -1) {
        GnHwWalletDeviceInfo *info = g_new0(GnHwWalletDeviceInfo, 1);
        info->device_id = g_strdup(cur->path);
        info->type = type;
        info->manufacturer = cur->manufacturer_string ?
          g_utf16_to_utf8((gunichar2 *)cur->manufacturer_string, -1, NULL, NULL, NULL) :
          g_strdup("SatoshiLabs");
        info->product = cur->product_string ?
          g_utf16_to_utf8((gunichar2 *)cur->product_string, -1, NULL, NULL, NULL) :
          g_strdup(gn_hw_wallet_type_to_string(type));
        info->serial = cur->serial_number ?
          g_utf16_to_utf8((gunichar2 *)cur->serial_number, -1, NULL, NULL, NULL) :
          NULL;
        info->state = GN_HW_WALLET_STATE_CONNECTED;
        info->needs_pin = TRUE;
        info->has_nostr_app = TRUE; /* Trezor doesn't have separate apps */

        g_ptr_array_add(devices, info);
      }
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
trezor_open_device(GnHwWalletProvider *provider, const gchar *device_id, GError **error)
{
  GnHwWalletTrezorProvider *self = GN_HW_WALLET_TREZOR_PROVIDER(provider);

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

  TrezorDeviceHandle *dev = g_new0(TrezorDeviceHandle, 1);
  dev->device_id = g_strdup(device_id);
  dev->handle = handle;
  dev->state = GN_HW_WALLET_STATE_CONNECTED;

  /* Send Initialize to get device features */
  guint16 recv_type;
  guint8 recv_data[1024];
  gsize recv_len;

  if (trezor_exchange(handle, TREZOR_MSG_INITIALIZE, NULL, 0,
                      &recv_type, recv_data, &recv_len, sizeof(recv_data), NULL)) {
    if (recv_type == TREZOR_MSG_FEATURES) {
      dev->initialized = TRUE;
      dev->state = GN_HW_WALLET_STATE_READY;

      /* Parse Features protobuf to extract device metadata */
      guint32 major_ver = 0, minor_ver = 0, patch_ver = 0;
      gsize fpos = 0;
      while (fpos < recv_len) {
        guint32 fnum;
        guint8 fwire;
        if (!pb_read_tag(recv_data, recv_len, &fpos, &fnum, &fwire))
          break;

        if (fwire == 0) { /* varint fields */
          guint64 val;
          if (!pb_read_varint(recv_data, recv_len, &fpos, &val))
            break;
          switch (fnum) {
            case 2: major_ver = (guint32)val; break;
            case 3: minor_ver = (guint32)val; break;
            case 4: patch_ver = (guint32)val; break;
            case 7: dev->pin_protection = (val != 0); break;
            case 8: dev->passphrase_protection = (val != 0); break;
            case 12: dev->initialized = (val != 0); break;
            default: break;
          }
        } else if (fwire == 2) { /* length-delimited fields */
          const guint8 *data;
          gsize data_len;
          if (!pb_read_bytes(recv_data, recv_len, &fpos, &data, &data_len))
            break;
          switch (fnum) {
            case 10: /* label */
              g_free(dev->label);
              dev->label = g_strndup((const gchar *)data, data_len);
              break;
            default: break;
          }
        } else {
          if (!pb_skip_field(recv_data, recv_len, &fpos, fwire))
            break;
        }
      }

      g_free(dev->device_version);
      dev->device_version = g_strdup_printf("%u.%u.%u",
                                             major_ver, minor_ver, patch_ver);
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
trezor_close_device(GnHwWalletProvider *provider, const gchar *device_id)
{
  GnHwWalletTrezorProvider *self = GN_HW_WALLET_TREZOR_PROVIDER(provider);

  g_mutex_lock(&self->lock);
  g_hash_table_remove(self->open_devices, device_id);
  g_mutex_unlock(&self->lock);
}

static GnHwWalletState
trezor_get_device_state(GnHwWalletProvider *provider, const gchar *device_id)
{
  GnHwWalletTrezorProvider *self = GN_HW_WALLET_TREZOR_PROVIDER(provider);

  g_mutex_lock(&self->lock);
  TrezorDeviceHandle *dev = g_hash_table_lookup(self->open_devices, device_id);
  GnHwWalletState state = dev ? dev->state : GN_HW_WALLET_STATE_DISCONNECTED;
  g_mutex_unlock(&self->lock);

  return state;
}

static gboolean
trezor_get_public_key(GnHwWalletProvider *provider, const gchar *device_id,
                      const gchar *derivation_path, guint8 *pubkey_out,
                      gsize *pubkey_len, gboolean confirm_on_device, GError **error)
{
  GnHwWalletTrezorProvider *self = GN_HW_WALLET_TREZOR_PROVIDER(provider);

#ifdef GNOSTR_HAVE_HIDAPI
  g_mutex_lock(&self->lock);
  TrezorDeviceHandle *dev = g_hash_table_lookup(self->open_devices, device_id);
  if (!dev) {
    g_mutex_unlock(&self->lock);
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_DEVICE_NOT_FOUND,
                "Device not open");
    return FALSE;
  }

  /* Build GetPublicKey message */
  guint8 msg_data[256];
  gsize msg_len = 0;

  /* Encode path */
  gsize path_len = trezor_encode_path(derivation_path, msg_data);
  msg_len = path_len;

  /* Add curve name field (field 2, string) */
  const char *curve = TREZOR_CURVE_SECP256K1;
  gsize curve_len = strlen(curve);
  msg_data[msg_len++] = 0x12; /* Field 2, length-delimited */
  msg_data[msg_len++] = (guint8)curve_len;
  memcpy(msg_data + msg_len, curve, curve_len);
  msg_len += curve_len;

  /* Add show_display field if needed (field 3, bool) */
  if (confirm_on_device) {
    msg_data[msg_len++] = 0x18; /* Field 3, varint */
    msg_data[msg_len++] = 0x01; /* true */
  }

  /* Exchange with device */
  guint16 recv_type;
  guint8 recv_data[256];
  gsize recv_len;

  gboolean ok = trezor_exchange(dev->handle, TREZOR_MSG_GET_PUBLIC_KEY,
                                 msg_data, msg_len, &recv_type, recv_data,
                                 &recv_len, sizeof(recv_data), error);

  /* Handle button request if needed */
  while (ok && recv_type == TREZOR_MSG_BUTTON_REQUEST) {
    dev->state = GN_HW_WALLET_STATE_BUSY;
    ok = trezor_handle_button_request(dev->handle, &recv_type, recv_data,
                                       &recv_len, sizeof(recv_data), error);
  }

  dev->state = GN_HW_WALLET_STATE_READY;
  g_mutex_unlock(&self->lock);

  if (!ok)
    return FALSE;

  if (recv_type == TREZOR_MSG_FAILURE) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_USER_REJECTED,
                "Operation failed or rejected");
    return FALSE;
  }

  if (recv_type != TREZOR_MSG_PUBLIC_KEY) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
                "Unexpected response type: %d", recv_type);
    return FALSE;
  }

  /* Parse PublicKey response: field 1 (HDNodeType node) → field 6 (public_key) */
  gsize i = 0;
  while (i < recv_len) {
    guint32 field_num;
    guint8 wire_type;
    if (!pb_read_tag(recv_data, recv_len, &i, &field_num, &wire_type))
      break;

    if (wire_type == 2 && field_num == 1) {
        /* Field 1 = HDNodeType (submessage). Read its bytes, then parse inside. */
        const guint8 *node_data;
        gsize node_len;
      if (!pb_read_bytes(recv_data, recv_len, &i, &node_data, &node_len))
        break;

      /* Walk inside the HDNodeType submessage for field 6 (public_key) */
      gsize ni = 0;
      while (ni < node_len) {
        guint32 nfield;
        guint8 nwire;
        if (!pb_read_tag(node_data, node_len, &ni, &nfield, &nwire))
          break;

        if (nwire == 2 && nfield == 6) {
          /* Field 6 = public_key (bytes, 33-byte compressed secp256k1) */
          const guint8 *pk_data;
          gsize pk_len;
          if (!pb_read_bytes(node_data, node_len, &ni, &pk_data, &pk_len))
            break;
          if (pk_len == 33) {
            /* Extract x-only public key (skip the 02/03 prefix byte) */
            memcpy(pubkey_out, pk_data + 1, 32);
            *pubkey_len = 32;
            return TRUE;
          }
        } else {
          if (!pb_skip_field(node_data, node_len, &ni, nwire))
            break;
        }
      }
    } else {
      if (!pb_skip_field(recv_data, recv_len, &i, wire_type))
        break;
    }
  }

  g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
              "Failed to parse public key response");
  return FALSE;
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
trezor_sign_hash(GnHwWalletProvider *provider, const gchar *device_id,
                 const gchar *derivation_path, const guint8 *hash, gsize hash_len,
                 guint8 *signature_out, gsize *signature_len, GError **error)
{
  GnHwWalletTrezorProvider *self = GN_HW_WALLET_TREZOR_PROVIDER(provider);

  if (hash_len != 32) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_FAILED,
                "Hash must be 32 bytes");
    return FALSE;
  }

#ifdef GNOSTR_HAVE_HIDAPI
  g_mutex_lock(&self->lock);
  TrezorDeviceHandle *dev = g_hash_table_lookup(self->open_devices, device_id);
  if (!dev) {
    g_mutex_unlock(&self->lock);
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_DEVICE_NOT_FOUND,
                "Device not open");
    return FALSE;
  }

  /* Build SignMessage message (using message signing for Schnorr) */
  guint8 msg_data[256];
  gsize msg_len = 0;

  /* Encode path (field 1) */
  gsize path_len = trezor_encode_path(derivation_path, msg_data);
  msg_len = path_len;

  /* Add message field (field 2, bytes) - the hash to sign */
  msg_data[msg_len++] = 0x12; /* Field 2, length-delimited */
  msg_data[msg_len++] = (guint8)hash_len;
  memcpy(msg_data + msg_len, hash, hash_len);
  msg_len += hash_len;

  /* Add coin_name field (field 3, string) */
  const char *coin = "Nostr";
  gsize coin_len = strlen(coin);
  msg_data[msg_len++] = 0x1A; /* Field 3, length-delimited */
  msg_data[msg_len++] = (guint8)coin_len;
  memcpy(msg_data + msg_len, coin, coin_len);
  msg_len += coin_len;

  /* Exchange with device */
  guint16 recv_type;
  guint8 recv_data[256];
  gsize recv_len;

  gboolean ok = trezor_exchange(dev->handle, TREZOR_MSG_SIGN_MESSAGE,
                                 msg_data, msg_len, &recv_type, recv_data,
                                 &recv_len, sizeof(recv_data), error);

  /* Handle button requests */
  while (ok && recv_type == TREZOR_MSG_BUTTON_REQUEST) {
    dev->state = GN_HW_WALLET_STATE_BUSY;
    ok = trezor_handle_button_request(dev->handle, &recv_type, recv_data,
                                       &recv_len, sizeof(recv_data), error);
  }

  dev->state = GN_HW_WALLET_STATE_READY;
  g_mutex_unlock(&self->lock);

  if (!ok)
    return FALSE;

  if (recv_type == TREZOR_MSG_FAILURE) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_USER_REJECTED,
                "Signing rejected or failed");
    return FALSE;
  }

  if (recv_type != TREZOR_MSG_MESSAGE_SIGNATURE) {
    g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
                "Unexpected response type: %d", recv_type);
    return FALSE;
  }

  /* Parse MessageSignature response: field 2 (signature, bytes) */
  gsize i = 0;
  while (i < recv_len) {
    guint32 field_num;
    guint8 wire_type;
    if (!pb_read_tag(recv_data, recv_len, &i, &field_num, &wire_type))
      break;

    if (wire_type == 2 && field_num == 2) {
      /* Field 2 = signature (64 bytes for Schnorr) */
      const guint8 *sig_data;
      gsize sig_len;
      if (!pb_read_bytes(recv_data, recv_len, &i, &sig_data, &sig_len))
        break;
      if (sig_len >= 64) {
        memcpy(signature_out, sig_data, 64);
        *signature_len = 64;
        return TRUE;
      }
    } else {
      if (!pb_skip_field(recv_data, recv_len, &i, wire_type))
        break;
    }
  }

  g_set_error(error, GN_HW_WALLET_ERROR, GN_HW_WALLET_ERROR_COMMUNICATION,
              "Failed to parse signature response");
  return FALSE;
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
gn_hw_wallet_trezor_provider_finalize(GObject *object)
{
  GnHwWalletTrezorProvider *self = GN_HW_WALLET_TREZOR_PROVIDER(object);

  g_mutex_lock(&self->lock);
  g_clear_pointer(&self->open_devices, g_hash_table_unref);
  g_mutex_unlock(&self->lock);
  g_mutex_clear(&self->lock);

  G_OBJECT_CLASS(gn_hw_wallet_trezor_provider_parent_class)->finalize(object);
}

static void
gn_hw_wallet_provider_iface_init(GnHwWalletProviderInterface *iface)
{
  iface->get_device_type = trezor_get_device_type;
  iface->enumerate_devices = trezor_enumerate_devices;
  iface->open_device = trezor_open_device;
  iface->close_device = trezor_close_device;
  iface->get_device_state = trezor_get_device_state;
  iface->get_public_key = trezor_get_public_key;
  iface->sign_hash = trezor_sign_hash;
}

static void
gn_hw_wallet_trezor_provider_class_init(GnHwWalletTrezorProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gn_hw_wallet_trezor_provider_finalize;
}

static void
gn_hw_wallet_trezor_provider_init(GnHwWalletTrezorProvider *self)
{
  g_mutex_init(&self->lock);
  self->open_devices = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, (GDestroyNotify)trezor_device_handle_free);
}

GnHwWalletProvider *
gn_hw_wallet_trezor_provider_new(void)
{
  return GN_HW_WALLET_PROVIDER(g_object_new(GN_TYPE_HW_WALLET_TREZOR_PROVIDER, NULL));
}
