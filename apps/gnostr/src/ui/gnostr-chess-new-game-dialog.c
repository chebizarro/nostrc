/**
 * GnostrChessNewGameDialog - New Chess Game Configuration Dialog
 *
 * Allows users to configure a new chess game:
 * - Select player color (White, Black, or Random)
 * - Choose AI difficulty level (Beginner, Intermediate, Advanced, Expert)
 */

#include "gnostr-chess-new-game-dialog.h"
#include <glib/gi18n.h>
#include <stdlib.h>
#include <time.h>

struct _GnostrChessNewGameDialog {
    AdwDialog parent_instance;

    /* Player color selection */
    GtkWidget *radio_white;
    GtkWidget *radio_black;
    GtkWidget *radio_random;

    /* AI difficulty dropdown */
    GtkWidget *combo_difficulty;

    /* Action buttons */
    GtkWidget *btn_cancel;
    GtkWidget *btn_start;

    /* Selected values */
    GnostrChessNewGameColor selected_color;
    gint selected_ai_depth;
};

G_DEFINE_TYPE(GnostrChessNewGameDialog, gnostr_chess_new_game_dialog, ADW_TYPE_DIALOG)

enum {
    SIGNAL_GAME_STARTED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* AI difficulty options */
typedef struct {
    const char *label;
    gint depth;
} DifficultyOption;

static const DifficultyOption DIFFICULTY_OPTIONS[] = {
    { "AI - Beginner", GNOSTR_CHESS_AI_BEGINNER },
    { "AI - Intermediate", GNOSTR_CHESS_AI_INTERMEDIATE },
    { "AI - Advanced", GNOSTR_CHESS_AI_ADVANCED },
    { "AI - Expert", GNOSTR_CHESS_AI_EXPERT },
};
static const gsize NUM_DIFFICULTY_OPTIONS = G_N_ELEMENTS(DIFFICULTY_OPTIONS);

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void on_cancel_clicked(GtkButton *btn, gpointer user_data);
static void on_start_clicked(GtkButton *btn, gpointer user_data);
static void on_color_toggled(GtkCheckButton *btn, gpointer user_data);
static void on_difficulty_changed(GObject *combo, GParamSpec *pspec, gpointer user_data);

/* ============================================================================
 * GObject Implementation
 * ============================================================================ */

static void
gnostr_chess_new_game_dialog_dispose(GObject *object)
{
    G_OBJECT_CLASS(gnostr_chess_new_game_dialog_parent_class)->dispose(object);
}

static void
gnostr_chess_new_game_dialog_class_init(GnostrChessNewGameDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = gnostr_chess_new_game_dialog_dispose;

    /**
     * GnostrChessNewGameDialog::game-started:
     * @self: The dialog
     * @player_color: Selected player color (GnostrChessNewGameColor)
     * @ai_depth: Selected AI search depth
     *
     * Emitted when the user clicks Start Game.
     */
    signals[SIGNAL_GAME_STARTED] = g_signal_new(
        "game-started",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2,
        G_TYPE_INT, G_TYPE_INT);
}

static void
gnostr_chess_new_game_dialog_init(GnostrChessNewGameDialog *self)
{
    /* Default values */
    self->selected_color = GNOSTR_CHESS_NEW_GAME_COLOR_WHITE;
    self->selected_ai_depth = GNOSTR_CHESS_AI_INTERMEDIATE;

    /* Seed random for color selection */
    srand((unsigned int)time(NULL));

    /* Set dialog properties */
    adw_dialog_set_title(ADW_DIALOG(self), _("New Chess Game"));
    adw_dialog_set_content_width(ADW_DIALOG(self), 360);
    adw_dialog_set_content_height(ADW_DIALOG(self), 340);

    /* Create main content box */
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Header bar with close button */
    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), TRUE);
    gtk_box_append(GTK_BOX(content), header);

    /* Main preferences page */
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(page, 12);
    gtk_widget_set_margin_bottom(page, 12);
    gtk_widget_set_margin_start(page, 12);
    gtk_widget_set_margin_end(page, 12);

    /* Play as section */
    GtkWidget *color_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(color_group), _("Play as"));

    /* Color radio buttons in a horizontal box */
    GtkWidget *color_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_widget_set_halign(color_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(color_box, 8);
    gtk_widget_set_margin_bottom(color_box, 8);

    self->radio_white = gtk_check_button_new_with_label(_("White"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(self->radio_white), TRUE);
    g_signal_connect(self->radio_white, "toggled", G_CALLBACK(on_color_toggled), self);
    gtk_box_append(GTK_BOX(color_box), self->radio_white);

    self->radio_black = gtk_check_button_new_with_label(_("Black"));
    gtk_check_button_set_group(GTK_CHECK_BUTTON(self->radio_black),
                               GTK_CHECK_BUTTON(self->radio_white));
    g_signal_connect(self->radio_black, "toggled", G_CALLBACK(on_color_toggled), self);
    gtk_box_append(GTK_BOX(color_box), self->radio_black);

    self->radio_random = gtk_check_button_new_with_label(_("Random"));
    gtk_check_button_set_group(GTK_CHECK_BUTTON(self->radio_random),
                               GTK_CHECK_BUTTON(self->radio_white));
    g_signal_connect(self->radio_random, "toggled", G_CALLBACK(on_color_toggled), self);
    gtk_box_append(GTK_BOX(color_box), self->radio_random);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(color_group), color_box);
    gtk_box_append(GTK_BOX(page), color_group);

    /* Opponent section */
    GtkWidget *opponent_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(opponent_group), _("Opponent"));

    /* Difficulty dropdown row */
    GtkWidget *difficulty_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(difficulty_row), _("Difficulty"));

    GtkStringList *difficulty_model = gtk_string_list_new(NULL);
    for (gsize i = 0; i < NUM_DIFFICULTY_OPTIONS; i++) {
        gtk_string_list_append(difficulty_model, DIFFICULTY_OPTIONS[i].label);
    }
    adw_combo_row_set_model(ADW_COMBO_ROW(difficulty_row), G_LIST_MODEL(difficulty_model));
    adw_combo_row_set_selected(ADW_COMBO_ROW(difficulty_row), 1);  /* Intermediate default */
    g_object_unref(difficulty_model);

    self->combo_difficulty = difficulty_row;
    g_signal_connect(difficulty_row, "notify::selected", G_CALLBACK(on_difficulty_changed), self);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(opponent_group), difficulty_row);
    gtk_box_append(GTK_BOX(page), opponent_group);

    /* Spacer */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(page), spacer);

    /* Action buttons */
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(button_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(button_box, 12);
    gtk_widget_set_margin_bottom(button_box, 12);

    self->btn_cancel = gtk_button_new_with_label(_("Cancel"));
    g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel_clicked), self);
    gtk_box_append(GTK_BOX(button_box), self->btn_cancel);

    self->btn_start = gtk_button_new_with_label(_("Start Game"));
    gtk_widget_add_css_class(self->btn_start, "suggested-action");
    g_signal_connect(self->btn_start, "clicked", G_CALLBACK(on_start_clicked), self);
    gtk_box_append(GTK_BOX(button_box), self->btn_start);

    gtk_box_append(GTK_BOX(page), button_box);

    gtk_box_append(GTK_BOX(content), page);
    adw_dialog_set_child(ADW_DIALOG(self), content);
}

/* ============================================================================
 * Signal Handlers
 * ============================================================================ */

static void
on_cancel_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    GnostrChessNewGameDialog *self = GNOSTR_CHESS_NEW_GAME_DIALOG(user_data);
    adw_dialog_close(ADW_DIALOG(self));
}

static void
on_start_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    GnostrChessNewGameDialog *self = GNOSTR_CHESS_NEW_GAME_DIALOG(user_data);

    /* Resolve random color */
    gint final_color = self->selected_color;
    if (final_color == GNOSTR_CHESS_NEW_GAME_COLOR_RANDOM) {
        final_color = (rand() % 2 == 0) ?
            GNOSTR_CHESS_NEW_GAME_COLOR_WHITE : GNOSTR_CHESS_NEW_GAME_COLOR_BLACK;
    }

    /* Emit game-started signal */
    g_signal_emit(self, signals[SIGNAL_GAME_STARTED], 0,
                  final_color, self->selected_ai_depth);

    adw_dialog_close(ADW_DIALOG(self));
}

static void
on_color_toggled(GtkCheckButton *btn, gpointer user_data)
{
    GnostrChessNewGameDialog *self = GNOSTR_CHESS_NEW_GAME_DIALOG(user_data);

    if (!gtk_check_button_get_active(btn))
        return;

    if (GTK_WIDGET(btn) == self->radio_white) {
        self->selected_color = GNOSTR_CHESS_NEW_GAME_COLOR_WHITE;
    } else if (GTK_WIDGET(btn) == self->radio_black) {
        self->selected_color = GNOSTR_CHESS_NEW_GAME_COLOR_BLACK;
    } else if (GTK_WIDGET(btn) == self->radio_random) {
        self->selected_color = GNOSTR_CHESS_NEW_GAME_COLOR_RANDOM;
    }
}

static void
on_difficulty_changed(GObject *combo, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    GnostrChessNewGameDialog *self = GNOSTR_CHESS_NEW_GAME_DIALOG(user_data);

    guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(combo));
    if (selected < NUM_DIFFICULTY_OPTIONS) {
        self->selected_ai_depth = DIFFICULTY_OPTIONS[selected].depth;
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GnostrChessNewGameDialog *
gnostr_chess_new_game_dialog_new(void)
{
    return g_object_new(GNOSTR_TYPE_CHESS_NEW_GAME_DIALOG, NULL);
}

void
gnostr_chess_new_game_dialog_present(GnostrChessNewGameDialog *self, GtkWidget *parent)
{
    g_return_if_fail(GNOSTR_IS_CHESS_NEW_GAME_DIALOG(self));

    /* Reset to defaults */
    gtk_check_button_set_active(GTK_CHECK_BUTTON(self->radio_white), TRUE);
    self->selected_color = GNOSTR_CHESS_NEW_GAME_COLOR_WHITE;

    adw_combo_row_set_selected(ADW_COMBO_ROW(self->combo_difficulty), 1);
    self->selected_ai_depth = GNOSTR_CHESS_AI_INTERMEDIATE;

    adw_dialog_present(ADW_DIALOG(self), parent);
}

GnostrChessNewGameColor
gnostr_chess_new_game_dialog_get_player_color(GnostrChessNewGameDialog *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_NEW_GAME_DIALOG(self),
                         GNOSTR_CHESS_NEW_GAME_COLOR_WHITE);
    return self->selected_color;
}

gint
gnostr_chess_new_game_dialog_get_ai_depth(GnostrChessNewGameDialog *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_NEW_GAME_DIALOG(self),
                         GNOSTR_CHESS_AI_INTERMEDIATE);
    return self->selected_ai_depth;
}

const gchar *
gnostr_chess_new_game_dialog_get_ai_difficulty_label(gint depth)
{
    for (gsize i = 0; i < NUM_DIFFICULTY_OPTIONS; i++) {
        if (DIFFICULTY_OPTIONS[i].depth == depth) {
            return DIFFICULTY_OPTIONS[i].label;
        }
    }
    return "AI - Intermediate";
}
