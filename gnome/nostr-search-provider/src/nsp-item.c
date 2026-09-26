/* nsp-item.c — see nsp-item.h. */
#include "nsp-item.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "nsp-text.h"

void nsp_item_free(NspItem *it) {
  if (!it) return;
  g_free(it->id_hex);
  g_free(it->pubkey_hex);
  g_free(it->content);
  g_free(it->d_tag);
  g_free(it->title);
  g_free(it->summary);
  g_free(it->name);
  g_free(it->display_name);
  g_free(it->nip05);
  g_free(it->picture);
  g_free(it->about);
  g_free(it->uri);
  g_free(it->haystack);
  g_free(it);
}

gboolean nsp_item_is_profile(const NspItem *it) {
  if (!it) return FALSE;
  return it->bare ? (!it->id_hex && !it->d_tag) : it->kind == 0;
}

static char *str_member(JsonObject *o, const char *name, gsize max) {
  JsonNode *n = o ? json_object_get_member(o, name) : NULL;
  if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING)
    return NULL;
  const char *s = json_node_get_string(n);
  if (!s || !*s || (max && strlen(s) > max)) return NULL;
  return g_strdup(s);
}

static char *first_tag(JsonObject *ev, const char *key) {
  JsonNode *tn = json_object_get_member(ev, "tags");
  if (!tn || !JSON_NODE_HOLDS_ARRAY(tn)) return NULL;
  JsonArray *tags = json_node_get_array(tn);
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonNode *t = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(t)) continue;
    JsonArray *ta = json_node_get_array(t);
    if (json_array_get_length(ta) < 2) continue;
    JsonNode *k = json_array_get_element(ta, 0), *v = json_array_get_element(ta, 1);
    if (JSON_NODE_HOLDS_VALUE(k) && json_node_get_value_type(k) == G_TYPE_STRING &&
        g_strcmp0(json_node_get_string(k), key) == 0 && JSON_NODE_HOLDS_VALUE(v) &&
        json_node_get_value_type(v) == G_TYPE_STRING)
      return g_strdup(json_node_get_string(v));
  }
  return NULL;
}

static void parse_metadata(NspItem *it) {
  g_autoptr(JsonParser) p = json_parser_new();
  if (!it->content || !json_parser_load_from_data(p, it->content, -1, NULL)) return;
  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return;
  JsonObject *o = json_node_get_object(root);
  it->name = str_member(o, "name", 1024);
  it->display_name = str_member(o, "display_name", 1024);
  if (!it->display_name) it->display_name = str_member(o, "displayName", 1024);
  it->nip05 = str_member(o, "nip05", 320);
  if (it->nip05) {
    char *l = g_ascii_strdown(it->nip05, -1);
    g_free(it->nip05);
    it->nip05 = l;
  }
  it->about = str_member(o, "about", 8192);
  char *pic = str_member(o, "picture", NSP_PICTURE_URL_MAX);
  if (pic && g_str_has_prefix(pic, "https://"))
    it->picture = pic;
  else
    g_free(pic); /* only https avatars are ever fetched */
}

static char *build_uri(const NspItem *it) {
  NdTarget t = {0};
  if (nsp_item_is_profile(it)) {
    t.entity = ND_ENTITY_PROFILE;
    t.pubkey_hex = it->pubkey_hex;
    t.kind = 0;
  } else if (it->d_tag && it->kind >= 30000 && it->kind < 40000) {
    t.entity = ND_ENTITY_ADDRESS;
    t.pubkey_hex = it->pubkey_hex;
    t.kind = it->kind;
    t.identifier = it->d_tag;
  } else {
    t.entity = ND_ENTITY_EVENT;
    t.id_hex = it->id_hex;
    t.pubkey_hex = it->pubkey_hex;
    t.kind = it->kind;
  }
  return nd_target_to_uri(&t);
}

static void build_haystack(NspItem *it) {
  g_autoptr(GString) h = g_string_new(NULL);
  const char *parts[] = {it->display_name, it->name, it->nip05, it->about,
                         it->title,        it->summary, it->kind == 0 ? NULL : it->content};
  for (guint i = 0; i < G_N_ELEMENTS(parts); i++)
    if (parts[i]) g_string_append_printf(h, "%s\n", parts[i]);
  it->haystack = nsp_text_fold(h->str);
}

NspItem *nsp_item_new_from_event(const NdEvent *ev) {
  if (!ev || !ev->validated || !ev->json || !ev->id_hex || !ev->pubkey_hex) return NULL;
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, ev->json, -1, NULL)) return NULL;
  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return NULL;
  JsonObject *o = json_node_get_object(root);

  g_autoptr(NspItem) it = g_new0(NspItem, 1);
  it->kind = ev->kind;
  it->id_hex = g_strdup(ev->id_hex);
  it->pubkey_hex = g_strdup(ev->pubkey_hex);
  it->created_at = json_object_get_int_member_with_default(o, "created_at", 0);
  it->content = str_member(o, "content", 0);
  it->d_tag = g_strdup(ev->d_tag);
  if (it->kind == 0) {
    parse_metadata(it);
  } else if (it->kind == 30023) {
    it->title = first_tag(o, "title");
    it->summary = first_tag(o, "summary");
  }
  it->uri = build_uri(it);
  if (!it->uri) return NULL;
  build_haystack(it);
  return g_steal_pointer(&it);
}

NspItem *nsp_item_new_bare(const NdTarget *target, const char *nip05) {
  if (!target) return NULL;
  g_autoptr(NspItem) it = g_new0(NspItem, 1);
  it->bare = TRUE;
  it->kind = target->entity == ND_ENTITY_PROFILE ? 0 : target->kind;
  it->pubkey_hex = g_strdup(target->pubkey_hex);
  if (target->entity == ND_ENTITY_EVENT) it->id_hex = g_strdup(target->id_hex);
  if (target->entity == ND_ENTITY_ADDRESS)
    it->d_tag = g_strdup(target->identifier ? target->identifier : "");
  it->nip05 = g_strdup(nip05);
  NdTarget t = *target;
  t.relays = NULL;
  it->uri = nd_target_to_uri(&t);
  if (!it->uri) return NULL;
  build_haystack(it);
  return g_steal_pointer(&it);
}

char *nsp_item_profile_name(const NspItem *it) {
  if (!it) return NULL;
  const char *cands[] = {it->display_name, it->name};
  for (guint i = 0; i < G_N_ELEMENTS(cands); i++) {
    if (!cands[i]) continue;
    char *s = nsp_text_sanitize(cands[i], NSP_NAME_MAX_CHARS);
    if (*s) return s;
    g_free(s);
  }
  return NULL;
}

/* Short bech32 of the URI's payload ("nevent1abcdefg…uvwxyz"). */
static char *short_uri(const char *uri) {
  const char *b = g_str_has_prefix(uri, "nostr:") ? uri + 6 : uri;
  gsize n = strlen(b);
  return n > 24 ? g_strdup_printf("%.14s\xe2\x80\xa6%s", b, b + n - 6) : g_strdup(b);
}

void nsp_item_meta_text(const NspItem *it, const NspItem *author, gboolean nip05_verified,
                        gint64 now, char **name_out, char **description_out) {
  g_autofree char *npub = it->pubkey_hex ? nsp_text_short_npub(it->pubkey_hex) : NULL;
  g_autofree char *nip05 = it->nip05 ? nsp_text_sanitize(it->nip05, NSP_NAME_MAX_CHARS) : NULL;
  if (nip05 && !*nip05) g_clear_pointer(&nip05, g_free);

  if (it->bare) {
    const char *what = nsp_item_is_profile(it)  ? "Open Nostr profile"
                       : it->kind == 30023      ? "Open Nostr article"
                                                : "Open Nostr note";
    g_autofree char *sh = short_uri(it->uri);
    *name_out = g_strdup(what);
    *description_out = nip05 ? g_strdup_printf("%s%s \xc2\xb7 %s", nip05_verified ? "\xe2\x9c\x93 " : "",
                                                nip05, sh)
                             : g_strdup(sh);
    return;
  }

  if (nsp_item_is_profile(it)) {
    char *name = nsp_item_profile_name(it);
    *name_out = name ? name : g_strdup(npub ? npub : "Nostr profile");
    if (nip05)
      *description_out = g_strdup_printf("%s%s \xc2\xb7 %s", nip05_verified ? "\xe2\x9c\x93 " : "",
                                         nip05, npub ? npub : "");
    else
      *description_out = g_strdup(npub ? npub : "");
    return;
  }

  g_autofree char *who = author ? nsp_item_profile_name(author) : NULL;
  g_autofree char *when = nsp_text_relative_time(it->created_at, now);
  *name_out = g_strdup_printf("%s \xc2\xb7 %s", who ? who : (npub ? npub : "Nostr"), when);
  const char *body = it->content;
  if (it->kind == 30023) body = it->title ? it->title : it->summary ? it->summary : it->content;
  *description_out = nsp_text_sanitize(body, NSP_NOTE_SNIPPET_CHARS);
}
