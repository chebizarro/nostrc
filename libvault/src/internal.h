/* SPDX-License-Identifier: MIT — not installed. */
#ifndef VAULT_INTERNAL_H
#define VAULT_INTERNAL_H
#include <vault/vault.h>
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>
#define VAULT_MAX_EPOCHS 4096u
int vault_ring_valid(const porthome_ring_t *);
const porthome_ring_entry_t *vault_ring_latest(const vault_ring *);
int vault_node_clone(const porthome_dir_node *, porthome_dir_node **);
int vault_head_clone(const porthome_head *, porthome_head **);
typedef struct {
    char *path;
    uint8_t digest[32];
} vault_path;
struct vault_record {
    uint8_t writer[32], head_hash[32];
    porthome_head *head;
    porthome_dir_node *root;
    vault_path *paths;
    size_t paths_len, paths_cap;
};
/* Private strict JSON cursor; schema strings are unescaped ASCII. */
typedef struct {
    const char *p, *end;
} vj_reader;
void vj_ws(vj_reader *);
int vj_take(vj_reader *, char);
int vj_string(vj_reader *, char *, size_t);
int vj_field(vj_reader *, const char *const *, size_t);
int vj_uint(vj_reader *, uint64_t *);
int vj_hex(vj_reader *, uint8_t[32]);
int vj_urls(vj_reader *, char ***, size_t *, int relay);
int vj_url_valid(const char *, int relay);
void vj_urls_free(char **, size_t);
int vj_append(char *, size_t, size_t *, const char *, ...);
int vj_write_urls(char *, size_t, size_t *, char *const *, size_t, int relay);
int vj_init(const char *, vj_reader *);
int vj_end(vj_reader *);
#endif
