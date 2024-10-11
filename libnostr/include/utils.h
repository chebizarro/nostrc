#ifndef UTILS_H
#define UTILS_H

#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Constants
#define MAX_LOCKS 50

// Mutex pool
extern pthread_mutex_t named_mutex_pool[MAX_LOCKS];

// Function prototypes
uint64_t memhash(const char *data, size_t len);
void named_lock(const char *name, void (*critical_section)(void *), void *arg);
bool similar(const int *as, size_t as_len, const int *bs, size_t bs_len);
char *escape_string(const char *s);
bool are_pointer_values_equal(const void *a, const void *b, size_t size);

char *normalize_url(const char *u);
char *normalize_ok_message(const char *reason, const char *prefix);

bool hex2bin(unsigned char *bin, const char *hex, size_t bin_len);

int64_t sub_id_to_serial(const char *sub_id);

char *escape_string(const char *str);

#endif // UTILS_H