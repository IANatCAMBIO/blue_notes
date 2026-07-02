/* ===========================================================================
 * search_window.h — the note search window
 *
 * A small window opened from the library's sidebar toolbar:
 *
 *   +--------------------------------------+
 *   | [ search text            ] [Search]  |
 *   | ( ) All notes  (o) Only "Work"       |
 *   | [ ] Case sensitive  [ ] Regex        |
 *   |--------------------------------------|
 *   | result                               |
 *   | result          (double-click opens) |
 *   +--------------------------------------+
 *
 * Matching is against note titles and their full plain text.  The scope
 * radio limits the search to the folder or tag currently selected in the
 * library.  Case-sensitive and regular-expression matching are optional.
 * =========================================================================== */

#ifndef ORANGE_SEARCH_WINDOW_H
#define ORANGE_SEARCH_WINDOW_H

#include "app.h"

/* What the search may be scoped to (mirrors the library selection).        */
typedef enum {
    ON_SCOPE_ALL,                    /* every note in the database          */
    ON_SCOPE_FOLDER,                 /* one folder's direct notes           */
    ON_SCOPE_TAG,                    /* notes carrying one tag              */
} OnSearchScope;

/* ---------------------------------------------------------------------------
 * on_search_window_open() — show a new search window.
 *
 * The window offers two scopes: "All Notes" and "Selected Folder/Tag".
 * The latter is resolved against the library's live sidebar selection
 * each time the Search button is pressed (via on_library_get_scope), so
 * changing the selection between searches changes what gets searched.
 *
 *   app          — global application context.
 *   scope_to_sel — TRUE preselects the "Selected Folder/Tag" radio (used
 *                  when search is launched from a folder's context menu);
 *                  FALSE preselects "All Notes".
 * ------------------------------------------------------------------------- */
void on_search_window_open(OnApp *app, gboolean scope_to_sel);

#endif /* ORANGE_SEARCH_WINDOW_H */
