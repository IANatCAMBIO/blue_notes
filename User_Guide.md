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
  show the note's database id at the right of the editor status bar
  (off by default — handy with the command line, which addresses notes
  by id); re-enable GTK's touch assistance (selection drag handles,
  magnifier, and the tap cut/copy/paste popup — all off by default;
  fully applies after a restart); and a custom image-viewer program for
  opening images.
- **Database** — store the database in a custom folder (see Storage),
  toggle the startup integrity check, and show or hide the database
  path shown before the folder path in each window's status bar (on by
  default — handy when you juggle more than one database).

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
a configured shared location) and exits — no windows. While the GUI is
running the command is forwarded to it over the socket instead, and the
windows refresh to show what changed.

Reading:

```
blue_notes note list [PATH|--all]     ids, modified dates and titles
blue_notes note cat ID [--md]         a note's text (--md: Markdown with
                                      formatting; images as placeholders)
blue_notes search TEXT [--regex]      case-insensitive, titles + full text;
                                      prints ID / modified / full path
blue_notes folder list                folder tree with note counts
blue_notes tag list                   every tag with its note count
blue_notes tag notes NAME             the notes labeled with a tag
blue_notes note tags ID               a note's tags, one per line
```

Writing:

```
blue_notes note new [--folder PATH] TEXT|-   create (first line = title)
blue_notes note append ID TEXT|-             add text on a fresh line
blue_notes note set ID TEXT|-                REPLACE a note's content
blue_notes note move ID [ID...] PATH         move into a folder ('/' = top)
blue_notes note tag ID NAME                  add a #tag to the note text
blue_notes note untag ID NAME                remove a #tag
blue_notes note add-image ID FILE            append an image file
blue_notes note set-modified ID TIMESTAMP    set the modified date (UNIX s)
blue_notes folder add PATH                   create nested, like mkdir -p
```

Deleting (safe by default — both go to the Trash, restorable in the GUI):

```
blue_notes note delete [--permanent] ID [ID...]
blue_notes note restore ID [ID...]
blue_notes folder delete [--permanent] PATH
```

GUI and maintenance:

```
blue_notes note open PATH             open an editor (id or Folder/Title;
                                      starts Blue Notes if needed)
blue_notes quicknote                  new note in the root folder + editor
blue_notes backup FILE.db             snapshot the live database
blue_notes export-md DIR              export all notes as Markdown
blue_notes export-html DIR            export all notes as HTML
```

Folders are addressed by path (`"Work/Projects"`); notes by the ids
`note list` and `search` print — ids are permanent: they are never
reused, so a stale id fails cleanly instead of hitting the wrong note.
`note new`, `note append` and `note set` take their content from the
argument or stdin (`-`). Plain text goes in; `note set` replaces the
whole note, so any formatting, images, tables or tags the old content
had are lost — prefer `note append` for adding to rich notes. Output is
tab-separated for easy scripting; exit codes: 0 success, 1 usage,
2 failure. `blue_notes help` shows the full reference.

## Using Blue Notes with an AI agent

The CLI is the intended interface for AI agents: it covers reading,
searching, writing, tagging and organizing, and because a running GUI
serves CLI calls over its socket, an agent can work on the database
while you have Blue Notes open — your windows update live as it works.

**Agents that can run shell commands** (Claude Code, Codex CLI, Cursor,
and similar) can use the binary directly. Tell the agent where it is
and how to behave — for Claude Code, drop something like this into the
project's `CLAUDE.md` (for other tools use `AGENTS.md` or the system
prompt):

```markdown
## My notes database

My notes live in Blue Notes; use its CLI: /path/to/blue_notes
Run "blue_notes help" for the full command list.

- Find notes with `search TEXT` (or `note list --all`), read one with
  `note cat ID` (`--md` keeps formatting), then act on the id.
- Add to an existing note with `note append` — `note set` REPLACES the
  note and destroys images/formatting, so use it only on notes you
  created yourself.
- `note delete` only moves to the Trash. NEVER use `--permanent` or
  `tag delete` unless I explicitly ask.
- Before any bulk operation, snapshot first: `backup /tmp/pre-agent.db`.
- Don't edit a note I currently have open in an editor window — my
  autosave could overwrite your change. Creating new notes is always
  safe.
```

A useful pattern is a capture workflow — "summarize this thread and
save it to my notes" becomes:

```
blue_notes folder add "Inbox/AI"
blue_notes note new --folder "Inbox/AI" -   <<'EOF'
Meeting summary 2026-07-10
...the agent's text...
EOF
blue_notes note tag 42 meeting
```

**Chat-only assistants** (ChatGPT or Claude in the browser, with no
shell access) cannot reach the database directly. Two options:

- Export and upload: `blue_notes export-md ~/notes-export` produces a
  folder of plain Markdown files that any assistant can read; paste the
  assistant's answers back with `note new -`/`note append -`.
- Wrap the CLI in an MCP server: each tool call shells out to one
  `blue_notes` subcommand. Clients that speak MCP (Claude Desktop,
  Claude in Slack, others) then get live read/write access with the
  same safety properties as the CLI — the wrapper needs no database
  code at all.

Whatever the agent, the safety rails are the same ones you use:
deletes default to the Trash, `--permanent` is the only destructive
flag, `backup` is cheap, and note ids never lie.
