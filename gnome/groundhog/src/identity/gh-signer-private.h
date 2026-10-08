#ifndef GH_SIGNER_PRIVATE_H
#define GH_SIGNER_PRIVATE_H

#include "gh-signer.h"
#include "gh-nip46-session.h"

typedef struct _GhNip55lSigner GhNip55lSigner;
typedef struct _GhNip46Signer GhNip46Signer;

GhNip55lSigner *gh_signer_nip55l_new(GDBusConnection *bus, const gchar *npub,
                                      GError **error);
void gh_signer_nip55l_free(GhNip55lSigner *signer);
gboolean gh_signer_nip55l_select(GhNip55lSigner *signer, const gchar *npub,
                                  GError **error);
void gh_signer_nip55l_sign_async(GhNip55lSigner *signer, const gchar *event,
                                 GCancellable *cancel, GAsyncReadyCallback cb, gpointer data);
gchar *gh_signer_nip55l_sign_finish(GAsyncResult *result, GError **error);
void gh_signer_nip55l_nip44_encrypt_async(GhNip55lSigner *signer, const gchar *text,
                                           const gchar *peer, GCancellable *cancel,
                                           GAsyncReadyCallback cb, gpointer data);
void gh_signer_nip55l_nip44_decrypt_async(GhNip55lSigner *signer, const gchar *text,
                                           const gchar *peer, GCancellable *cancel,
                                           GAsyncReadyCallback cb, gpointer data);
void gh_signer_nip55l_nip04_decrypt_async(GhNip55lSigner *signer, const gchar *text,
                                           const gchar *peer, GCancellable *cancel,
                                           GAsyncReadyCallback cb, gpointer data);
gchar *gh_signer_nip55l_nip44_finish(GAsyncResult *result, GError **error);

GhNip46Signer *gh_signer_nip46_new(GhNip46Session *session);
void gh_signer_nip46_free(GhNip46Signer *signer);
void gh_signer_nip46_call_async(GhNip46Signer *signer, const gchar *method,
                                const gchar *input, const gchar *peer,
                                GCancellable *cancel, GAsyncReadyCallback cb, gpointer data);
gchar *gh_signer_nip46_call_finish(GhNip46Signer *signer, GAsyncResult *result,
                                    GError **error);

#endif
