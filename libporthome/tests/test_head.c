/* SPDX-License-Identifier: MIT */
#include "test_helpers.h"
#include <porthome/head.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr/nip44/nip44.h>
#include <json.h>
#define ZERO "0000000000000000000000000000000000000000000000000000000000000000"
#define SIMPLE "{\"vault_id\":\"" ZERO "\",\"epoch\":1,\"root_manifest\":\"" ZERO "\",\"seq\":0,\"parents\":[],\"grants_rev\":0,\"written_at\":0}"
static const char signer[] = "0000000000000000000000000000000000000000000000000000000000000001";
static void drop(porthome_head *h) { porthome_head_dispose(h); free(h); }
static void refuse(const char *s) {
    porthome_head *out = (void *)1;
    assert(porthome_head_decode((const uint8_t *)s,strlen(s),&out)<0 && !out);
}
static void mutate(const char *s, const char *find, const char *replacement) {
    const char *p = strstr(s,find); assert(p);
    size_t before = (size_t)(p-s), after = strlen(p+strlen(find));
    char *copy = malloc(before+strlen(replacement)+after+1); assert(copy);
    memcpy(copy,s,before); strcpy(copy+before,replacement); strcpy(copy+before+strlen(replacement),p+strlen(find));
    refuse(copy); free(copy);
}
static void resign(NostrEvent *e) { assert(nostr_event_sign(e,signer)==0); }
int main(void) {
    nostr_json_init();
    porthome_head simple = {.epoch=1}, *out = NULL;
    uint8_t *json = NULL; size_t len = 0;
    assert(porthome_head_encode(&simple,&json,&len)==0 && len==strlen(SIMPLE) && !strcmp((char *)json,SIMPLE));
    assert(porthome_head_decode(json,len,&out)==0 && out->epoch==1 && !out->parents_len && !out->has_ref_state); drop(out);
    for (size_t i = 0; i < len; ++i) assert(porthome_head_decode(json,i,&out)<0 && !out);
    const char *bad_epochs[] = {"0","-1","1.0","1e0","01","+1","true","null","\"1\"","18446744073709551616"};
    for (size_t i = 0; i < sizeof(bad_epochs)/sizeof(bad_epochs[0]); ++i) {
        char value[128]; snprintf(value,sizeof(value),"\"epoch\":%s",bad_epochs[i]); mutate(SIMPLE,"\"epoch\":1",value);
    }
    mutate(SIMPLE,"\"epoch\":1","\"epoch\":1,\"epoch\":1");
    mutate(SIMPLE,"\"epoch\":1","\"unknown\":1");
    mutate(SIMPLE,"\"epoch\":1,","");
    mutate(SIMPLE,"\"parents\":[]","\"parents\":null");
    mutate(SIMPLE,"\"written_at\":0","\"written_at\":18446744073709551615");
    mutate(SIMPLE,"\"seq\":0","\"seq\":0,\"ref_state\":null");
    mutate(SIMPLE,ZERO,"g000000000000000000000000000000000000000000000000000000000000000");
    mutate(SIMPLE,ZERO,"00");
    mutate(SIMPLE,ZERO,ZERO " "); /* Space inside the string must not be ignored. */
    mutate(SIMPLE,"\"vault_id\"","\"vault_\\u0069d\"");
    refuse(SIMPLE "{}"); refuse(SIMPLE ","); refuse("{}");
    json[len/2]=0; assert(porthome_head_decode(json,len,&out)<0 && !out); free(json);
    const char reordered[] = " { \"parents\": [], \"written_at\":0,\"seq\":0,\"grants_rev\":0,\"root_manifest\":\"" ZERO "\",\"epoch\":1,\"vault_id\":\"" ZERO "\" }\n";
    assert(porthome_head_decode((const uint8_t *)reordered,strlen(reordered),&out)==0); drop(out);

    porthome_head_parent parents[2] = {{.writer={0x21},.seq=UINT64_MAX,.root_manifest={0x31}},
                                      {.writer={0x22},.seq=0,.root_manifest={0x32}}};
    porthome_head head = {.vault_id={0x11},.epoch=3,.root_manifest={0x12},.ref_state={0x13},.has_ref_state=1,
        .seq=UINT64_MAX,.parents=parents,.parents_len=2,.grants_rev=UINT64_MAX,.written_at=123456789};
    assert(porthome_head_encode(&head,&json,&len)==0);
    assert(porthome_head_decode(json,len,&out)==0);
    assert(out->epoch==3 && out->seq==UINT64_MAX && out->has_ref_state && !memcmp(out->ref_state,head.ref_state,32));
    assert(out->parents_len==2 && out->parents[0].seq==UINT64_MAX && !memcmp(out->parents[1].writer,parents[1].writer,32));
    uint8_t *again = NULL; size_t n = 0;
    assert(porthome_head_encode(out,&again,&n)==0 && n==len && !memcmp(again,json,len)); free(again); drop(out);
    mutate((char *)json,"\"writer\":", "\"seq\":0,\"writer\":");
    mutate((char *)json,"\"parents\":[", "\"parents\":[{},");

    uint8_t root[32], key[32]; fixture(root,key);
    porthome_ring_entry_t entries[2] = {{.epoch=1},{.epoch=3}};
    memcpy(entries[0].node_key,root,32); memcpy(entries[1].node_key,key,32);
    porthome_ring_t ring = {entries,2}, member = {entries+1,1};
    char *payload = NULL;
    assert(porthome_head_seal(&head,key,&payload)==0);
    assert(porthome_head_open(&member,3,payload,&out)==0 && out->parents_len==2); drop(out);
    assert(porthome_head_open(&ring,1,payload,&out)==PORTHOME_ERR_CRYPTO && !out); /* No retry of key at epoch 3. */
    assert(porthome_head_open(&member,1,payload,&out)==PORTHOME_ERR_EPOCH && !out);
    /* Independently exercise the exact specified self-ECDH construction. */
    uint8_t hk[32], pk[32], ck[32], *plain = NULL; size_t plain_len = 0; char sk_hex[65];
    assert(porthome_head_key_v2(key,hk)==0); porthome_hex64(hk,sk_hex);
    char *pub = nostr_key_get_public(sk_hex); assert(pub && porthome_from_hex64(pub,pk)==0); free(pub);
    assert(nostr_nip44_convkey(hk,pk,ck)==0);
    assert(nostr_nip44_decrypt_v2_with_convkey(ck,payload,&plain,&plain_len)==0 && plain_len==len && !memcmp(plain,json,len));
    free(plain); free(json);
    payload[10] = payload[10]=='A' ? 'B' : 'A';
    assert(porthome_head_open(&ring,3,payload,&out)==PORTHOME_ERR_CRYPTO && !out); free(payload);
    head.epoch=1;
    assert(porthome_head_seal(&head,key,&payload)==0);
    assert(porthome_head_open(&ring,3,payload,&out)==PORTHOME_ERR_EPOCH && !out); free(payload); head.epoch=3;

    pub = nostr_key_get_public(signer); assert(pub && porthome_from_hex64(pub,pk)==0); free(pub);
    NostrEvent *e = NULL;
    assert(porthome_head_event_new(&head,key,pk,&e)==0);
    assert(e->kind==30078 && e->created_at==(int64_t)head.written_at && !e->sig);
    char d[PORTHOME_HEAD_D_LEN]; assert(porthome_head_d_tag(head.vault_id,d)==0);
    assert(strlen(d)==76 && !strncmp(d,"porthome/v2/",12) && !strcmp(nostr_tags_get_d(e->tags),d));
    assert(porthome_head_event_open(e,head.vault_id,&member,3,&out)<0 && !out); /* unsigned */
    resign(e);
    /* Real signed event serialized/deserialized by libnostr, not a mocked signer. */
    char *wire = nostr_event_serialize(e); assert(wire);
    NostrEvent *received = nostr_event_new(); assert(received && nostr_event_deserialize(received,wire)==0); free(wire);
    assert(porthome_head_event_open(received,head.vault_id,&member,3,&out)==0 && out->seq==head.seq); drop(out); nostr_event_free(received);
    assert(porthome_head_event_open(e,root,&ring,3,&out)<0 && !out); /* wrong d/vault */
    assert(porthome_head_event_open(e,head.vault_id,&ring,1,&out)==PORTHOME_ERR_CRYPTO && !out);
    e->sig[0]=e->sig[0]=='0' ? '1' : '0';
    assert(porthome_head_event_open(e,head.vault_id,&member,3,&out)==PORTHOME_ERR_CRYPTO && !out); resign(e);
    e->kind=30079; resign(e); assert(porthome_head_event_open(e,head.vault_id,&member,3,&out)<0 && !out); e->kind=30078;
    nostr_tag_set(nostr_tags_get(e->tags,0),1,"porthome/v2/wrong"); resign(e);
    assert(porthome_head_event_open(e,head.vault_id,&member,3,&out)<0 && !out);
    nostr_tag_set(nostr_tags_get(e->tags,0),1,d);
    nostr_tags_append(e->tags,nostr_tag_new("client","test",NULL)); resign(e);
    assert(porthome_head_event_open(e,head.vault_id,&member,3,&out)==0); drop(out);
    nostr_tags_append(e->tags,nostr_tag_new("d",d,NULL)); resign(e);
    assert(porthome_head_event_open(e,head.vault_id,&member,3,&out)<0 && !out); nostr_event_free(e);
    assert(porthome_head_event_new(&head,key,pk,&e)==0);
    nostr_tag_append(nostr_tags_get(e->tags,0),"extra"); resign(e);
    assert(porthome_head_event_open(e,head.vault_id,&member,3,&out)<0 && !out); nostr_event_free(e);
    assert(porthome_head_event_new(&head,key,pk,&e)==0);
    nostr_event_set_tags(e,nostr_tags_new(0)); resign(e);
    assert(porthome_head_event_open(e,head.vault_id,&member,3,&out)<0 && !out); nostr_event_free(e);
    /* Correct outer d tag cannot redirect a valid ciphertext for another vault. */
    head.vault_id[0]^=1; assert(porthome_head_event_new(&head,key,pk,&e)==0);
    nostr_tag_set(nostr_tags_get(e->tags,0),1,d); resign(e); head.vault_id[0]^=1;
    assert(porthome_head_event_open(e,head.vault_id,&member,3,&out)<0 && !out); nostr_event_free(e);

    porthome_head_parent many[PORTHOME_HEAD_MAX_PARENTS] = {0};
    head.parents=many; head.parents_len=PORTHOME_HEAD_MAX_PARENTS;
    assert(porthome_head_encode(&head,&json,&len)==0 && porthome_head_decode(json,len,&out)==0 && out->parents_len==PORTHOME_HEAD_MAX_PARENTS); drop(out); free(json);
    head.parents_len=PORTHOME_HEAD_MAX_PARENTS+1;
    assert(porthome_head_encode(&head,&json,&len)==PORTHOME_ERR_TOO_LARGE && !json && !len);
    nostr_json_cleanup();
    return 0;
}
