#!/bin/sh
# ===========================================================================
# import-apple-notes.sh — migrate Apple Notes into Orange Notes
#
# Exports every folder and note from Notes.app via its AppleScript
# interface, converts the note bodies from HTML to text with textutil,
# and imports them through the orange_notes CLI.  Everything lands under
# an "Apple Notes Import" folder, mirroring the Apple Notes hierarchy.
#
# Usage:
#     tools/import-apple-notes.sh
#
# Notes:
#   - macOS will ask once for permission to control Notes.app.
#   - Image attachments are exported and appended to the end of each
#     imported note (their original inline position is not recoverable
#     from Notes' scripting interface).  Non-image attachments (PDFs,
#     scans, …) are skipped with a warning.
#   - "Recently Deleted" is skipped.
#   - Safe to re-run, but re-running imports duplicates (Orange Notes
#     ids differ) — delete the "Apple Notes Import" folder first.
# ===========================================================================

set -eu

# The orange_notes binary lives next to this repo's tools/ directory.
BIN="$(cd "$(dirname "$0")/.." && pwd)/orange_notes"
if [ ! -x "$BIN" ]; then
    echo "error: $BIN not found — run make first" >&2
    exit 1
fi

# Import destination root inside Orange Notes.
DEST_ROOT="Apple Notes Import"

# Workspace: one .html per note plus a manifest of TAB-separated
# "file<TAB>folder path" lines.
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "Exporting from Notes.app (macOS may ask for permission)..."

# ---------------------------------------------------------------------------
# The AppleScript walks every account and folder (rebuilding nested
# folder paths via each folder's container chain), writes each note's
# HTML body to <n>.html in the workspace, and appends a manifest line.
# ---------------------------------------------------------------------------
osascript - "$TMP" <<'APPLESCRIPT'
on run argv
    set outDir to item 1 of argv
    set counter to 0
    set manifest to ""

    tell application "Notes"
        repeat with acc in accounts
            repeat with f in folders of acc
                set fName to name of f
                if fName is not "Recently Deleted" then
                    -- Build the nested path by walking the containers.
                    set fPath to fName
                    set parentRef to f
                    repeat
                        try
                            set parentRef to container of parentRef
                            if class of parentRef is folder then
                                set fPath to (name of parentRef) & "/" & fPath
                            else
                                exit repeat
                            end if
                        on error
                            exit repeat
                        end try
                    end repeat

                    repeat with n in notes of f
                        set counter to counter + 1
                        set htmlBody to body of n

                        -- Write the note body as UTF-8 HTML.
                        set p to outDir & "/" & counter & ".html"
                        set fp to open for access (POSIX file p) ¬
                            with write permission
                        set eof of fp to 0
                        write htmlBody to fp as «class utf8»
                        close access fp

                        -- Save each attachment as "<n>-att<i>"; the
                        -- importer sniffs the format from the bytes.
                        set attIdx to 0
                        repeat with a in attachments of n
                            set attIdx to attIdx + 1
                            try
                                set ap to outDir & "/" & counter ¬
                                    & "-att" & attIdx
                                save a in POSIX file ap
                            on error
                                -- Some attachment types cannot be
                                -- saved; skip them.
                            end try
                        end repeat

                        set manifest to manifest & counter & tab ¬
                            & fPath & linefeed
                    end repeat
                end if
            end repeat
        end repeat
    end tell

    -- Write the manifest.
    set mp to outDir & "/manifest.tsv"
    set fp to open for access (POSIX file mp) with write permission
    set eof of fp to 0
    write manifest to fp as «class utf8»
    close access fp

    return counter
end run
APPLESCRIPT

if [ ! -s "$TMP/manifest.tsv" ]; then
    echo "No notes exported (empty Notes.app, or permission was denied)."
    exit 0
fi

echo "Importing into Orange Notes..."

imported=0
failed=0
images=0
skipped_att=0
# Manifest lines are "<file-number><TAB><folder path>".
while IFS="$(printf '\t')" read -r num folder; do
    [ -n "$num" ] || continue
    dest="$DEST_ROOT/$folder"

    # Idempotent: folder add finds existing path components by name.
    "$BIN" folder add "$dest" >/dev/null

    # HTML -> plain text (textutil ships with macOS), then import via
    # stdin; the note's first line becomes its title.  Capture the new
    # note's id ("note <id>\t<title>") for attaching images.
    note_id=$(textutil -convert txt -stdout "$TMP/$num.html" \
                  | "$BIN" note new --folder "$dest" - \
                  | awk '{print $2; exit}') || note_id=""
    if [ -z "$note_id" ]; then
        failed=$((failed + 1))
        echo "warning: failed to import note $num (folder: $folder)" >&2
        continue
    fi
    imported=$((imported + 1))

    # Append the note's exported attachments.  add-image sniffs the
    # format from the file contents; non-image attachments fail there
    # and are counted as skipped.
    for att in "$TMP/$num"-att*; do
        [ -e "$att" ] || continue
        if "$BIN" note add-image "$note_id" "$att" >/dev/null 2>&1; then
            images=$((images + 1))
        else
            skipped_att=$((skipped_att + 1))
        fi
    done
done < "$TMP/manifest.tsv"

echo "Done: $imported note(s) and $images image(s) imported into" \
     "\"$DEST_ROOT\""
[ "$failed" -gt 0 ] && echo "  $failed note(s) failed" >&2
[ "$skipped_att" -gt 0 ] && \
    echo "  $skipped_att non-image attachment(s) skipped" >&2
exit 0
