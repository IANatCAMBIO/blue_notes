/* ===========================================================================
 * serialize.c — BNBF binary note format (implementation)
 *
 * See serialize.h for the format specification.  The general strategy:
 *
 *   serialize:   walk the buffer character by character, grouping runs of
 *                identical formatting into TEXT records and emitting an
 *                IMAGE record (PNG bytes) wherever a GdkPixbuf is embedded.
 *
 *   deserialize: read records back, inserting text with the matching
 *                GtkTextTags applied, and decoding PNG bytes back into
 *                embedded pixbufs.
 * =========================================================================== */

#include "serialize.h"

#include <string.h>

/* Magic bytes at the start of every BNBF blob.  Blobs written before the
 * Blue Notes rename carry the legacy "ONBF" magic — the readers accept
 * both, forever (existing databases hold thousands of such notes).          */
static const guint8 BNBF_MAGIC[4] = { 'B', 'N', 'B', 'F' };
static const guint8 ONBF_MAGIC[4] = { 'O', 'N', 'B', 'F' };

/* magic_ok() — does this blob start with the current or legacy magic?       */
static gboolean
magic_ok(const guint8 *data, gsize len)
{
    return data != NULL && len >= 8 &&
           (memcmp(data, BNBF_MAGIC, 4) == 0 ||
            memcmp(data, ONBF_MAGIC, 4) == 0);
}

/* Current format version written by on_note_serialize().  Version 2 added
 * the display_width field to IMAGE records; version 3 added TABLE
 * records; version 4 added the tflags field to TABLE records; version 5
 * added CHECK records.  All older versions are still readable.              */
#define BNBF_VERSION 5u

/* TABLE record flag bits (the tflags field).                                */
#define TABLE_FLAG_HEADER 1u         /* first row is a header row           */

/* Record type bytes.                                                        */
#define REC_END   0x00               /* end of document                     */
#define REC_TEXT  0x01               /* formatted text run                  */
#define REC_IMAGE 0x02               /* inline PNG image                    */
#define REC_TABLE 0x03               /* embedded table of text cells        */
#define REC_CHECK 0x04               /* task-list checkbox                  */

/* ---------------------------------------------------------------------------
 * on_flag_tags — THE flag ⇄ tag-name table (declared in serialize.h).
 * Serializer, editor, undo and export all iterate this single copy so the
 * mapping can never fall out of sync.
 * ------------------------------------------------------------------------- */
const OnFlagTag on_flag_tags[] = {
    { ON_FMT_BOLD,        ON_TAGNAME_BOLD        },
    { ON_FMT_ITALIC,      ON_TAGNAME_ITALIC      },
    { ON_FMT_UNDERLINE,   ON_TAGNAME_UNDERLINE   },
    { ON_FMT_STRIKE,      ON_TAGNAME_STRIKE      },
    { ON_FMT_H1,          ON_TAGNAME_H1          },
    { ON_FMT_H2,          ON_TAGNAME_H2          },
    { ON_FMT_CODEBLOCK,   ON_TAGNAME_CODEBLOCK   },
    { ON_FMT_LIST_BULLET, ON_TAGNAME_LIST_BULLET },
    { ON_FMT_LIST_NUMBER, ON_TAGNAME_LIST_NUMBER },
    { ON_FMT_LIST_CHECK,  ON_TAGNAME_LIST_CHECK  },
    { ON_FMT_TAG,         ON_TAGNAME_TAG         },
};

void
on_buffer_ensure_tags(GtkTextBuffer *buffer)
{
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);

    /* If one of our tags exists they all do — creation is atomic below.    */
    if (gtk_text_tag_table_lookup(table, ON_TAGNAME_BOLD) != NULL)
        return;

    /* Inline character styles.                                             */
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_BOLD,
                               "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_ITALIC,
                               "style", PANGO_STYLE_ITALIC, NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_UNDERLINE,
                               "underline", PANGO_UNDERLINE_SINGLE, NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_STRIKE,
                               "strikethrough", TRUE, NULL);

    /* Headings: larger, bold text applied to whole lines.                  */
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_H1,
                               "weight", PANGO_WEIGHT_BOLD,
                               "scale",  1.6,
                               NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_H2,
                               "weight", PANGO_WEIGHT_BOLD,
                               "scale",  1.3,
                               NULL);

    /* Code block: monospace on a subtle grey background, slightly inset.   */
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_CODEBLOCK,
                               "family",             "monospace",
                               "paragraph-background", "#f0f0f0",
                               "left-margin",        24,
                               "right-margin",       24,
                               NULL);

    /* List items: indented paragraphs.  The visible "• " / "1. " prefix is
     * inserted as literal text by the editor; the tag provides indent so
     * wrapped lines align under the text, Apple Notes style.               */
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_LIST_BULLET,
                               "left-margin", 32,
                               "indent",      -16,
                               NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_LIST_NUMBER,
                               "left-margin", 32,
                               "indent",      -16,
                               NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_LIST_CHECK,
                               "left-margin", 32,
                               "indent",      -16,
                               NULL);

    /* Inline #tag token: tinted so tags stand out from prose.              */
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_TAG,
                               "foreground", "#c35a00",
                               "weight",     PANGO_WEIGHT_SEMIBOLD,
                               NULL);
}

const gsize on_n_flag_tags = G_N_ELEMENTS(on_flag_tags);

guint32
on_flags_at_iter(GtkTextBuffer *buffer, const GtkTextIter *iter,
                 guint32 mask)
{
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    guint32 flags = 0;               /* accumulated format bits             */

    for (gsize i = 0; i < on_n_flag_tags; i++) {
        if ((on_flag_tags[i].flag & mask) == 0)
            continue;
        GtkTextTag *tag =
            gtk_text_tag_table_lookup(table, on_flag_tags[i].tag_name);
        if (tag != NULL && gtk_text_iter_has_tag(iter, tag))
            flags |= on_flag_tags[i].flag;
    }
    return flags;
}

const gchar *
on_tag_name_for_flag(guint32 flag)
{
    for (gsize i = 0; i < on_n_flag_tags; i++)
        if (on_flag_tags[i].flag == (OnFormatFlags)flag)
            return on_flag_tags[i].tag_name;
    return NULL;
}

/* ---------------------------------------------------------------------------
 * put_u32() — append a little-endian u32 to a byte array.
 *   buf — destination array.
 *   v   — value to append.
 * ------------------------------------------------------------------------- */
static void
put_u32(GByteArray *buf, guint32 v)
{
    guint8 b[4] = {
        (guint8)(v & 0xff),          (guint8)((v >> 8) & 0xff),
        (guint8)((v >> 16) & 0xff),  (guint8)((v >> 24) & 0xff),
    };
    g_byte_array_append(buf, b, 4);
}

/* ---------------------------------------------------------------------------
 * flush_text_run() — emit one TEXT record if the pending run is non-empty,
 * then reset the run accumulator.
 *   out   — the BNBF blob under construction.
 *   run   — pending UTF-8 text (emptied by this call).
 *   flags — formatting bits for the whole run.
 * ------------------------------------------------------------------------- */
static void
flush_text_run(GByteArray *out, GString *run, guint32 flags)
{
    if (run->len == 0)
        return;
    guint8 rec = REC_TEXT;           /* record type byte                    */
    g_byte_array_append(out, &rec, 1);
    put_u32(out, flags);
    put_u32(out, (guint32)run->len);
    g_byte_array_append(out, (const guint8 *)run->str, run->len);
    g_string_truncate(run, 0);
}

guint8 *
on_note_serialize(GtkTextBuffer *buffer, gsize *out_len)
{
    GByteArray *out = g_byte_array_new();   /* the growing BNBF blob        */
    g_byte_array_append(out, BNBF_MAGIC, 4);
    put_u32(out, BNBF_VERSION);

    GtkTextIter iter;                /* walk position                       */
    gtk_text_buffer_get_start_iter(buffer, &iter);

    GString *run       = g_string_new(NULL); /* text of the pending run     */
    guint32  run_flags = 0;                  /* formatting of the run       */

    while (!gtk_text_iter_is_end(&iter)) {
        /* Images and tables live on child anchors (raw pixbufs are also
         * accepted for robustness against buffers built elsewhere).        */
        GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);

        /* Checkbox anchor?  Emit a CHECK record.                           */
        gboolean checked;            /* the checkbox's state                */
        if (anchor != NULL && on_anchor_is_checkbox(anchor, &checked)) {
            flush_text_run(out, run, run_flags);
            guint8 rec = REC_CHECK;
            g_byte_array_append(out, &rec, 1);
            guint8 state = checked ? 1 : 0;
            g_byte_array_append(out, &state, 1);
            gtk_text_iter_forward_char(&iter);
            continue;
        }

        /* Table anchor?  Emit a TABLE record.                              */
        OnTable *table = (anchor != NULL)
                         ? on_anchor_get_table(anchor) : NULL;
        if (table != NULL) {
            flush_text_run(out, run, run_flags);
            guint8 rec = REC_TABLE;
            g_byte_array_append(out, &rec, 1);
            put_u32(out, table->header ? TABLE_FLAG_HEADER : 0);
            put_u32(out, (guint32)table->rows);
            put_u32(out, (guint32)table->cols);
            for (gint i = 0; i < table->rows * table->cols; i++) {
                const gchar *cell =
                    g_ptr_array_index(table->cells, i);
                put_u32(out, (guint32)strlen(cell));
                g_byte_array_append(out, (const guint8 *)cell,
                                    strlen(cell));
            }
            gtk_text_iter_forward_char(&iter);
            continue;
        }

        GdkPixbuf *original = NULL;  /* full-resolution image to store      */
        gint display_width = 0;      /* the user's chosen display width     */
        if (anchor != NULL) {
            original = on_anchor_get_image(anchor, &display_width);
        } else {
            original = gtk_text_iter_get_pixbuf(&iter);
            if (original != NULL)
                display_width = gdk_pixbuf_get_width(original);
        }

        if (original != NULL) {
            /* An embedded image interrupts any text run.                   */
            flush_text_run(out, run, run_flags);

            /* The pixbuf never changes once attached, so its PNG encoding
             * is cached on it as "on-png" (attached at load time, or here
             * on the first save of a freshly pasted image).  Without the
             * cache every autosave re-compressed every image — the
             * editor's biggest main-loop stall on image-heavy notes.       */
            GBytes *png_bytes =
                g_object_get_data(G_OBJECT(original), "on-png");
            if (png_bytes == NULL) {
                gchar *png   = NULL; /* PNG bytes for the original          */
                gsize  n_png = 0;    /* PNG byte count                      */
                GError *err  = NULL;
                if (gdk_pixbuf_save_to_buffer(original, &png, &n_png,
                                              "png", &err, NULL)) {
                    png_bytes = g_bytes_new_take(png, n_png);
                    g_object_set_data_full(G_OBJECT(original), "on-png",
                                           png_bytes,
                                           (GDestroyNotify)g_bytes_unref);
                } else {
                    g_warning("serialize: image save failed: %s",
                              err->message);
                    g_clear_error(&err);
                }
            }
            if (png_bytes != NULL) {
                gsize n_png = 0;     /* PNG byte count                      */
                gconstpointer png = g_bytes_get_data(png_bytes, &n_png);
                guint8 rec = REC_IMAGE;
                g_byte_array_append(out, &rec, 1);
                put_u32(out, (guint32)display_width);
                put_u32(out, (guint32)n_png);
                g_byte_array_append(out, (const guint8 *)png, n_png);
            }
            gtk_text_iter_forward_char(&iter);
            continue;
        }
        if (anchor != NULL) {        /* imageless anchor: skip its 0xFFFC   */
            gtk_text_iter_forward_char(&iter);
            continue;
        }

        /* Regular character: extend the current run, or flush and start a
         * new one when the formatting changes under the cursor.            */
        guint32 flags = on_flags_at_iter(buffer, &iter, ~0u);
        if (flags != run_flags) {
            flush_text_run(out, run, run_flags);
            run_flags = flags;
        }
        gunichar ch = gtk_text_iter_get_char(&iter);
        /* Real anchors/pixbufs were handled above, so a U+FFFC here is a
         * stray object-replacement char in pasted text — drop it rather
         * than save a placeholder with nothing behind it.                */
        if (ch != 0xFFFC)
            g_string_append_unichar(run, ch);
        gtk_text_iter_forward_char(&iter);
    }
    flush_text_run(out, run, run_flags);
    g_string_free(run, TRUE);

    guint8 end = REC_END;            /* terminating record                  */
    g_byte_array_append(out, &end, 1);

    *out_len = out->len;
    return g_byte_array_free(out, FALSE);
}

/* ---------------------------------------------------------------------------
 * get_u32() — read a little-endian u32, advancing *pos.
 *   data — blob bytes.
 *   len  — blob length.
 *   pos  — in/out read cursor.
 *   out  — receives the value.
 * Returns FALSE if fewer than 4 bytes remain.
 * ------------------------------------------------------------------------- */
static gboolean
get_u32(const guint8 *data, gsize len, gsize *pos, guint32 *out)
{
    if (*pos + 4 > len)
        return FALSE;
    *out = (guint32)data[*pos]
         | ((guint32)data[*pos + 1] << 8)
         | ((guint32)data[*pos + 2] << 16)
         | ((guint32)data[*pos + 3] << 24);
    *pos += 4;
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * insert_with_flags() — insert `text` at the buffer end with every tag
 * named by `flags` applied.
 *   buffer — destination buffer.
 *   text   — UTF-8 text to insert.
 *   n      — byte length of `text`.
 *   flags  — ON_FMT_* bits to apply.
 * ------------------------------------------------------------------------- */
static void
insert_with_flags(GtkTextBuffer *buffer, const gchar *text, gssize n,
                  guint32 flags)
{
    GtkTextIter end;                 /* insertion point (buffer end)        */
    gtk_text_buffer_get_end_iter(buffer, &end);

    /* Remember where the inserted span starts so tags can be applied.      */
    gint start_offset = gtk_text_iter_get_offset(&end);
    gtk_text_buffer_insert(buffer, &end, text, n);

    if (flags == 0)
        return;

    GtkTextIter start;               /* start of the span just inserted     */
    gtk_text_buffer_get_iter_at_offset(buffer, &start, start_offset);
    gtk_text_buffer_get_end_iter(buffer, &end);

    for (gsize i = 0; i < on_n_flag_tags; i++) {
        if (flags & on_flag_tags[i].flag)
            gtk_text_buffer_apply_tag_by_name(
                buffer, on_flag_tags[i].tag_name, &start, &end);
    }
}

/* ---------------------------------------------------------------------------
 * on_size_prepared() — GdkPixbufLoader callback capping decode size:
 * shrink to at most `max_px` (passed via user_data) on the longest side,
 * preserving aspect ratio.  Never upscales.
 * ------------------------------------------------------------------------- */
static void
on_size_prepared(GdkPixbufLoader *loader, gint width, gint height,
                 gpointer user_data)
{
    gint max_px = GPOINTER_TO_INT(user_data);
    gint longest = MAX(width, height);
    if (longest > max_px) {
        gdouble scale = (gdouble)max_px / longest;
        gdk_pixbuf_loader_set_size(loader,
                                   MAX(1, (gint)(width * scale)),
                                   MAX(1, (gint)(height * scale)));
    }
}

gboolean
on_note_deserialize(GtkTextBuffer *buffer, const guint8 *data, gsize len)
{
    return on_note_deserialize_scaled(buffer, data, len, 0);
}

gboolean
on_note_deserialize_scaled(GtkTextBuffer *buffer, const guint8 *data,
                           gsize len, gint max_img_px)
{
    on_buffer_ensure_tags(buffer);
    gtk_text_buffer_set_text(buffer, "", -1);

    /* Validate header (current BNBF magic, or legacy ONBF).                */
    if (!magic_ok(data, len)) {
        g_warning("deserialize: bad or missing BNBF header");
        return FALSE;
    }
    gsize   pos = 4;                 /* read cursor, past the magic         */
    guint32 version;                 /* format version from the header      */
    if (!get_u32(data, len, &pos, &version) ||
        version < 1 || version > BNBF_VERSION) {
        g_warning("deserialize: unsupported BNBF version");
        return FALSE;
    }

    while (pos < len) {
        guint8 rec = data[pos++];    /* record type byte                    */
        if (rec == REC_END)
            return TRUE;

        if (rec == REC_TEXT) {
            guint32 flags, n;        /* run formatting and byte length      */
            if (!get_u32(data, len, &pos, &flags) ||
                !get_u32(data, len, &pos, &n)     ||
                pos + n > len) {
                g_warning("deserialize: truncated TEXT record");
                return FALSE;
            }
            insert_with_flags(buffer, (const gchar *)data + pos,
                              (gssize)n, flags);
            pos += n;
        } else if (rec == REC_IMAGE) {
            guint32 display_width = 0;   /* stored display width (v2+)      */
            if (version >= 2 &&
                !get_u32(data, len, &pos, &display_width)) {
                g_warning("deserialize: truncated IMAGE record");
                return FALSE;
            }
            guint32 n;               /* PNG byte length                     */
            if (!get_u32(data, len, &pos, &n) || pos + n > len) {
                g_warning("deserialize: truncated IMAGE record");
                return FALSE;
            }
            /* Decode the PNG bytes and embed an image-carrying anchor.
             * Widgets (for on-screen display) are attached separately by
             * the editor; offscreen consumers just read the anchor data.   */
            GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
            if (max_img_px > 0)
                g_signal_connect(loader, "size-prepared",
                                 G_CALLBACK(on_size_prepared),
                                 GINT_TO_POINTER(max_img_px));
            GError *err = NULL;
            if (gdk_pixbuf_loader_write(loader, data + pos, n, &err) &&
                gdk_pixbuf_loader_close(loader, &err)) {
                GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
                if (pixbuf != NULL) {
                    GtkTextIter end;
                    gtk_text_buffer_get_end_iter(buffer, &end);
                    GtkTextChildAnchor *anchor =
                        gtk_text_buffer_create_child_anchor(buffer, &end);
                    on_anchor_set_image(anchor, pixbuf,
                                        (gint)display_width);
                    /* Full-resolution load: keep the source PNG bytes on
                     * the pixbuf so saves emit them verbatim instead of
                     * re-encoding (see the "on-png" cache in
                     * on_note_serialize).  Scaled loads (thumbnails)
                     * never save, and their pixbuf no longer matches the
                     * bytes — skip those.                                  */
                    if (max_img_px == 0)
                        g_object_set_data_full(G_OBJECT(pixbuf), "on-png",
                            g_bytes_new(data + pos, n),
                            (GDestroyNotify)g_bytes_unref);
                }
            } else {
                g_warning("deserialize: bad image data: %s",
                          err != NULL ? err->message : "unknown");
                g_clear_error(&err);
            }
            g_object_unref(loader);
            pos += n;
        } else if (rec == REC_TABLE) {
            guint32 tflags = 0;      /* table flags (v4+)                   */
            if (version >= 4 && !get_u32(data, len, &pos, &tflags)) {
                g_warning("deserialize: truncated TABLE record");
                return FALSE;
            }
            guint32 rows, cols;      /* table dimensions                    */
            if (!get_u32(data, len, &pos, &rows) ||
                !get_u32(data, len, &pos, &cols) ||
                rows == 0 || cols == 0 || rows > 1024 || cols > 1024) {
                g_warning("deserialize: bad TABLE record");
                return FALSE;
            }
            OnTable *table = on_table_new((gint)rows, (gint)cols);
            table->header = (tflags & TABLE_FLAG_HEADER) != 0;
            for (guint32 i = 0; i < rows * cols; i++) {
                guint32 n;           /* cell byte length                    */
                if (!get_u32(data, len, &pos, &n) || pos + n > len) {
                    g_warning("deserialize: truncated TABLE cell");
                    on_table_free(table);
                    return FALSE;
                }
                gchar *cell = g_strndup((const gchar *)data + pos, n);
                on_table_set(table, (gint)(i / cols), (gint)(i % cols),
                             cell);
                g_free(cell);
                pos += n;
            }
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(buffer, &end);
            GtkTextChildAnchor *anchor =
                gtk_text_buffer_create_child_anchor(buffer, &end);
            on_anchor_set_table(anchor, table);
        } else if (rec == REC_CHECK) {
            if (pos >= len) {
                g_warning("deserialize: truncated CHECK record");
                return FALSE;
            }
            guint8 state = data[pos++];  /* 0 = unchecked, 1 = checked      */
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(buffer, &end);
            GtkTextChildAnchor *anchor =
                gtk_text_buffer_create_child_anchor(buffer, &end);
            on_anchor_set_checkbox(anchor, state != 0);
        } else {
            g_warning("deserialize: unknown record type 0x%02x", rec);
            return FALSE;
        }
    }
    /* Ran off the end without seeing REC_END — tolerate but report.        */
    g_warning("deserialize: missing end marker");
    return FALSE;
}

void
on_anchor_set_checkbox(GtkTextChildAnchor *anchor, gboolean checked)
{
    /* Encoded as 1 (unchecked) / 2 (checked) so NULL means "no checkbox". */
    g_object_set_data(G_OBJECT(anchor), "on-checkbox",
                      GINT_TO_POINTER(checked ? 2 : 1));
}

gboolean
on_anchor_is_checkbox(GtkTextChildAnchor *anchor, gboolean *out_checked)
{
    gint v = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(anchor),
                                               "on-checkbox"));
    if (out_checked != NULL)
        *out_checked = (v == 2);
    return v != 0;
}

gboolean
on_char_is_checkbox(gunichar c, gboolean *out_checked)
{
    gboolean checked = (c == 0x2705 || c == 0x2611);   /* ✅ or legacy ☑    */
    gboolean is_box  = checked ||
                       c == 0x2B1C || c == 0x2610;     /* ⬜ or legacy ☐    */
    if (out_checked != NULL)
        *out_checked = checked;
    return is_box;
}

glong
on_list_prefix_chars(const gchar *head)
{
    if (g_str_has_prefix(head, "\xe2\x80\xa2 ") ||
        (on_char_is_checkbox(g_utf8_get_char(head), NULL) &&
         g_utf8_get_char(g_utf8_next_char(head)) == ' '))
        return 2;                    /* glyph + one space                   */

    glong d = 0;                     /* leading digit characters            */
    while (g_ascii_isdigit(head[d]))
        d++;
    if (d > 0 && head[d] == '.' && head[d + 1] == ' ')
        return d + 2;                /* "12. "                              */
    return 0;
}

OnTable *
on_table_new(gint rows, gint cols)
{
    OnTable *t = g_new0(OnTable, 1);
    t->rows  = MAX(1, rows);
    t->cols  = MAX(1, cols);
    t->cells = g_ptr_array_new_with_free_func(g_free);
    for (gint i = 0; i < t->rows * t->cols; i++)
        g_ptr_array_add(t->cells, g_strdup(""));
    return t;
}

void
on_table_free(OnTable *table)
{
    if (table == NULL)
        return;
    g_ptr_array_free(table->cells, TRUE);
    g_free(table);
}

const gchar *
on_table_get(OnTable *table, gint r, gint c)
{
    if (r < 0 || r >= table->rows || c < 0 || c >= table->cols)
        return "";
    return g_ptr_array_index(table->cells, r * table->cols + c);
}

void
on_table_set(OnTable *table, gint r, gint c, const gchar *text)
{
    if (r < 0 || r >= table->rows || c < 0 || c >= table->cols)
        return;
    gint i = r * table->cols + c;    /* row-major cell index                */
    g_free(g_ptr_array_index(table->cells, i));
    g_ptr_array_index(table->cells, i) =
        g_strdup(text != NULL ? text : "");
}

void
on_table_resize(OnTable *table, gint rows, gint cols)
{
    rows = MAX(1, rows);
    cols = MAX(1, cols);

    /* Build the new cell array, carrying over overlapping content.         */
    GPtrArray *cells = g_ptr_array_new_with_free_func(g_free);
    for (gint r = 0; r < rows; r++)
        for (gint c = 0; c < cols; c++)
            g_ptr_array_add(cells,
                            g_strdup((r < table->rows && c < table->cols)
                                     ? on_table_get(table, r, c) : ""));
    g_ptr_array_free(table->cells, TRUE);
    table->cells = cells;
    table->rows  = rows;
    table->cols  = cols;
}

void
on_anchor_set_table(GtkTextChildAnchor *anchor, OnTable *table)
{
    g_object_set_data_full(G_OBJECT(anchor), "on-table", table,
                           (GDestroyNotify)on_table_free);
}

OnTable *
on_anchor_get_table(GtkTextChildAnchor *anchor)
{
    return g_object_get_data(G_OBJECT(anchor), "on-table");
}

void
on_anchor_set_image(GtkTextChildAnchor *anchor, GdkPixbuf *original,
                    gint display_width)
{
    g_object_set_data_full(G_OBJECT(anchor), "on-original",
                           g_object_ref(original), g_object_unref);
    g_object_set_data(G_OBJECT(anchor), "on-display-width",
                      GINT_TO_POINTER(display_width));
}

GdkPixbuf *
on_anchor_get_image(GtkTextChildAnchor *anchor, gint *display_width)
{
    if (display_width != NULL)
        *display_width = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(anchor), "on-display-width"));
    return g_object_get_data(G_OBJECT(anchor), "on-original");
}

gchar *
on_note_extract_text(const guint8 *data, gsize len)
{
    GString *out = g_string_new(NULL);

    if (!magic_ok(data, len))
        return g_string_free(out, FALSE);
    gsize   pos = 4;                 /* read cursor, past the magic         */
    guint32 version;
    if (!get_u32(data, len, &pos, &version) ||
        version < 1 || version > BNBF_VERSION)
        return g_string_free(out, FALSE);

    while (pos < len) {
        guint8 rec = data[pos++];    /* record type byte                    */
        if (rec == REC_END)
            break;

        if (rec == REC_TEXT) {
            guint32 flags, n;
            if (!get_u32(data, len, &pos, &flags) ||
                !get_u32(data, len, &pos, &n) || pos + n > len)
                break;
            g_string_append_len(out, (const gchar *)data + pos, n);
            pos += n;
        } else if (rec == REC_IMAGE) {
            guint32 dw = 0, n;
            if (version >= 2 && !get_u32(data, len, &pos, &dw))
                break;
            if (!get_u32(data, len, &pos, &n) || pos + n > len)
                break;
            pos += n;                /* skip the PNG payload                */
        } else if (rec == REC_TABLE) {
            guint32 tflags = 0, rows, cols;
            if (version >= 4 && !get_u32(data, len, &pos, &tflags))
                break;
            if (!get_u32(data, len, &pos, &rows) ||
                !get_u32(data, len, &pos, &cols) ||
                rows == 0 || cols == 0 || rows > 1024 || cols > 1024)
                break;               /* same clamp as the deserializer      */
            gboolean bad = FALSE;    /* truncated cell encountered          */
            for (guint32 i = 0; i < rows * cols && !bad; i++) {
                guint32 n;
                if (!get_u32(data, len, &pos, &n) || pos + n > len) {
                    bad = TRUE;
                    break;
                }
                g_string_append_len(out, (const gchar *)data + pos, n);
                g_string_append_c(out, ' ');
                pos += n;
            }
            if (bad)
                break;
        } else if (rec == REC_CHECK) {
            if (pos >= len)
                break;
            pos += 1;                /* skip the state byte                 */
        } else {
            break;                   /* unknown record: stop safely         */
        }
    }
    return g_string_free(out, FALSE);
}

gchar *
on_buffer_first_line(GtkTextBuffer *buffer)
{
    GtkTextIter start, line_end;     /* span of the first line              */
    gtk_text_buffer_get_start_iter(buffer, &start);

    /* Skip leading blank lines so a note starting with newlines still
     * gets a meaningful title.                                             */
    while (gtk_text_iter_ends_line(&start) &&
           !gtk_text_iter_is_end(&start))
        gtk_text_iter_forward_line(&start);

    line_end = start;
    if (!gtk_text_iter_ends_line(&line_end))
        gtk_text_iter_forward_to_line_end(&line_end);

    gchar *text = gtk_text_buffer_get_text(buffer, &start, &line_end, FALSE);
    g_strstrip(text);

    if (*text == '\0') {
        g_free(text);
        return g_strdup("New Note");
    }
    /* Keep titles a sane length for the list views.                        */
    if (g_utf8_strlen(text, -1) > 80) {
        gchar *cut = g_utf8_substring(text, 0, 80);
        g_free(text);
        return cut;
    }
    return text;
}

GList *
on_buffer_collect_tags(GtkTextBuffer *buffer)
{
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, ON_TAGNAME_TAG);
    if (tag == NULL)
        return NULL;

    GList *names = NULL;             /* collected unique tag names          */
    GtkTextIter iter;                /* scan position                       */
    gtk_text_buffer_get_start_iter(buffer, &iter);

    /* Jump from tag-span to tag-span using forward_to_tag_toggle.          */
    while (TRUE) {
        if (!gtk_text_iter_starts_tag(&iter, tag)) {
            if (!gtk_text_iter_forward_to_tag_toggle(&iter, tag))
                break;
            if (!gtk_text_iter_starts_tag(&iter, tag))
                continue;
        }
        GtkTextIter span_end = iter; /* end of this tag span                */
        gtk_text_iter_forward_to_tag_toggle(&span_end, tag);

        gchar *text = gtk_text_buffer_get_text(buffer, &iter,
                                               &span_end, FALSE);
        /* Spans include the leading '#'; strip it and surrounding space.   */
        g_strstrip(text);
        const gchar *name = (*text == '#') ? text + 1 : text;
        if (*name != '\0' &&
            g_list_find_custom(names, name,
                               (GCompareFunc)g_strcmp0) == NULL)
            names = g_list_prepend(names, g_strdup(name));
        g_free(text);

        iter = span_end;
    }
    return g_list_reverse(names);
}
