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

#include <algorithm>
#include <cmath>
#include <utility>

namespace lingtai::desktop {
namespace {

constexpr auto kStatusItemHeight = 18;
// Hard platform-safety cap, proven in repair 3: the platform tray plugin
// caps a status-item pixmap at int(dpr * (NSStatusBar.thickness - 4)) device
// px and smooth-downscales anything taller. kLogoSize * scale stays at or
// under that cap on every observed bar thickness, so the nonzero-count
// canvas is pinned to this height (never taller) below.
constexpr auto kLogoSize = kStatusItemHeight;

// Repair 4 root cause (human-rejected repair 3, email 20260903T163822-b780,
// "实在是太小了"): repair 3 drew the 18/36 template resource completely
// unscaled, so its own opaque "字" ink -- only ~13/18 (1x) / ~28/36 (2x) of
// the canvas at meaningful alpha -- read as a small mark even before the
// badge cut further into it. Repair 4 crops the resource's own meaningful-
// alpha ink and re-scales it to fill most of the fixed 18/36 height, then
// sizes/positions a taller, rounder badge relative to that enlarged (not
// nominal) logo footprint. All geometry below is derived once at 1x from
// the real cropped-ink aspect ratio and real QFontMetrics, then doubled for
// 2x, so the two scales agree by construction (see the scale-consistency
// regression in tests/native_shell_test.cpp).

// Alpha at/above this level is "meaningful" ink for measuring and cropping
// the logo: low enough to keep the glyph's real silhouette (not just its
// fully-opaque core), high enough to exclude the faint antialiased halo
// that reads as a soft shadow rather than a solid mark. Chosen from the
// resource's own alpha histogram: the "字" stroke bodies solidify (bounding
// box stops shrinking materially) right around this level at both 1x/2x.
constexpr auto kMeaningfulAlphaThreshold = 128;

// The enlarged logo ink's height at 1x: 17 of the fixed 18px canvas (94%),
// leaving exactly one clear row so the ink never touches the raw canvas
// edge. This is the practical ceiling: the source ink's own aspect ratio
// (~1.04, near-square) means pushing height to the full 18 would still only
// buy ~1 extra px of width, at the cost of edge-touching ink.
constexpr auto kLogoInkHeightBase = 17;

// Repair 4 correction (parent-rejected intermediate fb364579774e7b7d, "the
// current 1x badge font is only 7px"): sizing the badge to exactly the
// enlarged logo's own width (ratio 1.0) forced badge_font_pixel_size_base
// down to 7px to fit "99+", and at 7px the digit "1"'s single-pixel-wide
// stroke never reached a real, multi-pixel-wide clear-composition cutout --
// the measured worst destination pixel only fell to 41/255 (an inadequate
// ~84% reduction, one lone pixel, not a readable cutout). Shrinking
// typography further to chase a tighter width ratio is exactly the
// regression this correction rules out. This is the floor: the smallest
// DemiBold pixel size measured to still produce a genuinely multi-pixel,
// strongly-cleared "1" cutout (see the real-legibility regression in
// tests/native_shell_test.cpp) -- at or above repair 3's own real-font
// approach, never a nominal-box coincidence.
constexpr auto kMinLegibleFontPixelSize = 11;

// The badge's left edge lands this fraction into the *enlarged* logo's own
// width (not the nominal 18px box). Repair 4 correction: a genuinely
// legible badge (see kMinLegibleFontPixelSize above) is real-font-metrics
// wide rather than pinned to the logo's own width, so holding repair 4's
// original 0.23 offset here would push the badge's own right edge far past
// a modest protrusion of the enlarged logo. This smaller offset still reads
// as a deep, deliberate overlap into the logo's own left portion (not
// repair 3's shallow 71%-in contact-only touch), while leaving enough
// budget for the wider, legible badge's own protrusion to stay modest (see
// the deep-overlap regression in tests/native_shell_test.cpp).
constexpr double kBadgeLogoOverlapRatio = 0.10;

// How far the badge's bottom edge extends past the enlarged logo's own
// bottom edge, at 1x: a small, deliberate protrusion ("extends only
// modestly beyond") rather than repair 3's badge floating with a full clear
// margin row beneath it.
constexpr auto kBadgeBottomProtrusionBase = 1;

// Minimum clearance, at 1x, between the real font-metric text box and the
// badge's own edges on every side, so the label is never clipped and the
// padding is always nontrivial.
constexpr auto kMinBadgePaddingBase = 1;

// Explicit clearance from the canvas's own right edge, at 1x, so the badge
// still reads as inset rather than touching the corner.
constexpr auto kCanvasRightMarginBase = 2;

// The widest label any bucket can render; sizing the badge for this keeps
// its rectangle identical across every nonzero bucket.
[[nodiscard]] QString widest_badge_text() {
    return QStringLiteral("99+");
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

// The real, meaningful-alpha bounding box of `image` -- never a nominal or
// assumed footprint. Shared conceptually with the equivalent measurement
// tests/native_shell_test.cpp performs independently on the rendered
// output, so this crop's honesty is cross-checked, not merely asserted.
[[nodiscard]] QRect ink_bounds(const QImage &image, int alpha_threshold) {
    auto min_x = image.width();
    auto min_y = image.height();
    auto max_x = -1;
    auto max_y = -1;
    for (auto y = 0; y != image.height(); ++y) {
        for (auto x = 0; x != image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) < alpha_threshold) {
                continue;
            }
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        }
    }
    if (max_x < 0) {
        return {};
    }
    return QRect(QPoint(min_x, min_y), QPoint(max_x, max_y));
}

// The real template resource's own meaningful-alpha ink, always cropped
// from the sharper 2x asset (even when composing the 1x mask) so a
// downscale -- not an upscale of the softer native 18x18 PNG -- supplies
// the enlarged logo's pixels.
[[nodiscard]] QImage source_ink_2x() {
    return QImage(template_resource(2));
}

[[nodiscard]] QRect source_ink_crop() {
    return ink_bounds(source_ink_2x(), kMeaningfulAlphaThreshold);
}

// The enlarged logo ink's own width at 1x, solved from the real cropped
// ink's aspect ratio so the recomposition never distorts the glyph.
[[nodiscard]] int logo_ink_width_base(const QRect &crop) {
    return static_cast<int>(std::lround(
        kLogoInkHeightBase * crop.width() / static_cast<double>(crop.height())));
}

// Largest real DemiBold pixel size, at or above the legibility floor above,
// whose own natural "99+" metrics (horizontal advance and tight bounding
// height, each plus kMinBadgePaddingBase clearance) still produce a badge
// box smaller in area than the enlarged logo's own footprint -- keeping the
// logo spatially primary (see the footprint-primacy hierarchy gate) -- so
// the font is the largest genuinely legible size the current logo crop
// actually affords, not a hardcoded constant that could go stale if the
// resource ink changes. Falls back to the floor itself if even that would
// not fit (this project's own resource crop always accepts it in practice).
[[nodiscard]] int badge_font_pixel_size_base(int logo_area) {
    constexpr auto kMaxCandidatePixelSize = 16;
    for (auto px = kMaxCandidatePixelSize; px > kMinLegibleFontPixelSize;
            --px) {
        auto font = QFont();
        font.setPixelSize(px);
        font.setWeight(QFont::DemiBold);
        const auto metrics = QFontMetrics(font);
        const auto width =
            metrics.horizontalAdvance(widest_badge_text())
                + 2 * kMinBadgePaddingBase;
        const auto height =
            metrics.tightBoundingRect(widest_badge_text()).height()
                + 2 * kMinBadgePaddingBase;
        if (width * height < logo_area) {
            return px;
        }
    }
    return kMinLegibleFontPixelSize;
}

// The real, undistorted DemiBold metrics of the widest label at the chosen
// legible pixel size, plus kMinBadgePaddingBase clearance on every side --
// the badge's own width and height, honestly derived from the exact font it
// will be painted with rather than pinned to the logo's own nominal width.
struct BadgeBoxBase {
    int width;
    int height;
};

[[nodiscard]] BadgeBoxBase badge_box_base(int font_pixel_size) {
    auto font = QFont();
    font.setPixelSize(font_pixel_size);
    font.setWeight(QFont::DemiBold);
    const auto metrics = QFontMetrics(font);
    BadgeBoxBase box{};
    box.width = metrics.horizontalAdvance(widest_badge_text())
        + 2 * kMinBadgePaddingBase;
    box.height = metrics.tightBoundingRect(widest_badge_text()).height()
        + 2 * kMinBadgePaddingBase;
    return box;
}

struct BaseGeometry {
    int logo_width;
    int logo_height;
    int badge_x;
    int badge_y;
    int badge_width;
    int badge_height;
    int font_pixel_size;
};

// Every 1x geometry fact this file needs, computed once from the real
// resource ink and real font metrics. 2x geometry is this struct's values
// doubled exactly (see unread_logo_rect / unread_badge_rect / badge_font),
// so the two scales agree by construction rather than by two independent,
// potentially-drifting roundings.
[[nodiscard]] BaseGeometry base_geometry() {
    const auto crop = source_ink_crop();
    BaseGeometry geometry{};
    geometry.logo_height = kLogoInkHeightBase;
    geometry.logo_width = logo_ink_width_base(crop);
    const auto logo_area = geometry.logo_width * geometry.logo_height;
    geometry.font_pixel_size = badge_font_pixel_size_base(logo_area);
    const auto box = badge_box_base(geometry.font_pixel_size);
    // The badge is never narrower than the enlarged logo itself (it must
    // still read as covering the logo's own width, not a slim tag beside
    // it), but a genuinely legible font's own real metrics may need more.
    geometry.badge_width = std::max(box.width, geometry.logo_width);
    geometry.badge_height = box.height;
    geometry.badge_x = static_cast<int>(
        std::lround(kBadgeLogoOverlapRatio * geometry.logo_width));
    const auto badge_far_y = geometry.logo_height + kBadgeBottomProtrusionBase;
    geometry.badge_y = badge_far_y - geometry.badge_height;
    return geometry;
}

// Shared by both the sizing math (`unread_badge_rect`) and the paint call
// (`render_mask`), so the rectangle is always sized from the exact font it
// is drawn with, not a coincidentally-matching duplicate.
[[nodiscard]] QFont badge_font(int scale) {
    auto font = QFont();
    font.setPixelSize(base_geometry().font_pixel_size * scale);
    font.setWeight(QFont::DemiBold);
    return font;
}

// The nonzero-count canvas size render_mask allocates: exactly the fixed
// kLogoSize * scale tall (never taller -- see the platform-cap comment on
// kLogoSize) and wide enough for the badge's own horizontal protrusion
// (from DesktopStatusItem::unread_badge_rect, the public seam) plus a
// minimal outer right margin. Identical across every nonzero bucket
// because the badge is always sized for the widest label.
[[nodiscard]] QSize nonzero_canvas_size(int scale) {
    const auto badge = DesktopStatusItem::unread_badge_rect(scale);
    const auto width =
        badge.x() + badge.width() + kCanvasRightMarginBase * scale;
    return QSize(width, kLogoSize * scale);
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

QRect DesktopStatusItem::unread_logo_rect(int scale) {
    if (scale != 1 && scale != 2) {
        return {};
    }
    const auto geometry = base_geometry();
    return QRect(0, 0,
        geometry.logo_width * scale, geometry.logo_height * scale);
}

QRect DesktopStatusItem::unread_badge_rect(int scale) {
    if (scale != 1 && scale != 2) {
        return {};
    }
    const auto geometry = base_geometry();
    return QRect(geometry.badge_x * scale, geometry.badge_y * scale,
        geometry.badge_width * scale, geometry.badge_height * scale);
}

QImage DesktopStatusItem::render_mask(
        std::size_t exact_count,
        int scale) {
    if (scale != 1 && scale != 2) {
        return {};
    }
    if (exact_count == 0) {
        return QImage(template_resource(scale));
    }

    const auto source = source_ink_2x();
    if (source.isNull()) {
        return {};
    }
    const auto crop = ink_bounds(source, kMeaningfulAlphaThreshold);
    if (crop.isEmpty()) {
        return {};
    }
    const auto source_alpha = alpha_channel(source);

    auto result = QImage(nonzero_canvas_size(scale), QImage::Format_Alpha8);
    result.fill(0);
    auto painter = QPainter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // The enlarged logo: the real resource's own meaningful-alpha ink,
    // cropped and scaled (never distorted -- unread_logo_rect preserves the
    // crop's own aspect ratio) to fill most of the fixed canvas height.
    const auto logo = unread_logo_rect(scale);
    painter.drawImage(QRectF(logo), source_alpha, QRectF(crop));

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
