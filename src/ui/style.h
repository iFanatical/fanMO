/* The application's look, shared with fanFM: Fusion for the geometry, then a
 * palette and a stylesheet carrying the Tokyo-Night GTK theme's flat chrome. */
#ifndef FANMO_STYLE_H
#define FANMO_STYLE_H

class QApplication;

namespace Style {
/* Before the QApplication exists: the look comes from the GTK theme, so the
 * platform theme (qt6ct here) is left out -- it would otherwise re-apply its
 * own font over this one once the event loop starts. */
void prepare();
void apply(QApplication &app);
} /* namespace Style */

#endif
