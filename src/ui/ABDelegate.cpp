#include "ABDelegate.h"
#include "Lang.h"

#include "Palette.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QToolTip>

ABDelegate::ABDelegate(int idRole, QObject* parent)
    : QStyledItemDelegate(parent)
    , m_idRole(idRole)
{
}

QRect ABDelegate::pillRect(const QRect& row, int which) const
{
    const int w = 20, h = 16, gap = 4;
    const int right = row.right() - 6 - (which == 0 ? w + gap : 0);
    return QRect(right - w + 1, row.center().y() - h / 2 + 1, w, h);
}

// List views make an item as wide as its text; the buttons belong at the visible right edge.
QRect ABDelegate::rowRect(const QStyleOptionViewItem& opt)
{
    QRect r = opt.rect;
    if (const auto* view = qobject_cast<const QAbstractItemView*>(opt.widget)) r.setRight(view->viewport()->width() - 1);
    return r;
}

QSize ABDelegate::sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& index) const
{
    // Exactly as wide as the visible list, so the whole row (and its buttons) is clickable and
    // long names are elided instead of pushing the buttons off screen.
    QSize s = QStyledItemDelegate::sizeHint(opt, index);
    if (const auto* view = qobject_cast<const QAbstractItemView*>(opt.widget)) s.setWidth(std::max(40, view->viewport()->width() - 2));
    return s;
}

void ABDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const
{
    const QString id = index.data(m_idRole).toString();
    if (id.isEmpty() || !state) {
        QStyledItemDelegate::paint(p, opt, index);
        return;
    }
    const int s = state(id);

    // Shorten the name so it ends before the buttons. Elide in the middle: clips usually share
    // a prefix and differ in their numbering and extension, which stay visible this way.
    QStyleOptionViewItem o = opt;
    initStyleOption(&o, index);
    o.rect = rowRect(opt);
    const int buttons = (s & CanB) ? 2 * 20 + 4 + 12 : 20 + 12;
    const int iconW = o.icon.isNull() ? 0 : o.decorationSize.width() + 8;
    const int avail = std::max(20, o.rect.width() - buttons - iconW - 8);
    o.text = o.fontMetrics.elidedText(o.text, Qt::ElideMiddle, avail);
    const QWidget* w = o.widget;
    (w ? w->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &o, p, w);

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    QFont f = opt.font;
    f.setPixelSize(10);
    f.setBold(true);
    p->setFont(f);
    for (int which = 0; which < 2; ++which) {
        if (which == 1 && !(s & CanB)) continue;
        const bool on = s & (which == 0 ? IsA : IsB);
        const QRectF r = QRectF(pillRect(o.rect, which)).adjusted(0.5, 0.5, -0.5, -0.5);
        // Solid background so a long name running underneath doesn't show through.
        p->setPen(QPen(on ? Pal::text : Pal::border, 1));
        p->setBrush(on ? Pal::text : (opt.state & QStyle::State_Selected ? Pal::selected : Pal::panel));
        p->drawRoundedRect(r, 4, 4);
        p->setPen(on ? Pal::window : Pal::textFaint);
        p->drawText(r, Qt::AlignCenter, which == 0 ? QStringLiteral("A") : QStringLiteral("B"));
    }
    p->restore();
}

bool ABDelegate::editorEvent(QEvent* e, QAbstractItemModel* model, const QStyleOptionViewItem& opt, const QModelIndex& index)
{
    const QString id = index.data(m_idRole).toString();
    if (!id.isEmpty() && state && (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonRelease ||
                                   e->type() == QEvent::MouseButtonDblClick)) {
        const auto* me = static_cast<QMouseEvent*>(e);
        const int s = state(id);
        for (int which = 0; which < 2; ++which) {
            if (which == 1 && !(s & CanB)) continue;
            if (!pillRect(rowRect(opt), which).contains(me->position().toPoint())) continue;
            if (e->type() == QEvent::MouseButtonRelease && me->button() == Qt::LeftButton && clicked) {
                // Deferred: the handler rebuilds the list this delegate is painting.
                auto cb = clicked;
                QTimer::singleShot(0, this, [cb, id, which] { cb(id, which == 1); });
            }
            return true;  // swallow, so the click doesn't select / start a drag
        }
    }
    return QStyledItemDelegate::editorEvent(e, model, opt, index);
}

bool ABDelegate::helpEvent(QHelpEvent* e, QAbstractItemView* view, const QStyleOptionViewItem& opt, const QModelIndex& index)
{
    const QString id = index.data(m_idRole).toString();
    if (!id.isEmpty() && state) {
        if (pillRect(rowRect(opt), 0).contains(e->pos())) {
            QToolTip::showText(e->globalPos(), L("View as A", "A로 보기"), view);
            return true;
        }
        if ((state(id) & CanB) && pillRect(rowRect(opt), 1).contains(e->pos())) {
            QToolTip::showText(e->globalPos(), L("Compare as B", "B로 비교"), view);
            return true;
        }
        // Full name when it had to be shortened (the tree also carries the file path as its tooltip).
        const QString tip = index.data(Qt::ToolTipRole).toString();
        QToolTip::showText(e->globalPos(), tip.isEmpty() ? index.data(Qt::DisplayRole).toString() : tip, view);
        return true;
    }
    return QStyledItemDelegate::helpEvent(e, view, opt, index);
}
