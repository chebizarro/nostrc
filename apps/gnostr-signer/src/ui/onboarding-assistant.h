/**
 * onboarding-assistant.h - Multi-step onboarding wizard for GNostr Signer
 *
 * SPDX-License-Identifier: MIT
 *
 * Provides a guided onboarding experience for new users, including:
 * - Welcome explanation of what gnostr-signer does
 * - Security overview of key protection
 * - Create or Import key path selection
 * - Passphrase setup with strength meter (for create flow)
 * - BIP-39 seed phrase generation and display (for create flow)
 * - Import method selection (NIP-49 / mnemonic / file) for import flow
 * - Backup reminder with mandatory acknowledgment
 * - Ready/Get Started summary
 */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_ONBOARDING_ASSISTANT (onboarding_assistant_get_type())
G_DECLARE_FINAL_TYPE(OnboardingAssistant, onboarding_assistant, ONBOARDING, ASSISTANT, AdwWindow)

/**
 * onboarding_assistant_new:
 *
 * Creates a new onboarding assistant window.
 *
 * Returns: (transfer full): a new #OnboardingAssistant
 */
OnboardingAssistant *onboarding_assistant_new(void);

/**
 * OnboardingAssistantFinishedCb:
 * @completed: %TRUE if user completed onboarding, %FALSE if skipped/cancelled
 * @user_data: user data passed to the callback
 *
 * Callback invoked when onboarding finishes (completed or skipped).
 */
typedef void (*OnboardingAssistantFinishedCb)(gboolean completed, gpointer user_data);

/**
 * onboarding_assistant_set_on_finished:
 * @self: an #OnboardingAssistant
 * @cb: callback to invoke when onboarding is finished
 * @user_data: user data for callback
 *
 * Sets a callback to be invoked when the user finishes or skips onboarding.
 */
void onboarding_assistant_set_on_finished(OnboardingAssistant *self,
                                          OnboardingAssistantFinishedCb cb,
                                          gpointer user_data);

/**
 * onboarding_assistant_check_should_show:
 *
 * Checks GSettings to determine if onboarding should be shown.
 *
 * Returns: %TRUE if onboarding should be displayed (first run), %FALSE otherwise
 */
gboolean onboarding_assistant_check_should_show(void);

/**
 * onboarding_assistant_mark_completed:
 *
 * Marks onboarding as completed in GSettings so it won't show again.
 */
void onboarding_assistant_mark_completed(void);

/**
 * onboarding_assistant_reset:
 *
 * Resets onboarding state so it will show again on next launch.
 * Useful for allowing users to re-run onboarding from settings.
 */
void onboarding_assistant_reset(void);

G_END_DECLS
