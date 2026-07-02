/* ===========================================================================
 * editor_window.c — WYSIWYG note editor window (implementation)
 *
 * See editor_window.h for the feature overview.  The interesting moving
 * parts in here:
 *
 *   inline formatting  — a "current flags" model: the editor tracks which
 *                        inline styles (bold/italic/…) are active and
 *                        enforces them on newly typed characters, exactly
 *                        like a word processor.
 *
 *   paragraph styles   — headings, lists and code blocks are GtkTextTags
 *                        applied to whole lines (including the newline).
 *                        List items additionally carry a literal "• " or
 *                        "1. " prefix; Enter continues the list and Enter
 *                        on an empty item ends it.
 *
 *   #tag capture       — typing '#' begins a capture: a popup offers
 *                        matching known tags; space (or Enter/click-away)
 *                        ends the tag; Escape cancels it.
 *
 *   autosave           — every buffer change re-arms a short timer; when
 *                        it fires the buffer is serialized to ONBF and
 *                        written to SQLite, and the note's tag set is
 *                        recomputed from the text.
 * =========================================================================== */

#include "editor_window.h"
#include "serialize.h"

#include <gdk/gdkkeysyms.h>
#include <string.h>

/* Milliseconds of idle time after the last edit before autosaving.         */
#define AUTOSAVE_DELAY_MS 1200

/* Maximum number of suggestions shown in the tag popup.                    */
#define TAG_POPUP_MAX 8

/* Width reserved for the floating code-block copy button, and its inset
 * from the block's shaded top and right edges.                             */
#define CODE_BTN_SIZE   22
#define CODE_BTN_MARGIN 4

/* Horizontal margin the code-block tag applies (see on_buffer_ensure_tags
 * in serialize.c) — the shaded background ends this far from the window
 * edge, and the copy button must sit inside it.                            */
#define CODEBLOCK_RIGHT_MARGIN 24

/* The inline (character-level) formatting bits.                            */
#define INLINE_MASK (ON_FMT_BOLD | ON_FMT_ITALIC | \
                     ON_FMT_UNDERLINE | ON_FMT_STRIKE)

/* The paragraph (line-level) formatting bits.                              */
#define PARA_MASK (ON_FMT_H1 | ON_FMT_H2 | ON_FMT_CODEBLOCK | \
                   ON_FMT_LIST_BULLET | ON_FMT_LIST_NUMBER)

/* ---------------------------------------------------------------------------
 * OnEditor — all state for one open editor window.
 *
 * Fields:
 *   app             — global application context (not owned).
 *   note_id         — id of the note being edited.
 *   window          — the top-level GtkWindow.
 *   view            — the GtkTextView doing the editing.
 *   buffer          — the view's buffer; we hold an extra ref so the final
 *                     save on window destroy can still read it.
 *   inline_flags    — ON_FMT_* bits applied to newly typed characters.
 *   internal_change — nesting counter; >0 while *we* mutate the buffer
 *                     programmatically, so signal handlers know to ignore
 *                     the resulting insert/delete events.
 *   autosave_source — GLib timeout id of the pending autosave, 0 if none.
 *   toggle_buttons  — the four inline-style toggle tool buttons, indexed
 *                     in the same order as INLINE_TOGGLES[], used to
 *                     mirror inline_flags into the toolbar UI.
 *   tag_start       — text mark placed just before a '#' while a tag is
 *                     being typed; NULL when no capture is active.
 *   tag_popup       — popup window listing matching tags (lazily built).
 *   tag_listbox     — GtkListBox inside tag_popup holding suggestions.
 *   code_buttons    — floating "copy" buttons, one per code block, added
 *                     as children of the text window (upper-right corner
 *                     of each block).
 *   code_btn_idle   — idle source id for a pending code-button rebuild.
 *   popup_x/popup_y — text-window coordinates of the last right click,
 *                     used by the populate-popup handler to find the
 *                     image (if any) under the pointer.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnApp          *app;
    gint64          note_id;
    GtkWidget      *window;
    GtkTextView    *view;
    GtkTextBuffer  *buffer;

    guint32         inline_flags;
    gint            internal_change;
    guint           autosave_source;

    GtkWidget      *toggle_buttons[4];

    GtkTextMark    *tag_start;
    GtkWidget      *tag_popup;
    GtkWidget      *tag_listbox;

    GSList         *code_buttons;
    guint           code_btn_idle;
    gint            popup_x;
    gint            popup_y;
} OnEditor;

/* ---------------------------------------------------------------------------
 * INLINE_TOGGLES — table describing the four inline-style toggle buttons:
 * their flag bit, tag name, icon file, fallback markup, label and tooltip.
 * ------------------------------------------------------------------------- */
static const struct {
    OnFormatFlags flag;              /* bit this button controls            */
    const gchar  *tag_name;         /* GtkTextTag it applies               */
    const gchar  *icon;             /* local icon file basename            */
    const gchar  *markup;           /* icon fallback markup                */
    const gchar  *label;            /* button text label                   */
    const gchar  *tooltip;          /* hover help text                     */
} INLINE_TOGGLES[4] = {
    { ON_FMT_BOLD,      ON_TAGNAME_BOLD,      NULL,
      "<b>B</b>", "Bold",      "Bold (Ctrl+B)" },
    { ON_FMT_ITALIC,    ON_TAGNAME_ITALIC,    NULL,
      "<i>I</i>", "Italic",    "Italic (Ctrl+I)" },
    { ON_FMT_UNDERLINE, ON_TAGNAME_UNDERLINE, NULL,
      "<u>U</u>", "Underline", "Underline (Ctrl+U)" },
    { ON_FMT_STRIKE,    ON_TAGNAME_STRIKE,    NULL,
      "<s>S</s>", "Strike",    "Strikethrough" },
};

/* Forward declarations for callbacks referenced before their definition.   */
static void     editor_save(OnEditor *ed);
static void     editor_queue_autosave(OnEditor *ed);
static void     tag_capture_end(OnEditor *ed, gboolean apply);
static void     tag_popup_update(OnEditor *ed);
static void     renumber_list_block(OnEditor *ed, gint line);
static gboolean line_strip_list_prefix(OnEditor *ed, GtkTextIter *line_start);
static void     code_buttons_queue_rebuild(OnEditor *ed);

/* ===========================================================================
 * small helpers
 * =========================================================================== */

/* lookup_tag() — fetch a named GtkTextTag from the buffer's tag table.      */
static GtkTextTag *
lookup_tag(GtkTextBuffer *buffer, const gchar *name)
{
    return gtk_text_tag_table_lookup(
        gtk_text_buffer_get_tag_table(buffer), name);
}

/* ---------------------------------------------------------------------------
 * iter_inline_flags() — the INLINE_MASK bits in effect at `iter`, derived
 * by checking each inline tag.
 * ------------------------------------------------------------------------- */
static guint32
iter_inline_flags(GtkTextBuffer *buffer, const GtkTextIter *iter)
{
    guint32 flags = 0;               /* accumulated bits                    */
    for (gsize i = 0; i < G_N_ELEMENTS(INLINE_TOGGLES); i++) {
        GtkTextTag *tag = lookup_tag(buffer, INLINE_TOGGLES[i].tag_name);
        if (tag != NULL && gtk_text_iter_has_tag(iter, tag))
            flags |= INLINE_TOGGLES[i].flag;
    }
    return flags;
}

/* ---------------------------------------------------------------------------
 * line_para_flags() — which PARA_MASK style the line containing `iter`
 * carries (0 if plain body text).  Paragraph tags are applied to whole
 * lines, so testing the first character is sufficient; for an empty line
 * the newline position itself is tested.
 * ------------------------------------------------------------------------- */
static guint32
line_para_flags(GtkTextBuffer *buffer, const GtkTextIter *iter)
{
    GtkTextIter ls = *iter;          /* start of the line                   */
    gtk_text_iter_set_line_offset(&ls, 0);

    static const struct { OnFormatFlags flag; const gchar *name; } PARA[] = {
        { ON_FMT_H1,          ON_TAGNAME_H1          },
        { ON_FMT_H2,          ON_TAGNAME_H2          },
        { ON_FMT_CODEBLOCK,   ON_TAGNAME_CODEBLOCK   },
        { ON_FMT_LIST_BULLET, ON_TAGNAME_LIST_BULLET },
        { ON_FMT_LIST_NUMBER, ON_TAGNAME_LIST_NUMBER },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(PARA); i++) {
        GtkTextTag *tag = lookup_tag(buffer, PARA[i].name);
        if (tag != NULL && gtk_text_iter_has_tag(&ls, tag))
            return PARA[i].flag;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * line_span() — compute [start, end) covering one whole line *including*
 * its trailing newline (so paragraph tags cover the newline and typing at
 * end-of-line inherits them).
 *   buffer — the buffer.
 *   line   — line number.
 *   start  — receives the line start.
 *   end    — receives the start of the next line (or buffer end).
 * ------------------------------------------------------------------------- */
static void
line_span(GtkTextBuffer *buffer, gint line, GtkTextIter *start,
          GtkTextIter *end)
{
    gtk_text_buffer_get_iter_at_line(buffer, start, line);
    *end = *start;
    if (!gtk_text_iter_ends_line(end))
        gtk_text_iter_forward_to_line_end(end);
    /* Step over the newline so it is part of the span.                     */
    if (!gtk_text_iter_is_end(end))
        gtk_text_iter_forward_char(end);
}

/* ---------------------------------------------------------------------------
 * update_toggle_buttons() — mirror ed->inline_flags into the four toolbar
 * toggle buttons without re-triggering their "toggled" handlers (each
 * handler is blocked by function+data while the state is pushed).
 * ------------------------------------------------------------------------- */
static void on_inline_toggle(GtkToggleToolButton *btn, gpointer user_data);

static void
update_toggle_buttons(OnEditor *ed)
{
    for (gsize i = 0; i < G_N_ELEMENTS(INLINE_TOGGLES); i++) {
        GtkToggleToolButton *btn =
            GTK_TOGGLE_TOOL_BUTTON(ed->toggle_buttons[i]);
        g_signal_handlers_block_by_func(btn, on_inline_toggle, ed);
        gtk_toggle_tool_button_set_active(
            btn, (ed->inline_flags & INLINE_TOGGLES[i].flag) != 0);
        g_signal_handlers_unblock_by_func(btn, on_inline_toggle, ed);
    }
}

/* ===========================================================================
 * inline formatting
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * toggle_inline_format() — handle a bold/italic/underline/strike request.
 * With a selection: toggle the tag across the selected range.  Without:
 * flip the bit in inline_flags so upcoming typed text uses it.
 *   ed   — the editor.
 *   flag — which inline style to toggle.
 * ------------------------------------------------------------------------- */
static void
toggle_inline_format(OnEditor *ed, OnFormatFlags flag)
{
    /* Find this flag's tag name in the toggle table.                       */
    const gchar *tag_name = NULL;    /* tag name matching `flag`            */
    for (gsize i = 0; i < G_N_ELEMENTS(INLINE_TOGGLES); i++)
        if (INLINE_TOGGLES[i].flag == flag)
            tag_name = INLINE_TOGGLES[i].tag_name;
    if (tag_name == NULL)
        return;

    GtkTextIter sel_start, sel_end;  /* selection bounds, if any            */
    if (gtk_text_buffer_get_selection_bounds(ed->buffer,
                                             &sel_start, &sel_end)) {
        /* Toggle over the selection: if the first selected char already
         * has the style, remove it everywhere; otherwise apply it.         */
        GtkTextTag *tag = lookup_tag(ed->buffer, tag_name);
        gboolean has = gtk_text_iter_has_tag(&sel_start, tag);
        ed->internal_change++;
        if (has)
            gtk_text_buffer_remove_tag(ed->buffer, tag,
                                       &sel_start, &sel_end);
        else
            gtk_text_buffer_apply_tag(ed->buffer, tag,
                                      &sel_start, &sel_end);
        ed->internal_change--;
        ed->inline_flags = has ? (ed->inline_flags & ~(guint32)flag)
                               : (ed->inline_flags | (guint32)flag);
        editor_queue_autosave(ed);
    } else {
        /* No selection: arm/disarm the style for upcoming typing.          */
        ed->inline_flags ^= (guint32)flag;
    }
    update_toggle_buttons(ed);
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
}

/* on_inline_toggle() — "toggled" handler for the four style buttons.  The
 * flag each button controls is stashed on it as object data "on-flag".     */
static void
on_inline_toggle(GtkToggleToolButton *btn, gpointer user_data)
{
    OnEditor *ed  = user_data;       /* owning editor                       */
    guint32  flag = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn),
                                                       "on-flag"));
    toggle_inline_format(ed, (OnFormatFlags)flag);
}

/* ===========================================================================
 * paragraph formatting (headings, lists, code blocks)
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * line_strip_list_prefix() — if the line starting at `line_start` begins
 * with a literal list prefix ("• " or "12. "), delete it.
 *   ed         — the editor (for internal_change bookkeeping by caller).
 *   line_start — iterator at the line start; revalidated after deletion.
 * Returns TRUE if a prefix was removed.
 * ------------------------------------------------------------------------- */
static gboolean
line_strip_list_prefix(OnEditor *ed, GtkTextIter *line_start)
{
    GtkTextIter probe_end = *line_start;   /* end of the probe window       */
    /* A prefix is at most "9999. " — 6 chars; don't run past the line.     */
    for (gint i = 0; i < 7 && !gtk_text_iter_ends_line(&probe_end); i++)
        gtk_text_iter_forward_char(&probe_end);

    gchar *head = gtk_text_buffer_get_text(ed->buffer, line_start,
                                           &probe_end, FALSE);
    glong strip_chars = 0;           /* how many characters to delete       */

    if (g_str_has_prefix(head, "\xe2\x80\xa2 ")) {
        strip_chars = 2;             /* "• " is one char + one space        */
    } else {
        /* Try "<digits>. " — count digits then require ". ".               */
        glong d = 0;                 /* number of leading digit chars       */
        while (g_ascii_isdigit(head[d]))
            d++;
        if (d > 0 && head[d] == '.' && head[d + 1] == ' ')
            strip_chars = d + 2;
    }
    g_free(head);

    if (strip_chars == 0)
        return FALSE;

    GtkTextIter del_end = *line_start;     /* end of the prefix to delete   */
    gtk_text_iter_forward_chars(&del_end, (gint)strip_chars);
    gint line = gtk_text_iter_get_line(line_start);
    gtk_text_buffer_delete(ed->buffer, line_start, &del_end);
    /* Deletion invalidated the iters; recompute the line start.            */
    gtk_text_buffer_get_iter_at_line(ed->buffer, line_start, line);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * renumber_list_block() — rewrite the "N. " prefixes of the contiguous
 * numbered-list block containing `line` so they count 1, 2, 3…
 *   ed   — the editor.
 *   line — any line inside the block.
 * ------------------------------------------------------------------------- */
static void
renumber_list_block(OnEditor *ed, gint line)
{
    GtkTextTag *num_tag = lookup_tag(ed->buffer, ON_TAGNAME_LIST_NUMBER);
    if (num_tag == NULL)
        return;

    /* Walk back to the first line of the block.                            */
    gint first = line;               /* first line of the numbered block    */
    while (first > 0) {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(ed->buffer, &it, first - 1);
        if (!gtk_text_iter_has_tag(&it, num_tag))
            break;
        first--;
    }

    /* Rewrite prefixes forward until the block ends.                       */
    gint total_lines =
        gtk_text_buffer_get_line_count(ed->buffer);
    gint number = 1;                 /* the value to write on this line     */
    ed->internal_change++;
    for (gint l = first; l < total_lines; l++, number++) {
        GtkTextIter ls;              /* line start                          */
        gtk_text_buffer_get_iter_at_line(ed->buffer, &ls, l);
        if (!gtk_text_iter_has_tag(&ls, num_tag))
            break;

        line_strip_list_prefix(ed, &ls);

        gchar *prefix = g_strdup_printf("%d. ", number);
        gint offset = gtk_text_iter_get_offset(&ls);
        gtk_text_buffer_insert(ed->buffer, &ls, prefix, -1);

        /* Re-apply the list tag over the inserted prefix so the line stays
         * uniformly tagged.                                                */
        GtkTextIter ps, pe;          /* prefix span                         */
        gtk_text_buffer_get_iter_at_offset(ed->buffer, &ps, offset);
        gtk_text_buffer_get_iter_at_offset(
            ed->buffer, &pe, offset + (gint)g_utf8_strlen(prefix, -1));
        gtk_text_buffer_apply_tag(ed->buffer, num_tag, &ps, &pe);
        g_free(prefix);
    }
    ed->internal_change--;
}

/* ---------------------------------------------------------------------------
 * apply_paragraph_format() — set the paragraph style of every line touched
 * by the selection (or the cursor line) to `flag`.
 *   ed   — the editor.
 *   flag — one of ON_FMT_H1/H2/CODEBLOCK/LIST_BULLET/LIST_NUMBER, or 0
 *          for plain body text.
 * ------------------------------------------------------------------------- */
static void
apply_paragraph_format(OnEditor *ed, guint32 flag)
{
    /* Determine the affected line range from selection or cursor.          */
    GtkTextIter start, end;          /* selection (or cursor twice)         */
    if (!gtk_text_buffer_get_selection_bounds(ed->buffer, &start, &end))
        gtk_text_buffer_get_iter_at_mark(
            ed->buffer, &start,
            gtk_text_buffer_get_insert(ed->buffer)), end = start;

    gint first_line = gtk_text_iter_get_line(&start);
    gint last_line  = gtk_text_iter_get_line(&end);

    static const struct { OnFormatFlags f; const gchar *name; } PARA[] = {
        { ON_FMT_H1,          ON_TAGNAME_H1          },
        { ON_FMT_H2,          ON_TAGNAME_H2          },
        { ON_FMT_CODEBLOCK,   ON_TAGNAME_CODEBLOCK   },
        { ON_FMT_LIST_BULLET, ON_TAGNAME_LIST_BULLET },
        { ON_FMT_LIST_NUMBER, ON_TAGNAME_LIST_NUMBER },
    };

    ed->internal_change++;
    for (gint l = first_line; l <= last_line; l++) {
        GtkTextIter ls, le;          /* line span incl. trailing newline    */
        line_span(ed->buffer, l, &ls, &le);

        /* Clear every existing paragraph tag and any list prefix.          */
        for (gsize i = 0; i < G_N_ELEMENTS(PARA); i++)
            gtk_text_buffer_remove_tag_by_name(ed->buffer, PARA[i].name,
                                               &ls, &le);
        if (line_strip_list_prefix(ed, &ls))
            line_span(ed->buffer, l, &ls, &le);

        if (flag == 0)
            continue;                /* body text: nothing more to do       */

        /* For lists, insert the visible prefix first.                      */
        if (flag == ON_FMT_LIST_BULLET) {
            gtk_text_buffer_insert(ed->buffer, &ls, "\xe2\x80\xa2 ", -1);
            line_span(ed->buffer, l, &ls, &le);
        } else if (flag == ON_FMT_LIST_NUMBER) {
            /* Number is fixed up by renumber_list_block afterwards.        */
            gtk_text_buffer_insert(ed->buffer, &ls, "1. ", -1);
            line_span(ed->buffer, l, &ls, &le);
        }

        /* Apply the requested tag over the whole line.                     */
        for (gsize i = 0; i < G_N_ELEMENTS(PARA); i++)
            if (PARA[i].f == (OnFormatFlags)flag)
                gtk_text_buffer_apply_tag_by_name(ed->buffer, PARA[i].name,
                                                  &ls, &le);
    }
    ed->internal_change--;

    if (flag == ON_FMT_LIST_NUMBER)
        renumber_list_block(ed, first_line);

    editor_queue_autosave(ed);
    /* Code blocks may have appeared or vanished: refresh their buttons.
     * (Pure tag changes don't emit "changed", so this must be explicit.)   */
    code_buttons_queue_rebuild(ed);
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
}

/* on_para_button() — click handler for paragraph-style tool buttons; the
 * style each button applies is stashed on it as object data "on-flag".      */
static void
on_para_button(GtkToolButton *btn, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    apply_paragraph_format(
        ed, GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "on-flag")));
}

/* ---------------------------------------------------------------------------
 * handle_return_in_list() — implement list continuation on Enter.
 *
 * On a non-empty list line, Enter inserts a newline plus the next prefix
 * ("• " or "N. ") and keeps the list tag going.  On an empty list item,
 * Enter *ends* the list (removes the prefix and tag, leaving a plain
 * line).
 *   ed — the editor.
 * Returns TRUE if the key press was consumed (a list line was handled).
 * ------------------------------------------------------------------------- */
static gboolean
handle_return_in_list(OnEditor *ed)
{
    GtkTextIter cursor;              /* current insertion point             */
    gtk_text_buffer_get_iter_at_mark(ed->buffer, &cursor,
                                     gtk_text_buffer_get_insert(ed->buffer));

    guint32 para = line_para_flags(ed->buffer, &cursor);
    if (para != ON_FMT_LIST_BULLET && para != ON_FMT_LIST_NUMBER)
        return FALSE;

    const gchar *tag_name = (para == ON_FMT_LIST_BULLET)
                            ? ON_TAGNAME_LIST_BULLET
                            : ON_TAGNAME_LIST_NUMBER;
    gint line = gtk_text_iter_get_line(&cursor);

    /* Measure the line's content beyond its prefix.                        */
    GtkTextIter ls, le;              /* text-only line span                 */
    gtk_text_buffer_get_iter_at_line(ed->buffer, &ls, line);
    le = ls;
    if (!gtk_text_iter_ends_line(&le))
        gtk_text_iter_forward_to_line_end(&le);
    gchar *text = gtk_text_buffer_get_text(ed->buffer, &ls, &le, FALSE);

    /* Compute the character length of the literal prefix on this line.     */
    glong prefix_chars = 0;          /* prefix length in characters         */
    if (para == ON_FMT_LIST_BULLET) {
        if (g_str_has_prefix(text, "\xe2\x80\xa2 "))
            prefix_chars = 2;
    } else {
        glong d = 0;
        while (g_ascii_isdigit(text[d]))
            d++;
        if (d > 0 && text[d] == '.' && text[d + 1] == ' ')
            prefix_chars = d + 2;
    }
    gboolean item_empty =
        (glong)g_utf8_strlen(text, -1) <= prefix_chars;

    ed->internal_change++;
    if (item_empty) {
        /* Empty item: end the list here.                                   */
        GtkTextIter span_s, span_e;  /* full line incl. newline             */
        line_span(ed->buffer, line, &span_s, &span_e);
        gtk_text_buffer_remove_tag_by_name(ed->buffer, tag_name,
                                           &span_s, &span_e);
        gtk_text_buffer_get_iter_at_line(ed->buffer, &ls, line);
        line_strip_list_prefix(ed, &ls);
    } else {
        /* Continue the list: newline + next prefix, tagged.                */
        gchar *next_prefix;          /* text inserted after the newline     */
        if (para == ON_FMT_LIST_BULLET) {
            next_prefix = g_strdup("\n\xe2\x80\xa2 ");
        } else {
            glong n = g_ascii_strtoll(text, NULL, 10);
            next_prefix = g_strdup_printf("\n%ld. ", n + 1);
        }
        gint at = gtk_text_iter_get_offset(&cursor);
        gtk_text_buffer_insert(ed->buffer, &cursor, next_prefix, -1);

        GtkTextIter ins_s, ins_e;    /* span of the inserted text           */
        gtk_text_buffer_get_iter_at_offset(ed->buffer, &ins_s, at);
        gtk_text_buffer_get_iter_at_offset(
            ed->buffer, &ins_e,
            at + (gint)g_utf8_strlen(next_prefix, -1));
        gtk_text_buffer_apply_tag_by_name(ed->buffer, tag_name,
                                          &ins_s, &ins_e);
        g_free(next_prefix);

        if (para == ON_FMT_LIST_NUMBER)
            renumber_list_block(ed, line);
    }
    ed->internal_change--;
    g_free(text);

    editor_queue_autosave(ed);
    return TRUE;
}

/* ===========================================================================
 * code blocks — floating per-block copy buttons
 *
 * Every contiguous code-block span gets a small clipboard button pinned
 * to its upper-right corner.  The buttons are children of the text view's
 * TEXT window (which does NOT scroll with the buffer), so their positions
 * are recomputed whenever the buffer changes, the view resizes, or the
 * user scrolls.  Each button carries a left-gravity GtkTextMark at its
 * block's start as object data "on-mark".
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * on_code_copy_clicked() — copy the whole code block whose start mark is
 * attached to the clicked button.
 * ------------------------------------------------------------------------- */
static void
on_code_copy_clicked(GtkButton *btn, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    GtkTextMark *mark =              /* block-start mark on the button      */
        g_object_get_data(G_OBJECT(btn), "on-mark");
    GtkTextTag *tag = lookup_tag(ed->buffer, ON_TAGNAME_CODEBLOCK);
    if (mark == NULL || tag == NULL)
        return;

    GtkTextIter start;               /* block start                         */
    gtk_text_buffer_get_iter_at_mark(ed->buffer, &start, mark);
    if (!gtk_text_iter_has_tag(&start, tag))
        return;                      /* block vanished since last rebuild   */

    GtkTextIter end = start;         /* block end                           */
    gtk_text_iter_forward_to_tag_toggle(&end, tag);

    gchar *code = gtk_text_buffer_get_text(ed->buffer, &start, &end, FALSE);
    g_strchomp(code);
    gtk_clipboard_set_text(
        gtk_widget_get_clipboard(GTK_WIDGET(ed->view),
                                 GDK_SELECTION_CLIPBOARD),
        code, -1);
    g_free(code);
}

/* ---------------------------------------------------------------------------
 * code_buttons_update_positions() — pin every copy button to the current
 * on-screen upper-right corner of its block.  Cheap; safe to call from
 * scroll and resize handlers.
 * ------------------------------------------------------------------------- */
static void
code_buttons_update_positions(OnEditor *ed)
{
    GdkWindow *text_win =            /* the view's scrolling text window    */
        gtk_text_view_get_window(ed->view, GTK_TEXT_WINDOW_TEXT);
    if (text_win == NULL)
        return;
    gint win_width  = gdk_window_get_width(text_win);
    gint win_height = gdk_window_get_height(text_win);

    for (GSList *l = ed->code_buttons; l != NULL; l = l->next) {
        GtkWidget *btn = l->data;    /* one floating copy button            */
        GtkTextMark *mark = g_object_get_data(G_OBJECT(btn), "on-mark");
        if (mark == NULL)
            continue;

        /* The button's CSS pins it to exactly CODE_BTN_SIZE square in
         * every state (normal/hover/active), so positioning can rely on
         * the constant instead of chasing theme-dependent allocations.     */
        const gint bw = CODE_BTN_SIZE;
        const gint bh = CODE_BTN_SIZE;

        GtkTextIter it;              /* block start position                */
        gtk_text_buffer_get_iter_at_mark(ed->buffer, &it, mark);
        gtk_text_iter_set_line_offset(&it, 0);   /* normalize to line start */

        /* Ask the layout for the whole display line's y-range: it starts
         * exactly where the paragraph background (the shading) starts —
         * unlike the character box, which sits below the above-line
         * spacing and drifted the button downward.                         */
        gint line_y, line_h;         /* line extent in buffer coords        */
        gtk_text_view_get_line_yrange(ed->view, &it, &line_y, &line_h);

        gint wx, wy;                 /* same location in window coords      */
        gtk_text_view_buffer_to_window_coords(ed->view,
                                              GTK_TEXT_WINDOW_TEXT,
                                              0, line_y, &wx, &wy);
        gint block_top = wy;         /* top edge of the shaded block        */

        /* Children in the text window do not scroll with the buffer, so
         * hide the button whenever its block's top edge is off screen —
         * otherwise it would float over unrelated text.                    */
        if (block_top < -2 || block_top > win_height - bh) {
            gtk_widget_hide(btn);
            continue;
        }
        gtk_widget_show(btn);
        /* Equal CODE_BTN_MARGIN insets from the shaded top/right edges.
         * GTK3 quirk (verified empirically): children of the TEXT window
         * are allocated with the view's top margin added AGAIN on top of
         * the requested y, while buffer_to_window_coords already includes
         * it — so subtract it once to avoid double-counting.               */
        gtk_text_view_move_child(
            ed->view, btn,
            win_width - CODEBLOCK_RIGHT_MARGIN - bw - CODE_BTN_MARGIN,
            block_top + CODE_BTN_MARGIN
                - gtk_text_view_get_top_margin(ed->view));
    }
}

/* ---------------------------------------------------------------------------
 * code_buttons_rebuild() — destroy and recreate the floating copy buttons
 * to match the code blocks currently present in the buffer.
 * ------------------------------------------------------------------------- */
static void
code_buttons_rebuild(OnEditor *ed)
{
    /* Tear down the old set (their marks die with them).                   */
    for (GSList *l = ed->code_buttons; l != NULL; l = l->next) {
        GtkWidget *btn = l->data;
        GtkTextMark *mark = g_object_get_data(G_OBJECT(btn), "on-mark");
        if (mark != NULL)
            gtk_text_buffer_delete_mark(ed->buffer, mark);
        gtk_widget_destroy(btn);
    }
    g_slist_free(ed->code_buttons);
    ed->code_buttons = NULL;

    /* The buttons can be disabled entirely in File → Settings….            */
    if (!ed->app->code_copy_buttons)
        return;

    GtkTextTag *tag = lookup_tag(ed->buffer, ON_TAGNAME_CODEBLOCK);
    if (tag == NULL)
        return;

    /* Walk the buffer from span to span of the code-block tag.             */
    GtkTextIter it;                  /* scan position                       */
    gtk_text_buffer_get_start_iter(ed->buffer, &it);
    while (TRUE) {
        if (!gtk_text_iter_starts_tag(&it, tag)) {
            if (!gtk_text_iter_forward_to_tag_toggle(&it, tag))
                break;
            if (!gtk_text_iter_starts_tag(&it, tag))
                continue;
        }

        /* One block starts here: build its button.  Its CSS pins every
         * state (normal, hover, active) to exactly CODE_BTN_SIZE square —
         * hover styling must not change the geometry — and draws a solid
         * background + border in ALL states, so the button reads as a
         * button even while overlaying code text (normal relief keeps the
         * theme's frame instead of the hover-only flat look).              */
        GtkWidget *btn = gtk_button_new();
        GtkWidget *icon = on_app_icon_image_sized(ed->app, "edit-copy-symbolic", 16);
        if (icon == NULL)
            icon = gtk_label_new("\xe2\x8e\x98");    /* ⎘ fallback glyph    */
        gtk_container_add(GTK_CONTAINER(btn), icon);
        gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NORMAL);
        gtk_widget_set_tooltip_text(btn, "Copy code block");
        gtk_widget_set_size_request(btn, CODE_BTN_SIZE, CODE_BTN_SIZE);

        GtkCssProvider *css = gtk_css_provider_new();
        gchar *css_text = g_strdup_printf(
            "button, button:hover, button:active {"
            "  padding: 0;"
            "  margin: 0;"
            "  border: 1px solid #b0b0b0;"
            "  border-radius: 4px;"
            "  background-image: none;"
            "  background-color: #ffffff;"
            "  min-width: %dpx;"
            "  min-height: %dpx;"
            "}"
            "button:hover  { background-color: #f2f2f2; }"
            "button:active { background-color: #e2e2e2; }",
            CODE_BTN_SIZE - 2, CODE_BTN_SIZE - 2);
        gtk_css_provider_load_from_data(css, css_text, -1, NULL);
        g_free(css_text);
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(btn),
            GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);

        GtkTextMark *mark = gtk_text_buffer_create_mark(ed->buffer, NULL,
                                                        &it, TRUE);
        g_object_set_data(G_OBJECT(btn), "on-mark", mark);
        g_signal_connect(btn, "clicked",
                         G_CALLBACK(on_code_copy_clicked), ed);

        gtk_text_view_add_child_in_window(ed->view, btn,
                                          GTK_TEXT_WINDOW_TEXT, 0, 0);
        gtk_widget_show_all(btn);
        ed->code_buttons = g_slist_prepend(ed->code_buttons, btn);

        /* Jump past this block and continue scanning.                      */
        if (!gtk_text_iter_forward_to_tag_toggle(&it, tag))
            break;
    }
    code_buttons_update_positions(ed);
}

/* on_code_btn_idle() — deferred rebuild so the text layout has settled.     */
static gboolean
on_code_btn_idle(gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    ed->code_btn_idle = 0;
    code_buttons_rebuild(ed);
    return G_SOURCE_REMOVE;
}

/* code_buttons_queue_rebuild() — coalesce rebuild requests into one idle.   */
static void
code_buttons_queue_rebuild(OnEditor *ed)
{
    if (ed->code_btn_idle == 0)
        ed->code_btn_idle = g_idle_add(on_code_btn_idle, ed);
}

void
on_editor_rebuild_code_buttons_all(OnApp *app)
{
    GHashTableIter iter;             /* walk of the open-editors table      */
    gpointer key, value;
    g_hash_table_iter_init(&iter, app->editors);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        OnEditor *ed =               /* editor state stashed on its window  */
            g_object_get_data(G_OBJECT(value), "on-editor");
        if (ed != NULL)
            code_buttons_queue_rebuild(ed);
    }
}

/* on_view_scrolled() / on_view_size_allocate() — keep buttons pinned.       */
static void
on_view_scrolled(GtkAdjustment *adj, gpointer user_data)
{
    (void)adj;
    code_buttons_update_positions((OnEditor *)user_data);
}

static void
on_view_size_allocate(GtkWidget *widget, GdkRectangle *allocation,
                      gpointer user_data)
{
    (void)widget; (void)allocation;
    code_buttons_update_positions((OnEditor *)user_data);
}

/* ===========================================================================
 * images
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * image_effective_width() — the logical on-screen width an image should
 * be shown at: the stored choice, or the default thumbnail cap, never
 * wider than the original.
 * ------------------------------------------------------------------------- */
static gint
image_effective_width(GdkPixbuf *orig, gint display_width)
{
    gint w = gdk_pixbuf_get_width(orig);
    return (display_width > 0) ? MIN(display_width, w)
                               : MIN(w, ON_IMAGE_DEFAULT_WIDTH);
}

/* ---------------------------------------------------------------------------
 * image_widget_new() — build a HiDPI-aware GtkImage showing `orig` at
 * `display_width` logical pixels.  The backing pixbuf is scaled to
 * display_width × scale-factor physical pixels and wrapped in a cairo
 * surface with the matching device scale, so images stay sharp on Retina
 * displays instead of being stretched by the compositor.
 * ------------------------------------------------------------------------- */
static GtkWidget *
image_widget_new(OnEditor *ed, GdkPixbuf *orig, gint display_width)
{
    gint sf = gtk_widget_get_scale_factor(GTK_WIDGET(ed->view));
    gint w  = gdk_pixbuf_get_width(orig);
    gint h  = gdk_pixbuf_get_height(orig);

    gint want = image_effective_width(orig, display_width);
    /* Physical pixels backing the widget; never upscale past the source.   */
    gint pw = MIN(want * sf, w);
    gint ph = MAX(1, (gint)((gdouble)h * pw / w));

    GdkPixbuf *backing =             /* pixels actually handed to cairo     */
        (pw < w) ? gdk_pixbuf_scale_simple(orig, pw, ph,
                                           GDK_INTERP_BILINEAR)
                 : g_object_ref(orig);

    cairo_surface_t *surface = gdk_cairo_surface_create_from_pixbuf(
        backing, sf, gtk_widget_get_window(GTK_WIDGET(ed->view)));
    GtkWidget *image = gtk_image_new_from_surface(surface);
    cairo_surface_destroy(surface);
    g_object_unref(backing);
    return image;
}

/* ---------------------------------------------------------------------------
 * attach_image_widget() — (re)create the display widget for an
 * image-carrying anchor, destroying any widget it already had.
 * ------------------------------------------------------------------------- */
static void
attach_image_widget(OnEditor *ed, GtkTextChildAnchor *anchor)
{
    gint dw;                         /* stored display width                */
    GdkPixbuf *orig = on_anchor_get_image(anchor, &dw);
    if (orig == NULL)
        return;

    /* Drop any existing display widget.                                    */
    GList *widgets = gtk_text_child_anchor_get_widgets(anchor);
    for (GList *l = widgets; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(widgets);

    GtkWidget *image = image_widget_new(ed, orig, dw);
    gtk_text_view_add_child_at_anchor(ed->view, image, anchor);
    gtk_widget_show(image);
}

/* ---------------------------------------------------------------------------
 * editor_attach_image_widgets() — walk the whole buffer and give every
 * image anchor its display widget (used right after loading a note).
 * ------------------------------------------------------------------------- */
static void
editor_attach_image_widgets(OnEditor *ed)
{
    GtkTextIter it;                  /* scan cursor                         */
    gtk_text_buffer_get_start_iter(ed->buffer, &it);
    do {
        GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&it);
        if (anchor != NULL)
            attach_image_widget(ed, anchor);
    } while (gtk_text_iter_forward_char(&it));
}

/* ---------------------------------------------------------------------------
 * insert_image_pixbuf() — embed a full-resolution image at the cursor as
 * an anchor + HiDPI widget, shown at the default thumbnail width.
 *   ed     — the editor.
 *   pixbuf — full-resolution image (caller keeps its own reference).
 * ------------------------------------------------------------------------- */
static void
insert_image_pixbuf(OnEditor *ed, GdkPixbuf *pixbuf)
{
    GtkTextIter cursor;              /* insertion point                     */
    gtk_text_buffer_get_iter_at_mark(ed->buffer, &cursor,
                                     gtk_text_buffer_get_insert(ed->buffer));
    ed->internal_change++;
    GtkTextChildAnchor *anchor =
        gtk_text_buffer_create_child_anchor(ed->buffer, &cursor);
    on_anchor_set_image(anchor, pixbuf, 0);
    attach_image_widget(ed, anchor);
    ed->internal_change--;
    editor_queue_autosave(ed);
}

/* ---------------------------------------------------------------------------
 * anchor_at_offset() — the image anchor at buffer offset `offset`, or
 * NULL if that position holds none.
 * ------------------------------------------------------------------------- */
static GtkTextChildAnchor *
anchor_at_offset(OnEditor *ed, gint offset)
{
    GtkTextIter it;                  /* position of the anchor              */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &it, offset);
    return gtk_text_iter_get_child_anchor(&it);
}

/* ---------------------------------------------------------------------------
 * replace_image_display() — change the stored display width of the image
 * at `offset` and rebuild its widget.  No text changes, so this is purely
 * a presentation update plus an autosave (the width is persisted).
 * ------------------------------------------------------------------------- */
static void
replace_image_display(OnEditor *ed, gint offset, gint display_width)
{
    GtkTextChildAnchor *anchor = anchor_at_offset(ed, offset);
    if (anchor == NULL)
        return;
    GdkPixbuf *orig = on_anchor_get_image(anchor, NULL);
    if (orig == NULL)
        return;
    on_anchor_set_image(anchor, orig, display_width);
    attach_image_widget(ed, anchor);
    editor_queue_autosave(ed);
    code_buttons_queue_rebuild(ed);
}

/* on_img_copy() — "Copy Image": put the full-resolution image on the
 * clipboard so it can be pasted outside the note.                           */
static void
on_img_copy(GtkMenuItem *item, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    GtkTextChildAnchor *anchor = anchor_at_offset(
        ed, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item),
                                              "on-offset")));
    GdkPixbuf *orig = anchor != NULL
                      ? on_anchor_get_image(anchor, NULL) : NULL;
    if (orig != NULL)
        gtk_clipboard_set_image(
            gtk_widget_get_clipboard(GTK_WIDGET(ed->view),
                                     GDK_SELECTION_CLIPBOARD),
            orig);
}

/* on_img_open_full() — "Open Full Size…": show the original image in its
 * own scrollable viewer window.                                             */
static void
on_img_open_full(GtkMenuItem *item, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    GtkTextChildAnchor *anchor = anchor_at_offset(
        ed, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item),
                                              "on-offset")));
    GdkPixbuf *orig = anchor != NULL
                      ? on_anchor_get_image(anchor, NULL) : NULL;
    if (orig == NULL)
        return;

    GtkWidget *viewer = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(viewer), "Orange Notes - Image");
    gtk_window_set_transient_for(GTK_WINDOW(viewer),
                                 GTK_WINDOW(ed->window));
    gtk_window_set_default_size(
        GTK_WINDOW(viewer),
        MIN(gdk_pixbuf_get_width(orig) + 40, 1200),
        MIN(gdk_pixbuf_get_height(orig) + 40, 900));

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll),
                      gtk_image_new_from_pixbuf(orig));
    gtk_container_add(GTK_CONTAINER(viewer), scroll);
    gtk_widget_show_all(viewer);
}

/* on_img_display_full() / on_img_display_thumb() — inline display size.     */
static void
on_img_display_full(GtkMenuItem *item, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    gint offset = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item),
                                                    "on-offset"));
    GtkTextChildAnchor *anchor = anchor_at_offset(ed, offset);
    GdkPixbuf *orig = anchor != NULL
                      ? on_anchor_get_image(anchor, NULL) : NULL;
    if (orig != NULL)
        replace_image_display(ed, offset, gdk_pixbuf_get_width(orig));
}

static void
on_img_display_thumb(GtkMenuItem *item, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    replace_image_display(
        ed,
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "on-offset")),
        0);                          /* 0 = the default thumbnail width     */
}

/* ---------------------------------------------------------------------------
 * on_view_button_press() — remember where right clicks land (in the text
 * window's coordinates) so on_view_populate_popup() can tell whether the
 * click was on an embedded image.
 * ------------------------------------------------------------------------- */
static gboolean
on_view_button_press(GtkWidget *widget, GdkEventButton *event,
                     gpointer user_data)
{
    (void)widget;
    OnEditor *ed = user_data;        /* owning editor                       */
    if (event->button == GDK_BUTTON_SECONDARY) {
        ed->popup_x = (gint)event->x;
        ed->popup_y = (gint)event->y;
    }
    return FALSE;                    /* never consume: default menu runs    */
}

/* ---------------------------------------------------------------------------
 * on_view_populate_popup() — extend the text view's context menu with
 * image actions when the right click landed on an embedded image.
 * ------------------------------------------------------------------------- */
static void
on_view_populate_popup(GtkTextView *view, GtkWidget *popup,
                       gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    if (!GTK_IS_MENU(popup))
        return;

    /* Which buffer position was clicked?                                   */
    gint bx, by;                     /* click in buffer coordinates         */
    gtk_text_view_window_to_buffer_coords(view, GTK_TEXT_WINDOW_TEXT,
                                          ed->popup_x, ed->popup_y,
                                          &bx, &by);
    GtkTextIter it;                  /* iter under the pointer              */
    gtk_text_view_get_iter_at_location(view, &it, bx, by);

    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&it);
    if (anchor == NULL) {
        /* Clicks on the right half of an image resolve to the next iter;
         * look one character back before giving up.                        */
        if (!gtk_text_iter_backward_char(&it))
            return;
        anchor = gtk_text_iter_get_child_anchor(&it);
        if (anchor == NULL)
            return;
    }
    gint dw;                         /* stored display width                */
    GdkPixbuf *orig = on_anchor_get_image(anchor, &dw);
    if (orig == NULL)
        return;
    gint offset = gtk_text_iter_get_offset(&it);
    gboolean shown_full =
        image_effective_width(orig, dw) >= gdk_pixbuf_get_width(orig);

    /* Build the extra items (prepended so they sit on top).                */
    struct { const gchar *label; GCallback cb; gboolean show; } items[] = {
        { "Display as _Thumbnail", G_CALLBACK(on_img_display_thumb),
          shown_full },
        { "Display _Full Size",    G_CALLBACK(on_img_display_full),
          !shown_full },
        { "_Open Full Size\xe2\x80\xa6", G_CALLBACK(on_img_open_full),
          TRUE },
        { "Copy _Image",           G_CALLBACK(on_img_copy), TRUE },
    };

    GtkWidget *sep = gtk_separator_menu_item_new();
    gtk_widget_show(sep);
    gtk_menu_shell_prepend(GTK_MENU_SHELL(popup), sep);

    for (gsize i = 0; i < G_N_ELEMENTS(items); i++) {
        if (!items[i].show)
            continue;
        GtkWidget *mi = gtk_menu_item_new_with_mnemonic(items[i].label);
        g_object_set_data(G_OBJECT(mi), "on-offset",
                          GINT_TO_POINTER(offset));
        g_signal_connect(mi, "activate", items[i].cb, ed);
        gtk_widget_show(mi);
        gtk_menu_shell_prepend(GTK_MENU_SHELL(popup), mi);
    }
}

/* ---------------------------------------------------------------------------
 * on_paste_clipboard() — intercept Ctrl+V: if the clipboard holds an image
 * (e.g. a screenshot), embed it instead of letting the default text paste
 * run.  Plain text pastes fall through to GTK's default handler.
 * ------------------------------------------------------------------------- */
static void
on_paste_clipboard(GtkTextView *view, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    GtkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(view),
                                                GDK_SELECTION_CLIPBOARD);
    if (!gtk_clipboard_wait_is_image_available(cb))
        return;                      /* not an image: default paste runs    */

    GdkPixbuf *pixbuf = gtk_clipboard_wait_for_image(cb);
    if (pixbuf != NULL) {
        insert_image_pixbuf(ed, pixbuf);
        g_object_unref(pixbuf);
    }
    /* Swallow the signal so the default handler doesn't also paste.        */
    g_signal_stop_emission_by_name(view, "paste-clipboard");
}

/* ---------------------------------------------------------------------------
 * on_insert_image_clicked() — "Image…" toolbar button: pick an image file
 * and embed it at the cursor.
 * ------------------------------------------------------------------------- */
static void
on_insert_image_clicked(GtkToolButton *btn, gpointer user_data)
{
    (void)btn;
    OnEditor *ed = user_data;        /* owning editor                       */

    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Insert Image", GTK_WINDOW(ed->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Insert", GTK_RESPONSE_ACCEPT,
        NULL);

    /* Only offer image files.                                              */
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images");
    gtk_file_filter_add_pixbuf_formats(filter);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(
            GTK_FILE_CHOOSER(dialog));
        GError *err = NULL;
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &err);
        if (pixbuf != NULL) {
            insert_image_pixbuf(ed, pixbuf);
            g_object_unref(pixbuf);
        } else {
            g_warning("editor: cannot load image %s: %s",
                      path, err->message);
            g_clear_error(&err);
        }
        g_free(path);
    }
    gtk_widget_destroy(dialog);
}

/* ===========================================================================
 * #tag capture and autocomplete popup
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * tag_capture_span() — compute the span of the tag currently being typed:
 * from the '#' at ed->tag_start forward across word characters.
 *   ed    — the editor (capture must be active).
 *   start — receives the position of the '#'.
 *   end   — receives the position just past the last word character.
 * Returns TRUE if the span still starts with '#' (capture is valid).
 * ------------------------------------------------------------------------- */
static gboolean
tag_capture_span(OnEditor *ed, GtkTextIter *start, GtkTextIter *end)
{
    gtk_text_buffer_get_iter_at_mark(ed->buffer, start, ed->tag_start);
    if (gtk_text_iter_get_char(start) != '#')
        return FALSE;

    *end = *start;
    gtk_text_iter_forward_char(end);      /* step past the '#'              */
    while (!gtk_text_iter_is_end(end)) {
        gunichar c = gtk_text_iter_get_char(end);
        if (!(g_unichar_isalnum(c) || c == '_' || c == '-'))
            break;
        gtk_text_iter_forward_char(end);
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * tag_popup_hide() — hide the suggestion popup if it is showing.
 * ------------------------------------------------------------------------- */
static void
tag_popup_hide(OnEditor *ed)
{
    if (ed->tag_popup != NULL)
        gtk_widget_hide(ed->tag_popup);
}

/* ---------------------------------------------------------------------------
 * tag_capture_end() — finish the active tag capture.
 *   ed    — the editor.
 *   apply — TRUE to style the "#word" span as a tag (if the word is
 *           non-empty); FALSE to abandon it as plain text (Escape).
 * ------------------------------------------------------------------------- */
static void
tag_capture_end(OnEditor *ed, gboolean apply)
{
    if (ed->tag_start == NULL)
        return;

    GtkTextIter start, end;          /* the "#word" span                    */
    if (tag_capture_span(ed, &start, &end) && apply) {
        /* Only style it if there is at least one char after the '#'.       */
        if (gtk_text_iter_get_offset(&end) -
            gtk_text_iter_get_offset(&start) >= 2) {
            ed->internal_change++;
            gtk_text_buffer_apply_tag_by_name(ed->buffer, ON_TAGNAME_TAG,
                                              &start, &end);
            ed->internal_change--;
            editor_queue_autosave(ed);
        }
    }
    gtk_text_buffer_delete_mark(ed->buffer, ed->tag_start);
    ed->tag_start = NULL;
    tag_popup_hide(ed);
}

/* ---------------------------------------------------------------------------
 * on_tag_row_activated() — a suggestion row was clicked/activated: replace
 * the partially typed word with the chosen tag name, style it, and end the
 * capture with a trailing space (which is what "ends" a tag).
 * ------------------------------------------------------------------------- */
static void
on_tag_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void)box;
    OnEditor *ed = user_data;        /* owning editor                       */
    const gchar *name =              /* chosen tag name (no '#')            */
        g_object_get_data(G_OBJECT(row), "on-tag-name");
    if (name == NULL || ed->tag_start == NULL)
        return;

    GtkTextIter start, end;          /* current "#partial" span             */
    if (!tag_capture_span(ed, &start, &end)) {
        tag_capture_end(ed, FALSE);
        return;
    }

    ed->internal_change++;
    /* Replace "#partial" with "#name ".                                    */
    gint at = gtk_text_iter_get_offset(&start);
    gtk_text_buffer_delete(ed->buffer, &start, &end);
    gchar *full = g_strdup_printf("#%s", name);
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &start, at);
    gtk_text_buffer_insert(ed->buffer, &start, full, -1);

    GtkTextIter ts, te;              /* span of the completed tag           */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &ts, at);
    gtk_text_buffer_get_iter_at_offset(
        ed->buffer, &te, at + (gint)g_utf8_strlen(full, -1));
    gtk_text_buffer_apply_tag_by_name(ed->buffer, ON_TAGNAME_TAG, &ts, &te);

    /* Trailing space ends the tag (untagged).                              */
    gtk_text_buffer_insert(ed->buffer, &te, " ", -1);
    g_free(full);
    ed->internal_change--;

    /* Capture is over; the tag styling was applied above.                  */
    gtk_text_buffer_delete_mark(ed->buffer, ed->tag_start);
    ed->tag_start = NULL;
    tag_popup_hide(ed);
    editor_queue_autosave(ed);
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
}

/* ---------------------------------------------------------------------------
 * tag_popup_ensure() — lazily build the popup window + listbox.
 * ------------------------------------------------------------------------- */
static void
tag_popup_ensure(OnEditor *ed)
{
    if (ed->tag_popup != NULL)
        return;

    ed->tag_popup = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_transient_for(GTK_WINDOW(ed->tag_popup),
                                 GTK_WINDOW(ed->window));
    gtk_window_set_type_hint(GTK_WINDOW(ed->tag_popup),
                             GDK_WINDOW_TYPE_HINT_COMBO);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_OUT);
    gtk_container_add(GTK_CONTAINER(ed->tag_popup), frame);

    ed->tag_listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ed->tag_listbox),
                                    GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(frame), ed->tag_listbox);
    g_signal_connect(ed->tag_listbox, "row-activated",
                     G_CALLBACK(on_tag_row_activated), ed);
}

/* ---------------------------------------------------------------------------
 * tag_popup_update() — refresh the popup contents to the tags matching
 * the currently typed prefix, position it under the cursor, and show or
 * hide it depending on whether anything matches.
 * ------------------------------------------------------------------------- */
static void
tag_popup_update(OnEditor *ed)
{
    if (ed->tag_start == NULL)
        return;

    GtkTextIter start, end;          /* current "#partial" span             */
    if (!tag_capture_span(ed, &start, &end)) {
        tag_capture_end(ed, FALSE);
        return;
    }

    /* The prefix typed so far, without the '#'.                            */
    gchar *typed = gtk_text_buffer_get_text(ed->buffer, &start, &end, FALSE);
    const gchar *prefix = typed + 1;

    tag_popup_ensure(ed);

    /* Clear old suggestion rows.                                           */
    GList *children =
        gtk_container_get_children(GTK_CONTAINER(ed->tag_listbox));
    for (GList *l = children; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    /* Fill with case-insensitive prefix matches from the tag table.        */
    GList *tags = on_db_tag_list(ed->app->db);
    gint shown = 0;                  /* number of rows added                */
    gchar *prefix_ci = g_utf8_casefold(prefix, -1);
    for (GList *l = tags; l != NULL && shown < TAG_POPUP_MAX; l = l->next) {
        OnTag *t = l->data;
        gchar *name_ci = g_utf8_casefold(t->name, -1);
        gboolean match = g_str_has_prefix(name_ci, prefix_ci);
        g_free(name_ci);
        if (!match)
            continue;

        GtkWidget *label = gtk_label_new(NULL);
        gchar *markup = g_markup_printf_escaped("#%s", t->name);
        gtk_label_set_text(GTK_LABEL(label), markup);
        g_free(markup);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_widget_set_margin_start(label, 8);
        gtk_widget_set_margin_end(label, 8);
        gtk_widget_set_margin_top(label, 3);
        gtk_widget_set_margin_bottom(label, 3);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), label);
        g_object_set_data_full(G_OBJECT(row), "on-tag-name",
                               g_strdup(t->name), g_free);
        gtk_list_box_insert(GTK_LIST_BOX(ed->tag_listbox), row, -1);
        shown++;
    }
    g_free(prefix_ci);
    g_free(typed);
    on_db_tag_list_free(tags);

    if (shown == 0) {
        tag_popup_hide(ed);
        return;
    }

    /* Select the first row so Enter picks it immediately.                  */
    GtkListBoxRow *first_row =
        gtk_list_box_get_row_at_index(GTK_LIST_BOX(ed->tag_listbox), 0);
    gtk_list_box_select_row(GTK_LIST_BOX(ed->tag_listbox), first_row);

    /* Position the popup just below the '#' character.                     */
    GdkRectangle rect;               /* cursor rectangle in buffer coords   */
    gtk_text_view_get_iter_location(ed->view, &start, &rect);
    gint wx, wy;                     /* rect origin in widget coords        */
    gtk_text_view_buffer_to_window_coords(ed->view, GTK_TEXT_WINDOW_WIDGET,
                                          rect.x, rect.y + rect.height,
                                          &wx, &wy);
    GdkWindow *gdkwin =
        gtk_widget_get_window(GTK_WIDGET(ed->view));
    gint ox = 0, oy = 0;             /* view origin in screen coords        */
    if (gdkwin != NULL)
        gdk_window_get_origin(gdkwin, &ox, &oy);

    gtk_widget_show_all(ed->tag_popup);
    gtk_window_move(GTK_WINDOW(ed->tag_popup), ox + wx, oy + wy + 2);
}

/* ---------------------------------------------------------------------------
 * tag_popup_move_selection() — move the popup's selected row up or down
 * (keyboard navigation while typing a tag).
 *   ed    — the editor (popup must exist).
 *   delta — +1 for down, -1 for up.
 * ------------------------------------------------------------------------- */
static void
tag_popup_move_selection(OnEditor *ed, gint delta)
{
    GtkListBox *box = GTK_LIST_BOX(ed->tag_listbox);
    GtkListBoxRow *sel = gtk_list_box_get_selected_row(box);
    gint index = (sel != NULL)
                 ? gtk_list_box_row_get_index(sel) + delta
                 : 0;
    if (index < 0)
        index = 0;
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(box, index);
    if (row != NULL)
        gtk_list_box_select_row(box, row);
}

/* ===========================================================================
 * buffer signal handlers
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * on_buffer_insert_text_after() — runs after every insertion.  Three jobs:
 *   1. enforce ed->inline_flags on short (typed) insertions,
 *   2. start a tag capture when '#' is typed at a word boundary,
 *   3. advance/close an active tag capture as the user keeps typing.
 * ------------------------------------------------------------------------- */
static void
on_buffer_insert_text_after(GtkTextBuffer *buffer, GtkTextIter *location,
                            gchar *text, gint len, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    if (ed->internal_change > 0)
        return;

    glong n_chars = g_utf8_strlen(text, len);
    gint  end_off = gtk_text_iter_get_offset(location);

    /* --- job 1: make typed text obey the current inline style ---------- */
    if (n_chars <= 2) {
        GtkTextIter span_s;          /* start of the inserted span          */
        gtk_text_buffer_get_iter_at_offset(buffer, &span_s,
                                           end_off - (gint)n_chars);
        ed->internal_change++;
        for (gsize i = 0; i < G_N_ELEMENTS(INLINE_TOGGLES); i++) {
            if (ed->inline_flags & INLINE_TOGGLES[i].flag)
                gtk_text_buffer_apply_tag_by_name(
                    buffer, INLINE_TOGGLES[i].tag_name, &span_s, location);
            else
                gtk_text_buffer_remove_tag_by_name(
                    buffer, INLINE_TOGGLES[i].tag_name, &span_s, location);
        }
        ed->internal_change--;
        /* location may have been invalidated by tag ops; refresh it.       */
        gtk_text_buffer_get_iter_at_offset(buffer, location, end_off);
    }

    /* --- jobs 2 & 3: tag capture ---------------------------------------- */
    if (ed->tag_start == NULL) {
        /* Start capture on a freshly typed '#' at a word boundary.         */
        if (n_chars == 1 && text[0] == '#') {
            GtkTextIter hash;        /* position of the '#'                 */
            gtk_text_buffer_get_iter_at_offset(buffer, &hash, end_off - 1);
            GtkTextIter before = hash;
            gboolean at_boundary = !gtk_text_iter_backward_char(&before) ||
                g_unichar_isspace(gtk_text_iter_get_char(&before));
            if (at_boundary) {
                ed->tag_start = gtk_text_buffer_create_mark(
                    buffer, NULL, &hash, TRUE /* left gravity */);
                tag_popup_update(ed);
            }
        }
        return;
    }

    /* Capture active: whitespace ends the tag, word chars refresh the
     * popup, anything else (punctuation) also ends it.                     */
    if (n_chars == 1) {
        gunichar c = g_utf8_get_char(text);
        if (g_unichar_isspace(c)) {
            tag_capture_end(ed, TRUE);
        } else if (g_unichar_isalnum(c) || c == '_' || c == '-') {
            tag_popup_update(ed);
        } else {
            tag_capture_end(ed, TRUE);
        }
    } else {
        /* Multi-char insertion (paste) during capture: just end it.        */
        tag_capture_end(ed, TRUE);
    }
}

/* ---------------------------------------------------------------------------
 * on_buffer_delete_range_after() — after a deletion, cancel any active tag
 * capture whose '#' was removed, or refresh the popup otherwise.
 * ------------------------------------------------------------------------- */
static void
on_buffer_delete_range_after(GtkTextBuffer *buffer, GtkTextIter *start,
                             GtkTextIter *end, gpointer user_data)
{
    (void)buffer; (void)start; (void)end;
    OnEditor *ed = user_data;        /* owning editor                       */
    if (ed->internal_change > 0 || ed->tag_start == NULL)
        return;

    GtkTextIter s, e;                /* revalidated capture span            */
    if (!tag_capture_span(ed, &s, &e))
        tag_capture_end(ed, FALSE);  /* '#' itself was deleted              */
    else
        tag_popup_update(ed);
}

/* ---------------------------------------------------------------------------
 * on_buffer_changed() — any edit re-arms the autosave timer.
 * ------------------------------------------------------------------------- */
static void
on_buffer_changed(GtkTextBuffer *buffer, gpointer user_data)
{
    (void)buffer;
    OnEditor *ed = user_data;        /* owning editor                       */
    editor_queue_autosave(ed);
    /* Text edits can grow, shrink, split or remove code blocks.            */
    code_buttons_queue_rebuild(ed);
}

/* ---------------------------------------------------------------------------
 * on_cursor_moved() — "notify::cursor-position" handler.  Refreshes the
 * inline-style state from the character left of the new cursor position
 * (so the toolbar reflects where you are), and closes a tag capture when
 * the cursor leaves the tag being typed.
 * ------------------------------------------------------------------------- */
static void
on_cursor_moved(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)object; (void)pspec;
    OnEditor *ed = user_data;        /* owning editor                       */
    if (ed->internal_change > 0)
        return;

    GtkTextIter cursor;              /* new cursor position                 */
    gtk_text_buffer_get_iter_at_mark(ed->buffer, &cursor,
                                     gtk_text_buffer_get_insert(ed->buffer));

    /* Close the capture if the cursor moved outside the typed tag.         */
    if (ed->tag_start != NULL) {
        GtkTextIter s, e;            /* capture span                        */
        if (!tag_capture_span(ed, &s, &e) ||
            gtk_text_iter_compare(&cursor, &s) < 0 ||
            gtk_text_iter_compare(&cursor, &e) > 0)
            tag_capture_end(ed, TRUE);
    }

    /* Adopt the style of the character to the left of the cursor.          */
    GtkTextIter probe = cursor;      /* the char whose style we adopt       */
    if (gtk_text_iter_backward_char(&probe))
        ed->inline_flags = iter_inline_flags(ed->buffer, &probe);
    else
        ed->inline_flags = 0;
    update_toggle_buttons(ed);
}

/* ---------------------------------------------------------------------------
 * on_view_key_press() — key handling that must run before GtkTextView's
 * default: tag-popup navigation, Escape, Enter-in-list, and the classic
 * Ctrl/Cmd+B/I/U shortcuts.
 * Returns TRUE when the key was fully handled here.
 * ------------------------------------------------------------------------- */
static gboolean
on_view_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    (void)widget;
    OnEditor *ed = user_data;        /* owning editor                       */

    /* While the tag popup is visible it owns the navigation keys.          */
    if (ed->tag_start != NULL && ed->tag_popup != NULL &&
        gtk_widget_get_visible(ed->tag_popup)) {
        switch (event->keyval) {
        case GDK_KEY_Down:
            tag_popup_move_selection(ed, +1);
            return TRUE;
        case GDK_KEY_Up:
            tag_popup_move_selection(ed, -1);
            return TRUE;
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_Tab: {
            GtkListBoxRow *row = gtk_list_box_get_selected_row(
                GTK_LIST_BOX(ed->tag_listbox));
            if (row != NULL) {
                on_tag_row_activated(GTK_LIST_BOX(ed->tag_listbox),
                                     row, ed);
                return TRUE;
            }
            break;
        }
        case GDK_KEY_Escape:
            tag_capture_end(ed, FALSE);
            return TRUE;
        default:
            break;
        }
    } else if (ed->tag_start != NULL && event->keyval == GDK_KEY_Escape) {
        tag_capture_end(ed, FALSE);
        return TRUE;
    }

    /* Enter inside a list item: continue or end the list.                  */
    if (event->keyval == GDK_KEY_Return ||
        event->keyval == GDK_KEY_KP_Enter) {
        if (ed->tag_start != NULL)
            tag_capture_end(ed, TRUE);
        if (handle_return_in_list(ed))
            return TRUE;
    }

    /* Ctrl (or Cmd on macOS) + B/I/U inline-style shortcuts.               */
    if (event->state & (GDK_CONTROL_MASK | GDK_META_MASK)) {
        switch (gdk_keyval_to_lower(event->keyval)) {
        case GDK_KEY_b:
            toggle_inline_format(ed, ON_FMT_BOLD);
            return TRUE;
        case GDK_KEY_i:
            toggle_inline_format(ed, ON_FMT_ITALIC);
            return TRUE;
        case GDK_KEY_u:
            toggle_inline_format(ed, ON_FMT_UNDERLINE);
            return TRUE;
        default:
            break;
        }
    }
    return FALSE;
}

/* ===========================================================================
 * saving
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * editor_save() — serialize the buffer and persist everything: content
 * blob, derived title, and the note's tag set.  Also refreshes the window
 * title and pokes the library window to update its lists.
 * ------------------------------------------------------------------------- */
static void
editor_save(OnEditor *ed)
{
    gsize   blob_len = 0;            /* ONBF blob size                      */
    guint8 *blob = on_note_serialize(ed->buffer, &blob_len);
    gchar  *title = on_buffer_first_line(ed->buffer);

    on_db_note_save(ed->app->db, ed->note_id, title, blob, blob_len);

    /* Recompute the tag set from the text.                                 */
    GList *tags = on_buffer_collect_tags(ed->buffer);
    on_db_note_set_tags(ed->app->db, ed->note_id, tags);
    g_list_free_full(tags, g_free);

    /* Window title mirrors the note title.                                 */
    if (ed->window != NULL) {
        gchar *wtitle = g_strdup_printf("Orange Notes - %s", title);
        gtk_window_set_title(GTK_WINDOW(ed->window), wtitle);
        g_free(wtitle);
    }

    g_free(title);
    g_free(blob);

    if (ed->app->notify_notes_changed != NULL)
        ed->app->notify_notes_changed(ed->app);
}

/* on_autosave_timeout() — the debounce timer fired: save now.               */
static gboolean
on_autosave_timeout(gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    ed->autosave_source = 0;
    editor_save(ed);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * editor_queue_autosave() — (re)arm the autosave debounce timer.
 * ------------------------------------------------------------------------- */
static void
editor_queue_autosave(OnEditor *ed)
{
    if (ed->autosave_source != 0)
        g_source_remove(ed->autosave_source);
    ed->autosave_source = g_timeout_add(AUTOSAVE_DELAY_MS,
                                        on_autosave_timeout, ed);
}

/* ---------------------------------------------------------------------------
 * on_editor_destroy() — the window is going away: flush a final save,
 * drop the editor from the open-editors table, and free everything.
 * ------------------------------------------------------------------------- */
static void
on_editor_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnEditor *ed = user_data;        /* owning editor                       */

    if (ed->autosave_source != 0) {
        g_source_remove(ed->autosave_source);
        ed->autosave_source = 0;
    }
    if (ed->code_btn_idle != 0) {
        g_source_remove(ed->code_btn_idle);
        ed->code_btn_idle = 0;
    }
    g_slist_free(ed->code_buttons);  /* widgets die with the window         */
    ed->code_buttons = NULL;

    ed->window = NULL;               /* don't touch the dying window        */
    editor_save(ed);                 /* buffer is kept alive by our ref     */

    if (ed->tag_popup != NULL)
        gtk_widget_destroy(ed->tag_popup);

    g_hash_table_remove(ed->app->editors, &ed->note_id);
    g_object_unref(ed->buffer);
    g_free(ed);
}

/* ===========================================================================
 * toolbar + window construction
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * add_para_button() — helper: append a paragraph-style tool button.
 *   ed       — the editor.
 *   toolbar  — the GtkToolbar to append to.
 *   icon     — local icon file basename, or NULL.
 *   fallback — markup shown as the icon when the file is missing.
 *   label    — button text label.
 *   tooltip  — hover help.
 *   flag     — paragraph style the button applies (0 = body).
 * ------------------------------------------------------------------------- */
static void
add_para_button(OnEditor *ed, GtkWidget *toolbar, const gchar *icon,
                const gchar *fallback, const gchar *label,
                const gchar *tooltip, guint32 flag)
{
    GtkToolItem *item = on_app_tool_item_new(ed->app, FALSE, icon,
                                             fallback, label, tooltip);
    g_object_set_data(G_OBJECT(item), "on-flag", GUINT_TO_POINTER(flag));
    g_signal_connect(item, "clicked", G_CALLBACK(on_para_button), ed);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);
}

/* ---------------------------------------------------------------------------
 * build_toolbar() — construct the formatting toolbar: inline-style
 * toggles, paragraph-style buttons, code-block and image insertion.  The
 * toolbar is registered with the app so it follows the global
 * text/icons/both style preference.
 * Returns the toolbar widget.
 * ------------------------------------------------------------------------- */
static GtkWidget *
build_toolbar(OnEditor *ed)
{
    GtkWidget *toolbar = gtk_toolbar_new();
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar),
                              GTK_ICON_SIZE_SMALL_TOOLBAR);

    /* Inline style toggles (B, I, U, S).                                   */
    for (gsize i = 0; i < G_N_ELEMENTS(INLINE_TOGGLES); i++) {
        GtkToolItem *item = on_app_tool_item_new(
            ed->app, TRUE, INLINE_TOGGLES[i].icon, INLINE_TOGGLES[i].markup,
            INLINE_TOGGLES[i].label, INLINE_TOGGLES[i].tooltip);
        g_object_set_data(G_OBJECT(item), "on-flag",
                          GUINT_TO_POINTER(INLINE_TOGGLES[i].flag));
        g_signal_connect(item, "toggled",
                         G_CALLBACK(on_inline_toggle), ed);
        gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);
        ed->toggle_buttons[i] = GTK_WIDGET(item);
    }

    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    /* Paragraph styles.  These have no standard icons, so their "icons"
     * are text glyphs (still swappable by dropping a matching PNG — e.g.
     * heading-1.png — into the icons/ folder).                             */
    add_para_button(ed, toolbar, "heading-1", "<b>H1</b>", "Heading 1",
                    "Heading 1", ON_FMT_H1);
    add_para_button(ed, toolbar, "heading-2", "<b>H2</b>", "Heading 2",
                    "Heading 2", ON_FMT_H2);
    add_para_button(ed, toolbar, "body-text", "\xc2\xb6", "Body",
                    "Plain body text", 0);

    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    add_para_button(ed, toolbar, "list-bullet", "\xe2\x80\xa2", "Bullets",
                    "Bulleted list", ON_FMT_LIST_BULLET);
    add_para_button(ed, toolbar, "list-number", "1.", "Numbered",
                    "Numbered list", ON_FMT_LIST_NUMBER);

    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    add_para_button(ed, toolbar, "code-block", "{\xc2\xa0}", "Code",
                    "Code block (monospace)", ON_FMT_CODEBLOCK);

    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    GtkToolItem *img_item = on_app_tool_item_new(
        ed->app, FALSE, "insert-image", "\xf0\x9f\x96\xbc",
        "Image\xe2\x80\xa6",
        "Insert an image from a file (or just paste one)");
    g_signal_connect(img_item, "clicked",
                     G_CALLBACK(on_insert_image_clicked), ed);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), img_item, -1);

    on_app_register_toolbar(ed->app, ON_TOOLBAR_EDITOR, toolbar);
    return toolbar;
}

GtkWidget *
on_editor_window_open(OnApp *app, gint64 note_id)
{
    /* Already open?  Just raise the existing window.                       */
    GtkWidget *existing = g_hash_table_lookup(app->editors, &note_id);
    if (existing != NULL) {
        gtk_window_present(GTK_WINDOW(existing));
        return existing;
    }

    OnNoteMeta *meta = on_db_note_get(app->db, note_id);
    if (meta == NULL) {
        g_warning("editor: note %" G_GINT64_FORMAT " does not exist",
                  note_id);
        return NULL;
    }

    OnEditor *ed = g_new0(OnEditor, 1);
    ed->app     = app;
    ed->note_id = note_id;

    /* --- window: a plain GtkWindow, standard titlebar (no HeaderBar) ---- */
    ed->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(ed->window), 720, 800);
    gtk_application_add_window(app->gtk_app, GTK_WINDOW(ed->window));
    {
        gchar *wtitle = g_strdup_printf("Orange Notes - %s", meta->title);
        gtk_window_set_title(GTK_WINDOW(ed->window), wtitle);
        g_free(wtitle);
    }

    /* --- text view ------------------------------------------------------ */
    ed->view = GTK_TEXT_VIEW(gtk_text_view_new());
    ed->buffer = gtk_text_view_get_buffer(ed->view);
    g_object_ref(ed->buffer);        /* keep alive for the final save       */
    on_buffer_ensure_tags(ed->buffer);

    gtk_text_view_set_wrap_mode(ed->view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(ed->view, 16);
    gtk_text_view_set_right_margin(ed->view, 16);
    gtk_text_view_set_top_margin(ed->view, 12);
    gtk_text_view_set_bottom_margin(ed->view, 12);
    gtk_text_view_set_pixels_above_lines(ed->view, 2);

    /* --- load content ---------------------------------------------------- */
    gsize   blob_len = 0;            /* stored ONBF blob size               */
    guint8 *blob = on_db_note_load(app->db, note_id, &blob_len);
    if (blob != NULL) {
        ed->internal_change++;
        on_note_deserialize(ed->buffer, blob, blob_len);
        editor_attach_image_widgets(ed);
        ed->internal_change--;
        g_free(blob);
    }
    gtk_text_buffer_set_modified(ed->buffer, FALSE);

    /* --- layout ----------------------------------------------------------*/
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), build_toolbar(ed), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll),
                                              FALSE);
    gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(ed->view));
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(ed->window), vbox);

    /* --- signals (connected after load so loading doesn't autosave) ----- */
    g_signal_connect_after(ed->buffer, "insert-text",
                           G_CALLBACK(on_buffer_insert_text_after), ed);
    g_signal_connect_after(ed->buffer, "delete-range",
                           G_CALLBACK(on_buffer_delete_range_after), ed);
    g_signal_connect(ed->buffer, "changed",
                     G_CALLBACK(on_buffer_changed), ed);
    g_signal_connect(ed->buffer, "notify::cursor-position",
                     G_CALLBACK(on_cursor_moved), ed);
    g_signal_connect(ed->view, "key-press-event",
                     G_CALLBACK(on_view_key_press), ed);
    g_signal_connect(ed->view, "paste-clipboard",
                     G_CALLBACK(on_paste_clipboard), ed);
    g_signal_connect(ed->view, "button-press-event",
                     G_CALLBACK(on_view_button_press), ed);
    g_signal_connect(ed->view, "populate-popup",
                     G_CALLBACK(on_view_populate_popup), ed);
    g_signal_connect(ed->view, "size-allocate",
                     G_CALLBACK(on_view_size_allocate), ed);
    g_signal_connect(gtk_scrollable_get_vadjustment(
                         GTK_SCROLLABLE(ed->view)),
                     "value-changed", G_CALLBACK(on_view_scrolled), ed);
    g_signal_connect(ed->window, "destroy",
                     G_CALLBACK(on_editor_destroy), ed);

    /* Give existing code blocks their floating copy buttons.               */
    code_buttons_queue_rebuild(ed);

    /* Register in the open-editors table (key freed by the table), and
     * stash the editor state on its window for cross-module access.        */
    gint64 *key = g_new(gint64, 1);
    *key = note_id;
    g_hash_table_insert(app->editors, key, ed->window);
    g_object_set_data(G_OBJECT(ed->window), "on-editor", ed);

    on_db_note_meta_free(meta);
    gtk_widget_show_all(ed->window);
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
    return ed->window;
}
