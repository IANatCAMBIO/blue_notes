# Blue Notes

An Apple Notes–style notes application written in plain C with GTK3 and
SQLite.

![Blue Notes](Screenshot.png)

Notes live in a single portable SQLite database. They are organized in
a Library window — nested folders and `#tags` in a sidebar, notes shown
as a list or a grid of thumbnail cards — and edited in one or more
child Editor windows. Editors are WYSIWYG rich text: inline styles,
headings, bulleted/numbered/task lists, tables, code blocks and inline
images. Notes export to HTML or Markdown, and everything can be
automated from the command line, which cooperates with a running GUI
over a standard unix socket.

- **[User Guide](User_Guide.md)** — the features in detail: library,
  editor, search, settings, storage & backup, export, and the
  command-line interface.
- **[Internals](Internals.md)** — code layout, the database schema, and
  the BNBF note format.

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
