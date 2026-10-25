/* SPDX-License-Identifier: MIT */
#include "test_helpers.h"
#include <porthome/dir-node.h>
#include <porthome/cbor.h>

static void drop(porthome_dir_node *n) { porthome_dir_node_dispose(n); free(n); }
static void refuse(const uint8_t *b, size_t n) {
    porthome_dir_node *out = (void *)1;
    assert(porthome_dir_node_decode(b,n,&out) < 0 && !out);
}
static size_t tag_at(const uint8_t *b, size_t n, char c) {
    for (size_t i = 0; i+48 <= n; ++i) {
        size_t j = 0; while (j < 48 && b[i+j] == (uint8_t)c) ++j;
        if (j == 48) return i;
    }
    abort();
}
int main(void) {
    porthome_dir_node empty = {.epoch=1}, *out = NULL;
    uint8_t *bytes = NULL; size_t len = 0;
    assert(porthome_dir_node_encode(&empty,&bytes,&len) == 0);
    expect_hex(bytes,len,"a40002015820000000000000000000000000000000000000000000000000000000000000000002010380");
    assert(porthome_dir_node_decode(bytes,len,&out) == 0);
    assert(out->epoch == 1 && !out->entries_len && !out->vault); drop(out);
    uint8_t bad[128]; assert(len == 42);
    const struct { size_t offset; uint8_t value; } mutations[] = {
        {0,0xa3},{2,1},{3,0},{5,31},{38,3},{39,0},{39,0x20},{40,2},{40,4},{41,0x9f}
    };
    for (size_t i = 0; i < sizeof(mutations)/sizeof(mutations[0]); ++i) {
        memcpy(bad,bytes,len); bad[mutations[i].offset] = mutations[i].value; refuse(bad,len);
    }
    memcpy(bad,bytes,2); bad[2]=0x18; memcpy(bad+3,bytes+2,len-2); refuse(bad,len+1);
    memcpy(bad,bytes,len); bad[len]=0; refuse(bad,len+1);
    free(bytes);

    porthome_node_chunk chunks[2] = {
        {.sha256={0x21},.size_sealed=61,.chunk_key_id=1},
        {.sha256={0x22},.size_sealed=125,.chunk_key_id=3}
    };
    porthome_node_entry entries[4] = {
        {.kind=PORTHOME_KIND_DIR,.mode=0700,.mtime_ns=123,.node_id={0x31},.child_epoch=7,
         .manifest_sha256={0x32},.key_override={0x33},.has_key_override=1},
        {.kind=PORTHOME_KIND_FILE,.mode=0600,.mtime_ns=456,.size_plain=85,.chunks=chunks,.chunks_len=2},
        {.kind=PORTHOME_KIND_SYMLINK,.mode=0777,.symlink_target="dir/file"},
        {.kind=PORTHOME_KIND_DIR,.mode=0755,.node_id={0x41},.child_epoch=1,.manifest_sha256={0x42}}
    };
    for (size_t i = 0; i < 4; ++i) { memset(entries[i].name_enc,'a'+(int)i,48); entries[i].name_enc[48]=0; }
    uint8_t writers[2][32] = {{0x51},{0x52}}, readers[1][32] = {{0x61}};
    porthome_vault_info vault = {.vault_id={0x11},.owner={0x12},.writers=writers,.writers_len=2,
        .readers=readers,.readers_len=1,.grants_rev=UINT64_MAX};
    porthome_dir_node node = {.node_id={0x13},.epoch=3,.vault=&vault,.entries=entries,.entries_len=4};
    assert(porthome_dir_node_encode(&node,&bytes,&len) == 0);
    assert(porthome_dir_node_decode(bytes,len,&out) == 0);
    assert(!memcmp(out->node_id,node.node_id,32) && out->epoch==3 && out->entries_len==4);
    assert(out->vault && out->vault->grants_rev==UINT64_MAX && out->vault->writers_len==2 && out->vault->readers_len==1);
    assert(!memcmp(out->vault->vault_id,vault.vault_id,32) && !memcmp(out->vault->owner,vault.owner,32));
    assert(!memcmp(out->vault->writers,writers,sizeof(writers)) && !memcmp(out->vault->readers,readers,sizeof(readers)));
    assert(out->entries[0].has_key_override && out->entries[0].child_epoch==7);
    assert(!memcmp(out->entries[0].node_id,entries[0].node_id,32));
    assert(!memcmp(out->entries[0].key_override,entries[0].key_override,32));
    assert(!memcmp(out->entries[0].manifest_sha256,entries[0].manifest_sha256,32));
    assert(out->entries[1].chunks_len==2 && out->entries[1].chunks[0].chunk_key_id==1 && out->entries[1].chunks[1].chunk_key_id==3);
    assert(out->entries[1].size_plain==85 && out->entries[1].mtime_ns==456 && out->entries[1].mode==0600);
    assert(!strcmp(out->entries[2].symlink_target,"dir/file") && !out->entries[3].has_key_override);
    uint8_t *again = NULL; size_t again_len = 0;
    assert(porthome_dir_node_encode(out,&again,&again_len)==0 && again_len==len && !memcmp(again,bytes,len));
    free(again); drop(out);
    for (size_t i = 0; i < len; ++i) refuse(bytes,i);
    uint8_t *copy = malloc(len+1); assert(copy);
    memcpy(copy,bytes,len); copy[len]=0; refuse(copy,len+1);
    size_t b = tag_at(bytes,len,'b');
    memcpy(copy,bytes,len); memset(copy+b,'a',48); refuse(copy,len); /* duplicate name */
    memcpy(copy,bytes,len); copy[b]='A'; refuse(copy,len);
    memcpy(copy,bytes,len); copy[b+49]=4; refuse(copy,len); /* unknown kind */
    free(copy); free(bytes);
    entries[2].symlink_target="caf\xc3\xa9/file";
    assert(porthome_dir_node_encode(&node,&bytes,&len)==0);
    assert(porthome_dir_node_decode(bytes,len,&out)==0 && !strcmp(out->entries[2].symlink_target,entries[2].symlink_target)); drop(out); free(bytes);
    entries[2].symlink_target="bad\xc0\xaf";
    assert(porthome_dir_node_encode(&node,&bytes,&len)==PORTHOME_ERR_ARG && !bytes);
    entries[2].symlink_target="dir/file";

    uint8_t root[32], key[32]; fixture(root,key);
    porthome_ring_entry_t ring_entries[2] = {{.epoch=1},{.epoch=3}};
    memcpy(ring_entries[0].node_key,root,32); memcpy(ring_entries[1].node_key,key,32);
    porthome_ring_t ring = {ring_entries,2};
    uint8_t *sealed = NULL, address[32], computed[32]; size_t sealed_len = 0;
    assert(porthome_dir_node_seal(&node,key,&sealed,&sealed_len,address)==0);
    assert(porthome_sha256(sealed,sealed_len,computed)==0 && !memcmp(computed,address,32));
    assert(sealed[0]==2 && porthome_dir_node_open(&ring,3,sealed,sealed_len,&out)==0); drop(out);
    assert(porthome_dir_node_open(&ring,2,sealed,sealed_len,&out)==PORTHOME_ERR_EPOCH && !out);
    assert(porthome_dir_node_open(&ring,1,sealed,sealed_len,&out)==PORTHOME_ERR_CRYPTO && !out);
    sealed[sealed_len-1]^=1;
    assert(porthome_dir_node_open(&ring,3,sealed,sealed_len,&out)==PORTHOME_ERR_CRYPTO && !out);
    free(sealed);
    node.epoch=1; /* Same encryption key but inconsistent authenticated epoch. */
    assert(porthome_dir_node_seal(&node,key,&sealed,&sealed_len,NULL)==0);
    assert(porthome_dir_node_open(&ring,3,sealed,sealed_len,&out)==PORTHOME_ERR_EPOCH && !out); free(sealed);
    node.epoch=0; assert(porthome_dir_node_encode(&node,&bytes,&len)==PORTHOME_ERR_ARG && !bytes && !len); node.epoch=3;
    entries[0].child_epoch=0; assert(porthome_dir_node_encode(&node,&bytes,&len)<0); entries[0].child_epoch=7;
    chunks[0].chunk_key_id=0; assert(porthome_dir_node_encode(&node,&bytes,&len)<0); chunks[0].chunk_key_id=1;
    /* Do not carry the legacy Phase-1 fixture's 64-chunk cap into vaults,
     * or reject referenced 4 MiB v1 chunks during no-reseal migration. */
    porthome_node_chunk historical[65] = {0};
    for (size_t i = 0; i < 65; ++i) { historical[i].chunk_key_id=1; historical[i].size_sealed=4u*1024u*1024u+29; }
    entries[1].chunks=historical; entries[1].chunks_len=65; entries[1].size_plain=65u*4u*1024u*1024u;
    assert(porthome_dir_node_encode(&node,&bytes,&len)==0 && porthome_dir_node_decode(bytes,len,&out)==0);
    assert(out->entries[1].chunks_len==65 && out->entries[1].chunks[0].size_sealed==4u*1024u*1024u+29); drop(out); free(bytes);
    entries[1].chunks_len=PORTHOME_NODE_MAX_CHUNKS+1;
    assert(porthome_dir_node_encode(&node,&bytes,&len)==PORTHOME_ERR_TOO_LARGE);
    entries[1].chunks=chunks; entries[1].chunks_len=2; entries[1].size_plain=85;
    entries[2].symlink_target="../outside"; assert(porthome_dir_node_encode(&node,&bytes,&len)<0); entries[2].symlink_target="dir/file";
    vault.writers_len=PORTHOME_VAULT_MAX_MEMBERS+1; assert(porthome_dir_node_encode(&node,&bytes,&len)==PORTHOME_ERR_TOO_LARGE); vault.writers_len=2;
    /* Original flat v1 and flat name-schema v2 remain on their old codec. */
    for (unsigned version = 1; version <= 2; ++version) {
        porthome_manifest legacy, *parsed = NULL;
        assert((version==1 ? porthome_manifest_init(&legacy,root) : porthome_manifest_init_v2(&legacy,root))==0);
        assert(porthome_manifest_encode_sealed(&legacy,key,&sealed,&sealed_len)==0);
        assert(sealed[0]==1 && porthome_manifest_decode_sealed(sealed,sealed_len,key,&parsed)==0);
        assert(parsed->version==version); porthome_manifest_dispose(parsed); free(parsed);
        assert(porthome_dir_node_open(&ring,3,sealed,sealed_len,&out)==PORTHOME_ERR_VERSION && !out);
        porthome_manifest_dispose(&legacy); free(sealed);
    }
    return 0;
}
