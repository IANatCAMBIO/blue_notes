/* ===========================================================================
 * main.c — Orange Notes application entry point
 *
 * Wires everything together: opens the SQLite database, creates the
 * shared OnApp context, and shows the library window when the
 * GtkApplication activates.  Editor windows are opened from the library
 * (see library_window.c / editor_window.c).
 * =========================================================================== */

#include <gtk/gtk.h>

#include "app.h"
#include "db.h"
#include "library_window.h"

#ifdef HAVE_GTKOSX
#include <gtkosxapplication.h>
#endif

/* ---------------------------------------------------------------------------
 * on_activate() — GtkApplication "activate" handler: show the library
 * window, or just raise it if the app is activated a second time.
 *   gtk_app   — the application.
 *   user_data — the OnApp context created in main().
 * ------------------------------------------------------------------------- */
static void
on_activate(GtkApplication *gtk_app, gpointer user_data)
{
    (void)gtk_app;
    OnApp *app = user_data;          /* shared application context          */

    if (app->library_window != NULL) {
        gtk_window_present(GTK_WINDOW(app->library_window));
        return;
    }

    /* Default window icon: orange.png next to the icons/ folder.           */
    gchar *base = g_path_get_dirname(app->icons_dir);
    gchar *icon_path = g_build_filename(base, "orange.png", NULL);
    gtk_window_set_default_icon_from_file(icon_path, NULL);
    g_free(icon_path);
    g_free(base);

    on_library_window_create(app);

#ifdef HAVE_GTKOSX
    /* Honor the persisted native-menu-bar preference, then let the macOS
     * integration finish its launch handshake.                             */
    gchar *native = on_db_setting_get(app->db, "native_menubar");
    if (g_strcmp0(native, "1") == 0)
        on_library_apply_native_menubar(app, TRUE);
    g_free(native);
    gtkosx_application_ready(gtkosx_application_get());
#endif
}

/* ---------------------------------------------------------------------------
 * main() — set up the application context, run the GTK main loop, and
 * tear everything down afterwards.
 *   argc/argv — standard program arguments (GTK consumes its own flags).
 * Returns the process exit status.
 * ------------------------------------------------------------------------- */
int
main(int argc, char *argv[])
{
    /* Classic full-width scrollbars everywhere instead of the modern
     * overlay style (must be set before GTK initializes).                  */
    g_setenv("GTK_OVERLAY_SCROLLING", "0", TRUE);

    /* Open (or create) the notes database first — without it there is
     * nothing to show.  A custom location (e.g. a shared folder) may be
     * configured in the config file; fall back to the default if that
     * location has become unreachable.                                     */
    gchar *db_dir = on_app_config_load_db_dir();
    gchar *db_path = (db_dir != NULL)
                     ? g_build_filename(db_dir, "notes.db", NULL)
                     : NULL;
    OnDatabase *db = on_db_open(db_path);
    if (db == NULL && db_path != NULL) {
        g_printerr("orange_notes: cannot open %s; using the default "
                   "database location\n", db_path);
        g_free(db_dir);
        db_dir = NULL;
        db = on_db_open(NULL);
    }
    g_free(db_path);
    if (db == NULL) {
        g_printerr("orange_notes: could not open the notes database\n");
        return 1;
    }

    /* The shared context handed to every window.                           */
    OnApp app = {
        .gtk_app              = NULL,
        .db                   = db,
        /* Map of note id -> open editor window.  Keys are heap-allocated
         * gint64s owned (and freed) by the table itself.                   */
        .editors              = g_hash_table_new_full(g_int64_hash,
                                                      g_int64_equal,
                                                      g_free, NULL),
        .library_window       = NULL,
        .notify_notes_changed = NULL,
        .toolbar_style        = { GTK_TOOLBAR_BOTH, GTK_TOOLBAR_BOTH },
        .toolbars             = { NULL, NULL },
        .icons_dir            = NULL,
        .db_dir               = NULL,
    };
    app.db_dir = db_dir;             /* ownership transferred               */
    for (gint k = 0; k < ON_TOOLBAR_N_KINDS; k++)
        app.toolbars[k] = g_ptr_array_new();
    on_app_load_toolbar_styles(&app);
    on_app_init_icons_dir(&app, argv[0]);

    /* Code-block copy buttons are on unless explicitly disabled.           */
    gchar *ccb = on_db_setting_get(db, "code_copy_button");
    app.code_copy_buttons = g_strcmp0(ccb, "0") != 0;
    g_free(ccb);

    app.gtk_app = gtk_application_new("org.example.orange-notes",
                                      G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app.gtk_app, "activate",
                     G_CALLBACK(on_activate), &app);

    int status = g_application_run(G_APPLICATION(app.gtk_app), argc, argv);

    /* Windows (and their final autosaves) are done by the time run()
     * returns, so the database can be closed safely now.                   */
    g_object_unref(app.gtk_app);
    g_hash_table_destroy(app.editors);
    for (gint k = 0; k < ON_TOOLBAR_N_KINDS; k++)
        g_ptr_array_free(app.toolbars[k], TRUE);
    g_free(app.icons_dir);
    g_free(app.db_dir);
    on_db_close(app.db);
    return status;
}
