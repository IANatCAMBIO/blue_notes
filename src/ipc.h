/* ===========================================================================
 * ipc.h — talk to an already-running Blue Notes instance
 *
 * The GUI process listens on a per-user Unix-domain socket
 * (<runtime-dir>/blue_notes.sock).  Short-lived CLI invocations
 * ("blue_notes quicknote" / "blue_notes note open PATH") connect to that
 * socket and hand the running instance a one-line command so the note opens
 * in the existing windows rather than a second copy of the app.  When no
 * instance is listening the CLI records the request as a "pending action" and
 * returns control to main() so the GUI starts and performs it at activate.
 *
 * D-Bus (GApplication's usual single-instance channel) is not available on
 * the macOS target, so this bespoke socket both DETECTS a running instance
 * (connect succeeds) and CARRIES the command to it.
 * =========================================================================== */

#ifndef BLUE_IPC_H
#define BLUE_IPC_H

#include "app.h"

/* Outcome of trying to reach a running instance (client side).              */
typedef enum {
    ON_IPC_NO_SERVER = 0,            /* nothing listening — start the GUI    */
    ON_IPC_OK,                       /* running instance handled the command */
    ON_IPC_ERROR,                    /* it answered but reported a failure   */
} OnIpcResult;

/* ---------------------------------------------------------------------------
 * on_ipc_try_remote() — CLIENT: connect to a running instance and send one
 * command line ("quicknote" or "open PATH").  On a reply, *reply_out receives
 * the instance's human-readable message (newline stripped; g_free it).
 *   command   — the command line to send (no trailing newline needed).
 *   reply_out — receives the reply message, or NULL; may be NULL to ignore.
 * Returns ON_IPC_NO_SERVER when no instance is listening, otherwise ON_IPC_OK
 * or ON_IPC_ERROR per the instance's answer.
 * ------------------------------------------------------------------------- */
OnIpcResult on_ipc_try_remote(const gchar *command, gchar **reply_out);

/* ---------------------------------------------------------------------------
 * on_ipc_try_remote_run() — CLIENT: ask a running instance to execute a data
 * command (tag/folder/note/backup/export-*) against its live database and
 * stream the captured stdout/stderr back here.  On success this prints that
 * output to stdout/stderr and returns the command's exit code with *ran set
 * TRUE.  When no instance is listening it returns 0 with *ran FALSE, so the
 * caller runs the command headless instead.
 *   argc/argv — the full program arguments (argv[1] is the command).
 *   ran       — set TRUE when a running instance handled the command.
 * ------------------------------------------------------------------------- */
int on_ipc_try_remote_run(int argc, char **argv, gboolean *ran);

/* ---------------------------------------------------------------------------
 * on_ipc_set_pending_quicknote() / on_ipc_set_pending_open() — record the
 * action for the GUI to run once it has started (used when no running
 * instance answered).  on_ipc_run_pending() executes and clears it.
 * ------------------------------------------------------------------------- */
void on_ipc_set_pending_quicknote(void);
void on_ipc_set_pending_open(const gchar *path);
void on_ipc_run_pending(OnApp *app);

/* on_ipc_has_pending() — TRUE when a pending action is queued.  main() uses
 * this to launch the GtkApplication with a clean argv (the leftover CLI words
 * like "quicknote" would otherwise be taken as files to open).              */
gboolean on_ipc_has_pending(void);

/* ---------------------------------------------------------------------------
 * on_ipc_server_start() — SERVER: begin listening for CLI commands.  Safe to
 * call once, after the library window exists.  A stale socket left by a
 * crashed instance is cleared first; a genuinely-live one means another GUI
 * already serves and this call becomes a no-op.
 * ------------------------------------------------------------------------- */
void on_ipc_server_start(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_ipc_server_stop() — stop listening and unlink the socket file.  Called
 * at shutdown after the main loop returns.
 * ------------------------------------------------------------------------- */
void on_ipc_server_stop(void);

#endif /* BLUE_IPC_H */
