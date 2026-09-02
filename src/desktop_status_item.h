#pragma once

#include <QtCore/QObject>

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

private:
    std::unique_ptr<QMenu> menu_;
    std::unique_ptr<QSystemTrayIcon> tray_icon_;
};

} // namespace lingtai::desktop
