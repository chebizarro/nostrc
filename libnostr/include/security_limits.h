#ifndef NOSTR_SECURITY_LIMITS_H
#define NOSTR_SECURITY_LIMITS_H

/* Centralized security limits for parser and transport. Make configurable later. */

/* Transport/websocket */
#define NOSTR_MAX_FRAME_LEN_BYTES (2 * 1024 * 1024) /* 2MB */
#define NOSTR_MAX_FRAMES_PER_SEC 2000		    /* 2000 frames/sec - high for burst traffic */
#define NOSTR_MAX_BYTES_PER_SEC (50 * 1024 * 1024)  /* 50MB/s - high for burst traffic */

/* Event/JSON */
#define NOSTR_MAX_EVENT_SIZE_BYTES (256 * 1024) /* 256KB */
#define NOSTR_MAX_TAGS_PER_EVENT 100
#define NOSTR_MAX_TAG_DEPTH 4

/* Filters */
#define NOSTR_MAX_FILTERS_PER_REQ 20
#define NOSTR_MAX_IDS_PER_FILTER 500

/* Security: invalid signature controls */
#define NOSTR_INVALID_SIG_WINDOW_SECONDS 60 /* sliding window */
#define NOSTR_INVALID_SIG_THRESHOLD 20	    /* fails per window to trigger ban */
#define NOSTR_INVALID_SIG_BAN_SECONDS 300   /* 5 minutes ban */

#endif /* NOSTR_SECURITY_LIMITS_H */
