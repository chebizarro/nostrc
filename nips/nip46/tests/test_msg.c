#include "nostr/nip46/nip46_msg.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int test_request_roundtrip(void) {
    const char *id = "42";
    const char *method = "get_public_key";
    const char *params_arr[] = {"arg1", "arg2"};
    char *json = nostr_nip46_request_build(id, method, params_arr, 2);
    if (!json) { printf("request_build failed\n"); return 1; }

    NostrNip46Request req;
    if (nostr_nip46_request_parse(json, &req) != 0) { printf("request_parse failed\n"); free(json); return 2; }
    if (!req.id || strcmp(req.id, id) != 0) { printf("id mismatch\n"); free(json); nostr_nip46_request_free(&req); return 3; }
    if (!req.method || strcmp(req.method, method) != 0) { printf("method mismatch\n"); free(json); nostr_nip46_request_free(&req); return 4; }
    if (req.n_params < 2) { printf("params size mismatch\n"); free(json); nostr_nip46_request_free(&req); return 5; }

    nostr_nip46_request_free(&req);
    free(json);
    return 0;
}

static int test_response_ok_roundtrip(void) {
    const char *id = "7";
    const char *result_json = "{\"pubkey\":\"abc\"}";
    char *json = nostr_nip46_response_build_ok(id, result_json);
    if (!json) { printf("response_build_ok failed\n"); return 1; }
    NostrNip46Response res;
    if (nostr_nip46_response_parse(json, &res) != 0) { printf("response_parse failed\n"); free(json); return 2; }
    if (!res.id || strcmp(res.id, id) != 0) { printf("id mismatch\n"); nostr_nip46_response_free(&res); free(json); return 3; }
    /* result parsing stores string result when simple; check presence */
    if (!res.result) { printf("missing result\n"); nostr_nip46_response_free(&res); free(json); return 4; }

    nostr_nip46_response_free(&res);
    free(json);
    return 0;
}

static int test_response_err_roundtrip(void) {
    const char *id = "9";
    const char *err = "permission denied";
    char *json = nostr_nip46_response_build_err(id, err);
    if (!json) { printf("response_build_err failed\n"); return 1; }
    NostrNip46Response res;
    if (nostr_nip46_response_parse(json, &res) != 0) { printf("response_parse failed\n"); free(json); return 2; }
    if (!res.id || strcmp(res.id, id) != 0) { printf("id mismatch\n"); nostr_nip46_response_free(&res); free(json); return 3; }
    if (!res.error || strcmp(res.error, err) != 0) { printf("error mismatch\n"); nostr_nip46_response_free(&res); free(json); return 4; }

    nostr_nip46_response_free(&res);
    free(json);
    return 0;
}

int main(void) {
    int rc = 0;
    rc = test_request_roundtrip(); if (rc) return rc + 10;
    rc = test_response_ok_roundtrip(); if (rc) return rc + 20;
    rc = test_response_err_roundtrip(); if (rc) return rc + 30;
    printf("test_nip46_msg: OK\n");
    return 0;
}
