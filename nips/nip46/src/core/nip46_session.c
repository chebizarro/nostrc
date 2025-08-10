#include "nostr/nip46/nip46_types.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip04.h"
#include "nostr-keys.h"
#include "nostr-event.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward prototypes for local helpers */
static int csv_split(const char *csv, char ***out_vec, size_t *out_n);
static void csv_free(char **vec, size_t n);
static void acl_set_perms(NostrNip46Session *s, const char *client_pk, const char *perms_csv);
static int acl_has_perm(const NostrNip46Session *s, const char *client_pk, const char *method);

struct NostrNip46Session {
    /* TODO: wire relay pool, subscriptions, permissions, secrets */
    char *note;
    /* parsed URI fields */
    char *remote_pubkey_hex;   /* from bunker:// */
    char *client_pubkey_hex;   /* from nostrconnect:// */
    char *secret;              /* optional */
    char **relays; size_t n_relays;
    /* testing/transport placeholder */
    char *last_reply_json;
    /* bunker callbacks (optional) */
    NostrNip46BunkerCallbacks cbs;
    /* ACL: per-client allowed methods (simple list) */
    struct PermEntry { char *client_pk; char **methods; size_t n_methods; struct PermEntry *next; } *acl_head;
};

/* Common helpers */
static NostrNip46Session *session_new(const char *note) {
    NostrNip46Session *s = (NostrNip46Session *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    if (note) s->note = strdup(note);
    return s;
}

/* Accept common public key encodings used across modules: 
 * - 64 hex (x-only) 
 * - 66 hex (33B compressed SEC1) 
 * - 130 hex (65B uncompressed SEC1)
 */
static int is_valid_pubkey_hex_relaxed(const char *hex) {
    if (!hex) return 0;
    size_t n = strlen(hex);
    if (!(n == 64 || n == 66 || n == 130)) return 0;
    for (size_t i=0;i<n;++i) {
        char c = hex[i];
        if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return 0;
    }
    return 1;
}

void nostr_nip46_session_free(NostrNip46Session *s) {
    if (!s) return;
    if (s->note) { memset(s->note, 0, strlen(s->note)); free(s->note); }
    if (s->remote_pubkey_hex) { free(s->remote_pubkey_hex); }
    if (s->client_pubkey_hex) { free(s->client_pubkey_hex); }
    if (s->secret) { memset(s->secret, 0, strlen(s->secret)); free(s->secret); }
    if (s->relays) { for (size_t i=0;i<s->n_relays;++i) free(s->relays[i]); free(s->relays); }
    if (s->last_reply_json) free(s->last_reply_json);
    /* free ACL */
    struct PermEntry *it = s->acl_head; while (it) { struct PermEntry *nx = it->next; if (it->client_pk) free(it->client_pk); if (it->methods){ for(size_t i=0;i<it->n_methods;++i) free(it->methods[i]); free(it->methods);} free(it); it = nx; }
    free(s);
}

/* Client API */
NostrNip46Session *nostr_nip46_client_new(void) {
    return session_new("client");
}

int nostr_nip46_client_connect(NostrNip46Session *s,
                               const char *bunker_uri,
                               const char *requested_perms_csv) {
    (void)requested_perms_csv;
    if (!s || !bunker_uri) return -1;
    /* Reset stored fields */
    if (s->remote_pubkey_hex) { free(s->remote_pubkey_hex); s->remote_pubkey_hex=NULL; }
    if (s->client_pubkey_hex) { free(s->client_pubkey_hex); s->client_pubkey_hex=NULL; }
    if (s->secret) { memset(s->secret,0,strlen(s->secret)); free(s->secret); s->secret=NULL; }
    if (s->relays) { for(size_t i=0;i<s->n_relays;++i) free(s->relays[i]); free(s->relays); s->relays=NULL; s->n_relays=0; }

    if (strncmp(bunker_uri, "bunker://", 9) == 0) {
        NostrNip46BunkerURI u; if (nostr_nip46_uri_parse_bunker(bunker_uri, &u) != 0) return -1;
        if (!is_valid_pubkey_hex_relaxed(u.remote_signer_pubkey_hex)) { nostr_nip46_uri_bunker_free(&u); return -1; }
        s->remote_pubkey_hex = u.remote_signer_pubkey_hex; u.remote_signer_pubkey_hex=NULL;
        s->secret = u.secret; u.secret=NULL;
        s->relays = u.relays; s->n_relays = u.n_relays; u.relays=NULL; u.n_relays=0;
        nostr_nip46_uri_bunker_free(&u);
        return 0;
    } else if (strncmp(bunker_uri, "nostrconnect://", 15) == 0) {
        NostrNip46ConnectURI u; if (nostr_nip46_uri_parse_connect(bunker_uri, &u) != 0) return -1;
        if (!is_valid_pubkey_hex_relaxed(u.client_pubkey_hex)) { nostr_nip46_uri_connect_free(&u); return -1; }
        s->client_pubkey_hex = u.client_pubkey_hex; u.client_pubkey_hex=NULL;
        s->secret = u.secret; u.secret=NULL;
        s->relays = u.relays; s->n_relays = u.n_relays; u.relays=NULL; u.n_relays=0;
        nostr_nip46_uri_connect_free(&u);
        return 0;
    }
    return -1;
}

int nostr_nip46_client_get_public_key(NostrNip46Session *s, char **out_user_pubkey_hex) {
    if (!s || !out_user_pubkey_hex) return -1;
    /* If a client pubkey was provided (nostrconnect://), prefer it. */
    if (s->client_pubkey_hex) {
        size_t n = strlen(s->client_pubkey_hex);
        char *dup = (char*)malloc(n+1); if (!dup) return -1; memcpy(dup, s->client_pubkey_hex, n+1);
        *out_user_pubkey_hex = dup; return 0;
    }
    /* If we have our secret, derive the x-only user pubkey via libnostr. */
    if (s->secret) {
        char *pk = nostr_key_get_public(s->secret);
        if (!pk) return -1;
        *out_user_pubkey_hex = pk; /* already allocated */
        return 0;
    }
    /* Fallback: if only a bunker remote pubkey is known, return it (temporary until handshake). */
    if (s->remote_pubkey_hex) {
        size_t n = strlen(s->remote_pubkey_hex);
        char *dup = (char*)malloc(n+1); if (!dup) return -1; memcpy(dup, s->remote_pubkey_hex, n+1);
        *out_user_pubkey_hex = dup; return 0;
    }
    return -1;
}

int nostr_nip46_client_sign_event(NostrNip46Session *s, const char *event_json, char **out_signed_event_json) {
    if (!s || !event_json || !out_signed_event_json) return -1;
    *out_signed_event_json = NULL;
    /* Build a NIP-46 request {id, method:"sign_event", params:[event_json]} */
    const char *params[1] = { event_json };
    char *req = nostr_nip46_request_build("1", "sign_event", params, 1);
    if (!req) return -1;
    /* Encrypt request using NIP-04. Requires bunker remote pubkey (SEC1) and our secret (sk). */
    const char *peer = s->remote_pubkey_hex;
    if (!peer || !s->secret) { free(req); return -1; }

    char *cipher = NULL; char *err = NULL;
    if (nostr_nip04_encrypt(req, peer, s->secret, &cipher, &err) != 0 || !cipher) {
        if (err) free(err);
        free(req);
        return -1;
    }
    free(req);
    *out_signed_event_json = cipher; /* return ciphertext to be sent over transport */
    return 0;
}

int nostr_nip46_client_ping(NostrNip46Session *s) {
    (void)s; return 0;
}

int nostr_nip46_client_nip04_encrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *plaintext, char **out_ciphertext) {
    if (!s || !peer_pubkey_hex || !plaintext || !out_ciphertext) return -1;
    if (!s->secret) return -1;
    char *cipher = NULL; char *err = NULL;
    if (nostr_nip04_encrypt(plaintext, peer_pubkey_hex, s->secret, &cipher, &err) != 0 || !cipher) {
        if (err) free(err);
        return -1;
    }
    *out_ciphertext = cipher;
    return 0;
}

int nostr_nip46_client_nip04_decrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *ciphertext, char **out_plaintext) {
    if (!s || !peer_pubkey_hex || !ciphertext || !out_plaintext) return -1;
    if (!s->secret) return -1;
    char *plain = NULL; char *err = NULL;
    if (nostr_nip04_decrypt(ciphertext, peer_pubkey_hex, s->secret, &plain, &err) != 0 || !plain) {
        if (err) free(err);
        return -1;
    }
    *out_plaintext = plain;
    return 0;
}

int nostr_nip46_client_nip44_encrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *plaintext, char **out_ciphertext) { (void)s; (void)peer_pubkey_hex; (void)plaintext; (void)out_ciphertext; return -1; }
int nostr_nip46_client_nip44_decrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *ciphertext, char **out_plaintext) { (void)s; (void)peer_pubkey_hex; (void)ciphertext; (void)out_plaintext; return -1; }

/* Bunker API */
NostrNip46Session *nostr_nip46_bunker_new(const NostrNip46BunkerCallbacks *cbs) {
    NostrNip46Session *s = session_new("bunker");
    if (!s) return NULL;
    if (cbs) {
        s->cbs = *cbs; /* shallow copy of function pointers and user_data */
    } else {
        memset(&s->cbs, 0, sizeof(s->cbs));
    }
    return s;
}

int nostr_nip46_bunker_listen(NostrNip46Session *s, const char *const *relays, size_t n_relays) {
    (void)s; (void)relays; (void)n_relays; return -1;
}
static int is_unreserved(int c) {
    return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='.'||c=='_'||c=='~'||c==':'||c=='/';
}
static char *percent_encode(const char *s) {
    if (!s) return NULL; size_t n=strlen(s);
    char *out=(char*)malloc(n*3+1); if(!out) return NULL; size_t j=0;
    for(size_t i=0;i<n;++i){ unsigned char c=(unsigned char)s[i]; if(is_unreserved(c)){ out[j++]=c; } else { static const char hex[]="0123456789ABCDEF"; out[j++]='%'; out[j++]=hex[(c>>4)&0xF]; out[j++]=hex[c&0xF]; } }
    out[j]='\0'; return out;
}
int nostr_nip46_bunker_issue_bunker_uri(NostrNip46Session *s, const char *remote_signer_pubkey_hex, const char *const *relays, size_t n_relays, const char *secret, char **out_uri) {
    (void)s; if(!remote_signer_pubkey_hex||!out_uri) return -1; *out_uri=NULL;
    size_t cap = 16 + 64 + 1 + (n_relays? n_relays*64:0) + (secret? strlen(secret)+16:0);
    char *buf=(char*)malloc(cap); if(!buf) return -1; size_t len=0;
    len += snprintf(buf+len, cap-len, "bunker://%s", remote_signer_pubkey_hex);
    int first=1;
    if (relays && n_relays>0) {
        for (size_t i=0;i<n_relays;++i) {
            if (!relays[i]) continue; char *enc = percent_encode(relays[i]); if(!enc){ free(buf); return -1; }
            len += snprintf(buf+len, cap-len, "%srelay=%s", first?"?":"&", enc);
            first=0; free(enc);
        }
    }
    if (secret && *secret) {
        char *encs = percent_encode(secret); if(!encs){ free(buf); return -1; }
        len += snprintf(buf+len, cap-len, "%ssecret=%s", first?"?":"&", encs);
        free(encs);
    }
    *out_uri = buf; return 0;
}
int nostr_nip46_bunker_reply(NostrNip46Session *s, const NostrNip46Request *req, const char *result_or_json, const char *error_or_null) {
    if (!s || !req || !req->id) return -1;
    char *json = NULL;
    if (error_or_null) {
        json = nostr_nip46_response_build_err(req->id, error_or_null);
    } else {
        if (!result_or_json) return -1;
        json = nostr_nip46_response_build_ok(req->id, result_or_json);
    }
    if (!json) return -1;
    if (s->last_reply_json) { free(s->last_reply_json); }
    s->last_reply_json = json; /* transfer ownership to session, retrievable via getter */
    /* TODO: when transport is wired, emit this JSON over the relay */
    return 0;
}

int nostr_nip46_bunker_handle_cipher(NostrNip46Session *s,
                                     const char *client_pubkey_hex,
                                     const char *ciphertext,
                                     char **out_cipher_reply) {
    if (!s || !client_pubkey_hex || !ciphertext || !out_cipher_reply) return -1;
    *out_cipher_reply = NULL;
    if (!s->secret) return -1; /* need our secret to decrypt/encrypt */

    /* 1) Decrypt NIP-04 */
    char *plain = NULL; char *err = NULL;
    if (nostr_nip04_decrypt(ciphertext, client_pubkey_hex, s->secret, &plain, &err) != 0 || !plain) {
        if (err) free(err);
        return -1;
    }

    /* 2) Parse request */
    NostrNip46Request req = {0};
    if (nostr_nip46_request_parse(plain, &req) != 0 || !req.id || !req.method) {
        free(plain);
        nostr_nip46_request_free(&req);
        return -1;
    }

    /* 3) Dispatch */
    char *reply_json = NULL;
    if (strcmp(req.method, "get_public_key") == 0) {
        char *pub = nostr_key_get_public(s->secret);
        if (!pub) { nostr_nip46_request_free(&req); free(plain); return -1; }
        /* Build JSON string token for result: "<hex>" */
        size_t cap = strlen(pub) + 3;
        char *quoted = (char*)malloc(cap);
        if (!quoted) { free(pub); nostr_nip46_request_free(&req); free(plain); return -1; }
        snprintf(quoted, cap, "\"%s\"", pub);
        reply_json = nostr_nip46_response_build_ok(req.id, quoted);
        free(quoted);
        free(pub);
    } else if (strcmp(req.method, "sign_event") == 0) {
        /* enforce ACL: require permission for client */
        if (!acl_has_perm(s, client_pubkey_hex, "sign_event")) {
            reply_json = nostr_nip46_response_build_err(req.id, "forbidden");
        } else {
        if (req.n_params < 1 || !req.params || !req.params[0]) { nostr_nip46_request_free(&req); free(plain); return -1; }
        if (s->cbs.sign_cb) {
            char *signed_event_json = s->cbs.sign_cb(req.params[0], s->cbs.user_data);
            if (!signed_event_json) {
                reply_json = nostr_nip46_response_build_err(req.id, "signing_failed");
            } else {
                reply_json = nostr_nip46_response_build_ok(req.id, signed_event_json);
                free(signed_event_json);
            }
        } else {
            /* Real signing path using libnostr */
            if (!s->secret) { nostr_nip46_request_free(&req); free(plain); return -1; }
            NostrEvent *ev = nostr_event_new();
            if (!ev) { nostr_nip46_request_free(&req); free(plain); return -1; }
            int prc = nostr_event_deserialize(ev, req.params[0]);
            if (prc != 0) {
                nostr_event_free(ev);
                reply_json = nostr_nip46_response_build_err(req.id, "invalid_event_json");
            } else {
                /* Ensure pubkey matches bunker secret */
                char *bunker_pk_x = nostr_key_get_public(s->secret);
                if (!bunker_pk_x) {
                    nostr_event_free(ev);
                    nostr_nip46_request_free(&req); free(plain); return -1;
                }
                nostr_event_set_pubkey(ev, bunker_pk_x);
                free(bunker_pk_x);
                if (nostr_event_sign(ev, s->secret) != 0) {
                    reply_json = nostr_nip46_response_build_err(req.id, "signing_failed");
                } else {
                    char *signed_json = nostr_event_serialize(ev);
                    if (!signed_json) {
                        reply_json = nostr_nip46_response_build_err(req.id, "serialize_failed");
                    } else {
                        reply_json = nostr_nip46_response_build_ok(req.id, signed_json);
                        free(signed_json);
                    }
                }
                nostr_event_free(ev);
            }
        }
        }
    } else if (strcmp(req.method, "connect") == 0) {
        /* params: [client_pubkey_hex, perms_csv] */
        int allowed = 1;
        if (s->cbs.authorize_cb) {
            const char *pk = (req.n_params > 0 && req.params) ? req.params[0] : NULL;
            const char *perms = (req.n_params > 1 && req.params) ? req.params[1] : NULL;
            allowed = s->cbs.authorize_cb(pk, perms, s->cbs.user_data);
        }
        if (allowed) {
            const char *pk = (req.n_params > 0 && req.params) ? req.params[0] : NULL;
            const char *perms = (req.n_params > 1 && req.params) ? req.params[1] : NULL;
            if (pk && is_valid_pubkey_hex_relaxed(pk)) {
                acl_set_perms(s, pk, perms);
            }
            reply_json = nostr_nip46_response_build_ok(req.id, "\"ack\"");
        } else {
            reply_json = nostr_nip46_response_build_err(req.id, "denied");
        }
    } else {
        reply_json = nostr_nip46_response_build_err(req.id, "method_not_supported");
    }

    /* Save last reply (plaintext) for tests that may introspect it */
    if (s->last_reply_json) { free(s->last_reply_json); s->last_reply_json=NULL; }
    if (reply_json) {
        s->last_reply_json = strdup(reply_json);
    }

    /* 4) Encrypt reply */
    char *cipher = NULL; char *e2 = NULL;
    int rc = -1;
    if (reply_json && nostr_nip04_encrypt(reply_json, client_pubkey_hex, s->secret, &cipher, &e2) == 0 && cipher) {
        *out_cipher_reply = cipher;
        rc = 0;
    }
    if (e2) free(e2);

    /* 5) Cleanup */
    free(reply_json);
    nostr_nip46_request_free(&req);
    free(plain);
    return rc;
}

/* Getters */
static char *dupstr(const char *s){ if(!s) return NULL; size_t n=strlen(s); char *o=(char*)malloc(n+1); if(!o) return NULL; memcpy(o,s,n+1); return o; }
int nostr_nip46_session_get_remote_pubkey(const NostrNip46Session *s, char **out_hex){ if(!s||!out_hex) return -1; *out_hex = dupstr(s->remote_pubkey_hex); return 0; }
int nostr_nip46_session_get_client_pubkey(const NostrNip46Session *s, char **out_hex){ if(!s||!out_hex) return -1; *out_hex = dupstr(s->client_pubkey_hex); return 0; }
int nostr_nip46_session_get_secret(const NostrNip46Session *s, char **out_secret){ if(!s||!out_secret) return -1; *out_secret = dupstr(s->secret); return 0; }
int nostr_nip46_session_get_relays(const NostrNip46Session *s, char ***out_relays, size_t *out_n){ if(!s||!out_relays||!out_n) return -1; *out_relays=NULL; *out_n=0; if(!s->relays||s->n_relays==0) return 0; char **arr=(char**)malloc(sizeof(char*)*s->n_relays); if(!arr) return -1; for(size_t i=0;i<s->n_relays;++i){ arr[i]=dupstr(s->relays[i]); if(!arr[i]){ for(size_t j=0;j<i;++j) free(arr[j]); free(arr); return -1; } } *out_relays=arr; *out_n=s->n_relays; return 0; }

int nostr_nip46_session_take_last_reply_json(NostrNip46Session *s, char **out_json){ if(!s||!out_json) return -1; *out_json=NULL; if(!s->last_reply_json) return 0; *out_json = s->last_reply_json; s->last_reply_json=NULL; return 0; }

/* --- ACL helpers --- */
static int csv_split(const char *csv, char ***out_vec, size_t *out_n){ if(out_vec) *out_vec=NULL; if(out_n) *out_n=0; if(!csv||!*csv||!out_vec) return 0; size_t n=1; for(const char *p=csv; *p; ++p){ if(*p==',') n++; }
    char **vec=(char**)calloc(n, sizeof(char*)); if(!vec) return -1; size_t idx=0; const char *start=csv; for(const char *p=csv; ; ++p){ if(*p==','||*p=='\0'){ size_t len=(size_t)(p-start); char *s=(char*)malloc(len+1); if(!s){ csv_free(vec, idx); return -1; } memcpy(s,start,len); s[len]='\0'; vec[idx++]=s; if(*p=='\0') break; start=p+1; } }
    *out_vec=vec; if(out_n) *out_n=idx; return 0; }
static void csv_free(char **vec, size_t n){ if(!vec) return; for(size_t i=0;i<n;++i) free(vec[i]); free(vec); }
static void acl_set_perms(NostrNip46Session *s, const char *client_pk, const char *perms_csv){ if(!s||!client_pk) return; /* remove existing */ struct PermEntry **pp=&s->acl_head; while(*pp){ if(strcmp((*pp)->client_pk, client_pk)==0){ struct PermEntry *old=*pp; *pp=old->next; if(old->methods) { for(size_t i=0;i<old->n_methods;++i) free(old->methods[i]); free(old->methods);} free(old->client_pk); free(old); break; } pp=&(*pp)->next; }
    struct PermEntry *e=(struct PermEntry*)calloc(1,sizeof(*e)); if(!e) return; e->client_pk=strdup(client_pk); if(perms_csv && *perms_csv){ if(csv_split(perms_csv, &e->methods, &e->n_methods)!=0){ e->methods=NULL; e->n_methods=0; } } e->next=s->acl_head; s->acl_head=e; }
static int acl_has_perm(const NostrNip46Session *s, const char *client_pk, const char *method){ if(!s||!client_pk||!method) return 0; for(const struct PermEntry *it=s->acl_head; it; it=it->next){ if(it->client_pk && strcmp(it->client_pk, client_pk)==0){ if(it->n_methods==0) return 0; for(size_t i=0;i<it->n_methods;++i){ if(it->methods[i] && strcmp(it->methods[i], method)==0) return 1; } return 0; } } return 0; }
