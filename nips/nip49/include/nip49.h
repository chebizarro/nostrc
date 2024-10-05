#ifndef NIP49_H
#define NIP49_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t KeySecurityByte;

#define KNOWN_TO_HAVE_BEEN_HANDLED_INSECURELY    0x00
#define NOT_KNOWN_TO_HAVE_BEEN_HANDLED_INSECURELY 0x01
#define CLIENT_DOES_NOT_TRACK_THIS_DATA          0x02

int nip49_encrypt(const char *secret_key, const char *password, uint8_t logn, KeySecurityByte ksb, char **b32code);
int nip49_decrypt(const char *b32code, const char *password, char **secret_key);

#endif // NIP49_H
