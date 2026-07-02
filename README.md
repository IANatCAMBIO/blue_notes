# Orange Notes

An Apple Notes–style notes application written in plain C with GTK3 and
SQLite. Notes are rich text (WYSIWYG) with inline images, code blocks and
`#tags`, organized into nested folders, and exportable to HTML or Markdown.

## Features

- **Library window** — nested folder tree with per-folder note counts in
  the sidebar, notes shown as a list or a grid of square thumbnail cards
  (first image + text preview). Drag notes to reorder them within a
  folder, or drag them onto a sidebar folder to move them. The sidebar
  toolbar holds New/Rename/Delete Folder and Search; right-clicking a note
  offers Open, Export as HTML/Markdown, and Delete.
- **Search** — the sidebar Search button opens a search window that scans
  titles and full note text, scoped to all notes or just the selected
  folder/tag, with case-sensitive and regular-expression modes.
- **Editor windows** — every note opens in its own window with a standard
  titlebar (no GNOME header bars anywhere). Toolbar and shortcuts for
  **bold** (Ctrl/Cmd+B), *italic* (Ctrl/Cmd+I), underline (Ctrl/Cmd+U),
  strikethrough, two heading levels, bulleted and numbered lists.
- **Settings** — *File → Settings…* holds the toolbar button styles
  (text / icons / icons above text, separately for library and editor
  windows). Changes apply live and persist. Toolbar icons are the
  bundled elementary SVGs in `icons/` (they need
  `sudo port install librsvg` to render) and any of them can be replaced
  by dropping in a file — see `icons/README.md`.
- **Sorting** — click the Title header for alphabetical order or the
  Modified header for most-recent-first; drag-reordering works while the
  list is unsorted.
- **Folder context menu** — right-click a folder for New Subfolder,
  Rename, Delete, and "Search Here…" (a search pre-scoped to it).
- **Inline images** — paste an image from the clipboard straight into a
  note (screenshots included), or use the *Image…* button. Images are
  stored at full resolution; right-click one to copy it back out to the
  clipboard, open it full size, or toggle its inline display between full
  size and a thumbnail.
- **Code blocks** — the *Code* button turns the current line/selection into
  a monospaced, shaded code block; a floating clipboard button in each
  block's upper-right corner copies the whole block.
- **Tags** — type `#` in the editor and keep typing. A popup suggests
  existing tags (arrow keys + Enter to pick); a space ends the tag, Escape
  cancels it. All tags appear in the library sidebar with note counts;
  selecting one shows its notes exactly like a folder does.
- **Export** — *File → Export All as HTML…/Markdown…* writes every note to
  a directory of your choice, mirroring the folder hierarchy. HTML embeds
  images as data URIs; Markdown writes PNG files next to each note.
  Individual notes export from their right-click menu.
- **Autosave** — edits are saved to SQLite about a second after you stop
  typing, and again when the window closes.

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

Note content is stored as a compact binary blob ("ONBF") holding styled
text runs and PNG image records — see `src/serialize.h` for the format.

## Building

Requirements: a C compiler, GTK3 and SQLite3 development files, pkg-config.

macOS (MacPorts):

```sh
sudo port install pkgconf gtk3 +quartz
make
make run
```

Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libgtk-3-dev libsqlite3-dev
make
make run
```

## Code layout

| File                    | Purpose                                              |
|-------------------------|------------------------------------------------------|
| `src/main.c`            | GtkApplication entry point and app-context wiring    |
| `src/app.h`             | Shared `OnApp` context passed to every window        |
| `src/db.[ch]`           | SQLite layer: folders, notes, tags, ordering         |
| `src/serialize.[ch]`    | ONBF binary format ⇄ GtkTextBuffer conversion        |
| `src/editor_window.[ch]`| WYSIWYG editor: formatting, images, tags, autosave   |
| `src/library_window.[ch]`| Folder/tag sidebar, list & grid views, drag & drop  |
| `src/export.[ch]`       | HTML and Markdown exporters                          |
