#pragma once

#include "base/basic_types.h"

#include "styles/palette.h"

#include <QtGui/QColor>
#include <QtGui/QPainter>
#include <QtWidgets/QWidget>

namespace lingtai::desktop {

// The selected-Agent header reuses the Sidebar's initial-circle avatar
// language on a neutral header surface. The owning title is also exposed as
// its accessibility description, so the glyph never becomes an opaque icon.
class SelectedAgentAvatar final : public QWidget {
public:
    explicit SelectedAgentAvatar(QWidget *parent)
    : QWidget(parent) {
        setFixedSize(38, 38);
        setAccessibleName(QStringLiteral("Selected Agent avatar"));
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void set_agent_name(QString name) {
        if (name_ == name) return;
        name_ = std::move(name);
        setAccessibleDescription(name_);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (name_.isEmpty()) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#16785C")));
        painter.drawEllipse(QRectF(rect()).adjusted(1, 1, -1, -1));
        auto font = this->font();
        font.setPixelSize(20);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(QColor(Qt::white));
        painter.drawText(
            rect(), Qt::AlignCenter, name_.left(1).toUpper());
    }

private:
    QString name_;
};

} // namespace lingtai::desktop
