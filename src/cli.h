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
 *   blue_notes tag list
 *   blue_notes tag delete NAME
 *   blue_notes folder list
 *   blue_notes folder add PATH            (nested, created like mkdir -p)
 *   blue_notes folder delete PATH
 *   blue_notes note list [PATH|--all]
 *   blue_notes note new [--folder PATH] CONTENT|-   (- reads stdin)
 *   blue_notes note delete ID [ID...]
 *   blue_notes note move ID [ID...] PATH  (/ = top level)
 *   blue_notes backup FILE.db
 *   blue_notes export-md DIR
 *   blue_notes export-html DIR
 * =========================================================================== */

#ifndef BLUE_CLI_H
#define BLUE_CLI_H

/* ---------------------------------------------------------------------------
 * on_cli_run() — dispatch a command-line invocation.
 *   argc/argv — the program arguments as passed to main().
 * Returns a process exit code (>= 0) when a subcommand was handled, or
 * -1 when no subcommand was given and the GUI should start.
 * ------------------------------------------------------------------------- */
int on_cli_run(int argc, char **argv);

#endif /* BLUE_CLI_H */
