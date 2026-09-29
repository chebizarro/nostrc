#include "group-send-stub.h"

static GhSendUiDelegate recorded;
static gboolean has_delegate;
static gpointer recorded_data;
static guint refreshes;

void
gh_send_ui_set_delegate(GhWindow *window, const GhSendUiDelegate *delegate, gpointer data)
{
  g_assert_true(GH_IS_WINDOW(window));
  has_delegate = delegate != NULL;
  if (delegate)
    recorded = *delegate;
  recorded_data = data;
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
    *out_data = recorded_data;
  return has_delegate ? &recorded : NULL;
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
  recorded_data = NULL;
  refreshes = 0;
}
