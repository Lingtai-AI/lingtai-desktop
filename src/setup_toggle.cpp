#include "setup_toggle.h"

#include "setup_style.h"

#include <QtCore/QEasingCurve>
#include <QtCore/QSignalBlocker>
#include <QtGui/QColor>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>

namespace lingtai::desktop {
namespace {

constexpr auto kWidth = 36;
constexpr auto kHeight = 20;

} // namespace

SetupToggle::SetupToggle(QWidget *parent)
: QCheckBox(parent)
, animation_(this) {
    setText(QString());
    setFixedSize(kWidth, kHeight);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    setStyleSheet(QStringLiteral(
        "QCheckBox { background: transparent; spacing: 0; border: none; }"
        "QCheckBox::indicator { width: 0; height: 0; }"));
    animation_.setDuration(160);
    animation_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&animation_, &QVariantAnimation::valueChanged,
        this, [this](const QVariant &value) {
            progress_ = value.toReal();
            update();
        });
    connect(this, &QCheckBox::toggled, this, [this](bool on) {
        if (isVisible()) {
            animate_to(on);
        } else {
            snap_to(on);
        }
    });
}

void SetupToggle::set_checked(bool checked, bool animate) {
    if (!animate) {
        const QSignalBlocker block(this);
        QCheckBox::setChecked(checked);
        snap_to(checked);
        return;
    }
    QCheckBox::setChecked(checked);
}

void SetupToggle::snap_to(bool checked) {
    animation_.stop();
    progress_ = checked ? 1.0 : 0.0;
    update();
}

void SetupToggle::animate_to(bool checked) {
    const auto target = checked ? 1.0 : 0.0;
    if (qFuzzyCompare(progress_, target)) return;
    animation_.stop();
    animation_.setStartValue(progress_);
    animation_.setEndValue(target);
    animation_.start();
}

void SetupToggle::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto track = rect().adjusted(0, 0, -1, -1);
    painter.setPen(Qt::NoPen);
    const auto off = setup_is_dark(palette())
        ? QColor(QStringLiteral("#3A4541"))
        : QColor(QStringLiteral("#D1D5DB"));
    painter.setBrush(progress_ > 0.01 ? QColor(QStringLiteral("#16785C")) : off);
    painter.drawRoundedRect(track, kHeight / 2.0, kHeight / 2.0);

    const auto knob_diameter = kHeight - 4;
    const auto travel = track.width() - knob_diameter - 4;
    const auto knob_x = track.left() + 2 + int(travel * progress_);
    const auto knob_y = track.top() + 2;
    painter.setBrush(Qt::white);
    painter.drawEllipse(knob_x, knob_y, knob_diameter, knob_diameter);
}

bool SetupToggle::hitButton(const QPoint &pos) const {
    return rect().contains(pos);
}

} // namespace lingtai::desktop
