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
#include "serialize.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * cli_open_db() — open the same database the GUI would use.
 * Returns the handle, or NULL after printing an error.
 * ------------------------------------------------------------------------- */
static OnDatabase *
cli_open_db(void)
{
    gchar *db_dir = on_app_config_load_db_dir();
    gchar *path = (db_dir != NULL)
                  ? g_build_filename(db_dir, "notes.db", NULL)
                  : NULL;
    OnDatabase *db = on_db_open(path);
    if (db == NULL) {
        fprintf(stderr, "error: cannot open database%s%s\n",
                path != NULL ? " at " : "", path != NULL ? path : "");
    } else {
        /* The GUI marks a shared database while it has it open; CLI
         * commands still run, but the user should know about the risk.     */
        gchar *holder = on_db_setting_get(db, "in_use");
        if (holder != NULL && *holder != '\0')
            fprintf(stderr, "warning: database is in use by %s — "
                            "concurrent changes may conflict\n", holder);
        g_free(holder);
    }
    g_free(path);
    g_free(db_dir);
    return db;
}

/* ---------------------------------------------------------------------------
 * resolve_folder_path() — walk "A/B/C" through the folder tree.
 *   db     — open database.
 *   path   — folder path; "" or "/" mean the top level.
 *   create — TRUE to create missing components (mkdir -p style).
 *   out_id — receives the folder id (0 = top level).
 * Returns TRUE if the full path resolved (or was created).
 * ------------------------------------------------------------------------- */
static gboolean
resolve_folder_path(OnDatabase *db, const gchar *path, gboolean create,
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
 * cmd_list_tags() — one tag per line: "name<TAB>note-count".
 * ------------------------------------------------------------------------- */
static int
cmd_list_tags(OnDatabase *db)
{
    GList *tags = on_db_tag_list(db);
    for (GList *l = tags; l != NULL; l = l->next) {
        OnTag *t = l->data;
        printf("%s\t%d\n", t->name,
               on_db_note_count_for_tag(db, t->id));
    }
    on_db_tag_list_free(tags);
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

/* ---------------------------------------------------------------------------
 * print_folder_tree() — recursive indented listing with note counts.
 * ------------------------------------------------------------------------- */
static void
print_folder_tree(OnDatabase *db, gint64 parent, gint depth)
{
    GList *folders = on_db_folder_list(db, parent);
    for (GList *l = folders; l != NULL; l = l->next) {
        OnFolder *f = l->data;
        printf("%*s%s\t%d\n", depth * 2, "", f->name,
               on_db_note_count_for_folder(db, f->id));
        print_folder_tree(db, f->id, depth + 1);
    }
    on_db_folder_list_free(folders);
}

static int
cmd_list_folders(OnDatabase *db)
{
    print_folder_tree(db, 0, 0);
    return 0;
}

/* cmd_add_folder() — create a (possibly nested) folder path.                */
static int
cmd_add_folder(OnDatabase *db, const gchar *path)
{
    gint64 id;                       /* the created/found folder            */
    if (!resolve_folder_path(db, path, TRUE, &id) || id == 0) {
        fprintf(stderr, "error: could not create folder %s\n", path);
        return 2;
    }
    printf("folder %s (id %" G_GINT64_FORMAT ")\n", path, id);
    return 0;
}

/* cmd_delete_folder() — delete a folder path (cascades to its contents).    */
static int
cmd_delete_folder(OnDatabase *db, const gchar *path)
{
    gint64 id;                       /* the folder to delete                */
    if (!resolve_folder_path(db, path, FALSE, &id) || id == 0) {
        fprintf(stderr, "error: no such folder: %s\n", path);
        return 2;
    }
    if (!on_db_folder_delete(db, id)) {
        fprintf(stderr, "error: could not delete folder %s\n", path);
        return 2;
    }
    printf("deleted folder %s (and its contents)\n", path);
    return 0;
}

/* ---------------------------------------------------------------------------
 * print_note_line() — "ID<TAB>MODIFIED<TAB>TITLE".
 * ------------------------------------------------------------------------- */
static void
print_note_line(OnNoteMeta *m)
{
    GDateTime *dt = g_date_time_new_from_unix_local(m->updated_at);
    gchar *when = g_date_time_format(dt, "%Y-%m-%d %H:%M");
    g_date_time_unref(dt);
    printf("%" G_GINT64_FORMAT "\t%s\t%s\n", m->id, when, m->title);
    g_free(when);
}

/* cmd_list_notes() — notes in one folder, or every note with --all.         */
static int
cmd_list_notes(OnDatabase *db, const gchar *path)
{
    GList *notes;                    /* the OnNoteMeta* result set          */
    if (g_strcmp0(path, "--all") == 0) {
        notes = on_db_note_list_all(db);
    } else {
        gint64 folder;               /* resolved folder id                  */
        if (!resolve_folder_path(db, path, FALSE, &folder)) {
            fprintf(stderr, "error: no such folder: %s\n", path);
            return 2;
        }
        notes = on_db_note_list(db, folder);
    }
    for (GList *l = notes; l != NULL; l = l->next)
        print_note_line(l->data);
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
    if (!resolve_folder_path(db, folder_path, FALSE, &folder)) {
        fprintf(stderr, "error: no such folder: %s "
                        "(create it with 'folder add')\n", folder_path);
        return 2;
    }

    /* Pull the content from stdin when asked.                              */
    gchar *text;                     /* the note body (owned)               */
    if (g_strcmp0(content, "-") == 0) {
        GString *in = g_string_new(NULL);
        gchar buf[4096];
        gsize n;
        while ((n = fread(buf, 1, sizeof buf, stdin)) > 0)
            g_string_append_len(in, buf, (gssize)n);
        text = g_string_free(in, FALSE);
    } else {
        text = g_strdup(content);
    }
    if (!g_utf8_validate(text, -1, NULL)) {
        fprintf(stderr, "error: content is not valid UTF-8\n");
        g_free(text);
        return 2;
    }

    if (!gtk_init_check(NULL, NULL)) {
        fprintf(stderr, "error: GTK could not initialize (needed to "
                        "serialize note content)\n");
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

    gsize blob_len;                  /* serialized content size             */
    guint8 *blob = on_note_serialize(buffer, &blob_len);
    gchar *title = on_buffer_first_line(buffer);
    gchar *body = on_note_extract_text(blob, blob_len);
    on_db_note_save(db, id, title, blob, blob_len, body);

    printf("note %" G_GINT64_FORMAT "\t%s\n", id, title);

    g_free(body);
    g_free(title);
    g_free(blob);
    g_object_unref(buffer);
    g_free(text);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_add_image() — append an image file to an existing note.  The image
 * is stored at full resolution (same as pasting it in the editor) and
 * displayed at the default thumbnail width.
 * ------------------------------------------------------------------------- */
static int
cmd_add_image(OnDatabase *db, const gchar *id_str, const gchar *file)
{
    gint64 id = g_ascii_strtoll(id_str, NULL, 10);
    OnNoteMeta *meta = (id > 0) ? on_db_note_get(db, id) : NULL;
    if (meta == NULL) {
        fprintf(stderr, "error: no such note: %s\n", id_str);
        return 2;
    }

    if (!gtk_init_check(NULL, NULL)) {
        fprintf(stderr, "error: GTK could not initialize (needed to "
                        "process note content)\n");
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
    GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
    on_buffer_ensure_tags(buffer);
    gsize   blob_len = 0;            /* stored blob size                    */
    guint8 *blob = on_db_note_load(db, id, &blob_len);
    if (blob != NULL) {
        on_note_deserialize(buffer, blob, blob_len);
        g_free(blob);
    }

    GtkTextIter end;                 /* append position                     */
    gtk_text_buffer_get_end_iter(buffer, &end);
    if (gtk_text_buffer_get_char_count(buffer) > 0 &&
        !gtk_text_iter_starts_line(&end))
        gtk_text_buffer_insert(buffer, &end, "\n", -1);
    GtkTextChildAnchor *anchor =
        gtk_text_buffer_create_child_anchor(buffer, &end);
    on_anchor_set_image(anchor, pixbuf, 0);

    gchar *title = on_buffer_first_line(buffer);
    guint8 *out = on_note_serialize(buffer, &blob_len);
    gchar *body = on_note_extract_text(out, blob_len);
    on_db_note_save(db, id, title, out, blob_len, body);

    printf("added image to note %" G_GINT64_FORMAT "\t%s\n", id, title);

    g_free(body);
    g_free(out);
    g_free(title);
    g_object_unref(buffer);
    g_object_unref(pixbuf);
    on_db_note_meta_free(meta);
    return 0;
}

/* cmd_delete_notes() — delete each note id given.                           */
static int
cmd_delete_notes(OnDatabase *db, char **ids, int n)
{
    int rc = 0;                      /* worst exit code seen                */
    for (int i = 0; i < n; i++) {
        gint64 id = g_ascii_strtoll(ids[i], NULL, 10);
        OnNoteMeta *meta = (id > 0) ? on_db_note_get(db, id) : NULL;
        if (meta == NULL) {
            fprintf(stderr, "error: no such note: %s\n", ids[i]);
            rc = 2;
            continue;
        }
        on_db_note_delete(db, id);
        printf("deleted note %" G_GINT64_FORMAT "\t%s\n", id, meta->title);
        on_db_note_meta_free(meta);
    }
    return rc;
}

/* cmd_move_notes() — move note ids into a destination folder path.          */
static int
cmd_move_notes(OnDatabase *db, char **ids, int n, const gchar *dest)
{
    gint64 folder;                   /* destination folder id               */
    if (!resolve_folder_path(db, dest, FALSE, &folder)) {
        fprintf(stderr, "error: no such folder: %s\n", dest);
        return 2;
    }

    int rc = 0;                      /* worst exit code seen                */
    for (int i = 0; i < n; i++) {
        gint64 id = g_ascii_strtoll(ids[i], NULL, 10);
        OnNoteMeta *meta = (id > 0) ? on_db_note_get(db, id) : NULL;
        if (meta == NULL) {
            fprintf(stderr, "error: no such note: %s\n", ids[i]);
            rc = 2;
            continue;
        }
        on_db_note_move(db, id, folder);
        printf("moved note %" G_GINT64_FORMAT "\t%s -> %s\n",
               id, meta->title, *dest ? dest : "/");
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
    gint64 id = g_ascii_strtoll(id_str, NULL, 10);
    OnNoteMeta *meta = (id > 0) ? on_db_note_get(db, id) : NULL;
    if (meta == NULL) {
        fprintf(stderr, "error: no such note: %s\n", id_str);
        return 2;
    }

    gchar *endp = NULL;              /* end of the parsed number            */
    gint64 ts = g_ascii_strtoll(ts_str, &endp, 10);
    if (ts <= 0 || endp == NULL || *endp != '\0') {
        fprintf(stderr, "error: bad UNIX timestamp: %s\n", ts_str);
        on_db_note_meta_free(meta);
        return 2;
    }

    on_db_note_set_updated_at(db, id, ts);
    printf("set modified of note %" G_GINT64_FORMAT "\t%s\n",
           id, meta->title);
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

/* usage() — the help text.                                                  */
static int
usage(FILE *out)
{
    fputs(
"Usage: blue_notes [COMMAND ...]   (no command starts the GUI)\n"
"\n"
"  tag list                          print every tag with its note count\n"
"  tag delete NAME                   remove a tag from the database\n"
"\n"
"  folder list                       print the folder tree with note counts\n"
"  folder add PATH                   create a folder path (like mkdir -p)\n"
"  folder delete PATH                delete a folder AND everything inside\n"
"\n"
"  note list [PATH|--all]            print ID/modified/title per note\n"
"  note new [--folder PATH] TEXT     create a note (TEXT '-' reads stdin;\n"
"                                    the first line becomes the title)\n"
"  note delete ID [ID...]            delete notes by id (see note list)\n"
"  note move ID [ID...] PATH         move notes into a folder ('/' = top)\n"
"  note add-image ID FILE            append an image file to a note\n"
"  note set-modified ID TIMESTAMP    set a note's modified date (UNIX\n"
"                                    seconds; for importers)\n"
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
    if (g_strcmp0(verb, "delete") == 0 && argc == 1)
        return cmd_delete_folder(db, argv[0]);
    return usage(stderr);
}

static int
dispatch_note(OnDatabase *db, const char *verb, char **argv, int argc)
{
    if (g_strcmp0(verb, "list") == 0 && argc <= 1)
        return cmd_list_notes(db, argc == 1 ? argv[0] : "--all");
    if (g_strcmp0(verb, "new") == 0) {
        /* note new [--folder PATH] CONTENT                                 */
        if (argc == 1)
            return cmd_new_note(db, "", argv[0]);
        if (argc == 3 && g_strcmp0(argv[0], "--folder") == 0)
            return cmd_new_note(db, argv[1], argv[2]);
        return usage(stderr);
    }
    if (g_strcmp0(verb, "delete") == 0 && argc >= 1)
        return cmd_delete_notes(db, argv, argc);
    if (g_strcmp0(verb, "move") == 0 && argc >= 2)
        return cmd_move_notes(db, argv, argc - 1, argv[argc - 1]);
    if (g_strcmp0(verb, "add-image") == 0 && argc == 2)
        return cmd_add_image(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "set-modified") == 0 && argc == 2)
        return cmd_set_modified(db, argv[0], argv[1]);
    return usage(stderr);
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

    /* Anything that is not a known noun/command falls through to GTK
     * (which has its own option handling for things like --display).       */
    gboolean is_noun = g_strcmp0(cmd, "tag") == 0 ||
                       g_strcmp0(cmd, "folder") == 0 ||
                       g_strcmp0(cmd, "note") == 0;
    gboolean is_flat = g_strcmp0(cmd, "backup") == 0 ||
                       g_strcmp0(cmd, "export-md") == 0 ||
                       g_strcmp0(cmd, "export-html") == 0;
    if (!is_noun && !is_flat)
        return -1;

    /* Noun groups need a verb.                                             */
    if (is_noun && argc < 3)
        return usage(stderr);

    OnDatabase *db = cli_open_db();
    if (db == NULL)
        return 2;

    int rc;                          /* the command's exit code             */
    if (g_strcmp0(cmd, "tag") == 0)
        rc = dispatch_tag(db, argv[2], argv + 3, argc - 3);
    else if (g_strcmp0(cmd, "folder") == 0)
        rc = dispatch_folder(db, argv[2], argv + 3, argc - 3);
    else if (g_strcmp0(cmd, "note") == 0)
        rc = dispatch_note(db, argv[2], argv + 3, argc - 3);
    else if (g_strcmp0(cmd, "backup") == 0)
        rc = (argc == 3) ? cmd_backup(db, argv[2]) : usage(stderr);
    else if (g_strcmp0(cmd, "export-md") == 0)
        rc = (argc == 3) ? cmd_export(db, argv[2], ON_EXPORT_MARKDOWN)
                         : usage(stderr);
    else  /* export-html */
        rc = (argc == 3) ? cmd_export(db, argv[2], ON_EXPORT_HTML)
                         : usage(stderr);

    on_db_close(db);
    return rc;
}
