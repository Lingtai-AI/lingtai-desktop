#pragma once

#include "base/basic_types.h"

#include "styles/palette.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QWidget>

#include <functional>

namespace lingtai::desktop {

// One LingTai-owned semantic drag handle for the roster column: a fixed 8px
// strip between the roster and its one-pixel shadow that reports only the
// pointer's current global x while the primary button is held, so the shell
// can re-derive the runtime-only roster width ratio. It paints nothing and is
// deliberately distinct from the passive `Ui::PlainShadow` that follows it.
class RosterResizeHandle final : public QWidget {
public:
    using GlobalXCallback = std::function<void(int global_x)>;

    RosterResizeHandle(QWidget *parent, int width, GlobalXCallback callback)
    : QWidget(parent)
    , callback_(std::move(callback)) {
        setFixedWidth(width);
        setCursor(Qt::SplitHCursor);
        auto policy = sizePolicy();
        policy.setVerticalPolicy(QSizePolicy::Expanding);
        setSizePolicy(policy);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), st::windowBg->c);
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            dragging_ = true;
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (dragging_) {
            callback_(event->globalPosition().toPoint().x());
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            dragging_ = false;
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    GlobalXCallback callback_;
    bool dragging_ = false;
};

} // namespace lingtai::desktop
