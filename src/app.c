/* ===========================================================================
 * app.c — shared application helpers (implementation)
 *
 * Icon loading and the app-wide toolbar-style machinery.  Icons live in a
 * plain folder of PNG files next to the executable ("icons/"), named by
 * the usual freedesktop action names (edit-copy.png, folder-new.png, …) so
 * users can swap any of them by replacing the file.
 * =========================================================================== */

#include "app.h"

#include <string.h>

/* Settings-table keys under which the toolbar styles are persisted,
 * indexed by OnToolbarKind.                                                 */
static const gchar *STYLE_SETTING_KEYS[ON_TOOLBAR_N_KINDS] = {
    "toolbar_style_library",
    "toolbar_style_editor",
};

void
on_app_init_icons_dir(OnApp *app, const gchar *argv0)
{
    /* Prefer the directory containing the executable; when launched via a
     * bare name from PATH there is no directory part, so fall back to the
     * current working directory.                                           */
    gchar *exe_dir;                  /* directory containing the binary     */
    if (argv0 != NULL && strchr(argv0, G_DIR_SEPARATOR) != NULL) {
        gchar *abs = g_canonicalize_filename(argv0, NULL);
        exe_dir = g_path_get_dirname(abs);
        g_free(abs);
    } else {
        exe_dir = g_get_current_dir();
    }
    app->icons_dir = g_build_filename(exe_dir, "icons", NULL);
    g_free(exe_dir);
}

GtkWidget *
on_app_icon_image_sized(OnApp *app, const gchar *name, gint size)
{
    static const gchar *EXTS[] = { "svg", "png" };

    /* Rasterize at the display's scale factor so icons stay sharp on
     * HiDPI/Retina screens: `size` is the LOGICAL size, the backing
     * pixels are size × sf, and the cairo surface's device scale maps
     * between the two.                                                     */
    gint sf = 1;                     /* display scale factor                */
    GdkDisplay *display = gdk_display_get_default();
    if (display != NULL) {
        GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
        if (monitor == NULL)
            monitor = gdk_display_get_monitor(display, 0);
        if (monitor != NULL)
            sf = gdk_monitor_get_scale_factor(monitor);
    }

    for (gsize i = 0; i < G_N_ELEMENTS(EXTS); i++) {
        gchar *path = g_strdup_printf("%s%c%s.%s",
                                      app->icons_dir, G_DIR_SEPARATOR,
                                      name, EXTS[i]);
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            /* Verify the file actually decodes (SVGs need the librsvg
             * pixbuf loader) — a broken-image icon is worse than the
             * text fallback the caller provides.                           */
            GdkPixbuf *pix = gdk_pixbuf_new_from_file_at_size(
                path, size * sf, size * sf, NULL);
            if (pix != NULL) {
                cairo_surface_t *surface =
                    gdk_cairo_surface_create_from_pixbuf(pix, sf, NULL);
                GtkWidget *image = gtk_image_new_from_surface(surface);
                cairo_surface_destroy(surface);
                g_object_unref(pix);
                g_free(path);
                return image;
            }
        }
        g_free(path);
    }
    return NULL;
}

GtkWidget *
on_app_icon_image(OnApp *app, const gchar *name)
{
    return on_app_icon_image_sized(app, name, 24);
}

GtkToolItem *
on_app_tool_item_new(OnApp *app, gboolean toggle, const gchar *icon_name,
                     const gchar *fallback_markup, const gchar *label,
                     const gchar *tooltip)
{
    GtkToolItem *item = toggle
        ? GTK_TOOL_ITEM(gtk_toggle_tool_button_new())
        : GTK_TOOL_ITEM(gtk_tool_button_new(NULL, NULL));
    gtk_tool_button_set_label(GTK_TOOL_BUTTON(item), label);

    /* Icon: the local PNG if present, else the fallback markup rendered
     * as a label standing in for the icon.                                 */
    GtkWidget *icon = (icon_name != NULL)
                      ? on_app_icon_image(app, icon_name) : NULL;
    if (icon == NULL) {
        icon = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(icon),
                             fallback_markup != NULL ? fallback_markup
                                                     : label);
    }
    gtk_widget_show(icon);
    gtk_tool_button_set_icon_widget(GTK_TOOL_BUTTON(item), icon);

    gtk_tool_item_set_tooltip_text(item, tooltip);
    /* Show the label even in BOTH_HORIZ-style themes.                      */
    gtk_tool_item_set_is_important(item, TRUE);
    return item;
}

/* on_toolbar_destroyed() — drop a dying toolbar from its registry.  The
 * registry it lives in is stashed on the toolbar as "on-registry".          */
static void
on_toolbar_destroyed(GtkWidget *toolbar, gpointer user_data)
{
    (void)user_data;
    GPtrArray *registry =            /* the kind-specific registry          */
        g_object_get_data(G_OBJECT(toolbar), "on-registry");
    if (registry != NULL)
        g_ptr_array_remove(registry, toolbar);
}

/* on_style_menu_toggled() — a radio item in a toolbar's right-click menu
 * became active: apply that style to the item's toolbar family.             */
static void
on_style_menu_toggled(GtkCheckMenuItem *item, gpointer user_data)
{
    OnApp *app = user_data;          /* the application context             */
    if (!gtk_check_menu_item_get_active(item))
        return;                      /* ignore the deactivating item        */
    on_app_set_toolbar_style(
        app,
        (OnToolbarKind)GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(item), "on-kind")),
        (GtkToolbarStyle)GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(item), "on-style")));
}

/* ---------------------------------------------------------------------------
 * on_toolbar_context_menu() — "popup-context-menu" handler: right-clicking
 * a toolbar offers the text/icons/both radio choices for its family.
 * ------------------------------------------------------------------------- */
static gboolean
on_toolbar_context_menu(GtkToolbar *toolbar, gint x, gint y, gint button,
                        gpointer user_data)
{
    (void)x; (void)y; (void)button;
    OnApp *app = user_data;          /* the application context             */
    OnToolbarKind kind = (OnToolbarKind)GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(toolbar), "on-kind"));

    GtkWidget *menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(toolbar), NULL);
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);

    static const struct { const gchar *label; GtkToolbarStyle style; }
    STYLES[] = {
        { "_Text only",        GTK_TOOLBAR_TEXT  },
        { "_Icons only",       GTK_TOOLBAR_ICONS },
        { "Icons _above Text", GTK_TOOLBAR_BOTH  },
    };
    GSList *group = NULL;            /* the radio group being built         */
    for (gsize i = 0; i < G_N_ELEMENTS(STYLES); i++) {
        GtkWidget *item =
            gtk_radio_menu_item_new_with_mnemonic(group, STYLES[i].label);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_object_set_data(G_OBJECT(item), "on-kind",
                          GINT_TO_POINTER(kind));
        g_object_set_data(G_OBJECT(item), "on-style",
                          GINT_TO_POINTER(STYLES[i].style));
        /* Mark the current style BEFORE connecting so setup is silent.     */
        if (app->toolbar_style[kind] == STYLES[i].style)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item),
                                           TRUE);
        g_signal_connect(item, "toggled",
                         G_CALLBACK(on_style_menu_toggled), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
    return TRUE;
}

void
on_app_register_toolbar(OnApp *app, OnToolbarKind kind, GtkWidget *toolbar)
{
    gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), app->toolbar_style[kind]);
    g_ptr_array_add(app->toolbars[kind], toolbar);
    g_object_set_data(G_OBJECT(toolbar), "on-registry",
                      app->toolbars[kind]);
    g_object_set_data(G_OBJECT(toolbar), "on-kind", GINT_TO_POINTER(kind));
    g_signal_connect(toolbar, "destroy",
                     G_CALLBACK(on_toolbar_destroyed), app);
    /* Right-clicking any toolbar offers its style choices directly.        */
    g_signal_connect(toolbar, "popup-context-menu",
                     G_CALLBACK(on_toolbar_context_menu), app);
}

void
on_app_set_toolbar_style(OnApp *app, OnToolbarKind kind,
                         GtkToolbarStyle style)
{
    app->toolbar_style[kind] = style;

    /* Restyle every live toolbar of this kind.                             */
    GPtrArray *registry = app->toolbars[kind];
    for (guint i = 0; i < registry->len; i++)
        gtk_toolbar_set_style(
            GTK_TOOLBAR(g_ptr_array_index(registry, i)), style);

    /* Persist the choice.                                                  */
    const gchar *value =             /* settings-table representation       */
        (style == GTK_TOOLBAR_TEXT)  ? "text"
      : (style == GTK_TOOLBAR_ICONS) ? "icons"
                                     : "both";
    on_db_setting_set(app->db, STYLE_SETTING_KEYS[kind], value);
}

void
on_app_load_toolbar_styles(OnApp *app)
{
    for (gint kind = 0; kind < ON_TOOLBAR_N_KINDS; kind++) {
        gchar *value = on_db_setting_get(app->db, STYLE_SETTING_KEYS[kind]);
        GtkToolbarStyle style = GTK_TOOLBAR_BOTH;    /* icons above text    */
        if (value != NULL) {
            if (g_strcmp0(value, "text") == 0)
                style = GTK_TOOLBAR_TEXT;
            else if (g_strcmp0(value, "icons") == 0)
                style = GTK_TOOLBAR_ICONS;
            g_free(value);
        }
        app->toolbar_style[kind] = style;
    }
}
