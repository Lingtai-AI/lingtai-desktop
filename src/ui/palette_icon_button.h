#pragma once

#include "base/basic_types.h"

#include "styles/palette.h"
#include "styles/style_widgets.h"

#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QPushButton>

namespace lingtai::desktop {

// One compact palette-owned icon-only lifecycle secondary: the same light-pill
// language as `PaletteActionButton`, but painted with only a small crescent
// glyph and never a caption, so its accessible name stays the only label a
// screen reader hears and the header keeps exactly one captioned action.
class PaletteIconButton final : public QPushButton {
public:
    explicit PaletteIconButton(QWidget *parent)
    : QPushButton(parent) {
        setFixedSize(26, 26);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto radius = qMin(width(), height()) / 2;
        const auto pill = !isEnabled()
            ? st::defaultLightButton.textBg->c
            : (isDown() || underMouse())
                ? st::defaultLightButton.textBgOver->c
                : st::defaultLightButton.textBg->c;
        painter.setPen(Qt::NoPen);
        painter.setBrush(pill);
        painter.drawRoundedRect(rect(), radius, radius);
        const auto ink = !isEnabled()
            ? st::windowSubTextFg->c
            : (isDown() || underMouse())
                ? st::defaultLightButton.textFgOver->c
                : st::defaultLightButton.textFg->c;
        // A small crescent moon: one full disc with an overlapping pill-colored
        // disc carving the crescent, so the glyph never needs an icon font.
        const auto size = 13.0;
        const auto center = QPointF(width() / 2.0, height() / 2.0);
        painter.setBrush(ink);
        painter.drawEllipse(center, size / 2.0, size / 2.0);
        painter.setBrush(pill);
        painter.drawEllipse(
            center + QPointF(size * 0.55, -size * 0.25),
            size * 0.52, size * 0.52);
    }
};

} // namespace lingtai::desktop
