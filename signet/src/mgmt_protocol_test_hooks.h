/* SPDX-License-Identifier: MIT
 * Test-only seam; neither declaration nor implementation exists in normal builds. */
#ifndef SIGNET_MGMT_PROTOCOL_TEST_HOOKS_H
#define SIGNET_MGMT_PROTOCOL_TEST_HOOKS_H
#ifdef SIGNET_ENABLE_TEST_HOOKS
#include "signet/mgmt_protocol.h"
/* Receives every management reply as the plaintext the recipient would read
 * (a ContextVM reply in its JSON-RPC form) before it is encrypted, whether or
 * not relays are attached. reply_json is valid only during the call. */
typedef void (*SignetMgmtReplyHook)(void *data, const char *recipient_pubkey_hex,
                                    const char *reply_json);
void signet_mgmt_handler_set_reply_hook(SignetMgmtHandler *h,
                                        SignetMgmtReplyHook hook, void *data);
#endif
#endif
