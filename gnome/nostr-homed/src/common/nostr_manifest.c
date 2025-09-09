#include "nostr_manifest.h"
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

static char *xstrdup(const char *s){ if(!s) return NULL; size_t n=strlen(s)+1; char *p=malloc(n); if(p) memcpy(p,s,n); return p; }

int nh_manifest_parse_json(const char *json, nh_manifest *out){
  if (!json || !out) return -1;
  memset(out, 0, sizeof(*out));
  json_error_t err; json_t *root = json_loads(json, 0, &err);
  if (!root || !json_is_object(root)) { if (root) json_decref(root); return -1; }
  json_t *v = json_object_get(root, "version");
  if (!json_is_integer(v)) { json_decref(root); return -1; }
  out->version = (int)json_integer_value(v);

  json_t *entries = json_object_get(root, "entries");
  if (entries && json_is_array(entries)) {
    size_t n = json_array_size(entries);
    out->entries = (nh_entry*)calloc(n, sizeof(nh_entry)); out->entries_len = n;
    for (size_t i=0;i<n;i++){
      json_t *e = json_array_get(entries, i);
      if (!json_is_object(e)) continue;
      json_t *p = json_object_get(e, "path");
      json_t *c = json_object_get(e, "cid");
      json_t *sz = json_object_get(e, "size");
      json_t *meta = json_object_get(e, "meta");
      if (json_is_string(p)) out->entries[i].path = xstrdup(json_string_value(p));
      if (json_is_string(c)) out->entries[i].cid = xstrdup(json_string_value(c));
      if (json_is_integer(sz)) out->entries[i].size = (uint64_t)json_integer_value(sz);
      if (meta && json_is_object(meta)){
        json_t *mode=json_object_get(meta,"mode"); json_t *mtime=json_object_get(meta,"mtime");
        json_t *uid=json_object_get(meta,"uid"); json_t *gid=json_object_get(meta,"gid");
        if (json_is_integer(mode)) out->entries[i].mode=(uint32_t)json_integer_value(mode);
        if (json_is_integer(mtime)) out->entries[i].mtime=(uint64_t)json_integer_value(mtime);
        if (json_is_integer(uid)) out->entries[i].uid=(uint32_t)json_integer_value(uid);
        if (json_is_integer(gid)) out->entries[i].gid=(uint32_t)json_integer_value(gid);
      }
    }
  }
  json_t *links = json_object_get(root, "links");
  if (links && json_is_array(links)){
    size_t n = json_array_size(links);
    out->links = (nh_link*)calloc(n, sizeof(nh_link)); out->links_len = n;
    for (size_t i=0;i<n;i++){
      json_t *L = json_array_get(links, i);
      if (!json_is_object(L)) continue;
      json_t *p = json_object_get(L, "path");
      json_t *r = json_object_get(L, "manifest_event_ref");
      if (json_is_string(p)) out->links[i].path = xstrdup(json_string_value(p));
      if (json_is_string(r)) out->links[i].manifest_event_ref = xstrdup(json_string_value(r));
    }
  }
  json_decref(root);
  return 0;
}

void nh_manifest_free(nh_manifest *m){
  if (!m) return;
  for (size_t i=0;i<m->entries_len;i++){ free(m->entries[i].path); free(m->entries[i].cid); }
  free(m->entries); m->entries=NULL; m->entries_len=0;
  for (size_t i=0;i<m->links_len;i++){ free(m->links[i].path); free(m->links[i].manifest_event_ref); }
  free(m->links); m->links=NULL; m->links_len=0;
}
