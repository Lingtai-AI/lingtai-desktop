#include "desktop_status_item.h"

#include <QtGui/QIcon>
#include <QtWidgets/QMenu>
#include <QtWidgets/QSystemTrayIcon>

#include <utility>

namespace lingtai::desktop {

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
    QIcon icon;
    icon.addFile(
        QStringLiteral(":/lingtai/macos/StatusItemTemplate.png"),
        QSize(18, 18));
    icon.addFile(
        QStringLiteral(":/lingtai/macos/StatusItemTemplate@2x.png"),
        QSize(36, 36));
    icon.setIsMask(true);
    tray_icon_->setIcon(icon);
}

void DesktopStatusItem::show() {
    tray_icon_->show();
}

} // namespace lingtai::desktop
