/* SPDX-License-Identifier: MIT */
#include "vc.h"
#include <porthome/crypto.h>
#include <openssl/crypto.h>
#include <curl/curl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
static void usage(FILE *out)
{
    fputs(
        "vaultctl 0.3.1\nUsage: vaultctl COMMAND --state DIR --key-file FILE [options] [SUBTREE]\n"
        "Commands: share, grant, revoke, fold, fsck\n"
        "  --relay wss://URL --blossom https://URL (repeatable on share; retained)\n"
        "  --recipient HEX [--write] [--history] (grant)\n"
        "  --member HEX [--budget BYTES] [--eager] (revoke)\n"
        "  --resume [--reconcile] (resume saved operation, never rotate again)\n"
        "  --deadline-ms N (default 30000), --allow-local (loopback fixtures only)\n"
        "Private state contains keys and decoded manifests. Names and chunk lengths travel in sealed manifests.\n",
        out);
}
static int number(const char *s, uint64_t *n)
{
    if (!s || !*s || *s == '-' || *s == '+')
        return -1;
    errno = 0;
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || *end)
        return -1;
    *n = v;
    return 0;
}
static int endpoint(const char *url, int relay, int local)
{
    if (strlen(url) > VAULT_MAX_URL)
        return -1;
    CURLU *u = curl_url();
    if (!u)
        return -1;
    char *scheme = NULL, *host = NULL, *user = NULL, *query = NULL, *fragment = NULL;
    int rc = -1;
    if (curl_url_set(u, CURLUPART_URL, url, CURLU_NON_SUPPORT_SCHEME) ||
        curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) || curl_url_get(u, CURLUPART_HOST, &host, 0))
        goto done;
    if (!curl_url_get(u, CURLUPART_USER, &user, 0) ||
        !curl_url_get(u, CURLUPART_QUERY, &query, 0) ||
        !curl_url_get(u, CURLUPART_FRAGMENT, &fragment, 0))
        goto done;
    if (!strcmp(scheme, relay ? "wss" : "https"))
        rc = 0;
    else if (local && !strcmp(scheme, relay ? "ws" : "http") &&
             (!strcmp(host, "127.0.0.1") || !strcmp(host, "[::1]") || !strcmp(host, "localhost")))
        rc = 0;
done:
    curl_free(scheme);
    curl_free(host);
    curl_free(user);
    curl_free(query);
    curl_free(fragment);
    curl_url_cleanup(u);
    return rc;
}
static int options(int argc, char **argv, vc_options *o)
{
    if (argc < 2)
        return -1;
    if (!strcmp(argv[1], "--help")) {
        usage(stdout);
        return 1;
    }
    if (!strcmp(argv[1], "--version")) {
        puts("vaultctl 0.3.1");
        return 1;
    }
    o->command = argv[1];
    o->deadline_ms = 30000;
    o->budget = 16u * 1024u * 1024u;
    o->cap = VAULT_CAP_READ;
    optind = 2;
    static const struct option opts[] = {
        {"state", 1, 0, 's'},       {"key-file", 1, 0, 'k'},    {"relay", 1, 0, 'r'},
        {"blossom", 1, 0, 'b'},     {"recipient", 1, 0, 'm'},   {"member", 1, 0, 'm'},
        {"write", 0, 0, 'w'},       {"history", 0, 0, 'h'},     {"eager", 0, 0, 'e'},
        {"resume", 0, 0, 'R'},      {"reconcile", 0, 0, 'c'},   {"budget", 1, 0, 'B'},
        {"deadline-ms", 1, 0, 'D'}, {"allow-local", 0, 0, 'L'}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1)
        switch (c) {
        case 's':
            o->state_path = optarg;
            break;
        case 'k':
            o->key_file = optarg;
            break;
        case 'r':
            if (o->relays_len == VC_MAX_ENDPOINTS)
                return -1;
            o->relays[o->relays_len++] = optarg;
            break;
        case 'b':
            if (o->blossom_len == VC_MAX_ENDPOINTS)
                return -1;
            o->blossom[o->blossom_len++] = optarg;
            break;
        case 'm':
            o->member_hex = optarg;
            break;
        case 'w':
            o->cap = VAULT_CAP_WRITE;
            break;
        case 'h':
            o->history = 1;
            break;
        case 'e':
            o->eager = 1;
            break;
        case 'R':
            o->resume = 1;
            break;
        case 'c':
            o->reconcile = 1;
            break;
        case 'B':
            if (number(optarg, &o->budget))
                return -1;
            break;
        case 'D':
            if (number(optarg, &o->deadline_ms) || !o->deadline_ms || o->deadline_ms > 3600000)
                return -1;
            break;
        case 'L':
            o->allow_local = 1;
            break;
        default:
            return -1;
        }
    if (optind < argc)
        o->source = argv[optind++];
    if (optind != argc || !o->state_path || !o->key_file)
        return -1;
    int share = !strcmp(o->command, "share"), grant = !strcmp(o->command, "grant"),
        revoke = !strcmp(o->command, "revoke"),
        read = !strcmp(o->command, "fold") || !strcmp(o->command, "fsck");
    if (!share && !grant && !revoke && !read)
        return -1;
    if (o->source && (!share || o->resume))
        return -1;
    if (share && !o->resume && (!o->source || !o->relays_len || !o->blossom_len))
        return -1;
    if ((grant || revoke) && !o->resume && !o->member_hex)
        return -1;
    if (read && (o->resume || o->reconcile || o->member_hex))
        return -1;
    if (o->reconcile && !o->resume)
        return -1;
    return 0;
}
int main(int argc, char **argv)
{
    vc_app a = {0};
    a.state.dirfd = a.state.lockfd = a.state.objectsfd = a.state.acksfd = a.state.ringsfd = -1;
    a.state.current = SIZE_MAX;
    int parsed = options(argc, argv, &a.options);
    if (parsed) {
        if (parsed < 0)
            usage(stderr);
        return parsed < 0 ? 1 : 0;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int fresh = !strcmp(a.options.command, "share") && !a.options.resume,
        rc = vc_state_open(&a, fresh);
    if (!rc)
        rc = vc_identity_load(&a);
    if (!rc && fresh) {
        struct stat st;
        if (!fstatat(a.state.dirfd, "state.cbor", &st, AT_SYMLINK_NOFOLLOW)) {
            vc_error(&a, "state already contains a vault");
            rc = VAULT_ERR_CONFLICT;
        } else if (errno != ENOENT)
            rc = VAULT_ERR_IO;
        else {
            a.state.relays_len = a.options.relays_len;
            a.state.blossom_len = a.options.blossom_len;
            for (size_t i = 0; i < a.state.relays_len; i++)
                if (!(a.state.relays[i] = strdup(a.options.relays[i])))
                    rc = VAULT_ERR_OOM;
            for (size_t i = 0; i < a.state.blossom_len; i++)
                if (!(a.state.blossom[i] = strdup(a.options.blossom[i])))
                    rc = VAULT_ERR_OOM;
        }
    } else if (!rc)
        rc = vc_state_load(&a);
    if (!rc) {
        for (size_t i = 0; i < a.state.relays_len; i++)
            if (!a.state.relays[i] || endpoint(a.state.relays[i], 1, a.options.allow_local))
                rc = VAULT_ERR_ARG;
        for (size_t i = 0; i < a.state.blossom_len; i++)
            if (!a.state.blossom[i] || endpoint(a.state.blossom[i], 0, a.options.allow_local))
                rc = VAULT_ERR_ARG;
        if (a.options.relays_len) {
            if (a.options.relays_len != a.state.relays_len)
                rc = VAULT_ERR_ARG;
            else
                for (size_t i = 0; i < a.options.relays_len; i++)
                    if (strcmp(a.options.relays[i], a.state.relays[i]))
                        rc = VAULT_ERR_ARG;
        }
        if (a.options.blossom_len) {
            if (a.options.blossom_len != a.state.blossom_len)
                rc = VAULT_ERR_ARG;
            else
                for (size_t i = 0; i < a.options.blossom_len; i++)
                    if (strcmp(a.options.blossom[i], a.state.blossom[i]))
                        rc = VAULT_ERR_ARG;
        }
    }
    if (!rc)
        rc = vc_blobs_open(&a);
    if (!rc) {
        if (!strcmp(a.options.command, "share"))
            rc = vc_command_share(&a);
        else if (!strcmp(a.options.command, "grant"))
            rc = vc_command_grant(&a);
        else if (!strcmp(a.options.command, "revoke"))
            rc = vc_command_revoke(&a);
        else if (!strcmp(a.options.command, "fold"))
            rc = vc_command_fold(&a);
        else
            rc = vc_command_fsck(&a);
    }
    int exitcode = rc == 0                                               ? 0
                   : rc == VAULT_ERR_INCOMPLETE || rc == VAULT_ERR_EPOCH ? VC_EXIT_INCOMPLETE
                   : rc == VAULT_ERR_BUDGET                              ? VC_EXIT_REMAINING
                   : rc == VAULT_ERR_AGAIN                               ? VC_EXIT_UNCERTAIN
                   : rc == VAULT_ERR_CONFLICT                            ? VC_EXIT_CONFLICT
                                                                         : 1;
    if (rc && (a.state.pending_blob || a.state.intent))
        exitcode = VC_EXIT_UNCERTAIN;
    if (rc)
        fprintf(stderr, "vaultctl: %s (error %d); no success claimed\n",
                *a.error                     ? a.error
                : rc == VAULT_ERR_INCOMPLETE ? "incomplete inventory/history"
                : rc == VAULT_ERR_AGAIN
                    ? "pending operation; explicit --resume / --reconcile required"
                    : "operation failed",
                rc);
    vc_blobs_close(&a);
    vc_state_free(&a.state);
    vault_ring_dispose(&a.resolved_ring);
    OPENSSL_cleanse(a.sk, sizeof a.sk);
    OPENSSL_cleanse(a.secret, sizeof a.secret);
    curl_global_cleanup();
    return exitcode;
}
