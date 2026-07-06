# Blue Notes — toolbar icons

Custom PNG icons for the toolbars and dialogs.  The only SVGs left are
the bundled `theme/` symbolic arrows (see below); rendering those
requires the librsvg gdk-pixbuf loader: `sudo port install librsvg`
(then restart Blue Notes). Without it, GTK falls back to its stock
arrows.

## Replacing icons

The app loads each icon by filename (`<name>.svg`, then `<name>.png`) —
drop in any 24×24-ish image with the right name to replace one. If a file
is missing or cannot be decoded, the button falls back to a text glyph.

| File                     | Used for                       |
|--------------------------|--------------------------------|
| `file.png`               | New Note                       |
| `delete.png`             | Delete Note                    |
| `new-folder.png`         | New Folder                     |
| `delete-folder.png`      | Delete Folder                  |
| `view.png`               | List/Grid view toggle          |
| `web.png`                | Show/hide the folder pane      |
| `search.png`             | Search                         |
| `copy.png`               | Code-block copy button         |
| `vinyl.png`            | App logo: window icon, About button + dialog |
| `warning.png`            | Delete-confirmation dialogs    |
| `folder.png`             | Drag icon: dragging a folder   |
| `documents.png`          | Drag icon: dragging 2+ notes   |

`file.png` doubles as the drag-under-cursor icon when dragging a
single note.

These names are looked up but have no bundled file — the editor's
formatting buttons deliberately use crisp Pango text glyphs (B/I/U/S,
H1/H2, ¶, •, 1., { }, ⬜) instead of icons.  Add a file with one of these
names to override a glyph: `heading-1`, `heading-2`, `body-text`,
`list-bullet`, `list-number`, `list-check`, `code-block`.

`theme/` is a minimal bundled icon THEME (prepended to GTK's search
path at startup): sharp symbolic replacements for the stock arrows GTK
itself draws — sidebar expanders (`pan-*`), the in-note search entry
(`edit-find`/`edit-clear`), and the find prev/next buttons
(`go-up`/`go-down`). These are looked up by GTK, not by the app's icon
loader; keep `theme/hicolor/index.theme` in sync if you add sizes.
