#include "gn-nostr-profile.h"
#include "nostr_json.h"
#include <json.h>
#include <string.h>

struct _GNostrProfile {
  GObject parent_instance;
  
  char *pubkey;
  char *display_name;
  char *name;
  char *about;
  char *picture_url;
  char *nip05;
  char *lud16;
};

G_DEFINE_TYPE(GNostrProfile, gnostr_profile, G_TYPE_OBJECT)

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

static void gnostr_profile_finalize(GObject *object) {
  GNostrProfile *self = GNOSTR_PROFILE(object);

  g_clear_pointer(&self->pubkey, g_free);
  g_clear_pointer(&self->display_name, g_free);
  g_clear_pointer(&self->name, g_free);
  g_clear_pointer(&self->about, g_free);
  g_clear_pointer(&self->picture_url, g_free);
  g_clear_pointer(&self->nip05, g_free);
  g_clear_pointer(&self->lud16, g_free);

  G_OBJECT_CLASS(gnostr_profile_parent_class)->finalize(object);
}

static void gnostr_profile_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
  GNostrProfile *self = GNOSTR_PROFILE(object);
  
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

static void gnostr_profile_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
  GNostrProfile *self = GNOSTR_PROFILE(object);
  
  switch (prop_id) {
    case PROP_PUBKEY:
      g_free(self->pubkey);
      self->pubkey = g_value_dup_string(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gnostr_profile_class_init(GNostrProfileClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  
  object_class->finalize = gnostr_profile_finalize;
  object_class->get_property = gnostr_profile_get_property;
  object_class->set_property = gnostr_profile_set_property;
  
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

static void gnostr_profile_init(GNostrProfile *self) {
}

GNostrProfile *gnostr_profile_new(const char *pubkey) {
  return g_object_new(GNOSTR_TYPE_PROFILE, "pubkey", pubkey, NULL);
}

const char *gnostr_profile_get_pubkey(GNostrProfile *self) {
  g_return_val_if_fail(GNOSTR_IS_PROFILE(self), NULL);
  return self->pubkey;
}

const char *gnostr_profile_get_display_name(GNostrProfile *self) {
  g_return_val_if_fail(GNOSTR_IS_PROFILE(self), NULL);
  return self->display_name ? self->display_name : self->name;
}

const char *gnostr_profile_get_name(GNostrProfile *self) {
  g_return_val_if_fail(GNOSTR_IS_PROFILE(self), NULL);
  return self->name;
}

const char *gnostr_profile_get_about(GNostrProfile *self) {
  g_return_val_if_fail(GNOSTR_IS_PROFILE(self), NULL);
  return self->about;
}

const char *gnostr_profile_get_picture_url(GNostrProfile *self) {
  g_return_val_if_fail(GNOSTR_IS_PROFILE(self), NULL);
  return self->picture_url;
}

const char *gnostr_profile_get_nip05(GNostrProfile *self) {
  g_return_val_if_fail(GNOSTR_IS_PROFILE(self), NULL);
  return self->nip05;
}

const char *gnostr_profile_get_lud16(GNostrProfile *self) {
  g_return_val_if_fail(GNOSTR_IS_PROFILE(self), NULL);
  return self->lud16;
}

void gnostr_profile_set_display_name(GNostrProfile *self, const char *display_name) {
  g_return_if_fail(GNOSTR_IS_PROFILE(self));
  if (g_strcmp0(self->display_name, display_name) != 0) {
    g_free(self->display_name);
    self->display_name = g_strdup(display_name);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_DISPLAY_NAME]);
  }
}

void gnostr_profile_set_name(GNostrProfile *self, const char *name) {
  g_return_if_fail(GNOSTR_IS_PROFILE(self));
  if (g_strcmp0(self->name, name) != 0) {
    g_free(self->name);
    self->name = g_strdup(name);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_NAME]);
  }
}

void gnostr_profile_set_picture_url(GNostrProfile *self, const char *picture_url) {
  g_return_if_fail(GNOSTR_IS_PROFILE(self));
  if (g_strcmp0(self->picture_url, picture_url) != 0) {
    g_free(self->picture_url);
    self->picture_url = g_strdup(picture_url);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PICTURE_URL]);
  }
}

void gnostr_profile_set_nip05(GNostrProfile *self, const char *nip05) {
  g_return_if_fail(GNOSTR_IS_PROFILE(self));
  if (g_strcmp0(self->nip05, nip05) != 0) {
    g_free(self->nip05);
    self->nip05 = g_strdup(nip05);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_NIP05]);
  }
}

void gnostr_profile_update_from_json(GNostrProfile *self, const char *json_str) {
  g_return_if_fail(GNOSTR_IS_PROFILE(self));
  g_return_if_fail(json_str != NULL);

  /* nostrc-3nj: Use NostrJsonInterface helpers instead of json-glib */
  gboolean changed = FALSE;
  char *str = NULL;

  if (gnostr_json_has_key(json_str, "display_name")) {
    str = gnostr_json_get_string(json_str, "display_name", NULL);
    if (str) {
      if (g_strcmp0(self->display_name, str) != 0) {
        g_free(self->display_name);
        self->display_name = g_strdup(str);
        changed = TRUE;
      }
      free(str);
      str = NULL;
    }
  }

  if (gnostr_json_has_key(json_str, "name")) {
    str = gnostr_json_get_string(json_str, "name", NULL);
    if (str) {
      if (g_strcmp0(self->name, str) != 0) {
        g_free(self->name);
        self->name = g_strdup(str);
        changed = TRUE;
      }
      free(str);
      str = NULL;
    }
  }

  if (gnostr_json_has_key(json_str, "about")) {
    str = gnostr_json_get_string(json_str, "about", NULL);
    if (str) {
      if (g_strcmp0(self->about, str) != 0) {
        g_free(self->about);
        self->about = g_strdup(str);
        changed = TRUE;
      }
      free(str);
      str = NULL;
    }
  }

  if (gnostr_json_has_key(json_str, "picture")) {
    str = gnostr_json_get_string(json_str, "picture", NULL);
    if (str) {
      if (g_strcmp0(self->picture_url, str) != 0) {
        g_free(self->picture_url);
        self->picture_url = g_strdup(str);
        changed = TRUE;
      }
      free(str);
      str = NULL;
    }
  }

  if (gnostr_json_has_key(json_str, "nip05")) {
    str = gnostr_json_get_string(json_str, "nip05", NULL);
    if (str) {
      if (g_strcmp0(self->nip05, str) != 0) {
        g_free(self->nip05);
        self->nip05 = g_strdup(str);
        changed = TRUE;
      }
      free(str);
      str = NULL;
    }
  }

  if (gnostr_json_has_key(json_str, "lud16")) {
    str = gnostr_json_get_string(json_str, "lud16", NULL);
    if (str) {
      if (g_strcmp0(self->lud16, str) != 0) {
        g_free(self->lud16);
        self->lud16 = g_strdup(str);
        changed = TRUE;
      }
      free(str);
      str = NULL;
    }
  }

  if (changed) {
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_DISPLAY_NAME]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_NAME]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_ABOUT]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PICTURE_URL]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_NIP05]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LUD16]);
  }
}
