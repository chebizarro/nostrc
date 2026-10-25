#pragma once
#include <blossom/client.h>
int nostrfs_blossom_upload(const char *, const char *, const blossom_signer_t *, char **);
int nostrfs_blossom_client_head(const char *, const char *);
int nostrfs_blossom_fetch(const char *, const char *, const char *);
