#include "HotkeyManager.h"
#include "Lang.h"
#include <QAction>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

void HotkeyManager::assign(QAction* action, const QKeySequence& key) {
    if (key == QKeySequence(Qt::SHIFT | Qt::Key_Tab))
        action->setShortcuts({key, QKeySequence(Qt::SHIFT | Qt::Key_Backtab)});
    else action->setShortcut(key);
}
QString HotkeyManager::conflict(const QList<QKeySequence>& keys) {
    for (int i = 0; i < keys.size(); ++i) {
        if (keys[i].isEmpty()) continue;
        for (int j = i + 1; j < keys.size(); ++j) {
            if (keys[j].isEmpty()) continue;
            if (keys[i].matches(keys[j]) != QKeySequence::NoMatch || keys[j].matches(keys[i]) != QKeySequence::NoMatch)
                return keys[i].toString(QKeySequence::NativeText);
        }
    }
    return {};
}
HotkeyManager::HotkeyManager(const QMap<QString, QAction*>& actions, QWidget* parent)
    : QDialog(parent), m_actions(actions) {
    setWindowTitle(L("HotKey Manager", "단축키 관리자"));
    resize(650, 620);
    auto* layout = new QVBoxLayout(this);
    auto* search = new QLineEdit;
    search->setPlaceholderText(L("Search commands...", "명령 검색..."));
    layout->addWidget(search);
    layout->addWidget(new QLabel(L("Click a shortcut and press the new keys. Backspace clears it. Changes apply on Save.", "단축키를 클릭하고 새 키를 누르세요. Backspace로 비웁니다. 저장 시 적용됩니다.")));
    auto* table = new QTableWidget(actions.size(), 2);
    table->setHorizontalHeaderLabels({L("Command", "명령"), L("Shortcut", "단축키")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->verticalHeader()->hide();
    int row = 0;
    for (auto it = actions.cbegin(); it != actions.cend(); ++it, ++row) {
        auto* label = new QTableWidgetItem(it.value()->text().isEmpty() ? it.value()->property("commandLabel").toString() : it.value()->text());
        label->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        table->setItem(row, 0, label);
        auto* editor = new QKeySequenceEdit(it.value()->shortcut());
        editor->setMaximumSequenceLength(1);
        editor->setClearButtonEnabled(true);
        editor->setObjectName(it.key());
        table->setCellWidget(row, 1, editor);
        m_editors[it.key()] = editor;
    }
    connect(search, &QLineEdit::textChanged, this, [table](const QString& text) {
        for (int r = 0; r < table->rowCount(); ++r) table->setRowHidden(r, !table->item(r, 0)->text().contains(text, Qt::CaseInsensitive));
    });
    layout->addWidget(table);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this] {
        for (auto it = m_editors.begin(); it != m_editors.end(); ++it)
            it.value()->setKeySequence(QKeySequence(m_actions[it.key()]->property("defaultShortcut").toString(), QKeySequence::PortableText));
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        QList<QKeySequence> keys;
        for (auto* editor : m_editors) keys << editor->keySequence();
        const QString duplicate = conflict(keys);
        if (!duplicate.isEmpty()) {
            QMessageBox::warning(this, L("Shortcut Conflict", "단축키 충돌"), L("Duplicate or overlapping shortcut: %1", "중복되거나 겹치는 단축키: %1").arg(duplicate));
            return;
        }
        QSettings settings;
        for (auto it = m_editors.begin(); it != m_editors.end(); ++it) {
            const auto key = it.value()->keySequence();
            assign(m_actions[it.key()], key);
            settings.setValue("hotkeys/" + it.key(), key.toString(QKeySequence::PortableText));
        }
        accept();
    });
    layout->addWidget(buttons);
}
