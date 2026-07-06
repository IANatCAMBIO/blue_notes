/* ===========================================================================
 * settings_window.c — the application settings window (implementation)
 *
 * See settings_window.h for the overview.  Every control writes through
 * to the OnApp setters, which persist to the blue_notes.ini config file
 * and live-update all open windows, so there is no OK/Apply button.
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
    on_app_config_set("code_copy_button",
                      app->code_copy_buttons ? "1" : "0");
    on_editor_rebuild_code_buttons_all(app);
}

/* on_sidebar_counts_toggled() — show/hide the note counts next to
 * folders and tags in the library sidebar, live.                            */
static void
on_sidebar_counts_toggled(GtkToggleButton *check, gpointer user_data)
{
    OnApp *app = user_data;          /* application context                 */
    app->sidebar_counts = gtk_toggle_button_get_active(check);
    on_app_config_set("sidebar_counts",
                      app->sidebar_counts ? "1" : "0");
    if (app->notify_notes_changed != NULL)
        app->notify_notes_changed(app);  /* rebuild the sidebar labels      */
}

#ifdef HAVE_GTKOSX
/* on_native_menubar_toggled() — move the library menu into (or out of)
 * the native macOS menu bar, live.                                          */
static void
on_native_menubar_toggled(GtkToggleButton *check, gpointer user_data)
{
    OnApp *app = user_data;          /* application context                 */
    gboolean native = gtk_toggle_button_get_active(check);
    on_app_config_set("native_menubar", native ? "1" : "0");
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

static void on_db_custom_toggled(GtkToggleButton *check,
                                 gpointer user_data);

/* db_switch_report() — run the switch (it reports its own errors and
 * asks about existing databases) and re-sync this section's widgets with
 * whatever actually happened — a cancelled or failed switch leaves the
 * old location active.                                                      */
static void
db_switch_report(DbSection *s, const gchar *new_dir)
{
    on_app_switch_database(s->app, new_dir);
    g_signal_handlers_block_by_func(s->check, on_db_custom_toggled, s);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->check),
                                 s->app->db_dir != NULL);
    g_signal_handlers_unblock_by_func(s->check, on_db_custom_toggled, s);
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

/* on_viewer_entry_changed() — persist the image-viewer program path as
 * the user types; an empty entry clears the setting so "Open" on an
 * image falls back to the system default viewer.                            */
static void
on_viewer_entry_changed(GtkEditable *editable, gpointer user_data)
{
    (void)user_data;
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(editable));
    if (text != NULL && *text != '\0')
        on_app_config_set("image_viewer", text);
    else
        on_app_config_set("image_viewer", NULL);
}

/* on_viewer_browse_clicked() — pick the viewer program with a file
 * chooser; the entry's "changed" handler does the persisting.               */
static void
on_viewer_browse_clicked(GtkButton *btn, gpointer user_data)
{
    GtkWidget *entry = user_data;    /* the viewer-path entry               */
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Choose Image Viewer",
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(btn))),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT,
        NULL);
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        gchar *file = gtk_file_chooser_get_filename(
            GTK_FILE_CHOOSER(chooser));
        gtk_entry_set_text(GTK_ENTRY(entry), file);
        g_free(file);
    }
    gtk_widget_destroy(chooser);
}

/* on_line_numbers_toggled() — show/hide code-block line numbers in every
 * open editor, live.                                                        */
static void
on_line_numbers_toggled(GtkToggleButton *check, gpointer user_data)
{
    OnApp *app = user_data;          /* application context                 */
    app->code_line_numbers = gtk_toggle_button_get_active(check);
    on_app_config_set("code_line_numbers",
                      app->code_line_numbers ? "1" : "0");
    on_editor_apply_line_numbers_all(app);
}

/* on_first_line_h1_toggled() — auto-style the first line of new notes
 * as Heading 1 (affects notes created from now on).                         */
static void
on_first_line_h1_toggled(GtkToggleButton *check, gpointer user_data)
{
    OnApp *app = user_data;          /* application context                 */
    app->first_line_h1 = gtk_toggle_button_get_active(check);
    on_app_config_set("first_line_h1",
                      app->first_line_h1 ? "1" : "0");
}

/* on_compact_toolbar_toggled() — collapse/expand the editor toolbar's
 * paragraph-style and list buttons into "Styles"/"Lists" menus, live.       */
static void
on_compact_toolbar_toggled(GtkToggleButton *check, gpointer user_data)
{
    OnApp *app = user_data;          /* application context                 */
    app->compact_editor_toolbar = gtk_toggle_button_get_active(check);
    on_app_config_set("compact_editor_toolbar",
                      app->compact_editor_toolbar ? "1" : "0");
    on_editor_rebuild_toolbars_all(app);
}

/* on_db_integrity_check_toggled() — enable/disable the startup hash check.  */
static void
on_db_integrity_check_toggled(GtkToggleButton *check, gpointer user_data)
{
    OnApp *app = user_data;
    app->db_integrity_check = gtk_toggle_button_get_active(check);
    on_app_config_set("db_integrity_check",
                      app->db_integrity_check ? "1" : "0");
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
    gtk_window_set_title(GTK_WINDOW(window), "Blue Notes - Settings");
    gtk_window_set_default_size(GTK_WINDOW(window), 420, -1);
    gtk_window_set_transient_for(GTK_WINDOW(window),
                                 GTK_WINDOW(app->library_window));
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 14);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    /* --- appearance ----------------------------------------------------------*/
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Appearance"),
                       FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_start(grid, 12);

    GtkWidget *lib_label = gtk_label_new("Library toolbar buttons:");
    gtk_label_set_xalign(GTK_LABEL(lib_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), lib_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
                    style_combo_new(app, ON_TOOLBAR_LIBRARY), 1, 0, 1, 1);

    GtkWidget *ed_label = gtk_label_new("Editor toolbar buttons:");
    gtk_label_set_xalign(GTK_LABEL(ed_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), ed_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
                    style_combo_new(app, ON_TOOLBAR_EDITOR), 1, 1, 1, 1);

    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    GtkWidget *counts_check = gtk_check_button_new_with_label(
        "Show note counts next to folders and tags");
    gtk_widget_set_margin_start(counts_check, 12);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(counts_check),
                                 app->sidebar_counts);
    g_signal_connect(counts_check, "toggled",
                     G_CALLBACK(on_sidebar_counts_toggled), app);
    gtk_box_pack_start(GTK_BOX(vbox), counts_check, FALSE, FALSE, 0);

#ifdef __APPLE__
    /* Native macOS menu bar belongs with the other appearance choices.     */
    GtkWidget *mac_check = gtk_check_button_new_with_label(
        "Use the native macOS menu bar (hide the in-window menu)");
    gtk_widget_set_margin_start(mac_check, 12);
#ifdef HAVE_GTKOSX
    {
        gchar *native = on_app_config_get("native_menubar");
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

    /* The bundled symbolic tree arrows are still SVG: mention the loader
     * when it is missing.  Everything still works without it — those
     * icons just fall back to the theme defaults.                          */
    if (!svg_loader_available()) {
        GtkWidget *warn = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(warn),
            "<small><i>Some icons (tree arrows) render best "
            "with the librsvg loader:\nsudo port install librsvg "
            "(then restart Blue Notes)</i></small>");
        gtk_label_set_xalign(GTK_LABEL(warn), 0.0);
        gtk_widget_set_margin_start(warn, 12);
        gtk_box_pack_start(GTK_BOX(vbox), warn, FALSE, FALSE, 2);
    }

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

    GtkWidget *lines_check = gtk_check_button_new_with_label(
        "Show line numbers in code blocks");
    gtk_widget_set_margin_start(lines_check, 12);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lines_check),
                                 app->code_line_numbers);
    g_signal_connect(lines_check, "toggled",
                     G_CALLBACK(on_line_numbers_toggled), app);
    gtk_box_pack_start(GTK_BOX(vbox), lines_check, FALSE, FALSE, 0);

    GtkWidget *h1_check = gtk_check_button_new_with_label(
        "Format the first line of a new note as Heading 1");
    gtk_widget_set_margin_start(h1_check, 12);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(h1_check),
                                 app->first_line_h1);
    g_signal_connect(h1_check, "toggled",
                     G_CALLBACK(on_first_line_h1_toggled), app);
    gtk_box_pack_start(GTK_BOX(vbox), h1_check, FALSE, FALSE, 0);

    GtkWidget *compact_check = gtk_check_button_new_with_label(
        "Compact toolbar (group paragraph styles and lists into menus)");
    gtk_widget_set_margin_start(compact_check, 12);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(compact_check),
                                 app->compact_editor_toolbar);
    g_signal_connect(compact_check, "toggled",
                     G_CALLBACK(on_compact_toolbar_toggled), app);
    gtk_box_pack_start(GTK_BOX(vbox), compact_check, FALSE, FALSE, 0);

    /* Program used by an image's "Open" action; empty = system default.   */
    GtkWidget *viewer_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(viewer_row, 12);
    GtkWidget *viewer_label = gtk_label_new("Image viewer:");
    gtk_box_pack_start(GTK_BOX(viewer_row), viewer_label, FALSE, FALSE, 0);

    GtkWidget *viewer_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(viewer_entry),
                                   "System default");
    {
        gchar *viewer = on_app_config_get("image_viewer");
        if (viewer != NULL) {
            gtk_entry_set_text(GTK_ENTRY(viewer_entry), viewer);
            g_free(viewer);
        }
    }
    g_signal_connect(viewer_entry, "changed",
                     G_CALLBACK(on_viewer_entry_changed), app);
    gtk_box_pack_start(GTK_BOX(viewer_row), viewer_entry, TRUE, TRUE, 0);

    GtkWidget *viewer_btn = gtk_button_new_with_label(
        "Browse\xe2\x80\xa6");
    g_signal_connect(viewer_btn, "clicked",
                     G_CALLBACK(on_viewer_browse_clicked), viewer_entry);
    gtk_box_pack_start(GTK_BOX(viewer_row), viewer_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), viewer_row, FALSE, FALSE, 0);

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

    GtkWidget *hash_check = gtk_check_button_new_with_label(
        "Check database integrity on startup (detect external changes)");
    gtk_widget_set_margin_start(hash_check, 12);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hash_check),
                                 app->db_integrity_check);
    g_signal_connect(hash_check, "toggled",
                     G_CALLBACK(on_db_integrity_check_toggled), app);
    gtk_box_pack_start(GTK_BOX(vbox), hash_check, FALSE, FALSE, 0);

    /* --- close button ---------------------------------------------------------*/
    GtkWidget *close_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    g_signal_connect_swapped(close_btn, "clicked",
                             G_CALLBACK(gtk_widget_destroy), window);
    gtk_box_pack_end(GTK_BOX(close_row), close_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), close_row, FALSE, FALSE, 4);

    gtk_widget_show_all(window);
}
