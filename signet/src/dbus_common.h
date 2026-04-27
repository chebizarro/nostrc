/* SPDX-License-Identifier: MIT
 *
 * dbus_common.h - Shared authenticated D-Bus Signer/Credentials dispatch.
 */

#ifndef SIGNET_DBUS_COMMON_H
#define SIGNET_DBUS_COMMON_H

#include <gio/gio.h>

struct SignetAuditLogger;
struct SignetKeyStore;
struct SignetPolicyRegistry;
struct SignetStore;

typedef struct {
  struct SignetKeyStore *keys;
  struct SignetPolicyRegistry *policy;
  struct SignetStore *store;
  struct SignetAuditLogger *audit;
  const char *transport;
} SignetDbusDispatchContext;

void signet_dbus_dispatch_authenticated(const SignetDbusDispatchContext *ctx,
                                        const char *agent_id,
                                        const gchar *interface_name,
                                        const gchar *method_name,
                                        GVariant *parameters,
                                        GDBusMethodInvocation *invocation);

#endif /* SIGNET_DBUS_COMMON_H */
