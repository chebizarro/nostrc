#include "gh-signer-private.h"

struct _GhNip46Signer {
  GhNip46Session *session;
};

GhNip46Signer *
gh_signer_nip46_new(GhNip46Session *session)
{
  g_return_val_if_fail(GH_IS_NIP46_SESSION(session), NULL);
  GhNip46Signer *self = g_new0(GhNip46Signer, 1);
  self->session = g_object_ref(session);
  return self;
}

void
gh_signer_nip46_free(GhNip46Signer *self)
{
  if (!self) return;
  gh_nip46_session_cancel(self->session);
  g_object_unref(self->session);
  g_free(self);
}

void
gh_signer_nip46_call_async(GhNip46Signer *self, const gchar *method,
                                const gchar *input, const gchar *peer,
                                GCancellable *cancel, GAsyncReadyCallback cb, gpointer data)
{
  g_return_if_fail(self != NULL);
  const gchar *params[2] = { peer ? peer : input, peer ? input : NULL };
  gh_nip46_session_call_async(self->session, method, params, peer ? 2 : 1,
                              cancel, cb, data);
}

/* The session messages are user-facing and keep the sanitised text of the
 * signer (nostrc-8xfib.1); only the error domain and code are mapped. */
static GError *
map_error(const GError *error)
{
  static const struct { GhNip46SessionError from; GhSignerError to; } codes[] = {
    { GH_NIP46_SESSION_ERROR_INVALID_INPUT, GH_SIGNER_ERROR_INVALID_INPUT },
    { GH_NIP46_SESSION_ERROR_DENIED, GH_SIGNER_ERROR_DENIED },
    { GH_NIP46_SESSION_ERROR_TIMED_OUT, GH_SIGNER_ERROR_TIMED_OUT },
    { GH_NIP46_SESSION_ERROR_CANCELLED, GH_SIGNER_ERROR_CANCELLED },
    { GH_NIP46_SESSION_ERROR_INVALID_RESULT, GH_SIGNER_ERROR_INVALID_RESULT },
    { GH_NIP46_SESSION_ERROR_UNAVAILABLE, GH_SIGNER_ERROR_UNAVAILABLE },
  };
  if (error->domain == GH_NIP46_SESSION_ERROR)
    for (gsize i = 0; i < G_N_ELEMENTS(codes); i++)
      if (codes[i].from == (GhNip46SessionError)error->code)
        return g_error_new_literal(GH_SIGNER_ERROR, codes[i].to, error->message);
  return g_error_new_literal(GH_SIGNER_ERROR, GH_SIGNER_ERROR_UNAVAILABLE,
                             "Remote signer is unavailable");
}

gchar *
gh_signer_nip46_call_finish(GhNip46Signer *self, GAsyncResult *result,
                                    GError **error)
{
  g_autoptr(GError) session_error = NULL;
  gchar *value = gh_nip46_session_call_finish(self->session, result, &session_error);
  if (!value && session_error)
    g_propagate_error(error, map_error(session_error));
  return value;
}
