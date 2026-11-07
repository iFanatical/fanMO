/* Theme constants (Tokyo Night). Copied from fanFM's src/theme.h on
 * purpose rather than included from it, so the two projects build and change
 * independently; the bar-only constants (panel, tray, tags, toasts) were left
 * behind. */
#ifndef FANMO_THEME_H
#define FANMO_THEME_H

#include <QColor>
#include <QFont>

namespace Theme {

inline const QColor bg           ("#1a1b26");
inline const QColor surface      ("#32344a");
inline const QColor surfaceHover ("#444b6a");
inline const QColor border       ("#444b6a");
inline const QColor text         ("#dddddd");
inline const QColor textMuted    ("#787c99");
inline const QColor accent       ("#bc70ff");  /* purple/pink accent */
inline const QColor selected     ("#bc70ff");  /* purple/pink selection */
inline const QColor urgent       ("#f7768e");
inline const QColor transparent  (0, 0, 0, 0);
inline const QColor textStrong   ("#c0caf5");
inline const QColor danger       ("#f7768e");  /* red */
inline const QColor success      ("#9ece6a");  /* green */
inline const QColor warning      ("#e0af68");  /* yellow */

inline const QColor black        ("#32344a");
inline const QColor red          ("#f7768e");
inline const QColor green        ("#9ece6a");
inline const QColor yellow       ("#e0af68");
inline const QColor blue         ("#7aa2f7");
inline const QColor magenta      ("#ad8ee6");
inline const QColor cyan         ("#0db9d7");
inline const QColor white        ("#787c99");

inline const QColor brightblack  ("#444b6a");
inline const QColor brightred    ("#ff7a93");
inline const QColor brightgreen  ("#b9f27c");
inline const QColor brightyellow ("#ff9e64");
inline const QColor brightblue   ("#7da6ff");
inline const QColor brightmagenta("#bb9af7");
inline const QColor brightcyan   ("#449dab");
inline const QColor brightwhite  ("#acb0d0");

/* Widget chrome, taken from the Tokyo-Night GTK theme
 * (~/.themes/Tokyo-Night/gtk-3.0/gtk-dark.css) so fanMO sits flat among the
 * GTK apps rather than looking like the bar. The selector each comes from
 * is noted. */
namespace Gtk {
inline const QColor text          ("#c0caf5");  /* .background */
inline const QColor textDisabled  (192, 202, 245, 140);
inline const QColor base          ("#1a1b26");  /* .view */
inline const QColor selected      ("#7aa2f7");  /* .view:selected */
inline const QColor selectedText  ("#1a1b26");
inline const QColor line          ("#101014");  /* borders, dividers */
inline const QColor toolbar       ("#16161e");  /* .titlebar */
inline const QColor toolbarLine   ("#0d0d12");
inline const QColor hover         (255, 255, 255, 25);  /* menu menuitem:hover */
inline const QColor focusOutline  (122, 162, 247, 90);  /* * outline-color */
inline const QColor headerBg      ("#181923");  /* treeview header button */
inline const QColor headerHover   ("#1e1f2c");
inline const QColor headerText    ("#9fa7cc");
inline const QColor scrollTrack   ("#181923");  /* scrollbar */
inline const QColor slider        ("#5c6179");  /* scrollbar slider */
inline const QColor sliderHover   ("#4c5064");
inline const QColor sliderActive  ("#7aa2f7");
inline const QColor sidebar       ("#1e1f2c");  /* .sidebar */
inline const QColor tooltip       ("#749aeb");  /* tooltip.background */
inline const QColor tooltipText   ("#ffffff");
inline const QColor button        ("#222432");  /* button */
inline const QColor buttonHover   ("#2d2e41");
inline const QColor buttonActive  (255, 255, 255, 38);
inline const QColor destructive   ("#f7768e");  /* button.destructive-action */
inline const QColor progressTrough("#14151d");  /* progressbar trough */
inline const QColor scaleTrough   ("#101017");  /* scale trough */
inline const QColor scaleSlider   ("#222432");  /* scale slider */
inline const QColor scaleSliderBorder("#0b0b0e");
inline constexpr int radius       = 3;          /* entry */
/* gtk-font-name names "JetBrainsMonoNL Nerd Font Propo", which isn't
 * installed; fontconfig lands GTK on this, so this is what GTK apps show.
 * No size in gtk-font-name means GTK's default 10pt. NL: no ligatures, so
 * "->" in a file name stays two characters. */
inline const char *const fontFamily = "JetBrainsMonoNL Nerd Font";
inline constexpr qreal fontPointSize = 10;
} /* namespace Gtk */

inline const char *const fontFamily = "JetBrainsMono Nerd Font Propo";

/* fanMO layout */
inline constexpr int rowHeight = 24;

} /* namespace Theme */

#endif
