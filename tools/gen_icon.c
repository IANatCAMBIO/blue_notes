/* One-shot generator for orange.png — a simple orange-fruit app icon
 * drawn with cairo: orange disc, highlight, green leaf, note lines.        */
#include <cairo.h>
#include <math.h>

int
main(void)
{
    const int SZ = 512;
    cairo_surface_t *s =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, SZ, SZ);
    cairo_t *cr = cairo_create(s);

    double cx = SZ / 2.0, cy = SZ / 2.0 + 20, r = SZ * 0.40;

    /* Soft drop shadow.                                                     */
    cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
    cairo_arc(cr, cx + 6, cy + 10, r, 0, 2 * M_PI);
    cairo_fill(cr);

    /* Orange body with a radial gradient.                                   */
    cairo_pattern_t *g = cairo_pattern_create_radial(
        cx - r * 0.4, cy - r * 0.4, r * 0.1, cx, cy, r * 1.1);
    cairo_pattern_add_color_stop_rgb(g, 0.0, 1.00, 0.72, 0.25);
    cairo_pattern_add_color_stop_rgb(g, 0.6, 0.98, 0.55, 0.05);
    cairo_pattern_add_color_stop_rgb(g, 1.0, 0.85, 0.42, 0.00);
    cairo_set_source(cr, g);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(g);

    /* Leaf.                                                                 */
    cairo_set_source_rgb(cr, 0.30, 0.62, 0.22);
    cairo_move_to(cr, cx + 8, cy - r + 6);
    cairo_curve_to(cr, cx + 40, cy - r - 60, cx + 120, cy - r - 40,
                   cx + 96, cy - r + 10);
    cairo_curve_to(cr, cx + 60, cy - r + 30, cx + 24, cy - r + 22,
                   cx + 8, cy - r + 6);
    cairo_fill(cr);

    /* Stem.                                                                 */
    cairo_set_source_rgb(cr, 0.42, 0.28, 0.12);
    cairo_set_line_width(cr, 12);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, cx, cy - r + 2);
    cairo_line_to(cr, cx + 6, cy - r - 26);
    cairo_stroke(cr);

    /* Subtle highlight for depth.                                          */
    cairo_set_source_rgba(cr, 1, 1, 1, 0.25);
    cairo_arc(cr, cx - r * 0.35, cy - r * 0.35, r * 0.28, 0, 2 * M_PI);
    cairo_fill(cr);

    cairo_destroy(cr);
    cairo_surface_write_to_png(
        s, "/Users/ian_campbell/salt_development/orange_notes/orange.png");
    cairo_surface_destroy(s);
    return 0;
}
