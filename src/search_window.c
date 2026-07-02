/* ===========================================================================
 * search_window.c — the note search window (implementation)
 *
 * Each search deserializes candidate notes into an offscreen buffer to
 * obtain their plain text, then matches title-or-body against the query
 * using one of three strategies: plain case-insensitive (casefolded
 * strstr), plain case-sensitive (strstr), or GRegex (with or without
 * G_REGEX_CASELESS).
 * =========================================================================== */

#include "search_window.h"
#include "serialize.h"
#include "editor_window.h"
#include "library_window.h"

#include <string.h>

/* Result-list store columns.                                                */
enum {
    SR_ID,                           /* gint64: note id                     */
    SR_TITLE,                        /* gchar*: note title                  */
    SR_MODIFIED,                     /* gchar*: formatted updated_at        */
    SR_N_COLS
};

/* ---------------------------------------------------------------------------
 * OnSearch — all state for one search window.
 *
 * Fields:
 *   app           — global application context (not owned).
 *   window        — the search window itself.
 *   entry         — the query text entry.
 *   radio_all     — "All Notes" scope radio button.
 *   radio_scoped  — "Selected Folder/Tag" radio button; resolved against
 *                   the library's live selection on every search.
 *   check_case    — "Case sensitive" checkbox.
 *   check_regex   — "Regular expression" checkbox.
 *   store         — results list model.
 *   status        — label under the results showing match counts/errors.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnApp         *app;
    GtkWidget     *window;
    GtkWidget     *entry;
    GtkWidget     *radio_all;
    GtkWidget     *radio_scoped;
    GtkWidget     *check_case;
    GtkWidget     *check_regex;
    GtkListStore  *store;
    GtkWidget     *status;
} OnSearch;

/* ---------------------------------------------------------------------------
 * note_plain_text() — load a note and flatten it to plain text (title is
 * matched separately by the caller).  Returns a newly allocated string.
 * ------------------------------------------------------------------------- */
static gchar *
note_plain_text(OnApp *app, gint64 note_id)
{
    gsize   blob_len = 0;            /* stored blob size                    */
    guint8 *blob = on_db_note_load(app->db, note_id, &blob_len);
    if (blob == NULL)
        return g_strdup("");

    GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
    on_buffer_ensure_tags(buffer);
    on_note_deserialize(buffer, blob, blob_len);
    g_free(blob);

    GtkTextIter start, end;          /* full buffer bounds                  */
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    g_object_unref(buffer);
    return text;
}

/* ---------------------------------------------------------------------------
 * text_matches() — does `haystack` contain `needle` under the plain
 * (non-regex) rules?
 *   case_sensitive — FALSE casefolds both sides first.
 * ------------------------------------------------------------------------- */
static gboolean
text_matches(const gchar *haystack, const gchar *needle,
             gboolean case_sensitive)
{
    if (case_sensitive)
        return strstr(haystack, needle) != NULL;

    gchar *h = g_utf8_casefold(haystack, -1);
    gchar *n = g_utf8_casefold(needle, -1);
    gboolean hit = strstr(h, n) != NULL;
    g_free(h);
    g_free(n);
    return hit;
}

/* ---------------------------------------------------------------------------
 * run_search() — execute the query and fill the results list.
 * ------------------------------------------------------------------------- */
static void
run_search(OnSearch *sw)
{
    const gchar *query = gtk_entry_get_text(GTK_ENTRY(sw->entry));
    gtk_list_store_clear(sw->store);
    if (query == NULL || *query == '\0') {
        gtk_label_set_text(GTK_LABEL(sw->status), "Type something to search for.");
        return;
    }

    gboolean case_sensitive = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(sw->check_case));
    gboolean use_regex = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(sw->check_regex));
    gboolean scoped = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(sw->radio_scoped));

    /* Compile the regex up front so a bad pattern errors immediately.      */
    GRegex *regex = NULL;            /* compiled pattern (regex mode only)  */
    if (use_regex) {
        GError *err = NULL;
        regex = g_regex_new(query,
                            case_sensitive ? 0 : G_REGEX_CASELESS,
                            0, &err);
        if (regex == NULL) {
            gchar *msg = g_strdup_printf("Bad pattern: %s", err->message);
            gtk_label_set_text(GTK_LABEL(sw->status), msg);
            g_free(msg);
            g_clear_error(&err);
            return;
        }
    }

    /* Candidate notes per the scope radio.  The scoped variant reads the
     * library's selection NOW, so it always matches what is highlighted
     * in the sidebar at the moment Search is pressed.                      */
    GList *notes;                    /* OnNoteMeta* candidates              */
    gchar *scope_desc = NULL;        /* "in <name>" suffix for the status   */
    if (!scoped) {
        notes = on_db_note_list_all(sw->app->db);
    } else {
        OnSearchScope scope;         /* live library scope                  */
        gint64 scope_id;             /* live folder/tag id                  */
        gchar *scope_name;           /* live selection name                 */
        on_library_get_scope(sw->app, &scope, &scope_id, &scope_name);
        if (scope == ON_SCOPE_TAG)
            notes = on_db_notes_by_tag(sw->app->db, scope_id);
        else
            notes = on_db_note_list(sw->app->db, scope_id);
        scope_desc = g_strdup_printf(
            " in \xe2\x80\x9c%s\xe2\x80\x9d", scope_name);
        g_free(scope_name);
    }

    gint hits = 0;                   /* matching notes                      */
    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;     /* one candidate                       */
        gchar *body = note_plain_text(sw->app, m->id);

        gboolean match;              /* does this note match the query?     */
        if (regex != NULL)
            match = g_regex_match(regex, m->title, 0, NULL) ||
                    g_regex_match(regex, body, 0, NULL);
        else
            match = text_matches(m->title, query, case_sensitive) ||
                    text_matches(body, query, case_sensitive);
        g_free(body);
        if (!match)
            continue;

        GDateTime *dt = g_date_time_new_from_unix_local(m->updated_at);
        gchar *when = g_date_time_format(dt, "%b %e, %Y %H:%M");
        g_date_time_unref(dt);

        GtkTreeIter iter;
        gtk_list_store_append(sw->store, &iter);
        gtk_list_store_set(sw->store, &iter,
                           SR_ID,       m->id,
                           SR_TITLE,    m->title,
                           SR_MODIFIED, when,
                           -1);
        g_free(when);
        hits++;
    }
    on_db_note_list_free(notes);
    if (regex != NULL)
        g_regex_unref(regex);

    gchar *msg = g_strdup_printf("%d match%s%s", hits,
                                 hits == 1 ? "" : "es",
                                 scope_desc != NULL ? scope_desc : "");
    gtk_label_set_text(GTK_LABEL(sw->status), msg);
    g_free(msg);
    g_free(scope_desc);
}

/* on_search_clicked() / on_entry_activate() — both trigger the search.      */
static void
on_search_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    run_search((OnSearch *)user_data);
}

static void
on_entry_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    run_search((OnSearch *)user_data);
}

/* on_result_activated() — double-click/Enter on a result opens the note.    */
static void
on_result_activated(GtkTreeView *view, GtkTreePath *path,
                    GtkTreeViewColumn *col, gpointer user_data)
{
    (void)view; (void)col;
    OnSearch *sw = user_data;        /* owning search window                */
    GtkTreeIter iter;                /* activated row                       */
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(sw->store), &iter, path))
        return;
    gint64 id;                       /* note id of the row                  */
    gtk_tree_model_get(GTK_TREE_MODEL(sw->store), &iter, SR_ID, &id, -1);
    on_editor_window_open(sw->app, id);
}

/* on_search_destroy() — free the state struct with the window.              */
static void
on_search_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    g_free(user_data);
}

void
on_search_window_open(OnApp *app, gboolean scope_to_sel)
{
    OnSearch *sw = g_new0(OnSearch, 1);
    sw->app = app;

    /* --- window (standard titlebar) --------------------------------------*/
    sw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(sw->window), "Orange Notes - Search");
    gtk_window_set_default_size(GTK_WINDOW(sw->window), 680, 460);
    gtk_window_set_transient_for(GTK_WINDOW(sw->window),
                                 GTK_WINDOW(app->library_window));
    g_signal_connect(sw->window, "destroy",
                     G_CALLBACK(on_search_destroy), sw);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(sw->window), vbox);

    /* --- query row --------------------------------------------------------*/
    GtkWidget *query_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    sw->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(sw->entry),
                                   "Search titles and note text\xe2\x80\xa6");
    g_signal_connect(sw->entry, "activate",
                     G_CALLBACK(on_entry_activate), sw);
    gtk_box_pack_start(GTK_BOX(query_row), sw->entry, TRUE, TRUE, 0);

    GtkWidget *btn = gtk_button_new_with_label("Search");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_search_clicked), sw);
    gtk_box_pack_start(GTK_BOX(query_row), btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), query_row, FALSE, FALSE, 0);

    /* --- scope radios -------------------------------------------------------*/
    GtkWidget *scope_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    sw->radio_all = gtk_radio_button_new_with_label(NULL, "All Notes");
    gtk_box_pack_start(GTK_BOX(scope_row), sw->radio_all, FALSE, FALSE, 0);

    sw->radio_scoped = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(sw->radio_all), "Selected Folder/Tag");
    gtk_widget_set_tooltip_text(sw->radio_scoped,
        "Search only whatever folder or tag is selected in the library "
        "when you press Search");
    gtk_box_pack_start(GTK_BOX(scope_row), sw->radio_scoped,
                       FALSE, FALSE, 0);
    if (scope_to_sel)
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(sw->radio_scoped), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), scope_row, FALSE, FALSE, 0);

    /* --- matching options ---------------------------------------------------*/
    GtkWidget *opt_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    sw->check_case  = gtk_check_button_new_with_label("Case sensitive");
    sw->check_regex = gtk_check_button_new_with_label("Regular expression");
    gtk_box_pack_start(GTK_BOX(opt_row), sw->check_case,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opt_row), sw->check_regex, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), opt_row, FALSE, FALSE, 0);

    /* --- results -------------------------------------------------------------*/
    sw->store = gtk_list_store_new(SR_N_COLS,
                                   G_TYPE_INT64,    /* SR_ID               */
                                   G_TYPE_STRING,   /* SR_TITLE            */
                                   G_TYPE_STRING);  /* SR_MODIFIED         */

    GtkWidget *results = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(sw->store));
    g_object_unref(sw->store);       /* the view holds the ref now          */
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(results),
        gtk_tree_view_column_new_with_attributes(
            "Title", gtk_cell_renderer_text_new(),
            "text", SR_TITLE, NULL));
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(results),
        gtk_tree_view_column_new_with_attributes(
            "Modified", gtk_cell_renderer_text_new(),
            "text", SR_MODIFIED, NULL));
    gtk_tree_view_column_set_expand(
        gtk_tree_view_get_column(GTK_TREE_VIEW(results), 0), TRUE);
    g_signal_connect(results, "row-activated",
                     G_CALLBACK(on_result_activated), sw);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll),
                                              FALSE);
    gtk_container_add(GTK_CONTAINER(scroll), results);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    /* --- status line -----------------------------------------------------------*/
    sw->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(sw->status), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), sw->status, FALSE, FALSE, 0);

    gtk_widget_show_all(sw->window);
    gtk_widget_grab_focus(sw->entry);
}
