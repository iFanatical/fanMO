#include "style.h"
#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QSettings>
#include <QStandardPaths>
#include <QPainter>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOption>

static QString css(const QColor &c)
{
    if (c.alpha() == 255)
        return c.name();
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

/* Fusion, except for the focus frame: Fusion fills the current row with a
 * bordered box, which reads as a second selection. GTK marks it with a faint
 * outline (the theme's outline-color), so do that. */
class FlatStyle : public QProxyStyle {
public:
    FlatStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter, const QWidget *widget) const override
    {
        if (element != PE_FrameFocusRect) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(Theme::Gtk::focusOutline, 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5),
                                 2, 2);
        painter->restore();
    }
};

/* @tokens are filled from Theme::Gtk, so the colours live in one place. */
static const char *const styleSheet = R"(
QMainWindow, QStatusBar { background: @base; }
QStatusBar { border-top: 1px solid @line; }
QStatusBar::item { border: none; }

QToolBar#toolbar {
    background: @toolbar;
    border: none;
    border-bottom: 1px solid @toolbarLine;
    padding: 4px 6px;
    spacing: 4px;
}
QToolBar#toolbar QToolButton {
    background: transparent;
    border: none;
    border-radius: @radius;
    padding: 4px;
}
QToolBar#toolbar QToolButton:hover { background: @hover; }
QToolBar#toolbar QToolButton:pressed { background: @headerHover; }

QLineEdit {
    background: @base;
    color: @text;
    border: 1px solid @line;
    border-radius: @radius;
    padding: 4px 8px;
    selection-background-color: @selected;
    selection-color: @selectedText;
}
QLineEdit:focus { border-color: @selected; }

QAbstractItemView {
    background: @base;
    border: 1px solid @line;
    outline: 0;
    selection-background-color: @selected;
    selection-color: @selectedText;
}
QTreeView { border: none; }

QHeaderView { background: @headerBg; border: none; }
QHeaderView::section {
    background: @headerBg;
    color: @headerText;
    padding: 3px 6px;
    border: none;
    border-right: 1px solid @line;
    border-bottom: 1px solid @line;
}
QHeaderView::section:hover { background: @headerHover; }
QHeaderView::section:last { border-right: none; }

QScrollBar:vertical {
    background: @scrollTrack;
    width: 14px;
    margin: 0;
    border: none;
    border-left: 1px solid @line;
}
QScrollBar:horizontal {
    background: @scrollTrack;
    height: 14px;
    margin: 0;
    border: none;
    border-top: 1px solid @line;
}
QScrollBar::handle { background: @slider; border-radius: 3px; }
QScrollBar::handle:vertical { min-height: 32px; margin: 4px 4px 4px 3px; }
QScrollBar::handle:horizontal { min-width: 32px; margin: 3px 4px 4px 4px; }
QScrollBar::handle:hover { background: @sliderHover; }
QScrollBar::handle:pressed { background: @sliderActive; }
QScrollBar::add-line, QScrollBar::sub-line {
    width: 0; height: 0; border: none; background: none;
}
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

/* the one line between the panes (the side pane draws no border of its own) */
QSplitter::handle { background: @line; width: 1px; }

QSlider:horizontal { min-height: 17px; }
QSlider::groove:horizontal {
    height: 5px;
    background: @scaleTrough;
    border-radius: 2px;
}
QSlider::sub-page:horizontal { background: @selected; border-radius: 2px; }
QSlider::handle:horizontal {
    width: 13px;
    height: 13px;
    margin: -5px 0;
    border-radius: 7px;
    background: @scaleSlider;
    border: 1px solid @scaleSliderBorder;
}
QSlider::handle:horizontal:hover { background: @headerHover; }
QSlider::handle:horizontal:pressed { background: @selected; }

QDialog { background: @base; }

QPushButton {
    background: @button;
    color: @text;
    border: 1px solid @line;
    border-radius: @radius;
    padding: 5px 14px;
    min-height: 18px;
}
QPushButton:hover { background: @buttonHover; }
QPushButton:pressed { background: @buttonActive; }
QPushButton:focus { border-color: @selected; }
QPushButton#suggested {
    background: @selected;
    color: @selectedText;
    border-color: @selected;
}
QPushButton#destructive {
    background: @destructive;
    color: @selectedText;
    border-color: @destructive;
}
QPushButton:disabled { color: @textDisabled; }

QProgressBar {
    background: @progressTrough;
    border: 1px solid @line;
    border-radius: 2px;
    max-height: 6px;
}
QProgressBar::chunk { background: @selected; }

QCheckBox { color: @text; spacing: 6px; }
QCheckBox::indicator {
    width: 14px;
    height: 14px;
    border: 1px solid @line;
    border-radius: 3px;
    background: @button;
}
QCheckBox::indicator:checked { background: @selected; border-color: @selected; }

QTreeView QLineEdit {
    padding: 0 2px;
    border-radius: 2px;
    border-color: @selected;
}

QMenu {
    background: @base;
    border: 1px solid @line;
    padding: 4px 0;
}
QMenu::item {
    padding: 5px 16px 5px 8px;
    color: @text;
    background: transparent;
}
QMenu::item:selected { background: @hover; }
QMenu::item:disabled { color: @textDisabled; }
QMenu::icon { padding-left: 6px; }
QMenu::separator { height: 1px; background: @line; margin: 4px 0; }


QTabWidget::pane { border: none; border-top: 1px solid @line; background: @base; }
QTabBar { background: @toolbar; }
QTabBar::tab {
    background: transparent;
    color: @headerText;
    padding: 6px 14px;
    border: none;
    border-bottom: 2px solid transparent;
}
QTabBar::tab:hover { background: @hover; color: @text; }
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @selected; }

QComboBox {
    background: @button;
    color: @text;
    border: 1px solid @line;
    border-radius: @radius;
    padding: 4px 8px;
    min-height: 18px;
}
QComboBox:hover { background: @buttonHover; }
QComboBox:focus { border-color: @selected; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView {
    background: @base;
    border: 1px solid @line;
    selection-background-color: @hover;
    selection-color: @text;
}

QPlainTextEdit, QTextEdit, QTextBrowser {
    background: @base;
    color: @text;
    border: none;
    selection-background-color: @selected;
    selection-color: @selectedText;
}

QGroupBox {
    color: @headerText;
    border: 1px solid @line;
    border-radius: @radius;
    margin-top: 10px;
    padding: 8px 6px 6px 6px;
}
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }

QRadioButton { color: @text; spacing: 6px; }
QRadioButton::indicator {
    width: 14px;
    height: 14px;
    border: 1px solid @line;
    border-radius: 7px;
    background: @button;
}
QRadioButton::indicator:checked { background: @selected; border-color: @selected; }
QRadioButton:disabled, QCheckBox:disabled { color: @textDisabled; }

QLabel { color: @text; }
QLabel#muted { color: @headerText; }
QLabel#heading { color: @text; font-weight: bold; }

QSplitter::handle:vertical { height: 1px; width: auto; }

QTreeView { alternate-background-color: @headerHover; }
QTreeView::item { padding: 0 2px; }

QToolBar#toolbar QLabel { padding: 0 2px 0 8px; color: @headerText; }
QToolBar#toolbar QToolButton { padding: 4px 8px; }
QToolBar#toolbar QPushButton { padding: 4px 12px; }

QToolTip {
    background: @tooltip;
    color: @tooltipText;
    border: none;
    padding: 4px 6px;
}
)";

static QString buildStyleSheet()
{
    using namespace Theme::Gtk;
    /* Longest names first, so @headerHover isn't matched as @header... */
    const std::pair<const char *, QString> tokens[] = {
        {"@scaleSliderBorder", css(scaleSliderBorder)},
        {"@scaleSlider", css(scaleSlider)},
        {"@scaleTrough", css(scaleTrough)},
        {"@progressTrough", css(progressTrough)},
        {"@buttonActive", css(buttonActive)},
        {"@buttonHover", css(buttonHover)},
        {"@destructive", css(destructive)},
        {"@button", css(button)},
        {"@textDisabled", css(textDisabled)},
        {"@selectedText", css(selectedText)},
        {"@toolbarLine", css(toolbarLine)},
        {"@sliderActive", css(sliderActive)},
        {"@sliderHover", css(sliderHover)},
        {"@headerHover", css(headerHover)},
        {"@headerText", css(headerText)},
        {"@scrollTrack", css(scrollTrack)},
        {"@tooltipText", css(tooltipText)},
        {"@headerBg", css(headerBg)},
        {"@selected", css(selected)},
        {"@toolbar", css(toolbar)},
        {"@tooltip", css(tooltip)},
        {"@slider", css(slider)},
        {"@radius", QStringLiteral("%1px").arg(radius)},
        {"@hover", css(hover)},
        {"@base", css(base)},
        {"@line", css(line)},
        {"@text", css(text)},
    };
    QString sheet = QString::fromUtf8(styleSheet);
    for (const auto &[token, value] : tokens)
        sheet.replace(QLatin1String(token), value);
    return sheet;
}

void Style::prepare()
{
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
}

/* gtk-icon-theme-name from GTK 3's settings.ini, or empty. */
static QString gtkIconTheme()
{
    const QString file =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/gtk-3.0/settings.ini");
    QSettings ini(file, QSettings::IniFormat);
    return ini.value(QStringLiteral("Settings/gtk-icon-theme-name")).toString();
}

void Style::apply(QApplication &app)
{
    /* Where icon themes live, per the icon theme spec -- set here rather
     * than left to whichever platform theme is loaded to provide. */
    QStringList paths;
    for (const QString &dir :
         QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
        paths << dir + QStringLiteral("/icons");
    paths << QDir::homePath() + QStringLiteral("/.icons")
          << QStringLiteral("/usr/share/pixmaps");
    QIcon::setThemeSearchPaths(paths);
    QIcon::setFallbackThemeName(QStringLiteral("hicolor"));
    const QString icons = gtkIconTheme();
    if (!icons.isEmpty())
        QIcon::setThemeName(icons);

    using namespace Theme::Gtk;
    app.setStyle(new FlatStyle);

    QPalette pal;
    pal.setColor(QPalette::Window, base);
    pal.setColor(QPalette::WindowText, text);
    pal.setColor(QPalette::Base, base);
    pal.setColor(QPalette::AlternateBase, headerHover);
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::BrightText, tooltipText);
    pal.setColor(QPalette::Button, headerBg);
    pal.setColor(QPalette::ButtonText, text);
    pal.setColor(QPalette::Highlight, selected);
    pal.setColor(QPalette::HighlightedText, selectedText);
    pal.setColor(QPalette::ToolTipBase, tooltip);
    pal.setColor(QPalette::ToolTipText, tooltipText);
    pal.setColor(QPalette::PlaceholderText, textDisabled);
    pal.setColor(QPalette::Link, selected);
    pal.setColor(QPalette::Light, headerHover);
    pal.setColor(QPalette::Midlight, headerBg);
    pal.setColor(QPalette::Mid, line);
    pal.setColor(QPalette::Dark, line);
    pal.setColor(QPalette::Shadow, line);
    for (QPalette::ColorRole role :
         {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        pal.setColor(QPalette::Disabled, role, textDisabled);
    app.setPalette(pal);

    QFont font(QString::fromUtf8(fontFamily));
    font.setPointSizeF(fontPointSize);
    app.setFont(font);
    /* The platform theme also hands some classes fonts of their own -- and
     * here that is gtk-font-name's uninstalled "...Propo", which falls back
     * to a generic sans in labels and message boxes. Every class it can
     * name gets the one font. */
    for (const char *cls : {"QLabel", "QMessageBox", "QTipLabel", "QStatusBar",
                            "QPushButton", "QCheckBox", "QRadioButton", "QToolButton",
                            "QAbstractItemView", "QListView", "QHeaderView",
                            "QMenu", "QMenuBar", "QComboMenuItem", "QComboLineEdit",
                            "QLineEdit", "QDockWidgetTitle", "QGroupBox", "QTabBar"})
        app.setFont(font, cls);

    app.setStyleSheet(buildStyleSheet());
}
