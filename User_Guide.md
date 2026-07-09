# Blue Notes — User Guide

Everyday use of Blue Notes: the library, search, the editor, settings,
storage & backup, export, and command-line automation. For build
instructions and the Apple Notes importer see the [README](README.md);
for the database schema and file formats see [Internals](Internals.md).

## Library window

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

## Search

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

## Editor windows

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
- **Code blocks** (Ctrl/Cmd+M) — monospaced, shaded blocks with a
  floating copy button in the upper-right corner; optional per-block
  line numbers in a slim painted gutter (numbers are never part of
  selection/copy).
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

## Settings (*File → Settings…*)

- **Appearance** — toolbar button style (text / icons / icons above
  text), set separately for library and editor windows (also available
  by right-clicking any toolbar); note counts next to folders and tags;
  and — when built with gtk-mac-integration — a native macOS menu bar
  option.
- **Editor** — show/hide the code-block copy button and code-block line
  numbers; auto-style the first line of a new note as Heading 1; a
  compact toolbar that collapses the style and list buttons into menus;
  re-enable GTK's touch assistance (selection drag handles, magnifier,
  and the tap cut/copy/paste popup — all off by default; fully applies
  after a restart); and a custom image-viewer program for opening
  images.
- **Database** — store the database in a custom folder (see Storage)
  and toggle the startup integrity check.

All changes apply live and persist (in `blue_notes.ini` next to the
binary). Toolbar icons are PNGs bundled in `icons/` — replaceable by
dropping in files, see `icons/README.md`.

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
checkboxes — see [Internals](Internals.md) for the format. Older
versions load transparently.

## Export

*File → Export All as HTML…/Markdown…* writes every note to a chosen
directory, mirroring the folder hierarchy; single notes export from
their right-click menu. HTML embeds images as data URIs and renders
tables, task checkboxes and code blocks; Markdown writes sidecar PNGs,
pipe tables and `- [ ]` task items.

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
