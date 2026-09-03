#include "desktop_status_item.h"

#include <QtCore/QRectF>
#include <QtCore/QSize>
#include <QtGui/QFont>
#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtWidgets/QMenu>
#include <QtWidgets/QSystemTrayIcon>

#include <utility>

namespace lingtai::desktop {
namespace {

constexpr auto kStatusItemHeight = 18;
constexpr auto kUnreadStatusItemWidth = 48;
constexpr auto kCapsuleLeft = 20;
constexpr auto kCapsuleTop = 2;
constexpr auto kCapsuleWidth = 27;
constexpr auto kCapsuleHeight = 14;

[[nodiscard]] QString template_resource(int scale) {
    return scale == 1
        ? QStringLiteral(":/lingtai/macos/StatusItemTemplate.png")
        : QStringLiteral(":/lingtai/macos/StatusItemTemplate@2x.png");
}

[[nodiscard]] QImage alpha_channel(const QImage &source) {
    auto result = QImage(source.size(), QImage::Format_Alpha8);
    result.fill(0);
    for (auto y = 0; y != source.height(); ++y) {
        auto *line = result.scanLine(y);
        for (auto x = 0; x != source.width(); ++x) {
            line[x] = static_cast<uchar>(qAlpha(source.pixel(x, y)));
        }
    }
    return result;
}

[[nodiscard]] QIcon status_icon(std::size_t exact_count) {
    auto icon = QIcon();
    if (exact_count == 0) {
        icon.addFile(
            template_resource(1), QSize(kStatusItemHeight, kStatusItemHeight));
        icon.addFile(
            template_resource(2), QSize(2 * kStatusItemHeight,
                2 * kStatusItemHeight));
    } else {
        auto one_x = QPixmap::fromImage(
            DesktopStatusItem::render_mask(exact_count, 1));
        auto two_x = QPixmap::fromImage(
            DesktopStatusItem::render_mask(exact_count, 2));
        two_x.setDevicePixelRatio(2.0);
        icon.addPixmap(one_x);
        icon.addPixmap(two_x);
    }
    icon.setIsMask(true);
    return icon;
}

} // namespace

DesktopStatusItem::DesktopStatusItem(
        ActionCallback show_callback,
        ActionCallback quit_callback,
        QObject *parent)
: QObject(parent)
, menu_(std::make_unique<QMenu>())
, tray_icon_(std::make_unique<QSystemTrayIcon>(this)) {
    auto *show_action = menu_->addAction(QStringLiteral("Show LingTai"));
    menu_->addSeparator();
    auto *quit_action = menu_->addAction(QStringLiteral("Quit LingTai"));
    connect(show_action, &QAction::triggered, this,
        [callback = std::move(show_callback)] {
            if (callback) {
                callback();
            }
        });
    connect(quit_action, &QAction::triggered, this,
        [callback = std::move(quit_callback)] {
            if (callback) {
                callback();
            }
        });
    tray_icon_->setContextMenu(menu_.get());
    set_unread_count(0, 0);
}

void DesktopStatusItem::show() {
    tray_icon_->show();
}

void DesktopStatusItem::set_unread_count(
        std::size_t exact_count,
        std::size_t open_projects) {
    const auto bucket = exact_count == 0
        ? QStringLiteral("0")
        : display_count_text(exact_count);
    if (bucket != visible_bucket_) {
        tray_icon_->setIcon(status_icon(exact_count));
        visible_bucket_ = bucket;
        ++icon_rebuild_count_;
    }
    if (!has_presentation_
        || exact_count != exact_count_
        || open_projects != open_projects_) {
        const auto project_word = open_projects == 1
            ? QStringLiteral("project")
            : QStringLiteral("projects");
        tray_icon_->setToolTip(
            QStringLiteral("LingTai Desktop — %1 unread across %2 open %3")
                .arg(QString::number(exact_count),
                    QString::number(open_projects), project_word));
        exact_count_ = exact_count;
        open_projects_ = open_projects;
        has_presentation_ = true;
    }
}

QString DesktopStatusItem::display_count_text(std::size_t exact_count) {
    if (exact_count == 0) {
        return {};
    }
    return exact_count >= 100
        ? QStringLiteral("99+")
        : QString::number(exact_count);
}

QImage DesktopStatusItem::render_mask(
        std::size_t exact_count,
        int scale) {
    if (scale != 1 && scale != 2) {
        return {};
    }
    const auto logo = QImage(template_resource(scale));
    if (exact_count == 0) {
        return logo;
    }
    if (logo.isNull()) {
        return {};
    }

    auto result = QImage(
        QSize(kUnreadStatusItemWidth * scale, kStatusItemHeight * scale),
        QImage::Format_Alpha8);
    result.fill(0);
    auto painter = QPainter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.drawImage(QPoint(0, 0), alpha_channel(logo));

    const auto capsule = QRectF(
        kCapsuleLeft * scale,
        kCapsuleTop * scale,
        kCapsuleWidth * scale,
        kCapsuleHeight * scale);
    const auto template_brush = painter.pen().brush();
    painter.setPen(Qt::NoPen);
    painter.setBrush(template_brush);
    painter.drawRoundedRect(
        capsule, kCapsuleHeight * scale / 2.0,
        kCapsuleHeight * scale / 2.0);

    auto font = painter.font();
    font.setPixelSize(9 * scale);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.setPen(QPen());
    painter.setBrush(Qt::NoBrush);
    painter.drawText(capsule, Qt::AlignCenter,
        display_count_text(exact_count));
    painter.end();
    return result;
}

std::size_t DesktopStatusItem::icon_rebuild_count() const noexcept {
    return icon_rebuild_count_;
}

} // namespace lingtai::desktop
