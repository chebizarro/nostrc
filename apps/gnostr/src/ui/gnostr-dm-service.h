#ifndef GNOSTR_DM_SERVICE_H
#define GNOSTR_DM_SERVICE_H

#include <glib-object.h>
#include <gio/gio.h>
#include "gnostr-dm-inbox-view.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_DM_SERVICE (gnostr_dm_service_get_type())

G_DECLARE_FINAL_TYPE(GnostrDmService, gnostr_dm_service, GNOSTR, DM_SERVICE, GObject)

/**
 * GnostrDmService - Manages NIP-17 private direct message processing
 *
 * This service:
 * - Subscribes to gift wrap events (kind 1059) from relays
 * - Decrypts gift wraps using the signer D-Bus interface (NIP-44)
 * - Extracts seals (kind 13) and rumors (kind 14)
 * - Maintains conversation state per peer
 * - Updates the DM inbox view with decrypted messages
 *
 * The decryption flow:
 * 1. Receive gift wrap (kind 1059) from relay
 * 2. Decrypt gift wrap content using NIP-44 with ephemeral pubkey
 * 3. Parse seal (kind 13) from decrypted content
 * 4. Verify seal signature
 * 5. Decrypt seal content using NIP-44 with sender pubkey
 * 6. Parse rumor (kind 14) - the actual DM content
 * 7. Verify seal pubkey == rumor pubkey (anti-spoofing)
 * 8. Extract message content and update inbox
 */

typedef struct _GnostrDmService GnostrDmService;

/**
 * gnostr_dm_service_new:
 *
 * Creates a new DM service instance.
 *
 * Returns: (transfer full): a new DM service
 */
GnostrDmService *gnostr_dm_service_new(void);

/**
 * gnostr_dm_service_set_inbox_view:
 * @self: the DM service
 * @inbox: (transfer none): the inbox view to update
 *
 * Sets the inbox view that will receive decrypted message updates.
 */
void gnostr_dm_service_set_inbox_view(GnostrDmService *self,
                                       GnostrDmInboxView *inbox);

/**
 * gnostr_dm_service_set_user_pubkey:
 * @self: the DM service
 * @pubkey_hex: the logged-in user's public key (64 hex chars)
 *
 * Sets the current user's public key for determining message direction.
 */
void gnostr_dm_service_set_user_pubkey(GnostrDmService *self,
                                        const char *pubkey_hex);

/**
 * gnostr_dm_service_start:
 * @self: the DM service
 * @relay_urls: NULL-terminated array of relay URLs
 *
 * Starts subscribing to gift wrap events from the specified relays.
 * The service will decrypt received gift wraps and update the inbox.
 */
void gnostr_dm_service_start(GnostrDmService *self,
                              const char **relay_urls);

/**
 * gnostr_dm_service_stop:
 * @self: the DM service
 *
 * Stops the gift wrap subscription.
 */
void gnostr_dm_service_stop(GnostrDmService *self);

/**
 * gnostr_dm_service_process_gift_wrap:
 * @self: the DM service
 * @gift_wrap_json: JSON string of a gift wrap event
 *
 * Processes a single gift wrap event. This is useful for handling
 * gift wraps received through other channels (e.g., from storage).
 * The decryption happens asynchronously via the signer D-Bus interface.
 */
void gnostr_dm_service_process_gift_wrap(GnostrDmService *self,
                                          const char *gift_wrap_json);

/**
 * gnostr_dm_service_get_conversation_count:
 * @self: the DM service
 *
 * Returns the number of active conversations.
 *
 * Returns: number of conversations
 */
guint gnostr_dm_service_get_conversation_count(GnostrDmService *self);

/**
 * gnostr_dm_service_mark_read:
 * @self: the DM service
 * @peer_pubkey: pubkey of conversation to mark read
 *
 * Marks all messages in a conversation as read.
 */
void gnostr_dm_service_mark_read(GnostrDmService *self,
                                  const char *peer_pubkey);

G_END_DECLS

#endif /* GNOSTR_DM_SERVICE_H */
