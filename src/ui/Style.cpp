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
    )";
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
        .replace("@accent", rgba(Pal::accent));
    app.setStyleSheet(css);
}
