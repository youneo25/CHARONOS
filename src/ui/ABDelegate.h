#pragma once

#include <QStyledItemDelegate>

#include <functional>

// Draws small "A" / "B" buttons at the right end of each source row and reports clicks,
// so a clip can be made the main view (A) or the compare side (B) straight from a list.
class ABDelegate : public QStyledItemDelegate {
public:
    enum Flag { IsA = 1, IsB = 2, CanB = 4 };

    ABDelegate(int idRole, QObject* parent = nullptr);

    std::function<int(const QString& id)> state;               // Flag bits for a row id
    std::function<void(const QString& id, bool b)> clicked;   // b = the B button

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
    bool editorEvent(QEvent* e, QAbstractItemModel* model, const QStyleOptionViewItem& opt, const QModelIndex& index) override;
    bool helpEvent(QHelpEvent* e, QAbstractItemView* view, const QStyleOptionViewItem& opt, const QModelIndex& index) override;

private:
    QRect pillRect(const QRect& row, int which) const;  // 0 = A, 1 = B
    static QRect rowRect(const QStyleOptionViewItem& opt);
    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
    int m_idRole;
};
