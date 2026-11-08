#pragma once

#include <QDialog>
#include <QMap>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QVBoxLayout;

// First-run setup, shown once before the main window: language, which panels are shown at
// start, memory use, folders and update checks. Every answer is an ordinary setting that can
// be changed later (Window menu, Tools > Cache Memory, Help > Check for Updates).
class SetupWizard : public QDialog {
    Q_OBJECT
public:
    explicit SetupWizard(QWidget* parent = nullptr);

    // True until the wizard was finished or skipped once on this machine.
    static bool needed();
    // Writes the defaults the wizard would choose (used when it is skipped).
    static void applyDefaults();
    // The panel defaults of a new install: Project View and the Default Timeline panel only.
    static void writePanelDefaults(bool toolbar, bool clipBar, bool scrubBar, bool transport, bool timeline);

private:
    void rebuild();  // after a language change every label is built again
    QWidget* welcomePage();
    QWidget* workspacePage();
    QWidget* performancePage();
    QWidget* filesPage();
    QWidget* readyPage();
    void go(int page);
    void finish();
    QLabel* heading(const QString& title, const QString& subtitle, QVBoxLayout* into);

    QStackedWidget* m_pages = nullptr;
    QVector<QLabel*> m_steps;
    QWidget* m_body = nullptr;
    int m_page = 0;

    // answers (kept across a language rebuild)
    bool m_korean = false;
    QMap<QString, bool> m_panels;
    int m_cachePreset = 1;
    bool m_adaptive = true;
    bool m_trimMinimized = true;
    bool m_hardware = true;
    bool m_updates = true;
    bool m_mcp = true;
    bool m_drawingMode = false;
    QString m_exportDir;
};
