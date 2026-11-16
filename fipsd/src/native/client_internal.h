#ifndef FIPSD_NATIVE_CLIENT_INTERNAL_H
#define FIPSD_NATIVE_CLIENT_INTERNAL_H
/* Test seams of libfipsnative, exported but not installed: the setup
 * exchange over a connected descriptor a test supplies, and streams and
 * listeners wrapped around descriptors a test already holds. Each takes
 * ownership of the descriptor it is given. */
#include <fips-native-client.h>
int fips_native__connect_on(int fd, uint16_t local_port, const fips_native_addr *peer, fips_native_stream **out);
int fips_native__listen_on(int fd, uint16_t local_port, fips_native_listener **out);
int fips_native__stream_wrap(int fd, const fips_native_addr *peer, const fips_native_addr *local, size_t max_payload,
                             fips_native_stream **out);
int fips_native__listener_wrap(int fd, const fips_native_addr *local, fips_native_listener **out);
/* The pure line protocol: the reply reader and the errno table. `data` and
 * `data_len` describe the `data` object of an ok reply; a refusal returns
 * the negative errno its `errno` field names (-ECONNREFUSED when absent). */
int fips_native__reply(const unsigned char *line, size_t len, const unsigned char **data, size_t *data_len);
int fips_native__errno_of(const char *name, size_t len);
#endif
