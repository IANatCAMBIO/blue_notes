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
