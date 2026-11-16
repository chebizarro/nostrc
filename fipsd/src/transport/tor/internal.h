#ifndef FIPSD_TOR_INTERNAL_H
#define FIPSD_TOR_INTERNAL_H
#include "transport/tor/tor.h"
#include "transport/tor/control.h"
#include "transport/tcp/tcp.h"
#include "transport/socks5/socks5.h"
#include <stdatomic.h>
typedef enum tor_mode { TOR_MODE_SOCKS5, TOR_MODE_CONTROL_PORT, TOR_MODE_DIRECTORY } tor_mode;
typedef struct tor_instance {
    fipsd_transport transport;   /* the registered object; shared points into the inner owner */
    fipsd_transport *tcp;        /* inner TCP stream owner, same id, dialing through the proxy */
    fipsd_tor_config config;
    tor_mode mode;
    char name[128], socks5_addr[257], control_addr[257], control_auth[512], cookie_path[1024];
    char hostname_file[1024], bind_addr[257], onion[128];
    _Atomic bool has_onion;      /* onion written once by start, read by the control provider */
    fips_transport_addr proxy;
    fipsd_stream_dialer dialer;
    tor_control control;
    bool has_control, started;
    fipsd_runtime *runtime;
    GoContext *root;
} tor_instance;
bool tor_mode_parse(const char *, tor_mode *);
const char *tor_mode_name(tor_mode);
bool tor_host_port(const char *); /* upstream validate_host_port */
int tor_onion_read(tor_instance *); /* blocking executor file read, logs */
bool tor_target(const fips_transport_addr *, fipsd_socks5_target *);
int tor_upgrade(void *, int fd, const fips_transport_addr *, uint64_t deadline_ms, int *error);
#endif
