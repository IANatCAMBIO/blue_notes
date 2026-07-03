/* ===========================================================================
 * library_window.h — the Notes Library window
 *
 * The library is the app's main window (standard titlebar, no HeaderBar):
 *
 *   +--------------------------------------------------------------+
 *   | File  View                                   (menu bar)      |
 *   +----------------+---------------------------------------------+
 *   | v Notes        |  [New Note] [New Folder] [Delete] [List|Grid]|
 *   |   > Work       |                                             |
 *   |   > Personal   |   note   note   note        (list or grid)  |
 *   | v Tags         |   note   note                               |
 *   |   #todo        |                                             |
 *   +----------------+---------------------------------------------+
 *
 * The sidebar shows the nested folder hierarchy and, below it, every
 * known #tag.  Selecting a folder or tag shows its notes on the right,
 * as a list or a grid.  Notes can be drag-reordered within the list and
 * dragged onto sidebar folders to move them.  Double-clicking a note
 * opens it in its own editor window.
 * =========================================================================== */

#ifndef BLUE_LIBRARY_WINDOW_H
#define BLUE_LIBRARY_WINDOW_H

#include "app.h"
#include "search_window.h"

/* ---------------------------------------------------------------------------
 * on_library_window_create() — build and show the library window.
 *
 * Also installs app->notify_notes_changed so editor windows can trigger
 * refreshes, and stores the window in app->library_window.
 *
 *   app — global application context.
 * Returns the new GtkWindow.
 * ------------------------------------------------------------------------- */
GtkWidget *on_library_window_create(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_library_get_scope() — the library's current sidebar selection, used
 * by the search window to resolve "Selected Folder/Tag" at search time.
 *   app   — global application context.
 *   scope — receives ON_SCOPE_FOLDER (folders and the root) or
 *           ON_SCOPE_TAG.
 *   id    — receives the folder/tag id (0 for the root).
 *   name  — receives the selection's display name (g_free() it).
 * ------------------------------------------------------------------------- */
void on_library_get_scope(OnApp *app, OnSearchScope *scope, gint64 *id,
                          gchar **name);

/* ---------------------------------------------------------------------------
 * on_library_apply_native_menubar() — move the library's menu into the
 * native macOS menu bar (hiding the in-window one), or restore it.
 * Compiled in only when the gtk-mac-integration library is available
 * (HAVE_GTKOSX); a no-op otherwise.
 *   app    — global application context.
 *   native — TRUE for the macOS menu bar, FALSE for the in-window bar.
 * ------------------------------------------------------------------------- */
void on_library_apply_native_menubar(OnApp *app, gboolean native);

#endif /* BLUE_LIBRARY_WINDOW_H */
