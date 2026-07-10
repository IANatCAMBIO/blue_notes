# Blue Notes — project guide

Apple Notes–style app in **plain C + GTK3 + SQLite**. Two window types:
a Library (folders/tags sidebar, notes as list or grid) and one editor
window per note (WYSIWYG rich text). No GNOME HeaderBars anywhere —
plain `GtkWindow` titlebars, formatted `"Blue Notes - <thing>"`.

## Build & run

```sh
export PATH=/opt/local/bin:$PATH   # MacPorts pkg-config
make          # builds ./blue_notes
make run
make app      # dist/BlueNotes-<version>.app (macOS; sips/iconutil;
              # vinyl.png → .icns; NOT self-contained — needs MacPorts GTK)
make deb      # dist/blue_notes_<version>_<arch>.deb (needs dpkg-deb,
make rpm      # dist/blue_notes-<version>-1.<arch>.rpm  needs rpmbuild —
              # build these ON the target Linux distro; they install to
              # /opt/blue_notes + a /usr/bin wrapper script that execs by
              # absolute path so argv[0]-relative icons/defaults resolve;
              # the deb control Package: field stays "blue-notes" —
              # Debian forbids "_" in package names)
```

The semantic version is the `VERSION` variable at the top of the
Makefile — single source: baked into the binary as `ON_VERSION` (About
dialog) and into every package filename. Objects depend on the Makefile
so a version bump recompiles.

Dependencies (MacPorts): `gtk3 +quartz`, `sqlite3`, `pkgconf`, and
optionally `gtk-osx-application-gtk3` (native macOS menubar — pkg-config
module is **`gtk-mac-integration-gtk3`**, NOT "gtkmacintegration"; the
Makefile auto-detects it and defines `HAVE_GTKOSX`). librsvg is
OPTIONAL now that all app icons are PNGs (incl. `warning.png` in the
confirm dialogs) — it only renders the bundled `icons/theme/` symbolic
arrows.
After toggling a dependency, run `make clean && make` so every object
sees the new flags.

## File map

| File | Purpose |
|---|---|
| `src/main.c` | GtkApplication entry; config init + settings migration; sets `icons/vinyl.png` as default window icon |
| `src/app.[ch]` | Shared `OnApp` context: db handle, open-editors map, per-family toolbar styles, icon loading, toolbar registry + right-click style menu |
| `src/db.[ch]` | SQLite: folders (nested), notes (content BLOB), tags, note_tags, settings (key/value), counts, ordering |
| `src/serialize.[ch]` | BNBF binary format ⇄ GtkTextBuffer; image anchors; shared GtkTextTag set (`on_buffer_ensure_tags`) |
| `src/editor_window.[ch]` | WYSIWYG editor: inline/paragraph formatting, list continuation, #tag autocomplete popup, image paste/context menu, floating code-block copy buttons, debounced autosave |
| `src/library_window.[ch]` | Sidebar (folders+counts, tags+counts), notes list/grid (list: Title/Path/Modified/Created, all resizable + sortable, Path and Created hidden by default; Path fed by `on_db_folder_path_map`), notes sorted Modified-newest-first by default (in-list drag reorder is off while sorted — list stores refuse row drops), folder context menu has Sort Subfolders Alphabetically (one level, `on_db_folder_reorder`), DnD (notes→folder incl. multi-select; single folder rows re-nest INTO / reorder BEFORE-AFTER / trash / drag-restore via `on_db_folder_move`+`on_db_folder_reorder`; drag icons: folder.png, file.png for one note, documents.png for 2+), sortable headers, context menus, one unified toolbar (folder area \| notes area \| Search … About), menubar (File/View), native-menubar hook, bottom status bar (left: selection path; selecting notes posts a transient "N files selected" event from both views' selection signals; right: latest event — post from anywhere via `on_app_status()`, printf-style, no-op until the library installs `app->notify_status`) |
| `src/search_window.[ch]` | Search over titles + full text on a worker thread (spinner while running); scope = All Notes / live library selection; case + regex options |
| `src/settings_window.[ch]` | Toolbar styles, sidebar counts, code copy/line-number toggles, first-line-H1, image viewer, native macOS menubar, database location |
| `src/export.[ch]` | HTML + Markdown export (all notes mirroring folder tree, or single note) |
| `src/cli.[ch]` | Headless subcommand interface (runs before GTK in main; tags/folders/notes CRUD, backup, export); folders by path, notes by id |
| `icons/` | custom PNG toolbar icons + `vinyl.png` app logo (window icon, About button/dialog), loaded by basename; see icons/README.md |
| `tools/import-apple-notes.sh` | Apple Notes migration (AppleScript export → CLI import; keeps modification dates) |

## Data & formats

- DB: `~/.local/share/blue_notes/blue_notes.db` (GLib user-data dir; the
  filename is `ON_DB_FILENAME` in db.h — a legacy `notes.db` (pre-1.4) is
  renamed in place by `on_db_migrate_legacy_name()`, called before every
  directory-level open: GUI/CLI startup, db switch, first-run chooser;
  the dir was renamed
  from `orange-notes` pre-release — do NOT rename again once released,
  user data will live there).
- Note content: **BNBF v5** blobs (magic `BNBF`; the pre-rename `ONBF` magic is accepted forever) (see header comment in `serialize.h`).
  TEXT records = styled runs (flag bits ↔ named GtkTextTags via one
  shared table); IMAGE = full-resolution PNG + display width; TABLE;
  CHECK. All older versions (1–4) still parse.
- **Task checkboxes are GtkTextChildAnchors** carrying their state as
  object data (`on_anchor_set/is_checkbox`), rendered as native
  GtkCheckButtons (BNBF v5 CHECK records).  A task line = anchor + space
  + text under the on-list-check paragraph tag.  Legacy glyph-based
  notes (⬜/✅/☐/☑ prefixes) are migrated to anchors on load
  (`migrate_legacy_checkboxes`).  Like all anchors, copy/paste within a
  note drops the widget/state.
- **Images are GtkTextChildAnchors**, not pixbufs: the anchor carries the
  original pixbuf + display width as object data
  (`on_anchor_set_image/get_image`). The editor attaches a HiDPI-aware
  GtkImage (pixels scaled × scale-factor, cairo surface with device
  scale) at each anchor; export/search/thumbnails read anchor data
  offscreen and never need widgets. Default thumbnail display fits a
  200×125 box (`ON_IMAGE_THUMB_W/H`, aspect kept, never upscaled);
  right-click menu toggles thumbnail/full.
- ALL UI settings live in the ini (`[blue-notes]` group), loaded into
  memory ONCE by `on_app_config_init()` and written through on change
  (`on_app_config_get/set`); the file is never re-read while running.
  The ini normally sits NEXT TO THE BINARY (portable mode); when no
  binary-adjacent ini exists AND that directory is unwritable (system
  installs: .deb/.rpm in /opt, .app in /Applications) it falls back to
  `~/.config/blue_notes/blue_notes.ini` instead.
  On first launch (no ini) it is seeded from `blue_notes.ini.defaults`
  next to the binary (committed; empty `db_dir` = default DB location).
  The live ini is gitignored — its rewrites drop comments and carry
  per-machine values.
  Keys: `db_dir`, `toolbar_style_library`, `toolbar_style_editor`
  (`text|icons|both`, default icons), `code_copy_button` (`1|0`),
  `code_line_numbers` (`1|0`), `native_menubar` (`1|0`),
  `db_integrity_check` (`1|0`, default 1 — hash-compare the DB file at
  startup, offering Open Anyway / Run Integrity Check when it changed;
  `db_hash` is the stored snapshot, written at clean exit),
  `sidebar_counts` (`1|0`, default 0 — folder/tag counts in the
  sidebar), `first_line_h1` (`1|0`, code default 0, but
  blue_notes.ini.defaults seeds it to 1 — auto-style the first
  line of a new note as H1), `compact_editor_toolbar` (`1|0`, default 0
  — collapse the editor's H1/H2/¶ buttons into an "Aa" Styles menu
  button and the list buttons into a "≡" Lists one; applies live via
  `on_editor_rebuild_toolbars_all`), `touch_assist` (`1|0`, default 0 =
  DISABLED — GTK's touch aids: the teardrop drag handles under text
  selections/the cursor, the selection magnifier, and the tap
  cut/copy/paste bubble, which some Linux input stacks (VM tablets)
  show for plain mouse input; GTK3 has no API for any of them.  Two
  levers: CSS in `on_app_apply_touch_assist` hides handles + magnifier
  live (`cursor-handle` collapsed, `popover.magnifier` transparent —
  the bubble can't be CSS-hidden: invisible-but-clickable buttons), and
  main() sets GDK_CORE_DEVICE_EVENTS=1 before GTK init, which stops the
  touchscreen classification driving all three — restart to change;
  costs XI2 smooth scrolling; no-op off X11), `image_viewer` (program
  path; unset = system default),
  `search_win_w`/`search_win_h` (last search-window size, the default
  for the next one), `statusbar_db_path` (`1|0`, default 1 — prefix the
  folder path in the library/editor status bars with the DB file's path,
  formatted by `on_app_location_text`; applies live from Settings),
  `list_columns` (list-view column layout,
  `key:vis` pairs in display order, default
  `path:0,title:1,modified:1,created:0`
  — written on every header drag/toggle, applied at window
  construction; right-click a column header for the show/hide menu),
  `list_autofit` (`1|0`, default 1 — same header menu: Path/Modified/Created always show
  their FULL content, Title takes the ellipsis + expand and is the one
  column that truncates.  Implemented by MEASURING content with a
  PangoLayout after every refresh (`list_autofit_now`) and setting
  FIXED widths — NOT with GTK_TREE_VIEW_COLUMN_AUTOSIZE, which is
  unusable: columns cache resized/requested widths that override it
  (never shrinking back), and ellipsizing renderers report a ~3-char
  minimum so ellipsized columns collapse instead of fitting.  Manual
  resize grips come back when it's off). The DB settings table is legacy-only: main.c
  migrates any old UI keys out of it and deletes the retired `in_use`
  lock key at startup; nothing writes to it anymore.
- **Custom DB location** (shared-folder support) lives in the CONFIG
  FILE `blue_notes.ini` NEXT TO THE BINARY (`[blue-notes] db_dir=`;
  resolved from argv[0] by `on_app_config_init()`, which must run before
  any config read — main() calls it first thing), never in the DB.
  `on_app_switch_database()` switches live: closes all editors (flushing
  saves), swaps the handle, copies the current file to the target if no
  blue_notes.db exists there, persists, refreshes the library. Failure
  reverts to the old DB. If the configured DB can't be opened at
  startup, main.c ERRORS OUT — deliberately NO fallback to the default
  location: a silent fallback once made a user's notes "disappear" and
  strands writes in the wrong file (the trigger was a relaunch racing
  the dying instance's final flush past the 5 s busy timeout). One
  configured database, or a clear error. When no blue_notes.db EXISTS at
  the expected location (first launch / emptied dir),
  `startup_first_run()` asks — "Open a blue_notes.db File" (chooser also
  accepts a legacy notes.db, renamed on selection; persists the new
  db_dir) or "Create a New blue_notes.db" — instead of silently creating
  an empty DB; both paths clear any stale db_hash.
- **Trash is a soft-delete flag, not a folder**: `notes.trashed` /
  `folders.trashed` columns + the `trash_folder_ids` view (recursive
  closure — only the TOP deleted folder is flagged; its subtree stays
  attached and is implicitly trashed via the view). folder_id/parent_id
  are untouched by deletion — they ARE the restore location; restore
  clears the flag and re-parents to top level only when the original
  location is itself still trashed. Moving a note (`on_db_note_move`)
  always clears the flag (drag out of Trash = restore-to-folder). All
  normal listings/counts filter through `NOTE_VISIBLE_SQL`; search's
  All-scope uses `on_db_note_list_all(db, TRUE)` to keep deleted notes
  findable; export/CLI pass FALSE. CLI note/folder delete remains
  PERMANENT. Sidebar: "All Notes" section on top (`SB_KIND_ALL`,
  newest-first), "Trash" section at the bottom only while non-empty
  (`SB_KIND_TRASH`, trashed folders as `SB_KIND_TRASH_FOLDER` children);
  in trash views the Delete paths turn permanent (with confirm) and the
  note menu becomes Open/Restore/Delete Permanently; GUI note/folder
  deletes elsewhere just trash with a status message, no dialog.
- **Backup/Restore** (File menu): `on_db_backup_to()` uses SQLite's
  online backup API on the live DB; `on_app_restore_database()` closes
  editors, keeps the old file as `blue_notes.db.pre-restore`, copies the
  backup in, reopens (rolls back if the file isn't a usable DB).
- **CLI ↔ GUI coexistence is socket-based, not lock-based**: a running
  GUI serves later CLI invocations over a unix socket (`src/ipc.c`), so
  the two never write the DB concurrently. The old in-DB `in_use`
  instance lock and the read-only mode (`app->read_only`, `PRAGMA
  query_only`, `on_app_db_acquire/release`) were REMOVED with that
  change; main.c deletes any leftover `in_use` key at startup. SIGTERM
  (pkill) destroys all windows so editor autosaves flush and the loop
  ends cleanly.

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
13. **A custom GTK_TREE_MODEL_ROW drop handler must own the WHOLE dest
    protocol.**  GtkTreeView's default `drag-motion` handler validates
    row drops by requesting the drag DATA on every motion
    (`set_status_pending` + `gtk_drag_get_data`), so
    `"drag-data-received"` fires repeatedly MID-DRAG.  On quartz the
    reply arrives before the release, so a received-handler that treats
    every delivery as a drop runs with stale coordinates (0,0 → the top
    sidebar row) and `gtk_drag_finish()`es the drag while the button is
    still down — drops only land when an X11-style late reply slips
    past the release.  Fix (see the sidebar in library_window.c):
    connect `drag-motion` (compute + validate the target yourself,
    `gtk_tree_view_set_drag_dest_row` + `gdk_drag_status`, return TRUE
    to block the class closure), `drag-leave` (clear the indicator),
    and `drag-drop` (request the data, return TRUE); then
    `drag-data-received` fires exactly once, at drop time, with real
    coordinates.  Costs the built-in drag auto-scroll/auto-expand.
14. **`gtk_tree_view_expand_all` after every model rebuild re-expands
    folders the user collapsed.**  refresh_sidebar snapshots the
    expanded rows before the clear (keyed kind+id — paths shift when
    folders move) and restores that state in its selection-restore walk;
    only the first population expands everything.  Custom drag icons go
    on with `g_signal_connect_after("drag-begin")` — the class handler
    sets its own row-snapshot icon in the class closure, so a normal
    connection gets overridden.

15. **GTK 3.24's GtkTreeView collapses a multi-selection on PRESS.**
    Its multipress gesture does CLEAR_AND_SELECT on any unmodified
    primary press — no drag deferral — so dragging a multi-selection is
    impossible out of the box (GtkIconView is fine: it defers via
    `last_single_clicked`).  You can't just consume the press: the
    multipress AND row-drag gestures both run in the BUBBLE phase, so a
    TRUE from a button-press handler kills drag initiation too.  Fix
    (notes list): on press over an already-selected row with ≥2
    selected, install a veto select-function; a drag-begin lifts the
    veto keeping the selection, a plain button-release lifts it and
    applies the collapse via gtk_tree_view_set_cursor.

16. **GtkTreeView type-ahead search auto-picks a useless column**:
    `gtk_tree_view_set_model` sets the search column to the first model
    column transformable to string — our stores lead with the int64 id,
    so typing in a focused view popped a search box that matched
    nothing.  Every tree view disables it
    (`gtk_tree_view_set_enable_search(view, FALSE)`); to bring it back
    usefully, point `gtk_tree_view_set_search_column` at a text column
    (e.g. NL_TITLE) instead.

17. **`notify::cursor-position` fires INSIDE the insert-text class
    handler** — after the character lands in the buffer but BEFORE any
    after-handlers run.  So a cursor-moved handler that adopts the style
    of the char left of the cursor reads the brand-new, still-untagged
    character and wiped `ed->inline_flags` before the insert
    after-handler could apply it (broke arming bold with no selection:
    Ctrl/Cmd+B, then type).  Fix: an insert-text BEFORE-handler sets
    `ed->typing_insert` for ≤2-char (typed) insertions; on_cursor_moved
    skips style adoption while it's up; the after-handler clears it.
    Real navigation (clicks, arrows) still adopts.

## Performance decisions

- Grid thumbnails render ONLY while grid view is visible (`want_thumbs`
  in refresh_notes; on_view_grid refreshes) — the thumb cache keys on
  updated_at, so without the gate the edited note re-rendered on every
  autosave.
- Sidebar counts come from two GROUP BY maps (`on_db_note_count_map` /
  `on_db_tag_count_map`), not per-row COUNTs — per-query latency hurts
  on shared/network DBs.  The list view's Path column likewise reads
  `on_db_folder_path_map` (all folders in one query, paths built in
  memory), never per-note `on_db_folder_path`.
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
  already ran skips serialization entirely.
- **Images are never re-encoded on save**: the PNG bytes are cached on
  the pixbuf as `"on-png"` GBytes (attached from the blob at full-res
  deserialize, or on the first encode of a pasted/inserted image);
  `on_note_serialize` emits them verbatim. Scaled loads
  (`on_note_deserialize_scaled`, thumbnails) skip the cache — their
  pixbuf no longer matches the bytes. Before this, every autosave of an
  image-heavy note re-compressed every PNG on the main loop.
- code_buttons_rebuild has a fast path: when block-start offsets match
  the existing buttons' marks, it only repositions (no widget churn per
  keystroke).
- Cross-note search reads the `notes.body_text` cache column (filled by
  every save via `on_note_extract_text`, a record-walk over the BNBF
  blob that skips image payloads entirely) — fetched as ONE query for
  the whole table (`on_db_note_body_map`), not per note: per-query
  latency is what hurts on shared/network DBs. NULL rows (pre-column
  saves) fall back to the extractor and write back. Measured: full cold
  extraction of 1260 notes / 616 MB of blobs = 183 ms; the warm path
  reads ~1 MB of text. The old path deserialized every note into a
  GtkTextBuffer, decoding every PNG, per search.
- Search runs on a worker thread (GtkSpinner in the window), never the
  GTK main loop. The worker opens its OWN SQLite connection (one
  connection must not cross threads); scope is resolved on the main
  thread first (it reads library widgets); results come back via
  g_idle_add. A SearchJob owns everything and frees itself on the main
  thread after checking its atomic `cancelled` flag — set when the
  window closes or a newer search starts, so it never touches a dead
  window. GRegex is immutable ⇒ compile on main (instant bad-pattern
  errors), match on worker.
- refresh_sidebar keeps its `populating` guard up through the
  selection restore, so the restore's select_iter can't fire the
  changed handler and rebuild the notes pane a second time — every
  caller pairs it with an explicit refresh_notes. If the old selection
  no longer exists it falls back to the root and refreshes the notes
  pane itself.
- Multi-note deletes go through `on_db_notes_delete` (one transaction +
  one orphan-tag prune), not per-note `on_db_note_delete` (which
  fsyncs and prunes per call).  Multi-note DROPS likewise use
  `on_db_notes_move` (one transaction) — per-note `on_db_note_move`
  fsyncs per call and froze the GUI on big drops.  The drop handler
  also calls `gtk_drag_finish` BEFORE its refreshes so the DnD
  handshake isn't stalled by the model rebuilds.  Autofit column
  measuring rides refresh_notes' population loop (one PangoLayout, one
  measurement per unique folder path, skipped while the grid is the
  visible view — on_view_list re-measures on switch); it never does a
  second model walk. The `#tag` autocomplete queries the tag
  list ONCE per capture (`ed->tag_choices`) and filters in memory per
  keystroke. `on_app_config_set` skips the ini rewrite when the value
  is unchanged. The startup/exit DB hash streams through `GChecksum`
  (never loads the file whole). Exports uniquify names within the run
  only, so re-exporting to the same directory overwrites (a mirror),
  not duplicates.
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
  `./blue_notes & disown` after `pkill -f "./blue_notes"`.

## Conventions

- Every function gets a banner comment: purpose, params, return; comment
  non-obvious variables. Column-aligned trailing comments, ~78-col lines.
- `on_` prefix for public symbols; `On` prefix for types.
- UI strings use UTF-8 escapes for …, •, ✕ etc. in source.
- No GtkHeaderBar. Window titles `"Blue Notes - <name>"`.
- Scrollbars: overlay scrolling disabled globally
  (`GTK_OVERLAY_SCROLLING=0` in main) + per-scrolled-window; vertical
  policy AUTOMATIC.
