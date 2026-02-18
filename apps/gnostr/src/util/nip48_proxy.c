/**
 * @file nip48_proxy.c
 * @brief NIP-48 Proxy Tags implementation
 */

#include "nip48_proxy.h"
#include <string.h>
#include <stdlib.h>
#include <json-glib/json-glib.h>

/* Protocol string mappings */
static const struct {
  const char *str;
  GnostrProxyProtocol protocol;
} protocol_map[] = {
  { "activitypub", GNOSTR_PROXY_PROTOCOL_ACTIVITYPUB },
  { "atproto",     GNOSTR_PROXY_PROTOCOL_ATPROTO },
  { "rss",         GNOSTR_PROXY_PROTOCOL_RSS },
  { "web",         GNOSTR_PROXY_PROTOCOL_WEB },
  { "twitter",     GNOSTR_PROXY_PROTOCOL_TWITTER },
  { "x",           GNOSTR_PROXY_PROTOCOL_TWITTER },  /* Alias for Twitter */
  { "telegram",    GNOSTR_PROXY_PROTOCOL_TELEGRAM },
  { "discord",     GNOSTR_PROXY_PROTOCOL_DISCORD },
  { "matrix",      GNOSTR_PROXY_PROTOCOL_MATRIX },
  { "irc",         GNOSTR_PROXY_PROTOCOL_IRC },
  { "email",       GNOSTR_PROXY_PROTOCOL_EMAIL },
  { "smtp",        GNOSTR_PROXY_PROTOCOL_EMAIL },    /* Alias for email */
  { "xmpp",        GNOSTR_PROXY_PROTOCOL_XMPP },
  { "jabber",      GNOSTR_PROXY_PROTOCOL_XMPP },     /* Alias for XMPP */
  { NULL, 0 }
};

/* Display names for protocols */
static const char *display_names[] = {
  [GNOSTR_PROXY_PROTOCOL_UNKNOWN]     = "External",
  [GNOSTR_PROXY_PROTOCOL_ACTIVITYPUB] = "ActivityPub",
  [GNOSTR_PROXY_PROTOCOL_ATPROTO]     = "Bluesky",
  [GNOSTR_PROXY_PROTOCOL_RSS]         = "RSS",
  [GNOSTR_PROXY_PROTOCOL_WEB]         = "Web",
  [GNOSTR_PROXY_PROTOCOL_TWITTER]     = "Twitter",
  [GNOSTR_PROXY_PROTOCOL_TELEGRAM]    = "Telegram",
  [GNOSTR_PROXY_PROTOCOL_DISCORD]     = "Discord",
  [GNOSTR_PROXY_PROTOCOL_MATRIX]      = "Matrix",
  [GNOSTR_PROXY_PROTOCOL_IRC]         = "IRC",
  [GNOSTR_PROXY_PROTOCOL_EMAIL]       = "Email",
  [GNOSTR_PROXY_PROTOCOL_XMPP]        = "XMPP",
};

/* Icon names for protocols (using standard/symbolic icons) */
static const char *icon_names[] = {
  [GNOSTR_PROXY_PROTOCOL_UNKNOWN]     = "network-transmit-symbolic",
  [GNOSTR_PROXY_PROTOCOL_ACTIVITYPUB] = "network-server-symbolic",
  [GNOSTR_PROXY_PROTOCOL_ATPROTO]     = "weather-clear-symbolic",  /* Blue sky */
  [GNOSTR_PROXY_PROTOCOL_RSS]         = "application-rss+xml-symbolic",
  [GNOSTR_PROXY_PROTOCOL_WEB]         = "web-browser-symbolic",
  [GNOSTR_PROXY_PROTOCOL_TWITTER]     = "user-available-symbolic",
  [GNOSTR_PROXY_PROTOCOL_TELEGRAM]    = "mail-send-symbolic",
  [GNOSTR_PROXY_PROTOCOL_DISCORD]     = "audio-headphones-symbolic",
  [GNOSTR_PROXY_PROTOCOL_MATRIX]      = "network-workgroup-symbolic",
  [GNOSTR_PROXY_PROTOCOL_IRC]         = "utilities-terminal-symbolic",
  [GNOSTR_PROXY_PROTOCOL_EMAIL]       = "mail-unread-symbolic",
  [GNOSTR_PROXY_PROTOCOL_XMPP]        = "user-status-pending-symbolic",
};

GnostrProxyInfo *gnostr_proxy_info_new(void) {
  GnostrProxyInfo *info = g_new0(GnostrProxyInfo, 1);
  info->protocol = GNOSTR_PROXY_PROTOCOL_UNKNOWN;
  info->is_linkable = FALSE;
  return info;
}

void gnostr_proxy_info_free(GnostrProxyInfo *info) {
  if (!info) return;
  g_free(info->id);
  g_free(info->protocol_str);
  g_free(info);
}

GnostrProxyProtocol gnostr_proxy_parse_protocol(const char *protocol_str) {
  if (!protocol_str || !*protocol_str) {
    return GNOSTR_PROXY_PROTOCOL_UNKNOWN;
  }

  /* Convert to lowercase for comparison */
  char *lower = g_ascii_strdown(protocol_str, -1);

  GnostrProxyProtocol result = GNOSTR_PROXY_PROTOCOL_UNKNOWN;
  for (int i = 0; protocol_map[i].str != NULL; i++) {
    if (strcmp(lower, protocol_map[i].str) == 0) {
      result = protocol_map[i].protocol;
      break;
    }
  }

  g_free(lower);
  return result;
}

const char *gnostr_proxy_protocol_to_string(GnostrProxyProtocol protocol) {
  switch (protocol) {
    case GNOSTR_PROXY_PROTOCOL_ACTIVITYPUB: return "activitypub";
    case GNOSTR_PROXY_PROTOCOL_ATPROTO:     return "atproto";
    case GNOSTR_PROXY_PROTOCOL_RSS:         return "rss";
    case GNOSTR_PROXY_PROTOCOL_WEB:         return "web";
    case GNOSTR_PROXY_PROTOCOL_TWITTER:     return "twitter";
    case GNOSTR_PROXY_PROTOCOL_TELEGRAM:    return "telegram";
    case GNOSTR_PROXY_PROTOCOL_DISCORD:     return "discord";
    case GNOSTR_PROXY_PROTOCOL_MATRIX:      return "matrix";
    case GNOSTR_PROXY_PROTOCOL_IRC:         return "irc";
    case GNOSTR_PROXY_PROTOCOL_EMAIL:       return "email";
    case GNOSTR_PROXY_PROTOCOL_XMPP:        return "xmpp";
    default:                                return "unknown";
  }
}

const char *gnostr_proxy_get_display_name(GnostrProxyProtocol protocol) {
  if (protocol >= 0 && protocol < G_N_ELEMENTS(display_names)) {
    return display_names[protocol];
  }
  return display_names[GNOSTR_PROXY_PROTOCOL_UNKNOWN];
}

const char *gnostr_proxy_get_icon_name(GnostrProxyProtocol protocol) {
  if (protocol >= 0 && protocol < G_N_ELEMENTS(icon_names)) {
    return icon_names[protocol];
  }
  return icon_names[GNOSTR_PROXY_PROTOCOL_UNKNOWN];
}

gboolean gnostr_proxy_is_url(const char *id) {
  if (!id || !*id) return FALSE;

  return g_str_has_prefix(id, "http://") ||
         g_str_has_prefix(id, "https://") ||
         g_str_has_prefix(id, "at://");  /* AT Protocol URLs */
}

GnostrProxyInfo *gnostr_proxy_parse_tag(const char **tag_values, size_t n_values) {
  if (!tag_values || n_values < 3) return NULL;

  /* First element should be "proxy" */
  if (!tag_values[0] || strcmp(tag_values[0], "proxy") != 0) return NULL;

  /* Second element is the ID */
  if (!tag_values[1] || !*tag_values[1]) return NULL;

  /* Third element is the protocol */
  if (!tag_values[2] || !*tag_values[2]) return NULL;

  GnostrProxyInfo *info = gnostr_proxy_info_new();
  info->id = g_strdup(tag_values[1]);
  info->protocol_str = g_strdup(tag_values[2]);
  info->protocol = gnostr_proxy_parse_protocol(info->protocol_str);
  info->is_linkable = gnostr_proxy_is_url(info->id);

  return info;
}

GnostrProxyInfo *gnostr_proxy_parse_tags_json(const char *tags_json) {
  if (!tags_json || !*tags_json) return NULL;

  GError *error = NULL;
  g_autoptr(JsonParser) parser = json_parser_new();

  if (!json_parser_load_from_data(parser, tags_json, -1, &error)) {
    g_debug("nip48: Failed to parse tags JSON: %s", error ? error->message : "unknown");
    g_clear_error(&error);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) {
    return NULL;
  }

  JsonArray *tags_array = json_node_get_array(root);
  guint n_tags = json_array_get_length(tags_array);

  GnostrProxyInfo *result = NULL;

  for (guint i = 0; i < n_tags && result == NULL; i++) {
    JsonNode *tag_node = json_array_get_element(tags_array, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

    JsonArray *tag = json_node_get_array(tag_node);
    guint tag_len = json_array_get_length(tag);
    if (tag_len < 3) continue;

    /* Check if this is a proxy tag */
    const char *tag_name = json_array_get_string_element(tag, 0);
    if (!tag_name || strcmp(tag_name, "proxy") != 0) continue;

    /* Extract tag values */
    const char **values = g_new0(const char *, tag_len + 1);
    for (guint j = 0; j < tag_len; j++) {
      JsonNode *elem = json_array_get_element(tag, j);
      if (JSON_NODE_HOLDS_VALUE(elem)) {
        values[j] = json_node_get_string(elem);
      }
    }

    result = gnostr_proxy_parse_tag(values, tag_len);
    g_free(values);
  }

  return result;
}

char *gnostr_proxy_get_source_text(const GnostrProxyInfo *info) {
  if (!info) return NULL;

  const char *display = gnostr_proxy_get_display_name(info->protocol);
  return g_strdup_printf("via %s", display);
}

GtkWidget *gnostr_proxy_create_indicator_widget(const GnostrProxyInfo *info) {
  if (!info) return NULL;

  /* Create horizontal box for the indicator */
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(box, "proxy-indicator");
  gtk_widget_add_css_class(box, "dim-label");
  gtk_widget_add_css_class(box, "caption");

  /* Add protocol icon */
  const char *icon_name = gnostr_proxy_get_icon_name(info->protocol);
  GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 12);
  gtk_box_append(GTK_BOX(box), icon);

  /* Add "via Protocol" text */
  char *source_text = gnostr_proxy_get_source_text(info);
  GtkWidget *label = gtk_label_new(source_text);
  gtk_widget_add_css_class(label, "dim-label");
  gtk_widget_add_css_class(label, "caption");
  g_free(source_text);

  /* If the ID is a URL, make it a clickable link */
  if (info->is_linkable) {
    /* Wrap in a link button */
    GtkWidget *link = gtk_link_button_new_with_label(info->id, gtk_label_get_text(GTK_LABEL(label)));
    gtk_widget_add_css_class(link, "flat");
    gtk_widget_add_css_class(link, "proxy-link");
    gtk_box_append(GTK_BOX(box), link);
  } else {
    gtk_box_append(GTK_BOX(box), label);
  }

  /* Set tooltip with full source ID */
  if (info->id) {
    char *tooltip = g_strdup_printf("Source: %s", info->id);
    gtk_widget_set_tooltip_text(box, tooltip);
    g_free(tooltip);
  }

  return box;
}
