# Orange Notes — toolbar icons

These SVGs come from the **elementary** icon theme (GPL-3.0,
<https://github.com/elementary/icons>): 24px action icons for the library
toolbars, and monochrome `actions/symbolic` variants (`*-symbolic.svg`)
for the editor's formatting toolbar and the code-block copy button.

SVG rendering requires the librsvg gdk-pixbuf loader:
`sudo port install librsvg` (then restart Orange Notes). Without it, all
buttons show small text glyphs instead of icons.

## Replacing icons

The app loads each icon by filename (`<name>.svg`, then `<name>.png`) —
drop in any 24×24-ish image with the right name to replace one. If a file
is missing or cannot be decoded, the button falls back to a text glyph.

| File                            | Used for                       |
|---------------------------------|--------------------------------|
| `document-new.png`              | New Note                       |
| `folder-new.png`                | New Folder                     |
| `document-properties.png`       | Rename Folder                  |
| `edit-delete.png`               | Delete Note / Delete Folder    |
| `edit-find.png`                 | Search                         |
| `edit-copy.png`                 | Code-block copy button         |
| `insert-image.png`              | Insert Image                   |
| `format-text-bold.png`          | Bold                           |
| `format-text-italic.png`        | Italic                         |
| `format-text-underline.png`     | Underline                      |
| `format-text-strikethrough.png` | Strikethrough                  |

These names are looked up but have no bundled file yet (text-glyph
fallbacks are shown until you add them): `view-list.png`, `view-grid.png`,
`heading-1.png`, `heading-2.png`, `body-text.png`, `list-bullet.png`,
`list-number.png`, `code-block.png`.
