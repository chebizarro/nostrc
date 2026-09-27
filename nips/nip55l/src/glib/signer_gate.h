/* signer_gate.h - the org.nostr.Signer access-control path (grants, then
 * ApprovalRequested / ApproveRequest; see signer_service_g.c) for transports
 * other than D-Bus. The NIP-5F socket uses it (nostrc-q23h), so a request
 * over the socket is decided exactly like the same D-Bus call from the same
 * process: same principal, same signer-grants.ini entries, same prompt.
 *
 * Main context only (the one the D-Bus service runs on). */
#ifndef NIP55L_SIGNER_GATE_H
#define NIP55L_SIGNER_GATE_H

#include "signer_caller.h"

G_BEGIN_DECLS

typedef enum {
  SIGNER_GATE_GET_PUBLIC_KEY,  /* result: npub of the active identity */
  SIGNER_GATE_SIGN_EVENT,      /* a = event JSON; result: signed event JSON */
  SIGNER_GATE_NIP44_ENCRYPT,   /* a = plaintext, b = peer pubkey hex; result: payload */
  SIGNER_GATE_NIP44_DECRYPT,   /* a = payload, b = peer pubkey hex; result: plaintext */
} SignerGateOp;

/* Exactly one call per request, possibly before signer_gate_submit returns:
 * either @error_name (an org.nostr.Signer.Error.* name) and @message, or
 * @result. All borrowed; @result may be plaintext, so copy and wipe it. */
typedef void (*SignerGateReplyFn)(gpointer user_data, const gchar *error_name,
                                  const gchar *message, const gchar *result);

/* Decide and run @op for @who. @conn_key names the connection: calls queued
 * on one connection share a prompt, and signer_gate_connection_closed drops
 * them. @selector: identity as the caller named it (NULL/"" = active). The
 * D-Bus service must be exported (signer_export) for prompts to be raised. */
void signer_gate_submit(const SignerCaller *who, const gchar *conn_key, SignerGateOp op,
                        const gchar *a, const gchar *b, const gchar *selector,
                        SignerGateReplyFn reply, gpointer user_data);

/* @conn_key closed: fail its parked requests and forget its rate limits. */
void signer_gate_connection_closed(const gchar *conn_key);

G_END_DECLS

#endif /* NIP55L_SIGNER_GATE_H */
