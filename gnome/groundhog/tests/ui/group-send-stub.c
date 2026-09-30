#include "group-send-stub.h"

#define MAX_DELEGATES 4

static GhSendUiDelegate recorded[MAX_DELEGATES];
static gpointer recorded_data[MAX_DELEGATES];
static guint n_recorded;
static gboolean has_delegate;
static guint refreshes;

void
gh_send_ui_set_delegate(GhWindow *window, const GhSendUiDelegate *delegate, gpointer data)
{
  g_assert_true(GH_IS_WINDOW(window));
  has_delegate = delegate != NULL;
  n_recorded = 0;
  if (delegate) {
    recorded[0] = *delegate;
    recorded_data[0] = data;
    n_recorded = 1;
  }
}

void
gh_send_ui_add_delegate(GhWindow *window, const GhSendUiDelegate *delegate, gpointer data)
{
  g_assert_true(GH_IS_WINDOW(window));
  g_assert_nonnull(delegate);
  guint slot = 0;
  while (slot < n_recorded && recorded[slot].handles != delegate->handles)
    slot++;
  g_assert_cmpuint(slot, <, MAX_DELEGATES);
  recorded[slot] = *delegate;
  recorded_data[slot] = data;
  if (slot == n_recorded)
    n_recorded++;
}

void
gh_send_ui_refresh(GhWindow *window)
{
  g_assert_true(GH_IS_WINDOW(window));
  refreshes++;
}

const GhSendUiDelegate *
group_send_stub_delegate(gpointer *out_data)
{
  if (out_data)
    *out_data = has_delegate ? recorded_data[0] : NULL;
  return has_delegate ? &recorded[0] : NULL;
}

const GhSendUiDelegate *
group_send_stub_delegate_for(GhConversation *conversation, gpointer *out_data)
{
  for (guint i = 0; i < n_recorded; i++)
    if (recorded[i].handles(conversation, recorded_data[i])) {
      if (out_data)
        *out_data = recorded_data[i];
      return &recorded[i];
    }
  if (out_data)
    *out_data = NULL;
  return NULL;
}

guint
group_send_stub_refreshes(void)
{
  return refreshes;
}

void
group_send_stub_reset(void)
{
  has_delegate = FALSE;
  n_recorded = 0;
  refreshes = 0;
}
