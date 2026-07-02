/* ===========================================================================
 * app.h — shared application context for Orange Notes
 *
 * A single OnApp instance is created in main() and passed to every window.
 * It owns the database handle, tracks open editor windows, carries the
 * user's toolbar-style preference, and loads button icons from the
 * app-local icons/ folder (see on_app_icon_image).
 * =========================================================================== */

#ifndef ORANGE_APP_H
#define ORANGE_APP_H

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
 *   notify_notes_changed — hook installed by the library window; editor
 *                    windows call it after every save so lists, titles
 *                    and the tag sidebar stay current.  May be NULL.
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
 *   db_dir         — custom directory holding notes.db (owned string), or
 *                    NULL for the default location.  Persisted in the
 *                    config FILE (~/.config/orange-notes/config.ini), not
 *                    the database — the database's own location cannot
 *                    live inside it.
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
    GtkToolbarStyle  toolbar_style[ON_TOOLBAR_N_KINDS];
    GPtrArray       *toolbars[ON_TOOLBAR_N_KINDS];
    gchar           *icons_dir;
    gboolean         code_copy_buttons;
    gchar           *db_dir;
} OnApp;

/* ---------------------------------------------------------------------------
 * on_app_init_icons_dir() — locate the icons/ folder next to the
 * executable (falling back to ./icons) and remember it in app->icons_dir.
 *   app   — the application context.
 *   argv0 — argv[0] from main(), used to find the executable's directory.
 * ------------------------------------------------------------------------- */
void on_app_init_icons_dir(OnApp *app, const gchar *argv0);

/* ---------------------------------------------------------------------------
 * on_app_icon_image() — build a GtkImage for icon `name` from
 * "<icons_dir>/<name>.svg" (then .png).  The bundled icons are elementary
 * SVGs, which need the librsvg gdk-pixbuf loader to decode.
 *   app  — the application context.
 *   name — icon file basename without extension (e.g. "edit-copy").
 * Returns a new GtkImage, or NULL if no loadable file exists — callers
 * fall back to a text label in that case.
 * ------------------------------------------------------------------------- */
GtkWidget *on_app_icon_image(OnApp *app, const gchar *name);

/* ---------------------------------------------------------------------------
 * on_app_icon_image_sized() — like on_app_icon_image() but rendered at an
 * explicit pixel size (used for compact inline buttons).
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
 * On failure the old database is reopened.
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

#endif /* ORANGE_APP_H */
