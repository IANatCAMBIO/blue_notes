/* ===========================================================================
 * library_window.c — the Notes Library window (implementation)
 *
 * See library_window.h for the layout overview.  Key mechanics:
 *
 *   sidebar    — a GtkTreeView over a GtkTreeStore holding the sections
 *                "All Notes", the folder hierarchy (rooted at a fixed
 *                "Notes" row), a flat "Tags" section, "Pinned Notes",
 *                and — while non-empty — "Trash" (trashed folders as its
 *                children).  Row kinds are distinguished by the SB_KIND
 *                column.
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
 *                note into a folder.  The sidebar is a drag source too:
 *                a folder row drops INTO a folder (re-nest), BETWEEN
 *                folders (reorder/re-nest beside the sibling), onto
 *                Trash (delete gesture), or out of Trash (restore).
 *                The sidebar's dest protocol is fully custom (quirk #13).
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
    SB_KIND_ALL,                     /* the "All Notes" section             */
    SB_KIND_TRASH,                   /* the "Trash" section                 */
    SB_KIND_TRASH_FOLDER,            /* a trashed folder under Trash        */
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
    NL_PATH,                         /* gchar*: "/Folder/Sub" location      */
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
 *   sidebar_box   — the whole folder/tag pane, so the toolbar's
 *                   show/hide toggle can flip its visibility.
 *   status_path   — status-bar label (bottom left): the path of the
 *                   current sidebar selection.
 *   status_event  — status-bar label (bottom right): the latest event
 *                   message ("DB saved", …); see on_app_status().  Lives
 *                   inside status_revealer (crossfade) and fades out
 *                   after STATUS_FADE_SECONDS via status_timeout.
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
    gboolean      list_autofit;          /* list columns auto-size to their
                                            contents on every refresh (ini
                                            key "list_autofit")            */
    gboolean      notes_sel_blocked;     /* selection changes vetoed for
                                            the span of a press on an
                                            already-selected list row, so
                                            a drag keeps the whole
                                            multi-selection (quirk #15)    */
    GtkTreePath  *notes_press_path;      /* the row that press landed on
                                            (owned): a plain click
                                            collapses to it on release     */
    gint          populating;
    GHashTable   *thumb_cache;
    gint          shown_kind;
    gint64        shown_id;
    GtkWidget    *sidebar_box;
    GtkWidget    *sidebar_paned;         /* horizontal paned holding the sidebar */
    GtkWidget    *status_path;
    GtkWidget    *status_event;
    GtkWidget    *status_revealer;
    guint         status_timeout;
} OnLibrary;

/* How long a status-bar event message stays before fading out.              */
#define STATUS_FADE_SECONDS 4

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
static void    status_path_update(OnLibrary *lw);
static GArray *selected_note_ids(OnLibrary *lw);
static void    list_autofit_set(OnLibrary *lw, PangoLayout *lay,
                                const gchar *key, gint content_w);
static GtkTreePath *notes_sel_unblock(OnLibrary *lw, gboolean want_path);
static void    close_editors_for_ids(OnLibrary *lw, const gint64 *ids,
                                     gsize n);
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
 * `view` (every scrollable view here sits directly in one).  Returns NULL
 * if the parent isn't realized as a GtkScrolledWindow yet.                  */
static GtkAdjustment *
view_vadjustment(GtkWidget *view)
{
    GtkWidget *parent = gtk_widget_get_parent(view);
    if (!GTK_IS_SCROLLED_WINDOW(parent))
        return NULL;
    return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent));
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

/* in_trash_view() — is the notes pane showing Trash contents (the Trash
 * row itself or a trashed folder)?  Deletes there are permanent and the
 * note context menu switches to Restore/Delete Permanently.                 */
static gboolean
in_trash_view(OnLibrary *lw)
{
    return lw->sel_kind == SB_KIND_TRASH ||
           lw->sel_kind == SB_KIND_TRASH_FOLDER;
}

static void
add_folder_rows(OnLibrary *lw, gint64 parent_id, GtkTreeIter *parent_iter,
                GHashTable *note_counts)
{
    GList *folders = on_db_folder_list(lw->app->db, parent_id);
    for (GList *l = folders; l != NULL; l = l->next) {
        OnFolder *f = l->data;       /* one child folder                    */
        gchar *display = lw->app->sidebar_counts    /* name (+ note count)  */
            ? g_strdup_printf("%s (%d)", f->name,
                              count_from_map(note_counts, f->id))
            : g_strdup(f->name);
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

/* sb_row_key() — hashable identity of a sidebar row for state that must
 * survive a model rebuild (paths shift when folders move; kind+id don't). */
static gint64
sb_row_key(gint kind, gint64 id)
{
    return id * 16 + kind;
}

/* SbExpandCtx — working state for the expansion-capture walk below.        */
typedef struct {
    OnLibrary  *lw;
    GHashTable *expanded;                /* set of sb_row_key()s            */
} SbExpandCtx;

/* sb_expand_capture() — gtk_tree_model_foreach() callback: record the
 * key of every currently-expanded row.                                     */
static gboolean
sb_expand_capture(GtkTreeModel *model, GtkTreePath *path,
                  GtkTreeIter *iter, gpointer data)
{
    SbExpandCtx *ctx = data;
    if (gtk_tree_view_row_expanded(ctx->lw->sidebar, path)) {
        gint   kind;                 /* row kind                            */
        gint64 id;                   /* row id                              */
        gtk_tree_model_get(model, iter, SB_KIND, &kind, SB_ID, &id, -1);
        gint64 *key = g_new(gint64, 1);
        *key = sb_row_key(kind, id);
        g_hash_table_add(ctx->expanded, key);
    }
    return FALSE;
}

/* sb_reveal_path() — expand the ANCESTORS of `path` so the row itself is
 * visible (never the row itself: a collapsed selected folder stays
 * collapsed).                                                               */
static void
sb_reveal_path(GtkTreeView *view, GtkTreePath *path)
{
    GtkTreePath *parent = gtk_tree_path_copy(path);
    if (gtk_tree_path_up(parent) && gtk_tree_path_get_depth(parent) > 0)
        gtk_tree_view_expand_to_path(view, parent);
    gtk_tree_path_free(parent);
}

/* ---------------------------------------------------------------------------
 * refresh_sidebar() — rebuild the whole sidebar model: the folder tree
 * under the fixed "Notes" root, then the "Tags" section.  Attempts to
 * restore the previous selection by (kind, id), and puts back which rows
 * were expanded (only the very first population expands everything) —
 * a drop used to re-expand every folder because every successful drag
 * refreshes the sidebar.
 * ------------------------------------------------------------------------- */
static void
refresh_sidebar(OnLibrary *lw)
{
    gint   want_kind = lw->sel_kind; /* selection to restore                */
    gint64 want_id   = lw->sel_id;

    /* Capture the expansion state (keyed by kind+id, which survive the
     * rebuild) before the clear wipes it.                                  */
    SbExpandCtx ectx = {
        lw, g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                  g_free, NULL) };
    GtkTreeIter first;               /* is the model populated at all?      */
    gboolean first_build = !gtk_tree_model_get_iter_first(
        GTK_TREE_MODEL(lw->sidebar_store), &first);
    gtk_tree_model_foreach(GTK_TREE_MODEL(lw->sidebar_store),
                           sb_expand_capture, &ectx);

    /* Clearing the store zeroes the sidebar scrollbar; a sidebar rebuild
     * is never a navigation (counts changed, a folder was added, …), so
     * the position is always put back.                                     */
    GtkAdjustment *vadj      = view_vadjustment(GTK_WIDGET(lw->sidebar));
    gdouble        scroll_pos = vadj ? gtk_adjustment_get_value(vadj) : 0.0;

    lw->populating++;
    gtk_tree_store_clear(lw->sidebar_store);

    /* Batched counts: one query for all folders, one for all tags —
     * refresh_sidebar runs after every autosave, so per-row COUNT
     * queries added up (especially against a shared/networked db).         */
    GHashTable *note_counts = on_db_note_count_map(lw->app->db);
    GHashTable *tag_counts  = on_db_tag_count_map(lw->app->db);

    /* "All Notes" — a live view of every note outside the Trash.           */
    gchar *all_label = lw->app->sidebar_counts
        ? g_strdup_printf("All Notes (%d)",
                          on_db_note_count_visible(lw->app->db))
        : g_strdup("All Notes");
    GtkTreeIter all_iter;            /* the fixed "All Notes" row           */
    gtk_tree_store_append(lw->sidebar_store, &all_iter, NULL);
    gtk_tree_store_set(lw->sidebar_store, &all_iter,
                       SB_KIND, SB_KIND_ALL,
                       SB_ID,   (gint64)0,
                       SB_NAME, all_label,
                       SB_RAW,  "All Notes",
                       -1);
    g_free(all_label);

    /* "Notes" root — selecting it shows the top-level notes.               */
    gchar *root_label = lw->app->sidebar_counts
        ? g_strdup_printf("Notes (%d)", count_from_map(note_counts, 0))
        : g_strdup("Notes");
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
            gchar *label = lw->app->sidebar_counts
                ? g_strdup_printf("#%s (%d)", t->name,
                                  count_from_map(tag_counts, t->id))
                : g_strdup(raw);
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
        gchar *label = lw->app->sidebar_counts
            ? g_strdup_printf("Pinned Notes (%d)", n_pinned)
            : g_strdup("Pinned Notes");
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

    /* "Trash" at the bottom, only while it holds something.  Selecting
     * it lists the directly-trashed notes; trashed folders hang under it
     * as browsable children (their subtrees stay hidden until restore).    */
    gint n_trash = on_db_trash_count(lw->app->db);
    if (n_trash > 0) {
        gchar *label = lw->app->sidebar_counts
            ? g_strdup_printf("Trash (%d)", n_trash)
            : g_strdup("Trash");
        GtkTreeIter trash_iter;      /* the "Trash" section row             */
        gtk_tree_store_append(lw->sidebar_store, &trash_iter, NULL);
        gtk_tree_store_set(lw->sidebar_store, &trash_iter,
                           SB_KIND, SB_KIND_TRASH,
                           SB_ID,   (gint64)0,
                           SB_NAME, label,
                           SB_RAW,  "Trash",
                           -1);
        g_free(label);

        GList *trashed = on_db_folder_list_trashed(lw->app->db);
        for (GList *l = trashed; l != NULL; l = l->next) {
            OnFolder *f = l->data;   /* one trashed folder                  */
            gchar *display = lw->app->sidebar_counts
                ? g_strdup_printf("%s (%d)", f->name,
                                  count_from_map(note_counts, f->id))
                : g_strdup(f->name);
            GtkTreeIter iter;
            gtk_tree_store_append(lw->sidebar_store, &iter, &trash_iter);
            gtk_tree_store_set(lw->sidebar_store, &iter,
                               SB_KIND, SB_KIND_TRASH_FOLDER,
                               SB_ID,   f->id,
                               SB_NAME, display,
                               SB_RAW,  f->name,
                               -1);
            g_free(display);
        }
        on_db_folder_list_free(trashed);
    }

    g_hash_table_destroy(note_counts);
    g_hash_table_destroy(tag_counts);

    /* Only the very first population expands everything; every later
     * rebuild restores the captured expansion state in the walk below.     */
    if (first_build)
        gtk_tree_view_expand_all(lw->sidebar);

    /* Restore the previous selection, falling back to "All Notes".  The
     * populating guard stays up through the restore: the select_iter
     * below would otherwise fire the changed handler and rebuild the
     * notes pane a second time — every refresh_sidebar caller already
     * pairs it with an explicit refresh_notes.                             */
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
        GtkTreePath *row_path = gtk_tree_model_get_path(
            GTK_TREE_MODEL(lw->sidebar_store), &iter);

        /* Re-expand rows that were expanded before the rebuild.            */
        gint64 ekey = sb_row_key(kind, id);
        if (g_hash_table_contains(ectx.expanded, &ekey))
            gtk_tree_view_expand_row(lw->sidebar, row_path, FALSE);

        if (kind == want_kind && id == want_id && !restored) {
            /* The suppressed handler would have refreshed sel_name; do it
             * here so a renamed folder/tag keeps it current.               */
            gchar *raw = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(lw->sidebar_store), &iter,
                               SB_RAW, &raw, -1);
            g_free(lw->sel_name);
            lw->sel_name = raw;      /* ownership transferred               */
            sb_reveal_path(lw->sidebar, row_path);
            gtk_tree_selection_select_iter(sel, &iter);
            restored = TRUE;         /* no break: the walk must finish
                                        restoring the expansion state       */
        }
        gtk_tree_path_free(row_path);
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
    g_hash_table_destroy(ectx.expanded);

    if (!restored) {
        /* The first row is the "All Notes" section — the state must match
         * the row the fallback highlights.                                 */
        lw->sel_kind = SB_KIND_ALL;
        lw->sel_id   = 0;
        g_free(lw->sel_name);
        lw->sel_name = g_strdup("All Notes");
        if (gtk_tree_model_get_iter_first(
                GTK_TREE_MODEL(lw->sidebar_store), &iter))
            gtk_tree_selection_select_iter(sel, &iter);
    }
    lw->populating--;

    /* The old selection no longer exists (deleted folder/pruned tag), so
     * the notes pane still shows its contents: refresh for the new
     * fallback selection.  When the selection was restored, the caller's
     * own refresh_notes covers it.                                         */
    if (!restored)
        refresh_notes(lw);

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
    GtkWidget     *vis_child = gtk_stack_get_visible_child(GTK_STACK(lw->stack));
    GtkAdjustment *vadj      = GTK_IS_SCROLLED_WINDOW(vis_child)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(vis_child))
        : NULL;
    gdouble scroll_pos = vadj ? gtk_adjustment_get_value(vadj) : 0.0;

    lw->populating++;
    gtk_list_store_clear(lw->notes_store);

    /* Pick the note list matching the selection.                           */
    GList *notes;                    /* OnNoteMeta* list to display         */
    if (lw->sel_kind == SB_KIND_TAG)
        notes = on_db_notes_by_tag(lw->app->db, lw->sel_id);
    else if (lw->sel_kind == SB_KIND_PINNED)
        notes = on_db_note_list_pinned(lw->app->db);
    else if (lw->sel_kind == SB_KIND_ALL)
        notes = on_db_note_list_recent(lw->app->db);
    else if (lw->sel_kind == SB_KIND_TRASH)
        notes = on_db_note_list_trashed(lw->app->db);
    else                             /* root, folder, or trashed folder     */
        notes = on_db_note_list(lw->app->db, lw->sel_id);

    /* Folder paths for the list view's Path column — ONE query for all
     * folders, never per note (shared/network DBs).                        */
    GHashTable *paths = on_db_folder_path_map(lw->app->db);

    /* Autofit measuring rides this population loop (no second model
     * walk, no re-fetching the strings) and only while the LIST is the
     * visible view — the grid doesn't show these columns, and switching
     * back to list re-measures (on_view_list).  Repeated folder paths
     * are measured once, keyed by folder id.                               */
    gboolean     fit      = lw->list_autofit && !want_thumbs;
    PangoLayout *fit_lay  = NULL;    /* reused measuring layout             */
    GHashTable  *fit_seen = NULL;    /* folder id → measured path width     */
    gint fit_path_w = 0, fit_mod_w = 0;  /* running content maxima          */
    if (fit) {
        fit_lay  = gtk_widget_create_pango_layout(
            GTK_WIDGET(lw->notes_list), NULL);
        fit_seen = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                         g_free, NULL);
    }

    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;     /* one note                            */

        /* Format the modification time like "Jun 3, 2026 14:05".           */
        GDateTime *dt = g_date_time_new_from_unix_local(m->updated_at);
        gchar *when = g_date_time_format(dt, "%b %e, %Y %H:%M");
        g_date_time_unref(dt);

        /* "/Folder/Sub" location, "/" for the top level — the same
         * format as the status bar's path label.                           */
        const gchar *fpath = m->folder_id != 0
            ? g_hash_table_lookup(paths, &m->folder_id) : NULL;
        gchar *where = g_strdup_printf("/%s", fpath != NULL ? fpath : "");

        if (fit) {
            gint w;                  /* width of the current string         */
            gpointer cached =
                g_hash_table_lookup(fit_seen, &m->folder_id);
            if (cached != NULL) {
                w = GPOINTER_TO_INT(cached);
            } else {
                pango_layout_set_text(fit_lay, where, -1);
                pango_layout_get_pixel_size(fit_lay, &w, NULL);
                gint64 *k = g_new(gint64, 1);
                *k = m->folder_id;
                g_hash_table_insert(fit_seen, k, GINT_TO_POINTER(w));
            }
            fit_path_w = MAX(fit_path_w, w);

            pango_layout_set_text(fit_lay, when, -1);
            pango_layout_get_pixel_size(fit_lay, &w, NULL);
            fit_mod_w = MAX(fit_mod_w, w);
        }

        GtkTreeIter iter;
        gtk_list_store_append(lw->notes_store, &iter);
        gtk_list_store_set(lw->notes_store, &iter,
                           NL_ID,       m->id,
                           NL_TITLE,    m->title,
                           NL_MODIFIED, when,
                           NL_THUMB,    want_thumbs ? get_note_thumb(lw, m)
                                                    : NULL,
                           NL_UPDATED,  m->updated_at,
                           NL_PATH,     where,
                           -1);
        g_free(where);
        g_free(when);
    }
    g_hash_table_destroy(paths);
    on_db_note_list_free(notes);

    if (fit) {
        list_autofit_set(lw, fit_lay, "path",     fit_path_w);
        list_autofit_set(lw, fit_lay, "modified", fit_mod_w);
        g_hash_table_destroy(fit_seen);
        g_object_unref(fit_lay);
    }
    lw->populating--;

    lw->shown_kind = lw->sel_kind;
    lw->shown_id   = lw->sel_id;
    if (keep_scroll && scroll_pos > 0)
        scroll_keep_queue(vadj, scroll_pos);

    status_path_update(lw);
}

/* ---------------------------------------------------------------------------
 * persist_note_order() — write the notes model's current row order back
 * to the database.  Only meaningful when a folder (not a tag) is shown.
 * ------------------------------------------------------------------------- */
static void
persist_note_order(OnLibrary *lw)
{
    /* Only real folder views own an ordering; the tag, pinned, all-notes
     * and trash views are assembled from elsewhere (persisting a drag in
     * All Notes would scramble every folder's internal order).             */
    if (lw->sel_kind == SB_KIND_TAG || lw->sel_kind == SB_KIND_PINNED ||
        lw->sel_kind == SB_KIND_ALL || in_trash_view(lw))
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
 * drag & drop: note → folder, folder → folder
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * folder_move_beside() — re-parent folder `folder_id` under `new_parent`
 * and slot it directly before/after sibling `anchor_id` (the row the drop
 * indicator pointed at).  The move appends at the end of the new parent;
 * the reorder then writes the full sibling sequence with the moved folder
 * re-inserted at the anchor.
 * ------------------------------------------------------------------------- */
static gboolean
folder_move_beside(OnLibrary *lw, gint64 folder_id, gint64 new_parent,
                   gint64 anchor_id, gboolean after)
{
    if (!on_db_folder_move(lw->app->db, folder_id, new_parent))
        return FALSE;

    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    GList *sibs = on_db_folder_list(lw->app->db, new_parent);
    for (GList *l = sibs; l != NULL; l = l->next) {
        OnFolder *f = l->data;       /* one sibling                         */
        if (f->id == folder_id)
            continue;                /* re-inserted at the anchor below     */
        if (f->id == anchor_id && !after)
            g_array_append_val(ids, folder_id);
        g_array_append_val(ids, f->id);
        if (f->id == anchor_id && after)
            g_array_append_val(ids, folder_id);
    }
    on_db_folder_list_free(sibs);

    gboolean ok = on_db_folder_reorder(lw->app->db,
                                       (const gint64 *)ids->data, ids->len);
    g_array_free(ids, TRUE);
    return ok;
}

/* ---------------------------------------------------------------------------
 * sidebar_drop_target() — resolve and validate the drop target under the
 * pointer for the drag in `context`.  Which rows are legal depends on the
 * drag's SOURCE widget: from the sidebar itself only folder rows move
 * (onto folders, the root, or the Trash, never onto themselves or into
 * their own subtree — the dragged row is the sidebar's selected row,
 * since a button press always moves the single-mode selection before the
 * drag threshold is crossed); from the notes views any folder-ish row
 * accepts, and the position is coerced to INTO (a note drops *into* a
 * folder, never beside it).
 * Returns TRUE and fills `path_out` (caller frees) + `pos_out` when the
 * drop is legal.
 * ------------------------------------------------------------------------- */
static gboolean
sidebar_drop_target(OnLibrary *lw, GdkDragContext *context, gint x, gint y,
                    GtkTreePath **path_out, GtkTreeViewDropPosition *pos_out)
{
    *path_out = NULL;
    *pos_out  = GTK_TREE_VIEW_DROP_BEFORE;

    if (gtk_drag_dest_find_target(GTK_WIDGET(lw->sidebar), context,
                                  NULL) == GDK_NONE)
        return FALSE;                /* not a GTK_TREE_MODEL_ROW drag       */

    GtkTreePath *path = NULL;        /* row under the pointer               */
    GtkTreeViewDropPosition pos;     /* before/into/after                   */
    if (!gtk_tree_view_get_dest_row_at_pos(lw->sidebar, x, y, &path, &pos))
        return FALSE;

    GtkTreeModel *model = GTK_TREE_MODEL(lw->sidebar_store);
    GtkTreeIter iter;                /* target row                          */
    gint kind = -1;                  /* its kind                            */
    if (gtk_tree_model_get_iter(model, &iter, path))
        gtk_tree_model_get(model, &iter, SB_KIND, &kind, -1);

    gboolean ok = FALSE;             /* is this drop legal?                 */
    if (gtk_drag_get_source_widget(context) == GTK_WIDGET(lw->sidebar)) {
        /* --- folder drag ---------------------------------------------------*/
        GtkTreeIter  src_iter;       /* dragged (= selected) row            */
        GtkTreeModel *m;
        gint  src_kind = -1;         /* its kind                            */
        GtkTreePath *src_path = NULL;
        if (gtk_tree_selection_get_selected(
                gtk_tree_view_get_selection(lw->sidebar), &m, &src_iter)) {
            gtk_tree_model_get(m, &src_iter, SB_KIND, &src_kind, -1);
            src_path = gtk_tree_model_get_path(m, &src_iter);
        }
        if ((src_kind == SB_KIND_FOLDER ||
             src_kind == SB_KIND_TRASH_FOLDER) &&
            src_path != NULL &&
            gtk_tree_path_compare(src_path, path) != 0 &&
            !gtk_tree_path_is_descendant(path, src_path)) {
            if (kind == SB_KIND_FOLDER) {
                ok = TRUE;           /* nest INTO or reorder beside it      */
            } else if (kind == SB_KIND_ROOT ||
                       (kind == SB_KIND_TRASH &&
                        src_kind == SB_KIND_FOLDER)) {
                ok  = TRUE;
                pos = GTK_TREE_VIEW_DROP_INTO_OR_BEFORE;
            }
        }
        if (src_path != NULL)
            gtk_tree_path_free(src_path);
    } else {
        /* --- note drag -----------------------------------------------------*/
        if (kind == SB_KIND_FOLDER || kind == SB_KIND_ROOT ||
            kind == SB_KIND_TRASH) {
            ok  = TRUE;
            pos = GTK_TREE_VIEW_DROP_INTO_OR_BEFORE;
        }
    }

    if (ok) {
        *path_out = path;
        *pos_out  = pos;
    } else {
        gtk_tree_path_free(path);
    }
    return ok;
}

/* ---------------------------------------------------------------------------
 * on_sidebar_drag_motion() — answer every motion with gdk_drag_status()
 * and draw the drop indicator ourselves.  This REPLACES GtkTreeView's
 * default handler (returning TRUE stops the class closure), which for
 * GTK_TREE_MODEL_ROW targets requests the drag DATA on every motion to
 * validate the drop — those requests fire "drag-data-received" mid-drag,
 * and on quartz the reply arrives before the drop, so a received handler
 * that treats every delivery as a drop runs with stale coordinates and
 * gtk_drag_finish()es the drag while the button is still down (drops
 * landing only when the X11-style late reply happens to slip past the
 * release).  See quirk #13.
 * ------------------------------------------------------------------------- */
static gboolean
on_sidebar_drag_motion(GtkWidget *widget, GdkDragContext *context,
                       gint x, gint y, guint time, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreePath *path = NULL;        /* legal target row (or NULL)          */
    GtkTreeViewDropPosition pos;     /* indicator position                  */
    gboolean ok = sidebar_drop_target(lw, context, x, y, &path, &pos);

    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(widget),
                                    ok ? path : NULL, pos);
    gdk_drag_status(context, ok ? GDK_ACTION_MOVE : 0, time);
    if (path != NULL)
        gtk_tree_path_free(path);
    return TRUE;
}

/* on_sidebar_drag_leave() — clear the drop indicator (also fires right
 * before every drop; the class handler additionally kills its timers).     */
static void
on_sidebar_drag_leave(GtkWidget *widget, GdkDragContext *context,
                      guint time, gpointer user_data)
{
    (void)context; (void)time; (void)user_data;
    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(widget), NULL,
                                    GTK_TREE_VIEW_DROP_BEFORE);
}

/* ---------------------------------------------------------------------------
 * on_sidebar_drag_drop() — the button was released on a legal target:
 * request the row data (the actual move runs in drag-data-received, the
 * only place the dragged row can be decoded).  Returning TRUE keeps the
 * default handler out; FALSE (no legal target) cancels the drop cleanly.
 * ------------------------------------------------------------------------- */
static gboolean
on_sidebar_drag_drop(GtkWidget *widget, GdkDragContext *context,
                     gint x, gint y, guint time, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreePath *path = NULL;        /* legal target row (or NULL)          */
    GtkTreeViewDropPosition pos;     /* unused here                         */
    gboolean ok = sidebar_drop_target(lw, context, x, y, &path, &pos);
    if (path != NULL)
        gtk_tree_path_free(path);
    if (!ok)
        return FALSE;
    gtk_drag_get_data(widget, context,
                      gdk_atom_intern_static_string("GTK_TREE_MODEL_ROW"),
                      time);
    return TRUE;
}

/* Logical pixel size of the custom drag-under-cursor icons.                 */
#define DRAG_ICON_SIZE 32

/* ---------------------------------------------------------------------------
 * on_sidebar_drag_begin() — replace the drag-under-cursor icon of a
 * folder drag with the folder glyph (non-folder rows keep the default
 * row snapshot).  Connected AFTER the class handler, which would
 * otherwise override our icon with its own.
 * ------------------------------------------------------------------------- */
static void
on_sidebar_drag_begin(GtkWidget *widget, GdkDragContext *context,
                      gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */

    GtkTreeModel *m;                 /* the sidebar model                   */
    GtkTreeIter iter;                /* dragged (= selected) row            */
    gint kind = -1;                  /* its kind                            */
    if (gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)), &m, &iter))
        gtk_tree_model_get(m, &iter, SB_KIND, &kind, -1);
    if (kind != SB_KIND_FOLDER && kind != SB_KIND_TRASH_FOLDER)
        return;

    cairo_surface_t *icon =
        on_app_icon_surface(lw->app, "folder", DRAG_ICON_SIZE);
    if (icon != NULL) {
        gtk_drag_set_icon_surface(context, icon);
        cairo_surface_destroy(icon);
    }
}

/* ---------------------------------------------------------------------------
 * on_notes_drag_begin() — drag-under-cursor icon for note drags (both
 * views): one file for a single note, a document stack for several.
 * The press that started the drag has already settled the selection, so
 * its count IS the drag count.  Connected AFTER the class handler, as
 * above.
 * ------------------------------------------------------------------------- */
static void
on_notes_drag_begin(GtkWidget *widget, GdkDragContext *context,
                    gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */

    /* A drag from a blocked press keeps the whole multi-selection: just
     * lift the veto — the matching release lands in the DnD machinery,
     * never as a button-release on the view.                               */
    notes_sel_unblock(lw, FALSE);

    GArray *ids = selected_note_ids(lw);
    cairo_surface_t *icon = on_app_icon_surface(
        lw->app, ids->len > 1 ? "documents" : "file", DRAG_ICON_SIZE);
    g_array_free(ids, TRUE);
    if (icon != NULL) {
        gtk_drag_set_icon_surface(context, icon);
        cairo_surface_destroy(icon);
    }
}

/* ---------------------------------------------------------------------------
 * on_sidebar_drag_received() — a row was dropped on the sidebar: either a
 * notes row (move the note — or the whole multi-selection — into the
 * target folder, or trash it) or one of the sidebar's own folder rows
 * (re-nest INTO a folder, reorder BEFORE/AFTER a sibling, trash, or
 * restore-by-drag out of the Trash).  Fires exactly once per drop — only
 * on_sidebar_drag_drop() requests the data (the default motion-time
 * requests never happen, see on_sidebar_drag_motion), so x/y are the real
 * drop coordinates.  The default GtkTreeView handler is suppressed
 * because it would try to splice the dragged row into the *sidebar*
 * model.
 * ------------------------------------------------------------------------- */
static void
on_sidebar_drag_received(GtkWidget *widget, GdkDragContext *context,
                         gint x, gint y, GtkSelectionData *seldata,
                         guint info, guint time, gpointer user_data)
{
    (void)info;
    OnLibrary *lw = user_data;       /* owning library window               */
    g_signal_stop_emission_by_name(widget, "drag-data-received");

    gboolean success = FALSE;        /* whether anything moved              */

    /* Decode the dragged row.                                              */
    GtkTreeModel *src_model = NULL;  /* model the drag started in           */
    GtkTreePath  *src_path  = NULL;  /* dragged row's path                  */
    if (!gtk_tree_get_row_drag_data(seldata, &src_model, &src_path)) {
        gtk_drag_finish(context, FALSE, FALSE, time);
        return;
    }

    /* Resolve the drop target row (shared by both branches below).         */
    GtkTreeModel *sb_model  = GTK_TREE_MODEL(lw->sidebar_store);
    GtkTreePath  *dest_path = NULL;  /* row under the pointer               */
    GtkTreeViewDropPosition pos = GTK_TREE_VIEW_DROP_BEFORE;
    GtkTreeIter dest_iter;           /* target sidebar row                  */
    gint   dest_kind = -1;           /* its kind (-1 = no target row)       */
    gint64 dest_id   = 0;            /* its folder/tag id                   */
    gchar *dest_raw  = NULL;         /* its bare name                       */
    if (gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(widget),
                                          x, y, &dest_path, &pos) &&
        gtk_tree_model_get_iter(sb_model, &dest_iter, dest_path))
        gtk_tree_model_get(sb_model, &dest_iter,
                           SB_KIND, &dest_kind, SB_ID, &dest_id,
                           SB_RAW,  &dest_raw, -1);

    if (src_model == GTK_TREE_MODEL(lw->notes_store) && dest_kind != -1) {
        /* --- a note row from the notes pane -------------------------------*/
        GtkTreeIter src_iter;        /* dragged note row                    */
        gint64 note_id = 0;          /* dragged note id                     */
        if (gtk_tree_model_get_iter(src_model, &src_iter, src_path))
            gtk_tree_model_get(src_model, &src_iter, NL_ID, &note_id, -1);

        if (note_id != 0 &&
            (dest_kind == SB_KIND_FOLDER || dest_kind == SB_KIND_ROOT ||
             dest_kind == SB_KIND_TRASH)) {
            /* Multi-select support: when the dragged note is part of
             * the current selection, the whole selection moves.            */
            GArray *ids = selected_note_ids(lw);
            gboolean drag_in_selection = FALSE;
            for (guint i = 0; i < ids->len; i++)
                if (g_array_index(ids, gint64, i) == note_id)
                    drag_in_selection = TRUE;
            if (!drag_in_selection) {
                g_array_set_size(ids, 0);
                g_array_append_val(ids, note_id);
            }
            if (dest_kind == SB_KIND_TRASH) {
                /* Dropping on Trash IS the delete gesture.                 */
                close_editors_for_ids(lw, (const gint64 *)ids->data,
                                      ids->len);
                success = on_db_notes_trash(
                    lw->app->db, (const gint64 *)ids->data, ids->len);
                if (success)
                    on_app_status(lw->app, "Moved %u note%s to Trash",
                                  ids->len, ids->len == 1 ? "" : "s");
            } else {
                /* ONE transaction for the whole selection: per-note
                 * moves fsync per call and froze the GUI on big drops.  */
                success = on_db_notes_move(
                    lw->app->db, (const gint64 *)ids->data, ids->len,
                    dest_id);
                if (success)
                    on_app_status(lw->app,
                                  "Moved %u note%s to "
                                  "\xe2\x80\x9c%s\xe2\x80\x9d",
                                  ids->len, ids->len == 1 ? "" : "s",
                                  dest_raw);
            }
            g_array_free(ids, TRUE);
        }
    } else if (src_model == sb_model && dest_kind != -1) {
        /* --- one of the sidebar's own folder rows --------------------------*/
        GtkTreeIter src_iter;        /* dragged sidebar row                 */
        gint   src_kind  = -1;       /* its kind                            */
        gint64 folder_id = 0;        /* its folder id                       */
        gchar *fname     = NULL;     /* its bare name                       */
        if (gtk_tree_model_get_iter(src_model, &src_iter, src_path))
            gtk_tree_model_get(src_model, &src_iter,
                               SB_KIND, &src_kind, SB_ID, &folder_id,
                               SB_RAW,  &fname, -1);

        /* Only real folders move (trashed ones too — that's drag-restore);
         * a drop onto the row itself or into its own subtree is refused
         * (on_db_folder_move re-checks against the database).              */
        gboolean movable =
            (src_kind == SB_KIND_FOLDER ||
             src_kind == SB_KIND_TRASH_FOLDER) &&
            folder_id != 0 &&
            gtk_tree_path_compare(src_path, dest_path) != 0 &&
            !gtk_tree_path_is_descendant(dest_path, src_path);

        if (movable && dest_kind == SB_KIND_TRASH &&
            src_kind == SB_KIND_FOLDER) {
            /* Dropping on Trash IS the delete gesture.                     */
            GArray *nids = on_db_folder_note_ids(lw->app->db, folder_id);
            close_editors_for_ids(lw, (const gint64 *)nids->data,
                                  nids->len);
            g_array_free(nids, TRUE);
            success = on_db_folder_trash(lw->app->db, folder_id);
            if (success) {
                on_app_status(lw->app,
                              "Folder \xe2\x80\x9c%s\xe2\x80\x9d moved "
                              "to Trash", fname);
                if (lw->sel_kind == SB_KIND_FOLDER &&
                    lw->sel_id == folder_id) {
                    lw->sel_kind = SB_KIND_ROOT;
                    lw->sel_id   = 0;
                }
            }
        } else if (movable && (dest_kind == SB_KIND_FOLDER ||
                               dest_kind == SB_KIND_ROOT)) {
            /* INTO a folder (or anywhere on the root) re-nests, appended
             * at the end; BEFORE/AFTER a folder row slots the dragged
             * folder beside that sibling (re-nesting when the sibling
             * lives under a different parent).                             */
            gchar *where = NULL;     /* name for the status message         */
            if (dest_kind == SB_KIND_ROOT ||
                pos == GTK_TREE_VIEW_DROP_INTO_OR_BEFORE ||
                pos == GTK_TREE_VIEW_DROP_INTO_OR_AFTER) {
                gint64 new_parent =  /* root drops land at top level        */
                    (dest_kind == SB_KIND_FOLDER) ? dest_id : 0;
                success = on_db_folder_move(lw->app->db, folder_id,
                                            new_parent);
                where = g_strdup(dest_raw);
            } else {
                /* The dest folder's parent row is the new parent: the
                 * "Notes" root maps to top level (0).                      */
                GtkTreeIter par_iter;        /* dest's parent row           */
                gint   par_kind = SB_KIND_ROOT;
                gint64 par_id   = 0;
                gchar *par_raw  = NULL;
                if (gtk_tree_model_iter_parent(sb_model, &par_iter,
                                               &dest_iter))
                    gtk_tree_model_get(sb_model, &par_iter,
                                       SB_KIND, &par_kind,
                                       SB_ID,   &par_id,
                                       SB_RAW,  &par_raw, -1);
                gint64 new_parent =
                    (par_kind == SB_KIND_FOLDER) ? par_id : 0;
                success = folder_move_beside(
                    lw, folder_id, new_parent, dest_id,
                    pos == GTK_TREE_VIEW_DROP_AFTER);
                where = par_raw ? par_raw : g_strdup("Notes");
            }
            if (success) {
                on_app_status(lw->app,
                              src_kind == SB_KIND_TRASH_FOLDER
                                  ? "Folder \xe2\x80\x9c%s\xe2\x80\x9d "
                                    "restored to \xe2\x80\x9c%s\xe2\x80\x9d"
                                  : "Moved folder \xe2\x80\x9c%s\xe2\x80\x9d "
                                    "to \xe2\x80\x9c%s\xe2\x80\x9d",
                              fname, where);
                /* A trashed folder dragged out re-enters the tree as a
                 * normal folder; keep the selection on it.                 */
                if (lw->sel_kind == SB_KIND_TRASH_FOLDER &&
                    lw->sel_id == folder_id)
                    lw->sel_kind = SB_KIND_FOLDER;
            }
            g_free(where);
        }
        g_free(fname);
    }

    g_free(dest_raw);
    if (dest_path != NULL)
        gtk_tree_path_free(dest_path);
    gtk_tree_path_free(src_path);

    /* Finish the DnD handshake FIRST — the refreshes below rebuild both
     * models, and running them before gtk_drag_finish() stalls the drop
     * (the source sits waiting while we repaint).                          */
    gtk_drag_finish(context, success, FALSE, time);
    if (success) {
        refresh_sidebar(lw);         /* tree shape and counts changed       */
        refresh_notes(lw);
    }
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

    /* Warning icon on top (custom PNG; ⚠ as fallback).                     */
    GtkWidget *icon = on_app_icon_image_sized(lw->app, "warning", 32);
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
 * close_editors_for_ids() — destroy any open editor window for the notes
 * in `ids`; their destroy handlers flush pending autosaves first.
 * ------------------------------------------------------------------------- */
static void
close_editors_for_ids(OnLibrary *lw, const gint64 *ids, gsize n)
{
    for (gsize i = 0; i < n; i++) {
        gint64 note_id = ids[i];
        GtkWidget *editor =
            g_hash_table_lookup(lw->app->editors, &note_id);
        if (editor != NULL)
            gtk_widget_destroy(editor);
    }
}

/* ---------------------------------------------------------------------------
 * trash_notes() — move every note in `ids` to the Trash (closing any open
 * editors first).  No confirmation: the move is reversible, so a status
 * message is enough.
 * ------------------------------------------------------------------------- */
static void
trash_notes(OnLibrary *lw, GArray *ids)
{
    if (ids->len == 0)
        return;
    close_editors_for_ids(lw, (const gint64 *)ids->data, ids->len);
    if (on_db_notes_trash(lw->app->db, (const gint64 *)ids->data,
                          ids->len)) {
        on_app_status(lw->app, "Moved %u note%s to Trash",
                      ids->len, ids->len == 1 ? "" : "s");
        refresh_notes(lw);
        refresh_sidebar(lw);         /* counts + the Trash section          */
    }
}

/* ---------------------------------------------------------------------------
 * delete_notes_permanently() — confirm once, then permanently delete every
 * note in `ids` (closing any open editors first).  This is the Trash-view
 * delete; normal views go through trash_notes().
 * ------------------------------------------------------------------------- */
static void
delete_notes_permanently(OnLibrary *lw, GArray *ids)
{
    if (ids->len == 0)
        return;
    gchar *question = (ids->len == 1)
        ? g_strdup("Permanently delete this note?")
        : g_strdup_printf("Permanently delete these %u notes?", ids->len);
    gboolean ok = confirm(lw, question, "This cannot be undone.");
    g_free(question);
    if (!ok)
        return;

    close_editors_for_ids(lw, (const gint64 *)ids->data, ids->len);
    on_db_notes_delete(lw->app->db, (const gint64 *)ids->data, ids->len);
    refresh_notes(lw);
    refresh_sidebar(lw);             /* tag list/counts may have changed    */
}

/* on_delete_note() — action-bar Delete: trash every selected note, or
 * permanently delete it when the Trash is what's being viewed.              */
static void
on_delete_note(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    GArray *ids = selected_note_ids(lw);
    if (in_trash_view(lw))
        delete_notes_permanently(lw, ids);
    else
        trash_notes(lw, ids);
    g_array_free(ids, TRUE);
}

/* on_delete_folder() — sidebar-toolbar Delete: move the selected folder
 * (with its whole subtree) to the Trash; a folder already in the Trash is
 * permanently deleted instead (after confirmation).                         */
static void
on_delete_folder(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->sel_kind == SB_KIND_FOLDER) {
        if (on_db_folder_trash(lw->app->db, lw->sel_id)) {
            on_app_status(lw->app,
                          "Folder \xe2\x80\x9c%s\xe2\x80\x9d moved to Trash",
                          lw->sel_name);
            lw->sel_kind = SB_KIND_ROOT;
            lw->sel_id   = 0;
            refresh_sidebar(lw);
            refresh_notes(lw);
        }
    } else if (lw->sel_kind == SB_KIND_TRASH_FOLDER) {
        if (confirm(lw,
                    "Permanently delete this folder and everything "
                    "inside it?",
                    "This cannot be undone.")) {
            GArray *ids = on_db_folder_note_ids(lw->app->db, lw->sel_id);
            close_editors_for_ids(lw, (const gint64 *)ids->data, ids->len);
            g_array_free(ids, TRUE);
            on_db_folder_delete(lw->app->db, lw->sel_id);
            lw->sel_kind = SB_KIND_TRASH;
            lw->sel_id   = 0;
            refresh_sidebar(lw);
            refresh_notes(lw);
        }
    }
}

/* on_restore_folder() — Trash context menu: put the selected trashed
 * folder (and its subtree) back where it was deleted from; the selection
 * follows it to its restored spot.                                          */
static void
on_restore_folder(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->sel_kind != SB_KIND_TRASH_FOLDER)
        return;
    if (on_db_folder_restore(lw->app->db, lw->sel_id)) {
        on_app_status(lw->app,
                      "Folder \xe2\x80\x9c%s\xe2\x80\x9d restored",
                      lw->sel_name);
        lw->sel_kind = SB_KIND_FOLDER;   /* same id, back in the tree       */
        refresh_sidebar(lw);
        refresh_notes(lw);
    }
}

/* on_empty_trash() — Trash context menu: permanently delete everything in
 * the Trash after one confirmation.                                         */
static void
on_empty_trash(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (!confirm(lw, "Permanently delete everything in the Trash?",
                 "This cannot be undone."))
        return;

    /* Close editors for every note the purge will take with it —
     * including notes inside trashed folder subtrees.                      */
    GArray *ids = on_db_trash_note_ids(lw->app->db);
    close_editors_for_ids(lw, (const gint64 *)ids->data, ids->len);
    g_array_free(ids, TRUE);

    if (on_db_trash_empty(lw->app->db)) {
        on_app_status(lw->app, "Trash emptied");
        refresh_sidebar(lw);         /* the Trash section disappears        */
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
 * scope radio defaults to the selection when it is narrower than "all" —
 * a folder or tag; the All Notes/Trash/Pinned sections have no folder
 * scope, so they default to searching everything.                           */
static void
on_open_search(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    gboolean scoped = lw->sel_kind == SB_KIND_FOLDER ||
                      lw->sel_kind == SB_KIND_TAG ||
                      lw->sel_kind == SB_KIND_TRASH_FOLDER;
    on_search_window_open(lw->app, scoped);
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
        now, "blue-notes-backup-%Y%m%d.db");
    g_date_time_unref(now);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser),
                                      suggestion);
    g_free(suggestion);

    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(
            GTK_FILE_CHOOSER(chooser));
        gtk_widget_destroy(chooser);

        gboolean ok = on_db_backup_to(lw->app->db, path);
        if (ok)
            on_app_status(lw->app, "Database backed up");
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
 * on_open_db() — File → Open Database…: let the user pick any .db file and
 * open it, either as the new permanent default or for this session only.
 * ------------------------------------------------------------------------- */
static void
on_open_db(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;
    OnApp     *app = lw->app;

    /* Step 1: pick the file. */
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Open Database", GTK_WINDOW(lw->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT,
        NULL);
    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_add_pattern(ff, "*.db");
    gtk_file_filter_set_name(ff, "SQLite Database (*.db)");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), ff);

    gchar *file_path = NULL;
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
        file_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);

    if (file_path == NULL)
        return;

    /* Already open: nothing to do. */
    if (g_strcmp0(file_path, app->db->path) == 0) {
        g_free(file_path);
        return;
    }

    /* Step 2: ask how to open it. */
    gchar *display = g_path_get_basename(file_path);
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(lw->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "Open “%s” as your new default database, or for this "
        "session only?", display);
    g_free(display);
    gtk_window_set_title(GTK_WINDOW(dlg), "Blue Notes - Open Database");
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
        "_Cancel",         GTK_RESPONSE_CANCEL,
        "_Session Only",   1,
        "Set as _Default", 2,
        NULL);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    if (resp == GTK_RESPONSE_CANCEL || resp == GTK_RESPONSE_DELETE_EVENT) {
        g_free(file_path);
        return;
    }
    gboolean set_default = (resp == 2);

    /* Step 3: switch to the chosen database. */
    on_app_close_all_editors(app);
    gchar *old_path = g_strdup(app->db->path);
    on_db_close(app->db);
    app->db = on_db_open(file_path);

    if (app->db == NULL) {
        GtkWidget *err = gtk_message_dialog_new(
            GTK_WINDOW(lw->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Could not open:\n%s", file_path);
        gtk_window_set_title(GTK_WINDOW(err), "Blue Notes - Database Error");
        gtk_dialog_run(GTK_DIALOG(err));
        gtk_widget_destroy(err);
        /* Revert to old database. */
        app->db = on_db_open(old_path);
        g_free(old_path);
        g_free(file_path);
        return;
    }

    if (set_default) {
        gchar *new_dir = g_path_get_dirname(file_path);
        g_free(app->db_dir);
        app->db_dir = g_strdup(new_dir);
        app->db_transient = FALSE;
        on_app_config_set("db_dir",  new_dir);
        on_app_config_set("db_hash", NULL);
        g_free(new_dir);
    } else {
        app->db_transient = TRUE;   /* session only: don't persist anything */
    }

    g_free(old_path);
    g_free(file_path);

    if (app->notify_notes_changed != NULL)
        app->notify_notes_changed(app);
    on_app_status(app, "DB at %s loaded", app->db->path);
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

/* find_gtk_image() — first GtkImage in a widget subtree (depth-first).
 * Used to reach GtkAboutDialog's internal logo image, which the public
 * API only feeds with a plain (blurry-on-Retina) GdkPixbuf.                 */
static GtkWidget *
find_gtk_image(GtkWidget *widget)
{
    if (GTK_IS_IMAGE(widget))
        return widget;
    GtkWidget *hit = NULL;           /* first image found in the subtree    */
    if (GTK_IS_CONTAINER(widget)) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *l = kids; l != NULL && hit == NULL; l = l->next)
            hit = find_gtk_image(l->data);
        g_list_free(kids);
    }
    return hit;
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

    /* 128x128-logical logo from vinyl.png, decoded at the display's
     * scale factor so it stays sharp on Retina (quirk #5).                 */
    gint sf = gtk_widget_get_scale_factor(lw->window);
    gchar *icon_path = g_build_filename(lw->app->icons_dir, "vinyl.png",
                                        NULL);
    GdkPixbuf *logo = gdk_pixbuf_new_from_file_at_size(icon_path,
                                                       128 * sf, 128 * sf,
                                                       NULL);
    g_free(icon_path);

    const gchar *authors[] = { "Ian Campbell", "Claude Sonnet 4.5", "And thanks to Blue Note Records. Both for the great double entendre and for amazing music that shaped my youth.", NULL };

    GtkWidget *dialog = gtk_about_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog),
                                 GTK_WINDOW(lw->window));
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog),
                                      "Blue Notes");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), ON_VERSION);
    if (logo != NULL) {
        /* set_logo() first (it makes the internal image visible and
         * sized), then swap that image's content for a cairo surface
         * with the device scale — the pixbuf API renders 1 buffer px
         * per logical px and looks soft on HiDPI.                          */
        GdkPixbuf *at_128 = (sf > 1)
            ? gdk_pixbuf_scale_simple(logo, 128, 128, GDK_INTERP_BILINEAR)
            : g_object_ref(logo);
        gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(dialog), at_128);
        g_object_unref(at_128);

        if (sf > 1) {
            GtkWidget *img = find_gtk_image(
                gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
            if (img != NULL) {
                cairo_surface_t *surface =
                    gdk_cairo_surface_create_from_pixbuf(logo, sf, NULL);
                gtk_image_set_from_surface(GTK_IMAGE(img), surface);
                cairo_surface_destroy(surface);
            }
        }
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

/* folder_name_cmp() — GCompareFunc: case-insensitive alphabetical order
 * of two OnFolder*s.                                                        */
static gint
folder_name_cmp(gconstpointer a, gconstpointer b)
{
    const OnFolder *fa = a, *fb = b;
    gchar *ca = g_utf8_casefold(fa->name != NULL ? fa->name : "", -1);
    gchar *cb = g_utf8_casefold(fb->name != NULL ? fb->name : "", -1);
    gint result = g_strcmp0(ca, cb);
    g_free(ca);
    g_free(cb);
    return result;
}

/* ---------------------------------------------------------------------------
 * on_sort_subfolders() — folder context menu: order the selected
 * folder's (or the root's) DIRECT children alphabetically and persist
 * that as their sort_order.  One level only — each folder's own order
 * stays whatever the user made it.
 * ------------------------------------------------------------------------- */
static void
on_sort_subfolders(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->sel_kind != SB_KIND_FOLDER && lw->sel_kind != SB_KIND_ROOT)
        return;
    gint64 parent =                  /* whose children get sorted           */
        (lw->sel_kind == SB_KIND_FOLDER) ? lw->sel_id : 0;

    GList *kids = on_db_folder_list(lw->app->db, parent);
    kids = g_list_sort(kids, folder_name_cmp);
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    for (GList *l = kids; l != NULL; l = l->next)
        g_array_append_val(ids, ((OnFolder *)l->data)->id);
    on_db_folder_list_free(kids);

    if (ids->len > 1 &&
        on_db_folder_reorder(lw->app->db, (const gint64 *)ids->data,
                             ids->len)) {
        on_app_status(lw->app, "Sorted %u subfolders alphabetically",
                      ids->len);
        refresh_sidebar(lw);
        refresh_notes(lw);
    }
    g_array_free(ids, TRUE);
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

    if (kind == SB_KIND_TRASH) {
        /* The Trash row's only action: purge it.                           */
        add_menu_item(menu, "_Empty Trash\xe2\x80\xa6",
                      G_CALLBACK(on_empty_trash), lw);
        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
        return TRUE;
    }

    if (kind == SB_KIND_TRASH_FOLDER) {
        add_menu_item(menu, "_Restore Folder",
                      G_CALLBACK(on_restore_folder), lw);
        add_menu_item(menu, "Delete _Permanently",
                      G_CALLBACK(on_delete_folder), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
    } else if (kind == SB_KIND_FOLDER || kind == SB_KIND_ROOT) {
        add_menu_item(menu,
                      kind == SB_KIND_FOLDER ? "New _Subfolder\xe2\x80\xa6"
                                             : "New _Folder\xe2\x80\xa6",
                      G_CALLBACK(on_new_folder), lw);
        if (kind == SB_KIND_FOLDER) {
            add_menu_item(menu, "_Rename\xe2\x80\xa6",
                          G_CALLBACK(on_rename_folder), lw);
            add_menu_item(menu, "Move to _Trash",
                          G_CALLBACK(on_delete_folder), lw);
        }
        add_menu_item(menu, "Sort Subfolders _Alphabetically",
                      G_CALLBACK(on_sort_subfolders), lw);
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
    if (lw->list_autofit)
        refresh_notes(lw);           /* re-measure the widths grid skipped  */
}

static void
on_view_grid(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    gtk_stack_set_visible_child_name(GTK_STACK(lw->stack), "grid");
    refresh_notes(lw);               /* fill the thumbnails list mode skips */
}

/* on_toggle_sidebar() — toolbar show/hide button for the folder/tag
 * pane: the notes view takes the whole window while it is hidden.           */
static void
on_toggle_sidebar(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    gtk_widget_set_visible(lw->sidebar_box,
                           !gtk_widget_get_visible(lw->sidebar_box));
}

/* on_toggle_view() — toolbar List/Grid button: switch to whichever notes
 * view is not currently showing.                                            */
static void
on_toggle_view(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    const gchar *mode =              /* "list" or "grid"                    */
        gtk_stack_get_visible_child_name(GTK_STACK(lw->stack));
    if (g_strcmp0(mode, "list") == 0)
        on_view_grid(NULL, lw);
    else
        on_view_list(NULL, lw);
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
    if (in_trash_view(lw))
        delete_notes_permanently(lw, ids);
    else
        trash_notes(lw, ids);
    g_array_free(ids, TRUE);
}

/* on_note_ctx_restore() — Trash-view context menu: put every selected
 * note back where it was deleted from (top level when that folder is
 * itself still in the Trash).                                               */
static void
on_note_ctx_restore(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    OnLibrary *lw = user_data;       /* owning library window               */
    GArray *ids = selected_note_ids(lw);
    if (ids->len > 0) {
        for (guint i = 0; i < ids->len; i++)
            on_db_note_restore(lw->app->db, g_array_index(ids, gint64, i));
        on_app_status(lw->app, "Restored %u note%s",
                      ids->len, ids->len == 1 ? "" : "s");
        refresh_notes(lw);
        refresh_sidebar(lw);         /* counts + the Trash section          */
    }
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

    /* Two menus: the normal one, and the Trash view's restore/purge one.   */
    typedef struct { const gchar *label; GCallback cb; } NoteMenuItem;
    NoteMenuItem normal_items[] = {
        { "_Open",                          G_CALLBACK(on_note_ctx_open) },
        { pinned ? "Un_pin" : "_Pin",       G_CALLBACK(on_note_ctx_toggle_pin) },
        { NULL,                             NULL /* separator */          },
        { "Export as _HTML\xe2\x80\xa6",    G_CALLBACK(on_note_ctx_export_html) },
        { "Export as _Markdown\xe2\x80\xa6",G_CALLBACK(on_note_ctx_export_md)   },
        { NULL,                             NULL /* separator */          },
        { "Move to _Trash",                 G_CALLBACK(on_note_ctx_delete) },
    };
    NoteMenuItem trash_items[] = {
        { "_Open",                          G_CALLBACK(on_note_ctx_open) },
        { "_Restore",                       G_CALLBACK(on_note_ctx_restore) },
        { NULL,                             NULL /* separator */          },
        { "Delete _Permanently",            G_CALLBACK(on_note_ctx_delete) },
    };
    NoteMenuItem *items = in_trash_view(lw) ? trash_items : normal_items;
    gsize n_items = in_trash_view(lw) ? G_N_ELEMENTS(trash_items)
                                      : G_N_ELEMENTS(normal_items);
    for (gsize i = 0; i < n_items; i++) {
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

/* notes_sel_block_func() / notes_sel_allow_func() — temporary select
 * functions for the span of a press on an already-selected list row:
 * block vetoes EVERY selection change, allow puts normality back.          */
static gboolean
notes_sel_block_func(GtkTreeSelection *sel, GtkTreeModel *model,
                     GtkTreePath *path, gboolean selected, gpointer data)
{
    (void)sel; (void)model; (void)path; (void)selected; (void)data;
    return FALSE;
}

static gboolean
notes_sel_allow_func(GtkTreeSelection *sel, GtkTreeModel *model,
                     GtkTreePath *path, gboolean selected, gpointer data)
{
    (void)sel; (void)model; (void)path; (void)selected; (void)data;
    return TRUE;
}

/* notes_sel_unblock() — end a blocked press: selection changes work
 * again; the press path is optionally handed to the caller (transfer),
 * otherwise freed.                                                          */
static GtkTreePath *
notes_sel_unblock(OnLibrary *lw, gboolean want_path)
{
    if (!lw->notes_sel_blocked)
        return NULL;
    gtk_tree_selection_set_select_function(
        gtk_tree_view_get_selection(lw->notes_list),
        notes_sel_allow_func, NULL, NULL);
    lw->notes_sel_blocked = FALSE;
    GtkTreePath *path = lw->notes_press_path;
    lw->notes_press_path = NULL;
    if (!want_path) {
        gtk_tree_path_free(path);
        path = NULL;
    }
    return path;
}

/* ---------------------------------------------------------------------------
 * on_notes_list_button_press() — two jobs:
 *
 * 1. Left press on an already-selected row of a multi-selection: GTK
 *    3.24's tree view CLEAR_AND_SELECTs on PRESS with no deferral for a
 *    possible drag (quirk #15), which collapsed the selection before a
 *    multi-note drag could start.  Install a selection veto for the span
 *    of the press; on_notes_drag_begin keeps the selection, a plain
 *    release applies the collapse GTK wanted.  The event itself is NOT
 *    consumed — the view's drag gesture must still see it.
 *
 * 2. Right click: select the row under the pointer and show the note
 *    menu (an existing multi-selection is kept when clicked inside).
 * ------------------------------------------------------------------------- */
static gboolean
on_notes_list_button_press(GtkWidget *widget, GdkEventButton *event,
                           gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */

    if (event->button == GDK_BUTTON_PRIMARY &&
        event->type == GDK_BUTTON_PRESS &&
        !(event->state & gtk_accelerator_get_default_mod_mask())) {
        GtkTreeSelection *sel =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
        GtkTreePath *path = NULL;    /* row under the pointer               */
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
                                          (gint)event->x, (gint)event->y,
                                          &path, NULL, NULL, NULL)) {
            if (gtk_tree_selection_path_is_selected(sel, path) &&
                gtk_tree_selection_count_selected_rows(sel) > 1) {
                gtk_tree_selection_set_select_function(
                    sel, notes_sel_block_func, NULL, NULL);
                lw->notes_sel_blocked = TRUE;
                gtk_tree_path_free(lw->notes_press_path);
                lw->notes_press_path = path;     /* ownership taken         */
                return FALSE;
            }
            gtk_tree_path_free(path);
        }
        return FALSE;
    }

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

/* on_notes_list_button_release() — a blocked press ended WITHOUT a drag:
 * lift the veto and apply the collapse GTK wanted on press (a plain
 * click on a selected row means "select just this one").                    */
static gboolean
on_notes_list_button_release(GtkWidget *widget, GdkEventButton *event,
                             gpointer user_data)
{
    (void)event;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreePath *path = notes_sel_unblock(lw, TRUE);
    if (path != NULL) {
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(widget), path, NULL, FALSE);
        gtk_tree_path_free(path);
    }
    return FALSE;
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
 * status bar
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * status_path_update() — put the current selection's location in the
 * status bar's left label: "/" for the root, "/Folder/Sub" for folders,
 * and the raw sidebar name ("#tag", "Pinned Notes") for the non-path
 * views.  Runs from refresh_notes(), so it tracks every navigation.
 * ------------------------------------------------------------------------- */
static void
status_path_update(OnLibrary *lw)
{
    if (lw->status_path == NULL)
        return;

    gchar *text;                     /* the location part                   */
    if (lw->sel_kind == SB_KIND_TAG || lw->sel_kind == SB_KIND_PINNED ||
        lw->sel_kind == SB_KIND_ALL || lw->sel_kind == SB_KIND_TRASH) {
        text = g_strdup(lw->sel_name != NULL ? lw->sel_name : "");
    } else if (lw->sel_kind == SB_KIND_TRASH_FOLDER) {
        text = g_strdup_printf("Trash/%s",
                               lw->sel_name != NULL ? lw->sel_name : "");
    } else {
        gchar *path = on_db_folder_path(lw->app->db, lw->sel_id);
        text = g_strdup_printf("/%s", path);
        g_free(path);
    }

    gtk_label_set_text(GTK_LABEL(lw->status_path), text);
    g_free(text);
}

/* on_notes_selection_status() — selection changed in either notes view:
 * post "N files selected" as a transient event message (right label,
 * fades like any other).  Nothing is posted when the selection empties,
 * and refresh_notes() repopulation (which clears the selection row by
 * row) is skipped via the populating guard.                                 */
static void
on_notes_selection_status(gpointer view_or_selection, gpointer user_data)
{
    (void)view_or_selection;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->populating != 0)
        return;
    GArray *sel = selected_note_ids(lw);
    if (sel->len > 0)
        on_app_status(lw->app, "%u file%s selected",
                      sel->len, sel->len == 1 ? "" : "s");
    g_array_free(sel, TRUE);
}

/* status_fade_timeout() — the display period ended: fade the event
 * message out (the revealer crossfades to nothing).                         */
static gboolean
status_fade_timeout(gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    lw->status_timeout = 0;
    gtk_revealer_set_reveal_child(GTK_REVEALER(lw->status_revealer),
                                  FALSE);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * library_notify_status() — installed as app->notify_status; shows an
 * event message in the status bar's right label, fading it out after
 * STATUS_FADE_SECONDS (each new message restarts the clock).  Post
 * through on_app_status(), never directly.
 * ------------------------------------------------------------------------- */
static void
library_notify_status(OnApp *app, const gchar *message)
{
    OnLibrary *lw =                  /* library state stashed on the window */
        g_object_get_data(G_OBJECT(app->library_window), "on-library");
    if (lw == NULL || lw->status_event == NULL)
        return;

    gtk_label_set_text(GTK_LABEL(lw->status_event), message);
    gtk_revealer_set_reveal_child(GTK_REVEALER(lw->status_revealer), TRUE);

    if (lw->status_timeout != 0)
        g_source_remove(lw->status_timeout);
    lw->status_timeout = g_timeout_add_seconds(STATUS_FADE_SECONDS,
                                               status_fade_timeout, lw);
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

    /* Only tags and folder-like selections (including a trashed folder
     * browsed from the Trash section) make a meaningful scope; All Notes,
     * Trash and Pinned keep the "everything" default above.                 */
    if (lw->sel_kind != SB_KIND_TAG &&
        lw->sel_kind != SB_KIND_FOLDER &&
        lw->sel_kind != SB_KIND_TRASH_FOLDER &&
        lw->sel_kind != SB_KIND_ROOT)
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

/* ===========================================================================
 * list-view column layout (order + visibility)
 *
 * Headers drag to reorder (GtkTreeViewColumn reorderable) and right-click
 * for a show/hide menu.  The layout persists in the ini as
 * "list_columns=key:vis,key:vis,..." in display order; each column
 * carries its stable key as object data ("on-colkey").
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * list_columns_persist() — write the list view's current column order and
 * visibility to the ini.  A partial column list (the view tearing down
 * removes columns one by one) is never persisted.
 * ------------------------------------------------------------------------- */
static void
list_columns_persist(OnLibrary *lw)
{
    GList *cols = gtk_tree_view_get_columns(lw->notes_list);
    if (g_list_length(cols) != 3) {
        g_list_free(cols);
        return;
    }
    GString *s = g_string_new(NULL);
    for (GList *l = cols; l != NULL; l = l->next) {
        const gchar *key =           /* stable column identity              */
            g_object_get_data(G_OBJECT(l->data), "on-colkey");
        if (key == NULL)
            continue;
        if (s->len > 0)
            g_string_append_c(s, ',');
        g_string_append_printf(s, "%s:%d", key,
            gtk_tree_view_column_get_visible(l->data) ? 1 : 0);
    }
    g_list_free(cols);
    on_app_config_set("list_columns", s->str);
    g_string_free(s, TRUE);
}

/* list_column_by_key() — the list-view column carrying `key`, or NULL.      */
static GtkTreeViewColumn *
list_column_by_key(OnLibrary *lw, const gchar *key)
{
    GtkTreeViewColumn *found = NULL;
    GList *cols = gtk_tree_view_get_columns(lw->notes_list);
    for (GList *l = cols; l != NULL && found == NULL; l = l->next)
        if (g_strcmp0(g_object_get_data(G_OBJECT(l->data), "on-colkey"),
                      key) == 0)
            found = l->data;
    g_list_free(cols);
    return found;
}

/* ---------------------------------------------------------------------------
 * list_columns_apply() — put the saved column order and visibility back
 * (default: Path, Title, Modified, all visible).  Unknown keys are
 * skipped, missing ones keep their built order, and at least one column
 * is forced visible (a hand-edited ini can't blank the view).
 * ------------------------------------------------------------------------- */
static void
list_columns_apply(OnLibrary *lw)
{
    gchar *cfg = on_app_config_get("list_columns");
    if (cfg == NULL || *cfg == '\0') {
        g_free(cfg);
        cfg = g_strdup("path:1,title:1,modified:1");
    }

    GtkTreeViewColumn *prev = NULL;  /* each entry moves after the last     */
    gchar **entries = g_strsplit(cfg, ",", -1);
    for (gsize i = 0; entries[i] != NULL; i++) {
        gchar **kv = g_strsplit(entries[i], ":", 2);
        GtkTreeViewColumn *c =
            kv[0] != NULL ? list_column_by_key(lw, kv[0]) : NULL;
        if (c != NULL) {
            gtk_tree_view_move_column_after(lw->notes_list, c, prev);
            gtk_tree_view_column_set_visible(
                c, kv[1] == NULL || g_strcmp0(kv[1], "0") != 0);
            prev = c;
        }
        g_strfreev(kv);
    }
    g_strfreev(entries);
    g_free(cfg);

    GList *cols = gtk_tree_view_get_columns(lw->notes_list);
    gboolean any_visible = FALSE;    /* is at least one column shown?       */
    for (GList *l = cols; l != NULL; l = l->next)
        any_visible |= gtk_tree_view_column_get_visible(l->data);
    if (!any_visible && cols != NULL)
        gtk_tree_view_column_set_visible(cols->data, TRUE);
    g_list_free(cols);
}

/* on_notes_columns_changed() — a header drag reordered the columns:
 * persist the new layout.  Fires per-column during teardown too, when
 * the library state may already be gone — bail out then.                    */
static void
on_notes_columns_changed(GtkTreeView *view, gpointer user_data)
{
    if (gtk_widget_in_destruction(GTK_WIDGET(view)))
        return;
    list_columns_persist(user_data);
}

/* ---------------------------------------------------------------------------
 * list_autofit_apply() — put every column into (or out of) autofit mode.
 *
 * Autofit does NOT use GTK_TREE_VIEW_COLUMN_AUTOSIZE: tree-view columns
 * cache resized/requested widths that override it (they neither grow to
 * long content nor shrink back for short content).  Instead every column
 * goes FIXED, and refresh_notes() MEASURES the content with a
 * PangoLayout as it populates the model (see list_autofit_set) and
 * sets the exact widths — same technique as the sidebar width fit.
 *
 *   ON  — Path and Modified: no ellipsize, FIXED at their measured
 *         content width (grips off — the next refresh would reclaim
 *         them anyway).  Title takes the ellipsis + expand: it fills
 *         the remaining space and is the one column that truncates.
 *   OFF — back to the built modes: Title GROW_ONLY + expand, no
 *         ellipsize; Path FIXED at its current width, ellipsized;
 *         Modified GROW_ONLY; all user-resizable.
 * ------------------------------------------------------------------------- */
static void
list_autofit_apply(OnLibrary *lw)
{
    GList *cols = gtk_tree_view_get_columns(lw->notes_list);
    for (GList *l = cols; l != NULL; l = l->next) {
        GtkTreeViewColumn *c = l->data;  /* one column                      */
        const gchar *key =               /* stable column identity          */
            g_object_get_data(G_OBJECT(c), "on-colkey");
        GtkCellRenderer *cell =          /* its text renderer               */
            g_object_get_data(G_OBJECT(c), "on-cell");
        gboolean title = g_strcmp0(key, "title") == 0;
        gboolean path  = g_strcmp0(key, "path")  == 0;

        if (lw->list_autofit) {
            g_object_set(cell, "ellipsize",
                         title ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE,
                         NULL);
            gtk_tree_view_column_set_resizable(c, FALSE);
            gtk_tree_view_column_set_sizing(
                c, GTK_TREE_VIEW_COLUMN_FIXED);
            if (title)                   /* floor; expand fills the rest    */
                gtk_tree_view_column_set_fixed_width(c, 100);
            gtk_tree_view_column_set_expand(c, title);
        } else {
            g_object_set(cell, "ellipsize",
                         path ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE,
                         NULL);
            if (path) {
                gint w = gtk_tree_view_column_get_width(c);
                gtk_tree_view_column_set_sizing(
                    c, GTK_TREE_VIEW_COLUMN_FIXED);
                gtk_tree_view_column_set_fixed_width(c, w > 0 ? w : 180);
            } else {
                gtk_tree_view_column_set_sizing(
                    c, GTK_TREE_VIEW_COLUMN_GROW_ONLY);
            }
            gtk_tree_view_column_set_expand(c, title);
            gtk_tree_view_column_set_resizable(c, TRUE);
        }
        gtk_tree_view_column_queue_resize(c);
    }
    g_list_free(cols);
}

/* Extra pixels beyond the raw text: the renderer's xpad (10 a side)
 * plus tree-view cell chrome; headers get room for the sort arrow.        */
#define AUTOFIT_CELL_EXTRA   30
#define AUTOFIT_HEADER_EXTRA 40

/* ---------------------------------------------------------------------------
 * list_autofit_set() — apply a measured content width to one column
 * (bounded below by what its header label needs).  The measuring itself
 * rides refresh_notes()'s population loop — no second model walk.
 * ------------------------------------------------------------------------- */
static void
list_autofit_set(OnLibrary *lw, PangoLayout *lay, const gchar *key,
                 gint content_w)
{
    GtkTreeViewColumn *c = list_column_by_key(lw, key);
    if (c == NULL || !gtk_tree_view_column_get_visible(c))
        return;
    gint w = 0;                      /* header label width                  */
    pango_layout_set_text(lay, gtk_tree_view_column_get_title(c), -1);
    pango_layout_get_pixel_size(lay, &w, NULL);
    gtk_tree_view_column_set_fixed_width(
        c, MAX(content_w + AUTOFIT_CELL_EXTRA, w + AUTOFIT_HEADER_EXTRA));
}

/* on_autofit_toggled() — the "Autofit Column Widths" check item flipped:
 * remember, persist and apply.                                              */
static void
on_autofit_toggled(GtkCheckMenuItem *item, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    lw->list_autofit = gtk_check_menu_item_get_active(item);
    on_app_config_set("list_autofit", lw->list_autofit ? "1" : "0");
    list_autofit_apply(lw);
    refresh_notes(lw);               /* measuring rides the populate loop   */
}

/* on_column_toggled() — a check item in the header menu flipped:
 * show/hide that column and persist.                                        */
static void
on_column_toggled(GtkCheckMenuItem *item, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreeViewColumn *c = g_object_get_data(G_OBJECT(item), "on-column");
    gtk_tree_view_column_set_visible(
        c, gtk_check_menu_item_get_active(item));
    list_columns_persist(lw);
    if (lw->list_autofit)
        refresh_notes(lw);           /* a re-shown column needs its width   */
}

/* ---------------------------------------------------------------------------
 * on_column_header_press() — right click on a list-view column header:
 * a menu of check items showing/hiding each column.  The only remaining
 * visible column's item is disabled so the view can't go empty.
 * ------------------------------------------------------------------------- */
static gboolean
on_column_header_press(GtkWidget *button, GdkEventButton *event,
                       gpointer user_data)
{
    (void)button;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (event->button != GDK_BUTTON_SECONDARY)
        return FALSE;

    GtkWidget *menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(menu), lw->window, NULL);
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);

    GList *cols = gtk_tree_view_get_columns(lw->notes_list);
    gint n_visible = 0;              /* how many columns are shown          */
    for (GList *l = cols; l != NULL; l = l->next)
        if (gtk_tree_view_column_get_visible(l->data))
            n_visible++;

    for (GList *l = cols; l != NULL; l = l->next) {
        GtkTreeViewColumn *c = l->data;  /* one column                      */
        gboolean visible = gtk_tree_view_column_get_visible(c);
        GtkWidget *item = gtk_check_menu_item_new_with_label(
            gtk_tree_view_column_get_title(c));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), visible);
        if (visible && n_visible == 1)
            gtk_widget_set_sensitive(item, FALSE);
        g_object_set_data(G_OBJECT(item), "on-column", c);
        g_signal_connect(item, "toggled",
                         G_CALLBACK(on_column_toggled), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    g_list_free(cols);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());
    GtkWidget *fit = gtk_check_menu_item_new_with_label(
        "Autofit Column Widths");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(fit),
                                   lw->list_autofit);
    g_signal_connect(fit, "toggled",
                     G_CALLBACK(on_autofit_toggled), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), fit);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * sort_by_path() — case-insensitive alphabetical sort for the Path
 * header; equal paths fall back to the title so folders group cleanly.
 * ------------------------------------------------------------------------- */
static gint
sort_by_path(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
             gpointer user_data)
{
    (void)user_data;
    gchar *pa, *pb;                  /* the two paths                       */
    gtk_tree_model_get(model, a, NL_PATH, &pa, -1);
    gtk_tree_model_get(model, b, NL_PATH, &pb, -1);
    gchar *ca = g_utf8_casefold(pa != NULL ? pa : "", -1);
    gchar *cb = g_utf8_casefold(pb != NULL ? pb : "", -1);
    gint result = g_strcmp0(ca, cb);
    g_free(ca); g_free(cb);
    g_free(pa); g_free(pb);
    return result != 0 ? result : sort_by_title(model, a, b, NULL);
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
    add_menu_item(file_menu, "_Open Database\xe2\x80\xa6",
                  G_CALLBACK(on_open_db), lw);
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
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          gtk_separator_menu_item_new());
    add_menu_item(view_menu, "_Search Notes\xe2\x80\xa6",
                  G_CALLBACK(on_open_search), lw);

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
 * build_action_bar() — the single unified toolbar spanning the window:
 * a folder-actions area, a drawn separator, a note-actions area, another
 * separator, Search — and the About logo pushed to the right edge.
 * Returns the toolbar widget.
 * ------------------------------------------------------------------------- */
static GtkWidget *
build_action_bar(OnLibrary *lw)
{
    GtkWidget *toolbar = gtk_toolbar_new();
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar),
                              GTK_ICON_SIZE_SMALL_TOOLBAR);

    /* --- folder area ---------------------------------------------------- */
    add_tool_button(lw, toolbar, "web", "\xe2\x97\xa7",
                    "Folders", "Show or hide the folder pane",
                    G_CALLBACK(on_toggle_sidebar));
    add_tool_button(lw, toolbar, "new-folder", "+\xf0\x9f\x93\x81",
                    "New Folder", "Create a folder inside the selection",
                    G_CALLBACK(on_new_folder));
    add_tool_button(lw, toolbar, "delete-folder", "\xe2\x9c\x95",
                    "Delete Folder",
                    "Move the selected folder to the Trash",
                    G_CALLBACK(on_delete_folder));
    /* Rename lives in the folder's right-click menu only.                  */

    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    /* --- notes area ------------------------------------------------------*/
    add_tool_button(lw, toolbar, "file", "+", "New Note",
                    "Create a note in the current folder",
                    G_CALLBACK(on_new_note));
    add_tool_button(lw, toolbar, "delete", "\xe2\x9c\x95",
                    "Delete Note",
                    "Move the selected notes to the Trash",
                    G_CALLBACK(on_delete_note));
    add_tool_button(lw, toolbar, "view", "\xe2\x8a\x9e",
                    "List/Grid", "Toggle between list and grid view",
                    G_CALLBACK(on_toggle_view));

    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    add_tool_button(lw, toolbar, "search", "\xf0\x9f\x94\x8d",
                    "Search", "Search notes (Ctrl+F)",
                    G_CALLBACK(on_open_search));

    /* Expanding separator pushes the About button to the right edge.       */
    GtkToolItem *spacer = gtk_separator_tool_item_new();
    gtk_separator_tool_item_set_draw(GTK_SEPARATOR_TOOL_ITEM(spacer),
                                     FALSE);
    gtk_tool_item_set_expand(spacer, TRUE);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), spacer, -1);

    /* The About button at the far right.  Built by hand because the
     * child must stay centered (see about_button_fit_style).               */
    GtkToolItem *about_item = gtk_tool_item_new();
    GtkWidget *about_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(about_btn), GTK_RELIEF_NONE);
    gtk_container_add(GTK_CONTAINER(about_item), about_btn);
    {
        gchar *logo_path = g_build_filename(lw->app->icons_dir,
                                            "vinyl.png", NULL);
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
            logo = gtk_label_new("\xf0\x9f\x92\xbf");    /* 💿 fallback     */
        }
        GtkWidget *label = gtk_label_new("About");   /* text-mode child     */

        /* Keep owning refs so removal from the button never frees them.    */
        g_object_set_data_full(G_OBJECT(about_item), "on-logo",
                               g_object_ref_sink(logo), g_object_unref);
        g_object_set_data_full(G_OBJECT(about_item), "on-label",
                               g_object_ref_sink(label), g_object_unref);
        g_free(logo_path);
    }
    gtk_tool_item_set_tooltip_text(about_item, "About Blue Notes");
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
 * note in the currently selected folder; Ctrl/Cmd+F opens the search
 * window.
 * ------------------------------------------------------------------------- */
static gboolean
on_library_key_press(GtkWidget *widget, GdkEventKey *event,
                     gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (event->state & (GDK_CONTROL_MASK | GDK_META_MASK)) {
        guint key = gdk_keyval_to_lower(event->keyval);
        if (key == GDK_KEY_n) {
            on_new_note(NULL, lw);
            return TRUE;
        }
        if (key == GDK_KEY_f) {
            on_open_search(NULL, lw);
            return TRUE;
        }
    }
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * sidebar_name_cell_func() — bold the sidebar's section rows (the Notes
 * root, the Tags header, and Pinned Notes); folders and tags render at
 * normal weight.  Runs per row draw, keyed on SB_KIND.
 * ------------------------------------------------------------------------- */
static void
sidebar_name_cell_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                       GtkTreeModel *model, GtkTreeIter *iter,
                       gpointer user_data)
{
    (void)col; (void)user_data;
    gint kind;                       /* SB_KIND_* of this row               */
    gtk_tree_model_get(model, iter, SB_KIND, &kind, -1);
    gboolean bold = kind == SB_KIND_ROOT ||
                    kind == SB_KIND_TAGS_HEADER ||
                    kind == SB_KIND_PINNED ||
                    kind == SB_KIND_ALL ||
                    kind == SB_KIND_TRASH;
    g_object_set(cell, "weight",
                 bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, NULL);
}

/* library_free() — destructor for the OnLibrary attached to the window.     */
static void
library_free(gpointer data)
{
    OnLibrary *lw = data;
    if (lw->status_timeout != 0)
        g_source_remove(lw->status_timeout);
    g_hash_table_destroy(lw->thumb_cache);
    if (lw->notes_press_path != NULL)
        gtk_tree_path_free(lw->notes_press_path);
    g_free(lw->sel_name);
    g_free(lw);
}

/* sb_fit_ctx — working state passed through gtk_tree_model_foreach() for
 * the sidebar width measurement walk.                                       */
typedef struct {
    PangoLayout *lay;   /* reused layout (same font as the sidebar)          */
    gint         max_w; /* running maximum row pixel width                   */
} SbFitCtx;

/* sb_fit_measure() — foreach callback: measure one sidebar row and update
 * the running maximum in ctx->max_w.
 *   model / path / iter — standard foreach signature.
 *   data                — SbFitCtx *.
 * Returns FALSE to continue the walk.                                       */
static gboolean
sb_fit_measure(GtkTreeModel *model, GtkTreePath *path,
               GtkTreeIter *iter, gpointer data)
{
    SbFitCtx *ctx  = data;
    gchar    *name = NULL;
    gint      kind;
    gtk_tree_model_get(model, iter, SB_NAME, &name, SB_KIND, &kind, -1);
    if (name && *name) {
        gboolean bold = (kind == SB_KIND_ROOT  ||
                         kind == SB_KIND_TAGS_HEADER ||
                         kind == SB_KIND_PINNED ||
                         kind == SB_KIND_ALL ||
                         kind == SB_KIND_TRASH);
        PangoAttrList *al = pango_attr_list_new();
        if (bold)
            pango_attr_list_insert(al,
                pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        pango_layout_set_attributes(ctx->lay, al);
        pango_attr_list_unref(al);
        pango_layout_set_text(ctx->lay, name, -1);

        gint tw, th;
        pango_layout_get_pixel_size(ctx->lay, &tw, &th);

        /* 22 px per depth level covers the GTK3 expander column width +
         * level-indentation; 10 px base for cell left/right padding.        */
        gint depth = gtk_tree_path_get_depth(path);
        gint row_w = tw + depth * 22 + 10;
        if (row_w > ctx->max_w)
            ctx->max_w = row_w;
    }
    g_free(name);
    return FALSE;
}

/* on_sidebar_fit_to_content() — idle callback: set the paned divider to the
 * sidebar tree view's content width, measured with Pango so that ellipsizing
 * on the cell renderer doesn't cause get_preferred_width() to return a tiny
 * value.  Runs once after the window is realized and the model is populated. */
static gboolean
on_sidebar_fit_to_content(gpointer user_data)
{
    OnLibrary *lw = user_data;
    SbFitCtx ctx;
    ctx.lay   = gtk_widget_create_pango_layout(GTK_WIDGET(lw->sidebar), NULL);
    ctx.max_w = 160;   /* floor: never collapse the sidebar to unusable width */
    gtk_tree_model_foreach(GTK_TREE_MODEL(lw->sidebar_store),
                           sb_fit_measure, &ctx);
    g_object_unref(ctx.lay);
    gtk_paned_set_position(GTK_PANED(lw->sidebar_paned), ctx.max_w);
    return G_SOURCE_REMOVE;
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
    gtk_window_set_title(GTK_WINDOW(lw->window), "Blue Notes - Library");
    gtk_window_set_default_size(GTK_WINDOW(lw->window), 900, 620);
    gtk_application_add_window(app->gtk_app, GTK_WINDOW(lw->window));
    g_object_set_data_full(G_OBJECT(lw->window), "on-library", lw,
                           library_free);

    app->library_window       = lw->window;
    app->notify_notes_changed = library_notify_notes_changed;
    app->notify_note_saved    = library_notify_note_saved;
    app->notify_status        = library_notify_status;

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
        G_TYPE_INT64,                    /* NL_UPDATED                     */
        G_TYPE_STRING);                  /* NL_PATH                        */
    g_signal_connect(lw->notes_store, "row-deleted",
                     G_CALLBACK(on_notes_row_deleted), lw);

    /* --- sidebar -----------------------------------------------------------*/
    lw->sidebar = GTK_TREE_VIEW(
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(lw->sidebar_store)));
    gtk_tree_view_set_headers_visible(lw->sidebar, FALSE);
    /* No GTK type-ahead popup: set_model auto-picks the first column
     * transformable to string as the search column — here the int id,
     * so typing raised a search box that matched nothing.               */
    gtk_tree_view_set_enable_search(lw->sidebar, FALSE);
    {
        /* Ellipsizing names keeps the pane's MINIMUM width small: without
         * it the widest row dictates the minimum and the divider can't be
         * dragged past it.                                                 */
        GtkCellRenderer *name_cell = gtk_cell_renderer_text_new();
        g_object_set(name_cell,
                     "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *name_col =
            gtk_tree_view_column_new_with_attributes(
                "Name", name_cell, "text", SB_NAME, NULL);
        /* Section rows (Notes root, Tags, Pinned Notes) render bold.       */
        gtk_tree_view_column_set_cell_data_func(
            name_col, name_cell, sidebar_name_cell_func, NULL, NULL);
        gtk_tree_view_column_set_sizing(name_col,
                                        GTK_TREE_VIEW_COLUMN_AUTOSIZE);
        gtk_tree_view_append_column(lw->sidebar, name_col);
    }

    /* Sidebar palette: light grey backdrop (rows and the empty area
     * below them — the tree view paints the whole widget), muted grey
     * text, and a blue selection bar (white text for contrast).            */
    {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            "treeview.view {"
            "  background-color: rgb(230,230,230);"
            "  color: rgb(65,65,65);"
            "}"
            "treeview.view:selected {"
            "  background-color: rgb(86,131,224);"
            "  color: white;"
            "}"
            /* Drop indicator (drawn as the border of the row under the
             * pointer, state :drop(active) + a position class): a 2px
             * line in the selection blue — top edge for BEFORE, bottom
             * for AFTER, a full box for INTO.                             */
            "treeview.view:drop(active) {"
            "  border-color: rgb(86,131,224);"
            "  border-width: 2px;"
            "  border-style: solid;"
            "}"
            "treeview.view:drop(active).before {"
            "  border-style: solid none none none;"
            "}"
            "treeview.view:drop(active).after {"
            "  border-style: none none solid none;"
            "}",
            -1, NULL);
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(GTK_WIDGET(lw->sidebar)),
            GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }

    GtkTreeSelection *sb_sel = gtk_tree_view_get_selection(lw->sidebar);
    gtk_tree_selection_set_select_function(sb_sel, sidebar_select_func,
                                           NULL, NULL);
    g_signal_connect(sb_sel, "changed",
                     G_CALLBACK(on_sidebar_selection_changed), lw);

    /* Accept dragged note rows to move notes between folders, and let
     * folder rows be dragged to re-nest/reorder them.  The dest protocol
     * is fully custom (motion answers the status itself; only the drop
     * requests the row data) — see quirk #13.                              */
    gtk_tree_view_enable_model_drag_dest(lw->sidebar, &ROW_TARGET, 1,
                                         GDK_ACTION_MOVE);
    gtk_tree_view_enable_model_drag_source(lw->sidebar, GDK_BUTTON1_MASK,
                                           &ROW_TARGET, 1,
                                           GDK_ACTION_MOVE);
    g_signal_connect(lw->sidebar, "drag-motion",
                     G_CALLBACK(on_sidebar_drag_motion), lw);
    g_signal_connect(lw->sidebar, "drag-leave",
                     G_CALLBACK(on_sidebar_drag_leave), NULL);
    g_signal_connect(lw->sidebar, "drag-drop",
                     G_CALLBACK(on_sidebar_drag_drop), lw);
    g_signal_connect(lw->sidebar, "drag-data-received",
                     G_CALLBACK(on_sidebar_drag_received), lw);
    /* AFTER: the class handler sets a row-snapshot icon; ours overrides.   */
    g_signal_connect_after(lw->sidebar, "drag-begin",
                           G_CALLBACK(on_sidebar_drag_begin), lw);
    g_signal_connect(lw->sidebar, "button-press-event",
                     G_CALLBACK(on_sidebar_button_press), lw);

    GtkWidget *sidebar_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(sidebar_scroll), FALSE);
    gtk_container_add(GTK_CONTAINER(sidebar_scroll),
                      GTK_WIDGET(lw->sidebar));

    /* Sidebar column: just the tree (all buttons live in the unified
     * toolbar above the paned).  Its minimum width is whatever the tree
     * content needs — the scrolled window never scrolls horizontally, so
     * it requests the tree's full natural width.                           */
    GtkWidget *sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(sidebar_box), sidebar_scroll,
                       TRUE, TRUE, 0);
    lw->sidebar_box = sidebar_box;   /* for the toolbar show/hide toggle    */

    /* --- notes list view ---------------------------------------------------*/
    lw->notes_list = GTK_TREE_VIEW(
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(lw->notes_store)));
    /* No GTK type-ahead popup (auto-picked search column, see quirk 16).  */
    gtk_tree_view_set_enable_search(lw->notes_list, FALSE);

    /* Same drop-indicator styling as the sidebar: a 2px line in the
     * selection blue for the in-list reorder drag.  Notes are a flat
     * list, so there is no real "into" — GTK still reports INTO_OR_*
     * over the middle of a row, but a list-store drop there INSERTS
     * BEFORE that row, so the .into indicator is drawn as the same
     * between-rows line (top edge), never a box.                          */
    {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            "treeview.view:drop(active) {"
            "  border-color: rgb(86,131,224);"
            "  border-width: 2px;"
            "  border-style: solid none none none;"
            "}"
            "treeview.view:drop(active).after {"
            "  border-style: none none solid none;"
            "}",
            -1, NULL);
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(GTK_WIDGET(lw->notes_list)),
            GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }
    {
        /* Each column gets a data func painting the alternating row tint.  */
        GtkCellRenderer *r1 = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *c1 =
            gtk_tree_view_column_new_with_attributes("Title", r1,
                                                     "text", NL_TITLE,
                                                     NULL);
        gtk_tree_view_column_set_cell_data_func(c1, r1, notes_row_bg_func,
                                                NULL, NULL);
        gtk_tree_view_column_set_resizable(c1, TRUE);
        gtk_tree_view_append_column(lw->notes_list, c1);

        /* Path column: the note's folder location ("/Work/Projects").
         * Fixed width + ellipsize so deep trees can't blow the layout
         * out; the divider is user-draggable like the others.             */
        GtkCellRenderer *rp = gtk_cell_renderer_text_new();
        g_object_set(rp,
                     "ellipsize", PANGO_ELLIPSIZE_END,
                     "xpad",      10,
                     NULL);
        GtkTreeViewColumn *cp =
            gtk_tree_view_column_new_with_attributes("Path", rp,
                                                     "text", NL_PATH,
                                                     NULL);
        gtk_tree_view_column_set_cell_data_func(cp, rp, notes_row_bg_func,
                                                NULL, NULL);
        gtk_tree_view_column_set_sizing(cp, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(cp, 180);
        gtk_tree_view_column_set_resizable(cp, TRUE);
        gtk_tree_view_append_column(lw->notes_list, cp);

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
        gtk_tree_view_column_set_resizable(c2, TRUE);
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
        gtk_tree_sortable_set_sort_func(
            GTK_TREE_SORTABLE(lw->notes_store), NL_PATH,
            sort_by_path, NULL, NULL);
        gtk_tree_view_column_set_sort_column_id(c1, NL_TITLE);
        gtk_tree_view_column_set_sort_column_id(cp, NL_PATH);
        gtk_tree_view_column_set_sort_column_id(c2, NL_UPDATED);

        /* Default sort: Modified with the most recent on top
         * (sort_by_updated is deliberately inverted, so ASCENDING =
         * newest first).  While any sort is active the list store
         * refuses in-list row drops, so manual drag-reordering of notes
         * is off in this mode — moves to folders are unaffected.          */
        gtk_tree_sortable_set_sort_column_id(
            GTK_TREE_SORTABLE(lw->notes_store), NL_UPDATED,
            GTK_SORT_ASCENDING);

        /* Column layout management: drag a header to reorder, right-click
         * one for the show/hide menu; the layout persists in the ini
         * (applied below once all columns exist).  Each column carries
         * its renderer so autofit can move the ellipsis around.           */
        struct { GtkTreeViewColumn *col; GtkCellRenderer *cell;
                 const gchar *key; } COLS[] = {
            { c1, r1, "title" }, { cp, rp, "path" }, { c2, r2, "modified" },
        };
        for (gsize i = 0; i < G_N_ELEMENTS(COLS); i++) {
            g_object_set_data(G_OBJECT(COLS[i].col), "on-colkey",
                              (gpointer)COLS[i].key);
            g_object_set_data(G_OBJECT(COLS[i].col), "on-cell",
                              COLS[i].cell);
            gtk_tree_view_column_set_reorderable(COLS[i].col, TRUE);
            g_signal_connect(gtk_tree_view_column_get_button(COLS[i].col),
                             "button-press-event",
                             G_CALLBACK(on_column_header_press), lw);
        }
    }
    list_columns_apply(lw);          /* saved order + visibility            */
    {
        /* Autofit is the default; only an explicit "0" turns it off.       */
        gchar *v = on_app_config_get("list_autofit");
        lw->list_autofit = g_strcmp0(v, "0") != 0;
        g_free(v);
        if (lw->list_autofit)
            list_autofit_apply(lw);
    }
    /* Connected only now, so applying the saved layout doesn't re-persist
     * it; from here on every header drag writes the ini.                   */
    g_signal_connect(lw->notes_list, "columns-changed",
                     G_CALLBACK(on_notes_columns_changed), lw);
    gtk_tree_selection_set_mode(
        gtk_tree_view_get_selection(lw->notes_list),
        GTK_SELECTION_MULTIPLE);
    g_signal_connect(gtk_tree_view_get_selection(lw->notes_list),
                     "changed",
                     G_CALLBACK(on_notes_selection_status), lw);

    /* Built-in drag reordering; the new order is persisted from the
     * model's row-deleted signal.                                          */
    gtk_tree_view_set_reorderable(lw->notes_list, TRUE);
    /* AFTER: the class handler sets a row-snapshot icon; ours overrides.   */
    g_signal_connect_after(lw->notes_list, "drag-begin",
                           G_CALLBACK(on_notes_drag_begin), lw);
    g_signal_connect(lw->notes_list, "row-activated",
                     G_CALLBACK(on_note_list_activated), lw);
    g_signal_connect(lw->notes_list, "button-press-event",
                     G_CALLBACK(on_notes_list_button_press), lw);
    g_signal_connect(lw->notes_list, "button-release-event",
                     G_CALLBACK(on_notes_list_button_release), lw);

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
                     "weight",      PANGO_WEIGHT_BOLD,
                     NULL);
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(lw->notes_grid),
                                   txt, FALSE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(lw->notes_grid),
                                       txt, "text", NL_TITLE, NULL);
    }
    gtk_icon_view_set_item_width(lw->notes_grid, THUMB_SIZE);
    gtk_icon_view_set_selection_mode(lw->notes_grid,
                                     GTK_SELECTION_MULTIPLE);
    g_signal_connect(lw->notes_grid, "selection-changed",
                     G_CALLBACK(on_notes_selection_status), lw);
    gtk_icon_view_enable_model_drag_source(lw->notes_grid,
                                           GDK_BUTTON1_MASK,
                                           &ROW_TARGET, 1,
                                           GDK_ACTION_MOVE);
    g_signal_connect_after(lw->notes_grid, "drag-begin",
                           G_CALLBACK(on_notes_drag_begin), lw);
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

    /* --- status bar: selection path (left) + latest event (right) ----------*/
    lw->status_path = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(lw->status_path), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(lw->status_path),
                            PANGO_ELLIPSIZE_MIDDLE);

    lw->status_event = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(lw->status_event), 1.0);
    gtk_label_set_ellipsize(GTK_LABEL(lw->status_event),
                            PANGO_ELLIPSIZE_MIDDLE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(lw->status_event), "dim-label");

    /* Event messages fade: the label sits in a crossfading revealer that
     * library_notify_status() opens and a timer closes.                     */
    lw->status_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(lw->status_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(lw->status_revealer),
                                         600);
    gtk_container_add(GTK_CONTAINER(lw->status_revealer), lw->status_event);

    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(status_bar, 8);
    gtk_widget_set_margin_end(status_bar, 8);
    gtk_widget_set_margin_top(status_bar, 3);
    gtk_widget_set_margin_bottom(status_bar, 3);
    gtk_box_pack_start(GTK_BOX(status_bar), lw->status_path,
                       TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(status_bar), lw->status_revealer,
                     FALSE, FALSE, 0);

    /* Both labels a step smaller than the UI font.                          */
    {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, "label { font-size: 85%; }",
                                        -1, NULL);
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(lw->status_path),
            GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        gtk_style_context_add_provider(
            gtk_widget_get_style_context(lw->status_event),
            GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
    }

    /* --- assemble -----------------------------------------------------------*/
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    lw->sidebar_paned = paned;
    gtk_paned_pack1(GTK_PANED(paned), sidebar_box, FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), lw->stack, TRUE, FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *menubar = build_menubar(lw);
    /* Remembered so the settings window can move it into the native
     * macOS menu bar (see on_library_apply_native_menubar).                */
    g_object_set_data(G_OBJECT(lw->window), "on-menubar", menubar);
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), build_action_bar(lw),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), status_bar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(lw->window), vbox);

    /* --- initial population -------------------------------------------------*/
    refresh_sidebar(lw);
    refresh_notes(lw);
    on_app_status(app, "DB at %s loaded", app->db->path);

    gtk_widget_show_all(lw->window);

    /* Fit the sidebar pane to its content width on first show.  Done in an
     * idle so the tree view is fully realized and has measured its rows.     */
    g_idle_add(on_sidebar_fit_to_content, lw);

    return lw->window;
}
