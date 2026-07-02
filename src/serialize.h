/* ===========================================================================
 * serialize.h — ONBF binary note format
 *
 * Converts a GtkTextBuffer (rich text + inline images) to and from the
 * "Orange Notes Binary Format" (ONBF) blob stored in SQLite.
 *
 * ONBF layout (all integers little-endian):
 *
 *   [4 bytes]  magic "ONBF"
 *   [u32]      format version (currently 2; version 1 is still readable)
 *   ...records...
 *   [u8 0x00]  end marker
 *
 * Record types:
 *   [u8 0x01]  TEXT  : [u32 flags] [u32 byte_len] [byte_len UTF-8 bytes]
 *   [u8 0x02]  IMAGE : [u32 display_width]            (version >= 2 only)
 *                      [u32 png_len] [png_len bytes of PNG data]
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

#ifndef ORANGE_SERIALIZE_H
#define ORANGE_SERIALIZE_H

#include <gtk/gtk.h>

/* ---------------------------------------------------------------------------
 * Formatting flag bits used in ONBF TEXT records.  Each bit corresponds to
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
} OnFormatFlags;

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
#define ON_TAGNAME_TAG         "on-tag"

/* ---------------------------------------------------------------------------
 * on_buffer_ensure_tags() — create the standard Orange Notes tag set on
 * `buffer`'s tag table if not already present.  Both the editor window and
 * the exporter call this before touching a buffer, so the two always agree
 * on tag names and appearance.
 *   buffer — the text buffer to prepare.
 * ------------------------------------------------------------------------- */
void on_buffer_ensure_tags(GtkTextBuffer *buffer);

/* ---------------------------------------------------------------------------
 * on_note_serialize() — flatten a buffer into a newly allocated ONBF blob.
 *   buffer  — source buffer (must have been through on_buffer_ensure_tags).
 *   out_len — receives the blob size in bytes.
 * Returns a g_malloc'd byte array (g_free() it), or NULL on error.
 * ------------------------------------------------------------------------- */
guint8 *on_note_serialize(GtkTextBuffer *buffer, gsize *out_len);

/* ---------------------------------------------------------------------------
 * on_note_deserialize() — replace `buffer`'s contents with the note stored
 * in an ONBF blob.
 *   buffer — destination buffer (tags are ensured automatically).
 *   data   — ONBF bytes as loaded from SQLite.
 *   len    — length of `data`.
 * Returns TRUE if the blob parsed cleanly; on FALSE the buffer may hold a
 * partial document (best-effort recovery).
 * ------------------------------------------------------------------------- */
gboolean on_note_deserialize(GtkTextBuffer *buffer, const guint8 *data,
                             gsize len);

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

/* Default on-screen (logical) width for freshly inserted images — small
 * thumbnails by default; enlarge via the image's right-click menu.         */
#define ON_IMAGE_DEFAULT_WIDTH 320

/* ---------------------------------------------------------------------------
 * on_buffer_collect_tags() — collect the distinct #tag names present in
 * the buffer (spans carrying ON_TAGNAME_TAG), without the leading '#'.
 *   buffer — buffer to scan.
 * Returns a GList of newly allocated strings; free with
 * g_list_free_full(list, g_free).
 * ------------------------------------------------------------------------- */
GList *on_buffer_collect_tags(GtkTextBuffer *buffer);

#endif /* ORANGE_SERIALIZE_H */
