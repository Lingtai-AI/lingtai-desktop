#include "base/basic_types.h"

#include "ui/slash_command_card.h"

#include <QtCore/QLatin1String>
#include <QtGui/QPainterPath>

namespace lingtai::desktop {

void paint_slash_glyph(
        QPainter &painter,
        const QRectF &box,
        const QString &name,
        const QColor &ink,
        const QColor &well [[maybe_unused]]) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    const auto c = box.center();
    if (name == QLatin1String("agents")) {
        painter.setBrush(ink);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QRectF(c.x() - 6.5, c.y() - 6.2, 7.2, 7.2));
        painter.drawEllipse(QRectF(c.x() - 0.2, c.y() - 6.2, 7.2, 7.2));
        painter.drawChord(QRectF(c.x() - 8.0, c.y() + 0.6, 10.5, 9.5), 0, 180 * 16);
        painter.drawChord(QRectF(c.x() - 2.0, c.y() + 0.6, 10.5, 9.5), 0, 180 * 16);
    } else if (name == QLatin1String("presets")
            || name == QLatin1String("setup")) {
        for (auto i = 0; i < 3; ++i) {
            const auto y = box.top() + 7.5 + i * 6.2;
            painter.drawLine(QPointF(box.left() + 6, y), QPointF(box.right() - 6, y));
            painter.setBrush(ink);
            painter.drawEllipse(QPointF(box.left() + 9.5 + i * 5.0, y), 2.1, 2.1);
            painter.setBrush(Qt::NoBrush);
        }
    } else if (name == QLatin1String("sleep")) {
        QPainterPath moon;
        moon.addEllipse(c, 6.4, 6.4);
        QPainterPath hole;
        hole.addEllipse(c + QPointF(3.2, -2.0), 5.5, 5.5);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawPath(moon.subtracted(hole));
    } else if (name == QLatin1String("cpr")) {
        QPainterPath bolt;
        bolt.moveTo(c.x() + 1.2, box.top() + 6);
        bolt.lineTo(c.x() - 3.8, c.y() + 0.4);
        bolt.lineTo(c.x() + 0.2, c.y() + 0.4);
        bolt.lineTo(c.x() - 1.4, box.bottom() - 6);
        bolt.lineTo(c.x() + 3.8, c.y() - 0.2);
        bolt.lineTo(c.x() - 0.2, c.y() - 0.2);
        bolt.closeSubpath();
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawPath(bolt);
    } else if (name == QLatin1String("clear")) {
        painter.drawRoundedRect(box.adjusted(6.5, 6.5, -6.5, -6.5), 2.5, 2.5);
        painter.drawLine(c + QPointF(-3.2, -3.2), c + QPointF(3.2, 3.2));
        painter.drawLine(c + QPointF(3.2, -3.2), c + QPointF(-3.2, 3.2));
    } else if (name == QLatin1String("refresh")) {
        QRectF arc(box.adjusted(7, 7, -7, -7));
        painter.drawArc(arc, 40 * 16, 260 * 16);
        QPainterPath head;
        const auto tip = QPointF(arc.center().x() + 5.2, arc.top() + 1.5);
        head.moveTo(tip);
        head.lineTo(tip + QPointF(-4.4, 0.6));
        head.lineTo(tip + QPointF(-0.4, 4.6));
        painter.setBrush(ink);
        painter.setPen(Qt::NoPen);
        painter.drawPath(head);
    } else if (name == QLatin1String("suspend")) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawRoundedRect(QRectF(c.x() - 4.6, c.y() - 5.2, 3.2, 10.4), 1.2, 1.2);
        painter.drawRoundedRect(QRectF(c.x() + 1.4, c.y() - 5.2, 3.2, 10.4), 1.2, 1.2);
    } else if (name == QLatin1String("help")) {
        painter.drawEllipse(box.adjusted(6, 6, -6, -6));
        auto font = painter.font();
        font.setPixelSize(11);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.drawText(box.adjusted(0, -1, 0, 0), Qt::AlignCenter, QStringLiteral("?"));
    } else if (name == QLatin1String("quit")) {
        painter.drawRoundedRect(box.adjusted(7.5, 6.5, -7.5, -6.5), 2.2, 2.2);
        painter.drawLine(QPointF(box.right() - 7.5, c.y()), QPointF(box.right() - 2.8, c.y()));
        painter.drawLine(QPointF(box.right() - 5.4, c.y() - 2.6), QPointF(box.right() - 2.8, c.y()));
        painter.drawLine(QPointF(box.right() - 5.4, c.y() + 2.6), QPointF(box.right() - 2.8, c.y()));
    } else {
        painter.drawEllipse(box.adjusted(7, 7, -7, -7));
        painter.drawText(box, Qt::AlignCenter, QStringLiteral("/"));
    }
    painter.restore();
}

} // namespace lingtai::desktop
