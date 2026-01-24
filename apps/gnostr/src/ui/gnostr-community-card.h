/**
 * GnostrCommunityCard - NIP-72 Moderated Community Card Widget
 *
 * Displays a community card with:
 * - Community name and image
 * - Description text
 * - Rules summary
 * - Moderator count
 * - Member and post statistics
 * - Join/Leave button
 */

#ifndef GNOSTR_COMMUNITY_CARD_H
#define GNOSTR_COMMUNITY_CARD_H

#include <gtk/gtk.h>
#include "../util/nip72_communities.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_COMMUNITY_CARD (gnostr_community_card_get_type())

G_DECLARE_FINAL_TYPE(GnostrCommunityCard, gnostr_community_card, GNOSTR, COMMUNITY_CARD, GtkWidget)

/**
 * Signals:
 * "community-selected" (gchar* a_tag, gpointer user_data)
 *   - Emitted when user clicks to view the community feed
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks to view a moderator's profile
 * "join-community" (gchar* a_tag, gpointer user_data)
 *   - Emitted when user clicks to join the community
 * "leave-community" (gchar* a_tag, gpointer user_data)
 *   - Emitted when user clicks to leave the community
 * "create-post" (gchar* a_tag, gpointer user_data)
 *   - Emitted when user wants to create a post in the community
 */

typedef struct _GnostrCommunityCard GnostrCommunityCard;

/**
 * Create a new community card widget
 */
GnostrCommunityCard *gnostr_community_card_new(void);

/**
 * Set the community data for this card
 * @self: the card widget
 * @community: community data (copied internally)
 */
void gnostr_community_card_set_community(GnostrCommunityCard *self,
                                          const GnostrCommunity *community);

/**
 * Get the community "a" tag for this card
 * @return: "a" tag string or NULL
 */
const char *gnostr_community_card_get_a_tag(GnostrCommunityCard *self);

/**
 * Get the community d tag
 */
const char *gnostr_community_card_get_d_tag(GnostrCommunityCard *self);

/**
 * Get the community name
 */
const char *gnostr_community_card_get_name(GnostrCommunityCard *self);

/**
 * Get the community description
 */
const char *gnostr_community_card_get_description(GnostrCommunityCard *self);

/**
 * Get the creator's pubkey
 */
const char *gnostr_community_card_get_creator_pubkey(GnostrCommunityCard *self);

/**
 * Set the joined state
 * @self: the card widget
 * @is_joined: TRUE if user has joined this community
 */
void gnostr_community_card_set_joined(GnostrCommunityCard *self,
                                       gboolean is_joined);

/**
 * Get the joined state
 */
gboolean gnostr_community_card_get_joined(GnostrCommunityCard *self);

/**
 * Set the logged-in state (affects button sensitivity)
 * @self: the card widget
 * @logged_in: TRUE if user is logged in
 */
void gnostr_community_card_set_logged_in(GnostrCommunityCard *self,
                                          gboolean logged_in);

/**
 * Set whether the current user is a moderator of this community
 * @self: the card widget
 * @is_moderator: TRUE if user is a moderator
 */
void gnostr_community_card_set_is_moderator(GnostrCommunityCard *self,
                                             gboolean is_moderator);

/**
 * Update the post count display
 */
void gnostr_community_card_set_post_count(GnostrCommunityCard *self,
                                           guint count);

/**
 * Update the member count display
 */
void gnostr_community_card_set_member_count(GnostrCommunityCard *self,
                                             guint count);

G_END_DECLS

#endif /* GNOSTR_COMMUNITY_CARD_H */
