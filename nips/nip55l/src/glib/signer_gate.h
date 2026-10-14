/* Shared authorisation gate. All submission/cancellation and reply callbacks
 * run on the exported signer's owning GMainContext. Transport adapters marshal
 * authenticated immutable inputs there; never identify a remote peer by a label. */
#ifndef NIP55L_SIGNER_GATE_H
#define NIP55L_SIGNER_GATE_H
#include "signer_caller.h"
G_BEGIN_DECLS

typedef enum {
  SIGNER_GATE_GET_PUBLIC_KEY,
  SIGNER_GATE_SIGN_EVENT,
  SIGNER_GATE_NIP44_ENCRYPT,
  SIGNER_GATE_NIP44_DECRYPT,
  SIGNER_GATE_NIP04_ENCRYPT,
  SIGNER_GATE_NIP04_DECRYPT,
  SIGNER_GATE_CONNECT,
  SIGNER_GATE_PING,
  SIGNER_GATE_GET_RELAYS,
  SIGNER_GATE_CONVERSATION_KEY,
  SIGNER_GATE_NIP44_ENCRYPT_B64,
  SIGNER_GATE_NIP44_DECRYPT_B64,
  SIGNER_GATE_DECRYPT_ZAP,
  SIGNER_GATE_SELF_TEST, /* approver-only MANAGEMENT, always one-time */
  SIGNER_GATE_OP_COUNT
} SignerGateOp;

typedef enum {
  SIGNER_TRANSPORT_DBUS, SIGNER_TRANSPORT_NIP5F,
  SIGNER_TRANSPORT_NIP46, SIGNER_TRANSPORT_MANAGEMENT
} SignerTransport;

typedef struct {
  SignerTransport transport;
  gboolean no_identity; /* only CONNECT/PING; selector must be empty */
  const gchar *claimed_name; /* untrusted display label, never a credential */
  const gchar *reason;
  const gchar *purpose;
  const gchar *flow_id;
  gboolean existing_pairing; /* CONNECT only: authenticated NIP-46 adapter verified persisted admission */
  gboolean no_approval_ui; /* fail promptly if scoped policy requires review */
  gint64 deadline; /* monotonic microseconds; 0 = service's 300 s bound */
  /* MANAGEMENT self-test only, on the owner context; borrowed until reply. */
  void (*management_testing)(gpointer data);
  gpointer management_data;
} SignerGateContext;

typedef struct _SignerGateHandle SignerGateHandle;
/* Exactly once, including cancellation; all strings borrowed. Result can be
 * plaintext: copy only when needed and wipe the copy. May run before submit
 * returns. Callers may release a handle without cancelling the operation. */
typedef void (*SignerGateReplyFn)(gpointer user_data, const gchar *error_name,
                                  const gchar *message, const gchar *result);
SignerGateHandle *signer_gate_handle_ref(SignerGateHandle *handle);
void signer_gate_handle_unref(SignerGateHandle *handle);
void signer_gate_cancel(SignerGateHandle *handle);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SignerGateHandle, signer_gate_handle_unref)

/* Existing arguments preserved; the new return value is an owned cancel
 * handle. Identical principal+identity+op+canonical arguments share a prompt,
 * not a backend execution. Disconnect removes only that connection's replies. */
SignerGateHandle *signer_gate_submit(const SignerCaller *who, const gchar *conn_key,
                        SignerGateOp op, const gchar *a, const gchar *b,
                        const gchar *selector, SignerGateReplyFn reply, gpointer user_data);
SignerGateHandle *signer_gate_submit_full(const SignerCaller *who, const gchar *conn_key,
                        SignerGateOp op, const gchar *a, const gchar *b,
                        const gchar *selector, const SignerGateContext *context,
                        SignerGateReplyFn reply, gpointer user_data);
void signer_gate_connection_closed(const gchar *conn_key);
G_END_DECLS
#endif
