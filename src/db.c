/* ===========================================================================
 * db.c — SQLite persistence layer for Blue Notes (implementation)
 *
 * See db.h for the public API and schema overview.  All functions log
 * failures through g_warning() and return a "failed" value rather than
 * aborting, so the UI can degrade gracefully.
 * =========================================================================== */

#include "db.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * SCHEMA_SQL — DDL executed every time the database is opened.
 * Every statement uses IF NOT EXISTS so re-running is harmless.
 * ------------------------------------------------------------------------- */
static const char *SCHEMA_SQL =
    "PRAGMA foreign_keys = ON;"
    "CREATE TABLE IF NOT EXISTS folders ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  parent_id  INTEGER REFERENCES folders(id) ON DELETE CASCADE,"
    "  name       TEXT NOT NULL,"
    "  sort_order INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS notes ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  folder_id  INTEGER REFERENCES folders(id) ON DELETE CASCADE,"
    "  title      TEXT NOT NULL DEFAULT 'New Note',"
    "  content    BLOB,"
    "  sort_order INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL,"
    "  updated_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS tags ("
    "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name TEXT NOT NULL UNIQUE"
    ");"
    "CREATE TABLE IF NOT EXISTS note_tags ("
    "  note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,"
    "  tag_id  INTEGER NOT NULL REFERENCES tags(id)  ON DELETE CASCADE,"
    "  PRIMARY KEY (note_id, tag_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS settings ("
    "  key   TEXT PRIMARY KEY,"
    "  value TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_notes_folder  ON notes(folder_id);"
    "CREATE INDEX IF NOT EXISTS idx_folders_parent ON folders(parent_id);"
    "CREATE INDEX IF NOT EXISTS idx_note_tags_tag ON note_tags(tag_id);";

/* ---------------------------------------------------------------------------
 * TRASH_VIEW_SQL — the recursive closure of the Trash: every folder that
 * is flagged trashed OR sits anywhere below a flagged folder.  Created
 * AFTER the column migrations in on_db_open (it references
 * folders.trashed).  Notes inside these folders are implicitly trashed
 * without carrying their own flag.
 * ------------------------------------------------------------------------- */
static const char *TRASH_VIEW_SQL =
    "CREATE VIEW IF NOT EXISTS trash_folder_ids AS "
    "WITH RECURSIVE tf(id) AS ("
    "  SELECT id FROM folders WHERE trashed=1"
    "  UNION"
    "  SELECT f.id FROM folders f JOIN tf ON f.parent_id = tf.id"
    ") SELECT id FROM tf;";

/* WHERE fragment: a note is visible in normal (non-Trash) views — not
 * directly trashed and not inside a trashed folder's subtree.               */
#define NOTE_VISIBLE_SQL \
    "trashed=0 AND (folder_id IS NULL OR " \
    "folder_id NOT IN (SELECT id FROM trash_folder_ids))"

/* ---------------------------------------------------------------------------
 * exec_simple() — run a parameterless SQL string, logging any error.
 *   db  — open database.
 *   sql — SQL text to execute.
 * Returns TRUE if the statement(s) executed without error.
 * ------------------------------------------------------------------------- */
static gboolean
exec_simple(OnDatabase *db, const char *sql)
{
    char *errmsg = NULL;                 /* sqlite-allocated error string   */
    if (sqlite3_exec(db->handle, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        g_warning("db: exec failed: %s (sql: %.80s)", errmsg, sql);
        sqlite3_free(errmsg);
        return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * prepare() — wrap sqlite3_prepare_v2 with error logging.
 *   db  — open database.
 *   sql — single SQL statement with '?' placeholders.
 * Returns a prepared statement to finalize with sqlite3_finalize(), or
 * NULL on error.
 * ------------------------------------------------------------------------- */
static sqlite3_stmt *
prepare(OnDatabase *db, const char *sql)
{
    sqlite3_stmt *stmt = NULL;           /* the compiled statement          */
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        g_warning("db: prepare failed: %s (sql: %.80s)",
                  sqlite3_errmsg(db->handle), sql);
        return NULL;
    }
    return stmt;
}

/* ---------------------------------------------------------------------------
 * bind_id_or_null() — bind a folder/parent id, mapping 0 to SQL NULL.
 * The schema uses NULL (not 0) to mean "top level", so every id bind for
 * a nullable column goes through this helper.
 *   stmt — statement to bind into.
 *   idx  — 1-based parameter index.
 *   id   — the id value; 0 means NULL.
 * ------------------------------------------------------------------------- */
static void
bind_id_or_null(sqlite3_stmt *stmt, int idx, gint64 id)
{
    if (id > 0)
        sqlite3_bind_int64(stmt, idx, id);
    else
        sqlite3_bind_null(stmt, idx);
}

/* =========================================================================
 * lifecycle
 * ========================================================================= */

gchar *
on_db_default_path(void)
{
    gchar *dir = g_build_filename(g_get_user_data_dir(),
                                  "blue_notes", NULL);
    g_mkdir_with_parents(dir, 0700);
    gchar *path = g_build_filename(dir, "notes.db", NULL);
    g_free(dir);
    return path;
}

OnDatabase *
on_db_open(const gchar *path_override)
{
    /* Resolve the database path: explicit override, or the per-user data
     * directory default.                                                    */
    gchar *path = (path_override != NULL)   /* final absolute db path      */
                  ? g_strdup(path_override)
                  : on_db_default_path();

    OnDatabase *db = g_new0(OnDatabase, 1);
    db->path = path;

    if (sqlite3_open(path, &db->handle) != SQLITE_OK) {
        g_warning("db: cannot open %s: %s", path, sqlite3_errmsg(db->handle));
        on_db_close(db);
        return NULL;
    }

    /* Wait out short write locks instead of failing instantly — e.g. a
     * CLI command landing while the GUI is mid-autosave.                   */
    sqlite3_busy_timeout(db->handle, 5000);

    if (!exec_simple(db, SCHEMA_SQL)) {
        on_db_close(db);
        return NULL;
    }

    /* Migrations: these columns arrived after the original schema.
     * ALTER fails harmlessly when the column already exists.               */
    sqlite3_exec(db->handle,
                 "ALTER TABLE notes ADD COLUMN pinned INTEGER "
                 "NOT NULL DEFAULT 0",
                 NULL, NULL, NULL);
    sqlite3_exec(db->handle,
                 "ALTER TABLE notes ADD COLUMN body_text TEXT",
                 NULL, NULL, NULL);
    sqlite3_exec(db->handle,
                 "ALTER TABLE notes ADD COLUMN trashed INTEGER "
                 "NOT NULL DEFAULT 0",
                 NULL, NULL, NULL);
    sqlite3_exec(db->handle,
                 "ALTER TABLE folders ADD COLUMN trashed INTEGER "
                 "NOT NULL DEFAULT 0",
                 NULL, NULL, NULL);

    /* The Trash view references the trashed columns, so it is created
     * only after the migrations above have run.                             */
    if (!exec_simple(db, TRASH_VIEW_SQL)) {
        on_db_close(db);
        return NULL;
    }
    return db;
}

gboolean
on_db_backup_to(OnDatabase *db, const gchar *dest_path)
{
    sqlite3 *dest = NULL;            /* the backup file's connection        */
    if (sqlite3_open(dest_path, &dest) != SQLITE_OK) {
        g_warning("db: backup: cannot open %s: %s",
                  dest_path, sqlite3_errmsg(dest));
        sqlite3_close(dest);
        return FALSE;
    }

    /* The online backup API snapshots a live database safely.              */
    sqlite3_backup *backup =
        sqlite3_backup_init(dest, "main", db->handle, "main");
    gboolean ok = FALSE;             /* overall success                     */
    if (backup != NULL) {
        sqlite3_backup_step(backup, -1);     /* copy everything             */
        sqlite3_backup_finish(backup);
        ok = sqlite3_errcode(dest) == SQLITE_OK;
    }
    if (!ok)
        g_warning("db: backup to %s failed: %s",
                  dest_path, sqlite3_errmsg(dest));
    sqlite3_close(dest);
    return ok;
}

void
on_db_close(OnDatabase *db)
{
    if (db == NULL)
        return;
    if (db->handle != NULL)
        sqlite3_close(db->handle);
    g_free(db->path);
    g_free(db);
}

/* =========================================================================
 * folders
 * ========================================================================= */

gint64
on_db_folder_create(OnDatabase *db, gint64 parent_id, const gchar *name)
{
    sqlite3_stmt *stmt = prepare(db,
        "INSERT INTO folders (parent_id, name, sort_order) VALUES (?, ?, "
        "  COALESCE((SELECT MAX(sort_order)+1 FROM folders "
        "            WHERE parent_id IS ?), 0))");
    if (stmt == NULL)
        return 0;

    bind_id_or_null(stmt, 1, parent_id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    bind_id_or_null(stmt, 3, parent_id);

    gint64 new_id = 0;                   /* id of the inserted row          */
    if (sqlite3_step(stmt) == SQLITE_DONE)
        new_id = sqlite3_last_insert_rowid(db->handle);
    else
        g_warning("db: folder_create: %s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(stmt);
    return new_id;
}

gboolean
on_db_folder_rename(OnDatabase *db, gint64 id, const gchar *name)
{
    sqlite3_stmt *stmt = prepare(db, "UPDATE folders SET name=? WHERE id=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, id);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

gboolean
on_db_folder_delete(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db, "DELETE FROM folders WHERE id=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, id);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (ok)
        exec_simple(db,
            "DELETE FROM tags WHERE id NOT IN (SELECT tag_id FROM note_tags)");
    return ok;
}

/* run_folder_query() — collect every row of `stmt` (columns: id, parent,
 * name, sort_order) into a list of OnFolder* and finalize it.               */
static GList *
run_folder_query(sqlite3_stmt *stmt)
{
    GList *out = NULL;                   /* accumulated OnFolder* rows      */
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OnFolder *f  = g_new0(OnFolder, 1);
        f->id   = sqlite3_column_int64(stmt, 0);
        f->name = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
        out = g_list_prepend(out, f);
    }
    sqlite3_finalize(stmt);
    return g_list_reverse(out);
}

GList *
on_db_folder_list(OnDatabase *db, gint64 parent_id)
{
    /* trashed=0: a directly-trashed folder disappears from the normal
     * tree (it lives under the Trash section); its untouched descendants
     * never get listed because nothing recurses into it.                    */
    sqlite3_stmt *stmt = prepare(db,
        "SELECT id, COALESCE(parent_id,0), name, sort_order FROM folders "
        "WHERE parent_id IS ? AND trashed=0 "
        "ORDER BY sort_order, name COLLATE NOCASE");
    if (stmt == NULL)
        return NULL;
    bind_id_or_null(stmt, 1, parent_id);
    return run_folder_query(stmt);
}

/* free_folder() — GDestroyNotify for one OnFolder.                          */
static void
free_folder(gpointer data)
{
    OnFolder *f = data;
    g_free(f->name);
    g_free(f);
}

void
on_db_folder_list_free(GList *folders)
{
    g_list_free_full(folders, free_folder);
}

/* =========================================================================
 * notes
 * ========================================================================= */

gint64
on_db_note_create(OnDatabase *db, gint64 folder_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "INSERT INTO notes (folder_id, title, sort_order, created_at, "
        "                   updated_at) "
        "VALUES (?, 'New Note', "
        "  COALESCE((SELECT MAX(sort_order)+1 FROM notes "
        "            WHERE folder_id IS ?), 0), "
        "  strftime('%s','now'), strftime('%s','now'))");
    if (stmt == NULL)
        return 0;
    bind_id_or_null(stmt, 1, folder_id);
    bind_id_or_null(stmt, 2, folder_id);

    gint64 new_id = 0;                   /* id of the inserted note         */
    if (sqlite3_step(stmt) == SQLITE_DONE)
        new_id = sqlite3_last_insert_rowid(db->handle);
    else
        g_warning("db: note_create: %s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(stmt);
    return new_id;
}

gboolean
on_db_note_delete(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db, "DELETE FROM notes WHERE id=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, id);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (ok)
        exec_simple(db,
            "DELETE FROM tags WHERE id NOT IN (SELECT tag_id FROM note_tags)");
    return ok;
}

gboolean
on_db_notes_delete(OnDatabase *db, const gint64 *ids, gsize n)
{
    if (n == 0)
        return TRUE;

    /* One transaction for the lot — autocommit would fsync per note —
     * and the orphan-tag prune runs once at the end instead of per note.   */
    if (!exec_simple(db, "BEGIN IMMEDIATE"))
        return FALSE;

    sqlite3_stmt *stmt = prepare(db, "DELETE FROM notes WHERE id=?");
    gboolean ok = stmt != NULL;      /* every delete succeeded so far?      */
    for (gsize i = 0; ok && i < n; i++) {
        sqlite3_bind_int64(stmt, 1, ids[i]);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_reset(stmt);
    }
    if (stmt != NULL)
        sqlite3_finalize(stmt);

    if (ok)
        ok = exec_simple(db,
            "DELETE FROM tags WHERE id NOT IN (SELECT tag_id FROM note_tags)");
    exec_simple(db, ok ? "COMMIT" : "ROLLBACK");
    return ok;
}

gboolean
on_db_note_move(OnDatabase *db, gint64 id, gint64 folder_id)
{
    /* trashed=0: dragging a note out of the Trash into a folder is a
     * restore — a moved note is always meant to be visible where it
     * lands.                                                                */
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE notes SET folder_id=?, trashed=0, sort_order="
        "  COALESCE((SELECT MAX(sort_order)+1 FROM notes "
        "            WHERE folder_id IS ?), 0) "
        "WHERE id=?");
    if (stmt == NULL)
        return FALSE;
    bind_id_or_null(stmt, 1, folder_id);
    bind_id_or_null(stmt, 2, folder_id);
    sqlite3_bind_int64(stmt, 3, id);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

gboolean
on_db_note_save(OnDatabase *db, gint64 id, const gchar *title,
                const guint8 *content, gsize len, const gchar *body_text)
{
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE notes SET title=?, content=?, body_text=?, "
        "updated_at=strftime('%s','now') WHERE id=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_text(stmt, 1, title, -1, SQLITE_TRANSIENT);
    if (content != NULL && len > 0)
        sqlite3_bind_blob(stmt, 2, content, (int)len, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 2);
    if (body_text != NULL)
        sqlite3_bind_text(stmt, 3, body_text, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 3);
    sqlite3_bind_int64(stmt, 4, id);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok)
        g_warning("db: note_save: %s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(stmt);
    return ok;
}

gchar *
on_db_note_body_text(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT body_text FROM notes WHERE id=?");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, id);
    gchar *text = NULL;              /* cached text, NULL if unfilled       */
    if (sqlite3_step(stmt) == SQLITE_ROW &&
        sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        text = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return text;
}

gboolean
on_db_note_set_body_text(OnDatabase *db, gint64 id, const gchar *body_text)
{
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE notes SET body_text=? WHERE id=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_text(stmt, 1, body_text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, id);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

gboolean
on_db_note_set_updated_at(OnDatabase *db, gint64 id, gint64 ts)
{
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE notes SET updated_at=? WHERE id=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_int64(stmt, 2, id);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

guint8 *
on_db_note_load(OnDatabase *db, gint64 id, gsize *out_len)
{
    *out_len = 0;
    sqlite3_stmt *stmt = prepare(db, "SELECT content FROM notes WHERE id=?");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, id);

    guint8 *copy = NULL;                 /* caller-owned copy of the blob   */
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(stmt, 0);
        int         n    = sqlite3_column_bytes(stmt, 0);
        if (blob != NULL && n > 0) {
            copy = g_memdup2(blob, (gsize)n);
            *out_len = (gsize)n;
        }
    }
    sqlite3_finalize(stmt);
    return copy;
}

/* Column list shared by every note-metadata query, matching
 * meta_from_row()'s expectations.                                           */
#define NOTE_META_COLS \
    "id, COALESCE(folder_id,0), title, sort_order, updated_at, pinned"

/* ---------------------------------------------------------------------------
 * meta_from_row() — build one OnNoteMeta from the current result row of a
 * statement selecting NOTE_META_COLS in that order.
 * ------------------------------------------------------------------------- */
static OnNoteMeta *
meta_from_row(sqlite3_stmt *stmt)
{
    OnNoteMeta *m = g_new0(OnNoteMeta, 1);
    m->id         = sqlite3_column_int64(stmt, 0);
    m->folder_id  = sqlite3_column_int64(stmt, 1);
    m->title      = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
    m->updated_at = sqlite3_column_int64(stmt, 4);
    m->pinned     = sqlite3_column_int(stmt, 5) != 0;
    return m;
}

OnNoteMeta *
on_db_note_get(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE id=?");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, id);

    OnNoteMeta *meta = NULL;             /* result, NULL if no such row     */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        meta = meta_from_row(stmt);
    sqlite3_finalize(stmt);
    return meta;
}

/* run_meta_query() — collect every row of `stmt` into a list of
 * OnNoteMeta* (columns must match meta_from_row) and finalize it.           */
static GList *
run_meta_query(sqlite3_stmt *stmt)
{
    GList *out = NULL;                   /* accumulated OnNoteMeta* rows    */
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out = g_list_prepend(out, meta_from_row(stmt));
    sqlite3_finalize(stmt);
    return g_list_reverse(out);
}

GList *
on_db_note_list(OnDatabase *db, gint64 folder_id)
{
    /* trashed=0 keeps directly-trashed notes out; when the folder itself
     * sits in the Trash (browsing it from the Trash section) its regular
     * notes carry no flag and still list here.                              */
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE folder_id IS ? AND trashed=0 "
        "ORDER BY sort_order, updated_at DESC");
    if (stmt == NULL)
        return NULL;
    bind_id_or_null(stmt, 1, folder_id);
    return run_meta_query(stmt);
}

GList *
on_db_note_list_all(OnDatabase *db, gboolean include_trash)
{
    sqlite3_stmt *stmt = prepare(db, include_trash
        ? "SELECT " NOTE_META_COLS " "
          "FROM notes ORDER BY folder_id, sort_order"
        : "SELECT " NOTE_META_COLS " "
          "FROM notes WHERE " NOTE_VISIBLE_SQL " "
          "ORDER BY folder_id, sort_order");
    if (stmt == NULL)
        return NULL;
    return run_meta_query(stmt);
}

GList *
on_db_note_list_recent(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE " NOTE_VISIBLE_SQL " "
        "ORDER BY updated_at DESC");
    if (stmt == NULL)
        return NULL;
    return run_meta_query(stmt);
}

gboolean
on_db_note_set_pinned(OnDatabase *db, gint64 id, gboolean pinned)
{
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE notes SET pinned=? WHERE id=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int(stmt, 1, pinned ? 1 : 0);
    sqlite3_bind_int64(stmt, 2, id);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

GList *
on_db_note_list_pinned(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE pinned=1 AND " NOTE_VISIBLE_SQL " "
        "ORDER BY updated_at DESC");
    if (stmt == NULL)
        return NULL;
    return run_meta_query(stmt);
}

gint
on_db_note_count_pinned(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT COUNT(*) FROM notes WHERE pinned=1 AND " NOTE_VISIBLE_SQL);
    if (stmt == NULL)
        return 0;
    gint count = 0;                  /* number of pinned notes              */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

gboolean
on_db_note_reorder(OnDatabase *db, const gint64 *note_ids, gsize n)
{
    if (!exec_simple(db, "BEGIN"))
        return FALSE;
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE notes SET sort_order=? WHERE id=?");
    if (stmt == NULL) {
        exec_simple(db, "ROLLBACK");
        return FALSE;
    }

    gboolean ok = TRUE;                  /* set FALSE on first failure      */
    for (gsize i = 0; i < n && ok; i++) {
        sqlite3_bind_int(stmt, 1, (int)i);
        sqlite3_bind_int64(stmt, 2, note_ids[i]);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    exec_simple(db, ok ? "COMMIT" : "ROLLBACK");
    return ok;
}

void
on_db_note_meta_free(OnNoteMeta *meta)
{
    if (meta == NULL)
        return;
    g_free(meta->title);
    g_free(meta);
}

void
on_db_note_list_free(GList *notes)
{
    g_list_free_full(notes, (GDestroyNotify)on_db_note_meta_free);
}

/* =========================================================================
 * tags
 * ========================================================================= */

/* on_db_tag_get_or_create() — look up tag `name`, creating it if
 * missing.  Returns the tag id, or 0 on failure.                            */
static gint64
on_db_tag_get_or_create(OnDatabase *db, const gchar *name)
{
    /* Try the fast path first: the tag already exists.                     */
    sqlite3_stmt *stmt = prepare(db, "SELECT id FROM tags WHERE name=?");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    gint64 tag_id = 0;                   /* resulting tag id                */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        tag_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    if (tag_id != 0)
        return tag_id;

    /* Not found: insert it.                                                */
    stmt = prepare(db, "INSERT INTO tags (name) VALUES (?)");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_DONE)
        tag_id = sqlite3_last_insert_rowid(db->handle);
    sqlite3_finalize(stmt);
    return tag_id;
}

gint64
on_db_tag_find(OnDatabase *db, const gchar *name)
{
    sqlite3_stmt *stmt = prepare(db, "SELECT id FROM tags WHERE name=?");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    gint64 tag_id = 0;               /* found id, or 0                      */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        tag_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return tag_id;
}

gboolean
on_db_tag_delete(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db, "DELETE FROM tags WHERE id=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, id);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

GList *
on_db_tag_list(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT id, name FROM tags ORDER BY name COLLATE NOCASE");
    if (stmt == NULL)
        return NULL;

    GList *out = NULL;                   /* accumulated OnTag* rows         */
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OnTag *t = g_new0(OnTag, 1);
        t->id   = sqlite3_column_int64(stmt, 0);
        t->name = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
        out = g_list_prepend(out, t);
    }
    sqlite3_finalize(stmt);
    return g_list_reverse(out);
}

/* free_tag() — GDestroyNotify for one OnTag.                                */
static void
free_tag(gpointer data)
{
    OnTag *t = data;
    g_free(t->name);
    g_free(t);
}

void
on_db_tag_list_free(GList *tags)
{
    g_list_free_full(tags, free_tag);
}

gboolean
on_db_note_set_tags(OnDatabase *db, gint64 note_id, GList *tag_names)
{
    if (!exec_simple(db, "BEGIN"))
        return FALSE;

    /* Drop the note's old tag links, then re-add the current set.          */
    sqlite3_stmt *stmt = prepare(db,
        "DELETE FROM note_tags WHERE note_id=?");
    gboolean ok = stmt != NULL;          /* overall success flag            */
    if (ok) {
        sqlite3_bind_int64(stmt, 1, note_id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }

    for (GList *l = tag_names; ok && l != NULL; l = l->next) {
        const gchar *name = l->data;     /* one tag name, no leading '#'    */
        gint64 tag_id = on_db_tag_get_or_create(db, name);
        if (tag_id == 0) {
            ok = FALSE;
            break;
        }
        stmt = prepare(db,
            "INSERT OR IGNORE INTO note_tags (note_id, tag_id) VALUES (?,?)");
        if (stmt == NULL) {
            ok = FALSE;
            break;
        }
        sqlite3_bind_int64(stmt, 1, note_id);
        sqlite3_bind_int64(stmt, 2, tag_id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }

    /* Remove tags that no longer label any note so the tag list in the
     * library window stays tidy.                                           */
    if (ok)
        ok = exec_simple(db,
            "DELETE FROM tags WHERE id NOT IN (SELECT tag_id FROM note_tags)");

    exec_simple(db, ok ? "COMMIT" : "ROLLBACK");
    return ok;
}

GList *
on_db_notes_by_tag(OnDatabase *db, gint64 tag_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT n.id, COALESCE(n.folder_id,0), n.title, n.sort_order, "
        "       n.updated_at, n.pinned "
        "FROM notes n JOIN note_tags nt ON nt.note_id = n.id "
        "WHERE nt.tag_id=? AND n.trashed=0 AND (n.folder_id IS NULL OR "
        "      n.folder_id NOT IN (SELECT id FROM trash_folder_ids)) "
        "ORDER BY n.updated_at DESC");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, tag_id);
    return run_meta_query(stmt);
}

/* =========================================================================
 * trash
 * ========================================================================= */

gboolean
on_db_notes_trash(OnDatabase *db, const gint64 *ids, gsize n)
{
    if (n == 0)
        return TRUE;

    /* One transaction for the lot, like on_db_notes_delete.                 */
    if (!exec_simple(db, "BEGIN IMMEDIATE"))
        return FALSE;

    sqlite3_stmt *stmt = prepare(db, "UPDATE notes SET trashed=1 WHERE id=?");
    gboolean ok = stmt != NULL;      /* every update succeeded so far?      */
    for (gsize i = 0; ok && i < n; i++) {
        sqlite3_bind_int64(stmt, 1, ids[i]);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_reset(stmt);
    }
    if (stmt != NULL)
        sqlite3_finalize(stmt);
    exec_simple(db, ok ? "COMMIT" : "ROLLBACK");
    return ok;
}

gboolean
on_db_note_restore(OnDatabase *db, gint64 id)
{
    /* The stored folder_id IS the "where it was deleted from"; it only
     * moves to the top level when that folder is itself in the Trash.       */
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE notes SET trashed=0, folder_id="
        "  CASE WHEN folder_id IN (SELECT id FROM trash_folder_ids) "
        "       THEN NULL ELSE folder_id END "
        "WHERE id=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, id);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

gboolean
on_db_folder_trash(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE folders SET trashed=1 WHERE id=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, id);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

gboolean
on_db_folder_restore(OnDatabase *db, gint64 id)
{
    /* Re-parent to the top level when the original parent is still in
     * the Trash (the CASE subquery sees the pre-update flags; the folder
     * can never be its own parent, so clearing its flag in the same
     * statement is safe).                                                   */
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE folders SET trashed=0, parent_id="
        "  CASE WHEN parent_id IN (SELECT id FROM trash_folder_ids "
        "                          WHERE id<>?) "
        "       THEN NULL ELSE parent_id END "
        "WHERE id=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int64(stmt, 2, id);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

GList *
on_db_folder_list_trashed(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT id, COALESCE(parent_id,0), name, sort_order FROM folders "
        "WHERE trashed=1 ORDER BY name COLLATE NOCASE");
    if (stmt == NULL)
        return NULL;
    return run_folder_query(stmt);
}

GList *
on_db_note_list_trashed(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE trashed=1 ORDER BY updated_at DESC");
    if (stmt == NULL)
        return NULL;
    return run_meta_query(stmt);
}

gint
on_db_trash_count(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT (SELECT COUNT(*) FROM notes   WHERE trashed=1) + "
        "       (SELECT COUNT(*) FROM folders WHERE trashed=1)");
    if (stmt == NULL)
        return 0;
    gint count = 0;                  /* notes + folders directly in Trash   */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

/* run_id_query() — collect a one-column id result set into a GArray of
 * gint64 and finalize the statement.                                        */
static GArray *
run_id_query(sqlite3_stmt *stmt)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    if (stmt != NULL) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            gint64 id = sqlite3_column_int64(stmt, 0);
            g_array_append_val(ids, id);
        }
        sqlite3_finalize(stmt);
    }
    return ids;
}

GArray *
on_db_trash_note_ids(OnDatabase *db)
{
    return run_id_query(prepare(db,
        "SELECT id FROM notes WHERE trashed=1 OR "
        "folder_id IN (SELECT id FROM trash_folder_ids)"));
}

GArray *
on_db_folder_note_ids(OnDatabase *db, gint64 folder_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "WITH RECURSIVE sub(id) AS ("
        "  SELECT ? UNION "
        "  SELECT f.id FROM folders f JOIN sub ON f.parent_id = sub.id"
        ") SELECT id FROM notes WHERE folder_id IN (SELECT id FROM sub)");
    if (stmt != NULL)
        sqlite3_bind_int64(stmt, 1, folder_id);
    return run_id_query(stmt);
}

gboolean
on_db_trash_empty(OnDatabase *db)
{
    if (!exec_simple(db, "BEGIN IMMEDIATE"))
        return FALSE;

    /* The explicit notes delete covers directly-trashed notes; deleting
     * the flagged folders cascades through their subtrees (descendant
     * folders and every note inside).                                       */
    gboolean ok =
        exec_simple(db,
            "DELETE FROM notes WHERE trashed=1 OR "
            "folder_id IN (SELECT id FROM trash_folder_ids)") &&
        exec_simple(db, "DELETE FROM folders WHERE trashed=1") &&
        exec_simple(db,
            "DELETE FROM tags WHERE id NOT IN (SELECT tag_id FROM note_tags)");
    exec_simple(db, ok ? "COMMIT" : "ROLLBACK");
    return ok;
}

/* =========================================================================
 * counts
 * ========================================================================= */

gint
on_db_note_count_visible(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT COUNT(*) FROM notes WHERE " NOTE_VISIBLE_SQL);
    if (stmt == NULL)
        return 0;
    gint count = 0;                  /* every note outside the Trash        */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

gint
on_db_note_count_for_folder(OnDatabase *db, gint64 folder_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT COUNT(*) FROM notes WHERE folder_id IS ? AND trashed=0");
    if (stmt == NULL)
        return 0;
    bind_id_or_null(stmt, 1, folder_id);
    gint count = 0;                  /* number of notes found               */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

gint
on_db_note_count_for_tag(OnDatabase *db, gint64 tag_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT COUNT(*) FROM note_tags WHERE tag_id=?");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_int64(stmt, 1, tag_id);
    gint count = 0;                  /* number of tagged notes              */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

/* count_all() — COUNT(*) of one table (by trusted internal name).           */
static gint
count_all(OnDatabase *db, const gchar *table)
{
    gchar *sql = g_strdup_printf("SELECT COUNT(*) FROM %s", table);
    sqlite3_stmt *stmt = prepare(db, sql);
    g_free(sql);
    if (stmt == NULL)
        return 0;
    gint count = 0;                  /* the table's row count               */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

void
on_db_totals(OnDatabase *db, gint *notes, gint *folders, gint *tags)
{
    if (notes != NULL)
        *notes = count_all(db, "notes");
    if (folders != NULL)
        *folders = count_all(db, "folders");
    if (tags != NULL)
        *tags = count_all(db, "tags");
}

/* ---------------------------------------------------------------------------
 * count_map_from_query() — run a two-column (id, count) query into a
 * gint64* → GINT_TO_POINTER(count) hash table.
 * ------------------------------------------------------------------------- */
static GHashTable *
count_map_from_query(OnDatabase *db, const gchar *sql)
{
    GHashTable *map = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, NULL);
    sqlite3_stmt *stmt = prepare(db, sql);
    if (stmt != NULL) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            gint64 *key = g_new(gint64, 1);
            *key = sqlite3_column_int64(stmt, 0);
            g_hash_table_insert(map, key,
                                GINT_TO_POINTER(
                                    sqlite3_column_int(stmt, 1)));
        }
        sqlite3_finalize(stmt);
    }
    return map;
}

GHashTable *
on_db_note_count_map(OnDatabase *db)
{
    /* trashed=0 only (no subtree filter): folders inside the Trash keep
     * their counts — the map also labels the Trash section's folder rows.   */
    return count_map_from_query(db,
        "SELECT COALESCE(folder_id,0), COUNT(*) FROM notes WHERE trashed=0 "
        "GROUP BY COALESCE(folder_id,0)");
}

GHashTable *
on_db_tag_count_map(OnDatabase *db)
{
    return count_map_from_query(db,
        "SELECT nt.tag_id, COUNT(*) FROM note_tags nt "
        "JOIN notes n ON n.id = nt.note_id "
        "WHERE n.trashed=0 AND (n.folder_id IS NULL OR "
        "      n.folder_id NOT IN (SELECT id FROM trash_folder_ids)) "
        "GROUP BY nt.tag_id");
}

GHashTable *
on_db_note_body_map(OnDatabase *db)
{
    GHashTable *map = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, g_free);
    sqlite3_stmt *stmt = prepare(db,
        "SELECT id, body_text FROM notes WHERE body_text IS NOT NULL");
    if (stmt == NULL)
        return map;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        gint64 *key = g_new(gint64, 1);
        *key = sqlite3_column_int64(stmt, 0);
        g_hash_table_insert(map, key,
            g_strdup((const gchar *)sqlite3_column_text(stmt, 1)));
    }
    sqlite3_finalize(stmt);
    return map;
}

/* =========================================================================
 * settings
 * ========================================================================= */

gchar *
on_db_setting_get(OnDatabase *db, const gchar *key)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT value FROM settings WHERE key=?");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    gchar *value = NULL;             /* stored value, NULL if unset         */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        value = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return value;
}

gboolean
on_db_setting_delete(OnDatabase *db, const gchar *key)
{
    sqlite3_stmt *stmt = prepare(db, "DELETE FROM settings WHERE key=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

/* =========================================================================
 * utilities
 * ========================================================================= */

gchar *
on_db_folder_path(OnDatabase *db, gint64 folder_id)
{
    /* Walk from the folder up to the root, prepending each name.  The
     * depth cap keeps a corrupt parent_id cycle (hand-edited db) from
     * spinning forever — no real tree is anywhere near that deep.          */
    GString *path = g_string_new("");    /* built back-to-front             */
    gint64   cur  = folder_id;           /* current folder in the walk      */
    gint     depth = 0;                  /* levels walked so far            */

    while (cur > 0 && depth++ < 128) {
        sqlite3_stmt *stmt = prepare(db,
            "SELECT name, COALESCE(parent_id,0) FROM folders WHERE id=?");
        if (stmt == NULL)
            break;
        sqlite3_bind_int64(stmt, 1, cur);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const gchar *name = (const gchar *)sqlite3_column_text(stmt, 0);
            /* Prepend "name/" to whatever we have so far.                  */
            g_string_prepend(path, "/");
            g_string_prepend(path, name);
            cur = sqlite3_column_int64(stmt, 1);
        } else {
            cur = 0;
        }
        sqlite3_finalize(stmt);
    }

    /* Trim the trailing slash left by the loop, if any.                    */
    if (path->len > 0 && path->str[path->len - 1] == '/')
        g_string_truncate(path, path->len - 1);
    return g_string_free(path, FALSE);
}
