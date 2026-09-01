#pragma once

#include "base/basic_types.h"

#include <QtCore/QAbstractNativeEventFilter>
#include <QtCore/QObject>

#include <span>

namespace lingtai::desktop {

using NativeWindowIdentity = const void *;

// The native recipient has already been selected by AppKit. An event is an
// outside press exactly when at least one popup window exists and the recipient
// is not one of those windows; a null recipient is outside as well.
[[nodiscard]] bool ShouldDismissMacPopup(
    NativeWindowIdentity recipient,
    std::span<const NativeWindowIdentity> visible_popup_windows);

// One application-lifetime, observation-only Cocoa bridge. It never consumes
// or rewrites an event, so its ordering relative to other native filters does
// not affect their behavior.
class MacPopupDismissalBridge final
    : public QObject
    , public QAbstractNativeEventFilter {
public:
    MacPopupDismissalBridge(QObject *parent, Fn<void()> force_hide);

    bool nativeEventFilter(
        const QByteArray &event_type,
        void *message,
        qintptr *result) override;

private:
    Fn<void()> force_hide_;
};

} // namespace lingtai::desktop
