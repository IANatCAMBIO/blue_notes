/* ===========================================================================
 * settings_window.h — the application settings window
 *
 * Opened from File → Settings…, this window hosts:
 *
 *   - Toolbar button style (text / icons / icons above text), set
 *     independently for library windows and editor windows.
 *
 * All changes apply immediately and persist in the settings table.
 * (Toolbar icons are the bundled elementary SVG set; a hint is shown
 * when the librsvg gdk-pixbuf loader they need is missing.)
 * =========================================================================== */

#ifndef ORANGE_SETTINGS_WINDOW_H
#define ORANGE_SETTINGS_WINDOW_H

#include "app.h"

/* ---------------------------------------------------------------------------
 * on_settings_window_open() — show the settings window (a new one per
 * call; it is transient for the library window).
 *   app — global application context.
 * ------------------------------------------------------------------------- */
void on_settings_window_open(OnApp *app);

#endif /* ORANGE_SETTINGS_WINDOW_H */
