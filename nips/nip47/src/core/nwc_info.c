#include "nostr/nip47/nwc_info.h"
#include "nostr/nip47/nwc_envelope.h"
/* core nostr primitives */
#include "nostr-event.h"
#include "nostr-tag.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>


int nostr_nwc_info_build(const char *pubkey,
                         long long created_at,
                         const char **methods,
                         size_t methods_count,
                         const char **encryptions,
                         size_t enc_count,
                         int notifications,
                         char **out_event_json) {
  if (!out_event_json) return -1;
  *out_event_json = NULL;
  if (!methods || methods_count == 0) return -1;

  int rc = -1;
  NostrEvent *ev = nostr_event_new();
  if (!ev) return -1;

  /* kind 13194: NIP-47 Info */
  nostr_event_set_kind(ev, 13194);
  if (pubkey && *pubkey) nostr_event_set_pubkey(ev, pubkey);
  if (created_at <= 0) created_at = (long long)time(NULL);
  nostr_event_set_created_at(ev, (int64_t)created_at);

  /* content: {"methods":[...]} */
  {
    /* estimate buffer */
    size_t cap = 32 + methods_count * 16;
    for (size_t i = 0; i < methods_count; i++) cap += strlen(methods[i]) + 3;
    char *buf = (char *)malloc(cap);
    if (!buf) goto out;
    size_t len = 0;
    strcpy(buf, "{\"methods\":["); len = strlen(buf);
    for (size_t i = 0; i < methods_count; i++) {
      if (i) { buf[len++] = ','; buf[len] = '\0'; }
      /* naive JSON escape: reuse nostr_escape_string if exposed; else minimal */
      /* Here we assume methods are simple tokens (per spec examples). */
      size_t mlen = strlen(methods[i]);
      buf[len++] = '"';
      memcpy(buf + len, methods[i], mlen); len += mlen;
      buf[len++] = '"';
      buf[len] = '\0';
    }
    strcpy(buf + len, "]}");
    nostr_event_set_content(ev, buf);
    free(buf);
  }

  /* tags: one ["encryption", "<scheme> <scheme> ..."] — NIP-47 carries the
   * supported schemes as a single space-separated value (nostrc-iq04) — and
   * ["notifications", "true"|"false"]. */
  {
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) goto out;
    nostr_tags_reserve(tags, 2);
    if (encryptions && enc_count > 0) {
      size_t cap = 1;
      for (size_t i = 0; i < enc_count; i++)
        cap += (encryptions[i] ? strlen(encryptions[i]) : 0) + 1;
      char *joined = (char *)malloc(cap);
      if (!joined) { nostr_tags_free(tags); goto out; }
      size_t len = 0;
      for (size_t i = 0; i < enc_count; i++) {
        if (!encryptions[i] || !*encryptions[i]) continue;
        /* Normalise the legacy spelling so wallets parsing strictly match. */
        NostrNwcEncryption enc;
        const char *label = nostr_nwc_encryption_from_label(encryptions[i], &enc) == 0
                              ? nostr_nwc_encryption_label(enc) : encryptions[i];
        size_t n = strlen(label);
        if (len + n + 2 > cap) {
          char *grown = (char *)realloc(joined, len + n + 2);
          if (!grown) { free(joined); nostr_tags_free(tags); goto out; }
          joined = grown; cap = len + n + 2;
        }
        if (len) joined[len++] = ' ';
        memcpy(joined + len, label, n);
        len += n;
      }
      joined[len] = '\0';
      NostrTag *et = len ? nostr_tag_new("encryption", joined, NULL) : NULL;
      free(joined);
      if (len && !et) { nostr_tags_free(tags); goto out; }
      if (et) nostr_tags_append(tags, et);
    }
    NostrTag *t = nostr_tag_new("notifications", notifications ? "true" : "false", NULL);
    if (!t) { nostr_tags_free(tags); goto out; }
    nostr_tags_append(tags, t);
    nostr_event_set_tags(ev, tags); /* takes ownership */
  }

  /* Serialize to JSON */
  {
    char *json = nostr_event_serialize(ev);
    if (!json) goto out;
    *out_event_json = json;
  }
  rc = 0;

out:
  nostr_event_free(ev);
  return rc;
}

int nostr_nwc_info_parse(const char *event_json,
                         char ***out_methods,
                         size_t *out_methods_count,
                         char ***out_encryptions,
                         size_t *out_enc_count,
                         int *out_notifications) {
  if (!event_json) return -1;
  int rc = -1;
  char *content = NULL;
  char **methods = NULL; size_t methods_n = 0;
  char **encs = NULL; size_t encs_n = 0;
  int notifications = 0;

  /* Extract content string directly and parse methods from it */
  if (nostr_json_get_string(event_json, "content", &content) != 0 || !content) goto out;
  if (nostr_json_get_string_array(content, "methods", &methods, &methods_n) != 0 || methods_n == 0) goto out;

  /* Extract tags by deserializing event (so we can iterate tags conveniently) */
  NostrEvent *ev = nostr_event_new();
  if (!ev) goto out;
  if (nostr_event_deserialize(ev, event_json) != 0) { nostr_event_free(ev); goto out; }
  NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
  if (tags) {
    /* Each `encryption` tag value is a space-separated scheme list (NIP-47);
     * older nostrc wallets emitted one tag per scheme. Accept both by
     * splitting every value into tokens. */
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
      NostrTag *tag = nostr_tags_get(tags, i);
      const char *k = nostr_tag_get_key(tag);
      if (!k || nostr_tag_size(tag) < 2) continue;
      const char *v = nostr_tag_get_value(tag);
      if (strcmp(k, "notifications") == 0) {
        notifications = (v && strcmp(v, "true") == 0) ? 1 : 0;
        continue;
      }
      if (strcmp(k, "encryption") != 0 || !v) continue;
      const char *p = v;
      while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (p == s) continue;
        char **grown = (char **)realloc(encs, (encs_n + 1) * sizeof(char *));
        if (!grown) { nostr_event_free(ev); goto out; }
        encs = grown;
        encs[encs_n] = strndup(s, (size_t)(p - s));
        if (!encs[encs_n]) { nostr_event_free(ev); goto out; }
        encs_n++;
      }
    }
  }
  nostr_event_free(ev);

  if (out_methods) { *out_methods = methods; methods = NULL; }
  if (out_methods_count) { *out_methods_count = methods_n; }
  if (out_encryptions) { *out_encryptions = encs; encs = NULL; }
  if (out_enc_count) { *out_enc_count = encs_n; }
  if (out_notifications) { *out_notifications = notifications; }
  rc = 0;

out:
  if (content) free(content);
  if (methods) {
    for (size_t i = 0; i < methods_n; i++) free(methods[i]);
    free(methods);
  }
  if (encs) {
    for (size_t i = 0; i < encs_n; i++) free(encs[i]);
    free(encs);
  }
  return rc;
}
