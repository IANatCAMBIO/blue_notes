# Orange Notes — project guide

Apple Notes–style app in **plain C + GTK3 + SQLite**. Two window types:
a Library (folders/tags sidebar, notes as list or grid) and one editor
window per note (WYSIWYG rich text). No GNOME HeaderBars anywhere —
plain `GtkWindow` titlebars, formatted `"Orange Notes - <thing>"`.

## Build & run

```sh
export PATH=/opt/local/bin:$PATH   # MacPorts pkg-config
make          # builds ./orange_notes
make run
```

Dependencies (MacPorts): `gtk3 +quartz`, `sqlite3`, `pkgconf`, and
optionally `gtk-osx-application-gtk3` (native macOS menubar — pkg-config
module is **`gtk-mac-integration-gtk3`**, NOT "gtkmacintegration"; the
Makefile auto-detects it and defines `HAVE_GTKOSX`). librsvg provides the
SVG pixbuf loader the icons need. After toggling a dependency, run
`make clean && make` so every object sees the new flags.

## File map

| File | Purpose |
|---|---|
| `src/main.c` | GtkApplication entry; loads settings; sets `orange.png` as default window icon |
| `src/app.[ch]` | Shared `OnApp` context: db handle, open-editors map, per-family toolbar styles, icon loading, toolbar registry + right-click style menu |
| `src/db.[ch]` | SQLite: folders (nested), notes (content BLOB), tags, note_tags, settings (key/value), counts, ordering |
| `src/serialize.[ch]` | ONBF binary format ⇄ GtkTextBuffer; image anchors; shared GtkTextTag set (`on_buffer_ensure_tags`) |
| `src/editor_window.[ch]` | WYSIWYG editor: inline/paragraph formatting, list continuation, #tag autocomplete popup, image paste/context menu, floating code-block copy buttons, debounced autosave |
| `src/library_window.[ch]` | Sidebar (folders+counts, tags+counts), notes list/grid, DnD, sortable headers, context menus, menubar (File/View), native-menubar hook |
| `src/search_window.[ch]` | Search over titles + full text; scope = All Notes / live library selection; case + regex options |
| `src/settings_window.[ch]` | Toolbar styles (library vs editor), code-copy-button toggle, native macOS menubar toggle |
| `src/export.[ch]` | HTML + Markdown export (all notes mirroring folder tree, or single note) |
| `icons/` | elementary SVG icons, loaded by basename; see icons/README.md |
| `tools/gen_icon.c` | Regenerates `orange.png` (cairo drawing; keep it artifact-free — no lines/text over the fruit) |
| `orange.png` | App logo: default window icon and the 64×64 About-dialog logo |

## Data & formats

- DB: `~/.local/share/orange-notes/notes.db` (GLib user-data dir; do NOT
  rename this directory — existing user data lives there).
- Note content: **ONBF v2** blobs (see header comment in `serialize.h`).
  TEXT records = styled runs (flag bits ↔ named GtkTextTags via one shared
  table). IMAGE records = full-resolution PNG + chosen display width.
  v1 (no display width) still parses.
- **Images are GtkTextChildAnchors**, not pixbufs: the anchor carries the
  original pixbuf + display width as object data
  (`on_anchor_set_image/get_image`). The editor attaches a HiDPI-aware
  GtkImage (pixels scaled × scale-factor, cairo surface with device
  scale) at each anchor; export/search/thumbnails read anchor data
  offscreen and never need widgets. Default display width 320
  (`ON_IMAGE_DEFAULT_WIDTH`); right-click menu toggles thumbnail/full.
- Settings table keys: `toolbar_style_library`, `toolbar_style_editor`
  (`text|icons|both`, default both), `icon_theme` (legacy, unused),
  `code_copy_button` (`1|0`), `native_menubar` (`1|0`).
- **Custom DB location** (shared-folder support) lives in the CONFIG FILE
  `~/.config/orange-notes/config.ini` (`[orange-notes] db_dir=`), never in
  the DB. `on_app_switch_database()` switches live: closes all editors
  (flushing saves), swaps the handle, copies the current file to the
  target if no notes.db exists there, persists, refreshes the library.
  Failure reverts to the old DB. main.c falls back to the default
  location if the configured one is unreachable at startup.
- **Backup/Restore** (File menu): `on_db_backup_to()` uses SQLite's
  online backup API on the live DB; `on_app_restore_database()` closes
  editors, keeps the old file as `notes.db.pre-restore`, copies the
  backup in, reopens (rolls back if the file isn't a usable DB).

## Hard-won GTK3 quirks (do not re-learn these)

1. **Text-window children double-count the top margin.** Children added
   via `gtk_text_view_add_child_in_window(GTK_TEXT_WINDOW_TEXT)` are
   allocated at `requested_y + top_margin`, while
   `buffer_to_window_coords` already includes the margin — so subtract
   `gtk_text_view_get_top_margin()` once when positioning (x is NOT
   shifted). Verified by pixel-scanning an offscreen render.
2. Those children **do not scroll** with the buffer: reposition on
   vadjustment `value-changed`, view `size-allocate`, and rebuild on
   buffer changes (idle-coalesced); hide buttons whose block scrolled
   off-screen.
3. The floating copy button must be **pinned to an exact size in CSS for
   all states** (`button, button:hover, button:active { min-width/height;
   padding:0 }`) — theme hover styling otherwise changes its allocation
   and it jumps under the pointer. Position math uses the constant
   `CODE_BTN_SIZE`, never live allocations.
4. Anchor the button's y to `gtk_text_view_get_line_yrange()` (line top =
   where paragraph-background shading starts), not the char rect (which
   sits below pixels-above-lines).
5. **Retina blur**: raw pixbufs render 1 buffer-pixel = 1 logical px.
   Anything that must be sharp goes through cairo surfaces with device
   scale: editor images, grid thumbnails
   (`cairo_surface_set_device_scale`, list-store column type
   `CAIRO_GOBJECT_TYPE_SURFACE`, icon-view pixbuf renderer bound to the
   "surface" attribute), and toolbar icons (`on_app_icon_image_sized`
   rasterizes SVGs at size × monitor scale factor and wraps them via
   `gdk_cairo_surface_create_from_pixbuf`).
6. Toolbar styles: GtkToolbar natively supports TEXT/ICONS/BOTH — that IS
   the text/icons/icons-above-text feature. Buttons built via
   `on_app_tool_item_new` (icon file or Pango-markup glyph as
   icon_widget). Registered per family (`ON_TOOLBAR_LIBRARY/EDITOR`) so
   the two style settings are independent; right-click any toolbar for
   the style menu (`popup-context-menu` signal — fires on empty toolbar
   area only).
7. **Editor letter buttons (B/I/U/S) are markup glyphs on purpose** —
   elementary's symbolic SVGs are 16px light-grey and look fuzzy/washed
   next to Pango-rendered glyphs. Icon field NULL → fallback markup is
   the primary look. H1/H2/¶/•/1./{ } are glyphs too.
8. Multi-row drag to a folder: GtkTreeView drags a single
   GTK_TREE_MODEL_ROW; on drop, if the dragged note is in the current
   multi-selection, move the whole selection.
9. Paragraph-style tags must cover the trailing newline (see
   `line_span()`) so typing at line end inherits them; list items carry a
   literal "• "/"N. " prefix plus an indent tag; Enter continues lists,
   Enter on an empty item ends them; numbered blocks renumber.
10. Inline typing follows `ed->inline_flags` (word-processor model),
    enforced in the after-handler of `insert-text` for insertions ≤2
    chars (longer pastes keep their own tags).

## Environment gotchas

- Corporate TLS interception: MacPorts curl fails on github.com
  (self-signed cert in chain) — **use `/usr/bin/curl`** (macOS keychain
  trusts the proxy CA). gitlab.gnome.org and deb.debian.org work either
  way.
- clangd shows "gtk/gtk.h not found" diagnostics on every file — noise
  (no compile_commands.json); trust `make`, which builds `-Wall -Wextra`
  clean.
- The GUI can be launched in background for the user with
  `./orange_notes & disown` after `pkill -f "./orange_notes"`.

## Conventions

- Every function gets a banner comment: purpose, params, return; comment
  non-obvious variables. Column-aligned trailing comments, ~78-col lines.
- `on_` prefix for public symbols; `On` prefix for types.
- UI strings use UTF-8 escapes for …, •, ✕ etc. in source.
- No GtkHeaderBar. Window titles `"Orange Notes - <name>"`.
- Scrollbars: overlay scrolling disabled globally
  (`GTK_OVERLAY_SCROLLING=0` in main) + per-scrolled-window; vertical
  policy AUTOMATIC.
