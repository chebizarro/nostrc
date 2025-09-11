#ifndef NOSTR_BLOSSOM_H
#define NOSTR_BLOSSOM_H

#ifdef __cplusplus
extern "C" {
#endif

int nh_blossom_head(const char *base_url, const char *cid);
int nh_blossom_fetch(const char *base_url, const char *cid, const char *dest_path);
/* Upload a local file to Blossom; computes CID as sha256 hex of content and PUTs to base_url/cid.
 * On success returns 0 and sets *out_cid to a newly allocated string that caller must free. */
int nh_blossom_upload(const char *base_url, const char *src_path, char **out_cid);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_BLOSSOM_H */
