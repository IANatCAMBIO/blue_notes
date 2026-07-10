/* ===========================================================================
 * editor_window.h — WYSIWYG note editor window
 *
 * Each note opens in its own top-level GtkWindow (standard titlebar, no
 * GtkHeaderBar) containing a formatting toolbar and a GtkTextView.
 *
 * Features:
 *   - inline styles: bold, italic, underline, strikethrough
 *   - paragraph styles: heading 1/2, bulleted list, numbered list,
 *     monospace code blocks (with a one-click "copy code block" button)
 *   - inline images pasted from the clipboard or inserted from a file
 *   - inline #tags with an autocomplete popup (space ends the tag)
 *   - debounced autosave to SQLite via the BNBF serializer
 * =========================================================================== */

#ifndef BLUE_EDITOR_WINDOW_H
#define BLUE_EDITOR_WINDOW_H

#include "app.h"

/* ---------------------------------------------------------------------------
 * on_editor_window_open() — open (or focus) the editor for a note.
 *
 * If the note already has an open editor window, that window is presented;
 * otherwise a new window is created, the note content is loaded from the
 * database, and the window is shown.
 *
 *   app     — global application context.
 *   note_id — id of the note to edit.
 * Returns the editor's GtkWindow, or NULL if the note does not exist.
 * ------------------------------------------------------------------------- */
GtkWidget *on_editor_window_open(OnApp *app, gint64 note_id);

/* ---------------------------------------------------------------------------
 * on_editor_window_open_search() — like on_editor_window_open(), but also
 * pre-populates the editor's in-note search box with `search_term` and jumps
 * to the first match.  Used when a note is opened from the library's search
 * window so the searched-for text is highlighted straight away.  A NULL or
 * empty term behaves exactly like on_editor_window_open().
 *
 *   app         — global application context.
 *   note_id     — id of the note to edit.
 *   search_term — text to seed the in-note search with (may be NULL).
 * Returns the editor's GtkWindow, or NULL if the note does not exist.
 * ------------------------------------------------------------------------- */
GtkWidget *on_editor_window_open_search(OnApp *app, gint64 note_id,
                                        const gchar *search_term);

/* ---------------------------------------------------------------------------
 * on_editor_rebuild_code_buttons_all() — re-evaluate the code-block copy
 * buttons in every open editor window.  Called by the settings window
 * when the "show copy button" preference changes.
 * ------------------------------------------------------------------------- */
void on_editor_rebuild_code_buttons_all(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_editor_apply_line_numbers_all() — show/hide the code-block line
 * number gutter in every open editor per app->code_line_numbers.  Called
 * by the settings window when the preference changes.
 * ------------------------------------------------------------------------- */
void on_editor_apply_line_numbers_all(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_editor_rebuild_toolbars_all() — rebuild the formatting toolbar of
 * every open editor per app->compact_editor_toolbar.  Called by the
 * settings window when the compact-toolbar preference changes.
 * ------------------------------------------------------------------------- */
void on_editor_rebuild_toolbars_all(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_editor_status_refresh_all() — re-render the status-bar location of
 * every open editor window.  Called by the settings window when the
 * DB-path-prefix preference changes (applies live).
 * ------------------------------------------------------------------------- */
void on_editor_status_refresh_all(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_editor_action_set_done() — reflect an action item's done state into
 * its note CONTENT: the '!' line's text is struck through (done) or
 * un-struck (reopened).  With the note open in an editor the live buffer
 * is edited and autosave persists it (including the action_items rows);
 * otherwise the stored blob is rewritten offscreen (cached PNG bytes keep
 * images from being re-encoded) and the table refreshed immediately.
 * Called by the library's Action Items checkbox column.
 *   app     — global application context.
 *   note_id — the note holding the item.
 *   ord     — the item's position among the note's action lines.
 *   done    — the new state.
 * Returns TRUE when the item was found and updated.
 * ------------------------------------------------------------------------- */
gboolean on_editor_action_set_done(OnApp *app, gint64 note_id, gint ord,
                                   gboolean done);

/* ---------------------------------------------------------------------------
 * on_editor_action_set_due() — rewrite an action item's due date in its
 * note CONTENT: the '!' line's trailing "due <date>" is replaced (or
 * appended; `due` = 0 removes it).  The written form is ISO
 * ("due 2026-07-07"); appended text inherits the item's strike state.
 * Live-buffer + autosave when the note is open, offscreen rewrite
 * otherwise — same contract as on_editor_action_set_done.
 *   due — local-midnight UNIX timestamp, or 0 to clear.
 * Returns TRUE when the item was found and updated.
 * ------------------------------------------------------------------------- */
gboolean on_editor_action_set_due(OnApp *app, gint64 note_id, gint ord,
                                  gint64 due);

#endif /* BLUE_EDITOR_WINDOW_H */
