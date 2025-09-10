#ifndef NOSTR_BLOSSOM_H
#define NOSTR_BLOSSOM_H

#ifdef __cplusplus
extern "C" {
#endif

int nh_blossom_head(const char *base_url, const char *cid);
int nh_blossom_fetch(const char *base_url, const char *cid, const char *dest_path);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_BLOSSOM_H */
