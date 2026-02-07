#include "nostr_nip19.h"
#include "nostr-error.h"
#include <nostr/nip19/nip19.h>
#include <glib.h>
#include <string.h>

/* ── GNostrBech32Type enum registration ─────────────────────────── */

GType
gnostr_bech32_type_get_type(void)
{
  static GType type = 0;

  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GNOSTR_BECH32_UNKNOWN,  "GNOSTR_BECH32_UNKNOWN",  "unknown" },
      { GNOSTR_BECH32_NPUB,     "GNOSTR_BECH32_NPUB",     "npub" },
      { GNOSTR_BECH32_NSEC,     "GNOSTR_BECH32_NSEC",     "nsec" },
      { GNOSTR_BECH32_NOTE,     "GNOSTR_BECH32_NOTE",     "note" },
      { GNOSTR_BECH32_NPROFILE, "GNOSTR_BECH32_NPROFILE", "nprofile" },
      { GNOSTR_BECH32_NEVENT,   "GNOSTR_BECH32_NEVENT",   "nevent" },
      { GNOSTR_BECH32_NADDR,    "GNOSTR_BECH32_NADDR",    "naddr" },
      { GNOSTR_BECH32_NRELAY,   "GNOSTR_BECH32_NRELAY",   "nrelay" },
      { 0, NULL, NULL }
    };
    GType t = g_enum_register_static("GNostrBech32Type", values);
    g_once_init_leave(&type, t);
  }

  return type;
}

/* ── Internal helpers ────────────────────────────────────────────── */

static gboolean
hex_to_bytes(const gchar *hex, guint8 *out, gsize out_len)
{
  gsize hex_len = strlen(hex);
  if (hex_len != out_len * 2)
    return FALSE;

  for (gsize i = 0; i < out_len; i++) {
    gchar byte_str[3] = { hex[i*2], hex[i*2+1], '\0' };
    gchar *endptr;
    gulong val = strtoul(byte_str, &endptr, 16);
    if (*endptr != '\0')
      return FALSE;
    out[i] = (guint8)val;
  }

  return TRUE;
}

static gchar *
bytes_to_hex(const guint8 *bytes, gsize len)
{
  gchar *hex = g_malloc(len * 2 + 1);
  for (gsize i = 0; i < len; i++) {
    g_snprintf(hex + i * 2, 3, "%02x", bytes[i]);
  }
  hex[len * 2] = '\0';
  return hex;
}

static gchar **
strv_from_c_array(char **relays, size_t count)
{
  if (count == 0 || relays == NULL)
    return NULL;

  gchar **result = g_new0(gchar *, count + 1);
  for (size_t i = 0; i < count; i++)
    result[i] = g_strdup(relays[i]);
  result[count] = NULL;
  return result;
}

static size_t
strv_length(const gchar *const *strv)
{
  if (strv == NULL)
    return 0;
  size_t n = 0;
  while (strv[n] != NULL)
    n++;
  return n;
}

/* ── GNostrNip19 struct ──────────────────────────────────────────── */

struct _GNostrNip19 {
  GObject parent_instance;

  GNostrBech32Type entity_type;
  gchar *bech32;

  /* Simple types (npub/nsec/note): 32-byte key data as hex */
  gchar *hex;

  /* For npub and nprofile: pubkey hex */
  gchar *pubkey;

  /* For note and nevent: event ID hex */
  gchar *event_id;

  /* For nevent and naddr: author pubkey hex */
  gchar *author;

  /* For nevent and naddr: event kind (-1 if unset) */
  gint kind;

  /* For naddr: d-tag identifier */
  gchar *identifier;

  /* For nprofile, nevent, naddr, nrelay: relay URLs (NULL-terminated) */
  gchar **relays;
};

/* ── Properties ──────────────────────────────────────────────────── */

enum {
  PROP_0,
  PROP_ENTITY_TYPE,
  PROP_BECH32,
  PROP_PUBKEY,
  PROP_EVENT_ID,
  PROP_AUTHOR,
  PROP_KIND,
  PROP_IDENTIFIER,
  PROP_RELAYS,
  N_PROPERTIES
};

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };

G_DEFINE_TYPE(GNostrNip19, gnostr_nip19, G_TYPE_OBJECT)

static void
gnostr_nip19_get_property(GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  GNostrNip19 *self = GNOSTR_NIP19(object);

  switch (prop_id) {
  case PROP_ENTITY_TYPE:
    g_value_set_enum(value, self->entity_type);
    break;
  case PROP_BECH32:
    g_value_set_string(value, self->bech32);
    break;
  case PROP_PUBKEY:
    g_value_set_string(value, self->pubkey);
    break;
  case PROP_EVENT_ID:
    g_value_set_string(value, self->event_id);
    break;
  case PROP_AUTHOR:
    g_value_set_string(value, self->author);
    break;
  case PROP_KIND:
    g_value_set_int(value, self->kind);
    break;
  case PROP_IDENTIFIER:
    g_value_set_string(value, self->identifier);
    break;
  case PROP_RELAYS:
    g_value_set_boxed(value, self->relays);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void
gnostr_nip19_finalize(GObject *object)
{
  GNostrNip19 *self = GNOSTR_NIP19(object);

  g_free(self->bech32);
  g_free(self->hex);
  g_free(self->pubkey);
  g_free(self->event_id);
  g_free(self->author);
  g_free(self->identifier);
  g_strfreev(self->relays);

  G_OBJECT_CLASS(gnostr_nip19_parent_class)->finalize(object);
}

static void
gnostr_nip19_class_init(GNostrNip19Class *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->get_property = gnostr_nip19_get_property;
  object_class->finalize = gnostr_nip19_finalize;

  obj_properties[PROP_ENTITY_TYPE] = g_param_spec_enum(
    "entity-type", "Entity Type", "NIP-19 bech32 entity type",
    GNOSTR_TYPE_BECH32_TYPE, GNOSTR_BECH32_UNKNOWN,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  obj_properties[PROP_BECH32] = g_param_spec_string(
    "bech32", "Bech32", "The bech32-encoded string",
    NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  obj_properties[PROP_PUBKEY] = g_param_spec_string(
    "pubkey", "Public Key", "Public key hex (npub, nprofile, naddr)",
    NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  obj_properties[PROP_EVENT_ID] = g_param_spec_string(
    "event-id", "Event ID", "Event ID hex (note, nevent)",
    NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  obj_properties[PROP_AUTHOR] = g_param_spec_string(
    "author", "Author", "Author pubkey hex (nevent, naddr)",
    NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  obj_properties[PROP_KIND] = g_param_spec_int(
    "kind", "Kind", "Event kind (nevent, naddr); -1 if unset",
    -1, G_MAXINT, -1,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  obj_properties[PROP_IDENTIFIER] = g_param_spec_string(
    "identifier", "Identifier", "d-tag identifier (naddr)",
    NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  obj_properties[PROP_RELAYS] = g_param_spec_boxed(
    "relays", "Relays", "Relay URLs (nprofile, nevent, naddr, nrelay)",
    G_TYPE_STRV,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPERTIES, obj_properties);
}

static void
gnostr_nip19_init(GNostrNip19 *self)
{
  self->entity_type = GNOSTR_BECH32_UNKNOWN;
  self->kind = -1;
}

/* ── Decode constructor ──────────────────────────────────────────── */

GNostrNip19 *
gnostr_nip19_decode(const gchar *bech32, GError **error)
{
  g_return_val_if_fail(bech32 != NULL, NULL);

  /* Inspect type first */
  NostrBech32Type ctype;
  if (nostr_nip19_inspect(bech32, &ctype) != 0) {
    g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                "Invalid NIP-19 bech32 string: %s", bech32);
    return NULL;
  }

  GNostrNip19 *self = g_object_new(GNOSTR_TYPE_NIP19, NULL);
  self->entity_type = (GNostrBech32Type)ctype;
  self->bech32 = g_strdup(bech32);

  switch (ctype) {
  case NOSTR_B32_NPUB: {
    guint8 pubkey[32];
    if (nostr_nip19_decode_npub(bech32, pubkey) != 0) {
      g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                  "Failed to decode npub: %s", bech32);
      g_object_unref(self);
      return NULL;
    }
    self->hex = bytes_to_hex(pubkey, 32);
    self->pubkey = g_strdup(self->hex);
    break;
  }

  case NOSTR_B32_NSEC: {
    guint8 seckey[32];
    if (nostr_nip19_decode_nsec(bech32, seckey) != 0) {
      g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                  "Failed to decode nsec: %s", bech32);
      g_object_unref(self);
      return NULL;
    }
    self->hex = bytes_to_hex(seckey, 32);
    /* Wipe the stack buffer */
    memset(seckey, 0, 32);
    break;
  }

  case NOSTR_B32_NOTE: {
    guint8 event_id[32];
    if (nostr_nip19_decode_note(bech32, event_id) != 0) {
      g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                  "Failed to decode note: %s", bech32);
      g_object_unref(self);
      return NULL;
    }
    self->hex = bytes_to_hex(event_id, 32);
    self->event_id = g_strdup(self->hex);
    break;
  }

  case NOSTR_B32_NPROFILE: {
    NostrProfilePointer *pp = NULL;
    if (nostr_nip19_decode_nprofile(bech32, &pp) != 0 || pp == NULL) {
      g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                  "Failed to decode nprofile: %s", bech32);
      g_object_unref(self);
      return NULL;
    }
    self->pubkey = g_strdup(pp->public_key);
    self->relays = strv_from_c_array(pp->relays, pp->relays_count);
    nostr_profile_pointer_free(pp);
    break;
  }

  case NOSTR_B32_NEVENT: {
    NostrEventPointer *ep = NULL;
    if (nostr_nip19_decode_nevent(bech32, &ep) != 0 || ep == NULL) {
      g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                  "Failed to decode nevent: %s", bech32);
      g_object_unref(self);
      return NULL;
    }
    self->event_id = g_strdup(ep->id);
    self->author = g_strdup(ep->author);
    self->kind = ep->kind;
    self->relays = strv_from_c_array(ep->relays, ep->relays_count);
    nostr_event_pointer_free(ep);
    break;
  }

  case NOSTR_B32_NADDR: {
    NostrEntityPointer *ap = NULL;
    if (nostr_nip19_decode_naddr(bech32, &ap) != 0 || ap == NULL) {
      g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                  "Failed to decode naddr: %s", bech32);
      g_object_unref(self);
      return NULL;
    }
    self->pubkey = g_strdup(ap->public_key);
    self->identifier = g_strdup(ap->identifier);
    self->kind = ap->kind;
    self->relays = strv_from_c_array(ap->relays, ap->relays_count);
    nostr_entity_pointer_free(ap);
    break;
  }

  case NOSTR_B32_NRELAY: {
    char **relays = NULL;
    size_t count = 0;
    if (nostr_nip19_decode_nrelay(bech32, &relays, &count) != 0) {
      g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                  "Failed to decode nrelay: %s", bech32);
      g_object_unref(self);
      return NULL;
    }
    self->relays = strv_from_c_array(relays, count);
    /* Free the C-allocated relay array */
    for (size_t i = 0; i < count; i++)
      free(relays[i]);
    free(relays);
    break;
  }

  default:
    g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                "Unknown NIP-19 type in: %s", bech32);
    g_object_unref(self);
    return NULL;
  }

  return self;
}

/* ── Encode constructors ─────────────────────────────────────────── */

GNostrNip19 *
gnostr_nip19_encode_npub(const gchar *pubkey_hex, GError **error)
{
  g_return_val_if_fail(pubkey_hex != NULL, NULL);

  guint8 pubkey[32];
  if (!hex_to_bytes(pubkey_hex, pubkey, 32)) {
    g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_KEY,
                "Invalid public key hex (expected 64 hex chars): %s", pubkey_hex);
    return NULL;
  }

  char *bech = NULL;
  if (nostr_nip19_encode_npub(pubkey, &bech) != 0 || bech == NULL) {
    g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                        "Failed to encode npub");
    return NULL;
  }

  GNostrNip19 *self = g_object_new(GNOSTR_TYPE_NIP19, NULL);
  self->entity_type = GNOSTR_BECH32_NPUB;
  self->bech32 = g_strdup(bech);
  self->hex = g_strdup(pubkey_hex);
  self->pubkey = g_strdup(pubkey_hex);
  free(bech);
  return self;
}

GNostrNip19 *
gnostr_nip19_encode_nsec(const gchar *seckey_hex, GError **error)
{
  g_return_val_if_fail(seckey_hex != NULL, NULL);

  guint8 seckey[32];
  if (!hex_to_bytes(seckey_hex, seckey, 32)) {
    g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_KEY,
                "Invalid secret key hex (expected 64 hex chars): %s", seckey_hex);
    return NULL;
  }

  char *bech = NULL;
  if (nostr_nip19_encode_nsec(seckey, &bech) != 0 || bech == NULL) {
    memset(seckey, 0, 32);
    g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                        "Failed to encode nsec");
    return NULL;
  }

  GNostrNip19 *self = g_object_new(GNOSTR_TYPE_NIP19, NULL);
  self->entity_type = GNOSTR_BECH32_NSEC;
  self->bech32 = g_strdup(bech);
  self->hex = g_strdup(seckey_hex);
  free(bech);
  memset(seckey, 0, 32);
  return self;
}

GNostrNip19 *
gnostr_nip19_encode_note(const gchar *event_id_hex, GError **error)
{
  g_return_val_if_fail(event_id_hex != NULL, NULL);

  guint8 event_id[32];
  if (!hex_to_bytes(event_id_hex, event_id, 32)) {
    g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_KEY,
                "Invalid event ID hex (expected 64 hex chars): %s", event_id_hex);
    return NULL;
  }

  char *bech = NULL;
  if (nostr_nip19_encode_note(event_id, &bech) != 0 || bech == NULL) {
    g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                        "Failed to encode note");
    return NULL;
  }

  GNostrNip19 *self = g_object_new(GNOSTR_TYPE_NIP19, NULL);
  self->entity_type = GNOSTR_BECH32_NOTE;
  self->bech32 = g_strdup(bech);
  self->hex = g_strdup(event_id_hex);
  self->event_id = g_strdup(event_id_hex);
  free(bech);
  return self;
}

GNostrNip19 *
gnostr_nip19_encode_nprofile(const gchar    *pubkey_hex,
                              const gchar *const *relays,
                              GError        **error)
{
  g_return_val_if_fail(pubkey_hex != NULL, NULL);

  if (strlen(pubkey_hex) != 64) {
    g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_KEY,
                "Invalid public key hex (expected 64 hex chars): %s", pubkey_hex);
    return NULL;
  }

  size_t relay_count = strv_length(relays);

  NostrProfilePointer pp = {
    .public_key = (char *)pubkey_hex,
    .relays = (char **)relays,
    .relays_count = relay_count,
  };

  char *bech = NULL;
  if (nostr_nip19_encode_nprofile(&pp, &bech) != 0 || bech == NULL) {
    g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                        "Failed to encode nprofile");
    return NULL;
  }

  GNostrNip19 *self = g_object_new(GNOSTR_TYPE_NIP19, NULL);
  self->entity_type = GNOSTR_BECH32_NPROFILE;
  self->bech32 = g_strdup(bech);
  self->pubkey = g_strdup(pubkey_hex);
  self->relays = g_strdupv((gchar **)relays);
  free(bech);
  return self;
}

GNostrNip19 *
gnostr_nip19_encode_nevent(const gchar    *event_id_hex,
                            const gchar *const *relays,
                            const gchar    *author_hex,
                            gint            kind,
                            GError        **error)
{
  g_return_val_if_fail(event_id_hex != NULL, NULL);

  if (strlen(event_id_hex) != 64) {
    g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_KEY,
                "Invalid event ID hex (expected 64 hex chars): %s", event_id_hex);
    return NULL;
  }

  if (author_hex != NULL && strlen(author_hex) != 64) {
    g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_KEY,
                "Invalid author hex (expected 64 hex chars): %s", author_hex);
    return NULL;
  }

  size_t relay_count = strv_length(relays);

  NostrEventPointer ep = {
    .id = (char *)event_id_hex,
    .relays = (char **)relays,
    .relays_count = relay_count,
    .author = (char *)author_hex,
    .kind = kind,
  };

  char *bech = NULL;
  if (nostr_nip19_encode_nevent(&ep, &bech) != 0 || bech == NULL) {
    g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                        "Failed to encode nevent");
    return NULL;
  }

  GNostrNip19 *self = g_object_new(GNOSTR_TYPE_NIP19, NULL);
  self->entity_type = GNOSTR_BECH32_NEVENT;
  self->bech32 = g_strdup(bech);
  self->event_id = g_strdup(event_id_hex);
  self->author = g_strdup(author_hex);
  self->kind = kind;
  self->relays = g_strdupv((gchar **)relays);
  free(bech);
  return self;
}

GNostrNip19 *
gnostr_nip19_encode_naddr(const gchar    *identifier,
                           const gchar    *author_hex,
                           gint            kind,
                           const gchar *const *relays,
                           GError        **error)
{
  g_return_val_if_fail(identifier != NULL, NULL);
  g_return_val_if_fail(author_hex != NULL, NULL);

  if (strlen(author_hex) != 64) {
    g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_KEY,
                "Invalid author hex (expected 64 hex chars): %s", author_hex);
    return NULL;
  }

  if (kind < 0) {
    g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                        "naddr requires a non-negative kind");
    return NULL;
  }

  size_t relay_count = strv_length(relays);

  NostrEntityPointer ap = {
    .public_key = (char *)author_hex,
    .kind = kind,
    .identifier = (char *)identifier,
    .relays = (char **)relays,
    .relays_count = relay_count,
  };

  char *bech = NULL;
  if (nostr_nip19_encode_naddr(&ap, &bech) != 0 || bech == NULL) {
    g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                        "Failed to encode naddr");
    return NULL;
  }

  GNostrNip19 *self = g_object_new(GNOSTR_TYPE_NIP19, NULL);
  self->entity_type = GNOSTR_BECH32_NADDR;
  self->bech32 = g_strdup(bech);
  self->pubkey = g_strdup(author_hex);
  self->author = g_strdup(author_hex);
  self->kind = kind;
  self->identifier = g_strdup(identifier);
  self->relays = g_strdupv((gchar **)relays);
  free(bech);
  return self;
}

GNostrNip19 *
gnostr_nip19_encode_nrelay(const gchar *const *relays, GError **error)
{
  g_return_val_if_fail(relays != NULL, NULL);

  size_t count = strv_length(relays);
  if (count == 0) {
    g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                        "nrelay requires at least one relay URL");
    return NULL;
  }

  char *bech = NULL;
  if (nostr_nip19_encode_nrelay_multi(relays, count, &bech) != 0 || bech == NULL) {
    g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED,
                        "Failed to encode nrelay");
    return NULL;
  }

  GNostrNip19 *self = g_object_new(GNOSTR_TYPE_NIP19, NULL);
  self->entity_type = GNOSTR_BECH32_NRELAY;
  self->bech32 = g_strdup(bech);
  self->relays = g_strdupv((gchar **)relays);
  free(bech);
  return self;
}

/* ── Inspection ──────────────────────────────────────────────────── */

GNostrBech32Type
gnostr_nip19_inspect(const gchar *bech32)
{
  g_return_val_if_fail(bech32 != NULL, GNOSTR_BECH32_UNKNOWN);

  NostrBech32Type ctype;
  if (nostr_nip19_inspect(bech32, &ctype) != 0)
    return GNOSTR_BECH32_UNKNOWN;

  return (GNostrBech32Type)ctype;
}

/* ── Property accessors ──────────────────────────────────────────── */

GNostrBech32Type
gnostr_nip19_get_entity_type(GNostrNip19 *self)
{
  g_return_val_if_fail(GNOSTR_IS_NIP19(self), GNOSTR_BECH32_UNKNOWN);
  return self->entity_type;
}

const gchar *
gnostr_nip19_get_bech32(GNostrNip19 *self)
{
  g_return_val_if_fail(GNOSTR_IS_NIP19(self), NULL);
  return self->bech32;
}

const gchar *
gnostr_nip19_get_pubkey(GNostrNip19 *self)
{
  g_return_val_if_fail(GNOSTR_IS_NIP19(self), NULL);
  return self->pubkey;
}

const gchar *
gnostr_nip19_get_event_id(GNostrNip19 *self)
{
  g_return_val_if_fail(GNOSTR_IS_NIP19(self), NULL);
  return self->event_id;
}

const gchar *
gnostr_nip19_get_author(GNostrNip19 *self)
{
  g_return_val_if_fail(GNOSTR_IS_NIP19(self), NULL);
  return self->author;
}

gint
gnostr_nip19_get_kind(GNostrNip19 *self)
{
  g_return_val_if_fail(GNOSTR_IS_NIP19(self), -1);
  return self->kind;
}

const gchar *
gnostr_nip19_get_identifier(GNostrNip19 *self)
{
  g_return_val_if_fail(GNOSTR_IS_NIP19(self), NULL);
  return self->identifier;
}

const gchar *const *
gnostr_nip19_get_relays(GNostrNip19 *self)
{
  g_return_val_if_fail(GNOSTR_IS_NIP19(self), NULL);
  return (const gchar *const *)self->relays;
}
