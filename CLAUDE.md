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
| `src/library_window.[ch]` | Sidebar (folders+counts, tags+counts), notes list/grid, DnD, sortable headers, context menus, one unified toolbar (folder area \| notes area \| Search … About), menubar (File/View), native-menubar hook |
| `src/search_window.[ch]` | Search over titles + full text on a worker thread (spinner while running); scope = All Notes / live library selection; case + regex options |
| `src/settings_window.[ch]` | Toolbar styles (library vs editor), code-copy-button toggle, native macOS menubar toggle |
| `src/export.[ch]` | HTML + Markdown export (all notes mirroring folder tree, or single note) |
| `src/cli.[ch]` | Headless subcommand interface (runs before GTK in main; tags/folders/notes CRUD, backup, export); folders by path, notes by id |
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
- **Task checkboxes are GtkTextChildAnchors** carrying their state as
  object data (`on_anchor_set/is_checkbox`), rendered as native
  GtkCheckButtons (ONBF v5 CHECK records).  A task line = anchor + space
  + text under the on-list-check paragraph tag.  Legacy glyph-based
  notes (⬜/✅/☐/☑ prefixes) are migrated to anchors on load
  (`migrate_legacy_checkboxes`).  Like all anchors, copy/paste within a
  note drops the widget/state.
- **Images are GtkTextChildAnchors**, not pixbufs: the anchor carries the
  original pixbuf + display width as object data
  (`on_anchor_set_image/get_image`). The editor attaches a HiDPI-aware
  GtkImage (pixels scaled × scale-factor, cairo surface with device
  scale) at each anchor; export/search/thumbnails read anchor data
  offscreen and never need widgets. Default display width 320
  (`ON_IMAGE_DEFAULT_WIDTH`); right-click menu toggles thumbnail/full.
- ALL UI settings live in the ini (`[orange-notes]` group), loaded into
  memory ONCE by `on_app_config_init()` and written through on change
  (`on_app_config_get/set`); the file is never re-read while running.
  Keys: `db_dir`, `toolbar_style_library`, `toolbar_style_editor`
  (`text|icons|both`, default icons), `code_copy_button` (`1|0`),
  `code_line_numbers` (`1|0`), `native_menubar` (`1|0`),
  `sidebar_counts` (`1|0`, default 0 — folder/tag counts in the
  sidebar), `first_line_h1` (`1|0`, default 0 — auto-style the first
  line of a new note as H1), `image_viewer` (program path; unset =
  system default),
  `search_win_w`/`search_win_h` (last search-window size, the default
  for the next one). The DB settings table holds ONLY the `in_use`
  instance lock (it must be in the DB — it coordinates instances across
  machines); main.c migrates any legacy UI keys out of it at startup.
- **Custom DB location** (shared-folder support) lives in the CONFIG
  FILE `orange_notes.ini` NEXT TO THE BINARY (`[orange-notes] db_dir=`;
  resolved from argv[0] by `on_app_config_init()`, which must run before
  any config read — main() calls it first thing), never in the DB.
  `on_app_switch_database()` switches live: closes all editors (flushing
  saves), swaps the handle, copies the current file to the target if no
  notes.db exists there, persists, refreshes the library. Failure
  reverts to the old DB. If the configured DB can't be opened at
  startup, main.c ERRORS OUT — deliberately NO fallback to the default
  location: a silent fallback once made a user's notes "disappear" and
  strands writes in the wrong file (the trigger was a relaunch racing
  the dying instance's final flush past the 5 s busy timeout). One
  configured database, or a clear error.
- **Backup/Restore** (File menu): `on_db_backup_to()` uses SQLite's
  online backup API on the live DB; `on_app_restore_database()` closes
  editors, keeps the old file as `notes.db.pre-restore`, copies the
  backup in, reopens (rolls back if the file isn't a usable DB).
- **Instance lock**: settings key `in_use` = `user@host (pid N, since
  ...)`. `on_app_db_acquire()` claims it or shows the read-only/override
  dialog; `on_app_db_release()` clears it ONLY if it still holds our
  token. Read-only = `app->read_only` + `PRAGMA query_only=ON`
  (engine-enforced) + non-editable editors + title suffix. Acquire runs
  at activate, after db switch, and reclaim after restore; release at
  exit, before switch, and via the SIGTERM handler in main.c (so pkill
  is a clean quit, not a fake crash). Restore is blocked in read-only.

## Hard-won GTK3 quirks (do not re-learn these)

1. **Text-window children are BUFFER-anchored.** Children added via
   `gtk_text_view_add_child_in_window(GTK_TEXT_WINDOW_TEXT)` take their
   position in buffer coordinates — they ride scrolling at 1x on their
   own. The view's top margin is re-added ONLY on the initial
   allocation of a freshly added child; positions set later via
   `gtk_text_view_move_child()` land as-is (verified on screen), so add
   at 0,0 and position everything through move_child with plain buffer
   coordinates. Probing tip: use `gdk_window_get_origin` on a realized
   window — offscreen pixel-scans do NOT composite these children.
2. **Never reposition those children on scroll.** A `move_child()`
   issued while scrolled doesn't take effect until the next
   validate/allocate cycle, so scroll-driven "corrections" (especially
   ones computed via `buffer_to_window_coords`, which double-apply the
   scroll) land late and misalign the widget by the scroll delta.
   Reposition only when content or geometry changes: rebuild on buffer
   changes (idle-coalesced) and view `size-allocate`. No off-screen
   hiding is needed — they scroll and clip naturally.
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
11. **Clearing a tree/list store zeroes its view's scrollbar.** Every
    model rebuild (refresh_sidebar, refresh_notes) must capture the
    scrolled window's vadjustment value first and restore it via
    `scroll_keep_queue()` (idle-deferred so the rebuilt view re-validates
    its height before the value is clamped). The sidebar always
    restores; the notes pane only when re-showing the same selection
    (`shown_kind/shown_id`) so navigation still starts at the top.
12. **Emoji padding is macOS-only** (`#ifdef __APPLE__` in
    `tag_emoji_in_range`): Apple Color Emoji draws wider than its Pango
    advance, so emoji get an editor-only letter-spacing tag (self + the
    following char, since Pango splits spacing half-per-side at run
    edges). Linux emoji fonts fit their advance — the pass compiles to a
    no-op there. The only other platform-specific code is the
    HAVE_GTKOSX menubar integration; everything else is portable GTK3.

## Performance decisions

- Grid thumbnails render ONLY while grid view is visible (`want_thumbs`
  in refresh_notes; on_view_grid refreshes) — the thumb cache keys on
  updated_at, so without the gate the edited note re-rendered on every
  autosave.
- Sidebar counts come from two GROUP BY maps (`on_db_note_count_map` /
  `on_db_tag_count_map`), not per-row COUNTs — per-query latency hurts
  on shared/network DBs.
- Editor saves use the LIGHT notify (`app->notify_note_saved` →
  refresh_notes only): editing a note can't change folder counts, so
  the sidebar isn't rebuilt per autosave/close. The full
  `notify_notes_changed` (sidebar + notes) fires only when the save
  changed the note's tag set — tracked LIVE by `ed->tags_modified`
  (set in tag_capture_end / on_tag_row_activated on creation, the
  before-handler on delete-range when the doomed range touches an
  on-tag span, and the insert after-handler when typing inside one) —
  never by scanning the buffer at save time. note_tags is rewritten
  only when that flag is set. Create/move/delete run in the library,
  which refreshes itself directly; db switch/restore use the full
  notify.
- `ed->dirty` (set by editor_queue_autosave, cleared by editor_save)
  gates the close-time flush: closing a window whose last autosave
  already ran skips serialization entirely — for image-heavy notes the
  final save otherwise re-encodes every PNG, which is the "slow close".
- code_buttons_rebuild has a fast path: when block-start offsets match
  the existing buttons' marks, it only repositions (no widget churn per
  keystroke).
- Cross-note search reads the `notes.body_text` cache column (filled by
  every save via `on_note_extract_text`, a record-walk over the ONBF
  blob that skips image payloads entirely). NULL rows (pre-column saves)
  fall back to the extractor and write back unless read-only. Measured:
  full cold extraction of 1260 notes / 616 MB of blobs = 183 ms; the
  warm path reads ~1 MB of text. The old path deserialized every note
  into a GtkTextBuffer, decoding every PNG, per search.
- Search runs on a worker thread (GtkSpinner in the window), never the
  GTK main loop. The worker opens its OWN SQLite connection (one
  connection must not cross threads); scope is resolved on the main
  thread first (it reads library widgets); results come back via
  g_idle_add. A SearchJob owns everything and frees itself on the main
  thread after checking its atomic `cancelled` flag — set when the
  window closes or a newer search starts, so it never touches a dead
  window. GRegex is immutable ⇒ compile on main (instant bad-pattern
  errors), match on worker.
- Deliberately NOT done: WAL journal or synchronous=NORMAL pragmas —
  unsafe/risky on network filesystems, which the shared-DB feature
  targets.

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
