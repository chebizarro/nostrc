#include "gh-attachment-transfer.h"

struct _GhAttachmentTransfer {
  GObject parent_instance;
  gchar *rumor_id;
  GhNip17File *file;
  GhAttachmentState state;
  gchar *error;
  gboolean can_retry;
  GBytes *plaintext;
  gboolean previewable;
  gboolean from_cache;
  GObject *preview;
};

enum {
  PROP_0,
  PROP_STATE,
  PROP_ERROR,
  PROP_CAN_RETRY,
  PROP_PREVIEWABLE,
  PROP_PREVIEW,
  PROP_FROM_CACHE,
  N_PROPS
};
static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE(GhAttachmentTransfer, gh_attachment_transfer, G_TYPE_OBJECT)

GType
gh_attachment_state_get_type(void)
{
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GH_ATTACHMENT_STATE_IDLE, "GH_ATTACHMENT_STATE_IDLE", "idle" },
      { GH_ATTACHMENT_STATE_DOWNLOADING, "GH_ATTACHMENT_STATE_DOWNLOADING", "downloading" },
      { GH_ATTACHMENT_STATE_READY, "GH_ATTACHMENT_STATE_READY", "ready" },
      { GH_ATTACHMENT_STATE_FAILED, "GH_ATTACHMENT_STATE_FAILED", "failed" },
      { 0, NULL, NULL },
    };
    g_once_init_leave(&type, g_enum_register_static(g_intern_static_string("GhAttachmentState"),
                                                    values));
  }
  return type;
}

/* Every change of state notifies what it changed, together. */
static void
set_all(GhAttachmentTransfer *self, GhAttachmentState state, const gchar *error,
        gboolean can_retry, GBytes *plaintext, gboolean previewable, gboolean from_cache)
{
  g_object_freeze_notify(G_OBJECT(self));
  if (self->state != state) {
    self->state = state;
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATE]);
  }
  if (g_strcmp0(self->error, error) != 0) {
    g_free(self->error);
    self->error = g_strdup(error);
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_ERROR]);
  }
  if (self->can_retry != can_retry) {
    self->can_retry = can_retry;
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CAN_RETRY]);
  }
  if (self->plaintext != plaintext) {
    g_clear_pointer(&self->plaintext, g_bytes_unref);
    self->plaintext = plaintext ? g_bytes_ref(plaintext) : NULL;
  }
  if (self->previewable != previewable) {
    self->previewable = previewable;
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PREVIEWABLE]);
  }
  if (self->from_cache != from_cache) {
    self->from_cache = from_cache;
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_FROM_CACHE]);
  }
  if (!previewable && self->preview) {
    g_clear_object(&self->preview);
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PREVIEW]);
  }
  g_object_thaw_notify(G_OBJECT(self));
}

GhAttachmentTransfer *
gh_attachment_transfer_new(const gchar *rumor_id, const GhNip17File *file)
{
  g_return_val_if_fail(rumor_id != NULL, NULL);
  g_return_val_if_fail(file != NULL, NULL);
  GhAttachmentTransfer *self = g_object_new(GH_TYPE_ATTACHMENT_TRANSFER, NULL);
  self->rumor_id = g_strdup(rumor_id);
  self->file = gh_nip17_file_copy(file);
  return self;
}

const gchar *
gh_attachment_transfer_get_rumor_id(GhAttachmentTransfer *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_TRANSFER(self), NULL);
  return self->rumor_id;
}

const GhNip17File *
gh_attachment_transfer_get_file(GhAttachmentTransfer *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_TRANSFER(self), NULL);
  return self->file;
}

GhAttachmentState
gh_attachment_transfer_get_state(GhAttachmentTransfer *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_TRANSFER(self), GH_ATTACHMENT_STATE_IDLE);
  return self->state;
}

const gchar *
gh_attachment_transfer_get_error(GhAttachmentTransfer *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_TRANSFER(self), NULL);
  return self->error;
}

gboolean
gh_attachment_transfer_get_can_retry(GhAttachmentTransfer *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_TRANSFER(self), FALSE);
  return self->can_retry;
}

GBytes *
gh_attachment_transfer_get_plaintext(GhAttachmentTransfer *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_TRANSFER(self), NULL);
  return self->plaintext;
}

gboolean
gh_attachment_transfer_get_previewable(GhAttachmentTransfer *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_TRANSFER(self), FALSE);
  return self->previewable;
}

gboolean
gh_attachment_transfer_get_from_cache(GhAttachmentTransfer *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_TRANSFER(self), FALSE);
  return self->from_cache;
}

void
gh_attachment_transfer_set_preview(GhAttachmentTransfer *self, GObject *preview)
{
  g_return_if_fail(GH_IS_ATTACHMENT_TRANSFER(self));
  g_return_if_fail(!preview || G_IS_OBJECT(preview));
  if (preview && !self->previewable)
    return;
  if (g_set_object(&self->preview, preview))
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PREVIEW]);
}

GObject *
gh_attachment_transfer_get_preview(GhAttachmentTransfer *self)
{
  g_return_val_if_fail(GH_IS_ATTACHMENT_TRANSFER(self), NULL);
  return self->preview;
}

void
gh_attachment_transfer_start(GhAttachmentTransfer *self)
{
  g_return_if_fail(GH_IS_ATTACHMENT_TRANSFER(self));
  set_all(self, GH_ATTACHMENT_STATE_DOWNLOADING, NULL, FALSE, NULL, FALSE, FALSE);
}

void
gh_attachment_transfer_succeed(GhAttachmentTransfer *self, GBytes *plaintext,
                               gboolean previewable, gboolean from_cache)
{
  g_return_if_fail(GH_IS_ATTACHMENT_TRANSFER(self));
  g_return_if_fail(plaintext != NULL);
  set_all(self, GH_ATTACHMENT_STATE_READY, NULL, FALSE, plaintext, !!previewable, !!from_cache);
}

void
gh_attachment_transfer_fail(GhAttachmentTransfer *self, const gchar *error, gboolean can_retry)
{
  g_return_if_fail(GH_IS_ATTACHMENT_TRANSFER(self));
  g_return_if_fail(error != NULL);
  set_all(self, GH_ATTACHMENT_STATE_FAILED, error, !!can_retry, NULL, FALSE, FALSE);
}

void
gh_attachment_transfer_reset(GhAttachmentTransfer *self)
{
  g_return_if_fail(GH_IS_ATTACHMENT_TRANSFER(self));
  set_all(self, GH_ATTACHMENT_STATE_IDLE, NULL, FALSE, NULL, FALSE, FALSE);
}

static void
gh_attachment_transfer_get_property(GObject *object, guint prop_id, GValue *value,
                                    GParamSpec *pspec)
{
  GhAttachmentTransfer *self = GH_ATTACHMENT_TRANSFER(object);
  switch (prop_id) {
  case PROP_STATE:       g_value_set_enum(value, self->state); break;
  case PROP_ERROR:       g_value_set_string(value, self->error); break;
  case PROP_CAN_RETRY:   g_value_set_boolean(value, self->can_retry); break;
  case PROP_PREVIEWABLE: g_value_set_boolean(value, self->previewable); break;
  case PROP_PREVIEW:     g_value_set_object(value, self->preview); break;
  case PROP_FROM_CACHE:  g_value_set_boolean(value, self->from_cache); break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_attachment_transfer_set_property(GObject *object, guint prop_id, const GValue *value,
                                    GParamSpec *pspec)
{
  GhAttachmentTransfer *self = GH_ATTACHMENT_TRANSFER(object);
  switch (prop_id) {
  case PROP_PREVIEW:
    gh_attachment_transfer_set_preview(self, g_value_get_object(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gh_attachment_transfer_finalize(GObject *object)
{
  GhAttachmentTransfer *self = GH_ATTACHMENT_TRANSFER(object);
  g_free(self->rumor_id);
  gh_nip17_file_free(self->file);
  g_free(self->error);
  g_clear_pointer(&self->plaintext, g_bytes_unref);
  g_clear_object(&self->preview);
  G_OBJECT_CLASS(gh_attachment_transfer_parent_class)->finalize(object);
}

static void
gh_attachment_transfer_class_init(GhAttachmentTransferClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->get_property = gh_attachment_transfer_get_property;
  object_class->set_property = gh_attachment_transfer_set_property;
  object_class->finalize = gh_attachment_transfer_finalize;

  const GParamFlags ro = G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;
  props[PROP_STATE] = g_param_spec_enum("state", NULL, NULL, GH_TYPE_ATTACHMENT_STATE,
                                        GH_ATTACHMENT_STATE_IDLE, ro);
  props[PROP_ERROR] = g_param_spec_string("error", NULL, NULL, NULL, ro);
  props[PROP_CAN_RETRY] = g_param_spec_boolean("can-retry", NULL, NULL, FALSE, ro);
  props[PROP_PREVIEWABLE] = g_param_spec_boolean("previewable", NULL, NULL, FALSE, ro);
  props[PROP_PREVIEW] = g_param_spec_object("preview", NULL, NULL, G_TYPE_OBJECT,
                                            G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                                              G_PARAM_STATIC_STRINGS);
  props[PROP_FROM_CACHE] = g_param_spec_boolean("from-cache", NULL, NULL, FALSE, ro);
  g_object_class_install_properties(object_class, N_PROPS, props);
}

static void
gh_attachment_transfer_init(GhAttachmentTransfer *self)
{
  self->state = GH_ATTACHMENT_STATE_IDLE;
}
