/* ===========================================================================
 * db.h — SQLite persistence layer for Blue Notes
 *
 * All notes, folders and tags live in a single SQLite database file stored
 * in the user's data directory
 * (e.g. ~/.local/share/blue_notes/blue_notes.db).
 *
 * Note *content* is stored as an opaque binary BLOB in the custom "BNBF"
 * format produced by serialize.c; this module never interprets it.
 *
 * Schema
 * ------
 *   folders   (id, parent_id, name, sort_order, trashed, ai_mode, emoji) -- nested via parent_id
 *   notes     (id, folder_id, title, content BLOB,
 *              sort_order, created_at, updated_at,
 *              pinned, body_text, trashed)
 *   tags      (id, name UNIQUE)
 *   note_tags    (note_id, tag_id)                       -- many-to-many
 *   action_items (note_id, ord, text, done, due)         -- queryable mirror of '!' lines
 *
 * A NULL parent_id means "top-level folder"; a NULL folder_id means the
 * note lives at the top level ("Notes" root).
 *
 * Trash is a soft-delete FLAG, not a folder: deleting sets trashed=1 and
 * leaves folder_id/parent_id untouched (that is the "restore to" location).
 * A trashed folder keeps its whole subtree attached — only the top folder
 * is flagged; its descendants (and their notes) are implicitly trashed via
 * the trash_folder_ids view (recursive closure of flagged folders).  All
 * normal listings/counts filter trash out; on_db_note_list_all() can
 * include it so search still finds deleted notes.
 * =========================================================================== */

#ifndef BLUE_DB_H
#define BLUE_DB_H

#include <glib.h>
#include <sqlite3.h>

/* ---------------------------------------------------------------------------
 * OnDatabase — an open handle to the notes database.
 *
 * Fields:
 *   handle — the underlying sqlite3 connection, owned by this struct.
 *   path   — absolute path of the database file (owned string).
 * ------------------------------------------------------------------------- */
typedef struct {
    sqlite3 *handle;
    gchar   *path;
} OnDatabase;

/* ---------------------------------------------------------------------------
 * OnFolder — metadata for one folder row.
 *
 * Fields:
 *   id    — primary key of the folder.
 *   name  — display name (owned string).
 *   emoji — optional display prefix shown in the sidebar (owned string;
 *            "" when not set).
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64  id;
    gchar  *name;
    gchar  *emoji;
} OnFolder;

/* ---------------------------------------------------------------------------
 * OnNoteMeta — lightweight note metadata used by list views.
 *
 * Fields:
 *   id         — primary key of the note.
 *   folder_id  — id of the containing folder, or 0 for top level.
 *   title      — display title, derived from the first line (owned string).
 *   updated_at — UNIX timestamp of the last save.
 *   created_at — UNIX timestamp of the note's creation.
 *   pinned     — whether the note appears in the Pinned Notes section.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64   id;
    gint64   folder_id;
    gchar   *title;
    gint64   updated_at;
    gint64   created_at;
    gboolean pinned;
} OnNoteMeta;

/* ---------------------------------------------------------------------------
 * OnTag — one tag row.
 *
 * Fields:
 *   id   — primary key of the tag.
 *   name — tag text without the leading '#' (owned string).
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64  id;
    gchar  *name;
} OnTag;

/* ---------------------------------------------------------------------------
 * OnActionItem — one follow-up action item: a note line starting with '!'
 * (see on_note_extract_actions in serialize.h, which derives these from
 * note content; the action_items table is a queryable mirror rebuilt on
 * every save, like note_tags).
 *
 * Fields:
 *   note_id — owning note (0 when coming straight from the extractor).
 *   ord     — the item's position among the note's action lines.
 *   text    — item text: the '!' line's remainder, trimmed, without any
 *             trailing "due <date>" (owned).
 *   done    — completed: the whole text is struck through in the note.
 *   due     — due date as a UNIX timestamp (local midnight), parsed from
 *             the line's trailing "due <date>"; 0 = no due date.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64   note_id;
    gint     ord;
    gchar   *text;
    gboolean done;
    gint64   due;
} OnActionItem;

/* --------------------------- lifecycle ---------------------------------- */

/* The database filename inside its directory (default or configured).      */
#define ON_DB_FILENAME "blue_notes.db"

/* The default database path (~/.local/share/blue_notes/blue_notes.db),
 * creating the directory if needed. Returns a new string; g_free() it.     */
gchar *on_db_default_path(void);

/* Open (creating if necessary) the notes database.
 *   path_override — explicit db path, or NULL to use the default location
 *                   under g_get_user_data_dir().
 * Returns a new OnDatabase*, or NULL on failure (error is logged).          */
OnDatabase *on_db_open(const gchar *path_override);

/* Write a consistent snapshot of the (possibly in-use) database to
 * `dest_path` using SQLite's online backup API. Returns TRUE on success.   */
gboolean on_db_backup_to(OnDatabase *db, const gchar *dest_path);

/* Close the database and free the handle.  Safe to call with NULL.         */
void on_db_close(OnDatabase *db);

/* ---------------------------- folders ----------------------------------- */

/* Create a folder.
 *   parent_id — id of the parent folder, or 0 for top level.
 *   name      — display name for the new folder.
 * Returns the new folder's id, or 0 on failure.                             */
gint64 on_db_folder_create(OnDatabase *db, gint64 parent_id, const gchar *name);

/* Rename folder `id` to `name`. Returns TRUE on success.                    */
gboolean on_db_folder_rename(OnDatabase *db, gint64 id, const gchar *name);

/* ai_mode values stored in folders.ai_mode.                                 */
#define ON_AI_MODE_NORMAL  0
#define ON_AI_MODE_PROJECT 1
#define ON_AI_MODE_CUSTOM  2

gboolean on_db_folder_set_ai_mode(OnDatabase *db, gint64 id, gint mode);
gint     on_db_folder_get_ai_mode(OnDatabase *db, gint64 id);

/* Get a folder's display emoji. Returns a new string (g_free() it; never
 * NULL — "" when not set).                                                   */
gchar   *on_db_folder_get_emoji(OnDatabase *db, gint64 id);

/* Set a folder's display emoji (pass "" or NULL to clear it). Returns TRUE
 * on success.                                                                */
gboolean on_db_folder_set_emoji(OnDatabase *db, gint64 id, const gchar *emoji);

/* Move folder `id` (with its whole subtree, implicitly) under
 * `parent_id` (0 = top level), appending it after that parent's existing
 * children.  Also clears the trashed flag, so dragging a folder out of
 * the Trash restores it to the drop location.  Refuses cycles: moving a
 * folder into itself or any of its descendants returns FALSE.               */
gboolean on_db_folder_move(OnDatabase *db, gint64 id, gint64 parent_id);

/* Persist an explicit ordering of folders within one parent.
 *   folder_ids — ids in the desired order (sort_order = array index).
 *   n          — number of ids.
 * All updates run in one transaction. Returns TRUE on success.               */
gboolean on_db_folder_reorder(OnDatabase *db, const gint64 *folder_ids,
                              gsize n);

/* PERMANENTLY delete folder `id`; all descendant folders and contained
 * notes are removed by ON DELETE CASCADE and orphaned tags are pruned.
 * The GUI trashes folders instead (on_db_folder_trash); this is the
 * empty-trash / CLI path. Returns TRUE on success.                          */
gboolean on_db_folder_delete(OnDatabase *db, gint64 id);

/* List the *direct* children of `parent_id` (0 = top level), ordered by
 * sort_order then name.
 * Returns a GList of OnFolder*; free with on_db_folder_list_free().         */
GList *on_db_folder_list(OnDatabase *db, gint64 parent_id);

/* Free a list returned by on_db_folder_list().                              */
void on_db_folder_list_free(GList *folders);

/* ----------------------------- notes ------------------------------------ */

/* Create an empty note in folder `folder_id` (0 = top level).
 * Returns the new note's id, or 0 on failure.                               */
gint64 on_db_note_create(OnDatabase *db, gint64 folder_id);

/* Permanently delete `n` notes in ONE transaction, pruning orphaned tags
 * once at the end. Returns TRUE on success.                                 */
gboolean on_db_notes_delete(OnDatabase *db, const gint64 *ids, gsize n);

/* Move `n` notes into folder `folder_id` (0 = top level) in ONE
 * transaction (per-note moves would fsync per call — a big
 * multi-selection drop froze the GUI), appended in array order.  Clears
 * each note's trashed flag (drag out of Trash = restore).  Returns TRUE
 * if every move succeeded (all-or-nothing: failure rolls the batch back).   */
gboolean on_db_notes_move(OnDatabase *db, const gint64 *note_ids, gsize n,
                          gint64 folder_id);

/* Persist a note's title, serialized content, and searchable plain text.
 *   id        — note to save.
 *   title     — display title (first line of the note).
 *   content   — BNBF blob bytes (may be NULL when len is 0).
 *   len       — length of `content` in bytes.
 *   body_text — plain text of the note for fast searching (may be NULL).
 * Also bumps updated_at. Returns TRUE on success.                           */
gboolean on_db_note_save(OnDatabase *db, gint64 id, const gchar *title,
                         const guint8 *content, gsize len,
                         const gchar *body_text);

/* Read a note's cached searchable text. Returns a new string (g_free()
 * it), or NULL when the cache is unfilled (older rows) — callers then
 * extract from the blob and may write it back via
 * on_db_note_set_body_text().                                               */
gchar *on_db_note_body_text(OnDatabase *db, gint64 id);

/* Fill the searchable-text cache for one note.                              */
gboolean on_db_note_set_body_text(OnDatabase *db, gint64 id,
                                  const gchar *body_text);

/* Overwrite a note's updated_at with an explicit UNIX timestamp.  Used by
 * importers to preserve the original modification date (ordinary saves
 * stamp the current time). Returns TRUE on success.                         */
gboolean on_db_note_set_updated_at(OnDatabase *db, gint64 id, gint64 ts);

/* Load a note's serialized content.
 *   id      — note to load.
 *   out_len — receives the blob length in bytes.
 * Returns a newly allocated buffer (g_free() it), or NULL if the note has
 * no content yet or on error.                                               */
guint8 *on_db_note_load(OnDatabase *db, gint64 id, gsize *out_len);

/* Fetch metadata for a single note. Returns an OnNoteMeta* to free with
 * on_db_note_meta_free(), or NULL if not found.                             */
OnNoteMeta *on_db_note_get(OnDatabase *db, gint64 id);

/* List notes directly inside folder `folder_id` (0 = top level), ordered
 * by sort_order then updated_at descending.
 * Returns a GList of OnNoteMeta*; free with on_db_note_list_free().         */
GList *on_db_note_list(OnDatabase *db, gint64 folder_id);

/* List ALL notes in the database, ordered by folder then sort_order.
 *   include_trash — TRUE to include trashed notes and notes inside
 *                   trashed folders (search wants them); FALSE for the
 *                   normal visible set (export, CLI listing).
 * Returns a GList of OnNoteMeta*; free with on_db_note_list_free().         */
GList *on_db_note_list_all(OnDatabase *db, gboolean include_trash);

/* List every visible (non-trashed) note, newest first — the library's
 * "All Notes" view. Returns a GList of OnNoteMeta*; free with
 * on_db_note_list_free().                                                   */
GList *on_db_note_list_recent(OnDatabase *db);

/* Set or clear a note's pinned flag. Returns TRUE on success.               */
gboolean on_db_note_set_pinned(OnDatabase *db, gint64 id, gboolean pinned);

/* List every pinned note, newest first.
 * Returns a GList of OnNoteMeta*; free with on_db_note_list_free().         */
GList *on_db_note_list_pinned(OnDatabase *db);

/* Number of pinned notes.                                                   */
gint on_db_note_count_pinned(OnDatabase *db);

/* Persist an explicit ordering of notes within one folder.
 *   note_ids — array of note ids in the desired display order.
 *   n        — number of ids in the array.
 * Returns TRUE on success.                                                  */
gboolean on_db_note_reorder(OnDatabase *db, const gint64 *note_ids, gsize n);

/* Free one OnNoteMeta and its strings.                                      */
void on_db_note_meta_free(OnNoteMeta *meta);

/* Free a list returned by on_db_note_list()/on_db_note_list_all().          */
void on_db_note_list_free(GList *notes);

/* ------------------------------ tags ------------------------------------ */

/* Look up tag `name` WITHOUT creating it. Returns its id, or 0.             */
gint64 on_db_tag_find(OnDatabase *db, const gchar *name);

/* Delete tag `id` (its note links go with it; the literal #text inside
 * notes is untouched and will recreate the tag when such a note is saved
 * again). Returns TRUE on success.                                          */
gboolean on_db_tag_delete(OnDatabase *db, gint64 id);

/* List every known tag, ordered by name.
 * Returns a GList of OnTag*; free with on_db_tag_list_free().               */
GList *on_db_tag_list(OnDatabase *db);

/* Free a list returned by on_db_tag_list().                                 */
void on_db_tag_list_free(GList *tags);

/* Replace note `note_id`'s tag set with `tag_names` (a GList of "const
 * gchar*" tag names, without '#'). Creates missing tags and prunes tags
 * that no longer label any note. Returns TRUE on success.                   */
gboolean on_db_note_set_tags(OnDatabase *db, gint64 note_id, GList *tag_names);

/* List all notes labeled with tag `tag_id`, newest first.
 * Returns a GList of OnNoteMeta*; free with on_db_note_list_free().         */
GList *on_db_notes_by_tag(OnDatabase *db, gint64 tag_id);

/* List the tags labeling note `note_id`, ordered by name.
 * Returns a GList of OnTag*; free with on_db_tag_list_free().               */
GList *on_db_note_tag_list(OnDatabase *db, gint64 note_id);

/* --------------------------- action items -------------------------------- */

/* Replace note `note_id`'s action_items rows with `items` (a GList of
 * OnActionItem; ord is assigned from list order).  One transaction.
 * Returns TRUE on success.                                                  */
gboolean on_db_note_set_actions(OnDatabase *db, gint64 note_id,
                                GList *items);

/* Every action item of every visible (non-trashed) note, newest note
 * first, note order preserved within a note.  Returns a GList of
 * OnActionItem*; free with on_db_action_list_free().                        */
GList *on_db_action_list(OnDatabase *db);

/* Set one item's done flag (addressed by note id + position).               */
gboolean on_db_action_set_done(OnDatabase *db, gint64 note_id, gint ord,
                               gboolean done);

/* Item counts across all visible notes (either out-param may be NULL):
 * `total` = every item, `open` = the not-yet-done ones.                     */
void on_db_action_counts(OnDatabase *db, gint *total, gint *open);

/* Free a list of OnActionItem (from on_db_action_list or
 * on_note_extract_actions).                                                 */
void on_db_action_list_free(GList *items);

/* --------------------------- schema version ------------------------------ */

/* PRAGMA user_version accessors — gate one-time backfills (0 = unset).     */
gint on_db_user_version(OnDatabase *db);
gboolean on_db_set_user_version(OnDatabase *db, gint version);

/* ----------------------------- trash ------------------------------------ */

/* Move `n` notes to the Trash in one transaction (trashed=1; folder_id is
 * kept as the restore location). Returns TRUE on success.                   */
gboolean on_db_notes_trash(OnDatabase *db, const gint64 *ids, gsize n);

/* Restore note `id` from the Trash.  Its folder_id is kept unless that
 * folder is itself (implicitly) trashed, in which case the note lands at
 * the top level. Also un-nests a note out of a trashed folder (moves it
 * to the top level) even when the note itself carries no trashed flag.
 * Returns TRUE on success.                                                  */
gboolean on_db_note_restore(OnDatabase *db, gint64 id);

/* Move folder `id` (with its whole subtree, implicitly) to the Trash.       */
gboolean on_db_folder_trash(OnDatabase *db, gint64 id);

/* Restore folder `id` to its original parent, or to the top level when
 * that parent is itself still in the Trash. Returns TRUE on success.        */
gboolean on_db_folder_restore(OnDatabase *db, gint64 id);

/* List the directly-trashed folders (the Trash section's children),
 * ordered by name. Returns a GList of OnFolder*; free with
 * on_db_folder_list_free().                                                 */
GList *on_db_folder_list_trashed(OnDatabase *db);

/* List the directly-trashed notes (what selecting Trash shows), newest
 * first. Returns a GList of OnNoteMeta*; free with on_db_note_list_free(). */
GList *on_db_note_list_trashed(OnDatabase *db);

/* Number of items directly in the Trash: trashed notes + trashed folders. */
gint on_db_trash_count(OnDatabase *db);

/* Ids of EVERY note that would go with an empty-trash: directly-trashed
 * notes plus all notes inside trashed folder subtrees.  Used to close
 * their editors first. Returns a GArray of gint64; g_array_free(_, TRUE).   */
GArray *on_db_trash_note_ids(OnDatabase *db);

/* Ids of every note in folder `folder_id`'s subtree (the folder itself
 * included). Returns a GArray of gint64; g_array_free(_, TRUE).             */
GArray *on_db_folder_note_ids(OnDatabase *db, gint64 folder_id);

/* Permanently delete everything in the Trash (one transaction, orphaned
 * tags pruned). Returns TRUE on success.                                    */
gboolean on_db_trash_empty(OnDatabase *db);

/* ----------------------------- counts ----------------------------------- */

/* Number of visible (non-trashed) notes in the whole database — the
 * count shown on the "All Notes" sidebar row.                               */
gint on_db_note_count_visible(OnDatabase *db);

/* Total row counts across the whole database (any out-param may be NULL). */
void on_db_totals(OnDatabase *db, gint *notes, gint *folders, gint *tags);

/* Per-folder note counts in ONE query (folder id 0 = top level).
 * Returns a GHashTable of gint64* → GINT_TO_POINTER(count); destroy with
 * g_hash_table_destroy(). Missing keys mean zero.                           */
GHashTable *on_db_note_count_map(OnDatabase *db);

/* Every filled body_text row in ONE query: note id (gint64*) → owned
 * text.  Cross-note search reads this instead of one SELECT per note —
 * the whole column is ~1 MB of text even for image-heavy databases,
 * and per-query latency is what hurts on shared/network DBs.  Rows with
 * a NULL cache (pre-column saves) are absent; callers fall back to
 * on_db_note_body_text()/extraction for those.
 * Destroy with g_hash_table_destroy().                                      */
GHashTable *on_db_note_body_map(OnDatabase *db);

/* Per-tag note counts in one query; same shape as above.                    */
GHashTable *on_db_tag_count_map(OnDatabase *db);

/* ---------------------------- utilities --------------------------------- */

/* Build the "/Folder/Sub" style path of folder `folder_id` (empty string
 * for 0). Used by the exporter to mirror the hierarchy on disk.
 * Returns a newly allocated string; g_free() it.                            */
gchar *on_db_folder_path(OnDatabase *db, gint64 folder_id);

/* All folder paths in ONE query: folder id (gint64*) → "Folder/Sub"
 * string (same format as on_db_folder_path, no leading slash).  Trashed
 * folders are included so Trash listings resolve too; the top level (0)
 * has no entry — callers treat a miss as "".  Per-note path lookups in
 * the library go through this map, never per-row on_db_folder_path —
 * per-query latency hurts on shared/network DBs.
 * Free with g_hash_table_destroy().                                         */
GHashTable *on_db_folder_path_map(OnDatabase *db);

#endif /* BLUE_DB_H */
