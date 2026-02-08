/**
 * NIP-77 Negentropy Protocol Handler for relayd
 *
 * Handles set reconciliation messages:
 * - NEG-OPEN: Opens a negentropy session with a filter scope
 * - NEG-MSG: Continues the reconciliation
 * - NEG-CLOSE: Closes the session
 *
 * Also supports the older NEGENTROPY message format for compatibility.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <libwebsockets.h>
#include "protocol_nip77.h"
#include "nostr-relay-core.h"
#include "nostr-storage.h"
#include "nostr-filter.h"

/* Send text over WebSocket */
static void ws_send_text(struct lws *wsi, const char *s) {
  if (!wsi || !s) return;
  size_t blen = strlen(s);
  unsigned char *buf = (unsigned char*)malloc(LWS_PRE + blen);
  if (!buf) return;
  memcpy(&buf[LWS_PRE], s, blen);
  lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT);
  free(buf);
}

/* Build NEG-MSG response: ["NEG-MSG", <sub_id>, <message>] */
static char *build_neg_msg(const char *sub_id, const char *msg_hex) {
  if (!sub_id) sub_id = "";
  if (!msg_hex) msg_hex = "";
  size_t len = 32 + strlen(sub_id) + strlen(msg_hex);
  char *buf = (char*)malloc(len);
  if (!buf) return NULL;
  snprintf(buf, len, "[\"NEG-MSG\",\"%s\",\"%s\"]", sub_id, msg_hex);
  return buf;
}

/* Build NEG-ERR response: ["NEG-ERR", <sub_id>, <reason>] */
static char *build_neg_err(const char *sub_id, const char *reason) {
  if (!sub_id) sub_id = "";
  if (!reason) reason = "unknown error";
  size_t len = 32 + strlen(sub_id) + strlen(reason);
  char *buf = (char*)malloc(len);
  if (!buf) return NULL;
  snprintf(buf, len, "[\"NEG-ERR\",\"%s\",\"%s\"]", sub_id, reason);
  return buf;
}

/* Extract quoted string from JSON-like position */
static int extract_quoted_string(const char *start, char *out, size_t out_size) {
  if (!start || !out || out_size == 0) return -1;
  out[0] = '\0';

  /* Find opening quote */
  const char *q1 = strchr(start, '"');
  if (!q1) return -1;

  /* Find closing quote */
  const char *q2 = strchr(q1 + 1, '"');
  if (!q2) return -1;

  size_t len = (size_t)(q2 - q1 - 1);
  if (len >= out_size) len = out_size - 1;

  memcpy(out, q1 + 1, len);
  out[len] = '\0';
  return 0;
}

/* Extract JSON object from position (handles nested braces).
 * Returns allocated string on success, NULL on failure. */
static char *extract_json_object(const char *start) {
  if (!start) return NULL;

  /* Skip whitespace to find opening brace */
  const char *p = start;
  while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) p++;

  if (*p != '{') return NULL;

  const char *obj_start = p;
  int depth = 0;
  bool in_string = false;
  bool escape = false;

  while (*p) {
    char c = *p;

    if (escape) {
      escape = false;
      p++;
      continue;
    }

    if (c == '\\' && in_string) {
      escape = true;
      p++;
      continue;
    }

    if (c == '"') {
      in_string = !in_string;
    } else if (!in_string) {
      if (c == '{') {
        depth++;
      } else if (c == '}') {
        depth--;
        if (depth == 0) {
          /* Found matching close brace */
          size_t len = (size_t)(p - obj_start + 1);
          char *result = (char*)malloc(len + 1);
          if (!result) return NULL;
          memcpy(result, obj_start, len);
          result[len] = '\0';
          return result;
        }
      }
    }
    p++;
  }

  return NULL; /* Unbalanced braces */
}

/* Handle NEG-OPEN: ["NEG-OPEN", <sub_id>, <filter>, <initial_msg>] */
static int handle_neg_open(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx,
                           const char *msg, size_t len) {
  char sub_id[128] = "";
  char msg_hex[8192] = "";
  NostrFilter *filter = NULL;
  (void)len;

  /* Parse subscription ID (second element) */
  const char *p = strchr(msg, ',');
  if (!p) {
    char *err = build_neg_err("", "malformed: missing subscription_id");
    if (err) { ws_send_text(wsi, err); free(err); }
    return 1;
  }

  if (extract_quoted_string(p, sub_id, sizeof(sub_id)) != 0) {
    char *err = build_neg_err("", "malformed: invalid subscription_id");
    if (err) { ws_send_text(wsi, err); free(err); }
    return 1;
  }

  /* Skip to filter (third element) */
  p = strchr(p + 1, ',');
  if (!p) {
    char *err = build_neg_err(sub_id, "malformed: missing filter");
    if (err) { ws_send_text(wsi, err); free(err); }
    return 1;
  }

  /* Parse filter JSON (NIP-77 filter scope) */
  char *filter_json = extract_json_object(p);
  if (filter_json) {
    filter = nostr_filter_new();
    if (filter) {
      if (nostr_filter_deserialize_compact(filter, filter_json, NULL) != 1) {
        /* Parse failed, use NULL filter (all events) */
        nostr_filter_free(filter);
        filter = NULL;
      }
    }
    free(filter_json);
  }
  /* If filter extraction/parsing fails, we proceed with NULL (all events) */

  /* Skip to message (fourth element) */
  p = strchr(p + 1, ',');
  if (!p) {
    char *err = build_neg_err(sub_id, "malformed: missing initial_message");
    if (err) { ws_send_text(wsi, err); free(err); }
    nostr_filter_free(filter);
    return 1;
  }

  if (extract_quoted_string(p, msg_hex, sizeof(msg_hex)) != 0) {
    char *err = build_neg_err(sub_id, "malformed: invalid initial_message");
    if (err) { ws_send_text(wsi, err); free(err); }
    nostr_filter_free(filter);
    return 1;
  }

  /* Clean up any existing session */
  if (cs->neg_state && ctx->storage && ctx->storage->vt->set_free) {
    ctx->storage->vt->set_free(ctx->storage, cs->neg_state);
    cs->neg_state = NULL;
  }

  /* Initialize negentropy session via storage backend */
  if (!ctx->storage || !ctx->storage->vt->set_digest) {
    char *err = build_neg_err(sub_id, "error: storage backend unavailable");
    if (err) { ws_send_text(wsi, err); free(err); }
    nostr_filter_free(filter);
    return 1;
  }

  int rc = ctx->storage->vt->set_digest(ctx->storage, filter, &cs->neg_state);
  nostr_filter_free(filter); /* Storage backend copies if needed */
  if (rc != 0) {
    const char *reason = (rc == -ENOSYS) ? "error: negentropy not implemented" :
                         (rc == -ENOTSUP) ? "error: negentropy not supported" :
                         "error: failed to initialize session";
    char *err = build_neg_err(sub_id, reason);
    if (err) { ws_send_text(wsi, err); free(err); }
    return 1;
  }

  /* Store subscription ID */
  strncpy(cs->neg_subid, sub_id, sizeof(cs->neg_subid) - 1);
  cs->neg_subid[sizeof(cs->neg_subid) - 1] = '\0';

  /* Process initial message */
  void *resp = NULL;
  size_t resp_len = 0;

  rc = ctx->storage->vt->set_reconcile(ctx->storage, cs->neg_state,
                                        msg_hex, strlen(msg_hex),
                                        &resp, &resp_len);
  if (rc != 0) {
    const char *reason = (rc == -ENOSYS) ? "error: reconciliation not implemented" :
                         "error: reconciliation failed";
    char *err = build_neg_err(sub_id, reason);
    if (err) { ws_send_text(wsi, err); free(err); }

    /* Clean up session on error */
    if (ctx->storage->vt->set_free) {
      ctx->storage->vt->set_free(ctx->storage, cs->neg_state);
    }
    cs->neg_state = NULL;
    cs->neg_subid[0] = '\0';
    return 1;
  }

  /* Send response */
  char *response = build_neg_msg(sub_id, resp ? (const char*)resp : "");
  if (response) {
    ws_send_text(wsi, response);
    free(response);
  }

  if (resp) free(resp);

  /* If response is empty, session is complete */
  if (resp_len == 0 || (resp && ((char*)resp)[0] == '\0')) {
    if (ctx->storage->vt->set_free) {
      ctx->storage->vt->set_free(ctx->storage, cs->neg_state);
    }
    cs->neg_state = NULL;
    cs->neg_subid[0] = '\0';
  }

  return 1;
}

/* Handle NEG-MSG: ["NEG-MSG", <sub_id>, <message>] */
static int handle_neg_msg(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx,
                          const char *msg, size_t len) {
  char sub_id[128] = "";
  char msg_hex[8192] = "";

  /* Parse subscription ID */
  const char *p = strchr(msg, ',');
  if (!p || extract_quoted_string(p, sub_id, sizeof(sub_id)) != 0) {
    char *err = build_neg_err("", "malformed: invalid subscription_id");
    if (err) { ws_send_text(wsi, err); free(err); }
    return 1;
  }

  /* Verify session exists */
  if (!cs->neg_state || strcmp(cs->neg_subid, sub_id) != 0) {
    char *err = build_neg_err(sub_id, "error: no active session");
    if (err) { ws_send_text(wsi, err); free(err); }
    return 1;
  }

  /* Parse message */
  p = strchr(p + 1, ',');
  if (!p || extract_quoted_string(p, msg_hex, sizeof(msg_hex)) != 0) {
    char *err = build_neg_err(sub_id, "malformed: invalid message");
    if (err) { ws_send_text(wsi, err); free(err); }
    return 1;
  }

  /* Process message */
  void *resp = NULL;
  size_t resp_len = 0;

  int rc = ctx->storage->vt->set_reconcile(ctx->storage, cs->neg_state,
                                            msg_hex, strlen(msg_hex),
                                            &resp, &resp_len);
  if (rc != 0) {
    char *err = build_neg_err(sub_id, "error: reconciliation failed");
    if (err) { ws_send_text(wsi, err); free(err); }
    return 1;
  }

  /* Send response */
  char *response = build_neg_msg(sub_id, resp ? (const char*)resp : "");
  if (response) {
    ws_send_text(wsi, response);
    free(response);
  }

  if (resp) free(resp);

  /* If response is empty, session is complete */
  if (resp_len == 0 || (resp && ((char*)resp)[0] == '\0')) {
    if (ctx->storage->vt->set_free) {
      ctx->storage->vt->set_free(ctx->storage, cs->neg_state);
    }
    cs->neg_state = NULL;
    cs->neg_subid[0] = '\0';
  }

  return 1;
}

/* Handle NEG-CLOSE: ["NEG-CLOSE", <sub_id>] */
static int handle_neg_close(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx,
                            const char *msg, size_t len) {
  char sub_id[128] = "";
  (void)wsi;
  (void)len;

  /* Parse subscription ID */
  const char *p = strchr(msg, ',');
  if (p) {
    extract_quoted_string(p, sub_id, sizeof(sub_id));
  }

  /* Clean up session if it matches */
  if (cs->neg_state && (sub_id[0] == '\0' || strcmp(cs->neg_subid, sub_id) == 0)) {
    if (ctx->storage && ctx->storage->vt->set_free) {
      ctx->storage->vt->set_free(ctx->storage, cs->neg_state);
    }
    cs->neg_state = NULL;
    cs->neg_subid[0] = '\0';
  }

  /* No response needed for NEG-CLOSE */
  return 1;
}

/* Handle legacy NEGENTROPY format: ["NEGENTROPY", <sub_id>, <filter>, <message>] */
static int handle_legacy_negentropy(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx,
                                    const char *msg, size_t len) {
  /* The legacy format is identical to NEG-OPEN */
  return handle_neg_open(wsi, cs, ctx, msg, len);
}

int relayd_nip77_handle_frame(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx,
                               const char *msg, size_t len) {
  if (!msg || len < 10) return 0;

  /* Check for NIP-77 message types */
  if (len >= 11 && memcmp(msg, "[\"NEG-OPEN\"", 11) == 0) {
    /* NEG-OPEN: Open negentropy session */
    if (!ctx || !ctx->cfg.negentropy_enabled) {
      char sub_id[128] = "";
      const char *p = strchr(msg, ',');
      if (p) extract_quoted_string(p, sub_id, sizeof(sub_id));
      char *err = build_neg_err(sub_id[0] ? sub_id : "sub", "error: negentropy disabled");
      if (err) { ws_send_text(wsi, err); free(err); }
      return 1;
    }
    return handle_neg_open(wsi, cs, ctx, msg, len);
  }

  if (len >= 10 && memcmp(msg, "[\"NEG-MSG\"", 10) == 0) {
    /* NEG-MSG: Continue reconciliation */
    if (!ctx || !ctx->cfg.negentropy_enabled) {
      char sub_id[128] = "";
      const char *p = strchr(msg, ',');
      if (p) extract_quoted_string(p, sub_id, sizeof(sub_id));
      char *err = build_neg_err(sub_id[0] ? sub_id : "sub", "error: negentropy disabled");
      if (err) { ws_send_text(wsi, err); free(err); }
      return 1;
    }
    return handle_neg_msg(wsi, cs, ctx, msg, len);
  }

  if (len >= 12 && memcmp(msg, "[\"NEG-CLOSE\"", 12) == 0) {
    /* NEG-CLOSE: Close session */
    return handle_neg_close(wsi, cs, ctx, msg, len);
  }

  if (len >= 14 && memcmp(msg, "[\"NEG-ERR\"", 10) == 0) {
    /* NEG-ERR from client - just ignore */
    return 1;
  }

  /* Legacy format: ["NEGENTROPY", ...] */
  if (len >= 14 && memcmp(msg, "[\"NEGENTROPY\"", 14) == 0) {
    if (!ctx || !ctx->cfg.negentropy_enabled) {
      char sub_id[128] = "";
      const char *p = strchr(msg, ',');
      if (p) extract_quoted_string(p, sub_id, sizeof(sub_id));
      char *closed = nostr_closed_build_json(sub_id[0] ? sub_id : "sub1", "unsupported: negentropy");
      if (closed) { ws_send_text(wsi, closed); free(closed); }
      return 1;
    }
    return handle_legacy_negentropy(wsi, cs, ctx, msg, len);
  }

  return 0; /* Not a NIP-77 message */
}
