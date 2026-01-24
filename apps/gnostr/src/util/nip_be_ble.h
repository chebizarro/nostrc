/**
 * @file nip_be_ble.h
 * @brief NIP-BE (190) BLE Communications Utilities
 *
 * NIP-BE defines kind 190 (0xBE) events for Bluetooth Low Energy communication.
 * This module provides utilities for parsing and creating BLE message metadata
 * from event tags.
 *
 * This NIP enables offline/local Nostr communication via BLE:
 * - Devices broadcast their npub
 * - Messages can be exchanged locally
 * - Useful for mesh networking scenarios
 *
 * Required tags:
 * - "ble-id" - BLE device identifier (UUID)
 *
 * Optional tags:
 * - "service" - BLE service UUID
 * - "characteristic" - BLE characteristic UUID
 * - "mtu" - Negotiated MTU size in bytes
 * - "p" - Target recipient pubkey
 * - "e" - Related event ID
 *
 * Event content field contains the message payload.
 */

#ifndef GNOSTR_NIP_BE_BLE_H
#define GNOSTR_NIP_BE_BLE_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Kind number for BLE communication events (0xBE hex = 190 decimal) */
#define NIPBE_KIND_BLE 190

/* Standard Nostr BLE Service UUID (randomly generated, stable) */
#define GNOSTR_BLE_SERVICE_UUID "a1b2c3d4-e5f6-7890-abcd-ef1234567890"

/* Standard Nostr BLE Characteristic UUIDs */
#define GNOSTR_BLE_CHAR_NPUB_UUID    "a1b2c3d4-e5f6-7890-abcd-ef1234567891"
#define GNOSTR_BLE_CHAR_MESSAGE_UUID "a1b2c3d4-e5f6-7890-abcd-ef1234567892"
#define GNOSTR_BLE_CHAR_EVENT_UUID   "a1b2c3d4-e5f6-7890-abcd-ef1234567893"

/* Common BLE MTU sizes */
#define GNOSTR_BLE_MTU_DEFAULT  23   /* BLE 4.0 default ATT MTU */
#define GNOSTR_BLE_MTU_EXTENDED 185  /* Common extended MTU */
#define GNOSTR_BLE_MTU_MAX      512  /* Maximum MTU per BLE 4.2+ spec */

/**
 * GnostrBleMessage:
 * Structure containing parsed NIP-BE BLE message metadata.
 * All strings are owned by the structure and freed with gnostr_ble_message_free().
 */
typedef struct {
  gchar *device_uuid;     /* BLE device identifier ("ble-id" tag, required) */
  gchar *service_uuid;    /* BLE service UUID ("service" tag) */
  gchar *char_uuid;       /* BLE characteristic UUID ("characteristic" tag) */
  gint mtu;               /* Negotiated MTU size ("mtu" tag, 0 if not specified) */
  gchar *content;         /* Message payload (event content field) */
  gchar *recipient;       /* Target recipient pubkey ("p" tag) */
  gchar *related_event;   /* Related event ID ("e" tag) */
  gint64 created_at;      /* Event creation timestamp */
} GnostrBleMessage;

/**
 * gnostr_ble_message_new:
 *
 * Creates a new empty BLE message metadata structure.
 * Use gnostr_ble_message_free() to free.
 *
 * Returns: (transfer full): New BLE message metadata.
 */
GnostrBleMessage *gnostr_ble_message_new(void);

/**
 * gnostr_ble_message_free:
 * @msg: The BLE message to free, may be NULL.
 *
 * Frees a BLE message structure and all its contents.
 */
void gnostr_ble_message_free(GnostrBleMessage *msg);

/**
 * gnostr_ble_message_copy:
 * @msg: The BLE message to copy.
 *
 * Creates a deep copy of BLE message metadata.
 *
 * Returns: (transfer full) (nullable): Copy of the metadata or NULL on error.
 */
GnostrBleMessage *gnostr_ble_message_copy(const GnostrBleMessage *msg);

/**
 * gnostr_ble_message_parse:
 * @tags_json: JSON array string containing event tags.
 * @content: Event content (message payload), may be NULL.
 *
 * Parses NIP-BE specific tags from an event's tags array.
 * The tags_json should be the JSON representation of the tags array.
 *
 * Returns: (transfer full) (nullable): Parsed metadata or NULL on error.
 */
GnostrBleMessage *gnostr_ble_message_parse(const char *tags_json,
                                            const char *content);

/**
 * gnostr_ble_message_build_tags:
 * @msg: BLE message metadata.
 *
 * Creates a JSON array string of tags for a BLE message event.
 * Useful when creating new BLE message events.
 *
 * Returns: (transfer full) (nullable): JSON array string or NULL on error.
 */
char *gnostr_ble_message_build_tags(const GnostrBleMessage *msg);

/**
 * gnostr_ble_is_ble:
 * @kind: Event kind number.
 *
 * Checks if an event kind is a BLE message (kind 190).
 *
 * Returns: TRUE if kind is a BLE message event.
 */
gboolean gnostr_ble_is_ble(int kind);

/**
 * gnostr_ble_validate_uuid:
 * @uuid: UUID string to validate.
 *
 * Validates that a string is a valid UUID format.
 * Accepts both 8-4-4-4-12 format and 32-char hex format.
 *
 * Returns: TRUE if the UUID appears valid.
 */
gboolean gnostr_ble_validate_uuid(const char *uuid);

/**
 * gnostr_ble_normalize_uuid:
 * @uuid: UUID string to normalize.
 *
 * Normalizes a UUID to lowercase 8-4-4-4-12 format.
 * Handles 32-char hex input and mixed-case input.
 *
 * Returns: (transfer full) (nullable): Normalized UUID or NULL on error.
 *          Caller must free with g_free().
 */
gchar *gnostr_ble_normalize_uuid(const char *uuid);

/**
 * gnostr_ble_validate_mtu:
 * @mtu: MTU value to validate.
 *
 * Validates that an MTU value is within the valid BLE range.
 * Valid range is 23 (BLE 4.0 minimum) to 512 (BLE 4.2+ maximum).
 *
 * Returns: TRUE if the MTU is valid.
 */
gboolean gnostr_ble_validate_mtu(gint mtu);

/**
 * gnostr_ble_get_max_payload:
 * @mtu: Negotiated MTU size.
 *
 * Calculates the maximum payload size for a given MTU.
 * Accounts for ATT protocol overhead (3 bytes).
 *
 * Returns: Maximum payload size in bytes.
 */
gint gnostr_ble_get_max_payload(gint mtu);

/**
 * gnostr_ble_get_kind:
 *
 * Gets the NIP-BE BLE message event kind number.
 *
 * Returns: The BLE message kind (190).
 */
int gnostr_ble_get_kind(void);

/**
 * gnostr_ble_get_service_uuid:
 *
 * Gets the standard Nostr BLE service UUID.
 *
 * Returns: Static string containing the service UUID (do not free).
 */
const char *gnostr_ble_get_service_uuid(void);

/**
 * gnostr_ble_get_npub_char_uuid:
 *
 * Gets the standard Nostr BLE npub characteristic UUID.
 *
 * Returns: Static string containing the characteristic UUID (do not free).
 */
const char *gnostr_ble_get_npub_char_uuid(void);

/**
 * gnostr_ble_get_message_char_uuid:
 *
 * Gets the standard Nostr BLE message characteristic UUID.
 *
 * Returns: Static string containing the characteristic UUID (do not free).
 */
const char *gnostr_ble_get_message_char_uuid(void);

/**
 * gnostr_ble_get_event_char_uuid:
 *
 * Gets the standard Nostr BLE event characteristic UUID.
 *
 * Returns: Static string containing the characteristic UUID (do not free).
 */
const char *gnostr_ble_get_event_char_uuid(void);

G_END_DECLS

#endif /* GNOSTR_NIP_BE_BLE_H */
