#include "Style.h"

#include "Palette.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace {
QString hex(const QColor& c) { return c.name(QColor::HexRgb); }
QString rgba(const QColor& c) { return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha()); }
}

void applyDarkTheme(QApplication& app)
{
    app.setStyle(QStyleFactory::create("Fusion"));

    QPalette pal;
    pal.setColor(QPalette::Window, Pal::window);
    pal.setColor(QPalette::WindowText, Pal::text);
    pal.setColor(QPalette::Base, Pal::panel);
    pal.setColor(QPalette::AlternateBase, Pal::raised);
    pal.setColor(QPalette::ToolTipBase, Pal::raised);
    pal.setColor(QPalette::ToolTipText, Pal::text);
    pal.setColor(QPalette::Text, Pal::text);
    pal.setColor(QPalette::Button, Pal::raised);
    pal.setColor(QPalette::ButtonText, Pal::text);
    pal.setColor(QPalette::Highlight, Pal::selected);
    pal.setColor(QPalette::HighlightedText, Pal::textStrong);
    pal.setColor(QPalette::PlaceholderText, Pal::textFaint);
    pal.setColor(QPalette::Mid, Pal::border);
    pal.setColor(QPalette::Dark, Pal::divider);
    pal.setColor(QPalette::Disabled, QPalette::Text, Pal::textFaint);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, Pal::textFaint);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, Pal::textFaint);
    app.setPalette(pal);

    QString css = R"(
        * { font-family: "Segoe UI Variable Text", "Segoe UI", "Malgun Gothic"; font-size: 12px; outline: 0; }
        QMainWindow, QDialog { background: @window; }
        QToolTip { background: @raised; color: @text; border: 1px solid @border; padding: 4px 6px; border-radius: 6px; }

        QMenuBar { background: @window; color: @text; padding: 2px 4px; }
        QMenuBar::item { padding: 4px 9px; border-radius: 6px; background: transparent; }
        QMenuBar::item:selected { background: @hover; }
        QMenu { background: @panel; border: 1px solid @border; border-radius: 8px; padding: 5px; }
        QMenu::item { padding: 5px 26px 5px 12px; border-radius: 5px; color: @text; }
        QMenu::item:selected { background: @hover; }
        QMenu::item:disabled { color: @faint; }
        QMenu::separator { height: 1px; background: @border; margin: 5px 6px; }
        QMenu::indicator { width: 14px; height: 14px; left: 6px; }

        #Panel { background: @panel; }
        #PanelHeader { background: @panel; border-bottom: 1px solid @divider; }
        #PanelTitle { color: @dim; font-size: 11px; }
        #PanelFooter { background: @panel; border-top: 1px solid @divider; }
        #ToolStrip { background: @window; border-left: 1px solid @divider; border-right: 1px solid @divider; }
        #ClipBar, #TimelineBar { background: @viewer; }
        #BottomBar { background: @window; border-top: 1px solid @divider; }
        QSplitter::handle { background: @divider; }
        QSplitter::handle:horizontal { width: 1px; }
        QSplitter::handle:vertical { height: 1px; }

        QToolButton { background: transparent; border: 0; border-radius: 6px; padding: 3px; color: @text; }
        QToolButton:hover { background: @hover; }
        QToolButton:pressed { background: @selected; }
        QToolButton:checked { background: @selected; }
        QToolButton#PlayButton { border-radius: 16px; }
        QToolButton#Swatch { border-radius: 6px; }
        QToolButton::menu-indicator { image: none; width: 0; }

        QPushButton { background: @raised; border: 1px solid @border; border-radius: 6px; padding: 5px 14px; color: @text; }
        QPushButton:hover { background: @hover; }
        QPushButton:pressed { background: @selected; }
        QPushButton:default, QPushButton#Primary { background: @text; color: @window; border-color: @text; font-weight: 600; }
        QPushButton:default:hover, QPushButton#Primary:hover { background: @strong; }
        QPushButton:disabled { color: @faint; }

        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
            background: @raised; border: 1px solid @border; border-radius: 6px; padding: 4px 7px; color: @text;
            selection-background-color: @selected;
        }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: @dim; }
        QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 0; border: 0; }
        QComboBox::drop-down { border: 0; width: 18px; }
        QComboBox QAbstractItemView { background: @panel; border: 1px solid @border; selection-background-color: @hover; padding: 4px; }
        QCheckBox { color: @text; spacing: 8px; }
        QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid @faint; border-radius: 4px; background: @raised; }
        QCheckBox::indicator:checked { background: @text; border-color: @text; }

        QTreeWidget, QListWidget { background: @panel; border: 0; color: @text; padding: 4px 2px; }
        QTreeWidget::item, QListWidget::item { height: 24px; border-radius: 5px; padding-left: 2px; }
        QTreeWidget::item:hover, QListWidget::item:hover { background: @raised; }
        QTreeWidget::item:selected, QListWidget::item:selected { background: @selected; color: @strong; }
        QHeaderView::section { background: @panel; color: @faint; border: 0; border-bottom: 1px solid @divider; padding: 4px 8px; font-size: 11px; }

        QTabWidget::pane { border: 0; border-top: 1px solid @divider; }
        QTabBar::tab { background: transparent; color: @dim; padding: 6px 12px; border: 0; margin-left: 4px; }
        QTabBar::tab:selected { color: @strong; border-bottom: 1px solid @text; }
        QTabBar::tab:hover { color: @text; }

        QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
        QScrollBar::handle:vertical { background: @border; border-radius: 3px; min-height: 30px; }
        QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
        QScrollBar::handle:horizontal { background: @border; border-radius: 3px; min-width: 30px; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

        QSlider::groove:horizontal { height: 3px; background: @border; border-radius: 1px; }
        QSlider::sub-page:horizontal { background: @dim; border-radius: 1px; }
        QSlider::handle:horizontal { width: 12px; height: 12px; margin: -5px 0; border-radius: 6px; background: @text; }

        QProgressBar { background: @raised; border: 0; border-radius: 3px; height: 6px; text-align: center; color: transparent; }
        QProgressBar::chunk { background: @text; border-radius: 3px; }

        QLabel#Readout { color: @text; font-family: "Cascadia Mono", Consolas, monospace; font-size: 12px; }
        QLabel#Muted { color: @faint; font-size: 11px; }
        QLabel#FormKey { color: @faint; }
        QStatusBar { background: @window; color: @faint; }

        QRadioButton { color: @text; spacing: 8px; }
        QRadioButton::indicator { width: 14px; height: 14px; border: 1px solid @faint; border-radius: 8px; background: @raised; }
        QRadioButton::indicator:checked { background: @text; border: 3px solid @raised; }
        QRadioButton:disabled { color: @faint; }

        /* first-run setup */
        #WizardSide { background: @panel; border-right: 1px solid @divider; }
        QLabel#WizardBrand { color: @strong; font-size: 15px; font-weight: 600; }
        QLabel#WizardStep { color: @faint; padding: 7px 10px; border-radius: 6px; }
        QLabel#WizardStep[done="true"] { color: @dim; }
        QLabel#WizardStep[current="true"] { color: @strong; background: @selected; }
        QLabel#WizardTitle { color: @strong; font-size: 20px; font-weight: 600; }
        QLabel#WizardSubtitle { color: @dim; font-size: 12px; }
        QRadioButton#BigChoice { font-size: 14px; padding: 4px 0; }

        /* compare bar */
        #CompareBar { background: @window; border-bottom: 1px solid @divider; }
        QLabel#BadgeA { background: #3d8bfd; color: #ffffff; border-radius: 5px; font-weight: 700; }
        QLabel#BadgeB { background: @accentSolid; color: #ffffff; border-radius: 5px; font-weight: 700; }
        QLabel#CompareCaption { color: @faint; padding: 0 2px 0 4px; }
        #CompareDivider { background: @border; }
        QToolButton#Segment { background: @raised; border: 1px solid @border; border-radius: 6px; padding: 3px 9px; }
        QToolButton#Segment:hover { background: @hover; }
        QToolButton#Segment:checked { background: @text; color: @window; border-color: @text; }
        QToolButton#CompareButton { padding-right: 12px; }
        QToolButton#CompareButton::menu-button { width: 12px; border: 0; }

        /* export */
        QLabel#SectionTitle { color: @dim; font-size: 10px; font-weight: 600; letter-spacing: 1px; }
        #Section { background: @panel; border: 1px solid @divider; border-radius: 8px; }
        #ExportSummary { background: @panel; border: 1px solid @divider; border-radius: 8px; }
        QLabel#ExportPreview { background: @viewer; border-radius: 6px; color: @faint; }
        QLabel#Estimate { color: @strong; font-size: 13px; font-weight: 600; }
        QListWidget#PresetList { background: @panel; border: 1px solid @divider; border-radius: 8px; padding: 4px; }
        QListWidget#PresetList::item { height: 34px; padding-left: 8px; }
        QFrame#Divider { background: @divider; border: 0; }
        QScrollArea { background: transparent; }
        QScrollArea > QWidget > QWidget { background: transparent; }
    )";
#ifdef Q_OS_ANDROID
    // Fingers and pens: taller rows, bigger hit targets, wider scroll bars.
    css += R"(
        * { font-family: "Roboto", "Noto Sans CJK KR", "Noto Sans KR"; font-size: 14px; }
        QMenu::item { padding: 10px 30px 10px 14px; }
        QMenuBar::item { padding: 8px 14px; }
        QPushButton { padding: 9px 18px; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { padding: 7px 9px; }
        QTreeWidget::item, QListWidget::item { height: 40px; }
        QCheckBox::indicator, QRadioButton::indicator { width: 20px; height: 20px; }
        QScrollBar:vertical { width: 16px; }
        QScrollBar:horizontal { height: 16px; }
        QToolButton { padding: 6px; }
    )";
#endif
    css.replace("@window", hex(Pal::window))
        .replace("@panel", hex(Pal::panel))
        .replace("@raised", hex(Pal::raised))
        .replace("@hover", hex(Pal::hover))
        .replace("@selected", hex(Pal::selected))
        .replace("@border", hex(Pal::border))
        .replace("@divider", hex(Pal::divider))
        .replace("@viewer", hex(Pal::viewer))
        .replace("@strong", hex(Pal::textStrong))
        .replace("@text", hex(Pal::text))
        .replace("@dim", hex(Pal::textDim))
        .replace("@faint", hex(Pal::textFaint))
        .replace("@accentSolid", hex(Pal::accent))
        .replace("@accent", rgba(Pal::accent));
    app.setStyleSheet(css);
}
