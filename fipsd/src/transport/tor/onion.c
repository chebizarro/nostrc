#define _POSIX_C_SOURCE 200809L
#include "transport/tor/internal.h"
#include "blocking_executor.h"
#include "log.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define MODULE "tor"
bool tor_mode_parse(const char *s, tor_mode *out)
{
    if (!s) return false;
    if (!strcmp(s, "socks5")) { *out = TOR_MODE_SOCKS5; return true; }
    if (!strcmp(s, "control_port")) { *out = TOR_MODE_CONTROL_PORT; return true; }
    if (!strcmp(s, "directory")) { *out = TOR_MODE_DIRECTORY; return true; }
    return false;
}
const char *tor_mode_name(tor_mode m)
{ return m == TOR_MODE_DIRECTORY ? "directory" : m == TOR_MODE_CONTROL_PORT ? "control_port" : "socks5"; }
bool tor_host_port(const char *s)
{ fipsd_socks5_target t; return s && fipsd_socks5_target_parse(s, strlen(s), false, &t); }
bool tor_target(const fips_transport_addr *a, fipsd_socks5_target *out)
{
    const char *text; size_t len;
    if (!a || fips_transport_addr_as_str(a, &text, &len) != FIPS_OK) return false;
    return fipsd_socks5_target_parse(text, len, true, out);
}
typedef struct hostname_read { tor_instance *x; int error; char text[256]; } hostname_read;
static void *read_hostname(void *arg)
{
    hostname_read *r = arg;
    FILE *f = fopen(r->x->hostname_file, "r");
    if (!f) { r->error = errno ? errno : EIO; return NULL; }
    size_t n = fread(r->text, 1, sizeof r->text - 1, f);
    if (ferror(f)) r->error = errno ? errno : EIO;
    r->text[n] = 0; fclose(f);
    return NULL;
}
int tor_onion_read(tor_instance *x)
{
    hostname_read r = {.x = x};
    go_blocking_submit(read_hostname, &r);
    if (r.error) {
        FIPSD_ERROR(MODULE, "%s: failed to read onion hostname from '%s': %s (ensure HiddenServiceDir is configured in torrc and Tor has started)",
            x->name, x->hostname_file, strerror(r.error));
        return -1;
    }
    char *s = r.text, *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = 0;
    size_t n = strlen(s);
    if (n < 7 || n >= sizeof x->onion || strcmp(s + n - 6, ".onion") || strchr(s, ':')) {
        FIPSD_ERROR(MODULE, "%s: invalid onion address in '%s': '%s'", x->name, x->hostname_file, s);
        return -1;
    }
    memcpy(x->onion, s, n + 1);
    atomic_store_explicit(&x->has_onion, true, memory_order_release);
    FIPSD_INFO(MODULE, "%s: directory-mode onion service active (onion=%s listener=%s hostname_file=%s)",
        x->name, x->onion, x->bind_addr, x->hostname_file);
    return 0;
}
int tor_upgrade(void *user, int fd, const fips_transport_addr *peer, uint64_t deadline, int *error)
{
    tor_instance *x = user;
    fipsd_socks5_target target; const char *text; size_t len;
    if (!tor_target(peer, &target) || fips_transport_addr_as_str(peer, &text, &len) != FIPS_OK || len > 255) {
        *error = EINVAL; return -1;
    }
    /* Stream isolation: Tor does not verify these, they key the circuit. */
    char password[256]; memcpy(password, text, len); password[len] = 0;
    fipsd_socks5_auth auth = {.username = "fips", .password = password};
    uint8_t rep = 0;
    fipsd_socks5_result r = fipsd_socks5_handshake(fd, &auth, &target, deadline, &rep);
    if (r == FIPSD_SOCKS5_OK) return 0;
    *error = fipsd_socks5_errno(r);
    if (r != FIPSD_SOCKS5_CANCELLED)
        FIPSD_DEBUG(MODULE, "%s: SOCKS5 connect to %.*s failed: %s (rep=%u)", x->name, (int)len, text,
            fipsd_socks5_result_name(r), (unsigned)rep);
    return -1;
}
