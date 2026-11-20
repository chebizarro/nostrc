#ifndef FIPSD_STUN_FIXTURE_H
#define FIPSD_STUN_FIXTURE_H
#include <channel.h>
#include <fips/transport/types.h>
#include <pthread.h>
#include <stdbool.h>
typedef struct {
    int fd, stop[2]; pthread_t thread; unsigned drop; bool blackhole;
    char url[80]; fips_transport_addr address; GoChannel *requests;
} stun_fixture;
typedef struct { fips_transport_addr source; uint8_t request[20]; } stun_request;
void stun_fixture_start(stun_fixture *, unsigned drop, bool blackhole);
void stun_fixture_stop(stun_fixture *);
#endif
