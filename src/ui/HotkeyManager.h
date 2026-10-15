#pragma once
#include <QDialog>
#include <QMap>
#include <QKeySequence>

class QAction;
class QKeySequenceEdit;
class HotkeyManager : public QDialog {
    Q_OBJECT
public:
    explicit HotkeyManager(const QMap<QString, QAction*>& actions, QWidget* parent = nullptr);
    static QString conflict(const QList<QKeySequence>& keys);
    static void assign(QAction* action, const QKeySequence& key);
private:
    QMap<QString, QAction*> m_actions;
    QMap<QString, QKeySequenceEdit*> m_editors;
};
