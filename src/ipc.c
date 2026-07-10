/* ===========================================================================
 * ipc.c — talk to an already-running Blue Notes instance (implementation)
 *
 * See ipc.h.  The wire protocol is line based:
 *
 *   request   quicknote                 create a note in the root folder
 *             open <path>               open the editor for a note path
 *   response  OK <message>\n            success (message printed to stdout)
 *             ERR <message>\n           failure (message printed to stderr)
 *
 * A "note path" is either a numeric note id or "Folder/Sub/Title" where the
 * final component is matched against note titles in the resolved folder.
 * =========================================================================== */

#include "ipc.h"
#include "cli.h"
#include "db.h"
#include "editor_window.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * ipc_socket_path() — the per-user socket both sides agree on.  One instance
 * per user is the unit "a running Blue Notes"; the path lives in the user's
 * runtime dir (short, private) so the Unix sun_path limit is never a concern.
 * Returns a new string (g_free it).
 * ------------------------------------------------------------------------- */
static gchar *
ipc_socket_path(void)
{
    return g_build_filename(g_get_user_runtime_dir(),
                            "blue_notes.sock", NULL);
}

/* ===========================================================================
 * framed stream I/O
 *
 * The RUN protocol (a delegated data command and its captured output) mixes
 * short header lines with arbitrary-length byte blobs, so it needs exact
 * reads.  These helpers read/write over a connection's GInputStream /
 * GOutputStream: a line is bytes up to '\n'; a blob is a decimal length line
 * followed by exactly that many bytes.
 * =========================================================================== */

/* stream_write_all() — write every byte of `buf`; TRUE on success.          */
static gboolean
stream_write_all(GOutputStream *o, const void *buf, gsize n)
{
    return g_output_stream_write_all(o, buf, n, NULL, NULL, NULL);
}

/* Wire-format sanity bounds.  No legitimate frame comes close to these;
 * they keep a stray or hostile client from wedging the GUI (which reads
 * the socket on the main loop) or forcing an absurd allocation from a
 * bogus length header.                                                      */
#define IPC_MAX_LINE  (64 * 1024)            /* command/header line bytes  */
#define IPC_MAX_BLOB  (64 * 1024 * 1024)     /* argv/stdin/output blob     */
#define IPC_TIMEOUT_S 10                     /* per-socket-op limit (s)    */

/* stream_read_line() — read up to '\n'; return the line without it (owned),
 * or NULL at EOF before any byte or past IPC_MAX_LINE.  Byte-at-a-time:
 * header lines are short.                                                    */
static gchar *
stream_read_line(GInputStream *in)
{
    GString *s = g_string_new(NULL);
    for (;;) {
        char c;                      /* one byte                            */
        gssize got = g_input_stream_read(in, &c, 1, NULL, NULL);
        if (got <= 0) {
            if (s->len == 0) {
                g_string_free(s, TRUE);
                return NULL;
            }
            break;
        }
        if (c == '\n')
            break;
        if (s->len >= IPC_MAX_LINE) {    /* runaway line: give up          */
            g_string_free(s, TRUE);
            return NULL;
        }
        g_string_append_c(s, c);
    }
    return g_string_free(s, FALSE);
}

/* stream_read_uint_line() — read a decimal length line; -1 on EOF, on a
 * non-numeric line (garbage must fail, not parse as 0 and desync the
 * protocol), or on a negative value.                                         */
static gssize
stream_read_uint_line(GInputStream *in)
{
    gchar *line = stream_read_line(in);
    if (line == NULL)
        return -1;
    gchar *end = NULL;               /* first unparsed char                 */
    gint64 v = g_ascii_strtoll(line, &end, 10);
    gboolean ok = end != line && *end == '\0' && v >= 0;
    g_free(line);
    return ok ? (gssize)v : -1;
}

/* stream_read_blob() — read exactly `n` bytes (owned, NUL-terminated), or
 * NULL on short read or an over-limit length claim.                          */
static gchar *
stream_read_blob(GInputStream *in, gsize n)
{
    if (n > IPC_MAX_BLOB)
        return NULL;
    gchar *buf = g_malloc(n + 1);
    gsize  got = 0;                  /* bytes actually read                 */
    if (!g_input_stream_read_all(in, buf, n, &got, NULL, NULL) || got != n) {
        g_free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

/* stream_write_blob() — write "<len>\n" then the bytes.                      */
static gboolean
stream_write_blob(GOutputStream *o, const gchar *data, gsize n)
{
    gchar *hdr = g_strdup_printf("%" G_GSIZE_FORMAT "\n", n);
    gboolean ok = stream_write_all(o, hdr, strlen(hdr)) &&
                  stream_write_all(o, data, n);
    g_free(hdr);
    return ok;
}

/* ===========================================================================
 * shared note-path resolution
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * ipc_resolve_note_path() — turn a "note path" into a note id.
 *   db     — open database.
 *   path   — a numeric note id, or "Folder/Sub/Title" (last part = title).
 *   out_id — receives the resolved note id.
 * Returns TRUE if exactly one note was found.  Title match is exact.
 * ------------------------------------------------------------------------- */
static gboolean
ipc_resolve_note_path(OnDatabase *db, const gchar *path, gint64 *out_id)
{
    *out_id = 0;
    if (path == NULL || *path == '\0')
        return FALSE;

    /* A bare number is a note id (mirrors the rest of the CLI, which
     * addresses notes by id).                                               */
    gboolean all_digits = TRUE;      /* is every character a digit?          */
    for (const char *p = path; *p != '\0'; p++)
        if (!g_ascii_isdigit((guchar)*p)) { all_digits = FALSE; break; }
    if (all_digits) {
        gint64 id = g_ascii_strtoll(path, NULL, 10);
        OnNoteMeta *m = (id > 0) ? on_db_note_get(db, id) : NULL;
        if (m != NULL) {
            *out_id = id;
            on_db_note_meta_free(m);
            return TRUE;
        }
        return FALSE;
    }

    /* Otherwise the last non-empty path component is a note title and the
     * rest is the folder path holding it.                                   */
    gchar **parts = g_strsplit(path, "/", -1);
    gint    n     = (gint)g_strv_length(parts);
    gint    title_idx = -1;          /* index of the title component         */
    for (gint i = n - 1; i >= 0; i--)
        if (parts[i] != NULL && *parts[i] != '\0') { title_idx = i; break; }
    if (title_idx < 0) {
        g_strfreev(parts);
        return FALSE;
    }
    const gchar *title = parts[title_idx];

    GString *folder_path = g_string_new(NULL);   /* "Folder/Sub" prefix      */
    for (gint i = 0; i < title_idx; i++) {
        if (parts[i] == NULL || *parts[i] == '\0')
            continue;
        if (folder_path->len > 0)
            g_string_append_c(folder_path, '/');
        g_string_append(folder_path, parts[i]);
    }

    gint64 folder = 0;               /* resolved folder id (0 = top level)   */
    gboolean ok = on_cli_resolve_folder_path(db, folder_path->str, FALSE,
                                             &folder);
    g_string_free(folder_path, TRUE);
    if (!ok) {
        g_strfreev(parts);
        return FALSE;
    }

    gint64 found = 0;                /* first note whose title matches       */
    GList *notes = on_db_note_list(db, folder);
    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;
        if (g_strcmp0(m->title, title) == 0) {
            found = m->id;
            break;
        }
    }
    on_db_note_list_free(notes);
    g_strfreev(parts);

    if (found == 0)
        return FALSE;
    *out_id = found;
    return TRUE;
}

/* ===========================================================================
 * actions (run in the GUI process, whether reached over the socket or as a
 * pending action after the GUI started for us)
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * ipc_do_quicknote() — create an empty note in the root folder, refresh the
 * library, and open its editor to the front.
 * Returns a new "OK/ERR ..." reply line (g_free it).
 * ------------------------------------------------------------------------- */
static gchar *
ipc_do_quicknote(OnApp *app)
{
    gint64 id = on_db_note_create(app->db, 0);   /* 0 = root folder          */
    if (id == 0)
        return g_strdup("ERR could not create note");

    /* Reflect the new note in the library (sidebar counts + notes pane).    */
    if (app->notify_notes_changed != NULL)
        app->notify_notes_changed(app);

    GtkWidget *win = on_editor_window_open(app, id);
    if (win != NULL)
        gtk_window_present(GTK_WINDOW(win));
    return g_strdup_printf("OK created note %" G_GINT64_FORMAT, id);
}

/* ---------------------------------------------------------------------------
 * ipc_do_open() — open the editor for the note named by `path`, to the front.
 * Returns a new "OK/ERR ..." reply line (g_free it).
 * ------------------------------------------------------------------------- */
static gchar *
ipc_do_open(OnApp *app, const gchar *path)
{
    gint64 id;                       /* resolved note id                     */
    if (!ipc_resolve_note_path(app->db, path, &id))
        return g_strdup_printf("ERR no note matches path: %s", path);

    GtkWidget *win = on_editor_window_open(app, id);
    if (win == NULL)
        return g_strdup_printf("ERR could not open note: %s", path);
    gtk_window_present(GTK_WINDOW(win));
    return g_strdup_printf("OK opened note %" G_GINT64_FORMAT, id);
}

/* ---------------------------------------------------------------------------
 * ipc_dispatch() — run one request line and return its reply line (owned).
 * ------------------------------------------------------------------------- */
static gchar *
ipc_dispatch(OnApp *app, const gchar *line)
{
    if (g_strcmp0(line, "quicknote") == 0)
        return ipc_do_quicknote(app);
    if (g_str_has_prefix(line, "open ") && line[5] != '\0')
        return ipc_do_open(app, line + 5);
    return g_strdup_printf("ERR unknown command: %s", line);
}

/* ===========================================================================
 * pending action (no instance was running; do it once the GUI is up)
 * =========================================================================== */

typedef enum {
    IPC_PENDING_NONE = 0,
    IPC_PENDING_QUICKNOTE,
    IPC_PENDING_OPEN,
} IpcPending;

static IpcPending pending_kind = IPC_PENDING_NONE;
static gchar     *pending_path = NULL;   /* for IPC_PENDING_OPEN (owned)     */

void
on_ipc_set_pending_quicknote(void)
{
    pending_kind = IPC_PENDING_QUICKNOTE;
}

void
on_ipc_set_pending_open(const gchar *path)
{
    pending_kind = IPC_PENDING_OPEN;
    g_free(pending_path);
    pending_path = g_strdup(path);
}

gboolean
on_ipc_has_pending(void)
{
    return pending_kind != IPC_PENDING_NONE;
}

void
on_ipc_run_pending(OnApp *app)
{
    gchar *reply = NULL;             /* result line (for a warning on error) */
    switch (pending_kind) {
    case IPC_PENDING_QUICKNOTE:
        reply = ipc_do_quicknote(app);
        break;
    case IPC_PENDING_OPEN:
        reply = ipc_do_open(app, pending_path);
        break;
    case IPC_PENDING_NONE:
    default:
        return;
    }
    /* The GUI is already visible; surface only failures, on stderr.         */
    if (reply != NULL && g_str_has_prefix(reply, "ERR "))
        g_printerr("blue_notes: %s\n", reply + 4);
    g_free(reply);
    g_clear_pointer(&pending_path, g_free);
    pending_kind = IPC_PENDING_NONE;
}

/* ===========================================================================
 * server (running GUI instance)
 * =========================================================================== */

static GSocketService *ipc_service = NULL;   /* live listener, or NULL       */
static gchar          *ipc_bound_path = NULL;/* socket file to unlink at exit */

/* ---------------------------------------------------------------------------
 * ipc_read_file_all() — read a whole (rewound) FILE* into a new string.
 * ------------------------------------------------------------------------- */
static gchar *
ipc_read_file_all(FILE *f)
{
    GString *s = g_string_new(NULL);
    rewind(f);
    char   buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        g_string_append_len(s, buf, (gssize)n);
    return g_string_free(s, FALSE);
}

/* ---------------------------------------------------------------------------
 * ipc_capture_run() — execute a data command against the live database with
 * its stdout/stderr redirected to temp files, so the text can be shipped back
 * to the remote CLI.  Safe because we run synchronously on the main loop (no
 * other code writes fd 1/2 during the window).
 *   out_text, err_text — receive the captured streams (owned).
 * Returns the command's exit code.
 * ------------------------------------------------------------------------- */
static int
ipc_capture_run(OnApp *app, int argc, char **argv, const gchar *stdin_data,
                gchar **out_text, gchar **err_text)
{
    *out_text = NULL;
    *err_text = NULL;

    fflush(stdout);
    fflush(stderr);
    int   saved_out = dup(1);        /* real stdout/stderr fds              */
    int   saved_err = dup(2);
    FILE *fo = tmpfile();            /* capture files                       */
    FILE *fe = tmpfile();

    gboolean captured = saved_out >= 0 && saved_err >= 0 &&
                        fo != NULL && fe != NULL;
    if (captured) {
        dup2(fileno(fo), 1);
        dup2(fileno(fe), 2);
    }

    on_cli_set_stdin_data(stdin_data);
    int rc = on_cli_dispatch_db(app->db, argc, argv);
    on_cli_set_stdin_data(NULL);

    if (captured) {
        fflush(stdout);
        fflush(stderr);
        dup2(saved_out, 1);
        dup2(saved_err, 2);
        *out_text = ipc_read_file_all(fo);
        *err_text = ipc_read_file_all(fe);
    }
    if (saved_out >= 0) close(saved_out);
    if (saved_err >= 0) close(saved_err);
    if (fo != NULL) fclose(fo);
    if (fe != NULL) fclose(fe);
    return rc;
}

/* ---------------------------------------------------------------------------
 * ipc_handle_run() — server side of the RUN protocol: read the argv + stdin
 * frame, run the command against the live database, refresh the GUI if it
 * changed anything, and write back the exit code + captured output.
 * ------------------------------------------------------------------------- */
static void
ipc_handle_run(OnApp *app, GInputStream *in, GOutputStream *out)
{
    gssize nargs = stream_read_uint_line(in);
    if (nargs < 0 || nargs > 256)    /* sanity bound                        */
        return;

    /* Rebuild an argv the dispatcher understands: argv[0] is a placeholder
     * program name, argv[1..] the received words.                          */
    char    **argv = g_new0(char *, (gsize)nargs + 2);
    argv[0] = g_strdup("blue_notes");
    gboolean bad = FALSE;            /* frame decode failed?                */
    for (gssize i = 0; i < nargs && !bad; i++) {
        gssize len = stream_read_uint_line(in);
        gchar *arg = (len >= 0) ? stream_read_blob(in, (gsize)len) : NULL;
        if (arg == NULL)
            bad = TRUE;
        else
            argv[i + 1] = arg;
    }

    gssize slen = bad ? -1 : stream_read_uint_line(in);
    gchar *stdin_data = (slen >= 0) ? stream_read_blob(in, (gsize)slen) : NULL;

    if (!bad && stdin_data != NULL) {
        int argc = (int)nargs + 1;
        const gchar *sd = on_cli_command_reads_stdin(argc, argv)
                          ? stdin_data : NULL;
        gchar *out_text = NULL, *err_text = NULL;
        int rc = ipc_capture_run(app, argc, argv, sd, &out_text, &err_text);

        /* Let the running GUI reflect a change the command just made.       */
        if (on_cli_command_mutates(argc, argv) &&
            app->notify_notes_changed != NULL)
            app->notify_notes_changed(app);

        gchar *rc_line = g_strdup_printf("%d\n", rc);
        stream_write_all(out, rc_line, strlen(rc_line));
        stream_write_blob(out, out_text ? out_text : "",
                          out_text ? strlen(out_text) : 0);
        stream_write_blob(out, err_text ? err_text : "",
                          err_text ? strlen(err_text) : 0);
        g_free(rc_line);
        g_free(out_text);
        g_free(err_text);
    }

    g_free(stdin_data);
    g_strfreev(argv);
}

/* ---------------------------------------------------------------------------
 * on_incoming() — GSocketService "incoming" handler.  The first line selects
 * the protocol: "RUN" begins a delegated data command (framed); anything else
 * is a one-line GUI command ("quicknote" / "open PATH").  Runs on the main
 * loop, so touching widgets and the database is safe.
 * ------------------------------------------------------------------------- */
static gboolean
on_incoming(GSocketService *service, GSocketConnection *conn,
            GObject *source, gpointer user_data)
{
    (void)service; (void)source;
    OnApp *app = user_data;          /* shared application context           */

    /* This handler blocks the GTK main loop while it reads; the timeout
     * bounds how long a stalled or malicious client can freeze the GUI.    */
    g_socket_set_timeout(g_socket_connection_get_socket(conn),
                         IPC_TIMEOUT_S);

    GInputStream  *in  = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));

    gchar *first = stream_read_line(in);
    if (first != NULL) {
        if (g_strcmp0(first, "RUN") == 0) {
            ipc_handle_run(app, in, out);
        } else if (*first != '\0') {
            gchar *reply = ipc_dispatch(app, first);
            gchar *wire  = g_strconcat(reply, "\n", NULL);
            stream_write_all(out, wire, strlen(wire));
            g_free(wire);
            g_free(reply);
        }
    }
    g_free(first);

    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
    return TRUE;                      /* handled                             */
}

void
on_ipc_server_start(OnApp *app)
{
    if (ipc_service != NULL)
        return;                      /* already serving                     */

    gchar *path = ipc_socket_path();

    /* Clear a stale socket left by a crash, but never one a live instance
     * is still serving (that instance owns the name).                       */
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        GSocket *probe = g_socket_new(G_SOCKET_FAMILY_UNIX,
                                      G_SOCKET_TYPE_STREAM,
                                      G_SOCKET_PROTOCOL_DEFAULT, NULL);
        GSocketAddress *a = g_unix_socket_address_new(path);
        gboolean alive = probe != NULL &&
                         g_socket_connect(probe, a, NULL, NULL);
        g_object_unref(a);
        if (probe != NULL) {
            g_socket_close(probe, NULL);
            g_object_unref(probe);
        }
        if (alive) {                 /* another GUI already listens          */
            g_free(path);
            return;
        }
        g_unlink(path);
    }

    GSocketService *service = g_socket_service_new();
    GSocketAddress *addr = g_unix_socket_address_new(path);
    GError *err = NULL;               /* bind/listen error                   */
    gboolean ok = g_socket_listener_add_address(
        G_SOCKET_LISTENER(service), addr, G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &err);
    g_object_unref(addr);
    if (!ok) {                        /* non-fatal: GUI runs, remote won't   */
        g_warning("ipc: cannot listen on %s: %s", path,
                  err != NULL ? err->message : "unknown error");
        g_clear_error(&err);
        g_object_unref(service);
        g_free(path);
        return;
    }

    g_signal_connect(service, "incoming", G_CALLBACK(on_incoming), app);
    g_socket_service_start(service);

    ipc_service    = service;
    ipc_bound_path = path;           /* ownership kept for on_ipc_server_stop */
}

void
on_ipc_server_stop(void)
{
    if (ipc_service != NULL) {
        g_socket_service_stop(ipc_service);
        g_socket_listener_close(G_SOCKET_LISTENER(ipc_service));
        g_object_unref(ipc_service);
        ipc_service = NULL;
    }
    if (ipc_bound_path != NULL) {
        g_unlink(ipc_bound_path);
        g_clear_pointer(&ipc_bound_path, g_free);
    }
}

/* ===========================================================================
 * client (short-lived CLI invocation)
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * ipc_connect() — create a client socket and connect it to the per-user
 * instance socket, with IPC_TIMEOUT_S set on every socket operation so a
 * wedged server can never hang the CLI.
 * Returns a connected GSocket (g_object_unref it), or NULL when no
 * instance is listening (or the socket could not be created).
 * ------------------------------------------------------------------------- */
static GSocket *
ipc_connect(void)
{
    gchar   *path = ipc_socket_path();
    GSocket *sock = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                                 G_SOCKET_PROTOCOL_DEFAULT, NULL);
    if (sock != NULL) {
        g_socket_set_timeout(sock, IPC_TIMEOUT_S);
        GSocketAddress *addr = g_unix_socket_address_new(path);
        if (!g_socket_connect(sock, addr, NULL, NULL))
            g_clear_object(&sock);   /* nobody home                         */
        g_object_unref(addr);
    }
    g_free(path);
    return sock;
}

OnIpcResult
on_ipc_try_remote(const gchar *command, gchar **reply_out)
{
    if (reply_out != NULL)
        *reply_out = NULL;

    /* The whole exchange is one short line each way; on connect failure
     * the caller starts the GUI itself.                                    */
    GSocket *sock = ipc_connect();
    if (sock == NULL)
        return ON_IPC_NO_SERVER;

    /* Send the command, then read the single reply line.                    */
    gchar *wire = g_strconcat(command, "\n", NULL);
    g_socket_send(sock, wire, strlen(wire), NULL, NULL);
    g_free(wire);

    GString *reply = g_string_new(NULL);
    gchar    buf[512];               /* reply is a single short line         */
    for (;;) {
        gssize got = g_socket_receive(sock, buf, sizeof buf, NULL, NULL);
        if (got <= 0)
            break;
        g_string_append_len(reply, buf, got);
        if (memchr(buf, '\n', (size_t)got) != NULL)
            break;
    }
    g_socket_close(sock, NULL);
    g_object_unref(sock);

    /* Trim the trailing newline and split the OK/ERR status prefix.         */
    g_strchomp(reply->str);
    OnIpcResult result = g_str_has_prefix(reply->str, "OK")
                         ? ON_IPC_OK : ON_IPC_ERROR;
    const gchar *msg = reply->str;
    if (g_str_has_prefix(msg, "OK ") || g_str_has_prefix(msg, "ERR "))
        msg += (result == ON_IPC_OK) ? 3 : 4;
    if (reply_out != NULL)
        *reply_out = g_strdup(msg);
    g_string_free(reply, TRUE);
    return result;
}

int
on_ipc_try_remote_run(int argc, char **argv, gboolean *ran)
{
    *ran = FALSE;

    /* ipc_connect's timeout bounds the connect + request send; the
     * response wait is made unbounded again below — the server may
     * legitimately run a long command.  On connect failure the caller
     * runs headless instead.                                               */
    GSocket *sock = ipc_connect();
    if (sock == NULL)
        return 0;

    GSocketConnection *conn = g_socket_connection_factory_create_connection(sock);
    GInputStream  *in  = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));

    /* Slurp our stdin only for the one command that consumes it — the
     * instance cannot read this process's file descriptor.                  */
    gchar *stdin_data = g_strdup("");/* shipped stdin (owned)               */
    gsize  stdin_len  = 0;
    if (on_cli_command_reads_stdin(argc, argv)) {
        GString *s = g_string_new(NULL);
        char   buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, stdin)) > 0)
            g_string_append_len(s, buf, (gssize)n);
        stdin_len = s->len;
        g_free(stdin_data);
        stdin_data = g_string_free(s, FALSE);
    }

    /* Request frame: "RUN", the word count, each word, then stdin.          */
    stream_write_all(out, "RUN\n", 4);
    gchar *count = g_strdup_printf("%d\n", argc - 1);
    stream_write_all(out, count, strlen(count));
    g_free(count);
    for (int i = 1; i < argc; i++)
        stream_write_blob(out, argv[i], strlen(argv[i]));
    stream_write_blob(out, stdin_data, stdin_len);
    g_free(stdin_data);

    /* The command may take as long as it takes (e.g. a full export) —
     * only the connect/send above are timeout-guarded.                     */
    g_socket_set_timeout(sock, 0);

    /* Response frame: exit code, captured stdout, captured stderr.          */
    gssize rc   = stream_read_uint_line(in);
    gssize olen = stream_read_uint_line(in);
    gchar *otext = (olen >= 0) ? stream_read_blob(in, (gsize)olen) : NULL;
    gssize elen = (otext != NULL) ? stream_read_uint_line(in) : -1;
    gchar *etext = (elen >= 0) ? stream_read_blob(in, (gsize)elen) : NULL;

    if (otext != NULL && olen > 0)
        fwrite(otext, 1, (size_t)olen, stdout);
    if (etext != NULL && elen > 0)
        fwrite(etext, 1, (size_t)elen, stderr);
    fflush(stdout);
    fflush(stderr);

    g_free(otext);
    g_free(etext);
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
    g_object_unref(conn);
    g_object_unref(sock);

    *ran = TRUE;
    return (rc < 0) ? 2 : (int)rc;   /* decode failure → generic error       */
}
