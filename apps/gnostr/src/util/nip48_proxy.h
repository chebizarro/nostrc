/**
 * @file nip48_proxy.h
 * @brief NIP-48 Proxy Tags - Bridged content from external protocols
 *
 * NIP-48 defines proxy tags for content bridged from other protocols:
 * - Format: ["proxy", "<id>", "<protocol>"]
 * - Protocols: activitypub, atproto, rss, web, etc.
 * - Links Nostr events to their original source on other platforms
 *
 * This module parses proxy tags and provides display helpers for
 * showing bridged content attribution in the UI.
 */

#ifndef GNOSTR_NIP48_PROXY_H
#define GNOSTR_NIP48_PROXY_H

#include <glib.h>
#include <gtk/gtk.h>
#include <stdbool.h>

G_BEGIN_DECLS

/**
 * Known proxy protocol types
 */
typedef enum {
  GNOSTR_PROXY_PROTOCOL_UNKNOWN = 0,
  GNOSTR_PROXY_PROTOCOL_ACTIVITYPUB,  /* Mastodon, Pleroma, etc. */
  GNOSTR_PROXY_PROTOCOL_ATPROTO,      /* Bluesky AT Protocol */
  GNOSTR_PROXY_PROTOCOL_RSS,          /* RSS/Atom feeds */
  GNOSTR_PROXY_PROTOCOL_WEB,          /* Generic web content */
  GNOSTR_PROXY_PROTOCOL_TWITTER,      /* Twitter/X */
  GNOSTR_PROXY_PROTOCOL_TELEGRAM,     /* Telegram */
  GNOSTR_PROXY_PROTOCOL_DISCORD,      /* Discord */
  GNOSTR_PROXY_PROTOCOL_MATRIX,       /* Matrix protocol */
  GNOSTR_PROXY_PROTOCOL_IRC,          /* IRC */
  GNOSTR_PROXY_PROTOCOL_EMAIL,        /* Email (SMTP) */
  GNOSTR_PROXY_PROTOCOL_XMPP,         /* XMPP/Jabber */
} GnostrProxyProtocol;

/**
 * Parsed proxy tag data
 */
typedef struct {
  char *id;                      /* Original content identifier (URL, ID, etc.) */
  char *protocol_str;            /* Protocol string as specified in tag */
  GnostrProxyProtocol protocol;  /* Parsed protocol enum */
  gboolean is_linkable;          /* TRUE if id is a clickable URL */
} GnostrProxyInfo;

/**
 * gnostr_proxy_info_new:
 *
 * Creates a new empty proxy info structure.
 *
 * Returns: (transfer full): Newly allocated GnostrProxyInfo, free with gnostr_proxy_info_free()
 */
GnostrProxyInfo *gnostr_proxy_info_new(void);

/**
 * gnostr_proxy_info_free:
 * @info: (transfer full) (nullable): Proxy info to free
 *
 * Frees a proxy info structure and all its fields.
 */
void gnostr_proxy_info_free(GnostrProxyInfo *info);

/**
 * gnostr_proxy_parse_protocol:
 * @protocol_str: Protocol string from tag (e.g., "activitypub", "atproto")
 *
 * Parses a protocol string into the enum value.
 *
 * Returns: Protocol enum value, or GNOSTR_PROXY_PROTOCOL_UNKNOWN if not recognized
 */
GnostrProxyProtocol gnostr_proxy_parse_protocol(const char *protocol_str);

/**
 * gnostr_proxy_protocol_to_string:
 * @protocol: Protocol enum value
 *
 * Gets the canonical string for a protocol.
 *
 * Returns: (transfer none): Static string for the protocol
 */
const char *gnostr_proxy_protocol_to_string(GnostrProxyProtocol protocol);

/**
 * gnostr_proxy_get_display_name:
 * @protocol: Protocol enum value
 *
 * Gets a human-readable display name for the protocol.
 * For example: GNOSTR_PROXY_PROTOCOL_ACTIVITYPUB -> "ActivityPub"
 *
 * Returns: (transfer none): Static string with display name
 */
const char *gnostr_proxy_get_display_name(GnostrProxyProtocol protocol);

/**
 * gnostr_proxy_get_icon_name:
 * @protocol: Protocol enum value
 *
 * Gets an appropriate icon name for the protocol.
 *
 * Returns: (transfer none): Static string with icon name
 */
const char *gnostr_proxy_get_icon_name(GnostrProxyProtocol protocol);

/**
 * gnostr_proxy_parse_tag:
 * @tag_values: NULL-terminated array of tag values (first element is "proxy")
 * @n_values: Number of elements in the array
 *
 * Parses a single proxy tag into a GnostrProxyInfo structure.
 * The tag format is: ["proxy", "<id>", "<protocol>"]
 *
 * Returns: (transfer full) (nullable): Parsed proxy info or NULL on error
 */
GnostrProxyInfo *gnostr_proxy_parse_tag(const char **tag_values, size_t n_values);

/**
 * gnostr_proxy_parse_tags_json:
 * @tags_json: JSON array string of event tags
 *
 * Parses proxy tag from a JSON tags array.
 * Returns the first proxy tag found (typically there's only one).
 *
 * Returns: (transfer full) (nullable): Parsed proxy info or NULL if no proxy tag
 */
GnostrProxyInfo *gnostr_proxy_parse_tags_json(const char *tags_json);

/**
 * gnostr_proxy_is_url:
 * @id: The proxy ID string
 *
 * Checks if the proxy ID is a valid URL that can be opened.
 *
 * Returns: TRUE if the ID is a clickable URL
 */
gboolean gnostr_proxy_is_url(const char *id);

/**
 * gnostr_proxy_get_source_text:
 * @info: Proxy info structure
 *
 * Gets the attribution text for display (e.g., "via ActivityPub").
 *
 * Returns: (transfer full): Newly allocated string, caller must free
 */
char *gnostr_proxy_get_source_text(const GnostrProxyInfo *info);

/**
 * gnostr_proxy_create_indicator_widget:
 * @info: Proxy info structure
 *
 * Creates a GTK widget showing the proxy indicator (icon + "via X" text).
 * The widget includes a link button if the ID is a URL.
 *
 * Returns: (transfer full): New GtkWidget showing proxy attribution
 */
GtkWidget *gnostr_proxy_create_indicator_widget(const GnostrProxyInfo *info);

G_END_DECLS

#endif /* GNOSTR_NIP48_PROXY_H */
