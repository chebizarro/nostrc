#include "nostr/nip47/nwc_info.h"
#include "nostr/nip47/nwc_envelope.h"
/* core nostr primitives */
#include "nostr-event.h"
#include "nostr-tag.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static int is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* A token that can be joined with spaces and split back unchanged. */
static int token_ok(const char *s) {
  if (!s || !*s) return 0;
  for (const char *p = s; *p; p++)
    if (is_space(*p)) return 0;
  return 1;
}

static void free_list(char **items, size_t n) {
  if (!items) return;
  for (size_t i = 0; i < n; i++) free(items[i]);
  free(items);
}

/* Append strndup(s, n) to *items unless already present. */
static int list_add(char ***items, size_t *n, const char *s, size_t len) {
  for (size_t i = 0; i < *n; i++)
    if (strlen((*items)[i]) == len && memcmp((*items)[i], s, len) == 0) return 0;
  char **grown = (char **)realloc(*items, (*n + 1) * sizeof(char *));
  if (!grown) return -1;
  *items = grown;
  if (!(grown[*n] = strndup(s, len))) return -1;
  (*n)++;
  return 0;
}

/* Split a whitespace-separated list, dropping duplicates. */
static int list_split(const char *value, char ***items, size_t *n) {
  const char *p = value;
  while (p && *p) {
    while (is_space(*p)) p++;
    const char *s = p;
    while (*p && !is_space(*p)) p++;
    if (p > s && list_add(items, n, s, (size_t)(p - s)) != 0) return -1;
  }
  return 0;
}

/* Join tokens with single spaces; NULL on allocation failure. */
static char *list_join(const char **items, size_t n, const char *extra) {
  size_t cap = 1 + (extra ? strlen(extra) + 1 : 0);
  for (size_t i = 0; i < n; i++) cap += strlen(items[i]) + 1;
  char *out = (char *)malloc(cap);
  if (!out) return NULL;
  size_t len = 0;
  for (size_t i = 0; i < n + (extra ? 1 : 0); i++) {
    const char *s = i < n ? items[i] : extra;
    size_t sl = strlen(s);
    if (len) out[len++] = ' ';
    memcpy(out + len, s, sl);
    len += sl;
  }
  out[len] = '\0';
  return out;
}

int nostr_nwc_info_build(const char *pubkey,
                         long long created_at,
                         const char **methods,
                         size_t methods_count,
                         const char **encryptions,
                         size_t enc_count,
                         const char **notification_types,
                         size_t notif_count,
                         char **out_event_json) {
  if (!out_event_json) return -1;
  *out_event_json = NULL;
  if (!methods || methods_count == 0) return -1;
  if (notif_count && !notification_types) return -1;

  int has_notifications_cap = 0;
  for (size_t i = 0; i < methods_count; i++) {
    if (!token_ok(methods[i])) return -1;
    if (strcmp(methods[i], "notifications") == 0) has_notifications_cap = 1;
  }
  for (size_t i = 0; i < notif_count; i++)
    if (!token_ok(notification_types[i])) return -1;

  int rc = -1;
  char *content = NULL, *enc_value = NULL, *notif_value = NULL;
  NostrEvent *ev = nostr_event_new();
  if (!ev) return -1;

  /* kind 13194: NIP-47 Info */
  nostr_event_set_kind(ev, 13194);
  if (pubkey && *pubkey) nostr_event_set_pubkey(ev, pubkey);
  if (created_at <= 0) created_at = (long long)time(NULL);
  nostr_event_set_created_at(ev, (int64_t)created_at);

  /* content: the plaintext space-separated capability list. Advertising
   * notification types implies the `notifications` capability. */
  content = list_join(methods, methods_count,
                      (notif_count && !has_notifications_cap) ? "notifications" : NULL);
  if (!content) goto out;
  nostr_event_set_content(ev, content);

  NostrTags *tags = nostr_tags_new(0);
  if (!tags) goto out;
  nostr_event_set_tags(ev, tags); /* takes ownership */

  /* ["encryption", "<scheme> <scheme> ..."] (nostrc-iq04), legacy
   * spellings normalised so strict wallets match. */
  if (encryptions && enc_count > 0) {
    const char **labels = (const char **)calloc(enc_count, sizeof(char *));
    if (!labels) goto out;
    size_t n = 0;
    for (size_t i = 0; i < enc_count; i++) {
      if (!token_ok(encryptions[i])) continue;
      NostrNwcEncryption enc;
      labels[n++] = nostr_nwc_encryption_from_label(encryptions[i], &enc) == 0
                      ? nostr_nwc_encryption_label(enc) : encryptions[i];
    }
    enc_value = n ? list_join(labels, n, NULL) : NULL;
    free(labels);
    if (n && !enc_value) goto out;
    if (enc_value) {
      NostrTag *t = nostr_tag_new("encryption", enc_value, NULL);
      if (!t) goto out;
      nostr_tags_append(tags, t);
    }
  }

  /* ["notifications", "payment_received payment_sent"] — only when the
   * wallet sends notifications (NIP-47 SHOULD). */
  if (notif_count) {
    notif_value = list_join(notification_types, notif_count, NULL);
    if (!notif_value) goto out;
    NostrTag *t = nostr_tag_new("notifications", notif_value, NULL);
    if (!t) goto out;
    nostr_tags_append(tags, t);
  }

  *out_event_json = nostr_event_serialize(ev);
  if (*out_event_json) rc = 0;

out:
  free(content);
  free(enc_value);
  free(notif_value);
  nostr_event_free(ev);
  return rc;
}

int nostr_nwc_info_parse(const char *event_json,
                         char ***out_methods,
                         size_t *out_methods_count,
                         char ***out_encryptions,
                         size_t *out_enc_count,
                         char ***out_notification_types,
                         size_t *out_notif_count) {
  if (!event_json) return -1;
  int rc = -1;
  char **methods = NULL; size_t methods_n = 0;
  char **encs = NULL; size_t encs_n = 0;
  char **notifs = NULL; size_t notifs_n = 0;

  NostrEvent *ev = nostr_event_new();
  if (!ev) return -1;
  if (nostr_event_deserialize(ev, event_json) != 0) goto out;

  /* content: NIP-47 plaintext capability list; older nostrc wallets wrote
   * {"methods":[...]}, still accepted. */
  const char *content = nostr_event_get_content(ev);
  if (!content) goto out;
  const char *first = content;
  while (is_space(*first)) first++;
  if (*first == '{') {
    char **legacy = NULL; size_t legacy_n = 0;
    if (nostr_json_get_string_array(content, "methods", &legacy, &legacy_n) != 0) goto out;
    for (size_t i = 0; i < legacy_n; i++) {
      if (token_ok(legacy[i]) &&
          list_add(&methods, &methods_n, legacy[i], strlen(legacy[i])) != 0) {
        free_list(legacy, legacy_n);
        goto out;
      }
    }
    free_list(legacy, legacy_n);
  } else if (list_split(content, &methods, &methods_n) != 0) {
    goto out;
  }
  if (methods_n == 0) goto out;

  NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    const char *k = nostr_tag_get_key(tag);
    const char *v = nostr_tag_size(tag) >= 2 ? nostr_tag_get_value(tag) : NULL;
    if (!k || !v) continue;
    /* Values are space-separated lists; older nostrc wallets emitted one
     * encryption tag per scheme, so every tag is split and merged. */
    if (strcmp(k, "encryption") == 0) {
      if (list_split(v, &encs, &encs_n) != 0) goto out;
    } else if (strcmp(k, "notifications") == 0) {
      /* Legacy nostrc boolean ("true"/"false") names no types. */
      if (strcmp(v, "true") == 0 || strcmp(v, "false") == 0) continue;
      if (list_split(v, &notifs, &notifs_n) != 0) goto out;
    }
  }

  if (out_methods) { *out_methods = methods; methods = NULL; }
  if (out_methods_count) *out_methods_count = methods_n;
  if (out_encryptions) { *out_encryptions = encs; encs = NULL; }
  if (out_enc_count) *out_enc_count = encs_n;
  if (out_notification_types) { *out_notification_types = notifs; notifs = NULL; }
  if (out_notif_count) *out_notif_count = notifs_n;
  rc = 0;

out:
  nostr_event_free(ev);
  free_list(methods, methods_n);
  free_list(encs, encs_n);
  free_list(notifs, notifs_n);
  return rc;
}
