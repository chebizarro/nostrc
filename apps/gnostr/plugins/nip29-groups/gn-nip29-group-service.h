/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-group-service.h - NIP-29 group service/control plane
 */

#ifndef GN_NIP29_GROUP_SERVICE_H
#define GN_NIP29_GROUP_SERVICE_H

#include <glib-object.h>
#include <gio/gio.h>
#include <gnostr-plugin-api.h>
#include <nip29.h>

G_BEGIN_DECLS

#define GN_TYPE_NIP29_GROUP_SERVICE (gn_nip29_group_service_get_type())
G_DECLARE_FINAL_TYPE(GnNip29GroupService, gn_nip29_group_service, GN, NIP29_GROUP_SERVICE, GObject)

GnNip29GroupService *gn_nip29_group_service_new(GnostrPluginContext *context);

void         gn_nip29_group_service_shutdown          (GnNip29GroupService *self);
void         gn_nip29_group_service_set_current_pubkey(GnNip29GroupService *self,
                                                       const char          *pubkey_hex);
const char  *gn_nip29_group_service_get_current_pubkey(GnNip29GroupService *self);

guint        gn_nip29_group_service_get_group_count   (GnNip29GroupService *self);

gboolean     gn_nip29_group_service_track_group       (GnNip29GroupService *self,
                                                       const char          *relay_url,
                                                       const char          *group_id,
                                                       const char          *alias,
                                                       GError             **error);
void         gn_nip29_group_service_refresh_all       (GnNip29GroupService *self);

void         gn_nip29_group_service_create_group_async (GnNip29GroupService *self,
                                                        const char          *relay_url,
                                                        const char          *group_id,
                                                        const char          *name,
                                                        const char          *about,
                                                        const char          *picture,
                                                        gboolean             is_private,
                                                        gboolean             is_restricted,
                                                        gboolean             is_hidden,
                                                        gboolean             is_closed,
                                                        GCancellable        *cancellable,
                                                        GAsyncReadyCallback  callback,
                                                        gpointer             user_data);
gboolean     gn_nip29_group_service_create_group_finish(GnNip29GroupService *self,
                                                        GAsyncResult        *result,
                                                        GError             **error);
void         gn_nip29_group_service_join_group_async   (GnNip29GroupService *self,
                                                        const char          *group_key,
                                                        const char          *invite_code,
                                                        const char          *reason,
                                                        GCancellable        *cancellable,
                                                        GAsyncReadyCallback  callback,
                                                        gpointer             user_data);
gboolean     gn_nip29_group_service_join_group_finish  (GnNip29GroupService *self,
                                                        GAsyncResult        *result,
                                                        GError             **error);
void         gn_nip29_group_service_leave_group_async  (GnNip29GroupService *self,
                                                        const char          *group_key,
                                                        const char          *reason,
                                                        GCancellable        *cancellable,
                                                        GAsyncReadyCallback  callback,
                                                        gpointer             user_data);
gboolean     gn_nip29_group_service_leave_group_finish (GnNip29GroupService *self,
                                                        GAsyncResult        *result,
                                                        GError             **error);
void         gn_nip29_group_service_send_message_async (GnNip29GroupService *self,
                                                        const char          *group_key,
                                                        const char          *content,
                                                        GCancellable        *cancellable,
                                                        GAsyncReadyCallback  callback,
                                                        gpointer             user_data);
gboolean     gn_nip29_group_service_send_message_finish(GnNip29GroupService *self,
                                                        GAsyncResult        *result,
                                                        GError             **error);

/* ── UI-facing accessors (borrowed pointers, valid until next signal) ── */

GList              *gn_nip29_group_service_list_group_keys    (GnNip29GroupService *self);
const char         *gn_nip29_group_service_get_group_relay_url(GnNip29GroupService *self,
                                                               const char          *group_key);
const char         *gn_nip29_group_service_get_group_group_id (GnNip29GroupService *self,
                                                               const char          *group_key);
const char         *gn_nip29_group_service_get_group_alias    (GnNip29GroupService *self,
                                                               const char          *group_key);
const nostr_group_t *gn_nip29_group_service_get_group_data    (GnNip29GroupService *self,
                                                               const char          *group_key);
guint               gn_nip29_group_service_get_message_count_for_key(GnNip29GroupService *self,
                                                                     const char          *group_key);

typedef struct {
    const char *id;
    const char *event_json;
    gint64      created_at;
    gint        kind;
} GnNip29MessageRef;

gboolean gn_nip29_group_service_get_message_at(GnNip29GroupService *self,
                                                const char          *group_key,
                                                guint                index,
                                                GnNip29MessageRef   *out_ref);

G_END_DECLS

#endif /* GN_NIP29_GROUP_SERVICE_H */
