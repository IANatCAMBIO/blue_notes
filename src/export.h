/* ===========================================================================
 * export.h — export all notes to HTML or Markdown
 *
 * The exporter walks every note in the database, deserializes its BNBF
 * blob into an offscreen GtkTextBuffer, and renders that buffer to either
 * an .html or a .md file.  The on-disk layout mirrors the folder
 * hierarchy:
 *
 *     <dest>/Work/Project ideas.html
 *     <dest>/Personal/Travel/Packing list.html
 *
 * Images are embedded as base64 data URIs in HTML; for Markdown they are
 * written as PNG files next to the note and referenced relatively.
 * =========================================================================== */

#ifndef BLUE_EXPORT_H
#define BLUE_EXPORT_H

#include "app.h"

/* Output formats supported by on_export_all().                              */
typedef enum {
    ON_EXPORT_HTML,                  /* one .html file per note             */
    ON_EXPORT_MARKDOWN,              /* one .md file per note (+ .png files)*/
} OnExportFormat;

/* ---------------------------------------------------------------------------
 * on_export_all() — export every note in the database.
 *   app      — global application context (provides the database).
 *   dest_dir — directory to export into (created if missing).
 *   format   — ON_EXPORT_HTML or ON_EXPORT_MARKDOWN.
 *   out_err  — optional; receives a human-readable error message
 *              (g_free() it) when the return value is < 0.
 * Returns the number of notes exported, or -1 on a fatal error.
 * ------------------------------------------------------------------------- */
gint on_export_all(OnApp *app, const gchar *dest_dir, OnExportFormat format,
                   gchar **out_err);

/* ---------------------------------------------------------------------------
 * on_export_note() — export a single note into `dest_dir` (no folder
 * mirroring; the file lands directly in the chosen directory).
 *   app      — global application context.
 *   note_id  — the note to export.
 *   dest_dir — destination directory (created if missing).
 *   format   — ON_EXPORT_HTML or ON_EXPORT_MARKDOWN.
 * Returns TRUE if the file was written.
 * ------------------------------------------------------------------------- */
gboolean on_export_note(OnApp *app, gint64 note_id, const gchar *dest_dir,
                        OnExportFormat format);

/* ---------------------------------------------------------------------------
 * on_export_note_markdown() — render a single note's body to a Markdown
 * STRING.  No files are written: embedded images become numbered
 * "![image N]()" placeholders.  Used by the CLI's "note cat --md".
 *   app     — global application context.
 *   note_id — the note to render.
 * Returns a newly allocated string (g_free() it); empty for an unknown
 * or empty note.
 * ------------------------------------------------------------------------- */
gchar *on_export_note_markdown(OnApp *app, gint64 note_id);

#endif /* BLUE_EXPORT_H */
