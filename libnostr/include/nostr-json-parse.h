/**
 * @file nostr-json-parse.h
 * @brief Shared JSON parsing primitives for compact deserializers.
 *
 * Provides common helpers used by event.c, envelope.c, and filter.c
 * compact JSON parsers: hex digit conversion, whitespace skipping,
 * UTF-8 encoding, JSON string unescaping (with surrogate pair support),
 * and simple integer parsing.
 */
#ifndef NOSTR_JSON_PARSE_H
#define NOSTR_JSON_PARSE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Convert a hex digit character to its integer value (0-15).
 * @param c ASCII character ('0'-'9', 'a'-'f', 'A'-'F')
 * @return Integer value 0-15, or -1 if not a valid hex digit.
 */
int nostr_json_hexval(char c);

/**
 * Skip JSON whitespace characters (space, tab, newline, carriage return).
 * @param p Pointer into JSON text.
 * @return Pointer to the first non-whitespace character.
 */
const char *nostr_json_skip_ws(const char *p);

/**
 * Encode a Unicode code point as UTF-8.
 * @param cp Unicode code point (0 to 0x10FFFF).
 * @param out Output buffer (must have room for at least 4 bytes).
 * @return Number of bytes written (1-4).
 */
int nostr_json_utf8_encode(uint32_t cp, char *out);

/**
 * Parse a JSON string value with full unescape support.
 *
 * Handles all standard JSON escape sequences including \\uXXXX and
 * UTF-16 surrogate pairs. Has a fast path: if no escape sequences
 * are present, performs a direct memcpy instead of character-by-character
 * decoding.
 *
 * @param pp Pointer to current position; must point at the opening '"'.
 *           Advanced past the closing '"' on success.
 * @return Newly allocated (malloc) unescaped string, or NULL on error.
 *         Caller must free() the result.
 */
char *nostr_json_parse_string(const char **pp);

/**
 * Parse a simple JSON integer (optional leading '-', decimal digits only).
 * Does not handle exponent notation.
 *
 * @param pp Pointer to current position (whitespace is skipped).
 *           Advanced past the last digit on success.
 * @param out Receives the parsed value.
 * @return 1 on success, 0 on failure (no digits found).
 */
int nostr_json_parse_int64(const char **pp, long long *out);

/* nostrc-737: Structured error codes for compact JSON parsers */

/**
 * NostrJsonErrorCode:
 * Fine-grained error codes for compact JSON parsing.
 */
typedef enum {
    NOSTR_JSON_OK                  = 0,

    /* 1-9: Input validation */
    NOSTR_JSON_ERR_NULL_INPUT      = 1,
    NOSTR_JSON_ERR_EXPECTED_OBJECT = 2,
    NOSTR_JSON_ERR_EXPECTED_ARRAY  = 3,

    /* 10-19: Structural JSON errors */
    NOSTR_JSON_ERR_TRUNCATED       = 10,
    NOSTR_JSON_ERR_BAD_STRING      = 11,
    NOSTR_JSON_ERR_BAD_NUMBER      = 12,
    NOSTR_JSON_ERR_BAD_KEY         = 13,
    NOSTR_JSON_ERR_EXPECTED_COLON  = 14,
    NOSTR_JSON_ERR_BAD_SEPARATOR   = 15,
    NOSTR_JSON_ERR_UNCLOSED_BRACE  = 16,
    NOSTR_JSON_ERR_SKIP_VALUE      = 17,

    /* 20-29: Semantic / limit errors */
    NOSTR_JSON_ERR_OVERFLOW        = 20,
    NOSTR_JSON_ERR_KIND_RANGE      = 21,
    NOSTR_JSON_ERR_TAG_LIMIT       = 22,
    NOSTR_JSON_ERR_DEPTH_LIMIT     = 23,
    NOSTR_JSON_ERR_ALLOC           = 24,

    /* 30-39: Envelope-specific */
    NOSTR_JSON_ERR_BAD_LABEL       = 30,
    NOSTR_JSON_ERR_LABEL_MISMATCH  = 31,
    NOSTR_JSON_ERR_MISSING_FIELD   = 32,
    NOSTR_JSON_ERR_BAD_BOOL        = 33,
    NOSTR_JSON_ERR_NESTED_EVENT    = 34,
    NOSTR_JSON_ERR_NESTED_FILTER   = 35
} NostrJsonErrorCode;

/**
 * NostrJsonErrorInfo:
 * Optional structured error output for compact parsers.
 * Stack-allocatable, no heap. Pass NULL to ignore errors.
 */
typedef struct NostrJsonErrorInfo {
    int code;      /* NostrJsonErrorCode */
    int offset;    /* byte offset into input, or -1 */
} NostrJsonErrorInfo;

/** Return a static human-readable string for an error code. */
const char *nostr_json_error_string(int code);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_JSON_PARSE_H */
