/* ===========================================================================
 * serialize.h — BNBF binary note format
 *
 * Converts a GtkTextBuffer (rich text + inline images) to and from the
 * "Blue Notes Binary Format" (BNBF) blob stored in SQLite.
 *
 * BNBF layout (all integers little-endian):
 *
 *   [4 bytes]  magic "BNBF"
 *   [u32]      format version (currently 5; 1–4 are still readable)
 *   ...records...
 *   [u8 0x00]  end marker
 *
 * Record types:
 *   [u8 0x01]  TEXT  : [u32 flags] [u32 byte_len] [byte_len UTF-8 bytes]
 *   [u8 0x02]  IMAGE : [u32 display_width]            (version >= 2 only)
 *                      [u32 png_len] [png_len bytes of PNG data]
 *   [u8 0x03]  TABLE : [u32 tflags]                   (version >= 4 only;
 *                                                      bit 0 = header row)
 *                      [u32 rows] [u32 cols]          (version >= 3)
 *                      rows*cols x ([u32 len] [len UTF-8 bytes]) row-major
 *   [u8 0x04]  CHECK : [u8 state]                     (version >= 5)
 *                      a task-list checkbox (0 = unchecked, 1 = checked);
 *                      rendered as a native GtkCheckButton in the editor
 *
 * IMAGE records always hold the image at its ORIGINAL resolution; the
 * display_width field records how wide the user chose to show it in the
 * editor (0 = default thumbnail sizing).  Version 1 blobs lack the
 * display_width field.
 *
 * A TEXT record holds one "run": a maximal span of characters that all
 * share the same formatting flags.  Formatting is expressed as a bitmask
 * (ON_FMT_*) so the format is self-contained and easy to parse from any
 * language — this is also what export.c consumes indirectly by walking a
 * deserialized buffer.
 * =========================================================================== */

#ifndef BLUE_SERIALIZE_H
#define BLUE_SERIALIZE_H

#include <gtk/gtk.h>

/* ---------------------------------------------------------------------------
 * Formatting flag bits used in BNBF TEXT records.  Each bit corresponds to
 * exactly one named GtkTextTag (see ON_TAGNAME_* below).
 * ------------------------------------------------------------------------- */
typedef enum {
    ON_FMT_BOLD        = 1 << 0,   /* bold text                             */
    ON_FMT_ITALIC      = 1 << 1,   /* italic text                           */
    ON_FMT_UNDERLINE   = 1 << 2,   /* underlined text                       */
    ON_FMT_STRIKE      = 1 << 3,   /* strikethrough text                    */
    ON_FMT_H1          = 1 << 4,   /* heading level 1 (paragraph)           */
    ON_FMT_H2          = 1 << 5,   /* heading level 2 (paragraph)           */
    ON_FMT_CODEBLOCK   = 1 << 6,   /* monospace code block (paragraph)      */
    ON_FMT_LIST_BULLET = 1 << 7,   /* bulleted list item (paragraph)        */
    ON_FMT_LIST_NUMBER = 1 << 8,   /* numbered list item (paragraph)        */
    ON_FMT_TAG         = 1 << 9,   /* inline #tag token                     */
    ON_FMT_LIST_CHECK  = 1 << 10,  /* task-list item with checkbox (para)   */
} OnFormatFlags;

/* Task checkboxes are child anchors carrying their state as object data;
 * the editor attaches a native GtkCheckButton at each.                      */

/* on_anchor_set_checkbox() — mark an anchor as a task checkbox with the
 * given state.  on_anchor_is_checkbox() reads it back (returns FALSE for
 * non-checkbox anchors; out_checked may be NULL).                           */
void on_anchor_set_checkbox(GtkTextChildAnchor *anchor, gboolean checked);
gboolean on_anchor_is_checkbox(GtkTextChildAnchor *anchor,
                               gboolean *out_checked);

/* on_list_prefix_chars() — length in CHARACTERS of the literal list
 * prefix at the start of `head` ("\xe2\x80\xa2 " bullet or "12. "), or 0
 * if none.  `head` is a short UTF-8 probe
 * of the line start (callers pass ~7 chars).  The one parser both the
 * editor (prefix stripping) and the exporters use.                          */
glong on_list_prefix_chars(const gchar *head);

/* Names of the GtkTextTags the editor registers on every note buffer.
 * serialize.c maps between these tags and the ON_FMT_* bits.               */
#define ON_TAGNAME_BOLD        "on-bold"
#define ON_TAGNAME_ITALIC      "on-italic"
#define ON_TAGNAME_UNDERLINE   "on-underline"
#define ON_TAGNAME_STRIKE      "on-strike"
#define ON_TAGNAME_H1          "on-h1"
#define ON_TAGNAME_H2          "on-h2"
#define ON_TAGNAME_CODEBLOCK   "on-codeblock"
#define ON_TAGNAME_LIST_BULLET "on-list-bullet"
#define ON_TAGNAME_LIST_NUMBER "on-list-number"
#define ON_TAGNAME_LIST_CHECK  "on-list-check"
#define ON_TAGNAME_TAG         "on-tag"

/* Format-bit groups: the four inline (character) styles, and the six
 * mutually exclusive paragraph styles (applied to whole lines only —
 * apply_paragraph_format clears the others first, and loading heals any
 * stragglers, so at most one PARA bit is ever set on a character).          */
#define ON_FMT_INLINE_MASK (ON_FMT_BOLD | ON_FMT_ITALIC | \
                            ON_FMT_UNDERLINE | ON_FMT_STRIKE)
#define ON_FMT_PARA_MASK   (ON_FMT_H1 | ON_FMT_H2 | ON_FMT_CODEBLOCK | \
                            ON_FMT_LIST_BULLET | ON_FMT_LIST_NUMBER | \
                            ON_FMT_LIST_CHECK)

/* ---------------------------------------------------------------------------
 * The canonical flag ⇄ tag-name table.  THE single copy in the program —
 * serializer, editor, undo and export all iterate it, so the mapping can
 * never fall out of sync.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnFormatFlags flag;              /* the bitmask bit                     */
    const gchar  *tag_name;          /* the GtkTextTag name it maps to      */
} OnFlagTag;
extern const OnFlagTag on_flag_tags[];
extern const gsize     on_n_flag_tags;

/* ---------------------------------------------------------------------------
 * on_flags_at_iter() — the ON_FMT_* bits (restricted to `mask`) whose
 * tags cover the character at `iter`.
 *   buffer — buffer owning the tag table.
 *   iter   — position to inspect.
 *   mask   — which bits to test (ON_FMT_INLINE_MASK, ON_FMT_PARA_MASK,
 *            or ~0u for all).
 * ------------------------------------------------------------------------- */
guint32 on_flags_at_iter(GtkTextBuffer *buffer, const GtkTextIter *iter,
                         guint32 mask);

/* ---------------------------------------------------------------------------
 * on_tag_name_for_flag() — the GtkTextTag name for one ON_FMT_* bit, or
 * NULL if the bit is unknown.
 * ------------------------------------------------------------------------- */
const gchar *on_tag_name_for_flag(guint32 flag);

/* ---------------------------------------------------------------------------
 * on_buffer_ensure_tags() — create the standard Blue Notes tag set on
 * `buffer`'s tag table if not already present.  Both the editor window and
 * the exporter call this before touching a buffer, so the two always agree
 * on tag names and appearance.
 *   buffer — the text buffer to prepare.
 * ------------------------------------------------------------------------- */
void on_buffer_ensure_tags(GtkTextBuffer *buffer);

/* ---------------------------------------------------------------------------
 * on_note_serialize() — flatten a buffer into a newly allocated BNBF blob.
 *   buffer  — source buffer (must have been through on_buffer_ensure_tags).
 *   out_len — receives the blob size in bytes.
 * Returns a g_malloc'd byte array (g_free() it), or NULL on error.
 * ------------------------------------------------------------------------- */
guint8 *on_note_serialize(GtkTextBuffer *buffer, gsize *out_len);

/* ---------------------------------------------------------------------------
 * on_note_deserialize() — replace `buffer`'s contents with the note stored
 * in a BNBF blob.
 *   buffer — destination buffer (tags are ensured automatically).
 *   data   — BNBF bytes as loaded from SQLite.
 *   len    — length of `data`.
 * Returns TRUE if the blob parsed cleanly; on FALSE the buffer may hold a
 * partial document (best-effort recovery).
 * ------------------------------------------------------------------------- */
gboolean on_note_deserialize(GtkTextBuffer *buffer, const guint8 *data,
                             gsize len);

/* ---------------------------------------------------------------------------
 * on_note_deserialize_scaled() — like on_note_deserialize(), but images
 * are DECODED at no more than `max_img_px` on their longest side (0 =
 * full resolution).  For consumers that only render small previews
 * (grid thumbnails), this avoids inflating multi-megapixel bitmaps that
 * are immediately shrunk to card size.
 * ------------------------------------------------------------------------- */
gboolean on_note_deserialize_scaled(GtkTextBuffer *buffer,
                                    const guint8 *data, gsize len,
                                    gint max_img_px);

/* ---------------------------------------------------------------------------
 * on_note_extract_text() — pull the searchable plain text out of a BNBF
 * blob WITHOUT building a GtkTextBuffer or decoding any images: TEXT
 * runs are concatenated, table cells are appended (space-separated), and
 * image/checkbox payloads are skipped.  Orders of magnitude cheaper than
 * a full deserialize; used to (back)fill the notes.body_text column.
 * Returns a newly allocated string; g_free() it.
 * ------------------------------------------------------------------------- */
gchar *on_note_extract_text(const guint8 *data, gsize len);

/* ---------------------------------------------------------------------------
 * on_note_extract_actions() — pull the ACTION ITEMS out of a BNBF blob
 * without building a GtkTextBuffer (same cheap record walk as
 * on_note_extract_text).  An action item is a line whose first character
 * is '!' outside a code block (an embedded image/table/checkbox occupies
 * the first slot like any character, so such lines never qualify): its
 * text is the rest of the line, trimmed, and it is "done" when every
 * non-space character of that rest carries ON_FMT_STRIKE.  Lines with
 * nothing after the '!' are ignored.
 * Returns a GList of OnActionItem (db.h; ord = list position, note_id
 * left 0); free with on_db_action_list_free().
 * ------------------------------------------------------------------------- */
GList *on_note_extract_actions(const guint8 *data, gsize len);

/* ---------------------------------------------------------------------------
 * on_buffer_first_line() — extract the note title: the text of the first
 * non-empty line, or "New Note" if the buffer is empty.
 *   buffer — buffer to inspect.
 * Returns a newly allocated string; g_free() it.
 * ------------------------------------------------------------------------- */
gchar *on_buffer_first_line(GtkTextBuffer *buffer);

/* ---------------------------------------------------------------------------
 * Images are embedded as GtkTextChildAnchors (not raw pixbufs): the anchor
 * carries the FULL-RESOLUTION image plus the user's chosen display width
 * as object data, and the editor attaches a HiDPI-aware GtkImage widget
 * at each anchor.  Offscreen consumers (export, search, thumbnails) read
 * the anchor data directly and never need widgets.
 * ------------------------------------------------------------------------- */

/* on_anchor_set_image() — attach an image to an anchor.
 *   anchor        — the anchor embedded in the buffer.
 *   original      — full-resolution image (a reference is taken).
 *   display_width — chosen on-screen (logical) width; <= 0 means the
 *                   default thumbnail size.                                 */
void on_anchor_set_image(GtkTextChildAnchor *anchor, GdkPixbuf *original,
                         gint display_width);

/* on_anchor_get_image() — read an anchor's image.
 *   anchor        — the anchor to inspect.
 *   display_width — optional; receives the stored display width.
 * Returns the original pixbuf (borrowed ref), or NULL if this anchor
 * carries no image.                                                         */
GdkPixbuf *on_anchor_get_image(GtkTextChildAnchor *anchor,
                               gint *display_width);

/* Bounding box for the default thumbnail display of images (logical px):
 * a freshly inserted image is scaled to fit inside this, aspect kept;
 * enlarge via the image's right-click menu.                                 */
#define ON_IMAGE_THUMB_W 200
#define ON_IMAGE_THUMB_H 125

/* ---------------------------------------------------------------------------
 * OnTable — the data behind an embedded table anchor.
 *
 * Fields:
 *   rows/cols — current dimensions.
 *   header    — whether the first row is styled/exported as a header.
 *   cells     — rows*cols owned strings, row-major (never NULL entries;
 *               cell text may contain newlines).
 * ------------------------------------------------------------------------- */
typedef struct {
    gint       rows;
    gint       cols;
    gboolean   header;
    GPtrArray *cells;
} OnTable;

/* on_table_new() — a rows×cols table of empty cells.                        */
OnTable *on_table_new(gint rows, gint cols);

/* on_table_free() — release a table and its cell strings.                   */
void on_table_free(OnTable *table);

/* on_table_get()/on_table_set() — cell access (row r, column c).            */
const gchar *on_table_get(OnTable *table, gint r, gint c);
void on_table_set(OnTable *table, gint r, gint c, const gchar *text);

/* on_table_resize() — grow/shrink to rows×cols, preserving overlapping
 * cells (new cells become empty; dimensions clamp to at least 1×1).         */
void on_table_resize(OnTable *table, gint rows, gint cols);

/* on_anchor_set_table() — attach `table` to an anchor (ownership passes
 * to the anchor).  on_anchor_get_table() reads it back (borrowed).          */
void on_anchor_set_table(GtkTextChildAnchor *anchor, OnTable *table);
OnTable *on_anchor_get_table(GtkTextChildAnchor *anchor);

/* ---------------------------------------------------------------------------
 * on_buffer_collect_tags() — collect the distinct #tag names present in
 * the buffer (spans carrying ON_TAGNAME_TAG), without the leading '#'.
 *   buffer — buffer to scan.
 * Returns a GList of newly allocated strings; free with
 * g_list_free_full(list, g_free).
 * ------------------------------------------------------------------------- */
GList *on_buffer_collect_tags(GtkTextBuffer *buffer);

#endif /* BLUE_SERIALIZE_H */
