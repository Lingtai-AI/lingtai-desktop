#include "desktop_status_item.h"

#include <QtCore/QRect>
#include <QtCore/QRectF>
#include <QtCore/QSize>
#include <QtGui/QFont>
#include <QtGui/QFontMetrics>
#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtWidgets/QMenu>
#include <QtWidgets/QSystemTrayIcon>

#include <cmath>
#include <utility>

namespace lingtai::desktop {
namespace {

constexpr auto kStatusItemHeight = 18;
// The unread logo is drawn at its own native square footprint, unscaled and
// unmoved from the zero-state resource; only the overlapping badge grows
// the canvas beyond it.
constexpr auto kLogoSize = kStatusItemHeight;
// The badge's left edge lands this fraction across the logo's own width
// (measured from the logo's own top-left origin), so the badge always
// overlaps the logo's lower-right quadrant by construction -- Telegram-
// style, paper-plane-plus-badge -- instead of floating beside it with a
// gap, for any font metrics the real system produces. Chosen (not 0.5) so
// that, combined with the bottom-anchored badge below, real-resource ink
// coverage of the logo stays at or under the perceptual-primacy target
// (<=40%, hard <50%) at both 1x and 2x: measured against the actual
// StatusItemTemplate resources, 0.5 obscures ~64% of the logo's ink, 0.71
// obscures ~34-40%.
constexpr auto kBadgeLogoHorizontalOverlap = 0.71;
// DemiBold pixel size for the badge label at 1x.
constexpr auto kBadgeFontPixelSize = 12;
// Explicit padding around the real font-metric text box, at 1x.
constexpr auto kBadgeHorizontalPadding = 4;
constexpr auto kBadgeVerticalPadding = 2;
// Explicit clearance from the canvas edges, at 1x, so the badge still reads
// as inset rather than touching the corner; kept minimal so the combined
// logo+badge silhouette fills almost the whole canvas.
constexpr auto kCanvasRightMargin = 2;
// Also doubles as the badge's bottom anchor clearance: the badge sits
// `kCanvasBottomMargin * scale` px above the fixed kLogoSize*scale floor,
// so the mask height never grows past the logo's own height (see
// nonzero_canvas_size) and the platform tray plugin's status-item icon
// cap -- int(dpr * (NSStatusBar.thickness - 4)) -- selects it unscaled
// instead of smooth-downscaling a taller mask.
constexpr auto kCanvasBottomMargin = 1;
// The widest label any bucket can render; sizing the badge for this keeps
// its rectangle identical across every nonzero bucket.
[[nodiscard]] QString widest_badge_text() {
    return QStringLiteral("99+");
}

// Shared by both the sizing math (`unread_badge_rect`) and the paint call
// (`render_mask`), so the rectangle is always sized from the exact font it
// is drawn with, not a coincidentally-matching duplicate.
[[nodiscard]] QFont badge_font(int scale) {
    auto font = QFont();
    font.setPixelSize(kBadgeFontPixelSize * scale);
    font.setWeight(QFont::DemiBold);
    return font;
}

// The nonzero-count canvas size render_mask allocates: exactly the logo's
// own square footprint tall (never taller -- the platform tray plugin
// caps a status-item pixmap at int(dpr * (NSStatusBar.thickness - 4))
// device px, which is exactly kLogoSize * scale at a 22pt-thick bar and
// kLogoSize * scale + 2 * scale at a 24pt-thick bar, i.e. at or above
// kLogoSize * scale on every observed thickness, so pinning the mask to
// kLogoSize * scale keeps it safely at or under the cap everywhere;
// growing past it forces the plugin to smooth-downscale the whole icon)
// plus the overlapping badge's horizontal
// protrusion (from DesktopStatusItem::unread_badge_rect, the public seam)
// and a minimal outer right margin, so the combined logo+badge silhouette
// fills almost the whole canvas by construction. Identical across every
// nonzero bucket because the badge is always sized for the widest label.
[[nodiscard]] QSize nonzero_canvas_size(int scale) {
    const auto badge = DesktopStatusItem::unread_badge_rect(scale);
    const auto width = badge.x() + badge.width() + kCanvasRightMargin * scale;
    return QSize(width, kLogoSize * scale);
}

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

QRect DesktopStatusItem::unread_badge_rect(int scale) {
    if (scale != 1 && scale != 2) {
        return {};
    }
    const auto metrics = QFontMetrics(badge_font(scale));
    const auto text_width = metrics.horizontalAdvance(widest_badge_text());
    const auto text_height =
        metrics.tightBoundingRect(widest_badge_text()).height();
    const auto width = text_width + 2 * kBadgeHorizontalPadding * scale;
    const auto height = text_height + 2 * kBadgeVerticalPadding * scale;
    const auto logo_size = kLogoSize * scale;
    const auto x = static_cast<int>(
        std::lround(logo_size * kBadgeLogoHorizontalOverlap));
    // Bottom-anchored against the fixed logo_size floor (never against the
    // badge's own height growing the canvas -- see nonzero_canvas_size),
    // with exactly one kCanvasBottomMargin-px clear row beneath it, so
    // "99+" gets the most vertical room real font metrics allow inside the
    // mask's own fixed 18/36 height (see nonzero_canvas_size; always at or
    // under the platform tray-icon cap) without clipping.
    const auto y = logo_size - height - kCanvasBottomMargin * scale;
    return QRect(x, y, width, height);
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

    auto result = QImage(nonzero_canvas_size(scale), QImage::Format_Alpha8);
    result.fill(0);
    auto painter = QPainter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.drawImage(QPoint(0, 0), alpha_channel(logo));

    const auto capsule = QRectF(unread_badge_rect(scale));
    const auto template_brush = painter.pen().brush();
    painter.setPen(Qt::NoPen);
    painter.setBrush(template_brush);
    painter.drawRoundedRect(
        capsule, capsule.height() / 2.0, capsule.height() / 2.0);

    painter.setFont(badge_font(scale));
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
