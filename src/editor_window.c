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
 *                        it fires the buffer is serialized to BNBF and
 *                        written to SQLite, and the note's tag set is
 *                        recomputed from the text.
 * =========================================================================== */

#include "editor_window.h"
#include "serialize.h"

#include <gdk/gdkkeysyms.h>
#include <string.h>
#include <unistd.h>

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

/* Left margin of code blocks: the default from on_buffer_ensure_tags,
 * and the widened variant that leaves room for painted line numbers
 * inside the block's shading.                                              */
#define CODEBLOCK_LEFT_MARGIN        24
#define CODEBLOCK_LEFT_MARGIN_NUMS   36

/* The inline (character-level) formatting bits.                            */
#define INLINE_MASK (ON_FMT_BOLD | ON_FMT_ITALIC | \
                     ON_FMT_UNDERLINE | ON_FMT_STRIKE)

/* The paragraph (line-level) formatting bits.                              */
#define PARA_MASK (ON_FMT_H1 | ON_FMT_H2 | ON_FMT_CODEBLOCK | \
                   ON_FMT_LIST_BULLET | ON_FMT_LIST_NUMBER | \
                   ON_FMT_LIST_CHECK)

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
 *   typing_insert   — TRUE from the insert-text before-handler to the
 *                     after-handler for short (typed) insertions, so
 *                     on_cursor_moved() knows the cursor jump it sees
 *                     mid-insert comes from typing and must not adopt
 *                     the (still untagged) new character's style —
 *                     that would wipe a style armed with no selection.
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
 *   dirty           — TRUE while the note has edits the database hasn't
 *                     seen (set whenever an autosave is queued, cleared
 *                     by editor_save); closing a window with no unsaved
 *                     edits skips the final save entirely.
 *   tags_modified   — TRUE when an edit created, renamed or deleted a
 *                     styled #tag since the last save.  Maintained LIVE
 *                     by the tag-capture/insert/delete handlers, so
 *                     editor_save knows — without any buffer scan —
 *                     whether note_tags and the library's tag sidebar
 *                     need updating.
 *   auto_h1         — TRUE while the first line of a brand-new note is
 *                     being typed and should be styled as Heading 1
 *                     (File → Settings…); cleared once the title line
 *                     is finished (Enter pressed).
 *   status_path     — status-bar label (bottom left): the note's folder
 *                     path, same format as the library window's.  Set at
 *                     open and refreshed on window focus-in, so a move
 *                     made in the library shows up on return.
 *   undo_stack      — past buffer snapshots, oldest first (see the
 *                     undo/redo section for the whole design).
 *   redo_stack      — snapshots undone and re-doable, oldest first.
 *   undo_current    — snapshot matching the last committed buffer state
 *                     (what Ctrl+Z returns FROM).
 *   undo_commit_source — pending group-commit timer id, 0 if none.
 *   undo_restoring  — TRUE while undo/redo rebuilds the buffer, so the
 *                     resulting mutations don't record history.
 *   undo_sentences  — sentence enders ('.'/'?') typed into the pending
 *                     undo group; the UNDO_MAX_SENTENCES-th commits the
 *                     group early (reset on every commit).
 * ------------------------------------------------------------------------- */
typedef struct UndoSnap UndoSnap;    /* defined in the undo/redo section    */

typedef struct {
    OnApp          *app;
    gint64          note_id;
    GtkWidget      *window;
    GtkTextView    *view;
    GtkTextBuffer  *buffer;

    guint32         inline_flags;
    gboolean        typing_insert;
    gint            internal_change;
    guint           autosave_source;

    GtkWidget      *toggle_buttons[4];
    GtkWidget      *toolbar;          /* the formatting toolbar              */
    GtkWidget      *toolbar_box;      /* vbox it sits in (for live rebuild)  */

    GtkTextMark    *tag_start;
    GtkWidget      *tag_popup;
    GtkWidget      *tag_listbox;
    GList          *tag_choices;      /* OnTag* snapshot for the active
                                       * capture — queried once at '#',
                                       * filtered in memory per keystroke  */

    GSList         *code_buttons;
    guint           code_btn_idle;
    guint           scroll_idle;
    gint            popup_x;
    gint            popup_y;

    GtkWidget      *search_entry;
    gchar          *pending_search;   /* initial in-note query, applied once */
    guint           initial_search_idle;
    gboolean        dirty;
    gboolean        tags_modified;
    gboolean        auto_h1;
    GtkWidget      *status_path;

    GPtrArray      *undo_stack;
    GPtrArray      *redo_stack;
    UndoSnap       *undo_current;
    guint           undo_commit_source;
    gboolean        undo_restoring;
    gint            undo_sentences;
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
static void     attach_table_widget(OnEditor *ed,
                                    GtkTextChildAnchor *anchor);
static void     attach_checkbox_widget(OnEditor *ed,
                                       GtkTextChildAnchor *anchor);
static void     insert_checkbox_at(OnEditor *ed, gint at);
static void     editor_search_next(OnEditor *ed);
static void     undo_notify_change(OnEditor *ed);

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
        { ON_FMT_LIST_CHECK,  ON_TAGNAME_LIST_CHECK  },
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
    /* Task lines: the prefix is a checkbox anchor (+ separating space).    */
    GtkTextChildAnchor *anchor =
        gtk_text_iter_get_child_anchor(line_start);
    if (anchor != NULL && on_anchor_is_checkbox(anchor, NULL)) {
        GtkTextIter nx = *line_start;      /* char after the anchor         */
        gtk_text_iter_forward_char(&nx);
        gint n = (gtk_text_iter_get_char(&nx) == ' ') ? 2 : 1;
        GtkTextIter del_end = *line_start;
        gtk_text_iter_forward_chars(&del_end, n);
        gint line = gtk_text_iter_get_line(line_start);
        gtk_text_buffer_delete(ed->buffer, line_start, &del_end);
        gtk_text_buffer_get_iter_at_line(ed->buffer, line_start, line);
        return TRUE;
    }

    GtkTextIter probe_end = *line_start;   /* end of the probe window       */
    /* A prefix is at most "9999. " — 6 chars; don't run past the line.     */
    for (gint i = 0; i < 7 && !gtk_text_iter_ends_line(&probe_end); i++)
        gtk_text_iter_forward_char(&probe_end);

    gchar *head = gtk_text_buffer_get_text(ed->buffer, line_start,
                                           &probe_end, FALSE);
    glong strip_chars = 0;           /* how many characters to delete       */

    if (g_str_has_prefix(head, "\xe2\x80\xa2 ") ||
        (on_char_is_checkbox(g_utf8_get_char(head), NULL) &&
         g_utf8_get_char(g_utf8_next_char(head)) == ' ')) {
        strip_chars = 2;             /* glyph + one space                   */
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
        { ON_FMT_LIST_CHECK,  ON_TAGNAME_LIST_CHECK  },
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
        } else if (flag == ON_FMT_LIST_CHECK) {
            insert_checkbox_at(ed, gtk_text_iter_get_offset(&ls));
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
 * style each button applies is stashed on it as object data "on-flag".
 * The buttons toggle: when every line in the selection already carries
 * the button's style, clicking it reverts those lines to body text.         */
static void
on_para_button(GtkToolButton *btn, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    guint32 flag = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn),
                                                      "on-flag"));

    if (flag != 0) {
        /* Determine the affected line range (same rule as apply).          */
        GtkTextIter start, end;      /* selection (or cursor twice)         */
        if (!gtk_text_buffer_get_selection_bounds(ed->buffer,
                                                  &start, &end)) {
            gtk_text_buffer_get_iter_at_mark(
                ed->buffer, &start,
                gtk_text_buffer_get_insert(ed->buffer));
            end = start;
        }
        gboolean all_have = TRUE;    /* every line already this style?      */
        for (gint l = gtk_text_iter_get_line(&start);
             l <= gtk_text_iter_get_line(&end); l++) {
            GtkTextIter it;
            gtk_text_buffer_get_iter_at_line(ed->buffer, &it, l);
            if (line_para_flags(ed->buffer, &it) != flag) {
                all_have = FALSE;
                break;
            }
        }
        if (all_have)
            flag = 0;                /* toggle off: back to body text       */
    }
    apply_paragraph_format(ed, flag);
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
    if (para != ON_FMT_LIST_BULLET && para != ON_FMT_LIST_NUMBER &&
        para != ON_FMT_LIST_CHECK)
        return FALSE;

    const gchar *tag_name =          /* the list tag for this line type     */
        (para == ON_FMT_LIST_BULLET) ? ON_TAGNAME_LIST_BULLET
      : (para == ON_FMT_LIST_CHECK)  ? ON_TAGNAME_LIST_CHECK
                                     : ON_TAGNAME_LIST_NUMBER;
    gint line = gtk_text_iter_get_line(&cursor);

    /* Measure the line's content beyond its prefix.                        */
    GtkTextIter ls, le;              /* text-only line span                 */
    gtk_text_buffer_get_iter_at_line(ed->buffer, &ls, line);
    le = ls;
    if (!gtk_text_iter_ends_line(&le))
        gtk_text_iter_forward_to_line_end(&le);
    gchar *text = gtk_text_buffer_get_text(ed->buffer, &ls, &le, FALSE);

    /* Compute the character length of the prefix on this line.             */
    glong prefix_chars = 0;          /* prefix length in characters         */
    if (para == ON_FMT_LIST_CHECK) {
        /* The prefix is a checkbox anchor (+ separating space).            */
        GtkTextChildAnchor *a = gtk_text_iter_get_child_anchor(&ls);
        if (a != NULL && on_anchor_is_checkbox(a, NULL)) {
            GtkTextIter nx = ls;
            gtk_text_iter_forward_char(&nx);
            prefix_chars = (gtk_text_iter_get_char(&nx) == ' ') ? 2 : 1;
        }
    } else if (para == ON_FMT_LIST_BULLET) {
        if (g_str_has_prefix(text, "\xe2\x80\xa2 "))
            prefix_chars = 2;
    } else {
        glong d = 0;
        while (g_ascii_isdigit(text[d]))
            d++;
        if (d > 0 && text[d] == '.' && text[d + 1] == ' ')
            prefix_chars = d + 2;
    }
    /* Length in characters INCLUDING anchors (get_text drops them).        */
    gboolean item_empty =
        (glong)(gtk_text_iter_get_offset(&le) -
                gtk_text_iter_get_offset(&ls)) <= prefix_chars;

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
        /* Continue the list: newline + next prefix, tagged.  A new task
         * item always starts unchecked.                                    */
        gint at = gtk_text_iter_get_offset(&cursor);
        if (para == ON_FMT_LIST_CHECK) {
            gtk_text_buffer_insert(ed->buffer, &cursor, "\n", -1);
            GtkTextIter nl_s, nl_e;  /* the newline character               */
            gtk_text_buffer_get_iter_at_offset(ed->buffer, &nl_s, at);
            gtk_text_buffer_get_iter_at_offset(ed->buffer, &nl_e, at + 1);
            gtk_text_buffer_apply_tag_by_name(ed->buffer, tag_name,
                                              &nl_s, &nl_e);
            insert_checkbox_at(ed, at + 1);  /* anchor + space, tagged      */
        } else {
            gchar *next_prefix;      /* text inserted after the newline     */
            if (para == ON_FMT_LIST_BULLET) {
                next_prefix = g_strdup("\n\xe2\x80\xa2 ");
            } else {
                glong n = g_ascii_strtoll(text, NULL, 10);
                next_prefix = g_strdup_printf("\n%ld. ", n + 1);
            }
            gtk_text_buffer_insert(ed->buffer, &cursor, next_prefix, -1);

            GtkTextIter ins_s, ins_e;    /* span of the inserted text       */
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
 * code_buttons_update_positions() — anchor every copy button to the
 * upper-right corner of its block, in BUFFER coordinates.
 *
 * GTK3 model (verified empirically by origin-probing a test child, see
 * CLAUDE.md): text-window children are anchored to the TEXT — they ride
 * scrolling at 1x on their own, and a move_child() issued while
 * scrolled does not even take effect until the next validate/allocate
 * cycle.  So positions must be buffer-anchored and must NOT be
 * recomputed from the scroll position; this runs only when content or
 * geometry changes (rebuild, size-allocate), never on scroll.
 * ------------------------------------------------------------------------- */
static void
code_buttons_update_positions(OnEditor *ed)
{
    GdkWindow *text_win =            /* the view's scrolling text window    */
        gtk_text_view_get_window(ed->view, GTK_TEXT_WINDOW_TEXT);
    if (text_win == NULL)
        return;
    gint win_width = gdk_window_get_width(text_win);

    for (GSList *l = ed->code_buttons; l != NULL; l = l->next) {
        GtkWidget *btn = l->data;    /* one floating copy button            */
        GtkTextMark *mark = g_object_get_data(G_OBJECT(btn), "on-mark");
        if (mark == NULL)
            continue;

        /* The button's CSS pins it to exactly CODE_BTN_SIZE square in
         * every state (normal/hover/active), so positioning can rely on
         * the constant instead of chasing theme-dependent allocations.     */
        const gint bw = CODE_BTN_SIZE;

        GtkTextIter it;              /* block start position                */
        gtk_text_buffer_get_iter_at_mark(ed->buffer, &it, mark);
        gtk_text_iter_set_line_offset(&it, 0);   /* normalize to line start */

        /* Ask the layout for the whole display line's y-range: it starts
         * exactly where the paragraph background (the shading) starts —
         * unlike the character box, which sits below the above-line
         * spacing and drifted the button downward.                         */
        gint line_y, line_h;         /* line extent in buffer coords        */
        gtk_text_view_get_line_yrange(ed->view, &it, &line_y, &line_h);

        /* Equal CODE_BTN_MARGIN insets from the shaded top/right edges.
         * move_child() coordinates land as-is (no top-margin shift on
         * this path — verified on screen; only the INITIAL allocation of
         * a freshly added child re-adds the margin).                       */
        gtk_text_view_move_child(
            ed->view, btn,
            win_width - CODEBLOCK_RIGHT_MARGIN - bw - CODE_BTN_MARGIN,
            line_y + CODE_BTN_MARGIN);
    }
}

/* ---------------------------------------------------------------------------
 * code_buttons_rebuild() — destroy and recreate the floating copy buttons
 * to match the code blocks currently present in the buffer.
 * ------------------------------------------------------------------------- */
static void
code_buttons_rebuild(OnEditor *ed)
{
    GtkTextTag *tag = lookup_tag(ed->buffer, ON_TAGNAME_CODEBLOCK);

    /* Fast path: this runs after EVERY buffer change, but the set of
     * blocks rarely changes while typing.  When the current buttons'
     * marks already sit exactly on the block starts, just reposition —
     * no widget churn.                                                     */
    if (tag != NULL && ed->app->code_copy_buttons) {
        GArray *starts =             /* block start offsets, ascending      */
            g_array_new(FALSE, FALSE, sizeof(gint));
        GtkTextIter scan;
        gtk_text_buffer_get_start_iter(ed->buffer, &scan);
        while (TRUE) {
            if (!gtk_text_iter_starts_tag(&scan, tag)) {
                if (!gtk_text_iter_forward_to_tag_toggle(&scan, tag))
                    break;
                if (!gtk_text_iter_starts_tag(&scan, tag))
                    continue;
            }
            gint off = gtk_text_iter_get_offset(&scan);
            g_array_append_val(starts, off);
            if (!gtk_text_iter_forward_to_tag_toggle(&scan, tag))
                break;
        }

        gboolean same =              /* do buttons match block starts?      */
            (guint)g_slist_length(ed->code_buttons) == starts->len;
        if (same) {
            /* Buttons were prepended while scanning forward, so the list
             * is in REVERSE document order.                                */
            guint i = starts->len;
            for (GSList *l = ed->code_buttons; same && l != NULL;
                 l = l->next) {
                GtkTextMark *mark =
                    g_object_get_data(G_OBJECT(l->data), "on-mark");
                GtkTextIter mi;
                gtk_text_buffer_get_iter_at_mark(ed->buffer, &mi, mark);
                same = (i > 0) &&
                       gtk_text_iter_get_offset(&mi) ==
                           g_array_index(starts, gint, --i);
            }
        }
        g_array_free(starts, TRUE);
        if (same) {
            code_buttons_update_positions(ed);
            return;
        }
    }

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
        GtkWidget *icon = on_app_icon_image_sized(ed->app, "copy", 16);
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

/* ---------------------------------------------------------------------------
 * on_view_draw() — paint line numbers INSIDE each code block: the block's
 * left margin is widened (see editor_apply_line_numbers) and the numbers
 * are drawn onto that strip of the block's own shading, right-aligned
 * just before the code text.  Painted, not text — selection and copying
 * can never include them.  Each block numbers from 1.
 * ------------------------------------------------------------------------- */
static gboolean
on_view_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    if (!ed->app->code_line_numbers)
        return FALSE;

    GdkWindow *text_win = gtk_text_view_get_window(ed->view,
                                                   GTK_TEXT_WINDOW_TEXT);
    if (text_win == NULL || !gtk_cairo_should_draw_window(cr, text_win))
        return FALSE;
    GtkTextTag *tag = lookup_tag(ed->buffer, ON_TAGNAME_CODEBLOCK);
    if (tag == NULL)
        return FALSE;

    cairo_save(cr);
    gtk_cairo_transform_to_window(cr, widget, text_win);

    /* Start at the first visible buffer line.                              */
    GdkRectangle vis;                /* visible area in buffer coords       */
    gtk_text_view_get_visible_rect(ed->view, &vis);
    GtkTextIter it;                  /* walking line iterator               */
    gtk_text_view_get_line_at_y(ed->view, &it, vis.y, NULL);
    gtk_text_iter_set_line_offset(&it, 0);

    /* If that line sits mid-block, count how far into the block it is.     */
    gint num = 0;                    /* number of the PREVIOUS code line    */
    if (gtk_text_iter_has_tag(&it, tag)) {
        gint first = gtk_text_iter_get_line(&it);
        while (first > 0) {
            GtkTextIter prev;
            gtk_text_buffer_get_iter_at_line(ed->buffer, &prev, first - 1);
            if (!gtk_text_iter_has_tag(&prev, tag))
                break;
            first--;
        }
        num = gtk_text_iter_get_line(&it) - first;
    }

    PangoLayout *layout =            /* renders the number strings          */
        gtk_widget_create_pango_layout(widget, NULL);
    PangoFontDescription *fd =
        pango_font_description_from_string("monospace 9");
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);

    while (TRUE) {
        gint y, h;                   /* line extent in buffer coords        */
        gtk_text_view_get_line_yrange(ed->view, &it, &y, &h);
        if (y > vis.y + vis.height)
            break;

        if (gtk_text_iter_has_tag(&it, tag)) {
            num++;

            /* The line's first character marks where the code text
             * begins; the gutter band and its number sit just left of
             * it, over the block's shading.                                */
            GdkRectangle rect;       /* first char, buffer coords           */
            gtk_text_view_get_iter_location(ed->view, &it, &rect);
            gint wx, wy;             /* char position in window coords      */
            gtk_text_view_buffer_to_window_coords(
                ed->view, GTK_TEXT_WINDOW_TEXT, rect.x, rect.y,
                &wx, &wy);
            gint wy_line;            /* line-range top in window coords     */
            gtk_text_view_buffer_to_window_coords(
                ed->view, GTK_TEXT_WINDOW_TEXT, 0, y, NULL, &wy_line);

            /* Grey gutter band behind the number, spanning the whole
             * line range so adjacent lines tile seamlessly.                */
            cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
            cairo_rectangle(cr, wx - 20, wy_line, 16, h);
            cairo_fill(cr);

            gchar text[16];          /* the printed number                  */
            g_snprintf(text, sizeof text, "%d", num);
            pango_layout_set_text(layout, text, -1);
            gint tw, th;             /* rendered number size                */
            pango_layout_get_pixel_size(layout, &tw, &th);
            cairo_set_source_rgb(cr, 0.30, 0.30, 0.30);
            cairo_move_to(cr, wx - 6 - tw, wy + 1);
            pango_cairo_show_layout(cr, layout);
        } else {
            num = 0;                 /* block ended: restart numbering      */
        }
        if (!gtk_text_iter_forward_line(&it))
            break;
    }
    g_object_unref(layout);
    cairo_restore(cr);
    return FALSE;
}

/* editor_apply_line_numbers() — widen/narrow the code-block left margin
 * (making room for the painted numbers inside the shading) and redraw.      */
static void
editor_apply_line_numbers(OnEditor *ed)
{
    GtkTextTag *tag = lookup_tag(ed->buffer, ON_TAGNAME_CODEBLOCK);
    if (tag != NULL)
        g_object_set(tag, "left-margin",
                     ed->app->code_line_numbers
                         ? CODEBLOCK_LEFT_MARGIN_NUMS
                         : CODEBLOCK_LEFT_MARGIN,
                     NULL);
    gtk_widget_queue_draw(GTK_WIDGET(ed->view));
}

void
on_editor_apply_line_numbers_all(OnApp *app)
{
    GHashTableIter iter;             /* walk of the open-editors table      */
    gpointer key, value;
    g_hash_table_iter_init(&iter, app->editors);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        OnEditor *ed =               /* editor state stashed on its window  */
            g_object_get_data(G_OBJECT(value), "on-editor");
        if (ed != NULL)
            editor_apply_line_numbers(ed);
    }
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

/* on_view_size_allocate() — re-anchor buttons when the view's geometry
 * changes (width affects their x; reflow affects their line_y).  Scroll
 * needs NO handling: the buttons are text-window children and ride the
 * scroll on their own.                                                      */
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
    gint h = gdk_pixbuf_get_height(orig);
    if (display_width > 0)
        return MIN(display_width, w);

    /* Thumbnail mode: fit inside ON_IMAGE_THUMB_W × ON_IMAGE_THUMB_H
     * (aspect preserved), never upscaling.                                 */
    gint tw = MIN(w, ON_IMAGE_THUMB_W);
    if (h > 0 && h * tw > ON_IMAGE_THUMB_H * w)  /* still too tall at tw   */
        tw = w * ON_IMAGE_THUMB_H / h;
    return MAX(tw, 1);
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
        if (anchor == NULL)
            continue;
        if (on_anchor_is_checkbox(anchor, NULL))
            attach_checkbox_widget(ed, anchor);
        else if (on_anchor_get_table(anchor) != NULL)
            attach_table_widget(ed, anchor);
        else
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

/* on_img_open() — "Open": write the full-resolution image to a temporary
 * PNG file and hand it to an image viewer — the program configured under
 * Settings ("image_viewer" setting), or the platform opener (macOS
 * `open`, otherwise `xdg-open`) which launches the system's default
 * viewer.                                                                   */
static void
on_img_open(GtkMenuItem *item, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    GtkTextChildAnchor *anchor = anchor_at_offset(
        ed, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item),
                                              "on-offset")));
    GdkPixbuf *orig = anchor != NULL
                      ? on_anchor_get_image(anchor, NULL) : NULL;
    if (orig == NULL)
        return;

    GError *err = NULL;
    gchar  *path = NULL;             /* temporary PNG path                  */
    gint fd = g_file_open_tmp("blue-note-XXXXXX.png", &path, &err);
    if (fd < 0) {
        g_warning("cannot create temp image file: %s", err->message);
        g_clear_error(&err);
        return;
    }
    close(fd);
    if (!gdk_pixbuf_save(orig, path, "png", &err, NULL)) {
        g_warning("cannot write temp image file: %s", err->message);
        g_clear_error(&err);
        g_free(path);
        return;
    }

    gchar *viewer = on_app_config_get("image_viewer");
#ifdef __APPLE__
    const gchar *opener = "open";
#else
    const gchar *opener = "xdg-open";
#endif
    gchar *argv[] = {
        (viewer != NULL && *viewer != '\0') ? viewer : (gchar *)opener,
        path, NULL
    };
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, &err)) {
        g_warning("cannot launch image viewer: %s", err->message);
        g_clear_error(&err);
    }
    g_free(viewer);
    g_free(path);
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
    return FALSE;                    /* never consume: default handling     */
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

    /* Which buffer position was clicked?  get_iter_at_position, NOT
     * get_iter_at_location: the latter returns CURSOR positions, so a
     * click on the right half of an image resolves to the gap after it —
     * and with two adjacent images that gap IS the next image's anchor,
     * so the wrong image's menu appeared.  This returns the character
     * the click is inside, whichever half was hit.                        */
    gint bx, by;                     /* click in buffer coordinates         */
    gtk_text_view_window_to_buffer_coords(view, GTK_TEXT_WINDOW_TEXT,
                                          ed->popup_x, ed->popup_y,
                                          &bx, &by);
    GtkTextIter it;                  /* iter under the pointer              */
    gtk_text_view_get_iter_at_position(view, &it, NULL, bx, by);

    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&it);
    if (anchor == NULL) {
        /* Clicks past the end of the line resolve to the line-end iter;
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
        { "_Open",                 G_CALLBACK(on_img_open), TRUE },
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
 * (e.g. a screenshot), embed it and swallow the default paste; otherwise fall
 * through so GTK pastes text / rich text normally.
 *
 * We probe SPECIFIC image atoms rather than gtk_clipboard_wait_is_image_
 * available / gtk_clipboard_wait_for_image: those request the special TARGETS
 * atom, which on macOS makes GDK enumerate every NSPasteboard type and convert
 * each via gdk_atom_intern (gdkselection-quartz.c) — a call that asserts on
 * Apple-private types whose UTI has no MIME string.  gtk_clipboard_wait_for_
 * contents on one concrete image atom goes straight to [NSPasteboard
 * dataForType:] with no enumeration.  It is synchronous on the Quartz backend,
 * so the image is embedded before we decide whether to stop the emission.
 *
 * The default text-paste path (and the right-click/selection-bubble menus)
 * still hit that TARGETS enumeration internally; the resulting benign
 * "gdk_atom_intern: assertion 'atom_name != NULL'" critical is filtered at the
 * source by quartz_log_filter() in main.c.
 * ------------------------------------------------------------------------- */
static const gchar * const PASTE_IMG_ATOMS[] = {
    "image/png",                /* public.png  — web / app images            */
    "image/tiff",               /* public.tiff — macOS screenshots           */
    "image/jpeg",
    "image/bmp",
};

static void
on_paste_clipboard(GtkTextView *view, gpointer user_data)
{
    OnEditor     *ed = user_data;    /* owning editor                        */
    GtkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(view),
                                                GDK_SELECTION_CLIPBOARD);

    for (guint i = 0; i < G_N_ELEMENTS(PASTE_IMG_ATOMS); i++) {
        GtkSelectionData *sel = gtk_clipboard_wait_for_contents(
            cb, gdk_atom_intern_static_string(PASTE_IMG_ATOMS[i]));
        if (sel == NULL)
            continue;
        GdkPixbuf *pixbuf = gtk_selection_data_get_length(sel) > 0
            ? gtk_selection_data_get_pixbuf(sel) : NULL;
        gtk_selection_data_free(sel);
        if (pixbuf != NULL) {
            insert_image_pixbuf(ed, pixbuf);
            g_object_unref(pixbuf);
            /* Swallow the signal so the default handler doesn't also paste. */
            g_signal_stop_emission_by_name(view, "paste-clipboard");
            return;
        }
    }
    /* Not an image: let GTK's default handler paste text / rich text.       */
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
 * task-list checkboxes
 *
 * Each task line starts with a child anchor holding the checked state;
 * the editor attaches a native GtkCheckButton there.  Toggling writes
 * straight through to the anchor data and autosaves.
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * on_checkbox_enter() — hyperlink-style hand cursor while hovering a
 * task checkbox (the surrounding text view keeps its I-beam).  Set once
 * on the button's own event window; it applies whenever the pointer is
 * inside the button.
 * ------------------------------------------------------------------------- */
static gboolean
on_checkbox_enter(GtkWidget *widget, GdkEventCrossing *event,
                  gpointer user_data)
{
    (void)user_data;
    GdkCursor *hand =                /* cached on the button itself         */
        g_object_get_data(G_OBJECT(widget), "on-hand-cursor");
    if (hand == NULL) {
        hand = gdk_cursor_new_from_name(
            gdk_window_get_display(event->window), "pointer");
        g_object_set_data_full(G_OBJECT(widget), "on-hand-cursor",
                               hand, g_object_unref);
    }
    gdk_window_set_cursor(event->window, hand);
    return FALSE;
}

/* on_task_checkbox_toggled() — sync the widget state into the anchor.       */
static void
on_task_checkbox_toggled(GtkToggleButton *btn, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    GtkTextChildAnchor *anchor =
        g_object_get_data(G_OBJECT(btn), "on-anchor");
    if (anchor != NULL)
        on_anchor_set_checkbox(anchor,
                               gtk_toggle_button_get_active(btn));
    editor_queue_autosave(ed);
}

/* ---------------------------------------------------------------------------
 * attach_checkbox_widget() — give a checkbox anchor its GtkCheckButton
 * (replacing any widget it already had).
 * ------------------------------------------------------------------------- */
static void
attach_checkbox_widget(OnEditor *ed, GtkTextChildAnchor *anchor)
{
    gboolean checked;                /* the anchor's stored state           */
    if (!on_anchor_is_checkbox(anchor, &checked))
        return;

    GList *widgets = gtk_text_child_anchor_get_widgets(anchor);
    for (GList *l = widgets; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(widgets);

    GtkWidget *btn = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), checked);
    /* Keyboard focus stays in the text; the box is mouse-only.             */
    gtk_widget_set_can_focus(btn, FALSE);
    g_object_set_data(G_OBJECT(btn), "on-anchor", anchor);
    g_signal_connect(btn, "toggled",
                     G_CALLBACK(on_task_checkbox_toggled), ed);
    g_signal_connect(btn, "enter-notify-event",
                     G_CALLBACK(on_checkbox_enter), NULL);
    gtk_text_view_add_child_at_anchor(ed->view, btn, anchor);
    gtk_widget_show(btn);
}

/* ---------------------------------------------------------------------------
 * insert_checkbox_at() — put a fresh unchecked checkbox anchor (plus its
 * separating space) at buffer offset `at`, tagged as part of the line.
 * ------------------------------------------------------------------------- */
static void
insert_checkbox_at(OnEditor *ed, gint at)
{
    GtkTextIter it;                  /* insertion position                  */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &it, at);
    GtkTextChildAnchor *anchor =
        gtk_text_buffer_create_child_anchor(ed->buffer, &it);
    on_anchor_set_checkbox(anchor, FALSE);
    gtk_text_buffer_insert(ed->buffer, &it, " ", -1);

    GtkTextIter ts, te;              /* the anchor + space span             */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &ts, at);
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &te, at + 2);
    gtk_text_buffer_apply_tag_by_name(ed->buffer, ON_TAGNAME_LIST_CHECK,
                                      &ts, &te);
    attach_checkbox_widget(ed, anchor);
}

/* ===========================================================================
 * embedded tables
 *
 * A table is a child anchor carrying an OnTable (see serialize.h); the
 * editor attaches a GtkGrid of GtkEntry cells at it.  Right-clicking any
 * cell offers structural changes (add/remove rows and columns, delete),
 * which rebuild the widget from the updated data.
 * =========================================================================== */

static void attach_table_widget(OnEditor *ed, GtkTextChildAnchor *anchor);

/* on_table_cell_changed() — a cell buffer edited: write through to the
 * data (cells are small GtkTextViews so content can be multiline; a bare
 * text view requests its content size, so cells auto-grow).                 */
static void
on_table_cell_changed(GtkTextBuffer *cell_buf, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    GtkTextChildAnchor *anchor =
        g_object_get_data(G_OBJECT(cell_buf), "on-anchor");
    OnTable *table = (anchor != NULL)
                     ? on_anchor_get_table(anchor) : NULL;
    if (table == NULL)
        return;
    gint r = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell_buf),
                                               "on-row"));
    gint c = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell_buf),
                                               "on-col"));
    GtkTextIter s, e;                /* the cell's full contents            */
    gtk_text_buffer_get_bounds(cell_buf, &s, &e);
    gchar *text = gtk_text_buffer_get_text(cell_buf, &s, &e, FALSE);
    on_table_set(table, r, c, text);
    g_free(text);
    editor_queue_autosave(ed);
}

/* ---------------------------------------------------------------------------
 * table_menu_op() — one structural table operation, dispatched by the
 * "on-op" string on the activated menu item.
 * ------------------------------------------------------------------------- */
static void
table_menu_op(GtkMenuItem *item, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    GtkTextChildAnchor *anchor =
        g_object_get_data(G_OBJECT(item), "on-anchor");
    OnTable *table = (anchor != NULL)
                     ? on_anchor_get_table(anchor) : NULL;
    if (table == NULL)
        return;
    const gchar *op = g_object_get_data(G_OBJECT(item), "on-op");

    if (g_strcmp0(op, "row+") == 0)
        on_table_resize(table, table->rows + 1, table->cols);
    else if (g_strcmp0(op, "row-") == 0)
        on_table_resize(table, table->rows - 1, table->cols);
    else if (g_strcmp0(op, "col+") == 0)
        on_table_resize(table, table->rows, table->cols + 1);
    else if (g_strcmp0(op, "col-") == 0)
        on_table_resize(table, table->rows, table->cols - 1);
    else if (g_strcmp0(op, "header") == 0)
        table->header = !table->header;
    else if (g_strcmp0(op, "delete") == 0) {
        /* Remove the anchor character; its widget dies with it.            */
        GtkTextIter s, e;            /* the anchor's single character       */
        gtk_text_buffer_get_iter_at_child_anchor(ed->buffer, &s, anchor);
        e = s;
        gtk_text_iter_forward_char(&e);
        ed->internal_change++;
        gtk_text_buffer_delete(ed->buffer, &s, &e);
        ed->internal_change--;
        editor_queue_autosave(ed);
        return;
    }

    attach_table_widget(ed, anchor); /* rebuild at the new dimensions       */
    editor_queue_autosave(ed);
}

/* ---------------------------------------------------------------------------
 * on_table_cell_button_press() — right click in a cell: the structural
 * menu (add/remove last row/column, delete the table).
 * ------------------------------------------------------------------------- */
static gboolean
on_table_cell_button_press(GtkWidget *entry, GdkEventButton *event,
                           gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    if (event->button != GDK_BUTTON_SECONDARY) {
        /* Anchored children inside a GtkTextView don't reliably receive
         * focus from the default click handling — force it so the caret
         * lands in the cell, then let the default handler place it.        */
        gtk_widget_grab_focus(entry);
        return FALSE;
    }

    GtkTextChildAnchor *anchor =
        g_object_get_data(G_OBJECT(entry), "on-anchor");

    GtkWidget *menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(menu), entry, NULL);
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);

    OnTable *table = (anchor != NULL)
                     ? on_anchor_get_table(anchor) : NULL;

    static const struct { const gchar *label; const gchar *op; } OPS[] = {
        { "Add _Row",       "row+"   },
        { "Add _Column",    "col+"   },
        { "Remove Row",     "row-"   },
        { "Remove Column",  "col-"   },
        { NULL,             NULL     },
        { "_Header Row",    "header" },
        { NULL,             NULL     },
        { "_Delete Table",  "delete" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(OPS); i++) {
        GtkWidget *mi;               /* menu item (or separator)            */
        if (OPS[i].label == NULL) {
            mi = gtk_separator_menu_item_new();
        } else if (g_strcmp0(OPS[i].op, "header") == 0) {
            /* Check item mirroring the table's current header state.       */
            mi = gtk_check_menu_item_new_with_mnemonic(OPS[i].label);
            gtk_check_menu_item_set_active(
                GTK_CHECK_MENU_ITEM(mi),
                table != NULL && table->header);
            g_object_set_data(G_OBJECT(mi), "on-anchor", anchor);
            g_object_set_data(G_OBJECT(mi), "on-op", (gpointer)OPS[i].op);
            g_signal_connect(mi, "activate",
                             G_CALLBACK(table_menu_op), ed);
        } else {
            mi = gtk_menu_item_new_with_mnemonic(OPS[i].label);
            g_object_set_data(G_OBJECT(mi), "on-anchor", anchor);
            g_object_set_data(G_OBJECT(mi), "on-op", (gpointer)OPS[i].op);
            g_signal_connect(mi, "activate",
                             G_CALLBACK(table_menu_op), ed);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * attach_table_widget() — (re)build the GtkGrid of entries representing
 * an anchor's table, replacing any widget it already had.
 * ------------------------------------------------------------------------- */
static void
attach_table_widget(OnEditor *ed, GtkTextChildAnchor *anchor)
{
    OnTable *table = on_anchor_get_table(anchor);
    if (table == NULL)
        return;

    /* Drop the previous widget (after a structural change).                */
    GList *widgets = gtk_text_child_anchor_get_widgets(anchor);
    for (GList *l = widgets; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(widgets);

    GtkWidget *grid = gtk_grid_new();
    for (gint r = 0; r < table->rows; r++) {
        for (gint c = 0; c < table->cols; c++) {
            /* Each cell is a bare GtkTextView (multiline, and it requests
             * its content size so the cell auto-grows) inside a frame
             * that draws the cell border.                                  */
            GtkWidget *cell = gtk_text_view_new();
            GtkTextBuffer *cell_buf =
                gtk_text_view_get_buffer(GTK_TEXT_VIEW(cell));
            gtk_text_buffer_set_text(cell_buf,
                                     on_table_get(table, r, c), -1);
            gtk_text_view_set_left_margin(GTK_TEXT_VIEW(cell), 6);
            gtk_text_view_set_right_margin(GTK_TEXT_VIEW(cell), 6);
            gtk_text_view_set_top_margin(GTK_TEXT_VIEW(cell), 3);
            gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(cell), 3);
            gtk_widget_set_size_request(cell, 64, -1);

            /* Header row: bold on a light grey fill.                       */
            if (table->header && r == 0) {
                GtkCssProvider *css = gtk_css_provider_new();
                gtk_css_provider_load_from_data(css,
                    "textview, textview text {"
                    "  background-color: #ececec;"
                    "  font-weight: bold;"
                    "}", -1, NULL);
                gtk_style_context_add_provider(
                    gtk_widget_get_style_context(cell),
                    GTK_STYLE_PROVIDER(css),
                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
                g_object_unref(css);
            }

            g_object_set_data(G_OBJECT(cell_buf), "on-anchor", anchor);
            g_object_set_data(G_OBJECT(cell_buf), "on-row",
                              GINT_TO_POINTER(r));
            g_object_set_data(G_OBJECT(cell_buf), "on-col",
                              GINT_TO_POINTER(c));
            g_signal_connect(cell_buf, "changed",
                             G_CALLBACK(on_table_cell_changed), ed);
            g_object_set_data(G_OBJECT(cell), "on-anchor", anchor);
            g_signal_connect(cell, "button-press-event",
                             G_CALLBACK(on_table_cell_button_press), ed);

            GtkWidget *frame = gtk_frame_new(NULL);
            gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
            gtk_container_add(GTK_CONTAINER(frame), cell);
            gtk_grid_attach(GTK_GRID(grid), frame, c, r, 1, 1);
        }
    }
    gtk_text_view_add_child_at_anchor(ed->view, grid, anchor);
    gtk_widget_show_all(grid);
}

/* ---------------------------------------------------------------------------
 * is_emoji_char() — rough emoji detection: the blocks that render via the
 * color emoji font and overlap neighbouring text on macOS (Apple Color
 * Emoji draws wider than the advance Pango reserves for it).
 * ------------------------------------------------------------------------- */
static gboolean
is_emoji_char(gunichar c)
{
    return (c >= 0x1F000 && c <= 0x1FAFF) ||   /* emoji + symbols planes    */
           (c >= 0x2600  && c <= 0x27BF)  ||   /* misc symbols, dingbats    */
           (c >= 0x1F1E6 && c <= 0x1F1FF) ||   /* regional indicators       */
           c == 0x2B50 || c == 0x2B55;         /* star, circle              */
}

/* ---------------------------------------------------------------------------
 * tag_emoji_in_range() — apply the padding tag around every emoji between
 * the two buffer offsets.  The "on-emoji" tag adds letter spacing, which
 * Pango splits half-per-side at run edges — so the emoji itself gets
 * half a gap each side, and tagging the FOLLOWING character as well
 * doubles the trailing gap (Apple Color Emoji bleeds mostly rightward).
 *
 * macOS-only: on Linux, color emoji fonts (e.g. Noto) fit their advance
 * and need no artificial padding, so this is compiled to a no-op there.
 * Editor-only styling; never serialized.
 * ------------------------------------------------------------------------- */
static void
tag_emoji_in_range(OnEditor *ed, gint start_off, gint end_off)
{
#ifdef __APPLE__
    /* Start one character early so text typed directly after an existing
     * emoji still receives the follower's share of the padding.            */
    GtkTextIter it;                  /* scan cursor                         */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &it,
                                       MAX(0, start_off - 1));
    while (gtk_text_iter_get_offset(&it) < end_off) {
        if (is_emoji_char(gtk_text_iter_get_char(&it))) {
            GtkTextIter next = it;   /* the following character             */
            gtk_text_iter_forward_char(&next);
            gtk_text_buffer_apply_tag_by_name(ed->buffer, "on-emoji",
                                              &it, &next);
            if (!gtk_text_iter_is_end(&next) &&
                !is_emoji_char(gtk_text_iter_get_char(&next))) {
                GtkTextIter after = next;
                gtk_text_iter_forward_char(&after);
                gtk_text_buffer_apply_tag_by_name(ed->buffer, "on-emoji",
                                                  &next, &after);
            }
        }
        if (!gtk_text_iter_forward_char(&it))
            break;
    }
#else
    (void)ed; (void)start_off; (void)end_off;
#endif
}

/* ---------------------------------------------------------------------------
 * on_emoji_clicked() — toolbar "Emoji": open GTK's built-in emoji chooser
 * at the cursor via the text view's "insert-emoji" action (the same one
 * bound to Ctrl+. natively and Ctrl/Cmd+E in on_view_key_press).  The
 * picked emoji is inserted as plain UTF-8 text, so styling, storage and
 * export handle it like any typed character.
 * ------------------------------------------------------------------------- */
static void
on_emoji_clicked(GtkToolButton *btn, gpointer user_data)
{
    (void)btn;
    OnEditor *ed = user_data;        /* owning editor                       */
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
    g_signal_emit_by_name(ed->view, "insert-emoji");
}

/* ---------------------------------------------------------------------------
 * on_insert_table_clicked() — toolbar "Table": embed a fresh 3×3 table at
 * the cursor.  Rows/columns are added or removed afterwards from any
 * cell's right-click menu.
 * ------------------------------------------------------------------------- */
static void
on_insert_table_clicked(GtkToolButton *btn, gpointer user_data)
{
    (void)btn;
    OnEditor *ed = user_data;        /* owning editor                       */

    GtkTextIter cursor;              /* insertion point                     */
    gtk_text_buffer_get_iter_at_mark(ed->buffer, &cursor,
                                     gtk_text_buffer_get_insert(ed->buffer));
    ed->internal_change++;
    GtkTextChildAnchor *anchor =
        gtk_text_buffer_create_child_anchor(ed->buffer, &cursor);
    on_anchor_set_table(anchor, on_table_new(3, 3));
    attach_table_widget(ed, anchor);
    ed->internal_change--;
    editor_queue_autosave(ed);
}

/* ---------------------------------------------------------------------------
 * editor_insert_date() — insert today's date in ISO format (YYYY-MM-DD)
 * at the cursor, replacing the selection like typed text would.  It goes
 * through the normal insert-text path, so styling, autosave and undo of
 * the surrounding text behave exactly as for a paste.  Shared by the
 * Insert menu item and the Ctrl/Cmd+D shortcut.
 * ------------------------------------------------------------------------- */
static void
editor_insert_date(OnEditor *ed)
{
    GDateTime *now = g_date_time_new_now_local();
    gchar *date = g_date_time_format(now, "%Y-%m-%d");
    gtk_text_buffer_delete_selection(ed->buffer, TRUE, TRUE);
    gtk_text_buffer_insert_at_cursor(ed->buffer, date, -1);
    g_free(date);
    g_date_time_unref(now);
}

/* on_insert_date_clicked() — Insert menu "Date": see editor_insert_date(). */
static void
on_insert_date_clicked(GtkWidget *item, gpointer user_data)
{
    (void)item;
    OnEditor *ed = user_data;        /* owning editor                       */
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
    editor_insert_date(ed);
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
            ed->tags_modified = TRUE;    /* a tag was created               */
            editor_queue_autosave(ed);
        }
    }
    gtk_text_buffer_delete_mark(ed->buffer, ed->tag_start);
    ed->tag_start = NULL;
    on_db_tag_list_free(ed->tag_choices);
    ed->tag_choices = NULL;
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
    ed->tags_modified = TRUE;        /* a tag was created                   */
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

    /* Fill with case-insensitive prefix matches from the capture-start
     * snapshot (see ed->tag_choices).                                      */
    gint shown = 0;                  /* number of rows added                */
    gchar *prefix_ci = g_utf8_casefold(prefix, -1);
    for (GList *l = ed->tag_choices;
         l != NULL && shown < TAG_POPUP_MAX; l = l->next) {
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
 * undo / redo
 *
 * GTK3's GtkTextBuffer has no undo (that arrived in GTK4), so the editor
 * keeps its own history of whole-buffer SNAPSHOTS: a segment list
 * mirroring the serializer's walk — text runs with ON_FMT_* flags, plus
 * one segment per child anchor (image / checkbox / table).  Images are
 * held by pixbuf REFERENCE, shared across snapshots and with the live
 * buffer, so no PNG bytes are copied and nothing is re-decoded; tables
 * are deep-copied because the anchor's OnTable is mutated in place by
 * its cell entries.  Editor-only tags (emoji padding, search hits) stay
 * out of snapshots — restore re-runs the emoji pass.
 *
 * Grouping: every buffer mutation already funnels through
 * editor_queue_autosave(), which (re)arms a UNDO_GROUP_MS debounce
 * timer — like the autosave debounce, each change RESETS it, so a group
 * commits only after the user pauses for UNDO_GROUP_MS: one undo step
 * per typing burst.  When it fires, the previously committed snapshot
 * is pushed on the undo stack and a fresh capture becomes "current".
 * Ctrl/Cmd+Z flushes any pending group, then swaps the current snapshot
 * for the popped one (old current goes to the redo stack); Ctrl/Cmd+Y
 * or Ctrl/Cmd+Shift+Z mirrors it.  A commit that captures a state
 * identical to the current snapshot (e.g. an autosave queued by a
 * no-op) pushes nothing.  Two caps keep a non-stop burst from becoming
 * one giant step: a typed linebreak commits the group immediately, and
 * so does the UNDO_MAX_SENTENCES-th sentence ender ('.' or '?').
 *
 * Restore is MINIMAL-DIFF: the common prefix and suffix (in characters)
 * between the buffer's present state and the target snapshot stay
 * untouched — only the differing middle is deleted and re-inserted.
 * Undoing a word therefore doesn't churn every image widget in the note
 * or yank the scroll position the way a full clear-and-rebuild did.
 * =========================================================================== */

#define UNDO_GROUP_MS      1000      /* pause that closes an undo group     */
#define UNDO_MAX_GROUPS    100       /* history depth (oldest dropped)      */
#define UNDO_MAX_SENTENCES 5         /* sentence enders per group, tops     */

typedef enum {
    UNDO_SEG_TEXT,                   /* styled text run                     */
    UNDO_SEG_IMAGE,                  /* image anchor                        */
    UNDO_SEG_CHECK,                  /* task-checkbox anchor                */
    UNDO_SEG_TABLE,                  /* table anchor                        */
} UndoSegKind;

/* One buffer segment.  `flags` covers the whole run — for anchors, the
 * anchor's own 0xFFFC character (checkbox anchors carry the
 * on-list-check paragraph tag, and every anchor char sits inside its
 * line's paragraph span).                                                   */
typedef struct {
    UndoSegKind kind;
    guint32     flags;               /* ON_FMT_* bits on the segment        */
    gchar      *text;                /* TEXT: owned UTF-8 run               */
    GdkPixbuf  *pixbuf;              /* IMAGE: owned reference              */
    gint        display_width;       /* IMAGE: chosen on-screen width       */
    gboolean    checked;             /* CHECK: checkbox state               */
    OnTable    *table;               /* TABLE: owned deep copy              */
} UndoSeg;

/* One history entry: the whole buffer plus the cursor position.            */
struct UndoSnap {
    GPtrArray *segs;                 /* UndoSeg*, in buffer order           */
    gint       cursor;               /* insert-mark offset at capture time  */
    gboolean   has_tags;             /* any run carries ON_FMT_TAG (used to
                                      * set tags_modified on restore)       */
};

/* Full flag ⇄ tag map, same set the serializer uses (its copy is static
 * to serialize.c).  INLINE_TOGGLES only covers the four inline styles.     */
static const struct {
    OnFormatFlags flag;              /* format bit                          */
    const gchar  *tag_name;          /* matching GtkTextTag name            */
} UNDO_FLAG_TAGS[] = {
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

/* undo_seg_free() — release one segment and whatever it owns.               */
static void
undo_seg_free(UndoSeg *seg)
{
    g_free(seg->text);
    if (seg->pixbuf != NULL)
        g_object_unref(seg->pixbuf);
    if (seg->table != NULL)
        on_table_free(seg->table);
    g_free(seg);
}

/* undo_snap_free() — release one snapshot and all its segments.             */
static void
undo_snap_free(UndoSnap *snap)
{
    for (guint i = 0; i < snap->segs->len; i++)
        undo_seg_free(g_ptr_array_index(snap->segs, i));
    g_ptr_array_free(snap->segs, TRUE);
    g_free(snap);
}

/* undo_table_copy() — deep copy of a table (cells + header flag).           */
static OnTable *
undo_table_copy(OnTable *src)
{
    OnTable *copy = on_table_new(src->rows, src->cols);
    copy->header = src->header;
    for (gint r = 0; r < src->rows; r++)
        for (gint c = 0; c < src->cols; c++)
            on_table_set(copy, r, c, on_table_get(src, r, c));
    return copy;
}

/* undo_flags_at_iter() — ALL format bits in effect at `iter` (inline +
 * paragraph + #tag), unlike iter_inline_flags() which is inline-only.       */
static guint32
undo_flags_at_iter(GtkTextBuffer *buffer, const GtkTextIter *iter)
{
    guint32 flags = 0;               /* accumulated bits                    */
    for (gsize i = 0; i < G_N_ELEMENTS(UNDO_FLAG_TAGS); i++) {
        GtkTextTag *tag = lookup_tag(buffer, UNDO_FLAG_TAGS[i].tag_name);
        if (tag != NULL && gtk_text_iter_has_tag(iter, tag))
            flags |= UNDO_FLAG_TAGS[i].flag;
    }
    return flags;
}

/* undo_flush_run() — append the pending text run (if any) as a segment.     */
static void
undo_flush_run(UndoSnap *snap, GString *run, guint32 flags)
{
    if (run->len == 0)
        return;
    UndoSeg *seg = g_new0(UndoSeg, 1);
    seg->kind  = UNDO_SEG_TEXT;
    seg->flags = flags;
    seg->text  = g_strndup(run->str, run->len);
    g_ptr_array_add(snap->segs, seg);
    if (flags & ON_FMT_TAG)
        snap->has_tags = TRUE;
    g_string_truncate(run, 0);
}

/* ---------------------------------------------------------------------------
 * undo_snapshot_capture() — walk the buffer into a new snapshot.  Same
 * traversal as on_note_serialize(): anchors interrupt text runs; runs
 * split where the flag set changes; imageless anchors and stray 0xFFFC
 * chars are dropped (as on save).
 * ------------------------------------------------------------------------- */
static UndoSnap *
undo_snapshot_capture(OnEditor *ed)
{
    UndoSnap *snap = g_new0(UndoSnap, 1);
    snap->segs = g_ptr_array_new();

    GtkTextIter ins;                 /* current cursor                      */
    gtk_text_buffer_get_iter_at_mark(ed->buffer, &ins,
                                     gtk_text_buffer_get_insert(ed->buffer));
    snap->cursor = gtk_text_iter_get_offset(&ins);

    GtkTextIter iter;                /* walk position                       */
    gtk_text_buffer_get_start_iter(ed->buffer, &iter);

    GString *run       = g_string_new(NULL); /* pending text run            */
    guint32  run_flags = 0;                  /* its formatting              */

    while (!gtk_text_iter_is_end(&iter)) {
        GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
        if (anchor != NULL) {
            UndoSeg   *seg = NULL;   /* segment for this anchor             */
            gboolean   checked;      /* checkbox state                      */
            gint       dw;           /* image display width                 */
            GdkPixbuf *pixbuf;       /* anchor's image, if any              */
            OnTable   *table;        /* anchor's table, if any              */

            if (on_anchor_is_checkbox(anchor, &checked)) {
                seg = g_new0(UndoSeg, 1);
                seg->kind    = UNDO_SEG_CHECK;
                seg->checked = checked;
            } else if ((table = on_anchor_get_table(anchor)) != NULL) {
                seg = g_new0(UndoSeg, 1);
                seg->kind  = UNDO_SEG_TABLE;
                seg->table = undo_table_copy(table);
            } else if ((pixbuf = on_anchor_get_image(anchor, &dw)) != NULL) {
                seg = g_new0(UndoSeg, 1);
                seg->kind          = UNDO_SEG_IMAGE;
                seg->pixbuf        = g_object_ref(pixbuf);
                seg->display_width = dw;
            }
            if (seg != NULL) {       /* payloadless anchors are dropped     */
                undo_flush_run(snap, run, run_flags);
                seg->flags = undo_flags_at_iter(ed->buffer, &iter);
                g_ptr_array_add(snap->segs, seg);
            }
            gtk_text_iter_forward_char(&iter);
            continue;
        }

        guint32 flags = undo_flags_at_iter(ed->buffer, &iter);
        if (flags != run_flags) {
            undo_flush_run(snap, run, run_flags);
            run_flags = flags;
        }
        gunichar ch = gtk_text_iter_get_char(&iter);
        if (ch != 0xFFFC)
            g_string_append_unichar(run, ch);
        gtk_text_iter_forward_char(&iter);
    }
    undo_flush_run(snap, run, run_flags);
    g_string_free(run, TRUE);
    return snap;
}

/* undo_seg_equal() — content equality of two segments.                      */
static gboolean
undo_seg_equal(const UndoSeg *x, const UndoSeg *y)
{
    if (x->kind != y->kind || x->flags != y->flags)
        return FALSE;
    switch (x->kind) {
    case UNDO_SEG_TEXT:
        return g_strcmp0(x->text, y->text) == 0;
    case UNDO_SEG_IMAGE:
        return x->pixbuf == y->pixbuf &&
               x->display_width == y->display_width;
    case UNDO_SEG_CHECK:
        return x->checked == y->checked;
    case UNDO_SEG_TABLE:
        if (x->table->rows != y->table->rows ||
            x->table->cols != y->table->cols ||
            x->table->header != y->table->header)
            return FALSE;
        for (gint c = 0; c < x->table->rows * x->table->cols; c++)
            if (g_strcmp0(g_ptr_array_index(x->table->cells, c),
                          g_ptr_array_index(y->table->cells, c)) != 0)
                return FALSE;
        return TRUE;
    }
    return FALSE;
}

/* undo_snap_equal() — content equality (cursor position ignored).           */
static gboolean
undo_snap_equal(const UndoSnap *a, const UndoSnap *b)
{
    if (a->segs->len != b->segs->len)
        return FALSE;
    for (guint i = 0; i < a->segs->len; i++)
        if (!undo_seg_equal(g_ptr_array_index(a->segs, i),
                            g_ptr_array_index(b->segs, i)))
            return FALSE;
    return TRUE;
}

/* undo_seg_len() / undo_snap_len() — length in buffer characters (every
 * anchor occupies one 0xFFFC character).                                    */
static gint
undo_seg_len(const UndoSeg *seg)
{
    return seg->kind == UNDO_SEG_TEXT
           ? (gint)g_utf8_strlen(seg->text, -1) : 1;
}

static gint
undo_snap_len(const UndoSnap *snap)
{
    gint len = 0;                    /* accumulated char count              */
    for (guint i = 0; i < snap->segs->len; i++)
        len += undo_seg_len(g_ptr_array_index(snap->segs, i));
    return len;
}

/* ---------------------------------------------------------------------------
 * undo_common_prefix() / undo_common_suffix() — how many leading/trailing
 * characters two snapshots share.  Whole equal segments are consumed in
 * lockstep; at the first differing pair, two text runs with the SAME
 * flags still contribute their common prefix/suffix characters.  The
 * caller must clamp so the two regions never overlap.
 * ------------------------------------------------------------------------- */
static gint
undo_common_prefix(const UndoSnap *a, const UndoSnap *b)
{
    gint  chars = 0;                 /* shared leading characters           */
    guint i     = 0;                 /* segment index in both snapshots     */
    while (i < a->segs->len && i < b->segs->len) {
        const UndoSeg *x = g_ptr_array_index(a->segs, i);
        const UndoSeg *y = g_ptr_array_index(b->segs, i);
        if (undo_seg_equal(x, y)) {
            chars += undo_seg_len(x);
            i++;
            continue;
        }
        if (x->kind == UNDO_SEG_TEXT && y->kind == UNDO_SEG_TEXT &&
            x->flags == y->flags) {
            const gchar *p = x->text, *q = y->text;
            while (*p != '\0' && *q != '\0' &&
                   g_utf8_get_char(p) == g_utf8_get_char(q)) {
                chars++;
                p = g_utf8_next_char(p);
                q = g_utf8_next_char(q);
            }
        }
        break;
    }
    return chars;
}

static gint
undo_common_suffix(const UndoSnap *a, const UndoSnap *b)
{
    gint chars = 0;                  /* shared trailing characters          */
    gint i = (gint)a->segs->len - 1; /* walk both tails leftwards           */
    gint j = (gint)b->segs->len - 1;
    while (i >= 0 && j >= 0) {
        const UndoSeg *x = g_ptr_array_index(a->segs, i);
        const UndoSeg *y = g_ptr_array_index(b->segs, j);
        if (undo_seg_equal(x, y)) {
            chars += undo_seg_len(x);
            i--;
            j--;
            continue;
        }
        if (x->kind == UNDO_SEG_TEXT && y->kind == UNDO_SEG_TEXT &&
            x->flags == y->flags) {
            const gchar *p = x->text + strlen(x->text);
            const gchar *q = y->text + strlen(y->text);
            while (p > x->text && q > y->text) {
                const gchar *pp = g_utf8_prev_char(p);
                const gchar *qq = g_utf8_prev_char(q);
                if (g_utf8_get_char(pp) != g_utf8_get_char(qq))
                    break;
                chars++;
                p = pp;
                q = qq;
            }
        }
        break;
    }
    return chars;
}

/* ---------------------------------------------------------------------------
 * undo_commit_now() — close the in-progress edit group: push the old
 * current snapshot on the undo stack (unless nothing actually changed)
 * and capture the buffer as the new current state.  Any real commit
 * invalidates the redo branch.
 * ------------------------------------------------------------------------- */
static void
undo_commit_now(OnEditor *ed)
{
    ed->undo_sentences = 0;          /* the cap counts per group            */
    if (ed->undo_commit_source != 0) {
        g_source_remove(ed->undo_commit_source);
        ed->undo_commit_source = 0;
    }

    UndoSnap *now = undo_snapshot_capture(ed);
    if (ed->undo_current != NULL && undo_snap_equal(now, ed->undo_current)) {
        undo_snap_free(ed->undo_current);
        ed->undo_current = now;      /* content unchanged: refresh cursor   */
        return;
    }

    if (ed->undo_current != NULL) {
        if (ed->undo_stack->len >= UNDO_MAX_GROUPS) {
            undo_snap_free(g_ptr_array_index(ed->undo_stack, 0));
            g_ptr_array_remove_index(ed->undo_stack, 0);
        }
        g_ptr_array_add(ed->undo_stack, ed->undo_current);
    }
    ed->undo_current = now;

    for (guint i = 0; i < ed->redo_stack->len; i++)
        undo_snap_free(g_ptr_array_index(ed->redo_stack, i));
    g_ptr_array_set_size(ed->redo_stack, 0);
}

/* on_undo_commit_timeout() — the group timer fired: commit.                 */
static gboolean
on_undo_commit_timeout(gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    ed->undo_commit_source = 0;
    undo_commit_now(ed);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * undo_notify_change() — called (via editor_queue_autosave) on every
 * buffer mutation: (re)arm the group-commit debounce.  Each change
 * resets the timer, so the group closes UNDO_GROUP_MS after the user
 * STOPS editing — one undo step per typing burst.
 * ------------------------------------------------------------------------- */
static void
undo_notify_change(OnEditor *ed)
{
    if (ed->undo_restoring || ed->undo_stack == NULL)
        return;
    if (ed->undo_commit_source != 0)
        g_source_remove(ed->undo_commit_source);
    ed->undo_commit_source = g_timeout_add(UNDO_GROUP_MS,
                                           on_undo_commit_timeout, ed);
}

/* ---------------------------------------------------------------------------
 * undo_insert_seg_slice() — insert characters [s, s+n) of one segment at
 * buffer offset `at`: text slices by UTF-8 offset, anchors whole (their
 * slice is always the single 0xFFFC char).  The fresh span first has
 * every Blue Notes tag stripped — GtkTextBuffer makes insertions inherit
 * tags applied on BOTH sides of the insertion point — then the segment's
 * own flags applied.  Returns n.
 * ------------------------------------------------------------------------- */
static gint
undo_insert_seg_slice(OnEditor *ed, const UndoSeg *seg, gint s, gint n,
                      gint at)
{
    GtkTextIter it;                  /* insertion point                     */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &it, at);

    if (seg->kind == UNDO_SEG_TEXT) {
        const gchar *ps = g_utf8_offset_to_pointer(seg->text, s);
        const gchar *pe = g_utf8_offset_to_pointer(ps, n);
        gtk_text_buffer_insert(ed->buffer, &it, ps, (gint)(pe - ps));
    } else {
        GtkTextChildAnchor *anchor =
            gtk_text_buffer_create_child_anchor(ed->buffer, &it);
        switch (seg->kind) {
        case UNDO_SEG_IMAGE:
            on_anchor_set_image(anchor, seg->pixbuf, seg->display_width);
            attach_image_widget(ed, anchor);
            break;
        case UNDO_SEG_CHECK:
            on_anchor_set_checkbox(anchor, seg->checked);
            attach_checkbox_widget(ed, anchor);
            break;
        case UNDO_SEG_TABLE:
            on_anchor_set_table(anchor, undo_table_copy(seg->table));
            attach_table_widget(ed, anchor);
            break;
        default:
            break;
        }
    }

    GtkTextIter ss, se;              /* the span just inserted              */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &ss, at);
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &se, at + n);
    for (gsize t = 0; t < G_N_ELEMENTS(UNDO_FLAG_TAGS); t++) {
        if (seg->flags & UNDO_FLAG_TAGS[t].flag)
            gtk_text_buffer_apply_tag_by_name(
                ed->buffer, UNDO_FLAG_TAGS[t].tag_name, &ss, &se);
        else
            gtk_text_buffer_remove_tag_by_name(
                ed->buffer, UNDO_FLAG_TAGS[t].tag_name, &ss, &se);
    }
    gtk_text_buffer_remove_tag_by_name(ed->buffer, "on-search-hit",
                                       &ss, &se);
    return n;
}

/* ---------------------------------------------------------------------------
 * undo_restore() — take the buffer from state `from` (its present
 * content) to state `to`, touching as little as possible: the common
 * prefix and suffix stay in place, only the differing middle is deleted
 * and re-inserted (recreating any anchors + widgets inside it).  Then
 * re-run the emoji padding over the seam, put the cursor back, and
 * re-adopt the inline style there.  Runs with history recording
 * suppressed; the change still autosaves.
 * ------------------------------------------------------------------------- */
static void
undo_restore(OnEditor *ed, const UndoSnap *from, const UndoSnap *to)
{
    if (ed->tag_start != NULL)       /* abandon a half-typed #tag           */
        tag_capture_end(ed, FALSE);

    ed->undo_restoring = TRUE;
    ed->internal_change++;

    gint buf_len = gtk_text_buffer_get_char_count(ed->buffer);
    gint len_to  = undo_snap_len(to);

    gint prefix = undo_common_prefix(from, to);
    gint suffix = undo_common_suffix(from, to);
    /* The two regions must never overlap ("aaaa" → "aaa" counts 3+3).      */
    gint max_common = MIN(undo_snap_len(from), len_to) - prefix;
    suffix = CLAMP(suffix, 0, max_common);
    /* Belt and braces vs. the live buffer (from should always match it).   */
    prefix = MIN(prefix, buf_len);
    suffix = MIN(suffix, buf_len - prefix);

    /* Replace buffer [prefix, buf_len - suffix)
     * with `to`'s   [prefix, len_to - suffix).                             */
    GtkTextIter ds, de;              /* doomed middle range                 */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &ds, prefix);
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &de, buf_len - suffix);
    if (!gtk_text_iter_equal(&ds, &de))
        gtk_text_buffer_delete(ed->buffer, &ds, &de);

    gint at       = prefix;          /* insertion offset                    */
    gint want_end = len_to - suffix; /* target chars [prefix, want_end)     */
    gint pos      = 0;               /* char offset within `to`             */
    for (guint i = 0; i < to->segs->len && pos < want_end; i++) {
        const UndoSeg *seg = g_ptr_array_index(to->segs, i);
        gint slen = undo_seg_len(seg);
        gint s    = MAX(prefix - pos, 0);    /* slice within this segment   */
        gint e    = MIN(want_end - pos, slen);
        if (s < e)
            at += undo_insert_seg_slice(ed, seg, s, e - s, at);
        pos += slen;
    }

    /* Editor-only emoji padding is never part of snapshots — refresh it
     * over the replaced span plus one char each side (the pad tag also
     * covers the char after an emoji).                                     */
    gint emo_s = MAX(prefix - 1, 0);
    gint emo_e = MIN(at + 1, gtk_text_buffer_get_char_count(ed->buffer));
    if (emo_s < emo_e) {
        GtkTextIter es, ee;          /* emoji-refresh span                  */
        gtk_text_buffer_get_iter_at_offset(ed->buffer, &es, emo_s);
        gtk_text_buffer_get_iter_at_offset(ed->buffer, &ee, emo_e);
        gtk_text_buffer_remove_tag_by_name(ed->buffer, "on-emoji",
                                           &es, &ee);
        tag_emoji_in_range(ed, emo_s, emo_e);
    }

    GtkTextIter cur;                 /* restored cursor position            */
    gtk_text_buffer_get_iter_at_offset(ed->buffer, &cur, to->cursor);
    gtk_text_buffer_place_cursor(ed->buffer, &cur);

    ed->internal_change--;

    /* Re-adopt the inline style at the restored cursor (the guarded
     * cursor-position notify never saw the move).                          */
    ed->inline_flags = gtk_text_iter_backward_char(&cur)
                       ? iter_inline_flags(ed->buffer, &cur) : 0;
    update_toggle_buttons(ed);

    editor_queue_autosave(ed);       /* undo_restoring suppresses grouping  */
    code_buttons_queue_rebuild(ed);
    ed->undo_restoring = FALSE;

    gtk_text_view_scroll_to_mark(ed->view,
                                 gtk_text_buffer_get_insert(ed->buffer),
                                 0.1, FALSE, 0.0, 0.0);
}

/* ---------------------------------------------------------------------------
 * editor_undo() / editor_redo() — Ctrl/Cmd+Z and Ctrl/Cmd+Y.  Undo first
 * flushes the in-progress group so the very latest edits are what gets
 * undone.  tags_modified goes up whenever either side of the swap
 * contains #tag spans — the restore may change the note's tag set.
 * ------------------------------------------------------------------------- */
static void
editor_undo(OnEditor *ed)
{
    if (ed->undo_commit_source != 0)
        undo_commit_now(ed);
    if (ed->undo_stack->len == 0)
        return;

    UndoSnap *snap =                 /* state we are going back to          */
        g_ptr_array_index(ed->undo_stack, ed->undo_stack->len - 1);
    g_ptr_array_remove_index(ed->undo_stack, ed->undo_stack->len - 1);

    UndoSnap *from = ed->undo_current;   /* the buffer's present state      */
    if (snap->has_tags || from->has_tags)
        ed->tags_modified = TRUE;
    g_ptr_array_add(ed->redo_stack, from);
    ed->undo_current = snap;
    undo_restore(ed, from, snap);
}

static void
editor_redo(OnEditor *ed)
{
    if (ed->undo_commit_source != 0)
        undo_commit_now(ed);         /* may clear the redo stack            */
    if (ed->redo_stack->len == 0)
        return;

    UndoSnap *snap =                 /* state we are going forward to       */
        g_ptr_array_index(ed->redo_stack, ed->redo_stack->len - 1);
    g_ptr_array_remove_index(ed->redo_stack, ed->redo_stack->len - 1);

    UndoSnap *from = ed->undo_current;   /* the buffer's present state      */
    if (snap->has_tags || from->has_tags)
        ed->tags_modified = TRUE;
    g_ptr_array_add(ed->undo_stack, from);
    ed->undo_current = snap;
    undo_restore(ed, from, snap);
}

/* undo_free_history() — release both stacks and the current snapshot.       */
static void
undo_free_history(OnEditor *ed)
{
    if (ed->undo_stack != NULL) {
        for (guint i = 0; i < ed->undo_stack->len; i++)
            undo_snap_free(g_ptr_array_index(ed->undo_stack, i));
        g_ptr_array_free(ed->undo_stack, TRUE);
        ed->undo_stack = NULL;
    }
    if (ed->redo_stack != NULL) {
        for (guint i = 0; i < ed->redo_stack->len; i++)
            undo_snap_free(g_ptr_array_index(ed->redo_stack, i));
        g_ptr_array_free(ed->redo_stack, TRUE);
        ed->redo_stack = NULL;
    }
    g_clear_pointer(&ed->undo_current, undo_snap_free);
}

/* ===========================================================================
 * buffer signal handlers
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * on_buffer_insert_text_before() — runs BEFORE an insertion is carried
 * out.  Marks short (typed) insertions so on_cursor_moved() can tell the
 * resulting cursor jump apart from real navigation: GtkTextBuffer emits
 * notify::cursor-position INSIDE its insert-text class handler — before
 * on_buffer_insert_text_after() has applied ed->inline_flags to the new
 * text — so adopting the new character's (still untagged) style there
 * would clear a style armed via Ctrl/Cmd+B with no selection.  The same
 * ≤2-char threshold as the after-handler's enforcement keeps pastes on
 * the old adopt-from-buffer path.
 * ------------------------------------------------------------------------- */
static void
on_buffer_insert_text_before(GtkTextBuffer *buffer, GtkTextIter *location,
                             gchar *text, gint len, gpointer user_data)
{
    (void)buffer; (void)location;
    OnEditor *ed = user_data;        /* owning editor                       */
    if (ed->internal_change > 0)
        return;
    if (g_utf8_strlen(text, len) <= 2)
        ed->typing_insert = TRUE;
}

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
    ed->typing_insert = FALSE;       /* set by the before-handler; the
                                      * insertion is fully processed once
                                      * this handler runs                   */
    if (ed->internal_change > 0)
        return;

    glong n_chars = g_utf8_strlen(text, len);
    gint  end_off = gtk_text_iter_get_offset(location);

    /* Pad any emoji in the insertion so they don't overlap neighbours.     */
    tag_emoji_in_range(ed, end_off - (gint)n_chars, end_off);
    gtk_text_buffer_get_iter_at_offset(ed->buffer, location, end_off);

    /* Auto-H1: while the first line of a brand-new note is being typed,
     * keep it styled as a heading.  The tag can't be pre-applied to an
     * empty line (there is nothing to tag yet), so it is re-applied per
     * insertion; the first Enter ends the title line — the newline stays
     * untagged, so the next line starts as plain body text.                */
    if (ed->auto_h1) {
        if (gtk_text_buffer_get_line_count(buffer) > 1) {
            ed->auto_h1 = FALSE;     /* title line finished                 */
        } else {
            GtkTextIter ls, le;      /* whole first line                    */
            line_span(buffer, 0, &ls, &le);
            if (!gtk_text_iter_equal(&ls, &le)) {
                ed->internal_change++;
                gtk_text_buffer_apply_tag_by_name(buffer, ON_TAGNAME_H1,
                                                  &ls, &le);
                ed->internal_change--;
            }
        }
    }

    /* Typing INSIDE an existing styled #tag renames it — flag the tag set
     * as changed so the next save updates note_tags and the sidebar.       */
    if (!ed->tags_modified) {
        GtkTextIter ins_s;           /* first inserted character            */
        gtk_text_buffer_get_iter_at_offset(buffer, &ins_s,
                                           end_off - (gint)n_chars);
        GtkTextTag *ttag = gtk_text_tag_table_lookup(
            gtk_text_buffer_get_tag_table(buffer), ON_TAGNAME_TAG);
        if (ttag != NULL && gtk_text_iter_has_tag(&ins_s, ttag))
            ed->tags_modified = TRUE;
    }

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

    /* --- undo group caps: a linebreak closes the pending group at once,
     * and so does the UNDO_MAX_SENTENCES-th sentence ender, so one long
     * unbroken burst can't become a single giant undo step ---------------- */
    {
        gboolean cap_hit = FALSE;    /* commit the group after this char?   */
        for (const gchar *p = text; p < text + len;
             p = g_utf8_next_char(p)) {
            gunichar c = g_utf8_get_char(p);
            if (c == '\n') {
                cap_hit = TRUE;
                break;
            }
            if ((c == '.' || c == '?') &&
                ++ed->undo_sentences >= UNDO_MAX_SENTENCES) {
                cap_hit = TRUE;
                break;
            }
        }
        if (cap_hit)
            undo_commit_now(ed);     /* also resets the sentence counter    */
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
                /* One query per capture; every keystroke inside the
                 * capture filters this snapshot in memory instead of
                 * re-querying SQLite (a round trip on network DBs).       */
                ed->tag_choices = on_db_tag_list(ed->app->db);
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
 * on_buffer_delete_range_before() — runs BEFORE a deletion is carried out
 * (afterwards the removed text is unknowable): if the doomed range
 * touches a styled #tag, a tag is being deleted or renamed, so flag the
 * tag set as changed for the next save.
 * ------------------------------------------------------------------------- */
static void
on_buffer_delete_range_before(GtkTextBuffer *buffer, GtkTextIter *start,
                              GtkTextIter *end, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    if (ed->internal_change > 0 || ed->tags_modified)
        return;

    GtkTextTag *tag = gtk_text_tag_table_lookup(
        gtk_text_buffer_get_tag_table(buffer), ON_TAGNAME_TAG);
    if (tag == NULL)
        return;

    if (gtk_text_iter_has_tag(start, tag) ||
        gtk_text_iter_has_tag(end, tag)) {
        ed->tags_modified = TRUE;    /* range starts or ends inside a tag   */
        return;
    }
    /* Any tag toggle strictly inside the range?                            */
    GtkTextIter it = *start;
    if (gtk_text_iter_forward_to_tag_toggle(&it, tag) &&
        gtk_text_iter_compare(&it, end) < 0)
        ed->tags_modified = TRUE;
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

/* on_scroll_idle() — deferred caret-follow scroll (see on_buffer_changed). */
static gboolean
on_scroll_idle(gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    ed->scroll_idle = 0;
    gtk_text_view_scroll_to_mark(
        ed->view, gtk_text_buffer_get_insert(ed->buffer),
        0.08, FALSE, 0.0, 0.0);
    return G_SOURCE_REMOVE;
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

    /* Keep the caret comfortably above the window's bottom edge while
     * typing: within_margin makes the view scroll a little AHEAD of the
     * cursor instead of letting it ride the very last pixel row.  The
     * scroll is deferred to an idle — at "changed" time the text layout
     * has not revalidated yet, so an immediate scroll computes against
     * stale extents and does nothing at the end of the document.           */
    if (ed->internal_change == 0 && ed->scroll_idle == 0)
        ed->scroll_idle = g_idle_add(on_scroll_idle, ed);
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

    /* Adopt the style of the character to the left of the cursor — but
     * NOT when the move comes from typing: this notify fires inside the
     * insert-text class handler, before the after-handler has applied
     * ed->inline_flags to the new character, so probing it here would
     * clear a style armed with no selection (Ctrl/Cmd+B, then type).      */
    if (ed->typing_insert)
        return;
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
        if (handle_return_in_list(ed)) {
            undo_commit_now(ed);     /* linebreak: close the undo group
                                        (list Enter inserts internally, so
                                        the insert-text cap never sees it)  */
            return TRUE;
        }
    }

    /* Ctrl (or Cmd on macOS) + B/I/U inline-style shortcuts, and Ctrl+F
     * to jump into the in-note search box.                                 */
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
        case GDK_KEY_z:
            if (event->state & GDK_SHIFT_MASK)
                editor_redo(ed);
            else
                editor_undo(ed);
            return TRUE;
        case GDK_KEY_y:
            editor_redo(ed);
            return TRUE;
        case GDK_KEY_d:
            editor_insert_date(ed);
            return TRUE;
        case GDK_KEY_e:
            g_signal_emit_by_name(ed->view, "insert-emoji");
            return TRUE;
        case GDK_KEY_f:
            gtk_widget_grab_focus(ed->search_entry);
            return TRUE;
        default:
            break;
        }
    }
    return FALSE;
}

/* ===========================================================================
 * in-note search
 *
 * The toolbar's right-edge entry highlights every case-insensitive match
 * with the "on-search-hit" tag as you type; Enter (or the entry icon)
 * jumps to the next match, wrapping at the end.  Ctrl/Cmd+F focuses the
 * entry; Escape returns focus to the text.
 * =========================================================================== */

/* editor_search_clear() — drop all match highlighting.                      */
static void
editor_search_clear(OnEditor *ed)
{
    GtkTextIter s, e;                /* whole-buffer bounds                 */
    gtk_text_buffer_get_bounds(ed->buffer, &s, &e);
    gtk_text_buffer_remove_tag_by_name(ed->buffer, "on-search-hit",
                                       &s, &e);
}

/* ---------------------------------------------------------------------------
 * on_search_changed() — re-highlight every match of the current query.
 * ------------------------------------------------------------------------- */
static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    editor_search_clear(ed);

    const gchar *query = gtk_entry_get_text(GTK_ENTRY(entry));
    if (query == NULL || *query == '\0')
        return;

    GtkTextIter it;                  /* scan position                       */
    gtk_text_buffer_get_start_iter(ed->buffer, &it);
    GtkTextIter match_s, match_e;    /* bounds of one match                 */
    while (gtk_text_iter_forward_search(&it, query,
                                        GTK_TEXT_SEARCH_CASE_INSENSITIVE |
                                        GTK_TEXT_SEARCH_TEXT_ONLY,
                                        &match_s, &match_e, NULL)) {
        gtk_text_buffer_apply_tag_by_name(ed->buffer, "on-search-hit",
                                          &match_s, &match_e);
        it = match_e;
    }
}

/* ---------------------------------------------------------------------------
 * editor_search_next() — select and scroll to the next match after the
 * cursor, wrapping to the top when none remains below.
 * ------------------------------------------------------------------------- */
static void
editor_search_next(OnEditor *ed)
{
    const gchar *query =
        gtk_entry_get_text(GTK_ENTRY(ed->search_entry));
    if (query == NULL || *query == '\0')
        return;

    GtkTextIter from;                /* search start (after the cursor)     */
    gtk_text_buffer_get_iter_at_mark(
        ed->buffer, &from,
        gtk_text_buffer_get_selection_bound(ed->buffer));

    GtkTextIter match_s, match_e;    /* bounds of the found match           */
    gboolean found = gtk_text_iter_forward_search(
        &from, query,
        GTK_TEXT_SEARCH_CASE_INSENSITIVE | GTK_TEXT_SEARCH_TEXT_ONLY,
        &match_s, &match_e, NULL);
    if (!found) {                    /* wrap around to the top              */
        gtk_text_buffer_get_start_iter(ed->buffer, &from);
        found = gtk_text_iter_forward_search(
            &from, query,
            GTK_TEXT_SEARCH_CASE_INSENSITIVE | GTK_TEXT_SEARCH_TEXT_ONLY,
            &match_s, &match_e, NULL);
    }
    if (!found)
        return;

    gtk_text_buffer_select_range(ed->buffer, &match_s, &match_e);
    gtk_text_view_scroll_to_iter(ed->view, &match_s, 0.1,
                                 FALSE, 0.0, 0.0);
}

/* ---------------------------------------------------------------------------
 * editor_search_prev() — select and scroll to the previous match before
 * the cursor, wrapping to the bottom when none remains above.
 * ------------------------------------------------------------------------- */
static void
editor_search_prev(OnEditor *ed)
{
    const gchar *query =
        gtk_entry_get_text(GTK_ENTRY(ed->search_entry));
    if (query == NULL || *query == '\0')
        return;

    GtkTextIter from;                /* search start (before the cursor)    */
    gtk_text_buffer_get_iter_at_mark(
        ed->buffer, &from, gtk_text_buffer_get_insert(ed->buffer));

    GtkTextIter match_s, match_e;    /* bounds of the found match           */
    gboolean found = gtk_text_iter_backward_search(
        &from, query,
        GTK_TEXT_SEARCH_CASE_INSENSITIVE | GTK_TEXT_SEARCH_TEXT_ONLY,
        &match_s, &match_e, NULL);
    if (!found) {                    /* wrap around to the bottom           */
        gtk_text_buffer_get_end_iter(ed->buffer, &from);
        found = gtk_text_iter_backward_search(
            &from, query,
            GTK_TEXT_SEARCH_CASE_INSENSITIVE | GTK_TEXT_SEARCH_TEXT_ONLY,
            &match_s, &match_e, NULL);
    }
    if (!found)
        return;

    gtk_text_buffer_select_range(ed->buffer, &match_s, &match_e);
    gtk_text_view_scroll_to_iter(ed->view, &match_s, 0.1,
                                 FALSE, 0.0, 0.0);
}

/* on_search_activate() — Enter in the entry: jump to the next match.        */
static void
on_search_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    editor_search_next((OnEditor *)user_data);
}

/* on_search_next_clicked()/on_search_prev_clicked() — the arrow buttons.    */
static void
on_search_next_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    editor_search_next((OnEditor *)user_data);
}

static void
on_search_prev_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    editor_search_prev((OnEditor *)user_data);
}

/* on_search_stop() — Escape in the entry: back to the text view.            */
static void
on_search_stop(GtkSearchEntry *entry, gpointer user_data)
{
    (void)entry;
    OnEditor *ed = user_data;        /* owning editor                       */
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
}

/* ---------------------------------------------------------------------------
 * editor_apply_search_term() — put `term` in the in-note search box,
 * highlight every match and jump to the first.  Used when a note is opened
 * from the library search window so the searched-for text is highlighted at
 * once.  on_search_changed is called explicitly rather than waiting for the
 * GtkSearchEntry's debounced "search-changed".  NULL/empty is a no-op.
 * ------------------------------------------------------------------------- */
static void
editor_apply_search_term(OnEditor *ed, const gchar *term)
{
    if (term == NULL || *term == '\0')
        return;
    gtk_entry_set_text(GTK_ENTRY(ed->search_entry), term);
    on_search_changed(GTK_SEARCH_ENTRY(ed->search_entry), ed);
    editor_search_next(ed);
}

/* on_initial_search_idle() — apply ed->pending_search once the freshly
 * opened window is realized and allocated (so editor_search_next's
 * scroll-to-match lands, mirroring on_scroll_idle's deferral).               */
static gboolean
on_initial_search_idle(gpointer user_data)
{
    OnEditor *ed = user_data;        /* owning editor                       */
    ed->initial_search_idle = 0;
    editor_apply_search_term(ed, ed->pending_search);
    g_clear_pointer(&ed->pending_search, g_free);
    return G_SOURCE_REMOVE;
}

/* ===========================================================================
 * saving
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * editor_save() — serialize the buffer and persist it: content blob,
 * derived title, and — only when the live tags_modified flag says a #tag
 * was created, renamed or deleted — the note's tag set.  Then pokes the
 * library: the light notes-pane refresh normally (editing a note can
 * never change folder counts), the full one (sidebar included) only for
 * tag changes.
 * ------------------------------------------------------------------------- */
static void
editor_save(OnEditor *ed)
{
    gsize   blob_len = 0;            /* BNBF blob size                      */
    guint8 *blob = on_note_serialize(ed->buffer, &blob_len);
    gchar  *title = on_buffer_first_line(ed->buffer);
    gchar  *body = on_note_extract_text(blob, blob_len);

    on_db_note_save(ed->app->db, ed->note_id, title, blob, blob_len, body);
    g_free(body);

    /* Rewrite note_tags only when the tag set actually changed — flagged
     * live while editing, so the common save does no tag work at all.      */
    gboolean tags_changed = ed->tags_modified;
    if (tags_changed) {
        GList *tags = on_buffer_collect_tags(ed->buffer);
        on_db_note_set_tags(ed->app->db, ed->note_id, tags);
        g_list_free_full(tags, g_free);
        ed->tags_modified = FALSE;
    }
    ed->dirty = FALSE;

    /* Window title mirrors the note title.                                 */
    if (ed->window != NULL) {
        gchar *wtitle = g_strdup_printf("Blue Notes - %s", title);
        gtk_window_set_title(GTK_WINDOW(ed->window), wtitle);
        g_free(wtitle);
    }

    g_free(title);
    g_free(blob);

    if (tags_changed) {
        if (ed->app->notify_notes_changed != NULL)
            ed->app->notify_notes_changed(ed->app);
    } else {
        if (ed->app->notify_note_saved != NULL)
            ed->app->notify_note_saved(ed->app);
    }
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
    ed->dirty = TRUE;                /* there is now something to save      */
    undo_notify_change(ed);          /* every mutation passes through here,
                                        so this is also where undo groups
                                        start (no-op while restoring)       */
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
    if (ed->scroll_idle != 0) {
        g_source_remove(ed->scroll_idle);
        ed->scroll_idle = 0;
    }
    if (ed->initial_search_idle != 0) {
        g_source_remove(ed->initial_search_idle);
        ed->initial_search_idle = 0;
    }
    if (ed->undo_commit_source != 0) {
        g_source_remove(ed->undo_commit_source);
        ed->undo_commit_source = 0;
    }
    undo_free_history(ed);
    g_clear_pointer(&ed->pending_search, g_free);
    g_slist_free(ed->code_buttons);  /* widgets die with the window         */
    ed->code_buttons = NULL;

    ed->window = NULL;               /* don't touch the dying window        */
    if (ed->dirty)
        editor_save(ed);             /* flush edits the autosave missed
                                        (buffer kept alive by our ref);
                                        a clean note closes instantly —
                                        no serialize, no PNG encoding      */

    if (ed->tag_popup != NULL)
        gtk_widget_destroy(ed->tag_popup);
    on_db_tag_list_free(ed->tag_choices);   /* window closed mid-capture   */
    ed->tag_choices = NULL;

    g_hash_table_remove(ed->app->editors, &ed->note_id);
    g_object_unref(ed->buffer);
    g_free(ed);
}

/* ===========================================================================
 * status bar
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * editor_status_path_update() — put the note's folder path in the status
 * bar's left label, same format as the library window's: "/" for the top
 * level, "/Folder/Sub" otherwise.
 * ------------------------------------------------------------------------- */
static void
editor_status_path_update(OnEditor *ed)
{
    if (ed->status_path == NULL)
        return;

    OnNoteMeta *meta = on_db_note_get(ed->app->db, ed->note_id);
    if (meta == NULL)
        return;

    gchar *path = on_db_folder_path(ed->app->db, meta->folder_id);
    gchar *text = g_strdup_printf("/%s", path);
    gtk_label_set_text(GTK_LABEL(ed->status_path), text);
    g_free(text);
    g_free(path);
    on_db_note_meta_free(meta);
}

/* on_editor_focus_in() — the window regained focus: re-read the note's
 * location, so a move or folder rename made in the library while this
 * editor sat in the background shows up on return.                          */
static gboolean
on_editor_focus_in(GtkWidget *widget, GdkEventFocus *event,
                   gpointer user_data)
{
    (void)widget; (void)event;
    editor_status_path_update(user_data);
    return FALSE;                    /* let GTK handle the focus change     */
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
 * add_para_menu_item() — helper: the compact-toolbar counterpart of
 * add_para_button — one paragraph style as a menu item.  on_para_button
 * only reads the "on-flag" object data from whatever widget fired it, so
 * it serves menu items and tool buttons alike.
 * ------------------------------------------------------------------------- */
static void
add_para_menu_item(OnEditor *ed, GtkWidget *menu, const gchar *label,
                   guint32 flag)
{
    GtkWidget *mi = gtk_menu_item_new_with_mnemonic(label);
    g_object_set_data(G_OBJECT(mi), "on-flag", GUINT_TO_POINTER(flag));
    g_signal_connect(mi, "activate", G_CALLBACK(on_para_button), ed);
    gtk_widget_show(mi);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

/* ---------------------------------------------------------------------------
 * menu_tool_button_new() — helper: a glyph-labelled GtkMenuButton wrapped
 * in a GtkToolItem.  `markup` is Pango markup rendered as the button face
 * so the compact menu buttons match the letter-glyph tool buttons.  A
 * GtkMenuButton + GtkMenu is used instead of a GtkComboBox: the combo's
 * popup grab is unreliable inside a toolbar (it could close the moment
 * the pointer moved); a real menu holds its grab.
 * ------------------------------------------------------------------------- */
static GtkToolItem *
menu_tool_button_new(const gchar *markup, const gchar *tooltip,
                     GtkWidget *menu)
{
    GtkWidget *btn = gtk_menu_button_new();
    gtk_button_set_label(GTK_BUTTON(btn), markup);
    GtkWidget *face = gtk_bin_get_child(GTK_BIN(btn));
    if (GTK_IS_LABEL(face))          /* set_label's child IS the label      */
        gtk_label_set_use_markup(GTK_LABEL(face), TRUE);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(btn), menu);
    gtk_widget_set_tooltip_text(btn, tooltip);

    GtkToolItem *item = gtk_tool_item_new();
    gtk_container_add(GTK_CONTAINER(item), btn);
    return item;
}

/* ---------------------------------------------------------------------------
 * build_toolbar() — construct the formatting toolbar: inline-style
 * toggles, paragraph-style buttons, code-block and image insertion.  The
 * toolbar is registered with the app so it follows the global
 * text/icons/both style preference.  In compact mode (File → Settings…)
 * the three paragraph-style buttons collapse into an "Aa" Styles menu
 * button and the three list buttons into a "≡" Lists one.
 * Returns the toolbar widget.
 * ------------------------------------------------------------------------- */
static GtkWidget *
build_toolbar(OnEditor *ed)
{
    GtkWidget *toolbar = gtk_toolbar_new();
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar),
                              GTK_ICON_SIZE_SMALL_TOOLBAR);
    /* No overflow arrow: the toolbar then demands its full natural width,
     * which becomes the window's minimum — items can never be silently
     * clipped by narrowing the window.                                     */
    gtk_toolbar_set_show_arrow(GTK_TOOLBAR(toolbar), FALSE);

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

    add_para_button(ed, toolbar, "code-block", "{\xc2\xa0}", "Code",
                    "Code block (monospace)", ON_FMT_CODEBLOCK);

    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    /* Paragraph styles.  These have no standard icons, so their "icons"
     * are text glyphs (still swappable by dropping a matching PNG — e.g.
     * heading-1.png — into the icons/ folder).                             */
    if (ed->app->compact_editor_toolbar) {
        GtkWidget *styles_menu = gtk_menu_new();
        add_para_menu_item(ed, styles_menu, "Heading _1", ON_FMT_H1);
        add_para_menu_item(ed, styles_menu, "Heading _2", ON_FMT_H2);
        add_para_menu_item(ed, styles_menu, "_Body",      0);
        gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                           menu_tool_button_new("<b>A</b>a",
                               "Styles \xe2\x80\x94 paragraph style: "
                               "heading or body text",
                               styles_menu), -1);
    } else {
        add_para_button(ed, toolbar, "heading-1", "<b>H1</b>", "Heading 1",
                        "Heading 1", ON_FMT_H1);
        add_para_button(ed, toolbar, "heading-2", "<b>H2</b>", "Heading 2",
                        "Heading 2", ON_FMT_H2);
        add_para_button(ed, toolbar, "body-text", "\xc2\xb6", "Body",
                        "Plain body text", 0);
    }

    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    if (ed->app->compact_editor_toolbar) {
        GtkWidget *lists_menu = gtk_menu_new();
        add_para_menu_item(ed, lists_menu, "_Bulleted List",
                           ON_FMT_LIST_BULLET);
        add_para_menu_item(ed, lists_menu, "_Numbered List",
                           ON_FMT_LIST_NUMBER);
        add_para_menu_item(ed, lists_menu, "_Task List",
                           ON_FMT_LIST_CHECK);
        gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                           menu_tool_button_new("\xe2\x89\xa1",
                               "Lists \xe2\x80\x94 bullets, numbers, or "
                               "task checkboxes", lists_menu), -1);
    } else {
        add_para_button(ed, toolbar, "list-bullet", "\xe2\x80\xa2",
                        "Bullets", "Bulleted list", ON_FMT_LIST_BULLET);
        add_para_button(ed, toolbar, "list-number", "1.", "Numbered",
                        "Numbered list", ON_FMT_LIST_NUMBER);
        /* Fallback glyph is a plain text square (U+25A1 □), not the ⬜
         * color emoji: it renders in the text font like the •/1. glyphs
         * and avoids the emoji's oversized advance in the toolbar.        */
        add_para_button(ed, toolbar, "list-check", "\xe2\x96\xa1", "Tasks",
                        "Task list with checkboxes (click a box to toggle)",
                        ON_FMT_LIST_CHECK);
    }

    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    /* One "Insert ▾" dropdown replaces the Image/Table/Emoji buttons.      */
    GtkWidget *insert_menu = gtk_menu_new();
    static const struct {
        const gchar *label;          /* menu-item text                      */
        GCallback    cb;             /* existing insert handler             */
    } INSERTS[] = {
        { "_Image\xe2\x80\xa6", G_CALLBACK(on_insert_image_clicked) },
        { "_Table",             G_CALLBACK(on_insert_table_clicked) },
        { "_Emoji\xe2\x80\xa6 (Ctrl+E)", G_CALLBACK(on_emoji_clicked) },
        { "_Date (Ctrl+D)",              G_CALLBACK(on_insert_date_clicked) },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(INSERTS); i++) {
        GtkWidget *mi = gtk_menu_item_new_with_mnemonic(INSERTS[i].label);
        g_signal_connect(mi, "activate", INSERTS[i].cb, ed);
        gtk_widget_show(mi);
        gtk_menu_shell_append(GTK_MENU_SHELL(insert_menu), mi);
    }

    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       menu_tool_button_new("+",
                           "Insert an image, a table, an emoji, or "
                           "today's date at the cursor", insert_menu), -1);

    /* In-note search, pinned to the toolbar's right edge (Ctrl+F) by an
     * expanding blank spacer.                                              */
    GtkToolItem *spacer = gtk_separator_tool_item_new();
    gtk_separator_tool_item_set_draw(GTK_SEPARATOR_TOOL_ITEM(spacer),
                                     FALSE);
    gtk_tool_item_set_expand(spacer, TRUE);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), spacer, -1);

    ed->search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ed->search_entry),
                                   "Find in note (Ctrl+F)");
    gtk_entry_set_width_chars(GTK_ENTRY(ed->search_entry), 18);
    g_signal_connect(ed->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), ed);
    g_signal_connect(ed->search_entry, "activate",
                     G_CALLBACK(on_search_activate), ed);
    g_signal_connect(ed->search_entry, "stop-search",
                     G_CALLBACK(on_search_stop), ed);

    /* Entry + previous/next match buttons as one toolbar item.             */
    GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_pack_start(GTK_BOX(search_box), ed->search_entry,
                       TRUE, TRUE, 0);

    GtkWidget *prev_btn = gtk_button_new_from_icon_name(
        "go-up-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(prev_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(prev_btn, "Previous match");
    g_signal_connect(prev_btn, "clicked",
                     G_CALLBACK(on_search_prev_clicked), ed);
    gtk_box_pack_start(GTK_BOX(search_box), prev_btn, FALSE, FALSE, 0);

    GtkWidget *next_btn = gtk_button_new_from_icon_name(
        "go-down-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(next_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(next_btn, "Next match (Enter)");
    g_signal_connect(next_btn, "clicked",
                     G_CALLBACK(on_search_next_clicked), ed);
    gtk_box_pack_start(GTK_BOX(search_box), next_btn, FALSE, FALSE, 0);

    GtkToolItem *search_item = gtk_tool_item_new();
    gtk_container_add(GTK_CONTAINER(search_item), search_box);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), search_item, -1);

    on_app_register_toolbar(ed->app, ON_TOOLBAR_EDITOR, toolbar);
    return toolbar;
}

void
on_editor_rebuild_toolbars_all(OnApp *app)
{
    GHashTableIter iter;             /* walk of the open-editors table      */
    gpointer key, value;
    g_hash_table_iter_init(&iter, app->editors);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        OnEditor *ed =               /* editor state stashed on its window  */
            g_object_get_data(G_OBJECT(value), "on-editor");
        if (ed == NULL || ed->toolbar_box == NULL)
            continue;

        /* Destroying the old toolbar unregisters it from the style
         * registry; the rebuild registers the new one and refreshes the
         * ed-> widget pointers (toggle_buttons, search_entry).             */
        gchar *query =               /* in-note search survives the rebuild */
            g_strdup(gtk_entry_get_text(GTK_ENTRY(ed->search_entry)));
        gtk_widget_destroy(ed->toolbar);
        ed->toolbar = build_toolbar(ed);
        gtk_box_pack_start(GTK_BOX(ed->toolbar_box), ed->toolbar,
                           FALSE, FALSE, 0);
        gtk_box_reorder_child(GTK_BOX(ed->toolbar_box), ed->toolbar, 0);
        gtk_widget_show_all(ed->toolbar);
        update_toggle_buttons(ed);   /* fresh toggles: mirror inline_flags  */
        if (*query != '\0')          /* re-runs the search + highlights     */
            gtk_entry_set_text(GTK_ENTRY(ed->search_entry), query);
        g_free(query);
    }
}

/* ---------------------------------------------------------------------------
 * editor_window_open_full() — shared implementation behind the two public
 * open functions.  `search_term` (may be NULL) pre-populates the in-note
 * search box and jumps to the first match, so a note opened from the library
 * search window lands with its hit highlighted.
 * ------------------------------------------------------------------------- */
static GtkWidget *
editor_window_open_full(OnApp *app, gint64 note_id, const gchar *search_term)
{
    /* Already open?  Just raise the existing window (and re-run the search
     * if one was requested — the note may already be up from a prior open). */
    GtkWidget *existing = g_hash_table_lookup(app->editors, &note_id);
    if (existing != NULL) {
        gtk_window_present(GTK_WINDOW(existing));
        if (search_term != NULL && *search_term != '\0') {
            OnEditor *ed = g_object_get_data(G_OBJECT(existing), "on-editor");
            if (ed != NULL)
                editor_apply_search_term(ed, search_term);
        }
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
    gtk_window_set_default_size(GTK_WINDOW(ed->window), 720, 580);
    gtk_application_add_window(app->gtk_app, GTK_WINDOW(ed->window));
    {
        gchar *wtitle = g_strdup_printf("Blue Notes - %s", meta->title);
        gtk_window_set_title(GTK_WINDOW(ed->window), wtitle);
        g_free(wtitle);
    }

    /* --- text view ------------------------------------------------------ */
    ed->view = GTK_TEXT_VIEW(gtk_text_view_new());
    ed->buffer = gtk_text_view_get_buffer(ed->view);
    g_object_ref(ed->buffer);        /* keep alive for the final save       */
    on_buffer_ensure_tags(ed->buffer);
    /* Editor-only highlight for in-note search matches (never appears in
     * the serializer's flag table, so it is ignored on save).              */
    gtk_text_buffer_create_tag(ed->buffer, "on-search-hit",
                               "background", "#ffec8b", NULL);
    /* Editor-only padding around color-emoji glyphs (see is_emoji_char).   */
    gtk_text_buffer_create_tag(ed->buffer, "on-emoji",
                               "letter-spacing", 5 * PANGO_SCALE, NULL);

    gtk_text_view_set_editable(ed->view, TRUE);
    gtk_text_view_set_wrap_mode(ed->view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(ed->view, 16);
    gtk_text_view_set_right_margin(ed->view, 16);
    gtk_text_view_set_top_margin(ed->view, 12);
    /* Roughly one text line of bottom margin: typing on the LAST line
     * (where there is no further content to scroll ahead to) still
     * leaves a line-sized gap below the caret.                             */
    gtk_text_view_set_bottom_margin(ed->view, 20);
    gtk_text_view_set_pixels_above_lines(ed->view, 2);
    if (app->code_line_numbers)
        editor_apply_line_numbers(ed);

    /* --- load content ---------------------------------------------------- */
    gsize   blob_len = 0;            /* stored BNBF blob size               */
    guint8 *blob = on_db_note_load(app->db, note_id, &blob_len);
    if (blob != NULL) {
        ed->internal_change++;
        on_note_deserialize(ed->buffer, blob, blob_len);
        editor_attach_image_widgets(ed);
        /* Re-apply the (unserialized) emoji padding to loaded content.     */
        tag_emoji_in_range(ed, 0,
                           gtk_text_buffer_get_char_count(ed->buffer));
        ed->internal_change--;
        g_free(blob);
    }
    /* A brand-new (empty) note gets its first line auto-styled as H1
     * when the option is enabled.                                          */
    ed->auto_h1 = app->first_line_h1 &&
                  gtk_text_buffer_get_char_count(ed->buffer) == 0;

    /* Undo history starts at the loaded state.                             */
    ed->undo_stack   = g_ptr_array_new();
    ed->redo_stack   = g_ptr_array_new();
    ed->undo_current = undo_snapshot_capture(ed);

    /* --- layout ----------------------------------------------------------*/
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ed->toolbar_box = vbox;
    ed->toolbar     = build_toolbar(ed);
    gtk_box_pack_start(GTK_BOX(vbox), ed->toolbar, FALSE, FALSE, 0);
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

    /* --- status bar: note location (bottom left), library-style ---------- */
    ed->status_path = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(ed->status_path), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(ed->status_path),
                            PANGO_ELLIPSIZE_MIDDLE);

    /* A step smaller than the UI font, like the library's labels.          */
    {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, "label { font-size: 85%; }",
                                        -1, NULL);
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(ed->status_path),
            GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }

    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(status_bar, 8);
    gtk_widget_set_margin_end(status_bar, 8);
    gtk_widget_set_margin_top(status_bar, 3);
    gtk_widget_set_margin_bottom(status_bar, 3);
    gtk_box_pack_start(GTK_BOX(status_bar), ed->status_path,
                       TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), status_bar, FALSE, FALSE, 0);
    editor_status_path_update(ed);

    gtk_container_add(GTK_CONTAINER(ed->window), vbox);

    /* --- signals (connected after load so loading doesn't autosave) ----- */
    g_signal_connect(ed->buffer, "insert-text",
                     G_CALLBACK(on_buffer_insert_text_before), ed);
    g_signal_connect_after(ed->buffer, "insert-text",
                           G_CALLBACK(on_buffer_insert_text_after), ed);
    g_signal_connect(ed->buffer, "delete-range",
                     G_CALLBACK(on_buffer_delete_range_before), ed);
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
    g_signal_connect_after(ed->view, "draw",
                           G_CALLBACK(on_view_draw), ed);
    g_signal_connect(ed->window, "destroy",
                     G_CALLBACK(on_editor_destroy), ed);
    g_signal_connect(ed->window, "focus-in-event",
                     G_CALLBACK(on_editor_focus_in), ed);

    /* Give existing code blocks their floating copy buttons.               */
    code_buttons_queue_rebuild(ed);

    /* Register in the open-editors table (key freed by the table), and
     * stash the editor state on its window for cross-module access.        */
    gint64 *key = g_new(gint64, 1);
    *key = note_id;
    g_hash_table_insert(app->editors, key, ed->window);
    g_object_set_data(G_OBJECT(ed->window), "on-editor", ed);

    /* Opened from search: apply the query once the window is realized and
     * allocated so the scroll-to-match lands (see on_scroll_idle).          */
    if (search_term != NULL && *search_term != '\0') {
        ed->pending_search = g_strdup(search_term);
        ed->initial_search_idle = g_idle_add(on_initial_search_idle, ed);
    }

    on_db_note_meta_free(meta);
    gtk_widget_show_all(ed->window);
    gtk_widget_grab_focus(GTK_WIDGET(ed->view));
    return ed->window;
}

GtkWidget *
on_editor_window_open(OnApp *app, gint64 note_id)
{
    return editor_window_open_full(app, note_id, NULL);
}

GtkWidget *
on_editor_window_open_search(OnApp *app, gint64 note_id,
                             const gchar *search_term)
{
    return editor_window_open_full(app, note_id, search_term);
}
