/* The real, cancellable identity setup assistant. No creation password. */
#pragma once
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_ONBOARDING_ASSISTANT (onboarding_assistant_get_type())
G_DECLARE_FINAL_TYPE(OnboardingAssistant, onboarding_assistant, ONBOARDING, ASSISTANT, AdwWindow)
typedef enum {
  ONBOARDING_WELCOME, ONBOARDING_CHOICE, ONBOARDING_CREATE, ONBOARDING_IMPORT,
  ONBOARDING_CONNECT, ONBOARDING_TEST_WAITING, ONBOARDING_TESTING, ONBOARDING_TEST_FAILED,
  ONBOARDING_BACKUP, ONBOARDING_READY
} OnboardingState;
typedef enum { ONBOARDING_TEST_NONE, ONBOARDING_TEST_PASSED, ONBOARDING_TEST_DENIED,
  ONBOARDING_TEST_EXPIRED, ONBOARDING_TEST_KEYRING_UNAVAILABLE, ONBOARDING_TEST_VERIFICATION_FAILED } OnboardingTestResult;
OnboardingTestResult onboarding_assistant_get_test_result(OnboardingAssistant *self);
typedef void (*OnboardingAssistantFinishedCb)(gboolean verified, gpointer user_data);
OnboardingAssistant *onboarding_assistant_new(void);
void onboarding_assistant_present(GtkWindow *parent);
void onboarding_assistant_verify_identity(GtkWindow *parent, const gchar *npub);
void onboarding_assistant_set_on_finished(OnboardingAssistant *self, OnboardingAssistantFinishedCb cb, gpointer user_data);
OnboardingState onboarding_assistant_get_state(OnboardingAssistant *self);
gboolean onboarding_assistant_identity_stored(OnboardingAssistant *self);
gboolean onboarding_assistant_self_test_passed(OnboardingAssistant *self);
const gchar *onboarding_assistant_get_identity(OnboardingAssistant *self);
gboolean onboarding_assistant_check_should_show(void);
void onboarding_assistant_mark_completed(void);
void onboarding_assistant_reset(void);
G_END_DECLS
