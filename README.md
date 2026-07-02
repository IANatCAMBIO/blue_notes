# Orange Notes

An Apple Notes–style notes application written in plain C with GTK3 and
SQLite. Notes are rich text (WYSIWYG) with inline images, code blocks,
tables, task lists and `#tags`, organized into nested folders, and
exportable to HTML or Markdown — from the GUI or the command line.

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
- **Ctrl/Cmd+N** creates a note in the selected folder. The orange logo
  at the toolbar's right edge opens the About dialog (program info plus
  live database statistics).

### Search

The Search button (or a folder's "Search Here…") opens a search window
scanning titles and full note text. Scope radios choose **All Notes** or
**Selected Folder/Tag** — the latter resolves against whatever is
selected in the library at the moment you press Search. Case-sensitive
and regular-expression modes are available; double-click a result to
open it.

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
  by right-clicking any toolbar), and — when built with
  gtk-mac-integration — a native macOS menu bar option.
- **Editor** — show/hide the code-block copy button and code-block line
  numbers.
- **Database** — store the database in a custom folder (see Storage).

All changes apply live and persist. Toolbar icons are elementary SVGs
bundled in `icons/` — replaceable by dropping in files, see
`icons/README.md`.

### Export

*File → Export All as HTML…/Markdown…* writes every note to a chosen
directory, mirroring the folder hierarchy; single notes export from
their right-click menu. HTML embeds images as data URIs and renders
tables, task checkboxes and code blocks; Markdown writes sidecar PNGs,
pipe tables and `- [ ]` task items.

## Storage

Everything lives in a single SQLite database:

- `~/.local/share/orange-notes/notes.db` by default (GLib's user-data
  directory). *File → Settings… → Database* can point the app at a custom
  folder instead — e.g. a shared drive used by two machines (never open
  it from both at once). The choice is stored in
  `~/.config/orange-notes/config.ini`.
- *File → Back Up Database…* snapshots the live database to a file;
  *File → Restore Database…* replaces the current data with a backup
  (keeping the old file as `notes.db.pre-restore`).
- **Single-instance failsafe** — a running instance marks the database
  in use (`user@host`, pid, start time). A second instance opening the
  same database is offered **Open Read-Only** (writes are refused at the
  SQLite level) or **Override Lock** (for stale markers left by a
  crash). The marker is released on quit, window close, and SIGTERM;
  CLI commands warn when the database is marked in use.

Note content is stored as a compact binary blob ("ONBF", currently
version 5) holding styled text runs, PNG image records, tables and task
checkboxes — see `src/serialize.h` for the format. Older versions load
transparently.

## Building

Requirements: a C compiler, GTK3 and SQLite3 development files,
pkg-config, and librsvg (SVG icon rendering).

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
orange_notes tag list                         orange_notes tag delete NAME
orange_notes folder list                      orange_notes folder add PATH
orange_notes folder delete PATH               orange_notes note list [PATH|--all]
orange_notes note new [--folder PATH] TEXT|-  orange_notes note delete ID [ID...]
orange_notes note move ID [ID...] PATH        orange_notes note add-image ID FILE
orange_notes backup FILE.db                   orange_notes export-md DIR
orange_notes export-html DIR
```

Folders are addressed by path (`"Work/Projects"`, created like `mkdir -p`
by `folder add`); notes by the ids `note list` prints. `note new` takes
its content from the argument or stdin (`-`); the first line becomes the
title. Output is tab-separated for easy scripting; exit codes: 0 success,
1 usage, 2 failure. `orange_notes help` shows the full reference.

## Migrating from Apple Notes

```sh
tools/import-apple-notes.sh
```

Exports every folder and note from Notes.app (macOS will ask once for
permission to control Notes), converts the bodies to text, saves image
attachments, and imports everything — hierarchy included — under an
"Apple Notes Import" folder via the CLI. Images land at the end of each
note (Notes' scripting interface doesn't reveal their inline position);
non-image attachments (PDFs, scans) are skipped with a count. Re-running
duplicates notes, so delete the import folder first if retrying.

## Code layout

| File                      | Purpose                                             |
|---------------------------|-----------------------------------------------------|
| `src/main.c`              | GtkApplication entry point; settings/config loading |
| `src/app.[ch]`            | Shared `OnApp` context, icon loading, toolbar styles, DB switching/restore |
| `src/cli.[ch]`            | Headless noun-verb command-line interface           |
| `src/db.[ch]`             | SQLite layer: folders, notes, tags, settings, backup |
| `src/serialize.[ch]`      | ONBF binary format ⇄ GtkTextBuffer conversion       |
| `src/editor_window.[ch]`  | WYSIWYG editor: formatting, images, tables, tasks, code blocks, find-in-note |
| `src/library_window.[ch]` | Sidebar, list & grid views, drag & drop, menus, About |
| `src/search_window.[ch]`  | Cross-note search window                            |
| `src/settings_window.[ch]`| The Settings window                                 |
| `src/export.[ch]`         | HTML and Markdown exporters                         |
| `icons/`                  | Bundled elementary icons (see its README)           |
| `tools/gen_icon.c`        | Regenerates the `orange.png` app logo               |
