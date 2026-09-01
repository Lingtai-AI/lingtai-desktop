#include "mac_popup_dismissal_bridge.h"

#import <AppKit/AppKit.h>

#include <QtCore/QCoreApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <utility>
#include <vector>

namespace lingtai::desktop {

bool ShouldDismissMacPopup(
        NativeWindowIdentity recipient,
        std::span<const NativeWindowIdentity> visible_popup_windows) {
    return !visible_popup_windows.empty()
        && std::ranges::find(visible_popup_windows, recipient)
            == visible_popup_windows.end();
}

MacPopupDismissalBridge::MacPopupDismissalBridge(
        QObject *parent,
        Fn<void()> force_hide)
: QObject(parent)
, force_hide_(std::move(force_hide)) {
    Q_ASSERT(parent == QCoreApplication::instance());
    Q_ASSERT(force_hide_ != nullptr);
    qApp->installNativeEventFilter(this);
}

bool MacPopupDismissalBridge::nativeEventFilter(
        const QByteArray &event_type,
        void *message,
        qintptr *result) {
    (void)result;
    if (event_type == "mac_generic_NSEvent" && message != nullptr) {
        auto *event = static_cast<NSEvent *>(message);
        const auto type = event.type;
        if (type == NSEventTypeLeftMouseDown
                || type == NSEventTypeRightMouseDown
                || type == NSEventTypeOtherMouseDown) {
            auto popup_windows = std::vector<NativeWindowIdentity>();
            for (auto *widget : QApplication::topLevelWidgets()) {
                if (!widget->isVisible()
                        || widget->windowType() != Qt::Popup
                        || widget->internalWinId() == 0) {
                    continue;
                }
                auto *view = reinterpret_cast<NSView *>(
                    widget->internalWinId());
                auto *window = view.window;
                if (window == nil) {
                    continue;
                }
                const auto identity = static_cast<NativeWindowIdentity>(window);
                if (std::ranges::find(popup_windows, identity)
                        == popup_windows.end()) {
                    popup_windows.push_back(identity);
                }
            }
            const auto recipient = static_cast<NativeWindowIdentity>(
                event.window);
            if (ShouldDismissMacPopup(recipient, popup_windows)) {
                force_hide_();
            }
        }
    }
    return false;
}

} // namespace lingtai::desktop
