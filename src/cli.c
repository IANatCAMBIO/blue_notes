/* ===========================================================================
 * cli.c — command-line automation interface (implementation)
 *
 * See cli.h for the command list.  Commands print plain, tab-separated
 * text (easy to consume from shell scripts) and exit with:
 *   0 — success
 *   1 — usage error (bad command/arguments)
 *   2 — operation failed (missing folder, database error, …)
 *
 * The database is resolved exactly like the GUI resolves it: the custom
 * location from blue_notes.ini (next to the binary) when set, otherwise
 * the default per-user path.
 * =========================================================================== */

#include "cli.h"
#include "app.h"
#include "db.h"
#include "export.h"
#include "ipc.h"
#include "serialize.h"

#include <stdio.h>
#include <string.h>

/* Stdin substitute used while a command runs inside a GUI instance on behalf
 * of a remote CLI (see on_cli_set_stdin_data): the instance has no access to
 * the CLI's stdin, so "note new -" reads this pre-slurped text instead.  NULL
 * in the normal headless path, where the real stdin is read directly.        */
static const gchar *cli_stdin_data = NULL;

void
on_cli_set_stdin_data(const gchar *data)
{
    cli_stdin_data = data;
}

/* ---------------------------------------------------------------------------
 * cli_open_db() — open the same database the GUI would use.
 * Returns the handle, or NULL after printing an error.
 * ------------------------------------------------------------------------- */
static OnDatabase *
cli_open_db(void)
{
    gchar *db_dir = on_app_config_load_db_dir();
    gchar *path = (db_dir != NULL)
                  ? g_build_filename(db_dir, ON_DB_FILENAME, NULL)
                  : NULL;
    OnDatabase *db = on_db_open(path);
    if (db == NULL) {
        fprintf(stderr, "error: cannot open database%s%s\n",
                path != NULL ? " at " : "", path != NULL ? path : "");
    }
    g_free(path);
    g_free(db_dir);
    return db;
}

/* ---------------------------------------------------------------------------
 * on_cli_resolve_folder_path() — walk "A/B/C" through the folder tree.
 * (See cli.h — exported so the IPC note-path resolver can reuse it.)
 * ------------------------------------------------------------------------- */
gboolean
on_cli_resolve_folder_path(OnDatabase *db, const gchar *path, gboolean create,
                           gint64 *out_id)
{
    *out_id = 0;
    if (path == NULL || *path == '\0' || g_strcmp0(path, "/") == 0)
        return TRUE;

    gchar **parts = g_strsplit(path, "/", -1);
    gint64 parent = 0;               /* id of the folder walked so far      */
    gboolean ok = TRUE;              /* did every component resolve?        */

    for (gsize i = 0; ok && parts[i] != NULL; i++) {
        if (*parts[i] == '\0')
            continue;                /* tolerate leading//trailing slashes  */

        /* Find a child of `parent` with this name.                         */
        gint64 found = 0;            /* matching child folder id            */
        GList *children = on_db_folder_list(db, parent);
        for (GList *l = children; l != NULL; l = l->next) {
            OnFolder *f = l->data;
            if (g_strcmp0(f->name, parts[i]) == 0) {
                found = f->id;
                break;
            }
        }
        on_db_folder_list_free(children);

        if (found == 0 && create)
            found = on_db_folder_create(db, parent, parts[i]);
        if (found == 0)
            ok = FALSE;
        parent = found;
    }
    g_strfreev(parts);
    *out_id = parent;
    return ok;
}

/* ---------------------------------------------------------------------------
 * note_from_arg() — parse a note-id argument and fetch its metadata.
 *   db  — open database.
 *   arg — the id string as typed on the command line.
 * Returns an OnNoteMeta* (free with on_db_note_meta_free), or NULL after
 * printing "error: no such note: <arg>" to stderr when the argument is
 * not a positive number or names no note.
 * ------------------------------------------------------------------------- */
static OnNoteMeta *
note_from_arg(OnDatabase *db, const gchar *arg)
{
    gint64 id = g_ascii_strtoll(arg, NULL, 10);
    OnNoteMeta *meta = (id > 0) ? on_db_note_get(db, id) : NULL;
    if (meta == NULL)
        fprintf(stderr, "error: no such note: %s\n", arg);
    return meta;
}

/* ---------------------------------------------------------------------------
 * cli_require_gtk() — initialize GTK (windowless), needed by any command
 * that (de)serializes note content.  Inside a GUI instance running a
 * remote command this is a no-op (GTK is already up).
 * Returns TRUE on success, FALSE after printing an error.
 * ------------------------------------------------------------------------- */
static gboolean
cli_require_gtk(void)
{
    if (gtk_init_check(NULL, NULL))
        return TRUE;
    fprintf(stderr, "error: GTK could not initialize (needed to "
                    "process note content)\n");
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * cli_read_content() — resolve a CLI content argument: the literal text,
 * or stdin when `arg` is "-" (the pre-slurped cli_stdin_data when running
 * inside a GUI instance on behalf of a remote CLI).
 * Returns owned, UTF-8-validated text, or NULL after printing an error.
 * ------------------------------------------------------------------------- */
static gchar *
cli_read_content(const gchar *arg)
{
    gchar *text;                     /* the resolved content (owned)        */
    if (g_strcmp0(arg, "-") == 0) {
        if (cli_stdin_data != NULL) {
            text = g_strdup(cli_stdin_data);
        } else {
            GString *in = g_string_new(NULL);
            gchar buf[4096];
            gsize n;
            while ((n = fread(buf, 1, sizeof buf, stdin)) > 0)
                g_string_append_len(in, buf, (gssize)n);
            text = g_string_free(in, FALSE);
        }
    } else {
        text = g_strdup(arg);
    }
    if (!g_utf8_validate(text, -1, NULL)) {
        fprintf(stderr, "error: content is not valid UTF-8\n");
        g_free(text);
        return NULL;
    }
    return text;
}

/* ---------------------------------------------------------------------------
 * note_buffer_load() — deserialize note `id`'s stored content into a new
 * offscreen GtkTextBuffer (empty buffer if the note has none yet).
 * Requires GTK (cli_require_gtk).  Returns the buffer; g_object_unref() it.
 * ------------------------------------------------------------------------- */
static GtkTextBuffer *
note_buffer_load(OnDatabase *db, gint64 id)
{
    GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
    on_buffer_ensure_tags(buffer);
    gsize   blob_len = 0;            /* stored blob size                    */
    guint8 *blob = on_db_note_load(db, id, &blob_len);
    if (blob != NULL) {
        on_note_deserialize(buffer, blob, blob_len);
        g_free(blob);
    }
    return buffer;
}

/* ---------------------------------------------------------------------------
 * note_buffer_save() — serialize `buffer` and persist it as note `id`'s
 * content: title from the first line, searchable body text refreshed —
 * the same trio every editor save writes.  Returns TRUE on success.
 * ------------------------------------------------------------------------- */
static gboolean
note_buffer_save(OnDatabase *db, gint64 id, GtkTextBuffer *buffer)
{
    gsize blob_len;                  /* serialized content size             */
    guint8 *blob = on_note_serialize(buffer, &blob_len);
    gchar *title = on_buffer_first_line(buffer);
    gchar *body = on_note_extract_text(blob, blob_len);
    gboolean ok = on_db_note_save(db, id, title, blob, blob_len, body);
    g_free(body);
    g_free(title);
    g_free(blob);
    return ok;
}

/* ---------------------------------------------------------------------------
 * note_body_text_cached() — a note's plain text: the body_text cache when
 * filled, otherwise extracted from the BNBF blob (no GTK, images skipped)
 * and written back — the same read path cross-note search uses (see
 * note_plain_text in search_window.c).  Returns a new string; g_free() it.
 * ------------------------------------------------------------------------- */
static gchar *
note_body_text_cached(OnDatabase *db, gint64 id)
{
    gchar *cached = on_db_note_body_text(db, id);
    if (cached != NULL)
        return cached;

    gsize   blob_len = 0;            /* stored blob size                    */
    guint8 *blob = on_db_note_load(db, id, &blob_len);
    if (blob == NULL)
        return g_strdup("");

    gchar *text = on_note_extract_text(blob, blob_len);
    g_free(blob);
    on_db_note_set_body_text(db, id, text);
    return text;
}

/* ---------------------------------------------------------------------------
 * tag_name_valid() — TRUE when `name` is a non-empty run of the characters
 * a #tag token may contain (the editor's tag_capture_span rule: letters,
 * digits, '_', '-').
 * ------------------------------------------------------------------------- */
static gboolean
tag_name_valid(const gchar *name)
{
    if (name == NULL || *name == '\0')
        return FALSE;
    for (const gchar *p = name; *p != '\0'; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (!(g_unichar_isalnum(c) || c == '_' || c == '-'))
            return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * cmd_list_tags() — one tag per line: "name<TAB>note-count".
 * ------------------------------------------------------------------------- */
static int
cmd_list_tags(OnDatabase *db)
{
    GHashTable *counts = on_db_tag_count_map(db);   /* tag id → note count  */
    GList *tags = on_db_tag_list(db);
    for (GList *l = tags; l != NULL; l = l->next) {
        OnTag *t = l->data;
        printf("%s\t%d\n", t->name,
               GPOINTER_TO_INT(g_hash_table_lookup(counts, &t->id)));
    }
    on_db_tag_list_free(tags);
    g_hash_table_destroy(counts);
    return 0;
}

/* cmd_delete_tag() — remove one tag by name.                                */
static int
cmd_delete_tag(OnDatabase *db, const gchar *name)
{
    /* Accept the name with or without the leading '#'.                     */
    if (*name == '#')
        name++;
    gint64 id = on_db_tag_find(db, name);
    if (id == 0) {
        fprintf(stderr, "error: no such tag: %s\n", name);
        return 2;
    }
    if (!on_db_tag_delete(db, id)) {
        fprintf(stderr, "error: could not delete tag %s\n", name);
        return 2;
    }
    printf("deleted tag %s\n", name);
    return 0;
}

/* Forward declaration (defined with the other note-line printers below).   */
static void print_note_line(OnNoteMeta *m, const gchar *folder_path);

/* cmd_tag_notes() — list every note labeled with one tag.                   */
static int
cmd_tag_notes(OnDatabase *db, const gchar *name)
{
    /* Accept the name with or without the leading '#'.                     */
    if (*name == '#')
        name++;
    gint64 id = on_db_tag_find(db, name);
    if (id == 0) {
        fprintf(stderr, "error: no such tag: %s\n", name);
        return 2;
    }
    GList *notes = on_db_notes_by_tag(db, id);
    for (GList *l = notes; l != NULL; l = l->next)
        print_note_line(l->data, NULL);
    on_db_note_list_free(notes);
    return 0;
}

/* ---------------------------------------------------------------------------
 * print_folder_tree() — recursive indented listing with note counts.
 *   counts — folder id → note count map (on_db_note_count_map), fetched
 *            once by cmd_list_folders; a missing key means zero.
 * ------------------------------------------------------------------------- */
static void
print_folder_tree(OnDatabase *db, GHashTable *counts, gint64 parent,
                  gint depth)
{
    GList *folders = on_db_folder_list(db, parent);
    for (GList *l = folders; l != NULL; l = l->next) {
        OnFolder *f = l->data;
        printf("%*s%s\t%d\n", depth * 2, "", f->name,
               GPOINTER_TO_INT(g_hash_table_lookup(counts, &f->id)));
        print_folder_tree(db, counts, f->id, depth + 1);
    }
    on_db_folder_list_free(folders);
}

static int
cmd_list_folders(OnDatabase *db)
{
    GHashTable *counts = on_db_note_count_map(db);  /* one query, not per row */
    print_folder_tree(db, counts, 0, 0);
    g_hash_table_destroy(counts);
    return 0;
}

/* cmd_add_folder() — create a (possibly nested) folder path.                */
static int
cmd_add_folder(OnDatabase *db, const gchar *path)
{
    gint64 id;                       /* the created/found folder            */
    if (!on_cli_resolve_folder_path(db, path, TRUE, &id) || id == 0) {
        fprintf(stderr, "error: could not create folder %s\n", path);
        return 2;
    }
    printf("folder %s (id %" G_GINT64_FORMAT ")\n", path, id);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_delete_folder() — move a folder (with its whole subtree) to the
 * Trash, or with `permanent` delete it and its contents outright.
 * ------------------------------------------------------------------------- */
static int
cmd_delete_folder(OnDatabase *db, const gchar *path, gboolean permanent)
{
    gint64 id;                       /* the folder to delete                */
    if (!on_cli_resolve_folder_path(db, path, FALSE, &id) || id == 0) {
        fprintf(stderr, "error: no such folder: %s\n", path);
        return 2;
    }
    gboolean ok = permanent ? on_db_folder_delete(db, id)
                            : on_db_folder_trash(db, id);
    if (!ok) {
        fprintf(stderr, "error: could not %s folder %s\n",
                permanent ? "delete" : "trash", path);
        return 2;
    }
    printf(permanent ? "deleted folder %s (and its contents)\n"
                     : "trashed folder %s (and its contents)\n", path);
    return 0;
}

/* ---------------------------------------------------------------------------
 * print_note_line() — "ID<TAB>MODIFIED<TAB>TITLE", or with a non-NULL
 * `folder_path` (the "Folder/Sub" form on_db_folder_path_map yields; ""
 * for the top level) "ID<TAB>MODIFIED<TAB>/Folder/Sub/TITLE".
 * ------------------------------------------------------------------------- */
static void
print_note_line(OnNoteMeta *m, const gchar *folder_path)
{
    GDateTime *dt = g_date_time_new_from_unix_local(m->updated_at);
    gchar *when = g_date_time_format(dt, "%Y-%m-%d %H:%M");
    g_date_time_unref(dt);
    if (folder_path != NULL)
        printf("%" G_GINT64_FORMAT "\t%s\t/%s%s%s\n", m->id, when,
               folder_path, *folder_path != '\0' ? "/" : "", m->title);
    else
        printf("%" G_GINT64_FORMAT "\t%s\t%s\n", m->id, when, m->title);
    g_free(when);
}

/* cmd_list_notes() — notes in one folder, or every note with --all.         */
static int
cmd_list_notes(OnDatabase *db, const gchar *path)
{
    GList *notes;                    /* the OnNoteMeta* result set          */
    if (g_strcmp0(path, "--all") == 0) {
        notes = on_db_note_list_all(db, FALSE);
    } else {
        gint64 folder;               /* resolved folder id                  */
        if (!on_cli_resolve_folder_path(db, path, FALSE, &folder)) {
            fprintf(stderr, "error: no such folder: %s\n", path);
            return 2;
        }
        notes = on_db_note_list(db, folder);
    }
    for (GList *l = notes; l != NULL; l = l->next)
        print_note_line(l->data, NULL);
    on_db_note_list_free(notes);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_new_note() — create a note from CLI-supplied content.
 *   folder_path — destination ("" = top level; must already exist).
 *   content     — the note text, or "-" to read stdin.
 * Serializing needs GTK's text buffer machinery, so GTK is initialized
 * (windowless) for this command.
 * ------------------------------------------------------------------------- */
static int
cmd_new_note(OnDatabase *db, const gchar *folder_path, const gchar *content)
{
    gint64 folder;                   /* destination folder id               */
    if (!on_cli_resolve_folder_path(db, folder_path, FALSE, &folder)) {
        fprintf(stderr, "error: no such folder: %s "
                        "(create it with 'folder add')\n", folder_path);
        return 2;
    }

    gchar *text = cli_read_content(content);   /* the note body (owned)     */
    if (text == NULL)
        return 2;
    if (!cli_require_gtk()) {
        g_free(text);
        return 2;
    }

    gint64 id = on_db_note_create(db, folder);
    if (id == 0) {
        fprintf(stderr, "error: could not create note\n");
        g_free(text);
        return 2;
    }

    GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
    on_buffer_ensure_tags(buffer);
    gtk_text_buffer_set_text(buffer, text, -1);
    note_buffer_save(db, id, buffer);

    gchar *title = on_buffer_first_line(buffer);
    printf("note %" G_GINT64_FORMAT "\t%s\n", id, title);

    g_free(title);
    g_object_unref(buffer);
    g_free(text);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_cat_note() — print a note's content to stdout.
 *   markdown — FALSE: the cached plain text (no GTK needed);
 *              TRUE:  a Markdown render with formatting preserved
 *                     (images become "![image N]()" placeholders).
 * ------------------------------------------------------------------------- */
static int
cmd_cat_note(OnDatabase *db, const gchar *id_str, gboolean markdown)
{
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;

    gchar *text;                     /* what gets printed (owned)           */
    if (markdown) {
        if (!cli_require_gtk()) {
            on_db_note_meta_free(meta);
            return 2;
        }
        /* The exporter only touches app->db.                               */
        OnApp app = { 0 };
        app.db = db;
        text = on_export_note_markdown(&app, meta->id);
    } else {
        text = note_body_text_cached(db, meta->id);
    }

    fputs(text, stdout);
    if (*text == '\0' || text[strlen(text) - 1] != '\n')
        putchar('\n');

    g_free(text);
    on_db_note_meta_free(meta);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_append_note() — append plain text to an existing note, on a fresh
 * line (existing content, formatting and images are untouched).
 * ------------------------------------------------------------------------- */
static int
cmd_append_note(OnDatabase *db, const gchar *id_str, const gchar *content)
{
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;
    gchar *text = cli_read_content(content);   /* text to append (owned)    */
    if (text == NULL || !cli_require_gtk()) {
        g_free(text);
        on_db_note_meta_free(meta);
        return 2;
    }

    GtkTextBuffer *buffer = note_buffer_load(db, meta->id);
    GtkTextIter end;                 /* append position                     */
    gtk_text_buffer_get_end_iter(buffer, &end);
    if (gtk_text_buffer_get_char_count(buffer) > 0 &&
        !gtk_text_iter_starts_line(&end))
        gtk_text_buffer_insert(buffer, &end, "\n", -1);
    gtk_text_buffer_insert(buffer, &end, text, -1);

    int rc = 0;                      /* process exit code                   */
    if (note_buffer_save(db, meta->id, buffer)) {
        printf("appended to note %" G_GINT64_FORMAT "\t%s\n",
               meta->id, meta->title);
    } else {
        fprintf(stderr, "error: could not save note %" G_GINT64_FORMAT "\n",
                meta->id);
        rc = 2;
    }
    g_object_unref(buffer);
    g_free(text);
    on_db_note_meta_free(meta);
    return rc;
}

/* ---------------------------------------------------------------------------
 * cmd_set_note() — REPLACE a note's content with plain text.  Anything the
 * old content held (formatting, images, tables, checkboxes, #tags) is
 * gone, exactly like select-all + retype in the editor — so the note's
 * tag links are rewritten to the (empty) tag set of the new text.
 * ------------------------------------------------------------------------- */
static int
cmd_set_note(OnDatabase *db, const gchar *id_str, const gchar *content)
{
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;
    gchar *text = cli_read_content(content);   /* the new body (owned)      */
    if (text == NULL || !cli_require_gtk()) {
        g_free(text);
        on_db_note_meta_free(meta);
        return 2;
    }

    GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
    on_buffer_ensure_tags(buffer);
    gtk_text_buffer_set_text(buffer, text, -1);

    int rc = 0;                      /* process exit code                   */
    if (note_buffer_save(db, meta->id, buffer)) {
        on_db_note_set_tags(db, meta->id, NULL);   /* plain text: no tags   */
        gchar *title = on_buffer_first_line(buffer);
        printf("set note %" G_GINT64_FORMAT "\t%s\n", meta->id, title);
        g_free(title);
    } else {
        fprintf(stderr, "error: could not save note %" G_GINT64_FORMAT "\n",
                meta->id);
        rc = 2;
    }
    g_object_unref(buffer);
    g_free(text);
    on_db_note_meta_free(meta);
    return rc;
}

/* ---------------------------------------------------------------------------
 * cmd_add_image() — append an image file to an existing note.  The image
 * is stored at full resolution (same as pasting it in the editor) and
 * displayed at the default thumbnail width.
 * ------------------------------------------------------------------------- */
static int
cmd_add_image(OnDatabase *db, const gchar *id_str, const gchar *file)
{
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;
    gint64 id = meta->id;            /* validated note id                   */

    if (!cli_require_gtk()) {
        on_db_note_meta_free(meta);
        return 2;
    }

    GError *err = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(file, &err);
    if (pixbuf == NULL) {
        fprintf(stderr, "error: cannot load image %s: %s\n",
                file, err->message);
        g_clear_error(&err);
        on_db_note_meta_free(meta);
        return 2;
    }

    /* Load the note, append the image on a fresh line, save it back.       */
    GtkTextBuffer *buffer = note_buffer_load(db, id);

    GtkTextIter end;                 /* append position                     */
    gtk_text_buffer_get_end_iter(buffer, &end);
    if (gtk_text_buffer_get_char_count(buffer) > 0 &&
        !gtk_text_iter_starts_line(&end))
        gtk_text_buffer_insert(buffer, &end, "\n", -1);
    GtkTextChildAnchor *anchor =
        gtk_text_buffer_create_child_anchor(buffer, &end);
    on_anchor_set_image(anchor, pixbuf, 0);

    note_buffer_save(db, id, buffer);
    gchar *title = on_buffer_first_line(buffer);
    printf("added image to note %" G_GINT64_FORMAT "\t%s\n", id, title);

    g_free(title);
    g_object_unref(buffer);
    g_object_unref(pixbuf);
    on_db_note_meta_free(meta);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_note_tags() — print a note's tag names, one per line.
 * ------------------------------------------------------------------------- */
static int
cmd_note_tags(OnDatabase *db, const gchar *id_str)
{
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;
    GList *tags = on_db_note_tag_list(db, meta->id);
    for (GList *l = tags; l != NULL; l = l->next)
        printf("%s\n", ((OnTag *)l->data)->name);
    on_db_tag_list_free(tags);
    on_db_note_meta_free(meta);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_tag_note() — label a note with a #tag the way the editor does: the
 * literal "#name" token is appended to the note text (under the on-tag
 * text tag, so it survives GUI edits and re-saves), and the note's tag
 * links are rewritten from the buffer.  A tag living only in note_tags
 * would be silently dropped by the next tag-touching GUI save.
 * ------------------------------------------------------------------------- */
static int
cmd_tag_note(OnDatabase *db, const gchar *id_str, const gchar *name)
{
    /* Accept the name with or without the leading '#'.                     */
    if (*name == '#')
        name++;
    if (!tag_name_valid(name)) {
        fprintf(stderr, "error: bad tag name: %s (letters, digits, "
                        "'_' and '-' only)\n", name);
        return 2;
    }
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;
    if (!cli_require_gtk()) {
        on_db_note_meta_free(meta);
        return 2;
    }

    GtkTextBuffer *buffer = note_buffer_load(db, meta->id);
    GList *existing = on_buffer_collect_tags(buffer);
    int rc = 0;                      /* process exit code                   */

    if (g_list_find_custom(existing, name, (GCompareFunc)g_strcmp0) != NULL) {
        /* Already in the text; just make sure the db link agrees.          */
        on_db_note_set_tags(db, meta->id, existing);
        printf("note %" G_GINT64_FORMAT " already tagged #%s\n",
               meta->id, name);
    } else {
        GtkTextIter end;             /* append position                     */
        gtk_text_buffer_get_end_iter(buffer, &end);
        if (gtk_text_buffer_get_char_count(buffer) > 0 &&
            !gtk_text_iter_starts_line(&end))
            gtk_text_buffer_insert(buffer, &end, " ", -1);
        gchar *token = g_strdup_printf("#%s", name);
        gtk_text_buffer_insert_with_tags_by_name(buffer, &end, token, -1,
                                                 ON_TAGNAME_TAG, NULL);
        g_free(token);

        GList *tags = on_buffer_collect_tags(buffer);
        if (note_buffer_save(db, meta->id, buffer) &&
            on_db_note_set_tags(db, meta->id, tags)) {
            printf("tagged note %" G_GINT64_FORMAT "\t#%s\n",
                   meta->id, name);
        } else {
            fprintf(stderr, "error: could not tag note %" G_GINT64_FORMAT
                    "\n", meta->id);
            rc = 2;
        }
        g_list_free_full(tags, g_free);
    }

    g_list_free_full(existing, g_free);
    g_object_unref(buffer);
    on_db_note_meta_free(meta);
    return rc;
}

/* ---------------------------------------------------------------------------
 * cmd_untag_note() — remove a #tag from a note: every "#name" token is
 * deleted from the note text (plus one separating space before it) and
 * the tag links are rewritten from the remaining buffer tags.  A tag
 * linked in note_tags but absent from the text is unlinked directly.
 * ------------------------------------------------------------------------- */
static int
cmd_untag_note(OnDatabase *db, const gchar *id_str, const gchar *name)
{
    if (*name == '#')
        name++;
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;
    if (!cli_require_gtk()) {
        on_db_note_meta_free(meta);
        return 2;
    }

    GtkTextBuffer *buffer = note_buffer_load(db, meta->id);
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, ON_TAGNAME_TAG);

    /* Collect [start,end) offset pairs of every matching "#name" span
     * (ascending, the walk on_buffer_collect_tags uses), then delete them
     * back-to-front so earlier offsets stay valid.                          */
    GArray *spans = g_array_new(FALSE, FALSE, sizeof(gint));
    GtkTextIter iter;                /* scan position                       */
    gtk_text_buffer_get_start_iter(buffer, &iter);
    while (tag != NULL) {
        if (!gtk_text_iter_starts_tag(&iter, tag)) {
            if (!gtk_text_iter_forward_to_tag_toggle(&iter, tag))
                break;
            if (!gtk_text_iter_starts_tag(&iter, tag))
                continue;
        }
        GtkTextIter span_end = iter; /* end of this tag span                */
        gtk_text_iter_forward_to_tag_toggle(&span_end, tag);

        gchar *text = gtk_text_buffer_get_text(buffer, &iter, &span_end,
                                               FALSE);
        g_strstrip(text);
        const gchar *span_name = (*text == '#') ? text + 1 : text;
        if (g_strcmp0(span_name, name) == 0) {
            gint a = gtk_text_iter_get_offset(&iter);
            gint b = gtk_text_iter_get_offset(&span_end);
            GtkTextIter before = iter;   /* eat one separating space        */
            if (gtk_text_iter_backward_char(&before) &&
                gtk_text_iter_get_char(&before) == ' ')
                a--;
            g_array_append_val(spans, a);
            g_array_append_val(spans, b);
        }
        g_free(text);
        iter = span_end;
    }

    int rc = 0;                      /* process exit code                   */
    if (spans->len == 0) {
        /* Not in the text; drop a db-only link if one exists.              */
        GList *linked = on_db_note_tag_list(db, meta->id);
        GList *keep = NULL;          /* names to keep (borrowed strings)    */
        gboolean had = FALSE;        /* was the tag linked at all?          */
        for (GList *l = linked; l != NULL; l = l->next) {
            OnTag *t = l->data;
            if (g_strcmp0(t->name, name) == 0)
                had = TRUE;
            else
                keep = g_list_prepend(keep, t->name);
        }
        if (had && on_db_note_set_tags(db, meta->id, keep)) {
            printf("untagged note %" G_GINT64_FORMAT "\t#%s\n",
                   meta->id, name);
        } else {
            fprintf(stderr, "error: note %" G_GINT64_FORMAT
                    " has no tag #%s\n", meta->id, name);
            rc = 2;
        }
        g_list_free(keep);
        on_db_tag_list_free(linked);
    } else {
        for (guint i = spans->len; i >= 2; i -= 2) {
            GtkTextIter a, b;        /* span bounds to delete               */
            gtk_text_buffer_get_iter_at_offset(buffer, &a,
                g_array_index(spans, gint, i - 2));
            gtk_text_buffer_get_iter_at_offset(buffer, &b,
                g_array_index(spans, gint, i - 1));
            gtk_text_buffer_delete(buffer, &a, &b);
        }
        GList *tags = on_buffer_collect_tags(buffer);
        if (note_buffer_save(db, meta->id, buffer) &&
            on_db_note_set_tags(db, meta->id, tags)) {
            printf("untagged note %" G_GINT64_FORMAT "\t#%s\n",
                   meta->id, name);
        } else {
            fprintf(stderr, "error: could not untag note %" G_GINT64_FORMAT
                    "\n", meta->id);
            rc = 2;
        }
        g_list_free_full(tags, g_free);
    }

    g_array_free(spans, TRUE);
    g_object_unref(buffer);
    on_db_note_meta_free(meta);
    return rc;
}

/* ---------------------------------------------------------------------------
 * cmd_delete_notes() — move the given note ids to the Trash (the GUI's
 * soft delete), or with `permanent` remove them outright.  Either way the
 * valid ids go through ONE bulk call/transaction.
 * ------------------------------------------------------------------------- */
static int
cmd_delete_notes(OnDatabase *db, char **ids, int n, gboolean permanent)
{
    int rc = 0;                      /* worst exit code seen                */
    GArray *valid = g_array_new(FALSE, FALSE, sizeof(gint64));
    GList  *metas = NULL;            /* matched notes, for the messages     */
    for (int i = 0; i < n; i++) {
        OnNoteMeta *meta = note_from_arg(db, ids[i]);
        if (meta == NULL) {
            rc = 2;
            continue;
        }
        g_array_append_val(valid, meta->id);
        metas = g_list_prepend(metas, meta);
    }
    metas = g_list_reverse(metas);

    gboolean ok = TRUE;              /* did the bulk operation succeed?     */
    if (valid->len > 0)
        ok = permanent
             ? on_db_notes_delete(db, (const gint64 *)valid->data, valid->len)
             : on_db_notes_trash(db, (const gint64 *)valid->data, valid->len);
    if (!ok) {
        fprintf(stderr, "error: could not %s notes\n",
                permanent ? "delete" : "trash");
        rc = 2;
    } else {
        for (GList *l = metas; l != NULL; l = l->next) {
            OnNoteMeta *m = l->data;
            printf("%s note %" G_GINT64_FORMAT "\t%s\n",
                   permanent ? "deleted" : "trashed", m->id, m->title);
        }
    }

    g_list_free_full(metas, (GDestroyNotify)on_db_note_meta_free);
    g_array_free(valid, TRUE);
    return rc;
}

/* cmd_restore_notes() — restore each note id from the Trash.                */
static int
cmd_restore_notes(OnDatabase *db, char **ids, int n)
{
    int rc = 0;                      /* worst exit code seen                */
    for (int i = 0; i < n; i++) {
        OnNoteMeta *meta = note_from_arg(db, ids[i]);
        if (meta == NULL) {
            rc = 2;
            continue;
        }
        if (on_db_note_restore(db, meta->id)) {
            printf("restored note %" G_GINT64_FORMAT "\t%s\n",
                   meta->id, meta->title);
        } else {
            fprintf(stderr, "error: could not restore note %"
                    G_GINT64_FORMAT "\n", meta->id);
            rc = 2;
        }
        on_db_note_meta_free(meta);
    }
    return rc;
}

/* cmd_move_notes() — move note ids into a destination folder path.          */
static int
cmd_move_notes(OnDatabase *db, char **ids, int n, const gchar *dest)
{
    gint64 folder;                   /* destination folder id               */
    if (!on_cli_resolve_folder_path(db, dest, FALSE, &folder)) {
        fprintf(stderr, "error: no such folder: %s\n", dest);
        return 2;
    }

    int rc = 0;                      /* worst exit code seen                */
    for (int i = 0; i < n; i++) {
        OnNoteMeta *meta = note_from_arg(db, ids[i]);
        if (meta == NULL) {
            rc = 2;
            continue;
        }
        on_db_notes_move(db, &meta->id, 1, folder);
        printf("moved note %" G_GINT64_FORMAT "\t%s -> %s\n",
               meta->id, meta->title, *dest ? dest : "/");
        on_db_note_meta_free(meta);
    }
    return rc;
}

/* cmd_set_modified() — overwrite a note's modification date with a UNIX
 * timestamp.  Lets importers preserve the original edit date (a normal
 * save always stamps the current time).                                     */
static int
cmd_set_modified(OnDatabase *db, const gchar *id_str, const gchar *ts_str)
{
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;

    gchar *endp = NULL;              /* end of the parsed number            */
    gint64 ts = g_ascii_strtoll(ts_str, &endp, 10);
    if (ts <= 0 || endp == NULL || *endp != '\0') {
        fprintf(stderr, "error: bad UNIX timestamp: %s\n", ts_str);
        on_db_note_meta_free(meta);
        return 2;
    }

    on_db_note_set_updated_at(db, meta->id, ts);
    printf("set modified of note %" G_GINT64_FORMAT "\t%s\n",
           meta->id, meta->title);
    on_db_note_meta_free(meta);
    return 0;
}

/* cmd_backup() — snapshot the database to a file.                           */
static int
cmd_backup(OnDatabase *db, const gchar *dest)
{
    if (!on_db_backup_to(db, dest)) {
        fprintf(stderr, "error: backup to %s failed\n", dest);
        return 2;
    }
    printf("backed up to %s\n", dest);
    return 0;
}

/* cmd_export() — export every note as HTML or Markdown into a directory.    */
static int
cmd_export(OnDatabase *db, const gchar *dir, OnExportFormat format)
{
    if (!gtk_init_check(NULL, NULL)) {
        fprintf(stderr, "error: GTK could not initialize (needed to "
                        "render notes)\n");
        return 2;
    }
    /* The exporter only touches app->db.                                   */
    OnApp app = { 0 };
    app.db = db;

    gchar *err = NULL;               /* exporter error message              */
    gint n = on_export_all(&app, dir, format, &err);
    if (n < 0) {
        fprintf(stderr, "error: %s\n", err != NULL ? err : "export failed");
        g_free(err);
        return 2;
    }
    printf("exported %d note%s to %s\n", n, n == 1 ? "" : "s", dir);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_search() — case-insensitive search of every visible note's title +
 * plain text (the search window's strategy: the body_text cache fetched
 * as ONE map query, extraction fallback per unfilled row).  Prints one
 * "ID<TAB>MODIFIED<TAB>/Folder/Sub/Title" line per hit; no GTK needed.
 *   use_regex — TRUE to treat `query` as a GRegex pattern (still
 *               case-insensitive) instead of a literal substring.
 * ------------------------------------------------------------------------- */
static int
cmd_search(OnDatabase *db, const gchar *query, gboolean use_regex)
{
    GRegex *regex = NULL;            /* compiled pattern (regex mode)       */
    gchar  *query_ci = NULL;         /* casefolded needle (plain mode)      */
    if (use_regex) {
        GError *err = NULL;
        regex = g_regex_new(query, G_REGEX_CASELESS, 0, &err);
        if (regex == NULL) {
            fprintf(stderr, "error: bad pattern: %s\n", err->message);
            g_clear_error(&err);
            return 2;
        }
    } else {
        query_ci = g_utf8_casefold(query, -1);
    }

    GList      *notes  = on_db_note_list_all(db, FALSE);
    GHashTable *bodies = on_db_note_body_map(db);    /* id → body text      */
    GHashTable *paths  = on_db_folder_path_map(db);  /* folder id → path    */

    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;     /* candidate note                      */
        const gchar *body = g_hash_table_lookup(bodies, &m->id);
        gchar *fallback = NULL;      /* extracted text for unfilled rows    */
        if (body == NULL) {
            fallback = note_body_text_cached(db, m->id);
            body = fallback;
        }

        gboolean match;              /* does this note match the query?     */
        if (regex != NULL) {
            match = g_regex_match(regex, m->title, 0, NULL) ||
                    g_regex_match(regex, body, 0, NULL);
        } else {
            gchar *t = g_utf8_casefold(m->title, -1);
            gchar *b = g_utf8_casefold(body, -1);
            match = strstr(t, query_ci) != NULL ||
                    strstr(b, query_ci) != NULL;
            g_free(t);
            g_free(b);
        }
        if (match) {
            const gchar *fpath = g_hash_table_lookup(paths, &m->folder_id);
            print_note_line(m, fpath != NULL ? fpath : "");
        }
        g_free(fallback);
    }

    g_hash_table_destroy(paths);
    g_hash_table_destroy(bodies);
    on_db_note_list_free(notes);
    if (regex != NULL)
        g_regex_unref(regex);
    g_free(query_ci);
    return 0;
}

/* usage() — the help text.                                                  */
static int
usage(FILE *out)
{
    fputs(
"Usage: blue_notes [COMMAND ...]   (no command starts the GUI)\n"
"\n"
"  tag list                          print every tag with its note count\n"
"  tag notes NAME                    print the notes labeled with a tag\n"
"  tag delete NAME                   remove a tag from the database\n"
"\n"
"  folder list                       print the folder tree with note counts\n"
"  folder add PATH                   create a folder path (like mkdir -p)\n"
"  folder delete [--permanent] PATH  move a folder AND everything inside to\n"
"                                    the Trash (--permanent: delete outright)\n"
"\n"
"  note list [PATH|--all]            print ID/modified/title per note\n"
"  note cat ID [--md]                print a note's plain text (--md:\n"
"                                    Markdown keeping the formatting;\n"
"                                    images become placeholders)\n"
"  note new [--folder PATH] TEXT     create a note (TEXT '-' reads stdin;\n"
"                                    the first line becomes the title)\n"
"  note append ID TEXT|-             append plain text to a note, on a\n"
"                                    fresh line (existing content is kept)\n"
"  note set ID TEXT|-                REPLACE a note's content with plain\n"
"                                    text (formatting, images, tables and\n"
"                                    tags of the old content are LOST)\n"
"  note delete [--permanent] ID...   move notes to the Trash\n"
"                                    (--permanent: delete outright)\n"
"  note restore ID [ID...]           restore notes from the Trash\n"
"  note move ID [ID...] PATH         move notes into a folder ('/' = top)\n"
"  note tags ID                      print a note's tags, one per line\n"
"  note tag ID NAME                  add a #tag to a note (the literal\n"
"                                    '#NAME' token is appended to the text)\n"
"  note untag ID NAME                remove a #tag from a note's text\n"
"  note add-image ID FILE            append an image file to a note\n"
"  note set-modified ID TIMESTAMP    set a note's modified date (UNIX\n"
"                                    seconds; for importers)\n"
"  note open PATH                    open a note's editor in the running\n"
"                                    instance (PATH = id or Folder/Title);\n"
"                                    starts Blue Notes if it is not running\n"
"\n"
"  search TEXT [--regex]             case-insensitive search of all note\n"
"                                    titles + text; prints one\n"
"                                    ID/modified/path line per hit\n"
"\n"
"  quicknote                         create a note in the root folder and\n"
"                                    open its editor in the running instance\n"
"                                    (starts Blue Notes if not running)\n"
"\n"
"  backup FILE.db                    snapshot the database to FILE.db\n"
"  export-md DIR                     export all notes as Markdown into DIR\n"
"  export-html DIR                   export all notes as HTML into DIR\n"
"  help                              show this text\n",
        out);
    return out == stderr ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * dispatch_tag()/dispatch_folder()/dispatch_note() — verb handling for
 * each noun group.  argv/argc are the arguments AFTER the verb.
 * ------------------------------------------------------------------------- */
static int
dispatch_tag(OnDatabase *db, const char *verb, char **argv, int argc)
{
    if (g_strcmp0(verb, "list") == 0 && argc == 0)
        return cmd_list_tags(db);
    if (g_strcmp0(verb, "notes") == 0 && argc == 1)
        return cmd_tag_notes(db, argv[0]);
    if (g_strcmp0(verb, "delete") == 0 && argc == 1)
        return cmd_delete_tag(db, argv[0]);
    return usage(stderr);
}

static int
dispatch_folder(OnDatabase *db, const char *verb, char **argv, int argc)
{
    if (g_strcmp0(verb, "list") == 0 && argc == 0)
        return cmd_list_folders(db);
    if (g_strcmp0(verb, "add") == 0 && argc == 1)
        return cmd_add_folder(db, argv[0]);
    if (g_strcmp0(verb, "delete") == 0) {
        /* folder delete [--permanent] PATH  (flag accepted either side)    */
        if (argc == 1 && g_strcmp0(argv[0], "--permanent") != 0)
            return cmd_delete_folder(db, argv[0], FALSE);
        if (argc == 2 && g_strcmp0(argv[0], "--permanent") == 0)
            return cmd_delete_folder(db, argv[1], TRUE);
        if (argc == 2 && g_strcmp0(argv[1], "--permanent") == 0)
            return cmd_delete_folder(db, argv[0], TRUE);
        return usage(stderr);
    }
    return usage(stderr);
}

static int
dispatch_note(OnDatabase *db, const char *verb, char **argv, int argc)
{
    if (g_strcmp0(verb, "list") == 0 && argc <= 1)
        return cmd_list_notes(db, argc == 1 ? argv[0] : "--all");
    if (g_strcmp0(verb, "cat") == 0) {
        if (argc == 1)
            return cmd_cat_note(db, argv[0], FALSE);
        if (argc == 2 && g_strcmp0(argv[1], "--md") == 0)
            return cmd_cat_note(db, argv[0], TRUE);
        return usage(stderr);
    }
    if (g_strcmp0(verb, "new") == 0) {
        /* note new [--folder PATH] CONTENT                                 */
        if (argc == 1)
            return cmd_new_note(db, "", argv[0]);
        if (argc == 3 && g_strcmp0(argv[0], "--folder") == 0)
            return cmd_new_note(db, argv[1], argv[2]);
        return usage(stderr);
    }
    if (g_strcmp0(verb, "append") == 0 && argc == 2)
        return cmd_append_note(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "set") == 0 && argc == 2)
        return cmd_set_note(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "delete") == 0 && argc >= 1) {
        /* note delete [--permanent] ID...  (flag accepted anywhere)        */
        gboolean permanent = FALSE;
        char **ids = g_new(char *, (gsize)argc);
        int n = 0;                   /* ids kept after flag filtering       */
        for (int i = 0; i < argc; i++) {
            if (g_strcmp0(argv[i], "--permanent") == 0)
                permanent = TRUE;
            else
                ids[n++] = argv[i];
        }
        int rc = (n > 0) ? cmd_delete_notes(db, ids, n, permanent)
                         : usage(stderr);
        g_free(ids);
        return rc;
    }
    if (g_strcmp0(verb, "restore") == 0 && argc >= 1)
        return cmd_restore_notes(db, argv, argc);
    if (g_strcmp0(verb, "move") == 0 && argc >= 2)
        return cmd_move_notes(db, argv, argc - 1, argv[argc - 1]);
    if (g_strcmp0(verb, "tags") == 0 && argc == 1)
        return cmd_note_tags(db, argv[0]);
    if (g_strcmp0(verb, "tag") == 0 && argc == 2)
        return cmd_tag_note(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "untag") == 0 && argc == 2)
        return cmd_untag_note(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "add-image") == 0 && argc == 2)
        return cmd_add_image(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "set-modified") == 0 && argc == 2)
        return cmd_set_modified(db, argv[0], argv[1]);
    return usage(stderr);
}

/* ---------------------------------------------------------------------------
 * cli_gui_command() — handle the two commands that act on a GUI editor:
 * "quicknote" and "note open PATH".  Each first tries to reach a running
 * instance over the IPC socket; if one answers it did the work and we exit.
 * Otherwise the request is recorded as a pending action and -1 is returned so
 * main() starts the GUI, which performs it at activate.
 *   remote_cmd — the line to send a running instance ("quicknote"/"open ...").
 *   set_pending — records the same action for the not-running case.
 * Returns a process exit code, or -1 to start the GUI.
 * ------------------------------------------------------------------------- */
static int
cli_gui_command(const gchar *remote_cmd, void (*set_pending)(void),
                const gchar *pending_path)
{
    gchar *reply = NULL;             /* running instance's message           */
    OnIpcResult r = on_ipc_try_remote(remote_cmd, &reply);
    if (r == ON_IPC_NO_SERVER) {
        /* No instance: arrange for the GUI we are about to start to do it.  */
        if (set_pending != NULL)
            set_pending();
        else
            on_ipc_set_pending_open(pending_path);
        return -1;
    }
    if (reply != NULL && *reply != '\0')
        fprintf(r == ON_IPC_OK ? stdout : stderr, "%s\n", reply);
    g_free(reply);
    return (r == ON_IPC_OK) ? 0 : 2;
}

gboolean
on_cli_command_reads_stdin(int argc, char **argv)
{
    /* "note new … -", "note append ID -", "note set ID -": a content
     * argument of "-" consumes stdin.                                       */
    if (argc < 4 || g_strcmp0(argv[1], "note") != 0 ||
        g_strcmp0(argv[argc - 1], "-") != 0)
        return FALSE;
    const char *verb = argv[2];
    return g_strcmp0(verb, "new") == 0 ||
           g_strcmp0(verb, "append") == 0 ||
           g_strcmp0(verb, "set") == 0;
}

gboolean
on_cli_command_mutates(int argc, char **argv)
{
    if (argc < 2)
        return FALSE;
    const char *cmd  = argv[1];
    const char *verb = (argc >= 3) ? argv[2] : "";
    if (g_strcmp0(cmd, "tag") == 0)
        return g_strcmp0(verb, "delete") == 0;
    if (g_strcmp0(cmd, "folder") == 0)
        return g_strcmp0(verb, "add") == 0 || g_strcmp0(verb, "delete") == 0;
    if (g_strcmp0(cmd, "note") == 0)
        return g_strcmp0(verb, "new") == 0 ||
               g_strcmp0(verb, "append") == 0 ||
               g_strcmp0(verb, "set") == 0 ||
               g_strcmp0(verb, "delete") == 0 ||
               g_strcmp0(verb, "restore") == 0 ||
               g_strcmp0(verb, "move") == 0 ||
               g_strcmp0(verb, "tag") == 0 ||
               g_strcmp0(verb, "untag") == 0 ||
               g_strcmp0(verb, "add-image") == 0 ||
               g_strcmp0(verb, "set-modified") == 0;
    return FALSE;         /* search / backup / export-* are read-only       */
}

int
on_cli_dispatch_db(OnDatabase *db, int argc, char **argv)
{
    const char *cmd = argv[1];       /* the noun/flat command               */

    /* Noun groups need a verb.                                             */
    gboolean is_noun = g_strcmp0(cmd, "tag") == 0 ||
                       g_strcmp0(cmd, "folder") == 0 ||
                       g_strcmp0(cmd, "note") == 0;
    if (is_noun && argc < 3)
        return usage(stderr);

    if (g_strcmp0(cmd, "tag") == 0)
        return dispatch_tag(db, argv[2], argv + 3, argc - 3);
    if (g_strcmp0(cmd, "folder") == 0)
        return dispatch_folder(db, argv[2], argv + 3, argc - 3);
    if (g_strcmp0(cmd, "note") == 0)
        return dispatch_note(db, argv[2], argv + 3, argc - 3);
    if (g_strcmp0(cmd, "search") == 0) {
        if (argc == 3)
            return cmd_search(db, argv[2], FALSE);
        if (argc == 4 && g_strcmp0(argv[3], "--regex") == 0)
            return cmd_search(db, argv[2], TRUE);
        return usage(stderr);
    }
    if (g_strcmp0(cmd, "backup") == 0)
        return (argc == 3) ? cmd_backup(db, argv[2]) : usage(stderr);
    if (g_strcmp0(cmd, "export-md") == 0)
        return (argc == 3) ? cmd_export(db, argv[2], ON_EXPORT_MARKDOWN)
                           : usage(stderr);
    if (g_strcmp0(cmd, "export-html") == 0)
        return (argc == 3) ? cmd_export(db, argv[2], ON_EXPORT_HTML)
                           : usage(stderr);
    return usage(stderr);            /* unreachable for validated commands   */
}

int
on_cli_run(int argc, char **argv)
{
    if (argc < 2)
        return -1;                   /* no subcommand: start the GUI        */
    const char *cmd = argv[1];

    if (g_strcmp0(cmd, "help") == 0 || g_strcmp0(cmd, "--help") == 0 ||
        g_strcmp0(cmd, "-h") == 0)
        return usage(stdout);

    /* GUI-interacting commands: open an editor in the running instance, or
     * start the GUI and do it there.  Handled before the headless db path.  */
    if (g_strcmp0(cmd, "quicknote") == 0) {
        if (argc != 2)
            return usage(stderr);
        return cli_gui_command("quicknote", on_ipc_set_pending_quicknote,
                               NULL);
    }
    if (g_strcmp0(cmd, "note") == 0 && argc >= 3 &&
        g_strcmp0(argv[2], "open") == 0) {
        if (argc != 4)
            return usage(stderr);
        gchar *remote = g_strdup_printf("open %s", argv[3]);
        int rc = cli_gui_command(remote, NULL, argv[3]);
        g_free(remote);
        return rc;
    }

    /* Anything that is not a known noun/command falls through to GTK
     * (which has its own option handling for things like --display).       */
    gboolean is_noun = g_strcmp0(cmd, "tag") == 0 ||
                       g_strcmp0(cmd, "folder") == 0 ||
                       g_strcmp0(cmd, "note") == 0;
    gboolean is_flat = g_strcmp0(cmd, "search") == 0 ||
                       g_strcmp0(cmd, "backup") == 0 ||
                       g_strcmp0(cmd, "export-md") == 0 ||
                       g_strcmp0(cmd, "export-html") == 0;
    if (!is_noun && !is_flat)
        return -1;

    /* Route every data command through a running instance when one exists,
     * so a single process owns the database connection and the GUI refreshes
     * live.  With no instance, run headless against our own connection.      */
    gboolean ran = FALSE;            /* did a running instance handle it?    */
    int rc = on_ipc_try_remote_run(argc, argv, &ran);
    if (ran)
        return rc;

    OnDatabase *db = cli_open_db();
    if (db == NULL)
        return 2;
    rc = on_cli_dispatch_db(db, argc, argv);
    on_db_close(db);
    return rc;
}
