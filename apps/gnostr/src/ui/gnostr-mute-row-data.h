#ifndef GNOSTR_MUTE_ROW_DATA_H
#define GNOSTR_MUTE_ROW_DATA_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  GNOSTR_MUTE_ROW_USER = 0,
  GNOSTR_MUTE_ROW_WORD = 1,
  GNOSTR_MUTE_ROW_HASHTAG = 2
} GnostrMuteRowType;

typedef struct {
  char *display_value;
  char *canonical_value;
  GnostrMuteRowType type;
} GnostrMuteRowBinding;

GnostrMuteRowBinding *gnostr_mute_row_binding_new(const char *canonical_value,
                                                  GnostrMuteRowType type);
void gnostr_mute_row_binding_free(GnostrMuteRowBinding *binding);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnostrMuteRowBinding, gnostr_mute_row_binding_free)

G_END_DECLS

#endif /* GNOSTR_MUTE_ROW_DATA_H */
