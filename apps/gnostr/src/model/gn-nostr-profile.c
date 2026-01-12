#include "gn-nostr-profile.h"
#include <json-glib/json-glib.h>
#include <string.h>

struct _GnNostrProfile {
  GObject parent_instance;
  
  char *pubkey;
  char *display_name;
  char *name;
  char *about;
  char *picture_url;
  char *nip05;
  char *lud16;
};

G_DEFINE_TYPE(GnNostrProfile, gn_nostr_profile, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_PUBKEY,
  PROP_DISPLAY_NAME,
  PROP_NAME,
  PROP_ABOUT,
  PROP_PICTURE_URL,
  PROP_NIP05,
  PROP_LUD16,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void gn_nostr_profile_finalize(GObject *object) {
  GnNostrProfile *self = GN_NOSTR_PROFILE(object);
  
  g_free(self->pubkey);
  g_free(self->display_name);
  g_free(self->name);
  g_free(self->about);
  g_free(self->picture_url);
  g_free(self->nip05);
  g_free(self->lud16);
  
  G_OBJECT_CLASS(gn_nostr_profile_parent_class)->finalize(object);
}

static void gn_nostr_profile_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
  GnNostrProfile *self = GN_NOSTR_PROFILE(object);
  
  switch (prop_id) {
    case PROP_PUBKEY:
      g_value_set_string(value, self->pubkey);
      break;
    case PROP_DISPLAY_NAME:
      g_value_set_string(value, self->display_name);
      break;
    case PROP_NAME:
      g_value_set_string(value, self->name);
      break;
    case PROP_ABOUT:
      g_value_set_string(value, self->about);
      break;
    case PROP_PICTURE_URL:
      g_value_set_string(value, self->picture_url);
      break;
    case PROP_NIP05:
      g_value_set_string(value, self->nip05);
      break;
    case PROP_LUD16:
      g_value_set_string(value, self->lud16);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gn_nostr_profile_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
  GnNostrProfile *self = GN_NOSTR_PROFILE(object);
  
  switch (prop_id) {
    case PROP_PUBKEY:
      g_free(self->pubkey);
      self->pubkey = g_value_dup_string(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gn_nostr_profile_class_init(GnNostrProfileClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  
  object_class->finalize = gn_nostr_profile_finalize;
  object_class->get_property = gn_nostr_profile_get_property;
  object_class->set_property = gn_nostr_profile_set_property;
  
  properties[PROP_PUBKEY] = g_param_spec_string("pubkey", "Pubkey", "Public key hex", NULL,
                                                 G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  properties[PROP_DISPLAY_NAME] = g_param_spec_string("display-name", "Display Name", "Display name", NULL,
                                                       G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_NAME] = g_param_spec_string("name", "Name", "Username", NULL,
                                               G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_ABOUT] = g_param_spec_string("about", "About", "Bio", NULL,
                                                G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_PICTURE_URL] = g_param_spec_string("picture-url", "Picture URL", "Avatar URL", NULL,
                                                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_NIP05] = g_param_spec_string("nip05", "NIP-05", "NIP-05 identifier", NULL,
                                                G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_LUD16] = g_param_spec_string("lud16", "LUD16", "Lightning address", NULL,
                                                G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  
  g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void gn_nostr_profile_init(GnNostrProfile *self) {
}

GnNostrProfile *gn_nostr_profile_new(const char *pubkey) {
  return g_object_new(GN_TYPE_NOSTR_PROFILE, "pubkey", pubkey, NULL);
}

const char *gn_nostr_profile_get_pubkey(GnNostrProfile *self) {
  g_return_val_if_fail(GN_IS_NOSTR_PROFILE(self), NULL);
  return self->pubkey;
}

const char *gn_nostr_profile_get_display_name(GnNostrProfile *self) {
  g_return_val_if_fail(GN_IS_NOSTR_PROFILE(self), NULL);
  return self->display_name ? self->display_name : self->name;
}

const char *gn_nostr_profile_get_name(GnNostrProfile *self) {
  g_return_val_if_fail(GN_IS_NOSTR_PROFILE(self), NULL);
  return self->name;
}

const char *gn_nostr_profile_get_about(GnNostrProfile *self) {
  g_return_val_if_fail(GN_IS_NOSTR_PROFILE(self), NULL);
  return self->about;
}

const char *gn_nostr_profile_get_picture_url(GnNostrProfile *self) {
  g_return_val_if_fail(GN_IS_NOSTR_PROFILE(self), NULL);
  return self->picture_url;
}

const char *gn_nostr_profile_get_nip05(GnNostrProfile *self) {
  g_return_val_if_fail(GN_IS_NOSTR_PROFILE(self), NULL);
  return self->nip05;
}

const char *gn_nostr_profile_get_lud16(GnNostrProfile *self) {
  g_return_val_if_fail(GN_IS_NOSTR_PROFILE(self), NULL);
  return self->lud16;
}

void gn_nostr_profile_update_from_json(GnNostrProfile *self, const char *json) {
  g_return_if_fail(GN_IS_NOSTR_PROFILE(self));
  g_return_if_fail(json != NULL);
  
  GError *error = NULL;
  JsonParser *parser = json_parser_new();
  
  if (!json_parser_load_from_data(parser, json, -1, &error)) {
    g_warning("Failed to parse profile JSON: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return;
  }
  
  JsonNode *root_node = json_parser_get_root(parser);
  if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) {
    g_object_unref(parser);
    return;
  }
  
  JsonObject *root = json_node_get_object(root_node);
  gboolean changed = FALSE;
  
  if (json_object_has_member(root, "display_name")) {
    const char *str = json_object_get_string_member(root, "display_name");
    if (g_strcmp0(self->display_name, str) != 0) {
      g_free(self->display_name);
      self->display_name = g_strdup(str);
      changed = TRUE;
    }
  }
  
  if (json_object_has_member(root, "name")) {
    const char *str = json_object_get_string_member(root, "name");
    if (g_strcmp0(self->name, str) != 0) {
      g_free(self->name);
      self->name = g_strdup(str);
      changed = TRUE;
    }
  }
  
  if (json_object_has_member(root, "about")) {
    const char *str = json_object_get_string_member(root, "about");
    if (g_strcmp0(self->about, str) != 0) {
      g_free(self->about);
      self->about = g_strdup(str);
      changed = TRUE;
    }
  }
  
  if (json_object_has_member(root, "picture")) {
    const char *str = json_object_get_string_member(root, "picture");
    if (g_strcmp0(self->picture_url, str) != 0) {
      g_free(self->picture_url);
      self->picture_url = g_strdup(str);
      changed = TRUE;
    }
  }
  
  if (json_object_has_member(root, "nip05")) {
    const char *str = json_object_get_string_member(root, "nip05");
    if (g_strcmp0(self->nip05, str) != 0) {
      g_free(self->nip05);
      self->nip05 = g_strdup(str);
      changed = TRUE;
    }
  }
  
  if (json_object_has_member(root, "lud16")) {
    const char *str = json_object_get_string_member(root, "lud16");
    if (g_strcmp0(self->lud16, str) != 0) {
      g_free(self->lud16);
      self->lud16 = g_strdup(str);
      changed = TRUE;
    }
  }
  
  g_object_unref(parser);
  
  if (changed) {
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_DISPLAY_NAME]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_NAME]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_ABOUT]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PICTURE_URL]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_NIP05]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LUD16]);
  }
}
