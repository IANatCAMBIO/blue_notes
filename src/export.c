/* ===========================================================================
 * export.c — export all notes to HTML or Markdown (implementation)
 *
 * Strategy: every note's BNBF blob is deserialized into an *offscreen*
 * GtkTextBuffer (no window needed), which is then walked line by line.
 * Each line's paragraph style (heading / code block / list / plain) picks
 * the block element, and the characters inside the line are grouped into
 * runs of identical inline styling (bold/italic/…) for the span-level
 * markup.  Consecutive list lines are merged into one list, consecutive
 * code-block lines into one <pre> / fenced block.
 * =========================================================================== */

#include "export.h"
#include "serialize.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * OnExportCtx — per-note rendering context.
 *
 * Fields:
 *   format     — output format being produced.
 *   out        — the file body being built.
 *   note_dir   — directory the note file will live in (for image files).
 *   base_name  — note filename without extension (image name prefix).
 *   img_count  — how many images this note has emitted so far, used to
 *                number the side-car PNG files in Markdown mode.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnExportFormat format;
    GString       *out;
    const gchar   *note_dir;
    const gchar   *base_name;
    gint           img_count;
} OnExportCtx;

/* Inline-style bits considered when grouping runs inside a line.            */
#define EXPORT_INLINE_MASK (ON_FMT_BOLD | ON_FMT_ITALIC | \
                            ON_FMT_UNDERLINE | ON_FMT_STRIKE | ON_FMT_TAG)

/* ---------------------------------------------------------------------------
 * inline_flags_at() — the EXPORT_INLINE_MASK bits in effect at `iter`.
 * ------------------------------------------------------------------------- */
static guint32
inline_flags_at(GtkTextBuffer *buffer, const GtkTextIter *iter)
{
    static const struct { OnFormatFlags f; const gchar *name; } TAGS[] = {
        { ON_FMT_BOLD,      ON_TAGNAME_BOLD      },
        { ON_FMT_ITALIC,    ON_TAGNAME_ITALIC    },
        { ON_FMT_UNDERLINE, ON_TAGNAME_UNDERLINE },
        { ON_FMT_STRIKE,    ON_TAGNAME_STRIKE    },
        { ON_FMT_TAG,       ON_TAGNAME_TAG       },
    };
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    guint32 flags = 0;               /* accumulated bits                    */
    for (gsize i = 0; i < G_N_ELEMENTS(TAGS); i++) {
        GtkTextTag *tag = gtk_text_tag_table_lookup(table, TAGS[i].name);
        if (tag != NULL && gtk_text_iter_has_tag(iter, tag))
            flags |= TAGS[i].f;
    }
    return flags;
}

/* ---------------------------------------------------------------------------
 * line_para_flag() — the paragraph style of the line starting at `ls`
 * (0 = plain body text).
 * ------------------------------------------------------------------------- */
static guint32
line_para_flag(GtkTextBuffer *buffer, const GtkTextIter *ls)
{
    static const struct { OnFormatFlags f; const gchar *name; } TAGS[] = {
        { ON_FMT_H1,          ON_TAGNAME_H1          },
        { ON_FMT_H2,          ON_TAGNAME_H2          },
        { ON_FMT_CODEBLOCK,   ON_TAGNAME_CODEBLOCK   },
        { ON_FMT_LIST_BULLET, ON_TAGNAME_LIST_BULLET },
        { ON_FMT_LIST_NUMBER, ON_TAGNAME_LIST_NUMBER },
        { ON_FMT_LIST_CHECK,  ON_TAGNAME_LIST_CHECK  },
    };
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    for (gsize i = 0; i < G_N_ELEMENTS(TAGS); i++) {
        GtkTextTag *tag = gtk_text_tag_table_lookup(table, TAGS[i].name);
        if (tag != NULL && gtk_text_iter_has_tag(ls, tag))
            return TAGS[i].f;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * line_is_checked() — for a task-list line, whether its leading glyph is
 * the checked one (☑).
 * ------------------------------------------------------------------------- */
static gboolean
line_is_checked(const GtkTextIter *ls)
{
    gboolean checked = FALSE;        /* the checkbox's state                */
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(ls);
    if (anchor != NULL && on_anchor_is_checkbox(anchor, &checked))
        return checked;
    /* Legacy glyph-based notes (not yet re-saved after migration).         */
    on_char_is_checkbox(gtk_text_iter_get_char(ls), &checked);
    return checked;
}

/* ---------------------------------------------------------------------------
 * emit_table() — render an embedded table.
 * HTML: a plain <table>.  Markdown: a pipe table whose first row is the
 * header (as pipe tables require one).
 * ------------------------------------------------------------------------- */
static void
emit_table(OnExportCtx *ctx, OnTable *table)
{
    if (ctx->format == ON_EXPORT_HTML) {
        g_string_append(ctx->out, "<table>\n");
        for (gint r = 0; r < table->rows; r++) {
            /* Header rows use <th>.                                        */
            const gchar *cell_tag =
                (table->header && r == 0) ? "th" : "td";
            g_string_append(ctx->out, "<tr>");
            for (gint c = 0; c < table->cols; c++) {
                gchar *esc = g_markup_escape_text(
                    on_table_get(table, r, c), -1);
                /* Multiline cells: newlines become <br>.                   */
                gchar **lines = g_strsplit(esc, "\n", -1);
                gchar *joined = g_strjoinv("<br>", lines);
                g_strfreev(lines);
                g_string_append_printf(ctx->out, "<%s>%s</%s>",
                                       cell_tag, joined, cell_tag);
                g_free(joined);
                g_free(esc);
            }
            g_string_append(ctx->out, "</tr>\n");
        }
        g_string_append(ctx->out, "</table>\n");
    } else {
        g_string_append_c(ctx->out, '\n');
        for (gint r = 0; r < table->rows; r++) {
            for (gint c = 0; c < table->cols; c++) {
                /* Pipe tables cannot hold raw newlines: use <br>.          */
                gchar **lines = g_strsplit(on_table_get(table, r, c),
                                           "\n", -1);
                gchar *joined = g_strjoinv("<br>", lines);
                g_strfreev(lines);
                g_string_append_printf(ctx->out, "| %s ", joined);
                g_free(joined);
            }
            g_string_append(ctx->out, "|\n");
            if (r == 0) {            /* pipe tables require this row        */
                for (gint c = 0; c < table->cols; c++)
                    g_string_append(ctx->out, "| --- ");
                g_string_append(ctx->out, "|\n");
            }
        }
        g_string_append_c(ctx->out, '\n');
    }
}

/* ---------------------------------------------------------------------------
 * emit_image() — render one embedded pixbuf.
 * HTML: inline base64 data URI.  Markdown: write "<base>-imgN.png" beside
 * the note file and reference it relatively.
 *   ctx    — rendering context.
 *   pixbuf — the image.
 * ------------------------------------------------------------------------- */
static void
emit_image(OnExportCtx *ctx, GdkPixbuf *pixbuf)
{
    ctx->img_count++;

    if (ctx->format == ON_EXPORT_HTML) {
        gchar *png   = NULL;         /* PNG bytes                           */
        gsize  n_png = 0;            /* PNG byte count                      */
        GError *err  = NULL;
        if (!gdk_pixbuf_save_to_buffer(pixbuf, &png, &n_png,
                                       "png", &err, NULL)) {
            g_warning("export: image encode failed: %s", err->message);
            g_clear_error(&err);
            return;
        }
        gchar *b64 = g_base64_encode((const guchar *)png, n_png);
        g_string_append_printf(ctx->out,
            "<img src=\"data:image/png;base64,%s\" alt=\"image\">", b64);
        g_free(b64);
        g_free(png);
    } else {
        gchar *img_name = g_strdup_printf("%s-img%d.png",
                                          ctx->base_name, ctx->img_count);
        gchar *img_path = g_build_filename(ctx->note_dir, img_name, NULL);
        GError *err = NULL;
        if (gdk_pixbuf_save(pixbuf, img_path, "png", &err, NULL))
            g_string_append_printf(ctx->out, "![image](%s)", img_name);
        else {
            g_warning("export: image write failed: %s", err->message);
            g_clear_error(&err);
        }
        g_free(img_path);
        g_free(img_name);
    }
}

/* ---------------------------------------------------------------------------
 * emit_text_run() — render one run of identically-styled text.
 *   ctx   — rendering context.
 *   text  — the run's text (no newlines).
 *   flags — its EXPORT_INLINE_MASK bits.
 *   raw   — TRUE inside code blocks: no styling, escape-only (HTML).
 * ------------------------------------------------------------------------- */
static void
emit_text_run(OnExportCtx *ctx, const gchar *text, guint32 flags,
              gboolean raw)
{
    if (*text == '\0')
        return;

    if (ctx->format == ON_EXPORT_HTML) {
        gchar *esc = g_markup_escape_text(text, -1);
        if (raw) {
            g_string_append(ctx->out, esc);
        } else {
            if (flags & ON_FMT_TAG)
                g_string_append(ctx->out, "<span class=\"tag\">");
            if (flags & ON_FMT_BOLD)      g_string_append(ctx->out, "<strong>");
            if (flags & ON_FMT_ITALIC)    g_string_append(ctx->out, "<em>");
            if (flags & ON_FMT_UNDERLINE) g_string_append(ctx->out, "<u>");
            if (flags & ON_FMT_STRIKE)    g_string_append(ctx->out, "<s>");
            g_string_append(ctx->out, esc);
            if (flags & ON_FMT_STRIKE)    g_string_append(ctx->out, "</s>");
            if (flags & ON_FMT_UNDERLINE) g_string_append(ctx->out, "</u>");
            if (flags & ON_FMT_ITALIC)    g_string_append(ctx->out, "</em>");
            if (flags & ON_FMT_BOLD)      g_string_append(ctx->out, "</strong>");
            if (flags & ON_FMT_TAG)
                g_string_append(ctx->out, "</span>");
        }
        g_free(esc);
    } else {
        if (raw) {
            g_string_append(ctx->out, text);
        } else {
            /* Markdown markers.  Underline has no MD syntax; inline HTML
             * is valid Markdown, so <u> is used.  #tags stay literal.      */
            if (flags & ON_FMT_BOLD)      g_string_append(ctx->out, "**");
            if (flags & ON_FMT_ITALIC)    g_string_append(ctx->out, "*");
            if (flags & ON_FMT_UNDERLINE) g_string_append(ctx->out, "<u>");
            if (flags & ON_FMT_STRIKE)    g_string_append(ctx->out, "~~");
            g_string_append(ctx->out, text);
            if (flags & ON_FMT_STRIKE)    g_string_append(ctx->out, "~~");
            if (flags & ON_FMT_UNDERLINE) g_string_append(ctx->out, "</u>");
            if (flags & ON_FMT_ITALIC)    g_string_append(ctx->out, "*");
            if (flags & ON_FMT_BOLD)      g_string_append(ctx->out, "**");
        }
    }
}

/* ---------------------------------------------------------------------------
 * render_line_inline() — walk one line's characters, emitting styled text
 * runs and images in order.
 *   ctx    — rendering context.
 *   buffer — the note buffer.
 *   start  — line start (after any list-prefix stripping).
 *   end    — line end (before the newline).
 *   raw    — TRUE inside code blocks (no inline styling).
 * ------------------------------------------------------------------------- */
static void
render_line_inline(OnExportCtx *ctx, GtkTextBuffer *buffer,
                   const GtkTextIter *start, const GtkTextIter *end,
                   gboolean raw)
{
    GtkTextIter it = *start;         /* walk cursor                         */
    GString *run = g_string_new(NULL);   /* pending same-style text         */
    guint32  run_flags = 0;              /* its style bits                  */

    while (gtk_text_iter_compare(&it, end) < 0) {
        /* Images and tables live on child anchors; raw pixbufs are also
         * accepted.                                                        */
        GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&it);
        if (anchor != NULL && on_anchor_is_checkbox(anchor, NULL)) {
            /* Checkbox anchors are the line prefix, emitted at the block
             * level — skip the placeholder character here.                 */
            gtk_text_iter_forward_char(&it);
            continue;
        }
        OnTable *table = (anchor != NULL)
                         ? on_anchor_get_table(anchor) : NULL;
        if (table != NULL) {
            emit_text_run(ctx, run->str, run_flags, raw);
            g_string_truncate(run, 0);
            emit_table(ctx, table);
            gtk_text_iter_forward_char(&it);
            continue;
        }
        GdkPixbuf *pixbuf = (anchor != NULL)
                            ? on_anchor_get_image(anchor, NULL)
                            : gtk_text_iter_get_pixbuf(&it);
        if (pixbuf != NULL) {
            emit_text_run(ctx, run->str, run_flags, raw);
            g_string_truncate(run, 0);
            emit_image(ctx, pixbuf);
            gtk_text_iter_forward_char(&it);
            continue;
        }
        if (anchor != NULL) {        /* imageless anchor: skip its 0xFFFC   */
            gtk_text_iter_forward_char(&it);
            continue;
        }

        guint32 flags = raw ? 0 : inline_flags_at(buffer, &it);
        if (flags != run_flags && run->len > 0) {
            emit_text_run(ctx, run->str, run_flags, raw);
            g_string_truncate(run, 0);
        }
        run_flags = flags;

        gunichar c = gtk_text_iter_get_char(&it);
        if (c != 0xFFFC)
            g_string_append_unichar(run, c);
        gtk_text_iter_forward_char(&it);
    }
    emit_text_run(ctx, run->str, run_flags, raw);
    g_string_free(run, TRUE);
}

/* ---------------------------------------------------------------------------
 * strip_list_prefix_iter() — advance `start` past a literal "• " or
 * "12. " list prefix if present at the line start.
 * ------------------------------------------------------------------------- */
static void
strip_list_prefix_iter(GtkTextBuffer *buffer, GtkTextIter *start,
                       const GtkTextIter *end)
{
    /* Task lines: skip the checkbox anchor (+ separating space).           */
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(start);
    if (anchor != NULL && on_anchor_is_checkbox(anchor, NULL)) {
        gtk_text_iter_forward_char(start);
        if (gtk_text_iter_compare(start, end) < 0 &&
            gtk_text_iter_get_char(start) == ' ')
            gtk_text_iter_forward_char(start);
        return;
    }

    GtkTextIter probe_end = *start;  /* end of the probe window             */
    for (gint i = 0; i < 7 &&
         gtk_text_iter_compare(&probe_end, end) < 0; i++)
        gtk_text_iter_forward_char(&probe_end);

    gchar *head = gtk_text_buffer_get_text(buffer, start, &probe_end,
                                           FALSE);
    glong skip = 0;                  /* characters to skip                  */
    if (g_str_has_prefix(head, "\xe2\x80\xa2 ") ||
        (on_char_is_checkbox(g_utf8_get_char(head), NULL) &&
         g_utf8_get_char(g_utf8_next_char(head)) == ' ')) {
        skip = 2;
    } else {
        glong d = 0;                 /* leading digits                      */
        while (g_ascii_isdigit(head[d]))
            d++;
        if (d > 0 && head[d] == '.' && head[d + 1] == ' ')
            skip = d + 2;
    }
    g_free(head);
    gtk_text_iter_forward_chars(start, (gint)skip);
}

/* ---------------------------------------------------------------------------
 * BLOCK-STATE HELPERS — while rendering, `open_block` tracks which
 * multi-line construct is currently open (a list or a code block) so
 * consecutive lines of the same style merge into one block.
 * ------------------------------------------------------------------------- */

/* close_block() — emit the closer for whatever block is open.               */
static void
close_block(OnExportCtx *ctx, guint32 open_block)
{
    if (ctx->format == ON_EXPORT_HTML) {
        if (open_block == ON_FMT_CODEBLOCK)
            g_string_append(ctx->out, "</code></pre>\n");
        else if (open_block == ON_FMT_LIST_BULLET ||
                 open_block == ON_FMT_LIST_CHECK)
            g_string_append(ctx->out, "</ul>\n");
        else if (open_block == ON_FMT_LIST_NUMBER)
            g_string_append(ctx->out, "</ol>\n");
    } else {
        if (open_block == ON_FMT_CODEBLOCK)
            g_string_append(ctx->out, "```\n");
        /* Markdown lists need no closer.                                   */
    }
}

/* open_block() — emit the opener for a new multi-line block.                */
static void
open_block(OnExportCtx *ctx, guint32 block)
{
    if (ctx->format == ON_EXPORT_HTML) {
        if (block == ON_FMT_CODEBLOCK)
            g_string_append(ctx->out, "<pre><code>");
        else if (block == ON_FMT_LIST_BULLET)
            g_string_append(ctx->out, "<ul>\n");
        else if (block == ON_FMT_LIST_CHECK)
            g_string_append(ctx->out, "<ul class=\"tasks\">\n");
        else if (block == ON_FMT_LIST_NUMBER)
            g_string_append(ctx->out, "<ol>\n");
    } else {
        if (block == ON_FMT_CODEBLOCK)
            g_string_append(ctx->out, "```\n");
    }
}

/* ---------------------------------------------------------------------------
 * render_note_body() — walk every line of the buffer and build the full
 * document body in ctx->out.
 * ------------------------------------------------------------------------- */
static void
render_note_body(OnExportCtx *ctx, GtkTextBuffer *buffer)
{
    gint n_lines = gtk_text_buffer_get_line_count(buffer);
    guint32 open = 0;                /* currently open multi-line block     */

    for (gint line = 0; line < n_lines; line++) {
        GtkTextIter ls, le;          /* line bounds (newline excluded)      */
        gtk_text_buffer_get_iter_at_line(buffer, &ls, line);
        le = ls;
        if (!gtk_text_iter_ends_line(&le))
            gtk_text_iter_forward_to_line_end(&le);

        guint32 para = line_para_flag(buffer, &ls);

        /* Blocks (lists, code) persist across lines; close the open one
         * when the style changes.                                          */
        gboolean is_block = (para == ON_FMT_CODEBLOCK ||
                             para == ON_FMT_LIST_BULLET ||
                             para == ON_FMT_LIST_NUMBER ||
                             para == ON_FMT_LIST_CHECK);
        if (open != 0 && (!is_block || para != open)) {
            close_block(ctx, open);
            open = 0;
        }
        if (is_block && open == 0) {
            open_block(ctx, para);
            open = para;
        }

        if (ctx->format == ON_EXPORT_HTML) {
            switch (para) {
            case ON_FMT_H1:
                g_string_append(ctx->out, "<h1>");
                render_line_inline(ctx, buffer, &ls, &le, FALSE);
                g_string_append(ctx->out, "</h1>\n");
                break;
            case ON_FMT_H2:
                g_string_append(ctx->out, "<h2>");
                render_line_inline(ctx, buffer, &ls, &le, FALSE);
                g_string_append(ctx->out, "</h2>\n");
                break;
            case ON_FMT_CODEBLOCK:
                render_line_inline(ctx, buffer, &ls, &le, TRUE);
                g_string_append_c(ctx->out, '\n');
                break;
            case ON_FMT_LIST_BULLET:
            case ON_FMT_LIST_NUMBER:
                strip_list_prefix_iter(buffer, &ls, &le);
                g_string_append(ctx->out, "<li>");
                render_line_inline(ctx, buffer, &ls, &le, FALSE);
                g_string_append(ctx->out, "</li>\n");
                break;
            case ON_FMT_LIST_CHECK: {
                gboolean checked = line_is_checked(&ls);
                strip_list_prefix_iter(buffer, &ls, &le);
                g_string_append_printf(ctx->out,
                    "<li><input type=\"checkbox\"%s disabled> ",
                    checked ? " checked" : "");
                render_line_inline(ctx, buffer, &ls, &le, FALSE);
                g_string_append(ctx->out, "</li>\n");
                break;
            }
            default:
                if (gtk_text_iter_compare(&ls, &le) < 0) {
                    g_string_append(ctx->out, "<p>");
                    render_line_inline(ctx, buffer, &ls, &le, FALSE);
                    g_string_append(ctx->out, "</p>\n");
                }
                break;
            }
        } else {
            switch (para) {
            case ON_FMT_H1:
                g_string_append(ctx->out, "# ");
                render_line_inline(ctx, buffer, &ls, &le, FALSE);
                g_string_append_c(ctx->out, '\n');
                break;
            case ON_FMT_H2:
                g_string_append(ctx->out, "## ");
                render_line_inline(ctx, buffer, &ls, &le, FALSE);
                g_string_append_c(ctx->out, '\n');
                break;
            case ON_FMT_CODEBLOCK:
                render_line_inline(ctx, buffer, &ls, &le, TRUE);
                g_string_append_c(ctx->out, '\n');
                break;
            case ON_FMT_LIST_BULLET:
                strip_list_prefix_iter(buffer, &ls, &le);
                g_string_append(ctx->out, "- ");
                render_line_inline(ctx, buffer, &ls, &le, FALSE);
                g_string_append_c(ctx->out, '\n');
                break;
            case ON_FMT_LIST_CHECK: {
                gboolean checked = line_is_checked(&ls);
                strip_list_prefix_iter(buffer, &ls, &le);
                g_string_append(ctx->out, checked ? "- [x] " : "- [ ] ");
                render_line_inline(ctx, buffer, &ls, &le, FALSE);
                g_string_append_c(ctx->out, '\n');
                break;
            }
            case ON_FMT_LIST_NUMBER:
                /* Keep the literal "N. " prefix — it is valid Markdown.    */
                render_line_inline(ctx, buffer, &ls, &le, FALSE);
                g_string_append_c(ctx->out, '\n');
                break;
            default:
                render_line_inline(ctx, buffer, &ls, &le, FALSE);
                g_string_append_c(ctx->out, '\n');
                /* Blank separator line keeps paragraphs distinct.          */
                g_string_append_c(ctx->out, '\n');
                break;
            }
        }
    }
    if (open != 0)
        close_block(ctx, open);
}

/* ---------------------------------------------------------------------------
 * sanitize_filename() — turn a note title into a safe file basename:
 * path separators and control chars become '-', long names truncate.
 * Returns a newly allocated string.
 * ------------------------------------------------------------------------- */
static gchar *
sanitize_filename(const gchar *title)
{
    gchar *name = g_strdup(*title != '\0' ? title : "Untitled");
    for (gchar *p = name; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == ':' || (guchar)*p < 0x20)
            *p = '-';
    }
    g_strstrip(name);
    if (*name == '\0') {
        g_free(name);
        return g_strdup("Untitled");
    }
    if (strlen(name) > 100)
        name[100] = '\0';
    return name;
}

/* ---------------------------------------------------------------------------
 * unique_path() — build "<dir>/<base><ext>", appending " (2)", " (3)"…
 * to the base if the path was already emitted THIS RUN (`used`, may be
 * NULL for single-note exports).  Uniquifying against the set rather
 * than the disk makes exports re-runnable mirrors: a second export into
 * the same directory overwrites its previous output instead of growing
 * an endless "Note (2)", "Note (3)"… series.  Returns the full path
 * and, via out_base, the final base name (both newly allocated).
 * ------------------------------------------------------------------------- */
static gchar *
unique_path(const gchar *dir, const gchar *base, const gchar *ext,
            GHashTable *used, gchar **out_base)
{
    gchar *final_base = g_strdup(base);  /* base name actually used         */
    gchar *path = g_strdup_printf("%s/%s%s", dir, final_base, ext);
    for (gint n = 2;
         used != NULL && g_hash_table_contains(used, path); n++) {
        g_free(final_base);
        g_free(path);
        final_base = g_strdup_printf("%s (%d)", base, n);
        path = g_strdup_printf("%s/%s%s", dir, final_base, ext);
    }
    if (used != NULL)
        g_hash_table_add(used, g_strdup(path));
    *out_base = final_base;
    return path;
}

/* ---------------------------------------------------------------------------
 * export_one() — render one note into `note_dir` in the given format.
 *   app      — application context (provides the database).
 *   m        — metadata of the note to export.
 *   note_dir — directory the file is written into (must exist).
 *   format   — output format.
 *   used     — full paths already emitted this run (see unique_path);
 *              NULL for single-note exports.
 * Returns TRUE if the file was written.
 * ------------------------------------------------------------------------- */
static gboolean
export_one(OnApp *app, OnNoteMeta *m, const gchar *note_dir,
           OnExportFormat format, GHashTable *used)
{
    /* Load and deserialize into an offscreen buffer.                       */
    GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
    on_buffer_ensure_tags(buffer);
    gsize   blob_len = 0;            /* stored blob size                    */
    guint8 *blob = on_db_note_load(app->db, m->id, &blob_len);
    if (blob != NULL) {
        on_note_deserialize(buffer, blob, blob_len);
        g_free(blob);
    }

    /* Compute the output path.                                             */
    const gchar *ext = (format == ON_EXPORT_HTML) ? ".html" : ".md";
    gchar *base = sanitize_filename(m->title);
    gchar *final_base = NULL;        /* base after uniquification           */
    gchar *path = unique_path(note_dir, base, ext, used, &final_base);

    /* Render.                                                              */
    OnExportCtx ctx = {
        .format    = format,
        .out       = g_string_new(NULL),
        .note_dir  = note_dir,
        .base_name = final_base,
        .img_count = 0,
    };

    if (format == ON_EXPORT_HTML) {
        gchar *esc_title = g_markup_escape_text(m->title, -1);
        g_string_append_printf(ctx.out,
            "<!DOCTYPE html>\n<html>\n<head>\n"
            "<meta charset=\"utf-8\">\n<title>%s</title>\n"
            "<style>\n"
            "body { font-family: sans-serif; max-width: 46em;"
            " margin: 2em auto; padding: 0 1em; line-height: 1.5; }\n"
            "pre { background: #f0f0f0; padding: 1em;"
            " border-radius: 6px; overflow-x: auto; }\n"
            "img { max-width: 100%%; }\n"
            ".tag { color: #c35a00; font-weight: 600; }\n"
            "ul.tasks { list-style: none; padding-left: 1.2em; }\n"
            "table { border-collapse: collapse; }\n"
            "td { border: 1px solid #bbb; padding: 4px 8px; }\n"
            "</style>\n</head>\n<body>\n", esc_title);
        g_free(esc_title);
    }

    render_note_body(&ctx, buffer);

    if (format == ON_EXPORT_HTML)
        g_string_append(ctx.out, "</body>\n</html>\n");

    /* Write the file.                                                      */
    GError *err = NULL;
    gboolean ok = g_file_set_contents(path, ctx.out->str,
                                      (gssize)ctx.out->len, &err);
    if (!ok) {
        g_warning("export: cannot write %s: %s", path, err->message);
        g_clear_error(&err);
    }

    g_string_free(ctx.out, TRUE);
    g_object_unref(buffer);
    g_free(path);
    g_free(final_base);
    g_free(base);
    return ok;
}

gint
on_export_all(OnApp *app, const gchar *dest_dir, OnExportFormat format,
              gchar **out_err)
{
    if (out_err != NULL)
        *out_err = NULL;

    if (g_mkdir_with_parents(dest_dir, 0755) != 0) {
        if (out_err != NULL)
            *out_err = g_strdup_printf("cannot create directory %s",
                                       dest_dir);
        return -1;
    }

    GList *notes = on_db_note_list_all(app->db);
    gint exported = 0;               /* notes successfully written          */

    /* Paths emitted this run (for within-run uniquification) and a
     * folder_id → ready-made note_dir cache: notes cluster into few
     * folders, so the path query + mkdir run once per folder, not per
     * note.                                                                */
    GHashTable *used = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    GHashTable *dirs = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                             g_free, g_free);

    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;     /* the note being exported             */

        /* Mirror the folder hierarchy under the destination.               */
        gchar *note_dir = g_hash_table_lookup(dirs, &m->folder_id);
        if (note_dir == NULL) {
            gchar *rel_dir = on_db_folder_path(app->db, m->folder_id);
            note_dir = (*rel_dir != '\0')
                       ? g_build_filename(dest_dir, rel_dir, NULL)
                       : g_strdup(dest_dir);
            g_free(rel_dir);
            g_mkdir_with_parents(note_dir, 0755);
            gint64 *key = g_new(gint64, 1);
            *key = m->folder_id;
            g_hash_table_insert(dirs, key, note_dir);
        }

        if (export_one(app, m, note_dir, format, used))
            exported++;
    }

    g_hash_table_destroy(dirs);
    g_hash_table_destroy(used);
    on_db_note_list_free(notes);
    return exported;
}

gboolean
on_export_note(OnApp *app, gint64 note_id, const gchar *dest_dir,
               OnExportFormat format)
{
    if (g_mkdir_with_parents(dest_dir, 0755) != 0)
        return FALSE;

    OnNoteMeta *m = on_db_note_get(app->db, note_id);
    if (m == NULL)
        return FALSE;

    gboolean ok = export_one(app, m, dest_dir, format, NULL);
    on_db_note_meta_free(m);
    return ok;
}
