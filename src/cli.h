/* ===========================================================================
 * cli.h — command-line automation interface
 *
 * When the binary is invoked with a subcommand, the action runs headless
 * (no windows) against the same database the GUI uses — including a
 * configured custom/shared location — and the process exits.  With no
 * subcommand the GUI starts as usual.
 *
 * Commands follow a noun-verb structure:
 *
 *   orange_notes tag list
 *   orange_notes tag delete NAME
 *   orange_notes folder list
 *   orange_notes folder add PATH            (nested, created like mkdir -p)
 *   orange_notes folder delete PATH
 *   orange_notes note list [PATH|--all]
 *   orange_notes note new [--folder PATH] CONTENT|-   (- reads stdin)
 *   orange_notes note delete ID [ID...]
 *   orange_notes note move ID [ID...] PATH  (/ = top level)
 *   orange_notes backup FILE.db
 *   orange_notes export-md DIR
 *   orange_notes export-html DIR
 * =========================================================================== */

#ifndef ORANGE_CLI_H
#define ORANGE_CLI_H

/* ---------------------------------------------------------------------------
 * on_cli_run() — dispatch a command-line invocation.
 *   argc/argv — the program arguments as passed to main().
 * Returns a process exit code (>= 0) when a subcommand was handled, or
 * -1 when no subcommand was given and the GUI should start.
 * ------------------------------------------------------------------------- */
int on_cli_run(int argc, char **argv);

#endif /* ORANGE_CLI_H */
