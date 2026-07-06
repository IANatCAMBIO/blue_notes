/* ===========================================================================
 * main.c — Blue Notes application entry point
 *
 * Wires everything together: opens the SQLite database, creates the
 * shared OnApp context, and shows the library window when the
 * GtkApplication activates.  Editor windows are opened from the library
 * (see library_window.c / editor_window.c).
 * =========================================================================== */

#include <gtk/gtk.h>
#include <glib-unix.h>

#include "app.h"
#include "cli.h"
#include "db.h"
#include "ipc.h"
#include "library_window.h"

#ifdef HAVE_GTKOSX
#include <gtkosxapplication.h>
#endif

#ifdef __APPLE__
/* ---------------------------------------------------------------------------
 * quartz_log_filter() — GLogFunc that drops one specific, benign GDK
 * assertion emitted on macOS and forwards everything else unchanged.
 *
 * When GTK enumerates the clipboard's targets (the "TARGETS" atom — done
 * whenever the right-click/selection menus appear, on rich-text paste, and in
 * drag negotiation), the GDK Quartz backend converts each NSPasteboard type
 * to a GdkAtom via gdk_atom_intern(uti.preferredMIMEType.UTF8String)
 * (gdk/quartz/gdkselection-quartz.c).  Modern macOS pasteboards routinely
 * carry Apple-private types whose UTI has no MIME string, so UTF8String is
 * NULL and gdk_atom_intern trips its "atom_name != NULL" g_return_if_fail.
 * The check is non-fatal — gdk_atom_intern returns GDK_NONE and the
 * enumeration keeps going with the valid types — but it prints a Gdk-CRITICAL
 * on every affected paste/menu.  We cannot reach the upstream call site, so we
 * silence just this message and pass all other logs through untouched.
 *   domain  — log domain ("Gdk" for the offending message).
 *   level   — log level flags.
 *   message — the formatted log text.
 *   data    — unused.
 * ------------------------------------------------------------------------- */
static void
quartz_log_filter(const gchar   *domain,
                  GLogLevelFlags level,
                  const gchar   *message,
                  gpointer       data)
{
    (void)data;
    if (message != NULL &&
        strstr(message, "gdk_atom_intern") != NULL &&
        strstr(message, "atom_name != NULL") != NULL)
        return;                      /* benign macOS pasteboard artifact     */
    g_log_default_handler(domain, level, message, data);
}
#endif /* __APPLE__ */

/* ---------------------------------------------------------------------------
 * integrity_collect() — sqlite3_exec callback: accumulates non-"ok" rows
 * from a PRAGMA integrity_check result into the GString passed as `data`.
 * ------------------------------------------------------------------------- */
static int
integrity_collect(void *data, int argc, char **argv, char **col_names)
{
    (void)col_names;
    GString *out = data;
    for (int i = 0; i < argc; i++) {
        if (argv[i] != NULL && g_strcmp0(argv[i], "ok") != 0) {
            if (out->len > 0)
                g_string_append_c(out, '\n');
            g_string_append(out, argv[i]);
        }
    }
    return 0;
}

/* startup_integrity_check() — run PRAGMA integrity_check and show results.
 * Returns TRUE if the check passed (caller may proceed to open normally).   */
static gboolean
startup_integrity_check(OnApp *app)
{
    GString *errors = g_string_new(NULL);
    sqlite3_exec(app->db->handle, "PRAGMA integrity_check",
                 integrity_collect, errors, NULL);
    gboolean ok = (errors->len == 0);

    if (ok) {
        GtkWidget *d = gtk_message_dialog_new(
            NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "The database passed the integrity check.");
        gtk_window_set_title(GTK_WINDOW(d), "Blue Notes - Integrity Check");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
    } else {
        GtkWidget *d = gtk_message_dialog_new(
            NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "The integrity check found errors:\n\n%s", errors->str);
        gtk_window_set_title(GTK_WINDOW(d),
                             "Blue Notes - Integrity Check Failed");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
    }
    g_string_free(errors, TRUE);
    return ok;
}

/* startup_first_run() — no notes.db exists at the expected location: ask
 * whether to open an existing file or create a new one there, instead of
 * silently creating an empty database (a user pointing at a shared folder
 * usually means to OPEN a file that is already there).
 *   expected — the path where the database was looked for (dialog text).
 *   db_dir   — in/out: the configured db directory; replaced (and
 *              persisted to the ini) when an existing file is opened.
 *   db_path  — in/out: the path handed to on_db_open(); replaced when an
 *              existing file is opened.
 * Returns TRUE to proceed with on_db_open(*db_path), FALSE to quit.        */
static gboolean
startup_first_run(const gchar *expected, gchar **db_dir, gchar **db_path)
{
    /* Loop so cancelling the file chooser returns to the choice dialog.    */
    for (;;) {
        GtkWidget *dlg = gtk_message_dialog_new(
            NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
            "No notes database was found at\n%s",
            expected);
        gtk_window_set_title(GTK_WINDOW(dlg), "Blue Notes - Welcome");
        gtk_dialog_add_buttons(GTK_DIALOG(dlg),
            "_Open a notes.db File",  1,
            "Create a _New notes.db", 2,
            NULL);
        gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);

        if (resp == 2) {
            /* A stale hash (ini kept, DB file gone) would flag the fresh
             * database as "changed" in on_activate — clear it.             */
            on_app_config_set("db_hash", NULL);
            return TRUE;             /* on_db_open() creates it             */
        }
        if (resp != 1)
            return FALSE;            /* dialog closed — quit                */

        GtkWidget *chooser = gtk_file_chooser_dialog_new(
            "Open Notes Database", NULL,
            GTK_FILE_CHOOSER_ACTION_OPEN,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Open",   GTK_RESPONSE_ACCEPT,
            NULL);
        gtk_window_set_title(GTK_WINDOW(chooser),
                             "Blue Notes - Open Database");
        /* The app's model is a directory + the fixed name notes.db (the
         * ini stores db_dir only), so only notes.db files are openable.    */
        GtkFileFilter *ff = gtk_file_filter_new();
        gtk_file_filter_add_pattern(ff, "notes.db");
        gtk_file_filter_set_name(ff, "Notes Database (notes.db)");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), ff);

        gchar *file_path = NULL;     /* the chosen database file            */
        if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
            file_path = gtk_file_chooser_get_filename(
                GTK_FILE_CHOOSER(chooser));
        gtk_widget_destroy(chooser);
        if (file_path == NULL)
            continue;                /* cancelled — back to the choice      */

        gchar *dir = g_path_get_dirname(file_path);
        on_app_config_set("db_dir",  dir);
        on_app_config_set("db_hash", NULL);   /* stale for this file        */
        g_free(*db_dir);   *db_dir  = dir;
        g_free(*db_path);  *db_path = file_path;
        return TRUE;
    }
}

/* startup_check_db_hash() — compare the stored MD5 hash against the current
 * DB file.  If they differ, show a warning dialog offering two choices.
 * Returns TRUE when the app should proceed, FALSE to quit.
 *   verified — receives TRUE only when the database actually checked out
 *              (hash matched, or PRAGMA integrity_check passed) — not
 *              when the user proceeded past the warning unchecked.         */
static gboolean
startup_check_db_hash(OnApp *app, gboolean *verified)
{
    *verified = FALSE;
    gchar *stored = on_app_config_get("db_hash");
    if (stored == NULL)
        return TRUE;    /* no prior hash — first run or feature just enabled  */

    gchar *current = on_app_db_compute_hash(app->db->path);
    gboolean changed = (current == NULL ||
                        g_strcmp0(stored, current) != 0);
    g_free(current);
    g_free(stored);

    if (!changed) {
        *verified = TRUE;
        return TRUE;    /* hash matches — proceed normally                    */
    }

    /* Loop so a failed integrity check can re-present the warning dialog.   */
    for (;;) {
        GtkWidget *dlg = gtk_message_dialog_new(
            NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
            "The database has changed since last access.\n\n"
            "This may have been done by another Blue Notes instance, "
            "or it could indicate database corruption.");
        gtk_window_set_title(GTK_WINDOW(dlg),
                             "Blue Notes - Database Changed");
        gtk_dialog_add_buttons(GTK_DIALOG(dlg),
            "_Open Anyway",          1,
            "Run _Integrity Check",  2,
            NULL);
        gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);

        if (resp == 1) {
            return TRUE;                        /* open as-is                */
        } else if (resp == 2) {
            if (startup_integrity_check(app)) {
                *verified = TRUE;
                return TRUE;                    /* passed — proceed          */
            }
            /* failed — loop back to the warning dialog                      */
        } else {
            return FALSE;                       /* dialog closed — quit      */
        }
    }
}

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

    /* Default window icon: the app logo from the icons/ folder.            */
    gchar *icon_path = g_build_filename(app->icons_dir, "vinyl.png",
                                        NULL);
    gtk_window_set_default_icon_from_file(icon_path, NULL);
    g_free(icon_path);

    /* Bundled scalable theme icons (icons/theme/hicolor/...): provides
     * SVG pan-*-symbolic arrows so tree expanders render crisply on
     * HiDPI displays instead of GTK's built-in 1x raster fallbacks.        */
    gchar *theme_dir = g_build_filename(app->icons_dir, "theme", NULL);
    gtk_icon_theme_prepend_search_path(gtk_icon_theme_get_default(),
                                       theme_dir);
    g_free(theme_dir);

    /* DB integrity check: warn if the file changed since last exit.        */
    gboolean db_verified = FALSE;    /* did the startup check actually pass?*/
    if (app->db_integrity_check &&
        !startup_check_db_hash(app, &db_verified)) {
        g_application_quit(G_APPLICATION(app->gtk_app));
        return;
    }

    on_library_window_create(app);

    /* The window opened with "DB at … loaded"; append the check's verdict
     * when the startup verification actually ran and passed.               */
    if (db_verified)
        on_app_status(app, "DB at %s loaded, integrity check passed",
                      app->db->path);

    /* Listen for "quicknote"/"note open" from later CLI invocations, then
     * run any action a CLI already queued because no instance was running.  */
    on_ipc_server_start(app);
    on_ipc_run_pending(app);

#ifdef HAVE_GTKOSX
    /* Honor the persisted native-menu-bar preference, then let the macOS
     * integration finish its launch handshake.                             */
    gchar *native = on_app_config_get("native_menubar");
    if (g_strcmp0(native, "1") == 0)
        on_library_apply_native_menubar(app, TRUE);
    g_free(native);
    gtkosx_application_ready(gtkosx_application_get());
#endif
}

/* ---------------------------------------------------------------------------
 * on_sigterm() — terminate gracefully on SIGTERM (pkill, logout, system
 * shutdown): destroying every window flushes editor autosaves and lets
 * the main loop end cleanly.
 * ------------------------------------------------------------------------- */
static gboolean
on_sigterm(gpointer user_data)
{
    OnApp *app = user_data;          /* shared application context          */
    GList *windows =                 /* copy: destroying mutates the list   */
        g_list_copy(gtk_application_get_windows(app->gtk_app));
    for (GList *l = windows; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(windows);
    return G_SOURCE_REMOVE;
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
#ifdef __APPLE__
    /* Silence one benign, upstream GDK-Quartz clipboard critical (see
     * quartz_log_filter).  Installed before GTK so it covers every paste.   */
    g_log_set_handler("Gdk",
                      G_LOG_LEVEL_CRITICAL | G_LOG_FLAG_RECURSION,
                      quartz_log_filter, NULL);
#endif

    /* The application config (blue_notes.ini) lives next to the
     * binary; resolve its location before anything reads it.               */
    on_app_config_init(argv[0]);

    /* Headless automation: a recognized subcommand runs and exits
     * without ever creating windows (see cli.h for the command list).      */
    int cli_status = on_cli_run(argc, argv);
    if (cli_status >= 0)
        return cli_status;

    /* Classic full-width scrollbars everywhere instead of the modern
     * overlay style (must be set before GTK initializes).                  */
    g_setenv("GTK_OVERLAY_SCROLLING", "0", TRUE);

    /* Open (or create) the notes database first — without it there is
     * nothing to show.  A custom location (e.g. a shared folder) may be
     * configured in the config file.  There is deliberately NO fallback
     * to another location: silently opening a different database once
     * made a user's notes "disappear" (a startup racing the previous
     * instance's shutdown flush).  One configured database, or a clear
     * error.                                                               */
    gchar *db_dir = on_app_config_load_db_dir();
    gchar *db_path = (db_dir != NULL)
                     ? g_build_filename(db_dir, "notes.db", NULL)
                     : NULL;

    /* First launch (or an emptied configured directory): no notes.db at
     * the expected location.  Ask before creating one — the user may mean
     * to open an existing file elsewhere.  Needs GTK up early for the
     * dialog; skipped without a display (old create-silently behavior).    */
    {
        gchar *expected = (db_path != NULL)  /* where the db was looked for */
                          ? g_strdup(db_path)
                          : on_db_default_path();
        if (!g_file_test(expected, G_FILE_TEST_EXISTS) &&
            gtk_init_check(&argc, &argv) &&
            !startup_first_run(expected, &db_dir, &db_path)) {
            g_free(expected);
            g_free(db_path);
            g_free(db_dir);
            return 0;                /* user closed the dialog — quit       */
        }
        g_free(expected);
    }

    OnDatabase *db = on_db_open(db_path);
    if (db == NULL) {
        g_printerr("blue_notes: could not open the notes database at "
                   "%s\n(if another instance is still shutting down, "
                   "try again in a few seconds)\n",
                   db_path != NULL ? db_path : "the default location");
        g_free(db_path);
        g_free(db_dir);
        return 1;
    }
    g_free(db_path);

    /* One-time migration: UI settings used to live in the database's
     * settings table; move any leftovers into the ini (existing ini
     * values win) and purge them.                                          */
    static const gchar *MIGRATE_KEYS[] = {
        "toolbar_style_library", "toolbar_style_editor",
        "code_copy_button", "code_line_numbers", "native_menubar",
        "sidebar_counts", "image_viewer", "search_win_w", "search_win_h",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(MIGRATE_KEYS); i++) {
        gchar *old = on_db_setting_get(db, MIGRATE_KEYS[i]);
        if (old == NULL)
            continue;
        gchar *cur = on_app_config_get(MIGRATE_KEYS[i]);
        if (cur == NULL)
            on_app_config_set(MIGRATE_KEYS[i], old);
        g_free(cur);
        on_db_setting_delete(db, MIGRATE_KEYS[i]);
        g_free(old);
    }

    /* Remove any legacy "in_use" instance lock left by an older version.   */
    on_db_setting_delete(db, "in_use");

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
        .toolbar_style        = { GTK_TOOLBAR_ICONS, GTK_TOOLBAR_ICONS },
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
    gchar *ccb = on_app_config_get("code_copy_button");
    app.code_copy_buttons = g_strcmp0(ccb, "0") != 0;
    g_free(ccb);

    /* Code-block line numbers are off unless explicitly enabled.           */
    gchar *cln = on_app_config_get("code_line_numbers");
    app.code_line_numbers = g_strcmp0(cln, "1") == 0;
    g_free(cln);

    /* Sidebar folder/tag counts (hidden unless explicitly enabled).        */
    gchar *sbc = on_app_config_get("sidebar_counts");
    app.sidebar_counts = g_strcmp0(sbc, "1") == 0;
    g_free(sbc);

    /* Auto-H1 for the first line of a new note (off unless enabled).      */
    gchar *flh = on_app_config_get("first_line_h1");
    app.first_line_h1 = g_strcmp0(flh, "1") == 0;
    g_free(flh);

    /* Compact editor toolbar (off unless explicitly enabled).              */
    gchar *cet = on_app_config_get("compact_editor_toolbar");
    app.compact_editor_toolbar = g_strcmp0(cet, "1") == 0;
    g_free(cet);

    /* DB integrity check is enabled unless explicitly disabled.            */
    gchar *dic = on_app_config_get("db_integrity_check");
    app.db_integrity_check = g_strcmp0(dic, "0") != 0;
    g_free(dic);

    app.gtk_app = gtk_application_new("org.example.blue-notes",
                                      G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app.gtk_app, "activate",
                     G_CALLBACK(on_activate), &app);
    g_unix_signal_add(SIGTERM, on_sigterm, &app);

    /* A "quicknote"/"note open" that found no running instance falls
     * through to here to start the GUI.  Those leftover words are not GTK
     * options — run with a clean argv so GApplication doesn't try to open
     * them as files ("This application can not open files").                */
    int status;                      /* main-loop exit status               */
    if (on_ipc_has_pending()) {
        char *solo_argv[] = { argv[0], NULL };
        status = g_application_run(G_APPLICATION(app.gtk_app), 1, solo_argv);
    } else {
        status = g_application_run(G_APPLICATION(app.gtk_app), argc, argv);
    }

    /* Stop serving CLI commands and unlink the socket before teardown.      */
    on_ipc_server_stop();

    /* Windows (and their final autosaves) are done by the time run()
     * returns, so the database can be closed and hashed safely now.        */
    g_object_unref(app.gtk_app);
    g_hash_table_destroy(app.editors);
    for (gint k = 0; k < ON_TOOLBAR_N_KINDS; k++)
        g_ptr_array_free(app.toolbars[k], TRUE);

    /* Snapshot the DB file's MD5 so the next launch can detect external
     * changes.  Must run AFTER on_db_close so the file is fully flushed.
     * Skip when the session was a transient open (not the default DB) —
     * the stored hash must reflect the configured DB, not a one-time one.   */
    if (app.db_integrity_check && app.db != NULL && !app.db_transient) {
        gchar *db_path_snap = g_strdup(app.db->path);
        on_db_close(app.db);
        app.db = NULL;
        gchar *hash = on_app_db_compute_hash(db_path_snap);
        if (hash != NULL) {
            on_app_config_set("db_hash", hash);
            g_free(hash);
        }
        g_free(db_path_snap);
    } else {
        on_db_close(app.db);
    }

    g_free(app.icons_dir);
    g_free(app.db_dir);
    return status;
}
