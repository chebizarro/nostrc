#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_msg.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int check_ok_json(const char *json, const char *id, const char *expect_result_value){
    NostrNip46Response r; if (nostr_nip46_response_parse(json, &r) != 0) return 1;
    int rc = 0;
    if (!r.id || strcmp(r.id, id) != 0) rc = 2;
    if (!r.result || strcmp(r.result, expect_result_value) != 0) {
        rc = 3;
        if (r.result) printf("parsed result='%s' expected='%s'\n", r.result, expect_result_value);
        else printf("parsed result=NULL expected='%s'\n", expect_result_value);
    }
    if (r.error) rc = 4;
    nostr_nip46_response_free(&r);
    return rc;
}

static int check_err_json(const char *json, const char *id, const char *expect_err){
    NostrNip46Response r; if (nostr_nip46_response_parse(json, &r) != 0) return 1;
    int rc = 0;
    if (!r.id || strcmp(r.id, id) != 0) rc = 2;
    if (!r.error || strcmp(r.error, expect_err) != 0) rc = 3;
    if (r.result) rc = 4;
    nostr_nip46_response_free(&r);
    return rc;
}

int main(void) {
    NostrNip46Session *s = nostr_nip46_bunker_new(NULL);
    if (!s) { printf("session_new failed\n"); return 1; }

    /* Build a dummy request to reply to */
    NostrNip46Request req = {0};
    req.id = strdup("123");
    req.method = strdup("get_public_key");

    /* OK reply path */
    const char *result_json = "\"deadbeef\""; /* raw JSON string token */
    if (nostr_nip46_bunker_reply(s, &req, result_json, NULL) != 0) {
        printf("bunker_reply OK failed\n");
        free(req.id); free(req.method); nostr_nip46_session_free(s); return 2;
    }
    char *out = NULL; if (nostr_nip46_session_take_last_reply_json(s, &out) != 0 || !out) {
        printf("no last reply json\n"); free(req.id); free(req.method); nostr_nip46_session_free(s); return 3;
    }
    printf("built OK reply: %s\n", out);
    int rc = check_ok_json(out, "123", "deadbeef"); free(out); if (rc) { printf("OK json check rc=%d\n", rc); free(req.id); free(req.method); nostr_nip46_session_free(s); return 10+rc; }

    /* ERR reply path */
    const char *err = "denied";
    if (nostr_nip46_bunker_reply(s, &req, NULL, err) != 0) {
        printf("bunker_reply ERR failed\n"); free(req.id); free(req.method); nostr_nip46_session_free(s); return 4;
    }
    out = NULL; if (nostr_nip46_session_take_last_reply_json(s, &out) != 0 || !out) {
        printf("no last reply json (err)\n"); free(req.id); free(req.method); nostr_nip46_session_free(s); return 5;
    }
    printf("built ERR reply: %s\n", out);
    rc = check_err_json(out, "123", "denied"); free(out);
    free(req.id); free(req.method);
    nostr_nip46_session_free(s);
    if (rc) { printf("ERR json check rc=%d\n", rc); return 20+rc; }

    printf("test_bunker_reply: OK\n");
    return 0;
}
