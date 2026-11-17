#define _POSIX_C_SOURCE 200809L
#include "test_tmpdir.h"
#include "node/acl.h"
#include "node/diag.h"
#include "control/protocol.h"
#include "host_os.h"
#include <fips/identity/local.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(_WIN32)
#include <direct.h>
#define mkdir(path, mode) ((void)(mode), _mkdir(path))
#endif
static void put(const char *path, const char *text)
{ FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(text, 1, strlen(text), f) == strlen(text)); assert(fclose(f) == 0); }
static void peer(fips_secp *secp, uint8_t byte, fips_peer_identity *out, char npub[128])
{
    uint8_t seed[32]; memset(seed, byte, sizeof seed); fips_identity *id = NULL;
    assert(fips_identity_from_secret_bytes(secp, fipsd_host_os_entropy(), seed, &id) == FIPS_IDENTITY_OK);
    fips_xonly_pubkey key = fips_identity_pubkey(id);
    assert(fips_peer_identity_from_pubkey(secp, &key, out) == FIPS_IDENTITY_OK);
    assert(fips_peer_identity_npub(out, npub, 128) == FIPS_IDENTITY_OK); fips_identity_destroy(id);
}
static fipsd_acl *load(fipsd_config *cfg, fipsd_acl_paths *paths, const fipsd_acl *old)
{ fipsd_acl *a = fipsd_acl_load(cfg, paths, old); assert(a); return a; }
static void mode(const fipsd_acl *a, const char *expected, const char *decision)
{
    json_t *s = fipsd_acl_status(a); assert(s);
    assert(!strcmp(json_string_value(json_object_get(s, "effective_mode")), expected));
    assert(!strcmp(json_string_value(json_object_get(s, "default_decision")), decision)); json_decref(s);
}
int main(void)
{
    const char *tmp = fipsd_test_tmpdir(NULL);
    char allow[4096], deny[4096], hosts[4096], yamlpath[4096];
    snprintf(allow, sizeof allow, "%s/peers.allow", tmp); snprintf(deny, sizeof deny, "%s/peers.deny", tmp);
    snprintf(hosts, sizeof hosts, "%s/hosts", tmp); snprintf(yamlpath, sizeof yamlpath, "%s/fips.yaml", tmp);
    fipsd_acl_paths paths = {allow, deny, hosts}; fips_secp *secp = NULL;
    assert(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
    fips_peer_identity a, b; char apub[128], bpub[128]; peer(secp, 0x11, &a, apub); peer(secp, 0x22, &b, bpub);
    char yaml[1024]; snprintf(yaml, sizeof yaml, "peers: [{npub: '%s', alias: Alice, addresses: [{transport: udp, addr: '127.0.0.1:9999'}]}]", apub);
    fipsd_config *cfg = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(cfg);
    fipsd_acl *empty = load(cfg, &paths, NULL);
    assert(fipsd_acl_check(empty, &a) == FIPSD_ACL_DEFAULT_ALLOW); mode(empty, "default_open", "allow");
    json_t *wire_status = fipsd_acl_status(empty); assert(wire_status);
    json_object_set_new(wire_status, "allow_file", json_string("<redacted>"));
    json_object_set_new(wire_status, "deny_file", json_string("<redacted>"));
    json_t *response = fipsd_control_ok(wire_status);
    json_error_t error; json_t *golden = json_load_file(FIPSD_CONTROL_GOLDEN "/show_acl.json", 0, &error); assert(golden);
    char *actual = fipsd_control_encode(response), *expected = fipsd_control_encode(golden);
    assert(actual && expected && !strcmp(actual, expected));
    free(actual); free(expected); json_decref(response); json_decref(golden);

    /* Allow match wins, allow-list miss remains default allow. */
    put(allow, apub); put(deny, apub);
    fipsd_acl *rules = load(cfg, &paths, NULL);
    assert(fipsd_acl_check(rules, &a) == FIPSD_ACL_ALLOW_LIST);
    assert(fipsd_acl_check(rules, &b) == FIPSD_ACL_DEFAULT_ALLOW); mode(rules, "allow_then_deny", "allow"); fipsd_acl_free(rules);
    put(deny, "ALL\n"); rules = load(cfg, &paths, NULL);
    assert(fipsd_acl_check(rules, &a) == FIPSD_ACL_ALLOW_LIST && fipsd_acl_check(rules, &b) == FIPSD_ACL_DENY_LIST);
    mode(rules, "allow_then_deny_all", "deny"); fipsd_acl_free(rules);
    put(allow, "aLl # allow everything\n"); rules = load(cfg, &paths, NULL);
    assert(fipsd_acl_check(rules, &a) == FIPSD_ACL_ALLOW_LIST && fipsd_acl_check(rules, &b) == FIPSD_ACL_ALLOW_LIST);
    mode(rules, "allow_all", "allow"); fipsd_acl_free(rules);
    /* Invalid/multi-token/prefix entries do not activate enforcement. */
    put(allow, "bad two\nunknown-alias\nnpub1prefix*\n# comment\n"); put(deny, ""); rules = load(cfg, &paths, NULL);
    mode(rules, "default_open", "allow"); fipsd_acl_free(rules);
    /* Alias matching is case-insensitive, hosts file wins over YAML aliases. */
    char text[512]; snprintf(text, sizeof text, "Alice\n%s # duplicate effective identity\n", apub); put(allow, text);
    rules = load(cfg, &paths, NULL); json_t *s = fipsd_acl_status(rules);
    assert(json_array_size(json_object_get(s, "allow_entries")) == 1 && json_array_size(json_object_get(s, "allow_file_entries")) == 2);
    assert(fipsd_acl_check(rules, &a) == FIPSD_ACL_ALLOW_LIST); json_decref(s); fipsd_acl_free(rules);
    snprintf(text, sizeof text, "ALICE %s\n", bpub); put(hosts, text); put(allow, "aLiCe\n"); put(deny, "ALL\n");
    rules = load(cfg, &paths, NULL); assert(fipsd_acl_check(rules, &b) == FIPSD_ACL_ALLOW_LIST && fipsd_acl_check(rules, &a) == FIPSD_ACL_DENY_LIST);
    /* Reload unreadable input holds last good snapshot, then retries. A
     * directory reliably fails regular-file input even when tests run as root. */
    assert(unlink(deny) == 0 && mkdir(deny, 0700) == 0);
    fipsd_acl *held = load(cfg, &paths, rules); assert(fipsd_acl_stale(held));
    assert(fipsd_acl_check(held, &a) == FIPSD_ACL_DENY_LIST && fipsd_acl_changed(held));
    assert(rmdir(deny) == 0); put(deny, "ALL\n");
    fipsd_acl *recovered = load(cfg, &paths, held); assert(!fipsd_acl_stale(recovered));
    assert(fipsd_acl_check(recovered, &b) == FIPSD_ACL_ALLOW_LIST); fipsd_acl_free(held); fipsd_acl_free(rules);
    /* A clean in-place empty ACL is held exactly once; intentional file
     * deletion and loss of an alias take effect without this guard. */
    put(allow, ""); put(deny, ""); held = load(cfg, &paths, recovered);
    assert(fipsd_acl_stale(held) && fipsd_acl_check(held, &a) == FIPSD_ACL_DENY_LIST);
    rules = load(cfg, &paths, held); mode(rules, "default_open", "allow"); assert(!fipsd_acl_stale(rules));
    fipsd_acl_free(held); fipsd_acl_free(recovered); fipsd_acl_free(rules);
    put(deny, "ALL\n"); rules = load(cfg, &paths, NULL); assert(unlink(allow) == 0 && unlink(deny) == 0);
    held = load(cfg, &paths, rules); mode(held, "default_open", "allow"); assert(!fipsd_acl_stale(held)); fipsd_acl_free(held); fipsd_acl_free(rules);
    put(deny, "aLiCe\n"); rules = load(cfg, &paths, NULL); assert(unlink(hosts) == 0);
    held = load(cfg, &paths, rules); assert(fipsd_acl_check(held, &a) == FIPSD_ACL_DENY_LIST); /* YAML base restored */
    fipsd_acl_free(held); fipsd_acl_free(rules);
    /* SIGHUP's YAML reload path rebuilds the ACL in the same config snapshot;
     * retained old generations keep enforcing their original decision. */
    put(yamlpath, yaml); put(deny, "ALL\n"); assert(fipsd_config_enable_acl(cfg, &paths));
    GoSwap *slot = go_swap_create(cfg, fipsd_config_free); assert(slot);
    GoRefPtr old = go_swap_load(slot); put(allow, apub);
    fipsd_config_log log = {0}; assert(fipsd_config_reload(slot, yamlpath, &log));
    GoRefPtr next = go_swap_load(slot);
    assert(fipsd_acl_check(fipsd_config_acl(old.ptr), &a) == FIPSD_ACL_DENY_LIST);
    assert(fipsd_acl_check(fipsd_config_acl(next.ptr), &a) == FIPSD_ACL_ALLOW_LIST);
    go_refptr_release(&next); go_refptr_release(&old); go_swap_free(slot); fipsd_config_log_clear(&log);
    fipsd_acl_free(empty); fips_identity_destroy_secp(secp);
    puts("ACL: npub/alias/ALL precedence, default allow, invalid rules, hosts overrides, reload faults/empty guard/removal, retained generations: ok");
    return 0;
}
