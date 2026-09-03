#pragma once

#include <QtCore/QObject>
#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtGui/QImage>

#include <cstddef>
#include <functional>
#include <memory>

class QMenu;
class QSystemTrayIcon;

namespace lingtai::desktop {

class DesktopStatusItem final : public QObject {
public:
    using ActionCallback = std::function<void()>;

    explicit DesktopStatusItem(
        ActionCallback show_callback,
        ActionCallback quit_callback,
        QObject *parent = nullptr);

    void show();
    void set_unread_count(
        std::size_t exact_count,
        std::size_t open_projects);

    [[nodiscard]] static QString display_count_text(
        std::size_t exact_count);
    // Deterministic image seam shared with the source-level renderer contract.
    // `scale` accepts only 1 or 2; zero returns the corresponding resource.
    [[nodiscard]] static QImage render_mask(
        std::size_t exact_count,
        int scale);
    // The lower-right badge rectangle render_mask paints into, sized from
    // real font metrics for the widest label ("99+") plus explicit padding.
    // Shared with tests so the fit/anchor math is asserted, not duplicated.
    // `scale` accepts only 1 or 2; returns an empty rect otherwise.
    [[nodiscard]] static QRect unread_badge_rect(int scale);
    [[nodiscard]] std::size_t icon_rebuild_count() const noexcept;

private:
    // Declaration order makes the tray release its context-menu pointer first.
    std::unique_ptr<QMenu> menu_;
    std::unique_ptr<QSystemTrayIcon> tray_icon_;
    QString visible_bucket_;
    std::size_t exact_count_ = 0;
    std::size_t open_projects_ = 0;
    std::size_t icon_rebuild_count_ = 0;
    bool has_presentation_ = false;
};

} // namespace lingtai::desktop
