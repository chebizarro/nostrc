#ifndef RELAY_H
#define RELAY_H

#include "nostr.h"
#include <openssl/types.h>
#include <bits/pthreadtypes.h>


struct _RelayPrivate {
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    int socket;

    char *challenge;

    pthread_mutex_t mutex;
    bool assume_valid;
    void (*notice_handler)(const char *);
    bool (*signature_checker)(NostrEvent);
};


#endif // RELAY_H