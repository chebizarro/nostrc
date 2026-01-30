#include "nip39_identity.h"
#include <string.h>
#include "json.h"

/* Platform string mappings */
static const struct {
  const char *name;
  GnostrNip39Platform platform;
  const char *display_name;
  const char *icon_name;
  const char *url_template;  /* %s = identity */
} platform_info[] = {
  { "github",   GNOSTR_NIP39_PLATFORM_GITHUB,   "GitHub",    "github-symbolic",           "https://github.com/%s" },
  { "twitter",  GNOSTR_NIP39_PLATFORM_TWITTER,  "Twitter/X", "twitter-symbolic",          "https://twitter.com/%s" },
  { "mastodon", GNOSTR_NIP39_PLATFORM_MASTODON, "Mastodon",  "mastodon-symbolic",         NULL },  /* Mastodon needs special handling for server */
  { "telegram", GNOSTR_NIP39_PLATFORM_TELEGRAM, "Telegram",  "telegram-symbolic",         "https://t.me/%s" },
  { "keybase",  GNOSTR_NIP39_PLATFORM_KEYBASE,  "Keybase",   "security-high-symbolic",    "https://keybase.io/%s" },
  { "dns",      GNOSTR_NIP39_PLATFORM_DNS,      "DNS",       "network-server-symbolic",   "https://%s" },
  { "reddit",   GNOSTR_NIP39_PLATFORM_REDDIT,   "Reddit",    "reddit-symbolic",           "https://reddit.com/user/%s" },
  { "website",  GNOSTR_NIP39_PLATFORM_WEBSITE,  "Website",   "web-browser-symbolic",      "https://%s" },
  { NULL,       GNOSTR_NIP39_PLATFORM_UNKNOWN,  "Unknown",   "user-symbolic",             NULL }
};

GnostrNip39Platform gnostr_nip39_platform_from_string(const char *platform_str) {
  if (!platform_str || !*platform_str) {
    return GNOSTR_NIP39_PLATFORM_UNKNOWN;
  }

  for (int i = 0; platform_info[i].name != NULL; i++) {
    if (g_ascii_strcasecmp(platform_str, platform_info[i].name) == 0) {
      return platform_info[i].platform;
    }
  }

  return GNOSTR_NIP39_PLATFORM_UNKNOWN;
}

const char *gnostr_nip39_platform_to_string(GnostrNip39Platform platform) {
  for (int i = 0; ; i++) {
    if (platform_info[i].platform == platform) {
      return platform_info[i].name ? platform_info[i].name : "unknown";
    }
    if (platform_info[i].name == NULL) break;
  }
  return "unknown";
}

const char *gnostr_nip39_get_platform_icon(GnostrNip39Platform platform) {
  for (int i = 0; ; i++) {
    if (platform_info[i].platform == platform) {
      return platform_info[i].icon_name;
    }
    if (platform_info[i].name == NULL) break;
  }
  return "user-symbolic";
}

const char *gnostr_nip39_get_platform_display_name(GnostrNip39Platform platform) {
  for (int i = 0; ; i++) {
    if (platform_info[i].platform == platform) {
      return platform_info[i].display_name;
    }
    if (platform_info[i].name == NULL) break;
  }
  return "Unknown";
}

char *gnostr_nip39_get_profile_url(const GnostrExternalIdentity *identity) {
  if (!identity || !identity->identity) {
    return NULL;
  }

  /* Mastodon needs special handling: identity format is "user@server" */
  if (identity->platform == GNOSTR_NIP39_PLATFORM_MASTODON) {
    const char *at = strchr(identity->identity, '@');
    if (at && at != identity->identity) {
      char *user = g_strndup(identity->identity, at - identity->identity);
      const char *server = at + 1;
      char *url = g_strdup_printf("https://%s/@%s", server, user);
      g_free(user);
      return url;
    }
    return NULL;
  }

  /* Find the URL template for this platform */
  for (int i = 0; ; i++) {
    if (platform_info[i].platform == identity->platform) {
      if (platform_info[i].url_template) {
        return g_strdup_printf(platform_info[i].url_template, identity->identity);
      }
      break;
    }
    if (platform_info[i].name == NULL) break;
  }

  return NULL;
}

GnostrExternalIdentity *gnostr_nip39_parse_identity(const char *tag_value, const char *proof_url) {
  if (!tag_value || !*tag_value) {
    return NULL;
  }

  /* Format: "platform:identity" */
  const char *colon = strchr(tag_value, ':');
  if (!colon || colon == tag_value) {
    g_debug("nip39: invalid identity format (no colon): %s", tag_value);
    return NULL;
  }

  char *platform_str = g_strndup(tag_value, colon - tag_value);
  const char *identity = colon + 1;

  if (!identity || !*identity) {
    g_free(platform_str);
    g_debug("nip39: invalid identity format (empty identity): %s", tag_value);
    return NULL;
  }

  GnostrExternalIdentity *result = g_new0(GnostrExternalIdentity, 1);
  result->platform_name = platform_str;
  result->platform = gnostr_nip39_platform_from_string(platform_str);
  result->identity = g_strdup(identity);
  result->proof_url = proof_url ? g_strdup(proof_url) : NULL;
  result->status = GNOSTR_NIP39_STATUS_UNKNOWN;
  result->verified_at = 0;

  g_debug("nip39: parsed identity platform=%s identity=%s proof=%s",
          platform_str, identity, proof_url ? proof_url : "(none)");

  return result;
}

/* Callback context for parsing identity tags */
typedef struct {
  GPtrArray *identities;
} ParseIdentitiesCtx;

/* Callback for iterating over tags array */
static bool parse_identity_tag_cb(size_t index, const char *element_json, void *user_data) {
  (void)index;
  ParseIdentitiesCtx *ctx = user_data;

  /* Each tag element is an array like ["i", "platform:identity", "proof_url"] */
  /* Get array length first */
  size_t arr_len = 0;
  if (nostr_json_get_array_length(element_json, NULL, &arr_len) != 0 || arr_len < 2) {
    return true; /* continue iteration */
  }

  /* Get tag key (first element) */
  char *tag_key = NULL;
  if (nostr_json_get_array_string(element_json, NULL, 0, &tag_key) != 0 || !tag_key) {
    return true;
  }

  /* Check if this is an "i" tag */
  if (strcmp(tag_key, "i") != 0) {
    free(tag_key);
    return true;
  }
  free(tag_key);

  /* Get identity value (second element) */
  char *tag_value = NULL;
  if (nostr_json_get_array_string(element_json, NULL, 1, &tag_value) != 0 || !tag_value) {
    return true;
  }

  /* Get optional proof URL (third element) */
  char *proof_url = NULL;
  if (arr_len >= 3) {
    nostr_json_get_array_string(element_json, NULL, 2, &proof_url);
  }

  GnostrExternalIdentity *identity = gnostr_nip39_parse_identity(tag_value, proof_url);
  if (identity) {
    g_ptr_array_add(ctx->identities, identity);
  }

  free(tag_value);
  if (proof_url) free(proof_url);
  return true; /* continue iteration */
}

GPtrArray *gnostr_nip39_parse_identities_from_event(const char *event_json_str) {
  if (!event_json_str || !*event_json_str) {
    return NULL;
  }

  /* Validate JSON first */
  if (!nostr_json_is_valid(event_json_str)) {
    g_warning("nip39: failed to parse event JSON");
    return NULL;
  }

  /* Check if tags key exists */
  if (!nostr_json_has_key(event_json_str, "tags")) {
    return NULL;
  }

  GPtrArray *identities = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_external_identity_free);
  ParseIdentitiesCtx ctx = { .identities = identities };

  /* Iterate over tags array using facade */
  nostr_json_array_foreach(event_json_str, "tags", parse_identity_tag_cb, &ctx);

  if (identities->len == 0) {
    g_ptr_array_unref(identities);
    return NULL;
  }

  return identities;
}

void gnostr_external_identity_free(GnostrExternalIdentity *identity) {
  if (!identity) return;
  g_free(identity->platform_name);
  g_free(identity->identity);
  g_free(identity->proof_url);
  g_free(identity);
}

GtkWidget *gnostr_nip39_create_identity_row(const GnostrExternalIdentity *identity) {
  if (!identity) return NULL;

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(row, 4);
  gtk_widget_set_margin_bottom(row, 4);

  /* Platform icon */
  const char *icon_name = gnostr_nip39_get_platform_icon(identity->platform);
  GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
  gtk_widget_add_css_class(icon, "dim-label");
  gtk_box_append(GTK_BOX(row), icon);

  /* Platform name label */
  const char *display_name = gnostr_nip39_get_platform_display_name(identity->platform);
  GtkWidget *platform_lbl = gtk_label_new(display_name);
  gtk_label_set_xalign(GTK_LABEL(platform_lbl), 0.0);
  gtk_widget_add_css_class(platform_lbl, "dim-label");
  gtk_box_append(GTK_BOX(row), platform_lbl);

  /* Identity value - make clickable if we can generate a URL */
  char *profile_url = gnostr_nip39_get_profile_url(identity);
  if (profile_url) {
    char *markup = g_markup_printf_escaped("<a href=\"%s\">%s</a>", profile_url, identity->identity);
    GtkWidget *link = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(link), markup);
    gtk_label_set_xalign(GTK_LABEL(link), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(link), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(link, TRUE);
    gtk_box_append(GTK_BOX(row), link);
    g_free(markup);
    g_free(profile_url);
  } else {
    GtkWidget *val = gtk_label_new(identity->identity);
    gtk_label_set_xalign(GTK_LABEL(val), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(val), PANGO_ELLIPSIZE_END);
    gtk_label_set_selectable(GTK_LABEL(val), TRUE);
    gtk_widget_set_hexpand(val, TRUE);
    gtk_box_append(GTK_BOX(row), val);
  }

  /* Verification status indicator */
  if (identity->status == GNOSTR_NIP39_STATUS_VERIFIED) {
    GtkWidget *badge = gtk_image_new_from_icon_name("emblem-ok-symbolic");
    gtk_widget_add_css_class(badge, "success");
    gtk_widget_set_tooltip_text(badge, "Verified");
    gtk_box_append(GTK_BOX(row), badge);
  } else if (identity->proof_url && *identity->proof_url) {
    /* Show proof link if available but not verified */
    GtkWidget *proof_btn = gtk_link_button_new(identity->proof_url);
    gtk_button_set_label(GTK_BUTTON(proof_btn), "");
    gtk_button_set_icon_name(GTK_BUTTON(proof_btn), "emblem-documents-symbolic");
    gtk_widget_set_tooltip_text(proof_btn, "View proof");
    gtk_widget_add_css_class(proof_btn, "flat");
    gtk_box_append(GTK_BOX(row), proof_btn);
  }

  return row;
}

char *gnostr_nip39_build_tags_json(GPtrArray *identities) {
  if (!identities || identities->len == 0) {
    return g_strdup("[]");
  }

  NostrJsonBuilder *builder = nostr_json_builder_new();
  if (!builder) {
    return g_strdup("[]");
  }

  nostr_json_builder_begin_array(builder);

  for (guint i = 0; i < identities->len; i++) {
    GnostrExternalIdentity *identity = g_ptr_array_index(identities, i);
    if (!identity || !identity->platform_name || !identity->identity) {
      continue;
    }

    /* Begin tag array: ["i", "platform:identity", "proof_url"] */
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "i");

    /* Build "platform:identity" string */
    char *tag_value = g_strdup_printf("%s:%s", identity->platform_name, identity->identity);
    nostr_json_builder_add_string(builder, tag_value);
    g_free(tag_value);

    /* Add proof URL if present */
    if (identity->proof_url && *identity->proof_url) {
      nostr_json_builder_add_string(builder, identity->proof_url);
    }

    nostr_json_builder_end_array(builder);
  }

  nostr_json_builder_end_array(builder);

  char *result = nostr_json_builder_finish(builder);
  nostr_json_builder_free(builder);
  return result;
}

const char *gnostr_nip39_status_to_string(GnostrNip39Status status) {
  switch (status) {
    case GNOSTR_NIP39_STATUS_UNKNOWN:      return "unknown";
    case GNOSTR_NIP39_STATUS_VERIFYING:    return "verifying";
    case GNOSTR_NIP39_STATUS_VERIFIED:     return "verified";
    case GNOSTR_NIP39_STATUS_FAILED:       return "failed";
    case GNOSTR_NIP39_STATUS_UNVERIFIABLE: return "unverifiable";
    default:                               return "unknown";
  }
}
