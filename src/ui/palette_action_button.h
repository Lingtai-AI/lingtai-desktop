#pragma once

#include "styles/palette.h"
#include "styles/style_widgets.h"

#include <QtGui/QPainter>
#include <QtWidgets/QPushButton>

namespace lingtai::desktop {

class PaletteActionButton final : public QPushButton {
public:
    explicit PaletteActionButton(QWidget *parent, const QString &text)
    : QPushButton(text, parent) {
        setFixedHeight(26);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto radius = qMin(width(), height()) / 2;
        painter.setPen(Qt::NoPen);
        painter.setBrush(!isEnabled()
            ? st::defaultLightButton.textBg->c
            : (isDown() || underMouse())
                ? st::defaultLightButton.textBgOver->c
                : st::defaultLightButton.textBg->c);
        painter.drawRoundedRect(rect(), radius, radius);
        auto font = this->font();
        font.setPointSize(11);
        painter.setFont(font);
        painter.setPen(!isEnabled()
            ? st::windowSubTextFg->c
            : (isDown() || underMouse())
                ? st::defaultLightButton.textFgOver->c
                : st::defaultLightButton.textFg->c);
        painter.drawText(rect(), Qt::AlignCenter, text());
    }
};

} // namespace lingtai::desktop
