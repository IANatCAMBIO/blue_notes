/* ===========================================================================
 * library_window.c — the Notes Library window (implementation)
 *
 * See library_window.h for the layout overview.  Key mechanics:
 *
 *   sidebar    — a GtkTreeView over a GtkTreeStore holding two sections:
 *                the folder hierarchy (rooted at a fixed "Notes" row) and
 *                a flat "Tags" section.  Row kinds are distinguished by
 *                the SB_KIND column.
 *
 *   notes pane — a GtkListStore shown either as a GtkTreeView (list mode,
 *                drag-reorderable) or a GtkIconView (grid mode).  Both
 *                views stay attached to the same store; a GtkStack flips
 *                between them.
 *
 *   drag&drop  — list rows use GtkTreeView's built-in reordering; the
 *                resulting order is persisted from the "row-deleted"
 *                signal.  Both views are also GTK_TREE_MODEL_ROW drag
 *                sources, and the sidebar accepts those drops to move a
 *                note into a folder.
 * =========================================================================== */

#include "library_window.h"
#include "editor_window.h"
#include "export.h"
#include "search_window.h"
#include "serialize.h"
#include "settings_window.h"

#include <cairo-gobject.h>
#include <glib/gstdio.h>
#include <string.h>

#ifdef HAVE_GTKOSX
#include <gtkosxapplication.h>
#endif

/* Logical pixel size of the square grid-view thumbnails.                    */
#define THUMB_SIZE 140

/* Background for even rows in list view (odd rows stay white).              */
#define ROW_TINT "#e8f2fb"

/* Sidebar row kinds (SB_KIND column).                                       */
enum {
    SB_KIND_ROOT = 0,                /* the fixed "Notes" root row          */
    SB_KIND_FOLDER,                  /* a real folder                       */
    SB_KIND_TAGS_HEADER,             /* the "Tags" section header           */
    SB_KIND_TAG,                     /* one tag                             */
    SB_KIND_PINNED,                  /* the "Pinned Notes" section          */
};

/* Sidebar GtkTreeStore columns.                                             */
enum {
    SB_KIND,                         /* gint: one of SB_KIND_*              */
    SB_ID,                           /* gint64: folder id or tag id         */
    SB_NAME,                         /* gchar*: display text (with count)   */
    SB_RAW,                          /* gchar*: bare name (no count suffix) */
    SB_N_COLS
};

/* Notes GtkListStore columns.                                               */
enum {
    NL_ID,                           /* gint64: note id                     */
    NL_TITLE,                        /* gchar*: note title                  */
    NL_MODIFIED,                     /* gchar*: formatted updated_at        */
    NL_THUMB,                        /* cairo_surface_t*: grid thumbnail    */
    NL_UPDATED,                      /* gint64: raw updated_at (sort key)   */
    NL_N_COLS
};

/* Drag-and-drop target shared by the notes views (sources) and the
 * sidebar (destination).  GTK_TREE_MODEL_ROW is the built-in target used
 * by GtkTreeView's own reordering machinery, so one target serves both
 * in-list reordering and note→folder drops.                                */
static const GtkTargetEntry ROW_TARGET =
    { (gchar *)"GTK_TREE_MODEL_ROW", GTK_TARGET_SAME_APP, 0 };

/* ---------------------------------------------------------------------------
 * OnLibrary — all state for the library window.
 *
 * Fields:
 *   app           — global application context (not owned).
 *   window        — the top-level GtkWindow.
 *   sidebar_store — tree model behind the sidebar.
 *   sidebar       — the sidebar GtkTreeView.
 *   notes_store   — list model behind both notes views.
 *   notes_list    — list-mode view (GtkTreeView).
 *   notes_grid    — grid-mode view (GtkIconView).
 *   stack         — GtkStack switching between list and grid.
 *   sel_kind      — SB_KIND_* of the current sidebar selection; controls
 *                   which notes are listed.
 *   sel_id        — folder id (for ROOT/FOLDER) or tag id (for TAG) of
 *                   the current selection.
 *   sel_name      — bare name of the current selection (owned string),
 *                   used for rename pre-fill and the search scope label.
 *   populating    — nesting counter; >0 while code (not the user) is
 *                   rewriting the models, so persistence signal handlers
 *                   know to stand down.
 *   thumb_cache   — note id (gint64*) → ThumbEntry*, so grid thumbnails
 *                   are only re-rendered when a note actually changed.
 *   shown_kind/
 *   shown_id      — the selection refresh_notes() last populated for;
 *                   a matching refresh (e.g. after an editor autosave)
 *                   keeps the notes-pane scroll position instead of
 *                   jumping back to the top.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnApp        *app;
    GtkWidget    *window;
    GtkTreeStore *sidebar_store;
    GtkTreeView  *sidebar;
    GtkListStore *notes_store;
    GtkTreeView  *notes_list;
    GtkIconView  *notes_grid;
    GtkWidget    *stack;
    gint          sel_kind;
    gint64        sel_id;
    gchar        *sel_name;
    gint          populating;
    GHashTable   *thumb_cache;
    gint          shown_kind;
    gint64        shown_id;
} OnLibrary;

/* ---------------------------------------------------------------------------
 * ThumbEntry — one cached grid thumbnail.
 *
 * Fields:
 *   updated_at — the note's updated_at when the thumbnail was rendered;
 *                a mismatch means the cache entry is stale.
 *   pix        — the rendered thumbnail (owned reference).
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64           updated_at;
    cairo_surface_t *surface;
} ThumbEntry;

/* thumb_entry_free() — GDestroyNotify for cache values.                     */
static void
thumb_entry_free(gpointer data)
{
    ThumbEntry *e = data;
    if (e->surface != NULL)
        cairo_surface_destroy(e->surface);
    g_free(e);
}

/* Forward declarations.                                                     */
static void    refresh_sidebar(OnLibrary *lw);
static void    refresh_notes(OnLibrary *lw);
static GArray *selected_note_ids(OnLibrary *lw);
static void    add_menu_item(GtkWidget *menu, const gchar *label,
                             GCallback callback, gpointer data);
static void    about_button_fit_style(GtkToolItem *item,
                                      GtkToolbarStyle style);
static void    on_action_toolbar_style_changed(GtkToolbar *toolbar,
                                               GtkToolbarStyle style,
                                               gpointer user_data);

/* ===========================================================================
 * scroll preservation
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * ScrollKeep — one vadjustment position being restored after a model
 * rebuild (clearing a store zeroes its view's scrollbar).  The restore
 * runs at idle so the rebuilt view has re-validated its height first.
 * ------------------------------------------------------------------------- */
typedef struct {
    GtkAdjustment *adj;              /* the scrollbar to restore (owned ref)*/
    gdouble        value;            /* position before the rebuild         */
} ScrollKeep;

/* scroll_keep_restore() — idle callback: put the scrollbar back.            */
static gboolean
scroll_keep_restore(gpointer user_data)
{
    ScrollKeep *k = user_data;
    gtk_adjustment_set_value(k->adj, k->value);  /* clamps to the range     */
    g_object_unref(k->adj);
    g_free(k);
    return G_SOURCE_REMOVE;
}

/* scroll_keep_queue() — schedule `adj` to be put back at `value`.           */
static void
scroll_keep_queue(GtkAdjustment *adj, gdouble value)
{
    ScrollKeep *k = g_new(ScrollKeep, 1);
    k->adj   = g_object_ref(adj);
    k->value = value;
    g_idle_add(scroll_keep_restore, k);
}

/* view_vadjustment() — the vadjustment of the scrolled window holding
 * `view` (every scrollable view here sits directly in one).                 */
static GtkAdjustment *
view_vadjustment(GtkWidget *view)
{
    return gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(gtk_widget_get_parent(view)));
}

/* ===========================================================================
 * sidebar population
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * add_folder_rows() — recursively append the subfolders of `parent_id`
 * beneath tree row `parent_iter`.
 *   lw          — the library window.
 *   parent_id   — database folder id whose children to add (0 = roots).
 *   parent_iter — sidebar row to attach them under.
 * ------------------------------------------------------------------------- */
/* count_from_map() — look one id up in a count map (0 when absent).         */
static gint
count_from_map(GHashTable *map, gint64 id)
{
    return GPOINTER_TO_INT(g_hash_table_lookup(map, &id));
}

static void
add_folder_rows(OnLibrary *lw, gint64 parent_id, GtkTreeIter *parent_iter,
                GHashTable *note_counts)
{
    GList *folders = on_db_folder_list(lw->app->db, parent_id);
    for (GList *l = folders; l != NULL; l = l->next) {
        OnFolder *f = l->data;       /* one child folder                    */
        gchar *display = g_strdup_printf(   /* name + note count            */
            "%s (%d)", f->name, count_from_map(note_counts, f->id));
        GtkTreeIter iter;
        gtk_tree_store_append(lw->sidebar_store, &iter, parent_iter);
        gtk_tree_store_set(lw->sidebar_store, &iter,
                           SB_KIND, SB_KIND_FOLDER,
                           SB_ID,   f->id,
                           SB_NAME, display,
                           SB_RAW,  f->name,
                           -1);
        g_free(display);
        add_folder_rows(lw, f->id, &iter, note_counts);
    }
    on_db_folder_list_free(folders);
}

/* ---------------------------------------------------------------------------
 * refresh_sidebar() — rebuild the whole sidebar model: the folder tree
 * under the fixed "Notes" root, then the "Tags" section.  Attempts to
 * restore the previous selection by (kind, id).
 * ------------------------------------------------------------------------- */
static void
refresh_sidebar(OnLibrary *lw)
{
    gint   want_kind = lw->sel_kind; /* selection to restore                */
    gint64 want_id   = lw->sel_id;

    /* Clearing the store zeroes the sidebar scrollbar; a sidebar rebuild
     * is never a navigation (counts changed, a folder was added, …), so
     * the position is always put back.                                     */
    GtkAdjustment *vadj = view_vadjustment(GTK_WIDGET(lw->sidebar));
    gdouble scroll_pos = gtk_adjustment_get_value(vadj);

    lw->populating++;
    gtk_tree_store_clear(lw->sidebar_store);

    /* Batched counts: one query for all folders, one for all tags —
     * refresh_sidebar runs after every autosave, so per-row COUNT
     * queries added up (especially against a shared/networked db).         */
    GHashTable *note_counts = on_db_note_count_map(lw->app->db);
    GHashTable *tag_counts  = on_db_tag_count_map(lw->app->db);

    /* "Notes" root — selecting it shows the top-level notes.               */
    gchar *root_label = g_strdup_printf(
        "Notes (%d)", count_from_map(note_counts, 0));
    GtkTreeIter root;                /* the fixed root row                  */
    gtk_tree_store_append(lw->sidebar_store, &root, NULL);
    gtk_tree_store_set(lw->sidebar_store, &root,
                       SB_KIND, SB_KIND_ROOT,
                       SB_ID,   (gint64)0,
                       SB_NAME, root_label,
                       SB_RAW,  "Notes",
                       -1);
    g_free(root_label);
    add_folder_rows(lw, 0, &root, note_counts);

    /* "Tags" header + one row per known tag.                               */
    GList *tags = on_db_tag_list(lw->app->db);
    if (tags != NULL) {
        GtkTreeIter header;          /* the "Tags" section row              */
        gtk_tree_store_append(lw->sidebar_store, &header, NULL);
        gtk_tree_store_set(lw->sidebar_store, &header,
                           SB_KIND, SB_KIND_TAGS_HEADER,
                           SB_ID,   (gint64)0,
                           SB_NAME, "Tags",
                           SB_RAW,  "Tags",
                           -1);
        for (GList *l = tags; l != NULL; l = l->next) {
            OnTag *t = l->data;      /* one tag                             */
            gchar *raw   = g_strdup_printf("#%s", t->name);
            gchar *label = g_strdup_printf(
                "#%s (%d)", t->name, count_from_map(tag_counts, t->id));
            GtkTreeIter iter;
            gtk_tree_store_append(lw->sidebar_store, &iter, &header);
            gtk_tree_store_set(lw->sidebar_store, &iter,
                               SB_KIND, SB_KIND_TAG,
                               SB_ID,   t->id,
                               SB_NAME, label,
                               SB_RAW,  raw,
                               -1);
            g_free(label);
            g_free(raw);
        }
    }
    on_db_tag_list_free(tags);

    /* "Pinned Notes" below folders and tags — a live view, not a real
     * location; notes stay in their folders.                               */
    gint n_pinned = on_db_note_count_pinned(lw->app->db);
    if (n_pinned > 0) {
        gchar *label = g_strdup_printf("Pinned Notes (%d)", n_pinned);
        GtkTreeIter iter;
        gtk_tree_store_append(lw->sidebar_store, &iter, NULL);
        gtk_tree_store_set(lw->sidebar_store, &iter,
                           SB_KIND, SB_KIND_PINNED,
                           SB_ID,   (gint64)0,
                           SB_NAME, label,
                           SB_RAW,  "Pinned Notes",
                           -1);
        g_free(label);
    }

    g_hash_table_destroy(note_counts);
    g_hash_table_destroy(tag_counts);

    gtk_tree_view_expand_all(lw->sidebar);
    lw->populating--;

    /* Restore the previous selection, falling back to the root row.        */
    GtkTreeSelection *sel = gtk_tree_view_get_selection(lw->sidebar);
    GtkTreeIter iter;                /* candidate row while searching       */
    gboolean restored = FALSE;       /* did we find the old selection?      */

    gboolean valid = gtk_tree_model_get_iter_first(
        GTK_TREE_MODEL(lw->sidebar_store), &iter);
    /* Depth-first walk of the whole sidebar model.                         */
    GQueue queue = G_QUEUE_INIT;     /* pending iters (BFS is fine too)     */
    while (valid || !g_queue_is_empty(&queue)) {
        if (!valid) {
            GtkTreeIter *q = g_queue_pop_head(&queue);
            iter = *q;
            g_free(q);
            valid = TRUE;
        }
        gint   kind;                 /* row kind                            */
        gint64 id;                   /* row id                              */
        gtk_tree_model_get(GTK_TREE_MODEL(lw->sidebar_store), &iter,
                           SB_KIND, &kind, SB_ID, &id, -1);
        if (kind == want_kind && id == want_id) {
            gtk_tree_selection_select_iter(sel, &iter);
            restored = TRUE;
            break;
        }
        GtkTreeIter child;           /* first child, if any                 */
        if (gtk_tree_model_iter_children(GTK_TREE_MODEL(lw->sidebar_store),
                                         &child, &iter)) {
            GtkTreeIter *copy = g_new(GtkTreeIter, 1);
            *copy = child;
            g_queue_push_tail(&queue, copy);
        }
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(lw->sidebar_store),
                                         &iter);
    }
    g_queue_clear_full(&queue, g_free);

    if (!restored) {
        lw->sel_kind = SB_KIND_ROOT;
        lw->sel_id   = 0;
        if (gtk_tree_model_get_iter_first(
                GTK_TREE_MODEL(lw->sidebar_store), &iter))
            gtk_tree_selection_select_iter(sel, &iter);
    }

    if (scroll_pos > 0)
        scroll_keep_queue(vadj, scroll_pos);
}

/* ===========================================================================
 * notes pane population + grid thumbnails
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * render_note_thumb() — draw a square THUMB_SIZE (logical) card for one
 * note: its first embedded image (if any) above the beginning of its body
 * text.  The card is rendered at the window's scale factor and returned
 * as a cairo surface with a matching device scale, so it is sharp on
 * HiDPI/Retina displays.  The title is NOT drawn here — the grid shows it
 * as a real text label under the card.
 *   lw — the library window (for the database and scale factor).
 *   m  — the note to render.
 * Returns a new cairo surface reference.
 * ------------------------------------------------------------------------- */
static cairo_surface_t *
render_note_thumb(OnLibrary *lw, OnNoteMeta *m)
{
    /* Load the note into an offscreen buffer.                              */
    GtkTextBuffer *buf = gtk_text_buffer_new(NULL);
    on_buffer_ensure_tags(buf);
    gsize   blob_len = 0;            /* stored blob size                    */
    guint8 *blob = on_db_note_load(lw->app->db, m->id, &blob_len);
    if (blob != NULL) {
        /* Cap image decode at 512px: the card preview is at most ~256
         * physical pixels wide, so full-resolution decode (potentially
         * tens of MB per screenshot) is pure waste here.                   */
        on_note_deserialize_scaled(buf, blob, blob_len, 512);
        g_free(blob);
    }

    /* First embedded image, if any (borrowed ref, kept alive by buf).      */
    GdkPixbuf *img = NULL;           /* preview image for the card          */
    GtkTextIter it;                  /* scan cursor                         */
    gtk_text_buffer_get_start_iter(buf, &it);
    do {
        GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&it);
        img = (anchor != NULL) ? on_anchor_get_image(anchor, NULL)
                               : gtk_text_iter_get_pixbuf(&it);
        if (img != NULL)
            break;
    } while (gtk_text_iter_forward_char(&it));

    /* Body text: everything after the TITLE line.  The title is the first
     * non-empty line (matching on_buffer_first_line, which derives the
     * name shown under the card) — skipping only the literal first line
     * used to leave the title duplicated inside the thumbnail whenever a
     * note began with blank lines.                                         */
    GtkTextIter s, e;                /* full buffer bounds                  */
    gtk_text_buffer_get_bounds(buf, &s, &e);
    gchar *text = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
    const gchar *body = text;        /* start of the post-title content     */
    while (*body == '\n')
        body++;                      /* skip leading blank lines            */
    const gchar *nl = strchr(body, '\n');
    body = (nl != NULL) ? nl + 1 : "";
    while (*body == '\n')
        body++;                      /* don't lead the card with blanks     */
    gchar *body_cut = g_utf8_substring(
        body, 0, MIN(g_utf8_strlen(body, -1), 300));

    /* Render at the display's scale factor so the card is pixel-sharp.     */
    gint sf = gtk_widget_get_scale_factor(lw->window);
    const gint SZ = THUMB_SIZE;      /* logical square edge length          */
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, SZ * sf, SZ * sf);
    cairo_surface_set_device_scale(surface, sf, sf);
    cairo_t *cr = cairo_create(surface);

    /* White background with a light border.                                */
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, SZ - 1, SZ - 1);
    cairo_stroke(cr);

    gdouble y = 6;                   /* current vertical drawing position   */

    if (img != NULL) {
        /* Fit the image into the card's top area (logical units; cairo's
         * device scale keeps the physical pixels dense).                   */
        gint iw = gdk_pixbuf_get_width(img);
        gint ih = gdk_pixbuf_get_height(img);
        gdouble scale = MIN((gdouble)(SZ - 12) / iw, 72.0 / ih);
        scale = MIN(scale, 1.0);
        gdouble dw = iw * scale, dh = ih * scale;

        cairo_save(cr);
        cairo_translate(cr, (SZ - dw) / 2.0, y);
        cairo_scale(cr, scale, scale);
        gdk_cairo_set_source_pixbuf(cr, img, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr),
                                 CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
        y += dh + 4;
    }

    /* Body preview (small grey), clipped to the card.                      */
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_width(layout, (SZ - 12) * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 8");
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);

    gchar *markup = g_markup_printf_escaped(
        "<span foreground=\"#444444\">%s</span>", body_cut);
    pango_layout_set_markup(layout, markup, -1);
    g_free(markup);

    cairo_rectangle(cr, 6, y, SZ - 12, SZ - y - 6);
    cairo_clip(cr);
    cairo_move_to(cr, 6, y);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
    cairo_destroy(cr);

    g_free(body_cut);
    g_free(text);
    g_object_unref(buf);
    return surface;
}

/* ---------------------------------------------------------------------------
 * get_note_thumb() — cached access to a note's thumbnail; re-renders only
 * when the note's updated_at changed since the cached render.
 * Returns a borrowed reference owned by the cache.
 * ------------------------------------------------------------------------- */
static cairo_surface_t *
get_note_thumb(OnLibrary *lw, OnNoteMeta *m)
{
    ThumbEntry *e = g_hash_table_lookup(lw->thumb_cache, &m->id);
    if (e != NULL && e->updated_at == m->updated_at)
        return e->surface;

    cairo_surface_t *thumb = render_note_thumb(lw, m);
    e = g_new0(ThumbEntry, 1);
    e->updated_at = m->updated_at;
    e->surface    = thumb;

    gint64 *key = g_new(gint64, 1);
    *key = m->id;
    g_hash_table_replace(lw->thumb_cache, key, e);
    return thumb;
}

/* ---------------------------------------------------------------------------
 * refresh_notes() — repopulate the notes model from the current sidebar
 * selection (a folder's notes, or a tag's notes).  When the selection is
 * the same one already shown — a content refresh (autosave, editor
 * close), not a navigation — the scroll position is preserved.
 * ------------------------------------------------------------------------- */
static void
refresh_notes(OnLibrary *lw)
{
    /* Thumbnails are only rendered while the grid is showing: list mode
     * never pays for them (they used to be regenerated for the edited
     * note on EVERY autosave), and switching to grid refreshes.            */
    gboolean want_thumbs =
        g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(lw->stack)),
                  "grid") == 0;

    /* Same selection as last time?  Then remember where the visible
     * notes pane is scrolled so the rebuild doesn't jump to the top.       */
    gboolean keep_scroll = lw->shown_kind == lw->sel_kind &&
                           lw->shown_id   == lw->sel_id;
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(
            gtk_stack_get_visible_child(GTK_STACK(lw->stack))));
    gdouble scroll_pos = gtk_adjustment_get_value(vadj);

    lw->populating++;
    gtk_list_store_clear(lw->notes_store);

    /* Pick the note list matching the selection.                           */
    GList *notes;                    /* OnNoteMeta* list to display         */
    if (lw->sel_kind == SB_KIND_TAG)
        notes = on_db_notes_by_tag(lw->app->db, lw->sel_id);
    else if (lw->sel_kind == SB_KIND_PINNED)
        notes = on_db_note_list_pinned(lw->app->db);
    else
        notes = on_db_note_list(lw->app->db, lw->sel_id);

    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;     /* one note                            */

        /* Format the modification time like "Jun 3, 2026 14:05".           */
        GDateTime *dt = g_date_time_new_from_unix_local(m->updated_at);
        gchar *when = g_date_time_format(dt, "%b %e, %Y %H:%M");
        g_date_time_unref(dt);

        GtkTreeIter iter;
        gtk_list_store_append(lw->notes_store, &iter);
        gtk_list_store_set(lw->notes_store, &iter,
                           NL_ID,       m->id,
                           NL_TITLE,    m->title,
                           NL_MODIFIED, when,
                           NL_THUMB,    want_thumbs ? get_note_thumb(lw, m)
                                                    : NULL,
                           NL_UPDATED,  m->updated_at,
                           -1);
        g_free(when);
    }
    on_db_note_list_free(notes);
    lw->populating--;

    lw->shown_kind = lw->sel_kind;
    lw->shown_id   = lw->sel_id;
    if (keep_scroll && scroll_pos > 0)
        scroll_keep_queue(vadj, scroll_pos);
}

/* ---------------------------------------------------------------------------
 * persist_note_order() — write the notes model's current row order back
 * to the database.  Only meaningful when a folder (not a tag) is shown.
 * ------------------------------------------------------------------------- */
static void
persist_note_order(OnLibrary *lw)
{
    /* Only real folder views own an ordering; tag and pinned views are
     * assembled from elsewhere.                                            */
    if (lw->sel_kind == SB_KIND_TAG || lw->sel_kind == SB_KIND_PINNED)
        return;

    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    GtkTreeIter iter;                /* row cursor                          */
    gboolean valid = gtk_tree_model_get_iter_first(
        GTK_TREE_MODEL(lw->notes_store), &iter);
    while (valid) {
        gint64 id;                   /* note id of this row                 */
        gtk_tree_model_get(GTK_TREE_MODEL(lw->notes_store), &iter,
                           NL_ID, &id, -1);
        g_array_append_val(ids, id);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(lw->notes_store),
                                         &iter);
    }
    on_db_note_reorder(lw->app->db, (gint64 *)ids->data, ids->len);
    g_array_free(ids, TRUE);
}

/* ---------------------------------------------------------------------------
 * on_notes_row_deleted() — fires at the end of a built-in drag-reorder
 * (GtkTreeView implements reordering as insert+delete).  Persist the new
 * order unless we are the ones rewriting the model.
 * ------------------------------------------------------------------------- */
static void
on_notes_row_deleted(GtkTreeModel *model, GtkTreePath *path,
                     gpointer user_data)
{
    (void)model; (void)path;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->populating == 0)
        persist_note_order(lw);
}

/* ===========================================================================
 * selection and activation
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * on_sidebar_selection_changed() — a folder or tag was selected: remember
 * it and refresh the notes pane.
 * ------------------------------------------------------------------------- */
static void
on_sidebar_selection_changed(GtkTreeSelection *sel, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->populating > 0)
        return;

    GtkTreeModel *model;             /* the sidebar model                   */
    GtkTreeIter iter;                /* selected row                        */
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return;

    gint   kind;                     /* selected row kind                   */
    gint64 id;                       /* selected row id                     */
    gchar *raw = NULL;               /* bare name of the row                */
    gtk_tree_model_get(model, &iter, SB_KIND, &kind, SB_ID, &id,
                       SB_RAW, &raw, -1);
    if (kind == SB_KIND_TAGS_HEADER) {
        g_free(raw);
        return;                      /* header row: not a real selection    */
    }

    lw->sel_kind = kind;
    lw->sel_id   = id;
    g_free(lw->sel_name);
    lw->sel_name = raw;              /* ownership transferred               */
    refresh_notes(lw);
}

/* sidebar_select_func() — forbid selecting the "Tags" header row.           */
static gboolean
sidebar_select_func(GtkTreeSelection *sel, GtkTreeModel *model,
                    GtkTreePath *path, gboolean currently_selected,
                    gpointer user_data)
{
    (void)sel; (void)currently_selected; (void)user_data;
    GtkTreeIter iter;                /* row being (de)selected              */
    gint kind;                       /* its kind                            */
    gtk_tree_model_get_iter(model, &iter, path);
    gtk_tree_model_get(model, &iter, SB_KIND, &kind, -1);
    return kind != SB_KIND_TAGS_HEADER;
}

/* ---------------------------------------------------------------------------
 * open_note_at_iter() — open the editor for the note in `iter` of the
 * notes model.
 * ------------------------------------------------------------------------- */
static void
open_note_at_iter(OnLibrary *lw, GtkTreeIter *iter)
{
    gint64 id;                       /* note id                             */
    gtk_tree_model_get(GTK_TREE_MODEL(lw->notes_store), iter,
                       NL_ID, &id, -1);
    on_editor_window_open(lw->app, id);
}

/* on_note_list_activated() — double-click/Enter in list mode opens it.      */
static void
on_note_list_activated(GtkTreeView *view, GtkTreePath *path,
                       GtkTreeViewColumn *col, gpointer user_data)
{
    (void)view; (void)col;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreeIter iter;                /* activated row                       */
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->notes_store),
                                &iter, path))
        open_note_at_iter(lw, &iter);
}

/* on_note_grid_activated() — double-click in grid mode opens the note.      */
static void
on_note_grid_activated(GtkIconView *view, GtkTreePath *path,
                       gpointer user_data)
{
    (void)view;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreeIter iter;                /* activated item                      */
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->notes_store),
                                &iter, path))
        open_note_at_iter(lw, &iter);
}

/* ===========================================================================
 * drag & drop: note → folder
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * on_sidebar_drag_received() — a notes row was dropped on the sidebar.
 * Resolve the drop position to a folder (or the "Notes" root) and move
 * the note there in the database.  The default GtkTreeView handler is
 * suppressed because it would try to splice the dragged row into the
 * *sidebar* model.
 * ------------------------------------------------------------------------- */
static void
on_sidebar_drag_received(GtkWidget *widget, GdkDragContext *context,
                         gint x, gint y, GtkSelectionData *seldata,
                         guint info, guint time, gpointer user_data)
{
    (void)info;
    OnLibrary *lw = user_data;       /* owning library window               */
    g_signal_stop_emission_by_name(widget, "drag-data-received");

    gboolean success = FALSE;        /* whether a note was moved            */

    /* Decode the dragged row: it must come from the notes model.           */
    GtkTreeModel *src_model = NULL;  /* model the drag started in           */
    GtkTreePath  *src_path  = NULL;  /* dragged row's path                  */
    if (gtk_tree_get_row_drag_data(seldata, &src_model, &src_path) &&
        src_model == GTK_TREE_MODEL(lw->notes_store)) {

        GtkTreeIter src_iter;        /* dragged note row                    */
        gint64 note_id = 0;          /* dragged note id                     */
        if (gtk_tree_model_get_iter(src_model, &src_iter, src_path))
            gtk_tree_model_get(src_model, &src_iter, NL_ID, &note_id, -1);

        /* Resolve the drop target row in the sidebar.                      */
        GtkTreePath *dest_path = NULL;          /* row under the pointer    */
        GtkTreeViewDropPosition pos;            /* before/into/after        */
        if (note_id != 0 &&
            gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(widget),
                                              x, y, &dest_path, &pos)) {
            GtkTreeIter dest_iter;   /* target sidebar row                  */
            gint   kind;             /* its kind                            */
            gint64 folder_id;        /* its folder id                       */
            gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->sidebar_store),
                                    &dest_iter, dest_path);
            gtk_tree_model_get(GTK_TREE_MODEL(lw->sidebar_store),
                               &dest_iter,
                               SB_KIND, &kind, SB_ID, &folder_id, -1);
            if (kind == SB_KIND_FOLDER || kind == SB_KIND_ROOT) {
                /* Multi-select support: when the dragged note is part of
                 * the current selection, the whole selection moves.        */
                GArray *ids = selected_note_ids(lw);
                gboolean drag_in_selection = FALSE;
                for (guint i = 0; i < ids->len; i++)
                    if (g_array_index(ids, gint64, i) == note_id)
                        drag_in_selection = TRUE;
                if (!drag_in_selection) {
                    g_array_set_size(ids, 0);
                    g_array_append_val(ids, note_id);
                }
                for (guint i = 0; i < ids->len; i++)
                    success |= on_db_note_move(
                        lw->app->db, g_array_index(ids, gint64, i),
                        folder_id);
                g_array_free(ids, TRUE);
                if (success) {
                    refresh_notes(lw);
                    refresh_sidebar(lw);    /* folder counts changed        */
                }
            }
            gtk_tree_path_free(dest_path);
        }
    }
    if (src_path != NULL)
        gtk_tree_path_free(src_path);
    gtk_drag_finish(context, success, FALSE, time);
}

/* ===========================================================================
 * commands: new note / new folder / delete / rename
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * current_folder_id() — the folder new notes/folders should land in:
 * the selected folder, or the top level when the root or a tag is
 * selected.
 * ------------------------------------------------------------------------- */
static gint64
current_folder_id(OnLibrary *lw)
{
    return (lw->sel_kind == SB_KIND_FOLDER) ? lw->sel_id : 0;
}

/* ---------------------------------------------------------------------------
 * prompt_for_text() — tiny modal dialog with one text entry.
 *   lw      — the library window (dialog parent).
 *   title   — dialog title.
 *   initial — pre-filled entry text, or NULL.
 * Returns the entered string (g_free() it), or NULL if cancelled/empty.
 * ------------------------------------------------------------------------- */
static gchar *
prompt_for_text(OnLibrary *lw, const gchar *title, const gchar *initial)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        title, GTK_WINDOW(lw->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK",     GTK_RESPONSE_OK,
        NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    if (initial != NULL)
        gtk_entry_set_text(GTK_ENTRY(entry), initial);
    gtk_widget_set_margin_start(entry, 12);
    gtk_widget_set_margin_end(entry, 12);
    gtk_widget_set_margin_top(entry, 12);
    gtk_widget_set_margin_bottom(entry, 12);
    gtk_box_pack_start(
        GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
        entry, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);

    gchar *result = NULL;            /* entered text, NULL if cancelled     */
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const gchar *text = gtk_entry_get_text(GTK_ENTRY(entry));
        if (text != NULL && *g_strstrip(g_strdup(text)) != '\0')
            result = g_strdup(text);
    }
    gtk_widget_destroy(dialog);
    return result;
}

/* ---------------------------------------------------------------------------
 * confirm() — modal yes/no warning dialog: a 32px warning icon above a
 * centered primary question and a centered secondary line.
 *   lw        — the library window (dialog parent).
 *   primary   — the question (e.g. "Delete this note?").
 *   secondary — supporting line (e.g. "This cannot be undone."); may be
 *               NULL.
 * Returns TRUE if the user accepted.
 * ------------------------------------------------------------------------- */
static gboolean
confirm(OnLibrary *lw, const gchar *primary, const gchar *secondary)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Confirm", GTK_WINDOW(lw->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_No",  GTK_RESPONSE_NO,
        "_Yes", GTK_RESPONSE_YES,
        NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    /* Warning icon on top (elementary status icon; ⚠ as fallback).         */
    GtkWidget *icon = on_app_icon_image_sized(lw->app, "dialog-warning",
                                              32);
    if (icon == NULL)
        icon = gtk_label_new("\xe2\x9a\xa0");
    gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);

    GtkWidget *primary_label = gtk_label_new(primary);
    gtk_label_set_justify(GTK_LABEL(primary_label), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(primary_label, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(box), primary_label, FALSE, FALSE, 0);

    if (secondary != NULL) {
        GtkWidget *secondary_label = gtk_label_new(secondary);
        gtk_label_set_justify(GTK_LABEL(secondary_label),
                              GTK_JUSTIFY_CENTER);
        gtk_widget_set_halign(secondary_label, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(box), secondary_label, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(
        GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
        box, TRUE, TRUE, 0);

    /* Center the No/Yes buttons under the text, 8px apart.  The action
     * area accessor is deprecated but remains the only way to restyle
     * the button row in GTK3.                                              */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GtkWidget *action_area = gtk_dialog_get_action_area(GTK_DIALOG(dialog));
    G_GNUC_END_IGNORE_DEPRECATIONS
    gtk_button_box_set_layout(GTK_BUTTON_BOX(action_area),
                              GTK_BUTTONBOX_CENTER);
    gtk_box_set_spacing(GTK_BOX(action_area), 8);

    gtk_widget_show_all(dialog);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return response == GTK_RESPONSE_YES;
}

/* on_new_note() — create a note in the current folder and open it.          */
static void
on_new_note(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    gint64 id = on_db_note_create(lw->app->db, current_folder_id(lw));
    if (id != 0) {
        refresh_notes(lw);
        refresh_sidebar(lw);         /* the folder's count just grew        */
        on_editor_window_open(lw->app, id);
    }
}

/* on_new_folder() — prompt for a name; create under the current folder.     */
static void
on_new_folder(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    gchar *name = prompt_for_text(lw, "New Folder", NULL);
    if (name != NULL) {
        on_db_folder_create(lw->app->db, current_folder_id(lw), name);
        g_free(name);
        refresh_sidebar(lw);
    }
}

/* ---------------------------------------------------------------------------
 * selected_note_ids() — the ids of every note selected in whichever notes
 * view is active (both views allow multi-selection).
 * Returns a GArray of gint64; free with g_array_free(ids, TRUE).
 * ------------------------------------------------------------------------- */
static GArray *
selected_note_ids(OnLibrary *lw)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    const gchar *mode =              /* "list" or "grid"                    */
        gtk_stack_get_visible_child_name(GTK_STACK(lw->stack));

    GList *paths;                    /* selected row paths                  */
    if (g_strcmp0(mode, "grid") == 0) {
        paths = gtk_icon_view_get_selected_items(lw->notes_grid);
    } else {
        paths = gtk_tree_selection_get_selected_rows(
            gtk_tree_view_get_selection(lw->notes_list), NULL);
    }

    for (GList *l = paths; l != NULL; l = l->next) {
        GtkTreeIter iter;            /* one selected row                    */
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->notes_store),
                                    &iter, l->data)) {
            gint64 id;               /* its note id                         */
            gtk_tree_model_get(GTK_TREE_MODEL(lw->notes_store), &iter,
                               NL_ID, &id, -1);
            g_array_append_val(ids, id);
        }
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    return ids;
}

/* ---------------------------------------------------------------------------
 * delete_notes_with_confirm() — confirm once, then delete every note in
 * `ids` (closing any open editors first).
 * ------------------------------------------------------------------------- */
static void
delete_notes_with_confirm(OnLibrary *lw, GArray *ids)
{
    if (ids->len == 0)
        return;
    gchar *question = (ids->len == 1)
        ? g_strdup("Delete this note?")
        : g_strdup_printf("Delete these %u notes?", ids->len);
    gboolean ok = confirm(lw, question, "This cannot be undone.");
    g_free(question);
    if (!ok)
        return;

    for (guint i = 0; i < ids->len; i++) {
        gint64 note_id = g_array_index(ids, gint64, i);
        /* Close its editor first, if open.                                 */
        GtkWidget *editor =
            g_hash_table_lookup(lw->app->editors, &note_id);
        if (editor != NULL)
            gtk_widget_destroy(editor);
        on_db_note_delete(lw->app->db, note_id);
    }
    refresh_notes(lw);
    refresh_sidebar(lw);             /* tag list/counts may have changed    */
}

/* on_delete_note() — action-bar Delete: remove every selected note.         */
static void
on_delete_note(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    GArray *ids = selected_note_ids(lw);
    delete_notes_with_confirm(lw, ids);
    g_array_free(ids, TRUE);
}

/* on_delete_folder() — sidebar-toolbar Delete: remove the selected folder
 * (and, by cascade, everything inside it).                                  */
static void
on_delete_folder(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->sel_kind != SB_KIND_FOLDER)
        return;
    if (confirm(lw, "Delete this folder and everything inside it?",
                "This cannot be undone.")) {
        on_db_folder_delete(lw->app->db, lw->sel_id);
        lw->sel_kind = SB_KIND_ROOT;
        lw->sel_id   = 0;
        refresh_sidebar(lw);
        refresh_notes(lw);
    }
}

/* on_rename_folder() — prompt for a new name for the selected folder.       */
static void
on_rename_folder(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->sel_kind != SB_KIND_FOLDER)
        return;
    gchar *name = prompt_for_text(lw, "Rename Folder", lw->sel_name);
    if (name != NULL) {
        on_db_folder_rename(lw->app->db, lw->sel_id, name);
        g_free(name);
        refresh_sidebar(lw);
    }
}

/* on_open_search() — sidebar-toolbar Search: open the search window (it
 * reads the live library selection each time Search is pressed).  The
 * scope radio defaults to the selection when it is narrower than "all".    */
static void
on_open_search(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    on_search_window_open(lw->app, lw->sel_kind != SB_KIND_ROOT);
}

/* on_open_settings() — File → Settings…                                     */
static void
on_open_settings(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    on_settings_window_open(lw->app);
}

/* ---------------------------------------------------------------------------
 * on_backup_db() — File → Back Up Database…: write a consistent snapshot
 * of the live database to a user-chosen file.
 * ------------------------------------------------------------------------- */
static void
on_backup_db(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */

    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Back Up Database", GTK_WINDOW(lw->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Back Up", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(
        GTK_FILE_CHOOSER(chooser), TRUE);

    /* Suggest a dated filename.                                            */
    GDateTime *now = g_date_time_new_now_local();
    gchar *suggestion = g_date_time_format(
        now, "orange-notes-backup-%Y%m%d.db");
    g_date_time_unref(now);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser),
                                      suggestion);
    g_free(suggestion);

    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(
            GTK_FILE_CHOOSER(chooser));
        gtk_widget_destroy(chooser);

        gboolean ok = on_db_backup_to(lw->app->db, path);
        GtkWidget *msg = gtk_message_dialog_new(
            GTK_WINDOW(lw->window), GTK_DIALOG_MODAL,
            ok ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            ok ? "Database backed up to\n%s" : "Backup to %s failed",
            path);
        gtk_dialog_run(GTK_DIALOG(msg));
        gtk_widget_destroy(msg);
        g_free(path);
    } else {
        gtk_widget_destroy(chooser);
    }
}

/* ---------------------------------------------------------------------------
 * on_restore_db() — File → Restore Database…: replace the current
 * database with a backup file (after confirmation; the replaced file is
 * kept as notes.db.pre-restore).
 * ------------------------------------------------------------------------- */
static void
on_restore_db(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */

    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Restore Database", GTK_WINDOW(lw->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Restore", GTK_RESPONSE_ACCEPT,
        NULL);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Database files (*.db)");
    gtk_file_filter_add_pattern(filter, "*.db");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);

    if (gtk_dialog_run(GTK_DIALOG(chooser)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(chooser);
        return;
    }
    gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);

    if (confirm(lw, "Replace ALL current notes with this backup?",
                "The current database will be kept as "
                "notes.db.pre-restore.")) {
        gboolean ok = on_app_restore_database(lw->app, path);
        GtkWidget *msg = gtk_message_dialog_new(
            GTK_WINDOW(lw->window), GTK_DIALOG_MODAL,
            ok ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            ok ? "Database restored." :
                 "Restore failed — the previous database is still in use.");
        gtk_dialog_run(GTK_DIALOG(msg));
        gtk_widget_destroy(msg);
    }
    g_free(path);
}

/* ---------------------------------------------------------------------------
 * on_about() — File → About: the standard about dialog with the app icon,
 * author, build date and a link to the BSD license.
 * ------------------------------------------------------------------------- */
static void
on_about(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */

    /* 64x64 logo from orange.png, which lives next to the icons/ folder.   */
    gchar *base = g_path_get_dirname(lw->app->icons_dir);
    gchar *icon_path = g_build_filename(base, "orange.png", NULL);
    GdkPixbuf *logo = gdk_pixbuf_new_from_file_at_size(icon_path, 64, 64,
                                                       NULL);
    g_free(icon_path);
    g_free(base);

    const gchar *authors[] = { "Ian Campbell", "Claude Sonnet 4.5", NULL };

    GtkWidget *dialog = gtk_about_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog),
                                 GTK_WINDOW(lw->window));
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog),
                                      "Orange Notes");
    if (logo != NULL) {
        gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(dialog), logo);
        g_object_unref(logo);
    }
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);

    /* Database vitals: entry counts, location, on-disk size.               */
    gint n_notes, n_folders, n_tags;     /* totals across the database      */
    on_db_totals(lw->app->db, &n_notes, &n_folders, &n_tags);
    GStatBuf st;                     /* for the database file size          */
    gchar *size_str = (g_stat(lw->app->db->path, &st) == 0)
                      ? g_format_size((guint64)st.st_size)
                      : g_strdup("unknown");

    /* __DATE__/__TIME__ expand when this file is compiled — the closest
     * portable thing to a "last compiled" stamp.                           */
    gchar *comments = g_strdup_printf(
        "Compiled " __DATE__ " " __TIME__ "\n\n"
        "Database: %s\n"
        "%d notes in %d folders, %d tags \xe2\x80\x94 %s on disk",
        lw->app->db->path, n_notes, n_folders, n_tags, size_str);
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog), comments);
    g_free(comments);
    g_free(size_str);
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dialog),
                                      GTK_LICENSE_BSD);
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
                                 "https://opensource.org/license/bsd-3-clause");
    gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog),
                                       "BSD License");

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* ---------------------------------------------------------------------------
 * on_quit() — File → Quit: destroy every application window.  Editor
 * windows flush their final autosave from their destroy handlers, and the
 * GTK main loop ends once the last window is gone.
 * ------------------------------------------------------------------------- */
static void
on_quit(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    GList *windows =                 /* copy: destroying mutates the list   */
        g_list_copy(gtk_application_get_windows(lw->app->gtk_app));
    for (GList *l = windows; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(windows);
}

/* on_sidebar_search_here() — sidebar context menu Search: the clicked row
 * is already selected, so a scoped search targets it directly.              */
static void
on_sidebar_search_here(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    on_search_window_open(lw->app, TRUE);
}

/* ---------------------------------------------------------------------------
 * on_sidebar_button_press() — right click in the folder/tag tree: select
 * the row under the pointer and show a context menu mirroring the sidebar
 * toolbar (folder actions + scoped search).
 * ------------------------------------------------------------------------- */
static gboolean
on_sidebar_button_press(GtkWidget *widget, GdkEventButton *event,
                        gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    if (event->button != GDK_BUTTON_SECONDARY)
        return FALSE;

    GtkTreePath *path = NULL;        /* row under the pointer               */
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
                                       (gint)event->x, (gint)event->y,
                                       &path, NULL, NULL, NULL))
        return FALSE;

    gtk_tree_selection_select_path(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)), path);

    /* What kind of row was clicked decides which actions make sense.       */
    GtkTreeIter iter;                /* the clicked row                     */
    gint kind = SB_KIND_TAGS_HEADER; /* default: nothing to offer           */
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->sidebar_store),
                                &iter, path))
        gtk_tree_model_get(GTK_TREE_MODEL(lw->sidebar_store), &iter,
                           SB_KIND, &kind, -1);
    gtk_tree_path_free(path);
    if (kind == SB_KIND_TAGS_HEADER)
        return TRUE;                 /* consumed, but no menu               */

    GtkWidget *menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(menu), lw->window, NULL);
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);

    if (kind == SB_KIND_FOLDER || kind == SB_KIND_ROOT) {
        add_menu_item(menu,
                      kind == SB_KIND_FOLDER ? "New _Subfolder\xe2\x80\xa6"
                                             : "New _Folder\xe2\x80\xa6",
                      G_CALLBACK(on_new_folder), lw);
        if (kind == SB_KIND_FOLDER) {
            add_menu_item(menu, "_Rename\xe2\x80\xa6",
                          G_CALLBACK(on_rename_folder), lw);
            add_menu_item(menu, "_Delete Folder",
                          G_CALLBACK(on_delete_folder), lw);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
    }
    add_menu_item(menu, "Search _Here\xe2\x80\xa6",
                  G_CALLBACK(on_sidebar_search_here), lw);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    return TRUE;
}

/* ===========================================================================
 * view mode & export
 * =========================================================================== */

/* on_view_list() / on_view_grid() — flip the stack between the modes.       */
static void
on_view_list(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    gtk_stack_set_visible_child_name(GTK_STACK(lw->stack), "list");
}

static void
on_view_grid(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    gtk_stack_set_visible_child_name(GTK_STACK(lw->stack), "grid");
    refresh_notes(lw);               /* fill the thumbnails list mode skips */
}

/* ---------------------------------------------------------------------------
 * run_export() — pick a destination directory and export every note in
 * the requested format, then report the result.
 *   lw     — the library window.
 *   format — ON_EXPORT_HTML or ON_EXPORT_MARKDOWN.
 * ------------------------------------------------------------------------- */
static void
run_export(OnLibrary *lw, OnExportFormat format)
{
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Choose Export Folder", GTK_WINDOW(lw->window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Export", GTK_RESPONSE_ACCEPT,
        NULL);

    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        gchar *dir = gtk_file_chooser_get_filename(
            GTK_FILE_CHOOSER(chooser));
        gtk_widget_destroy(chooser);

        gchar *err = NULL;           /* exporter error message              */
        gint   n   = on_export_all(lw->app, dir, format, &err);

        GtkWidget *msg;              /* result dialog                       */
        if (n >= 0) {
            msg = gtk_message_dialog_new(
                GTK_WINDOW(lw->window), GTK_DIALOG_MODAL,
                GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                "Exported %d note%s to\n%s", n, n == 1 ? "" : "s", dir);
        } else {
            msg = gtk_message_dialog_new(
                GTK_WINDOW(lw->window), GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                "Export failed: %s", err != NULL ? err : "unknown error");
        }
        gtk_dialog_run(GTK_DIALOG(msg));
        gtk_widget_destroy(msg);
        g_free(err);
        g_free(dir);
    } else {
        gtk_widget_destroy(chooser);
    }
}

/* on_export_html() / on_export_markdown() — File-menu export entries.       */
static void
on_export_html(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    run_export((OnLibrary *)user_data, ON_EXPORT_HTML);
}

static void
on_export_markdown(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    run_export((OnLibrary *)user_data, ON_EXPORT_MARKDOWN);
}

/* ===========================================================================
 * per-note context menu (right click in list or grid)
 * =========================================================================== */

/* Context-menu item handlers.  Delete and export act on the whole
 * selection; Open uses the note id stashed on the item as boxed object
 * data "on-note-id".                                                        */
static void
on_note_ctx_open(GtkMenuItem *item, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    gint64 *id = g_object_get_data(G_OBJECT(item), "on-note-id");
    if (id != NULL)
        on_editor_window_open(lw->app, *id);
}

/* ctx_export_selection() — export every selected note to one directory.     */
static void
ctx_export_selection(OnLibrary *lw, OnExportFormat format)
{
    GArray *ids = selected_note_ids(lw);
    if (ids->len == 0) {
        g_array_free(ids, TRUE);
        return;
    }

    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Choose Export Folder", GTK_WINDOW(lw->window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Export", GTK_RESPONSE_ACCEPT,
        NULL);
    if (gtk_dialog_run(GTK_DIALOG(chooser)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(chooser);
        g_array_free(ids, TRUE);
        return;
    }
    gchar *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);

    gint exported = 0;               /* how many notes were written         */
    for (guint i = 0; i < ids->len; i++)
        if (on_export_note(lw->app, g_array_index(ids, gint64, i),
                           dir, format))
            exported++;

    GtkWidget *msg = gtk_message_dialog_new(
        GTK_WINDOW(lw->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "Exported %d note%s to\n%s",
        exported, exported == 1 ? "" : "s", dir);
    gtk_dialog_run(GTK_DIALOG(msg));
    gtk_widget_destroy(msg);
    g_free(dir);
    g_array_free(ids, TRUE);
}

static void
on_note_ctx_export_html(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    ctx_export_selection((OnLibrary *)user_data, ON_EXPORT_HTML);
}

static void
on_note_ctx_export_md(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    ctx_export_selection((OnLibrary *)user_data, ON_EXPORT_MARKDOWN);
}

static void
on_note_ctx_delete(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    OnLibrary *lw = user_data;       /* owning library window               */
    GArray *ids = selected_note_ids(lw);
    delete_notes_with_confirm(lw, ids);
    g_array_free(ids, TRUE);
}

/* on_note_ctx_toggle_pin() — pin or unpin every selected note (the new
 * state is the opposite of the clicked note's current state).               */
static void
on_note_ctx_toggle_pin(GtkMenuItem *item, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    gboolean pin =                   /* target state for the selection      */
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "on-pin"));

    GArray *ids = selected_note_ids(lw);
    for (guint i = 0; i < ids->len; i++)
        on_db_note_set_pinned(lw->app->db,
                              g_array_index(ids, gint64, i), pin);
    g_array_free(ids, TRUE);

    refresh_sidebar(lw);             /* Pinned Notes count/section          */
    refresh_notes(lw);               /* the pinned view may be showing      */
}

/* ---------------------------------------------------------------------------
 * show_note_context_menu() — build and pop up the per-note menu.
 *   lw      — the library window.
 *   note_id — the note that was right-clicked.
 *   event   — the triggering button event (for popup placement).
 * ------------------------------------------------------------------------- */
static void
show_note_context_menu(OnLibrary *lw, gint64 note_id, GdkEventButton *event)
{
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(menu), lw->window, NULL);
    /* One-shot menu: destroy it after the selection is done.               */
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);

    /* Pin/Unpin reflects the clicked note's current state.                 */
    OnNoteMeta *meta = on_db_note_get(lw->app->db, note_id);
    gboolean pinned = meta != NULL && meta->pinned;
    on_db_note_meta_free(meta);

    struct { const gchar *label; GCallback cb; } items[] = {
        { "_Open",                          G_CALLBACK(on_note_ctx_open) },
        { pinned ? "Un_pin" : "_Pin",       G_CALLBACK(on_note_ctx_toggle_pin) },
        { NULL,                             NULL /* separator */          },
        { "Export as _HTML\xe2\x80\xa6",    G_CALLBACK(on_note_ctx_export_html) },
        { "Export as _Markdown\xe2\x80\xa6",G_CALLBACK(on_note_ctx_export_md)   },
        { NULL,                             NULL /* separator */          },
        { "_Delete",                        G_CALLBACK(on_note_ctx_delete) },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(items); i++) {
        GtkWidget *mi;               /* menu item (or separator)            */
        if (items[i].label == NULL) {
            mi = gtk_separator_menu_item_new();
        } else {
            mi = gtk_menu_item_new_with_mnemonic(items[i].label);
            gint64 *boxed = g_new(gint64, 1);
            *boxed = note_id;
            g_object_set_data_full(G_OBJECT(mi), "on-note-id",
                                   boxed, g_free);
            if (items[i].cb == G_CALLBACK(on_note_ctx_toggle_pin))
                g_object_set_data(G_OBJECT(mi), "on-pin",
                                  GINT_TO_POINTER(!pinned));
            g_signal_connect(mi, "activate", items[i].cb, lw);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

/* ---------------------------------------------------------------------------
 * on_notes_list_button_press() — right click in list mode: select the row
 * under the pointer and show the note menu.
 * ------------------------------------------------------------------------- */
static gboolean
on_notes_list_button_press(GtkWidget *widget, GdkEventButton *event,
                           gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    if (event->button != GDK_BUTTON_SECONDARY)
        return FALSE;

    GtkTreePath *path = NULL;        /* row under the pointer               */
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
                                       (gint)event->x, (gint)event->y,
                                       &path, NULL, NULL, NULL))
        return FALSE;

    /* Right-clicking inside an existing multi-selection keeps it (so bulk
     * actions can target it); clicking elsewhere selects just that row.    */
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
    if (!gtk_tree_selection_path_is_selected(sel, path)) {
        gtk_tree_selection_unselect_all(sel);
        gtk_tree_selection_select_path(sel, path);
    }

    GtkTreeIter iter;                /* the clicked row                     */
    gint64 id = 0;                   /* its note id                         */
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->notes_store),
                                &iter, path))
        gtk_tree_model_get(GTK_TREE_MODEL(lw->notes_store), &iter,
                           NL_ID, &id, -1);
    gtk_tree_path_free(path);

    if (id != 0)
        show_note_context_menu(lw, id, event);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * on_notes_grid_button_press() — right click in grid mode: same as above
 * for the icon view.
 * ------------------------------------------------------------------------- */
static gboolean
on_notes_grid_button_press(GtkWidget *widget, GdkEventButton *event,
                           gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    if (event->button != GDK_BUTTON_SECONDARY)
        return FALSE;

    GtkTreePath *path = gtk_icon_view_get_path_at_pos(
        GTK_ICON_VIEW(widget), (gint)event->x, (gint)event->y);
    if (path == NULL)
        return FALSE;

    /* Keep an existing multi-selection when right-clicking inside it.      */
    if (!gtk_icon_view_path_is_selected(GTK_ICON_VIEW(widget), path)) {
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(widget));
        gtk_icon_view_select_path(GTK_ICON_VIEW(widget), path);
    }

    GtkTreeIter iter;                /* the clicked item                    */
    gint64 id = 0;                   /* its note id                         */
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->notes_store),
                                &iter, path))
        gtk_tree_model_get(GTK_TREE_MODEL(lw->notes_store), &iter,
                           NL_ID, &id, -1);
    gtk_tree_path_free(path);

    if (id != 0)
        show_note_context_menu(lw, id, event);
    return TRUE;
}

/* ===========================================================================
 * refresh hook + construction
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * library_notify_notes_changed() — installed as app->notify_notes_changed;
 * editor windows call it after each save so titles, ordering and the tag
 * sidebar stay current.
 * ------------------------------------------------------------------------- */
static void
library_notify_notes_changed(OnApp *app)
{
    OnLibrary *lw =                  /* library state stashed on the window */
        g_object_get_data(G_OBJECT(app->library_window), "on-library");
    if (lw != NULL) {
        refresh_sidebar(lw);
        refresh_notes(lw);
    }
}

/* ---------------------------------------------------------------------------
 * library_notify_note_saved() — installed as app->notify_note_saved; the
 * light editor-save refresh: titles and modified times in the notes pane
 * only.  Editing a note can't change folder counts, so the sidebar (and
 * its scroll position) is deliberately left alone; a save that changed
 * the note's tag set uses the full notify_notes_changed instead.
 * ------------------------------------------------------------------------- */
static void
library_notify_note_saved(OnApp *app)
{
    OnLibrary *lw =                  /* library state stashed on the window */
        g_object_get_data(G_OBJECT(app->library_window), "on-library");
    if (lw != NULL)
        refresh_notes(lw);
}

void
on_library_apply_native_menubar(OnApp *app, gboolean native)
{
#ifdef HAVE_GTKOSX
    GtkWidget *menubar =             /* the in-window GtkMenuBar            */
        g_object_get_data(G_OBJECT(app->library_window), "on-menubar");
    if (menubar == NULL)
        return;

    GtkosxApplication *osx = gtkosx_application_get();
    if (native) {
        /* The same menu shell drives the macOS bar; the in-window widget
         * just has to be hidden.                                           */
        gtk_widget_hide(menubar);
        gtkosx_application_set_menu_bar(osx, GTK_MENU_SHELL(menubar));
    } else {
        gtk_widget_show(menubar);
        /* Hand macOS an empty bar so the app menu stays functional.        */
        GtkWidget *empty = gtk_menu_bar_new();
        gtkosx_application_set_menu_bar(osx, GTK_MENU_SHELL(empty));
    }
    gtkosx_application_sync_menubar(osx);
#else
    (void)app; (void)native;
#endif
}

void
on_library_get_scope(OnApp *app, OnSearchScope *scope, gint64 *id,
                     gchar **name)
{
    *scope = ON_SCOPE_FOLDER;
    *id    = 0;
    *name  = g_strdup("Notes");

    OnLibrary *lw =                  /* library state stashed on the window */
        (app->library_window != NULL)
        ? g_object_get_data(G_OBJECT(app->library_window), "on-library")
        : NULL;
    if (lw == NULL)
        return;

    *scope = (lw->sel_kind == SB_KIND_TAG) ? ON_SCOPE_TAG
                                           : ON_SCOPE_FOLDER;
    *id    = lw->sel_id;
    if (lw->sel_name != NULL) {
        g_free(*name);
        *name = g_strdup(lw->sel_name);
    }
}

/* ---------------------------------------------------------------------------
 * sort_by_title() — case-insensitive alphabetical sort for the Title
 * header.
 * ------------------------------------------------------------------------- */
static gint
sort_by_title(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
              gpointer user_data)
{
    (void)user_data;
    gchar *ta, *tb;                  /* the two titles                      */
    gtk_tree_model_get(model, a, NL_TITLE, &ta, -1);
    gtk_tree_model_get(model, b, NL_TITLE, &tb, -1);
    gchar *ca = g_utf8_casefold(ta != NULL ? ta : "", -1);
    gchar *cb = g_utf8_casefold(tb != NULL ? tb : "", -1);
    gint result = g_strcmp0(ca, cb);
    g_free(ca); g_free(cb);
    g_free(ta); g_free(tb);
    return result;
}

/* ---------------------------------------------------------------------------
 * sort_by_updated() — sort for the Modified header.  Deliberately
 * inverted so the FIRST click on the header shows the most recently
 * modified notes on top.
 * ------------------------------------------------------------------------- */
static gint
sort_by_updated(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
                gpointer user_data)
{
    (void)user_data;
    gint64 ua, ub;                   /* the two timestamps                  */
    gtk_tree_model_get(model, a, NL_UPDATED, &ua, -1);
    gtk_tree_model_get(model, b, NL_UPDATED, &ub, -1);
    return (ub > ua) - (ub < ua);
}

/* ---------------------------------------------------------------------------
 * notes_row_bg_func() — cell data function giving list rows alternating
 * white / light-blue backgrounds regardless of theme.
 * ------------------------------------------------------------------------- */
static void
notes_row_bg_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                  GtkTreeModel *model, GtkTreeIter *iter,
                  gpointer user_data)
{
    (void)col; (void)user_data;
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gboolean even =                  /* row parity drives the tint          */
        (gtk_tree_path_get_indices(path)[0] % 2) == 0;
    gtk_tree_path_free(path);
    g_object_set(cell,
                 "cell-background", even ? NULL : ROW_TINT,
                 NULL);
}

/* ---------------------------------------------------------------------------
 * add_menu_item() — helper: append one item with a callback to a menu.
 *   menu     — the GtkMenu to append to.
 *   label    — item label (mnemonics with '_').
 *   callback — "activate" handler.
 *   data     — user data for the handler.
 * ------------------------------------------------------------------------- */
static void
add_menu_item(GtkWidget *menu, const gchar *label, GCallback callback,
              gpointer data)
{
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", callback, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

/* ---------------------------------------------------------------------------
 * build_menubar() — the File and View menus.
 * Returns the GtkMenuBar widget.
 * ------------------------------------------------------------------------- */
static GtkWidget *
build_menubar(OnLibrary *lw)
{
    GtkWidget *bar = gtk_menu_bar_new();

    /* File menu.                                                           */
    GtkWidget *file_menu = gtk_menu_new();
    add_menu_item(file_menu, "_New Note",
                  G_CALLBACK(on_new_note), lw);
    add_menu_item(file_menu, "New _Folder\xe2\x80\xa6",
                  G_CALLBACK(on_new_folder), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    add_menu_item(file_menu, "Export All as _HTML\xe2\x80\xa6",
                  G_CALLBACK(on_export_html), lw);
    add_menu_item(file_menu, "Export All as _Markdown\xe2\x80\xa6",
                  G_CALLBACK(on_export_markdown), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    add_menu_item(file_menu, "_Back Up Database\xe2\x80\xa6",
                  G_CALLBACK(on_backup_db), lw);
    add_menu_item(file_menu, "Restore _Database\xe2\x80\xa6",
                  G_CALLBACK(on_restore_db), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    add_menu_item(file_menu, "_Settings\xe2\x80\xa6",
                  G_CALLBACK(on_open_settings), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    add_menu_item(file_menu, "_About", G_CALLBACK(on_about), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    add_menu_item(file_menu, "_Quit", G_CALLBACK(on_quit), lw);

    GtkWidget *file_root = gtk_menu_item_new_with_mnemonic("_File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_root), file_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), file_root);

    /* View menu.  (Toolbar styles now live in File → Settings….)           */
    GtkWidget *view_menu = gtk_menu_new();
    add_menu_item(view_menu, "Notes as _List", G_CALLBACK(on_view_list), lw);
    add_menu_item(view_menu, "Notes as _Grid", G_CALLBACK(on_view_grid), lw);

    GtkWidget *view_root = gtk_menu_item_new_with_mnemonic("_View");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_root), view_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), view_root);

    return bar;
}

/* ---------------------------------------------------------------------------
 * add_tool_button() — helper: append a tool button with a callback.
 *   lw       — the library window.
 *   toolbar  — the GtkToolbar to append to.
 *   icon     — local icon file basename, or NULL.
 *   fallback — markup shown as the icon when the file is missing.
 *   label    — button text label.
 *   tooltip  — hover help.
 *   cb       — "clicked" handler.
 * ------------------------------------------------------------------------- */
static void
add_tool_button(OnLibrary *lw, GtkWidget *toolbar, const gchar *icon,
                const gchar *fallback, const gchar *label,
                const gchar *tooltip, GCallback cb)
{
    GtkToolItem *item = on_app_tool_item_new(lw->app, FALSE, icon,
                                             fallback, label, tooltip);
    g_signal_connect(item, "clicked", cb, lw);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);
}

/* ---------------------------------------------------------------------------
 * build_action_bar() — the toolbar above the notes pane: note actions on
 * the left, the List/Grid switcher pushed to the right.
 * Returns the toolbar widget.
 * ------------------------------------------------------------------------- */
static GtkWidget *
build_action_bar(OnLibrary *lw)
{
    GtkWidget *toolbar = gtk_toolbar_new();
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar),
                              GTK_ICON_SIZE_SMALL_TOOLBAR);

    add_tool_button(lw, toolbar, "document-new", "+", "New Note",
                    "Create a note in the current folder",
                    G_CALLBACK(on_new_note));
    add_tool_button(lw, toolbar, "edit-delete", "\xe2\x9c\x95",
                    "Delete Note", "Delete the selected note",
                    G_CALLBACK(on_delete_note));

    /* Expanding separator pushes the view switcher to the right edge.      */
    GtkToolItem *spacer = gtk_separator_tool_item_new();
    gtk_separator_tool_item_set_draw(GTK_SEPARATOR_TOOL_ITEM(spacer),
                                     FALSE);
    gtk_tool_item_set_expand(spacer, TRUE);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), spacer, -1);

    /* The app logo at the far right opens the About dialog.  Built by
     * hand because the logo lives at the project root, not in icons/, and
     * because the child must stay centered (see about_button_fit_style).
     * (List/Grid switching lives in the View menu.)                        */
    GtkToolItem *about_item = gtk_tool_item_new();
    GtkWidget *about_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(about_btn), GTK_RELIEF_NONE);
    gtk_container_add(GTK_CONTAINER(about_item), about_btn);
    {
        gchar *base = g_path_get_dirname(lw->app->icons_dir);
        gchar *logo_path = g_build_filename(base, "orange.png", NULL);
        gint sf = gtk_widget_get_scale_factor(lw->window);
        GdkPixbuf *pix = gdk_pixbuf_new_from_file_at_size(
            logo_path, 24 * sf, 24 * sf, NULL);
        GtkWidget *logo;             /* the icon-mode child                 */
        if (pix != NULL) {
            cairo_surface_t *surface =
                gdk_cairo_surface_create_from_pixbuf(pix, sf, NULL);
            logo = gtk_image_new_from_surface(surface);
            cairo_surface_destroy(surface);
            g_object_unref(pix);
        } else {
            logo = gtk_label_new("\xf0\x9f\x8d\x8a");    /* 🍊 fallback     */
        }
        GtkWidget *label = gtk_label_new("About");   /* text-mode child     */

        /* Keep owning refs so removal from the button never frees them.    */
        g_object_set_data_full(G_OBJECT(about_item), "on-logo",
                               g_object_ref_sink(logo), g_object_unref);
        g_object_set_data_full(G_OBJECT(about_item), "on-label",
                               g_object_ref_sink(label), g_object_unref);
        g_free(logo_path);
        g_free(base);
    }
    gtk_tool_item_set_tooltip_text(about_item, "About Orange Notes");
    g_signal_connect(about_btn, "clicked", G_CALLBACK(on_about), lw);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), about_item, -1);

    /* Logo only, except in text-only mode — applied now and re-applied on
     * every style switch.                                                  */
    about_button_fit_style(about_item,
                           lw->app->toolbar_style[ON_TOOLBAR_LIBRARY]);
    g_signal_connect(toolbar, "style-changed",
                     G_CALLBACK(on_action_toolbar_style_changed),
                     about_item);

    on_app_register_toolbar(lw->app, ON_TOOLBAR_LIBRARY, toolbar);
    return toolbar;
}

/* ---------------------------------------------------------------------------
 * build_sidebar_toolbar() — the toolbar above the folder/tag tree:
 * New Folder, Rename Folder, Delete Folder, and Search.
 * Returns the toolbar widget.
 * ------------------------------------------------------------------------- */
static GtkWidget *
build_sidebar_toolbar(OnLibrary *lw)
{
    GtkWidget *toolbar = gtk_toolbar_new();
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar),
                              GTK_ICON_SIZE_SMALL_TOOLBAR);

    add_tool_button(lw, toolbar, "list-add", "+\xf0\x9f\x93\x81",
                    "New Folder", "Create a folder inside the selection",
                    G_CALLBACK(on_new_folder));
    add_tool_button(lw, toolbar, "list-remove", "\xe2\x9c\x95",
                    "Delete Folder", "Delete the selected folder",
                    G_CALLBACK(on_delete_folder));
    /* Rename lives in the folder's right-click menu only.                  */

    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    add_tool_button(lw, toolbar, "edit-find", "\xf0\x9f\x94\x8d",
                    "Search", "Search notes",
                    G_CALLBACK(on_open_search));

    on_app_register_toolbar(lw->app, ON_TOOLBAR_LIBRARY, toolbar);
    return toolbar;
}

/* ---------------------------------------------------------------------------
 * sidebar_min_width_update() — make the sidebar at least as wide as its
 * toolbar's natural size plus a little slack, so every button stays
 * visible in any toolbar style (including icons-above-text).
 *   toolbar — the sidebar toolbar.
 *   box     — the sidebar container whose minimum width is being set.
 * ------------------------------------------------------------------------- */
static void
sidebar_min_width_update(GtkWidget *toolbar, GtkWidget *box)
{
    gint min_w, nat_w;               /* the toolbar's width requests        */
    gtk_widget_get_preferred_width(toolbar, &min_w, &nat_w);

    /* Only write on change — this also runs from size-allocate, where an
     * unconditional set_size_request would re-trigger allocation forever.  */
    gint cur_w, cur_h;               /* the box's current size request      */
    gtk_widget_get_size_request(box, &cur_w, &cur_h);
    if (cur_w != nat_w + 5)
        gtk_widget_set_size_request(box, nat_w + 5, -1);
}

/* on_sidebar_toolbar_style_changed() — re-fit the sidebar width whenever
 * the toolbar switches between text/icons/both.                             */
static void
on_sidebar_toolbar_style_changed(GtkToolbar *toolbar,
                                 GtkToolbarStyle style, gpointer user_data)
{
    (void)style;
    sidebar_min_width_update(GTK_WIDGET(toolbar),
                             GTK_WIDGET(user_data));
}

/* on_sidebar_toolbar_size_allocate() — the toolbar's metrics are only
 * final once it is realized and allocated; before that, the preferred
 * width measured at construction comes out too small and the buttons
 * start truncated.  Re-fitting here keeps the minimum correct from the
 * first frame on (the change guard above prevents allocate loops).          */
static void
on_sidebar_toolbar_size_allocate(GtkWidget *toolbar,
                                 GdkRectangle *allocation,
                                 gpointer user_data)
{
    (void)allocation;
    sidebar_min_width_update(toolbar, GTK_WIDGET(user_data));
}

/* ---------------------------------------------------------------------------
 * about_button_fit_style() — the About button shows the centered logo in
 * every toolbar style; the "About" text appears ONLY in text-only mode
 * (where there would otherwise be nothing to click).  The item is a plain
 * GtkToolItem wrapping a GtkButton whose single child gets swapped — a
 * GtkToolButton would reserve empty label space under the icon in
 * icons-above-text mode.  The logo and label widgets live as object data
 * ("on-logo"/"on-label", owning refs) so they survive being unparented.
 *   item  — the About tool item.
 *   style — the toolbar style being applied.
 * ------------------------------------------------------------------------- */
static void
about_button_fit_style(GtkToolItem *item, GtkToolbarStyle style)
{
    GtkWidget *btn   = gtk_bin_get_child(GTK_BIN(item));
    GtkWidget *logo  = g_object_get_data(G_OBJECT(item), "on-logo");
    GtkWidget *label = g_object_get_data(G_OBJECT(item), "on-label");

    GtkWidget *want =                /* the child this style calls for      */
        (style == GTK_TOOLBAR_TEXT) ? label : logo;
    GtkWidget *cur = gtk_bin_get_child(GTK_BIN(btn));
    if (cur == want)
        return;
    if (cur != NULL)
        gtk_container_remove(GTK_CONTAINER(btn), cur);
    gtk_container_add(GTK_CONTAINER(btn), want);
    gtk_widget_show(want);
}

/* on_action_toolbar_style_changed() — keep the About button's label rule
 * applied when the library toolbar style changes.                           */
static void
on_action_toolbar_style_changed(GtkToolbar *toolbar,
                                GtkToolbarStyle style, gpointer user_data)
{
    (void)toolbar;
    about_button_fit_style(GTK_TOOL_ITEM(user_data), style);
}

/* ---------------------------------------------------------------------------
 * on_library_key_press() — window-level shortcuts: Ctrl/Cmd+N creates a
 * note in the currently selected folder.
 * ------------------------------------------------------------------------- */
static gboolean
on_library_key_press(GtkWidget *widget, GdkEventKey *event,
                     gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    if ((event->state & (GDK_CONTROL_MASK | GDK_META_MASK)) &&
        gdk_keyval_to_lower(event->keyval) == GDK_KEY_n) {
        on_new_note(NULL, lw);
        return TRUE;
    }
    return FALSE;
}

/* library_free() — destructor for the OnLibrary attached to the window.     */
static void
library_free(gpointer data)
{
    OnLibrary *lw = data;
    g_hash_table_destroy(lw->thumb_cache);
    g_free(lw->sel_name);
    g_free(lw);
}

GtkWidget *
on_library_window_create(OnApp *app)
{
    OnLibrary *lw = g_new0(OnLibrary, 1);
    lw->app      = app;
    lw->sel_kind = SB_KIND_ROOT;
    lw->sel_id   = 0;
    lw->sel_name = g_strdup("Notes");
    lw->thumb_cache = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, thumb_entry_free);

    /* --- window (standard titlebar, no HeaderBar) ------------------------*/
    lw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(lw->window),
                         app->read_only
                         ? "Orange Notes - Library (Read-Only)"
                         : "Orange Notes - Library");
    gtk_window_set_default_size(GTK_WINDOW(lw->window), 900, 620);
    gtk_application_add_window(app->gtk_app, GTK_WINDOW(lw->window));
    g_object_set_data_full(G_OBJECT(lw->window), "on-library", lw,
                           library_free);

    app->library_window       = lw->window;
    app->notify_notes_changed = library_notify_notes_changed;
    app->notify_note_saved    = library_notify_note_saved;

    g_signal_connect(lw->window, "key-press-event",
                     G_CALLBACK(on_library_key_press), lw);

    /* --- models -----------------------------------------------------------*/
    lw->sidebar_store = gtk_tree_store_new(SB_N_COLS,
                                           G_TYPE_INT,     /* SB_KIND      */
                                           G_TYPE_INT64,   /* SB_ID        */
                                           G_TYPE_STRING,  /* SB_NAME      */
                                           G_TYPE_STRING); /* SB_RAW       */
    lw->notes_store = gtk_list_store_new(
        NL_N_COLS,
        G_TYPE_INT64,                    /* NL_ID                          */
        G_TYPE_STRING,                   /* NL_TITLE                       */
        G_TYPE_STRING,                   /* NL_MODIFIED                    */
        CAIRO_GOBJECT_TYPE_SURFACE,      /* NL_THUMB                       */
        G_TYPE_INT64);                   /* NL_UPDATED                     */
    g_signal_connect(lw->notes_store, "row-deleted",
                     G_CALLBACK(on_notes_row_deleted), lw);

    /* --- sidebar -----------------------------------------------------------*/
    lw->sidebar = GTK_TREE_VIEW(
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(lw->sidebar_store)));
    gtk_tree_view_set_headers_visible(lw->sidebar, FALSE);
    gtk_tree_view_append_column(
        lw->sidebar,
        gtk_tree_view_column_new_with_attributes(
            "Name", gtk_cell_renderer_text_new(),
            "text", SB_NAME, NULL));

    GtkTreeSelection *sb_sel = gtk_tree_view_get_selection(lw->sidebar);
    gtk_tree_selection_set_select_function(sb_sel, sidebar_select_func,
                                           NULL, NULL);
    g_signal_connect(sb_sel, "changed",
                     G_CALLBACK(on_sidebar_selection_changed), lw);

    /* Accept dragged note rows to move notes between folders.              */
    gtk_tree_view_enable_model_drag_dest(lw->sidebar, &ROW_TARGET, 1,
                                         GDK_ACTION_MOVE);
    g_signal_connect(lw->sidebar, "drag-data-received",
                     G_CALLBACK(on_sidebar_drag_received), lw);
    g_signal_connect(lw->sidebar, "button-press-event",
                     G_CALLBACK(on_sidebar_button_press), lw);

    GtkWidget *sidebar_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(sidebar_scroll), FALSE);
    gtk_container_add(GTK_CONTAINER(sidebar_scroll),
                      GTK_WIDGET(lw->sidebar));

    /* Sidebar column: its toolbar on top, the tree below.  The minimum
     * width always fits the toolbar's buttons (+5px), tracked across
     * toolbar-style changes.                                               */
    GtkWidget *sidebar_toolbar = build_sidebar_toolbar(lw);
    GtkWidget *sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(sidebar_box), sidebar_toolbar,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar_box),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar_box), sidebar_scroll,
                       TRUE, TRUE, 0);
    sidebar_min_width_update(sidebar_toolbar, sidebar_box);
    g_signal_connect(sidebar_toolbar, "style-changed",
                     G_CALLBACK(on_sidebar_toolbar_style_changed),
                     sidebar_box);
    g_signal_connect_after(sidebar_toolbar, "size-allocate",
                           G_CALLBACK(on_sidebar_toolbar_size_allocate),
                           sidebar_box);

    /* --- notes list view ---------------------------------------------------*/
    lw->notes_list = GTK_TREE_VIEW(
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(lw->notes_store)));
    {
        /* Each column gets a data func painting the alternating row tint.  */
        GtkCellRenderer *r1 = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *c1 =
            gtk_tree_view_column_new_with_attributes("Title", r1,
                                                     "text", NL_TITLE,
                                                     NULL);
        gtk_tree_view_column_set_cell_data_func(c1, r1, notes_row_bg_func,
                                                NULL, NULL);
        gtk_tree_view_append_column(lw->notes_list, c1);

        GtkCellRenderer *r2 = gtk_cell_renderer_text_new();
        /* Horizontal padding so the timestamps don't hug the column
         * edges.                                                           */
        g_object_set(r2, "xpad", 10, NULL);
        GtkTreeViewColumn *c2 =
            gtk_tree_view_column_new_with_attributes("Modified", r2,
                                                     "text", NL_MODIFIED,
                                                     NULL);
        gtk_tree_view_column_set_cell_data_func(c2, r2, notes_row_bg_func,
                                                NULL, NULL);
        gtk_tree_view_append_column(lw->notes_list, c2);
        gtk_tree_view_column_set_expand(c1, TRUE);

        /* Clickable headers: Title sorts alphabetically, Modified sorts
         * most-recent-first (drag reordering works while unsorted).        */
        gtk_tree_sortable_set_sort_func(
            GTK_TREE_SORTABLE(lw->notes_store), NL_TITLE,
            sort_by_title, NULL, NULL);
        gtk_tree_sortable_set_sort_func(
            GTK_TREE_SORTABLE(lw->notes_store), NL_UPDATED,
            sort_by_updated, NULL, NULL);
        gtk_tree_view_column_set_sort_column_id(c1, NL_TITLE);
        gtk_tree_view_column_set_sort_column_id(c2, NL_UPDATED);
    }
    gtk_tree_selection_set_mode(
        gtk_tree_view_get_selection(lw->notes_list),
        GTK_SELECTION_MULTIPLE);

    /* Built-in drag reordering; the new order is persisted from the
     * model's row-deleted signal.                                          */
    gtk_tree_view_set_reorderable(lw->notes_list, TRUE);
    g_signal_connect(lw->notes_list, "row-activated",
                     G_CALLBACK(on_note_list_activated), lw);
    g_signal_connect(lw->notes_list, "button-press-event",
                     G_CALLBACK(on_notes_list_button_press), lw);

    GtkWidget *list_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(list_scroll), FALSE);
    gtk_container_add(GTK_CONTAINER(list_scroll),
                      GTK_WIDGET(lw->notes_list));

    /* --- notes grid view ---------------------------------------------------*/
    lw->notes_grid = GTK_ICON_VIEW(
        gtk_icon_view_new_with_model(GTK_TREE_MODEL(lw->notes_store)));
    {
        /* Custom cell layout: the HiDPI thumbnail surface with the note
         * title as a real text label underneath.                           */
        GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(lw->notes_grid),
                                   pix, FALSE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(lw->notes_grid),
                                       pix, "surface", NL_THUMB, NULL);

        GtkCellRenderer *txt = gtk_cell_renderer_text_new();
        g_object_set(txt,
                     "xalign",      0.5,
                     "alignment",   PANGO_ALIGN_CENTER,
                     "wrap-mode",   PANGO_WRAP_WORD_CHAR,
                     "wrap-width",  THUMB_SIZE,
                     NULL);
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(lw->notes_grid),
                                   txt, FALSE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(lw->notes_grid),
                                       txt, "text", NL_TITLE, NULL);
    }
    gtk_icon_view_set_item_width(lw->notes_grid, THUMB_SIZE);
    gtk_icon_view_set_selection_mode(lw->notes_grid,
                                     GTK_SELECTION_MULTIPLE);
    gtk_icon_view_enable_model_drag_source(lw->notes_grid,
                                           GDK_BUTTON1_MASK,
                                           &ROW_TARGET, 1,
                                           GDK_ACTION_MOVE);
    g_signal_connect(lw->notes_grid, "item-activated",
                     G_CALLBACK(on_note_grid_activated), lw);
    g_signal_connect(lw->notes_grid, "button-press-event",
                     G_CALLBACK(on_notes_grid_button_press), lw);

    GtkWidget *grid_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(grid_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(grid_scroll), FALSE);
    gtk_container_add(GTK_CONTAINER(grid_scroll),
                      GTK_WIDGET(lw->notes_grid));

    /* --- stack: list <-> grid ----------------------------------------------*/
    lw->stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(lw->stack), list_scroll, "list");
    gtk_stack_add_named(GTK_STACK(lw->stack), grid_scroll, "grid");
    gtk_stack_set_visible_child_name(GTK_STACK(lw->stack), "list");

    /* --- assemble -----------------------------------------------------------*/
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(right), build_action_bar(lw),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), lw->stack, TRUE, TRUE, 0);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(paned), sidebar_box, FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), right, TRUE, FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *menubar = build_menubar(lw);
    /* Remembered so the settings window can move it into the native
     * macOS menu bar (see on_library_apply_native_menubar).                */
    g_object_set_data(G_OBJECT(lw->window), "on-menubar", menubar);
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(lw->window), vbox);

    /* --- initial population -------------------------------------------------*/
    refresh_sidebar(lw);
    refresh_notes(lw);

    gtk_widget_show_all(lw->window);
    return lw->window;
}
