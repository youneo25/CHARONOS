#pragma once
#include <QDockWidget>

class QPlainTextEdit;
class ConsoleDock : public QDockWidget {
    Q_OBJECT
public:
    explicit ConsoleDock(QWidget* parent = nullptr);
    ~ConsoleDock() override;
    void append(const QString& text);
signals:
    void commandEntered(QString command);
private:
    QPlainTextEdit* m_log;
};
