#pragma once
#include <gio/gio.h>
#include "signer_approval_service.h"
G_BEGIN_DECLS
#define GROTTO_TYPE_SESSION_LOCK (grotto_session_lock_get_type())
G_DECLARE_FINAL_TYPE(GrottoSessionLock, grotto_session_lock, GROTTO, SESSION_LOCK, GObject)
/* NULL bus selects the system bus; an injected bus is for isolated tests. */
GrottoSessionLock *grotto_session_lock_new(SignerApprovalService *approval, GSettings *settings, GDBusConnection *bus);
void grotto_session_lock_stop(GrottoSessionLock *self);
gboolean grotto_session_lock_available(GrottoSessionLock *self);
gboolean grotto_session_lock_locked(GrottoSessionLock *self);
gboolean grotto_session_lock_enabled(GrottoSessionLock *self);
gboolean grotto_session_lock_can_resume(GrottoSessionLock *self);
G_END_DECLS
