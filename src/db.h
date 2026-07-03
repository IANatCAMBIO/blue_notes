/* ===========================================================================
 * db.h — SQLite persistence layer for Blue Notes
 *
 * All notes, folders and tags live in a single SQLite database file stored
 * in the user's data directory (e.g. ~/.local/share/blue_notes/notes.db).
 *
 * Note *content* is stored as an opaque binary BLOB in the custom "BNBF"
 * format produced by serialize.c; this module never interprets it.
 *
 * Schema
 * ------
 *   folders   (id, parent_id, name, sort_order)          -- nested via parent_id
 *   notes     (id, folder_id, title, content BLOB,
 *              sort_order, created_at, updated_at)
 *   tags      (id, name UNIQUE)
 *   note_tags (note_id, tag_id)                          -- many-to-many
 *
 * A NULL parent_id means "top-level folder"; a NULL folder_id means the
 * note lives at the top level ("Notes" root).
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
 *   id         — primary key of the folder.
 *   parent_id  — id of the parent folder, or 0 for top level.
 *   name       — display name (owned string).
 *   sort_order — position among its siblings (ascending).
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64  id;
    gint64  parent_id;
    gchar  *name;
    gint    sort_order;
} OnFolder;

/* ---------------------------------------------------------------------------
 * OnNoteMeta — lightweight note metadata used by list views.
 *
 * Fields:
 *   id         — primary key of the note.
 *   folder_id  — id of the containing folder, or 0 for top level.
 *   title      — display title, derived from the first line (owned string).
 *   sort_order — position among notes in the same folder.
 *   updated_at — UNIX timestamp of the last save.
 *   pinned     — whether the note appears in the Pinned Notes section.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64   id;
    gint64   folder_id;
    gchar   *title;
    gint     sort_order;
    gint64   updated_at;
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

/* --------------------------- lifecycle ---------------------------------- */

/* The default database path (~/.local/share/blue_notes/notes.db),
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

/* Delete folder `id`; all descendant folders and contained notes are
 * removed by ON DELETE CASCADE. Returns TRUE on success.                    */
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

/* Permanently delete note `id`. Returns TRUE on success.                    */
gboolean on_db_note_delete(OnDatabase *db, gint64 id);

/* Move note `id` into folder `folder_id` (0 = top level), appending it at
 * the end of that folder. Returns TRUE on success.                          */
gboolean on_db_note_move(OnDatabase *db, gint64 id, gint64 folder_id);

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

/* List ALL notes in the database (for export), ordered by folder then
 * sort_order. Returns a GList of OnNoteMeta*; free with
 * on_db_note_list_free().                                                   */
GList *on_db_note_list_all(OnDatabase *db);

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

/* Look up tag `name`, creating it if missing. Returns the tag id, or 0
 * on failure.                                                               */
gint64 on_db_tag_get_or_create(OnDatabase *db, const gchar *name);

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

/* ----------------------------- counts ----------------------------------- */

/* Number of notes directly inside folder `folder_id` (0 = top level).      */
gint on_db_note_count_for_folder(OnDatabase *db, gint64 folder_id);

/* Number of notes labeled with tag `tag_id`.                               */
gint on_db_note_count_for_tag(OnDatabase *db, gint64 tag_id);

/* Total row counts across the whole database (any out-param may be NULL). */
void on_db_totals(OnDatabase *db, gint *notes, gint *folders, gint *tags);

/* Per-folder note counts in ONE query (folder id 0 = top level).
 * Returns a GHashTable of gint64* → GINT_TO_POINTER(count); destroy with
 * g_hash_table_destroy(). Missing keys mean zero.                           */
GHashTable *on_db_note_count_map(OnDatabase *db);

/* Per-tag note counts in one query; same shape as above.                    */
GHashTable *on_db_tag_count_map(OnDatabase *db);

/* ---------------------------- settings ---------------------------------- */

/* Read persistent setting `key`. Returns a newly allocated value string
 * (g_free() it), or NULL if the key was never set.                         */
gchar *on_db_setting_get(OnDatabase *db, const gchar *key);

/* Store persistent setting `key` = `value` (upsert).
 * Returns TRUE on success.                                                 */
gboolean on_db_setting_set(OnDatabase *db, const gchar *key,
                           const gchar *value);

/* Remove persistent setting `key`. Returns TRUE on success.                */
gboolean on_db_setting_delete(OnDatabase *db, const gchar *key);

/* ---------------------------- utilities --------------------------------- */

/* Build the "/Folder/Sub" style path of folder `folder_id` (empty string
 * for 0). Used by the exporter to mirror the hierarchy on disk.
 * Returns a newly allocated string; g_free() it.                            */
gchar *on_db_folder_path(OnDatabase *db, gint64 folder_id);

#endif /* BLUE_DB_H */
