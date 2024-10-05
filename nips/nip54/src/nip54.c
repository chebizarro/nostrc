#include "nip54.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unicode/utrans.h>
#include <unicode/ucnv.h>
#include <unicode/unorm2.h>
#include <unicode/ustring.h>
#include <unicode/ustdio.h>

static void to_lower(char *str) {
    for (int i = 0; str[i]; i++) {
        str[i] = tolower((unsigned char)str[i]);
    }
}

char *normalize_identifier(const char *name) {
    if (name == NULL) {
        return NULL;
    }

    // Convert to UTF-16
    UErrorCode status = U_ZERO_ERROR;
    int32_t src_len = (int32_t)strlen(name);
    int32_t dest_len = src_len * 2 + 1; // Just to ensure enough space for UTF-16
    UChar *utf16_str = (UChar *)malloc(dest_len * sizeof(UChar));
    u_strFromUTF8(utf16_str, dest_len, NULL, name, src_len, &status);

    if (U_FAILURE(status)) {
        free(utf16_str);
        return NULL;
    }

    // Normalize using NFKC
    const UNormalizer2 *norm = unorm2_getInstance(NULL, "nfkc", UNORM2_COMPOSE, &status);
    if (U_FAILURE(status)) {
        free(utf16_str);
        return NULL;
    }

    int32_t norm_len = unorm2_normalize(norm, utf16_str, -1, NULL, 0, &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
        free(utf16_str);
        return NULL;
    }

    UChar *norm_str = (UChar *)malloc((norm_len + 1) * sizeof(UChar));
    status = U_ZERO_ERROR;
    unorm2_normalize(norm, utf16_str, -1, norm_str, norm_len + 1, &status);
    free(utf16_str);

    if (U_FAILURE(status)) {
        free(norm_str);
        return NULL;
    }

    // Convert back to UTF-8
    char *utf8_norm_str = (char *)malloc((norm_len * 3 + 1) * sizeof(char));
    u_strToUTF8(utf8_norm_str, norm_len * 3 + 1, NULL, norm_str, norm_len, &status);
    free(norm_str);

    if (U_FAILURE(status)) {
        free(utf8_norm_str);
        return NULL;
    }

    // Process the normalized string
    to_lower(utf8_norm_str);
    int len = strlen(utf8_norm_str);
    for (int i = 0; i < len; i++) {
        if (!isalnum((unsigned char)utf8_norm_str[i])) {
            utf8_norm_str[i] = '-';
        }
    }

    return utf8_norm_str;
}
