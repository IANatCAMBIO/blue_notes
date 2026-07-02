/* ===========================================================================
 * settings_window.c — the application settings window (implementation)
 *
 * See settings_window.h for the overview.  Every control writes through
 * to the OnApp setters, which persist to SQLite and live-update all open
 * windows, so there is no OK/Apply button.
 * =========================================================================== */

#include "settings_window.h"
#include "editor_window.h"
#include "library_window.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * svg_loader_available() — TRUE if gdk-pixbuf can decode SVG files (i.e.
 * the librsvg loader is installed).  Used to warn about the elementary
 * theme, which is SVG-only.
 * ------------------------------------------------------------------------- */
static gboolean
svg_loader_available(void)
{
    GSList *formats = gdk_pixbuf_get_formats();
    gboolean found = FALSE;          /* did we see an svg decoder?          */
    for (GSList *l = formats; l != NULL && !found; l = l->next) {
        gchar *name = gdk_pixbuf_format_get_name(l->data);
        found = g_ascii_strcasecmp(name, "svg") == 0;
        g_free(name);
    }
    g_slist_free(formats);
    return found;
}

/* ---------------------------------------------------------------------------
 * on_style_combo_changed() — a toolbar-style combo changed: apply it to
 * the toolbar family stored on the combo as "on-kind".
 * Combo index order matches: 0=text, 1=icons, 2=icons above text.
 * ------------------------------------------------------------------------- */
static void
on_style_combo_changed(GtkComboBox *combo, gpointer user_data)
{
    OnApp *app = user_data;          /* application context                 */
    static const GtkToolbarStyle STYLES[] = {
        GTK_TOOLBAR_TEXT, GTK_TOOLBAR_ICONS, GTK_TOOLBAR_BOTH,
    };
    gint active = gtk_combo_box_get_active(combo);
    if (active < 0 || active > 2)
        return;
    on_app_set_toolbar_style(
        app,
        (OnToolbarKind)GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(combo), "on-kind")),
        STYLES[active]);
}

/* ---------------------------------------------------------------------------
 * style_combo_new() — build one toolbar-style combo, pre-set to the
 * current style of toolbar family `kind`.
 * ------------------------------------------------------------------------- */
static GtkWidget *
style_combo_new(OnApp *app, OnToolbarKind kind)
{
    GtkWidget *combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
                                   "Text only");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
                                   "Icons only");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
                                   "Icons above text");

    gint active =                    /* combo index of the current style    */
        (app->toolbar_style[kind] == GTK_TOOLBAR_TEXT)  ? 0
      : (app->toolbar_style[kind] == GTK_TOOLBAR_ICONS) ? 1
                                                        : 2;
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);

    g_object_set_data(G_OBJECT(combo), "on-kind", GINT_TO_POINTER(kind));
    g_signal_connect(combo, "changed",
                     G_CALLBACK(on_style_combo_changed), app);
    return combo;
}

/* on_code_copy_toggled() — enable/disable the code-block copy buttons in
 * every open editor, live.                                                  */
static void
on_code_copy_toggled(GtkToggleButton *check, gpointer user_data)
{
    OnApp *app = user_data;          /* application context                 */
    app->code_copy_buttons = gtk_toggle_button_get_active(check);
    on_db_setting_set(app->db, "code_copy_button",
                      app->code_copy_buttons ? "1" : "0");
    on_editor_rebuild_code_buttons_all(app);
}

#ifdef HAVE_GTKOSX
/* on_native_menubar_toggled() — move the library menu into (or out of)
 * the native macOS menu bar, live.                                          */
static void
on_native_menubar_toggled(GtkToggleButton *check, gpointer user_data)
{
    OnApp *app = user_data;          /* application context                 */
    gboolean native = gtk_toggle_button_get_active(check);
    on_db_setting_set(app->db, "native_menubar", native ? "1" : "0");
    on_library_apply_native_menubar(app, native);
}
#endif /* HAVE_GTKOSX */

/* ---------------------------------------------------------------------------
 * DbSection — widgets of the "Database" settings block, so its handlers
 * can keep the labels in sync after a location switch.
 *
 * Fields:
 *   app        — application context.
 *   check      — "custom folder" checkbox.
 *   choose_btn — folder-picker button (sensitive only when custom).
 *   path_label — shows the active database file path.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnApp     *app;
    GtkWidget *check;
    GtkWidget *choose_btn;
    GtkWidget *path_label;
} DbSection;

/* db_section_refresh() — sync the database widgets with reality.            */
static void
db_section_refresh(DbSection *s)
{
    gchar *markup = g_markup_printf_escaped(
        "<small>Current database: %s\n"
        "<i>Do not open the same database from two machines at the same "
        "time.</i></small>", s->app->db->path);
    gtk_label_set_markup(GTK_LABEL(s->path_label), markup);
    g_free(markup);
    gtk_widget_set_sensitive(s->choose_btn, s->app->db_dir != NULL);
}

/* db_switch_report() — run the switch and report failures in a dialog.      */
static void
db_switch_report(DbSection *s, const gchar *new_dir)
{
    if (!on_app_switch_database(s->app, new_dir)) {
        GtkWidget *msg = gtk_message_dialog_new(
            GTK_WINDOW(gtk_widget_get_toplevel(s->check)),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Could not open a database at that location.\n"
            "The previous database is still in use.");
        gtk_dialog_run(GTK_DIALOG(msg));
        gtk_widget_destroy(msg);
    }
    db_section_refresh(s);
}

/* db_pick_folder() — folder chooser; returns a new path or NULL.            */
static gchar *
db_pick_folder(DbSection *s)
{
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Choose Database Folder",
        GTK_WINDOW(gtk_widget_get_toplevel(s->check)),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT,
        NULL);
    if (s->app->db_dir != NULL)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser),
                                            s->app->db_dir);
    gchar *dir = NULL;               /* the picked folder, if any           */
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
        dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    return dir;
}

/* on_db_custom_toggled() — enable/disable the custom database location.     */
static void
on_db_custom_toggled(GtkToggleButton *check, gpointer user_data)
{
    DbSection *s = user_data;        /* the database section                */
    gboolean want_custom = gtk_toggle_button_get_active(check);

    if (want_custom && s->app->db_dir == NULL) {
        /* Enabling needs a folder right away.                              */
        gchar *dir = db_pick_folder(s);
        if (dir == NULL) {
            /* Cancelled: silently revert the checkbox.                     */
            g_signal_handlers_block_by_func(check,
                                            on_db_custom_toggled, s);
            gtk_toggle_button_set_active(check, FALSE);
            g_signal_handlers_unblock_by_func(check,
                                              on_db_custom_toggled, s);
            return;
        }
        db_switch_report(s, dir);
        g_free(dir);
    } else if (!want_custom && s->app->db_dir != NULL) {
        db_switch_report(s, NULL);   /* back to the default location        */
    }
}

/* on_db_choose_clicked() — re-pick the custom folder.                       */
static void
on_db_choose_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    DbSection *s = user_data;        /* the database section                */
    gchar *dir = db_pick_folder(s);
    if (dir != NULL) {
        db_switch_report(s, dir);
        g_free(dir);
    }
}

/* ---------------------------------------------------------------------------
 * section_label() — bold section header for the settings layout.
 * ------------------------------------------------------------------------- */
static GtkWidget *
section_label(const gchar *text)
{
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    return label;
}

void
on_settings_window_open(OnApp *app)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Orange Notes - Settings");
    gtk_window_set_default_size(GTK_WINDOW(window), 420, -1);
    gtk_window_set_transient_for(GTK_WINDOW(window),
                                 GTK_WINDOW(app->library_window));
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 14);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    /* --- toolbar button styles ---------------------------------------------*/
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Toolbar Buttons"),
                       FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_start(grid, 12);

    GtkWidget *lib_label = gtk_label_new("Library windows:");
    gtk_label_set_xalign(GTK_LABEL(lib_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), lib_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
                    style_combo_new(app, ON_TOOLBAR_LIBRARY), 1, 0, 1, 1);

    GtkWidget *ed_label = gtk_label_new("Editor windows:");
    gtk_label_set_xalign(GTK_LABEL(ed_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), ed_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
                    style_combo_new(app, ON_TOOLBAR_EDITOR), 1, 1, 1, 1);

    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 4);

    /* --- editor options ------------------------------------------------------*/
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Editor"),
                       FALSE, FALSE, 0);

    GtkWidget *code_check = gtk_check_button_new_with_label(
        "Show copy button on code blocks");
    gtk_widget_set_margin_start(code_check, 12);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(code_check),
                                 app->code_copy_buttons);
    g_signal_connect(code_check, "toggled",
                     G_CALLBACK(on_code_copy_toggled), app);
    gtk_box_pack_start(GTK_BOX(vbox), code_check, FALSE, FALSE, 0);

#ifdef __APPLE__
    /* --- macOS integration --------------------------------------------------*/
    GtkWidget *mac_check = gtk_check_button_new_with_label(
        "Use the native macOS menu bar (hide the in-window menu)");
    gtk_widget_set_margin_start(mac_check, 12);
#ifdef HAVE_GTKOSX
    {
        gchar *native = on_db_setting_get(app->db, "native_menubar");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mac_check),
                                     g_strcmp0(native, "1") == 0);
        g_free(native);
    }
    g_signal_connect(mac_check, "toggled",
                     G_CALLBACK(on_native_menubar_toggled), app);
#else
    gtk_widget_set_sensitive(mac_check, FALSE);
    gtk_widget_set_tooltip_text(mac_check,
        "Requires the gtk-mac-integration library:\n"
        "sudo port install gtk-osx-application-gtk3, then rebuild "
        "(make clean && make)");
#endif
    gtk_box_pack_start(GTK_BOX(vbox), mac_check, FALSE, FALSE, 0);
#endif /* __APPLE__ */

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 4);

    /* --- database location ---------------------------------------------------*/
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Database"),
                       FALSE, FALSE, 0);

    DbSection *dbs = g_new0(DbSection, 1);
    dbs->app = app;
    /* Freed with the window.                                               */
    g_object_set_data_full(G_OBJECT(window), "on-db-section", dbs, g_free);

    dbs->check = gtk_check_button_new_with_label(
        "Store the database in a custom folder (e.g. a shared drive)");
    gtk_widget_set_margin_start(dbs->check, 12);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dbs->check),
                                 app->db_dir != NULL);
    gtk_box_pack_start(GTK_BOX(vbox), dbs->check, FALSE, FALSE, 0);

    GtkWidget *db_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(db_row, 12);
    dbs->choose_btn = gtk_button_new_with_label(
        "Choose Folder\xe2\x80\xa6");
    gtk_box_pack_start(GTK_BOX(db_row), dbs->choose_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), db_row, FALSE, FALSE, 0);

    dbs->path_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(dbs->path_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(dbs->path_label), TRUE);
    gtk_widget_set_margin_start(dbs->path_label, 12);
    gtk_box_pack_start(GTK_BOX(vbox), dbs->path_label, FALSE, FALSE, 0);

    db_section_refresh(dbs);
    g_signal_connect(dbs->check, "toggled",
                     G_CALLBACK(on_db_custom_toggled), dbs);
    g_signal_connect(dbs->choose_btn, "clicked",
                     G_CALLBACK(on_db_choose_clicked), dbs);

    /* The bundled elementary icons are SVG: warn when they can't render.   */
    if (!svg_loader_available()) {
        GtkWidget *warn = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(warn),
            "<small><i>Toolbar icons are SVG files and need the librsvg "
            "loader to display:\nsudo port install librsvg "
            "(then restart Orange Notes)</i></small>");
        gtk_label_set_xalign(GTK_LABEL(warn), 0.0);
        gtk_widget_set_margin_start(warn, 12);
        gtk_box_pack_start(GTK_BOX(vbox), warn, FALSE, FALSE, 2);
    }

    /* --- close button ---------------------------------------------------------*/
    GtkWidget *close_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    g_signal_connect_swapped(close_btn, "clicked",
                             G_CALLBACK(gtk_widget_destroy), window);
    gtk_box_pack_end(GTK_BOX(close_row), close_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), close_row, FALSE, FALSE, 4);

    gtk_widget_show_all(window);
}
