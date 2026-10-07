#include "gh-display-name.h"
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <string.h>
#include <stdlib.h>

static GhDisplayNameFunc resolver;
static gpointer resolver_data;
static GObject *notifier;

/* The notifier: one instance, one signal ("changed", s). */
typedef struct { GObject parent_instance; } GhDisplayNames;
typedef struct { GObjectClass parent_class; } GhDisplayNamesClass;
static GType gh_display_names_get_type(void);
G_DEFINE_TYPE(GhDisplayNames, gh_display_names, G_TYPE_OBJECT)
static guint changed_signal;
static void gh_display_names_class_init(GhDisplayNamesClass *klass)
{
  changed_signal = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}
static void gh_display_names_init(GhDisplayNames *self) { (void)self; }

void
gh_display_name_set_resolver(GhDisplayNameFunc func, gpointer data)
{
  resolver = func;
  resolver_data = data;
}

const gchar *
gh_display_name_lookup(const gchar *pubkey_hex)
{
  if (!resolver || !pubkey_hex || strlen(pubkey_hex) != 64)
    return NULL;
  const gchar *name = resolver(pubkey_hex, resolver_data);
  return name && *name ? name : NULL;
}

gchar *
gh_display_name_short_npub(const gchar *pubkey_hex)
{
  if (!pubkey_hex)
    return g_strdup("");
  guint8 bytes[32];
  char *npub = NULL;
  if (strlen(pubkey_hex) != 64 || !nostr_hex2bin(bytes, pubkey_hex, sizeof bytes) ||
      nostr_nip19_encode_npub(bytes, &npub) != 0 || !npub)
    return g_strdup(pubkey_hex);
  gsize length = strlen(npub);
  gchar *out = length > 16 ? g_strdup_printf("%.10s…%s", npub, npub + length - 4) : g_strdup(npub);
  free(npub);
  return out;
}

gchar *
gh_display_name_for(const gchar *pubkey_hex)
{
  const gchar *name = gh_display_name_lookup(pubkey_hex);
  return name ? g_strdup(name) : gh_display_name_short_npub(pubkey_hex);
}

GObject *
gh_display_name_get_notifier(void)
{
  if (!notifier)
    notifier = g_object_new(gh_display_names_get_type(), NULL);
  return notifier;
}

void
gh_display_name_changed(const gchar *pubkey_hex)
{
  GObject *n = gh_display_name_get_notifier();
  g_signal_emit(n, changed_signal, 0, pubkey_hex);
}
