#pragma once

#include "base/basic_types.h"

#include "ui/rp_widget.h"

#include "styles/palette.h"

#include <QtGui/QPainter>

namespace lingtai::desktop {

// One LingTai-owned full-surface widget whose background is painted from the
// shared lib_ui palette (never a raw white Qt surface): the right chat/content
// pane fills `windowBg`, the same token the chat surface and top bar use.
class PaletteSurface final : public Ui::RpWidget {
public:
    explicit PaletteSurface(QWidget *parent, style::color fill)
    : Ui::RpWidget(parent)
    , fill_(std::move(fill)) {
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), fill_);
    }

private:
    style::color fill_;
};

} // namespace lingtai::desktop
