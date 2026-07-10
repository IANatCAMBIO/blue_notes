# Code consolidation — July 2026

Result of a full-codebase simplification review (2026-07-09): four parallel
reviews swept `src/` for dead code, duplicate functions/tables, and easy
consolidations, and the plan below was then executed in five phases.

**Outcome: net −284 lines (1111 added / 1395 removed across 15 files), two
real bugs fixed, zero compiler warnings, no dead public API found.**
Verified by clean rebuild, CLI smoke tests (direct and socket-forwarded to
a running GUI), and a full 1264-note Markdown export.

## Bugs found and fixed (Phase 0)

- **Column-layout persistence was dead** — `list_columns_persist()` bailed
  on `g_list_length(cols) != 3`, written when the notes list had three
  columns; the Created column made it four, so header drags/toggles were
  never saved. Now guarded by `N_LIST_COLUMNS`, a constant tied to the
  `COLS[]` table so it can't drift again.
- **`prompt_for_text` leaked** a strdup'd copy on every OK press and
  returned the *untrimmed* text despite testing a stripped copy. It now
  returns the trimmed text and frees correctly.

## Phase 1 — one canonical flag⇄tag table

The `ON_FMT_*` ⇄ `ON_TAGNAME_*` mapping existed **six times** (serialize.c,
editor undo, two editor `PARA[]` copies, two export.c copies, plus a
name-only list in the load normalizer). Now there is exactly one:

- `on_flag_tags[]` / `on_n_flag_tags` exported from `serialize.[ch]`.
- `on_flags_at_iter(buffer, iter, mask)` replaces four near-identical
  tag-probe loops; `on_tag_name_for_flag()` replaces three linear
  flag→name lookups.
- `ON_FMT_INLINE_MASK` / `ON_FMT_PARA_MASK` moved to `serialize.h`
  (the PARA bits are mutually exclusive by construction — documented
  there).

## Phase 2 — shared mechanical helpers

| Helper | Absorbed |
|---|---|
| `on_app_notice()` (app.c) | 9 modal message-dialog run/destroy blocks |
| `on_app_pick_path()` (app.c) | 3 file choosers (viewer browse, open-db, restore-db); export choosers use it via Phase 4 |
| `on_app_widget_add_css()` (app.c) | 7 CSS-provider rituals across 3 files |
| `on_app_config_get_bool()` (app.c) | ~13 get/strcmp/free triplets, incl. main.c's whole settings block |
| `editors_foreach()` (editor_window.c) | the 4 open-editors hash-walk iterators |
| Table-driven `BOOL_SETTINGS[]` (settings_window.c) | 7 near-identical toggled handlers + 7 checkbox construction blocks (`offsetof(OnApp, field)` + shared handler; touch-assist and the gtkosx menubar stay hand-written) |

## Phase 3 — db.c / cli.c / ipc.c (net −97 lines)

- `stmt_done()` — shared prepare/step/warn/finalize tail for 12 DML
  functions. Side effect: failures in these paths now always `g_warning`
  (before, only 4 of 14 did).
- `query_int64()` — shared scalar-query shape for 5 functions; the
  find-half of `on_db_tag_get_or_create` now calls `on_db_tag_find`.
- **Deleted** `on_db_note_move` / `on_db_note_delete` (single-note
  variants; CLI was their only caller and now uses the bulk
  `on_db_notes_move/delete` with `&id, 1`, same as the GUI).
- **Deleted** `on_db_note_count_for_folder` / `_for_tag` (CLI-only
  per-row COUNTs); `folder list` / `tag list` fetch the count maps once —
  faster on shared/network DBs, per the project's own perf rule.
- `reorder_rows()` merges the byte-identical folder/note reorder twins;
  `PRUNE_ORPHAN_TAGS_SQL` defined once (was 5 copies); `meta_query()`
  wraps the 4 no-bind list functions.
- cli.c `note_from_arg()` — the parse-id/lookup/error preamble, 4 sites.
- ipc.c `ipc_connect()` — the duplicated socket-connect sequence.

## Phase 4 — per-file merges

- **library_window.c** (net −40): `refresh_all()` replaced 13 adjacent
  refresh_sidebar+refresh_notes pairs; `sort_by_time()` merges the
  updated/created comparators (column via user_data); `utf8_casecmp()`
  for the three casefold comparators; `pick_export_dir()` +
  `report_exported()` shared by both export flows; `trash_notes_core()` +
  `trash_folder()` shared by drag and menu paths; `menu_new_transient()` +
  `menu_popup()` for the three context menus; `sb_kind_is_section()`,
  `lw_from_app()`, `notes_ctx_popup()`, `open_note_at_path()` (list/grid
  handler tails, activation handlers now one-liners).
- **editor_window.c**: `editor_search_move(ed, forward)` merges the
  mirror-image next/prev search pair (+ one `EDITOR_SEARCH_FLAGS` define,
  was spelled out 5×; the three trigger callbacks became two);
  `anchor_clear_widgets()` (3 sites), `image_from_menu_item()` (3 sites),
  `selection_line_range()` (2 sites); deleted a duplicate forward
  declaration; two longhand tag lookups now use `lookup_tag()`.
- **search_window.c**: per-folder path memoization replaced by the
  one-query `on_db_folder_path_map`; the two identical search-trigger
  wrappers replaced by swapped-signal connects.
- **export.c + editor_window.c**: the character-identical list-prefix
  parser now lives once as `on_list_prefix_chars()` in serialize.c (next
  to `on_char_is_checkbox`, which it uses).

## Intentional behavior changes

1. **Trashing a folder from the toolbar/context menu now closes editor
   windows open on its notes** (flushing autosaves) — the drag-to-trash
   path always did this; the inconsistency was resolved in favor of
   closing.
2. **`blue_notes tag list` counts exclude trashed notes**, matching the
   GUI sidebar (the old per-tag COUNT included trashed notes' links).
3. db-layer DML failures now log a generic "statement failed" warning
   uniformly (previously inconsistent / mostly silent).
4. `prompt_for_text` trims leading/trailing whitespace from folder names
   (part of the leak fix; matches the check's original intent).

## Skipped (deliberately — revisit only if wanted)

- **`code_buttons_rebuild` single-scan restructure** (~15 lines, one
  fewer buffer traversal) — medium risk: the fast-path comparison depends
  on prepend-reversed button-list order; touchy for the savings.
- **HTML vs Markdown `render_note_body` switch merge** (~40 lines) — a
  format×paragraph string table would cost real readability; the emit
  level is where the two formats genuinely differ.
- **BNBF record-walk iterator** (serialize deserialize vs extract_text,
  ~30 lines) and a callback-based buffer walker (serialize/undo/export) —
  larger refactors of touchy GTK-iter code; only worth it with deeper
  surgery.
- **CLI command table** (noun/verb/argc/mutates/handler) — machinery
  costs ~30 lines against ~60 saved; only pays if more commands are
  coming.
- ~~**Settings-table migration in main.c**~~ — **removed 2026-07-09**
  along with the other legacy shims; see "Legacy-shim removal" below.
- **export.c folder-path map swap** — its `dirs` cache already runs one
  query per *unique* folder and additionally caches the mkdir; churn
  outweighed the gain.
- **`undo_common_prefix`/`suffix` merge** — genuinely asymmetric UTF-8
  walks; merging would obfuscate.
- **`on_editor_window_open` / `_open_search` unification** — optional
  API cleanup touching 6 call sites for ~15 lines; low value.

## Legacy-shim removal (2026-07-09, follow-up)

With a single known user, the upgrade shims were retired after a
read-only scan of every BNBF blob in the live database proved them
unneeded (and a one-time offline heal made it so):

- **Blob scan**: zero notes with pre-v5 glyph checkboxes; 8 notes with
  half-tagged paragraph lines (styled text, untagged newline — the state
  `normalize_paragraph_tags` repaired on every load but that only
  persisted on re-save).
- **One-time heal**: after `blue_notes backup` (kept as
  `~/.local/share/blue_notes/pre-heal-backup-20260709.db`) and a clean
  GUI shutdown, the 8 blobs were rewritten offline, extending each
  line's start style over the whole line — a byte-level mirror of the
  load pass, verified by re-parse (text and record streams identical)
  and a clean re-scan.
- **Removed**: `migrate_legacy_checkboxes()` and
  `normalize_paragraph_tags()` (serialize.c, the "LEGACY LOAD FIXUP"
  pair); the main.c settings-table migration + `in_use` purge and with
  them `on_db_setting_get/delete()` (their only callers); and
  `on_db_migrate_legacy_name()` (the pre-1.4 `notes.db` rename) with all
  four call sites plus the first-run chooser's `notes.db` filter.
- **Kept**: the ONBF magic acceptance (the DB is full of pre-rename
  blobs), the empty `settings` table in the schema (old files must
  open), `on_char_is_checkbox()` (still parses typed/pasted glyph
  prefixes), and the runtime paragraph-continuity fix in the editor
  (prevents new half-tagged lines from being created at all).

If the app ever gets other users, upgrades from pre-2026-07 builds will
need these shims restored from git history.
