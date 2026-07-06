/* ===========================================================================
 * app.h — shared application context for Blue Notes
 *
 * A single OnApp instance is created in main() and passed to every window.
 * It owns the database handle, tracks open editor windows, carries the
 * user's toolbar-style preference, and loads button icons from the
 * app-local icons/ folder (see on_app_icon_image_sized).
 * =========================================================================== */

#ifndef BLUE_APP_H
#define BLUE_APP_H

#include <gtk/gtk.h>
#include "db.h"

/* ---------------------------------------------------------------------------
 * OnApp — global application state.
 *
 * Fields:
 *   gtk_app        — the GtkApplication driving the main loop.
 *   db             — open notes database (owned; closed at shutdown).
 *   editors        — map of open editor windows, keyed by note id
 *                    (gint64* keys, GtkWindow* values).  An entry exists
 *                    exactly while that note's editor window is open.
 *   library_window — the (single) library window, or NULL before startup.
 *   notify_notes_changed — hook installed by the library window: the
 *                    FULL refresh (sidebar counts/tags + notes pane).
 *                    For structural changes — notes created/moved/
 *                    deleted, database switched/restored, tag set
 *                    edited.  May be NULL.
 *   notify_note_saved — lighter hook, also installed by the library
 *                    window: refreshes only the notes pane (titles,
 *                    modified times).  Editor saves use this unless the
 *                    note's tag set changed — editing a note can never
 *                    change folder counts, so the sidebar is left
 *                    untouched (and its scrollbar unmoved).  May be NULL.
 *   notify_status  — hook installed by the library window: shows an event
 *                    message ("DB saved", …) on the right side of its
 *                    status bar.  Post through on_app_status(), which
 *                    handles the hook being NULL.
 *   toolbar_style  — how toolbar buttons render (text only, icons only,
 *                    or icons above text), kept separately for library
 *                    toolbars and editor toolbars.  Indexed by
 *                    OnToolbarKind; persisted in the settings table.
 *   toolbars       — every live toolbar per kind, so a style change can
 *                    be applied to all open windows at once.  Entries
 *                    remove themselves on destroy.
 *   icons_dir      — absolute path of the local icons/ folder the button
 *                    icons (elementary SVGs) are loaded from (owned
 *                    string).
 *   code_copy_buttons — whether code blocks show their floating copy
 *                    button (File → Settings…); persisted as the
 *                    "code_copy_button" setting.
 *   sidebar_counts — whether the library sidebar shows note counts next
 *                    to folders and tags; persisted as the
 *                    "sidebar_counts" setting (default off).
 *   first_line_h1  — whether the first line typed into a brand-new note
 *                    is automatically formatted as Heading 1; persisted
 *                    as the "first_line_h1" setting (default off).
 *   compact_editor_toolbar — whether the editor toolbar collapses the
 *                    paragraph-style buttons (H1/H2/¶) into a "Styles"
 *                    menu button and the list buttons into a "Lists"
 *                    one; persisted as the "compact_editor_toolbar"
 *                    setting (default off).
 *   db_dir         — custom directory holding notes.db (owned string), or
 *                    NULL for the default location.  Persisted in the
 *                    config FILE (blue_notes.ini next to the binary), not
 *                    the database — the database's own location cannot
 *                    live inside it.
 *   db_integrity_check — when TRUE, the MD5 of the database file is
 *                    written to the ini on exit and verified on the next
 *                    launch; a mismatch triggers a warning dialog.
 * ------------------------------------------------------------------------- */

/* Which family a toolbar belongs to — each has its own style setting.       */
typedef enum {
    ON_TOOLBAR_LIBRARY = 0,          /* library + sidebar toolbars          */
    ON_TOOLBAR_EDITOR  = 1,          /* editor-window formatting toolbars   */
    ON_TOOLBAR_N_KINDS
} OnToolbarKind;

typedef struct OnApp {
    GtkApplication  *gtk_app;
    OnDatabase      *db;
    GHashTable      *editors;
    GtkWidget       *library_window;
    void           (*notify_notes_changed)(struct OnApp *app);
    void           (*notify_note_saved)(struct OnApp *app);
    void           (*notify_status)(struct OnApp *app, const gchar *message);
    GtkToolbarStyle  toolbar_style[ON_TOOLBAR_N_KINDS];
    GPtrArray       *toolbars[ON_TOOLBAR_N_KINDS];
    gchar           *icons_dir;
    gboolean         code_copy_buttons;
    gboolean         code_line_numbers;
    gboolean         sidebar_counts;
    gboolean         first_line_h1;
    gboolean         compact_editor_toolbar;
    gchar           *db_dir;
    gboolean         db_integrity_check;
    gboolean         db_transient;     /* TRUE when the current DB was opened
                                        * for this session only (not default) */
} OnApp;

/* ---------------------------------------------------------------------------
 * on_app_status() — post a one-line event message to the library window's
 * status bar (printf-style).  Safe to call from anywhere: a no-op until
 * the library window has installed app->notify_status.
 *   app — the application context.
 *   fmt — printf-style format for the message.
 * ------------------------------------------------------------------------- */
void on_app_status(OnApp *app, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

/* ---------------------------------------------------------------------------
 * on_app_init_icons_dir() — locate the icons/ folder next to the
 * executable (falling back to ./icons) and remember it in app->icons_dir.
 *   app   — the application context.
 *   argv0 — argv[0] from main(), used to find the executable's directory.
 * ------------------------------------------------------------------------- */
void on_app_init_icons_dir(OnApp *app, const gchar *argv0);

/* ---------------------------------------------------------------------------
 * on_app_icon_image_sized() — build a GtkImage for icon `name` from
 * "<icons_dir>/<name>.svg" (then .png), rendered at an explicit pixel
 * size.  The bundled icons are elementary SVGs, which need the librsvg
 * gdk-pixbuf loader to decode.
 *   app  — the application context.
 *   name — icon file basename without extension (e.g. "edit-copy").
 *   size — logical pixel size to render at.
 * Returns a new GtkImage, or NULL if no loadable file exists — callers
 * fall back to a text label in that case.
 * ------------------------------------------------------------------------- */
GtkWidget *on_app_icon_image_sized(OnApp *app, const gchar *name,
                                   gint size);

/* ---------------------------------------------------------------------------
 * on_app_tool_item_new() — create a toolbar button that honors the
 * app-wide toolbar style.
 *   app             — the application context.
 *   toggle          — TRUE for a GtkToggleToolButton, FALSE for a plain
 *                     GtkToolButton.
 *   icon_name       — local icon file to use (see on_app_icon_image), or
 *                     NULL for none.
 *   fallback_markup — Pango markup rendered as the "icon" when the icon
 *                     file is missing (e.g. "<b>H1</b>"); NULL to fall
 *                     back to the plain label.
 *   label           — the button's text label (shown in text/both modes).
 *   tooltip         — hover help text.
 * Returns the new tool item (not yet shown).
 * ------------------------------------------------------------------------- */
GtkToolItem *on_app_tool_item_new(OnApp *app, gboolean toggle,
                                  const gchar *icon_name,
                                  const gchar *fallback_markup,
                                  const gchar *label,
                                  const gchar *tooltip);

/* ---------------------------------------------------------------------------
 * on_app_register_toolbar() — apply the current style for `kind` to
 * `toolbar` and keep it updated when that style changes.  The toolbar
 * unregisters itself automatically when destroyed.
 * ------------------------------------------------------------------------- */
void on_app_register_toolbar(OnApp *app, OnToolbarKind kind,
                             GtkWidget *toolbar);

/* ---------------------------------------------------------------------------
 * on_app_set_toolbar_style() — change one toolbar family's style on every
 * live toolbar of that kind and persist the choice.
 * ------------------------------------------------------------------------- */
void on_app_set_toolbar_style(OnApp *app, OnToolbarKind kind,
                              GtkToolbarStyle style);

/* ---------------------------------------------------------------------------
 * on_app_load_toolbar_styles() — read both persisted toolbar styles into
 * app->toolbar_style[] (defaulting to icons-above-text when unset).
 * ------------------------------------------------------------------------- */
void on_app_load_toolbar_styles(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_app_config_init() — resolve the application config file once
 * ("blue_notes.ini" in the same directory as the binary, from `argv0`)
 * and load it into memory.  All later reads are served from memory; the
 * file is only written when a setting changes.  Must run before any
 * other config call; safe to call repeatedly.
 * ------------------------------------------------------------------------- */
void on_app_config_init(const gchar *argv0);

/* ---------------------------------------------------------------------------
 * on_app_config_get() — read one setting from the in-memory config.
 * Returns a new string (g_free() it), or NULL when unset/empty.
 * ------------------------------------------------------------------------- */
gchar *on_app_config_get(const gchar *key);

/* ---------------------------------------------------------------------------
 * on_app_config_set() — change one setting: updates the in-memory config
 * AND writes the ini file through.  NULL removes the key.
 * ------------------------------------------------------------------------- */
void on_app_config_set(const gchar *key, const gchar *value);

/* ---------------------------------------------------------------------------
 * on_app_config_load_db_dir() — read the custom database directory from
 * the config file. Returns a new string (g_free() it), or NULL when the
 * default location is in use.
 * ------------------------------------------------------------------------- */
gchar *on_app_config_load_db_dir(void);

/* ---------------------------------------------------------------------------
 * on_app_close_all_editors() — destroy every open editor window (each
 * flushes its final autosave on destroy). Used before switching or
 * restoring the database.
 * ------------------------------------------------------------------------- */
void on_app_close_all_editors(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_app_switch_database() — move the app onto a different database
 * location, live: closes all editors, closes the current database, opens
 * notes.db inside `new_dir` (NULL = the default location), and persists
 * the choice in the config file.  If the target has no notes.db yet, the
 * current database file is copied there first, so notes follow the move.
 * If the target ALREADY has a notes.db, the user is asked first — use
 * the existing database, overwrite it with a copy of the current one, or
 * cancel (which leaves everything untouched).  Failures are reported in
 * a dialog and the old database is reopened.
 *   app     — the application context.
 *   new_dir — directory to hold notes.db, or NULL for the default.
 * Returns TRUE if the switch happened.
 * ------------------------------------------------------------------------- */
gboolean on_app_switch_database(OnApp *app, const gchar *new_dir);

/* ---------------------------------------------------------------------------
 * on_app_restore_database() — replace the current database with a backup
 * file: closes all editors, snapshots the current file next to itself as
 * "notes.db.pre-restore", copies `backup_path` over the active location,
 * and reopens.  Returns TRUE on success (on failure the old file is
 * still in place and reopened).
 * ------------------------------------------------------------------------- */
gboolean on_app_restore_database(OnApp *app, const gchar *backup_path);

/* ---------------------------------------------------------------------------
 * on_app_db_compute_hash() — compute the MD5 hex digest of the database
 * file at `path`.  Returns a new string (g_free it), or NULL on I/O error.
 * ------------------------------------------------------------------------- */
gchar *on_app_db_compute_hash(const gchar *path);

#endif /* BLUE_APP_H */
