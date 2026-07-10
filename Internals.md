# Blue Notes — Internals

How Blue Notes is put together: the source layout, the database schema,
and the BNBF note format. For everyday use see the
[User Guide](User_Guide.md); for build instructions see the
[README](README.md).

## Code layout

| File                      | Purpose                                             |
|---------------------------|-----------------------------------------------------|
| `src/main.c`              | GtkApplication entry point; settings/config loading |
| `src/app.[ch]`            | Shared `OnApp` context, icon loading, toolbar styles, DB switching/restore |
| `src/cli.[ch]`            | Headless noun-verb command-line interface           |
| `src/db.[ch]`             | SQLite layer: folders, notes, tags, backup          |
| `src/serialize.[ch]`      | BNBF binary format ⇄ GtkTextBuffer conversion       |
| `src/editor_window.[ch]`  | WYSIWYG editor: formatting, images, tables, tasks, code blocks, find-in-note |
| `src/library_window.[ch]` | Sidebar, list & grid views, drag & drop, menus, About |
| `src/search_window.[ch]`  | Cross-note search window                            |
| `src/settings_window.[ch]`| The Settings window                                 |
| `src/export.[ch]`         | HTML and Markdown exporters                         |
| `icons/`                  | Bundled PNG icons + app logo (see its README)       |
| `tools/import-apple-notes.sh` | Apple Notes migration script                    |

## Database format

Everything lives in one ordinary SQLite file (see *Storage* in the
[User Guide](User_Guide.md)), so any standard SQLite tool can read it:

```sql
CREATE TABLE folders (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  parent_id  INTEGER REFERENCES folders(id) ON DELETE CASCADE,
  name       TEXT NOT NULL,
  sort_order INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE notes (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  folder_id  INTEGER REFERENCES folders(id) ON DELETE CASCADE,
  title      TEXT NOT NULL DEFAULT 'New Note',
  content    BLOB,             -- the note itself, in BNBF (see below)
  sort_order INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL, -- UNIX seconds
  updated_at INTEGER NOT NULL, -- UNIX seconds
  pinned     INTEGER NOT NULL DEFAULT 0,
  body_text  TEXT              -- plain-text cache used by search
);

CREATE TABLE tags      (id INTEGER PRIMARY KEY AUTOINCREMENT,
                        name TEXT NOT NULL UNIQUE);
CREATE TABLE note_tags (note_id INTEGER NOT NULL REFERENCES notes(id)
                          ON DELETE CASCADE,
                        tag_id  INTEGER NOT NULL REFERENCES tags(id)
                          ON DELETE CASCADE,
                        PRIMARY KEY (note_id, tag_id));
CREATE INDEX idx_notes_folder   ON notes(folder_id);
CREATE INDEX idx_folders_parent ON folders(parent_id);
CREATE INDEX idx_note_tags_tag  ON note_tags(tag_id);
```

Semantics worth knowing when querying directly:

- A `NULL` `folder_id` means the note sits at the top level; a `NULL`
  `parent_id` means a top-level folder. Deleting a folder cascades to
  its subfolders and notes.
- `body_text` is a derived plain-text copy of `content`, kept for fast
  searching. It may be `NULL` on rows written by older versions (the app
  backfills it on first search). Treat it as read-only convenience — the
  `content` blob is authoritative.
- `tags.name` is stored without the leading `#`.

Example — every note with its full folder path:

```sql
WITH RECURSIVE fpath(id, path) AS (
  SELECT id, '/' || name FROM folders WHERE parent_id IS NULL
  UNION ALL
  SELECT f.id, p.path || '/' || f.name
  FROM folders f JOIN fpath p ON f.parent_id = p.id
)
SELECT COALESCE(p.path, '') || '/' || n.title AS note,
       datetime(n.updated_at, 'unixepoch', 'localtime') AS modified
FROM notes n LEFT JOIN fpath p ON n.folder_id = p.id
ORDER BY 1;
```

`content` is BNBF ("Blue Notes Binary Format"), a simple little-endian
record stream — 4-byte magic `BNBF`, a `u32` version
(currently 5), then typed records until a `0x00` end marker:

| Record | Type byte | Payload |
|--------|-----------|---------|
| TEXT   | `0x01`    | `u32` format flags, `u32` byte length, UTF-8 bytes |
| IMAGE  | `0x02`    | `u32` display width (v2+), `u32` PNG length, PNG bytes at full resolution |
| TABLE  | `0x03`    | `u32` flags (v4+; bit 0 = header row), `u32` rows, `u32` cols, then rows×cols of (`u32` length, UTF-8 bytes) |
| CHECK  | `0x04`    | `u8` state (0 unchecked / 1 checked) (v5+) |

TEXT flag bits mark bold, italic, headings, code blocks, list kinds,
inline `#tags`, and so on. The authoritative spec — including every flag
bit and all version differences — is the header comment in
`src/serialize.h`. Prefer exporting (`export-md` / `export-html`) over
parsing BNBF yourself when possible.

Two practical cautions: the app sets a 5-second busy timeout, so brief
external readers coexist fine, but long write transactions from other
tools will stall it; and back up with `blue_notes backup FILE.db`
(SQLite's online backup API) rather than copying the file while the app
is running.
