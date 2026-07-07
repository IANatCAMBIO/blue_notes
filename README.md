# Blue Notes

An Apple Notes–style notes application written in plain C with GTK3 and
SQLite. Notes are rich text (WYSIWYG) with inline images, code blocks,
tables, task lists and `#tags`, organized into nested folders, and
exportable to HTML or Markdown — from the GUI or the command line.

![Blue Notes](Screenshot.png)

## Features

### Library window

- Nested folder tree with per-folder note counts, plus a tag list with
  per-tag counts, in the sidebar. The sidebar toolbar holds New Folder,
  Delete Folder and Search; renaming lives in the folder's right-click
  menu (New Subfolder, Rename, Delete, and "Search Here…" — a search
  pre-scoped to that folder or tag).
- Notes display as a list or as a grid of square thumbnail cards (first
  image + text preview, title underneath) — switch via *View → Notes as
  List / Notes as Grid*. List rows alternate white/light-blue; clicking
  the Title header sorts alphabetically, the Modified header sorts
  most-recent-first (drag-reordering works while unsorted).
- Multi-select (Cmd/Shift-click, rubber-band in grid) for bulk actions:
  drag any selected note onto a sidebar folder to move the whole
  selection, or use the right-click menu — Open, Export as
  HTML/Markdown, Delete.
- **Ctrl/Cmd+N** creates a note in the selected folder. The vinyl logo
  at the toolbar's right edge opens the About dialog (program info plus
  live database statistics).

### Search

The Search button (or a folder's "Search Here…") opens a search window
scanning titles and full note text. Scope radios choose **All Notes** or
**Selected Folder/Tag** — the latter resolves against whatever is
selected in the library at the moment you press Search. Case-sensitive
and regular-expression modes are available; double-click a result to
open it. Results show each note's full path, including any nested
folders it lives in.

Note text is cached in a `body_text` column on save, so searches never
decode images or parse the binary blobs; notes saved by older versions
are backfilled automatically the first time they are searched.

### Editor windows

Every note opens in its own window with a standard titlebar (no GNOME
header bars anywhere). The first line of the note becomes its title.

- Inline styles — **bold** (Ctrl/Cmd+B), *italic* (Ctrl/Cmd+I),
  underline (Ctrl/Cmd+U), strikethrough — plus two heading levels,
  bulleted and numbered lists. Paragraph-style buttons toggle: clicking
  one on lines that already have that style reverts them to body text.
- **Task lists** — native checkboxes at the start of each item (click to
  toggle; the pointer becomes a hand over them). Enter continues the
  list unchecked; Enter on an empty item ends it.
- **Tables** — the Table button inserts a 3×3 grid of cells that grow
  with their content (multiline supported). Right-click any cell to add
  or remove rows/columns, toggle a bold **header row**, or delete the
  table.
- **Code blocks** — monospaced, shaded blocks with a floating copy
  button in the upper-right corner; optional per-block line numbers in a
  slim painted gutter (numbers are never part of selection/copy).
- **Inline images** — paste from the clipboard (screenshots included) or
  insert from a file. Stored at full resolution, displayed as sharp
  HiDPI thumbnails; right-click for Copy Image, Open Full Size, or
  Display Full Size / as Thumbnail.
- **Tags** — type `#` and keep typing; a popup suggests existing tags
  (arrows + Enter to pick), space ends the tag, Escape cancels. Tags
  appear in the sidebar and act like folders.
- **Find in note** — the search box at the toolbar's right (Ctrl/Cmd+F)
  highlights every match as you type; Enter or the ↑/↓ buttons jump
  between matches with wrap-around.
- **Autosave** — edits persist about a second after you stop typing, and
  again when the window closes.

### Settings (*File → Settings…*)

- **Appearance** — toolbar button style (text / icons / icons above
  text), set separately for library and editor windows (also available
  by right-clicking any toolbar); note counts next to folders and tags;
  and — when built with gtk-mac-integration — a native macOS menu bar
  option.
- **Editor** — show/hide the code-block copy button and code-block line
  numbers; auto-style the first line of a new note as Heading 1; a
  compact toolbar that collapses the style and list buttons into menus;
  show/hide the touch drag handles under selected text; and a custom
  image-viewer program for opening images.
- **Database** — store the database in a custom folder (see Storage)
  and toggle the startup integrity check.

All changes apply live and persist (in `blue_notes.ini` next to the
binary). Toolbar icons are PNGs bundled in `icons/` — replaceable by
dropping in files, see `icons/README.md`.

### Export

*File → Export All as HTML…/Markdown…* writes every note to a chosen
directory, mirroring the folder hierarchy; single notes export from
their right-click menu. HTML embeds images as data URIs and renders
tables, task checkboxes and code blocks; Markdown writes sidecar PNGs,
pipe tables and `- [ ]` task items.

## Storage

Everything lives in a single SQLite database:

- `~/.local/share/blue_notes/blue_notes.db` by default (GLib's user-data
  directory). *File → Settings… → Database* can point the app at a custom
  folder instead — e.g. a shared drive used by two machines (never open
  it from both at once). The choice is stored in `blue_notes.ini` in
  the same directory as the binary (created on first launch from
  `blue_notes.ini.defaults`). If the configured database cannot be
  opened at startup, the app reports the error and exits — it never
  silently opens a different database.
- *File → Back Up Database…* snapshots the live database to a file;
  *File → Restore Database…* replaces the current data with a backup
  (keeping the old file as `blue_notes.db.pre-restore`).
- **CLI and GUI cooperate over a socket** — while the GUI is running,
  `blue_notes` command-line invocations are forwarded to it over a unix
  socket instead of opening the database a second time, so the two never
  write concurrently. If the database file changed on disk since the
  last clean exit (shared folder, crash), startup offers **Open Anyway**
  or **Run Integrity Check** before proceeding (the check can be turned
  off in Settings).

Note content is stored as a compact binary blob ("BNBF", currently
version 5) holding styled text runs, PNG image records, tables and task
checkboxes — see `src/serialize.h` for the format. Older versions load
transparently.

## Building

Requirements: a C compiler, GTK3 and SQLite3 development files, and
pkg-config. librsvg is optional — the toolbar icons are PNGs; it only
sharpens the few remaining SVG icons (the warning-dialog icon and the
bundled symbolic arrows), which otherwise fall back to text glyphs and
GTK's built-in raster icons.

macOS (MacPorts):

```sh
sudo port install pkgconf gtk3 +quartz librsvg
sudo port install gtk-osx-application-gtk3   # optional: native menu bar
make
make run
```

Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libgtk-3-dev libsqlite3-dev \
                 librsvg2-common
make
make run
```

The Makefile auto-detects `gtk-mac-integration-gtk3`; rebuild from clean
(`make clean && make`) after installing it.

## Command-line automation

A recognized subcommand runs headless against the same database (including
a configured shared location) and exits — no windows:

```
blue_notes tag list                         blue_notes tag delete NAME
blue_notes folder list                      blue_notes folder add PATH
blue_notes folder delete PATH               blue_notes note list [PATH|--all]
blue_notes note new [--folder PATH] TEXT|-  blue_notes note delete ID [ID...]
blue_notes note move ID [ID...] PATH        blue_notes note add-image ID FILE
blue_notes note set-modified ID TIMESTAMP   blue_notes backup FILE.db
blue_notes export-md DIR                    blue_notes export-html DIR
```

Folders are addressed by path (`"Work/Projects"`, created like `mkdir -p`
by `folder add`); notes by the ids `note list` prints. `note new` takes
its content from the argument or stdin (`-`); the first line becomes the
title. Output is tab-separated for easy scripting; exit codes: 0 success,
1 usage, 2 failure. `blue_notes help` shows the full reference.

## Migrating from Apple Notes

```sh
tools/import-apple-notes.sh
```

Exports every folder and note from Notes.app (macOS will ask once for
permission to control Notes), converts the bodies to text, saves image
attachments, and imports everything — hierarchy included — under an
"Apple Notes Import" folder via the CLI. Each note keeps its original
last-edited date from Apple Notes. Images land at the end of each note
(Notes' scripting interface doesn't reveal their inline position);
non-image attachments (PDFs, scans) are skipped with a count. Re-running
duplicates notes, so delete the import folder first if retrying.

## Database format

Everything lives in one ordinary SQLite file (see *Where the data lives*
above), so any standard SQLite tool can read it:

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
CREATE TABLE settings  (key TEXT PRIMARY KEY, value TEXT NOT NULL);

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
  `content` blob is the source of truth.
- `tags.name` is stored without the leading `#`.
- `settings` is legacy-only: nothing writes to it anymore (old versions
  kept UI preferences and an instance lock there; startup migrates and
  deletes any leftovers). App preferences live in `blue_notes.ini` next
  to the binary, not in the database.

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
record stream — 4-byte magic `BNBF` (blobs written before the rename
carry the legacy `ONBF` magic; readers accept both), a `u32` version
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

## Code layout

| File                      | Purpose                                             |
|---------------------------|-----------------------------------------------------|
| `src/main.c`              | GtkApplication entry point; settings/config loading |
| `src/app.[ch]`            | Shared `OnApp` context, icon loading, toolbar styles, DB switching/restore |
| `src/cli.[ch]`            | Headless noun-verb command-line interface           |
| `src/db.[ch]`             | SQLite layer: folders, notes, tags, settings, backup |
| `src/serialize.[ch]`      | BNBF binary format ⇄ GtkTextBuffer conversion       |
| `src/editor_window.[ch]`  | WYSIWYG editor: formatting, images, tables, tasks, code blocks, find-in-note |
| `src/library_window.[ch]` | Sidebar, list & grid views, drag & drop, menus, About |
| `src/search_window.[ch]`  | Cross-note search window                            |
| `src/settings_window.[ch]`| The Settings window                                 |
| `src/export.[ch]`         | HTML and Markdown exporters                         |
| `icons/`                  | Bundled PNG icons + app logo (see its README)       |
| `tools/import-apple-notes.sh` | Apple Notes migration script                    |
