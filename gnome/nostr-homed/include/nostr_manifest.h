#ifndef NOSTR_MANIFEST_H
#define NOSTR_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char *path;
  char *cid;
  uint64_t size;
  uint32_t mode;
  uint32_t uid;
  uint32_t gid;
  uint64_t mtime;
} nh_entry;

typedef struct {
  char *path;
  char *manifest_event_ref;
} nh_link;

typedef struct {
  int version;
  nh_entry *entries; size_t entries_len;
  nh_link *links; size_t links_len;
} nh_manifest;

int nh_manifest_parse_json(const char *json, nh_manifest *out);
void nh_manifest_free(nh_manifest *m);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_MANIFEST_H */
