#include "native_window_background.h"

#import <AppKit/AppKit.h>

#include <QtCore/QPoint>
#include <QtGui/QColor>
#include <QtWidgets/QWidget>

namespace lingtai::desktop {

void ApplyNativeWindowBackground(QWidget *widget, const QColor &color) {
    auto *view = reinterpret_cast<NSView *>(widget->winId());
    auto *window = view.window;
    if (!window) {
        return;
    }
    window.backgroundColor = [NSColor colorWithSRGBRed:color.redF()
        green:color.greenF()
        blue:color.blueF()
        alpha:color.alphaF()];
    window.opaque = YES;
}

void ApplyNativeFullSizeTitlebar(QWidget *widget) {
    auto *view = reinterpret_cast<NSView *>(widget->winId());
    auto *window = view.window;
    if (!window) {
        widget->setProperty("lingtai_native_titlebar_full_size", false);
        return;
    }
    window.styleMask = window.styleMask | NSWindowStyleMaskFullSizeContentView;
    window.titlebarAppearsTransparent = YES;
    window.titleVisibility = NSWindowTitleHidden;
    [window.contentView.superview layoutSubtreeIfNeeded];
    const auto covers_window = qAbs(qRound(
        window.frame.size.height - view.frame.size.height)) <= 1;
    const auto has_full_size_bit =
        (window.styleMask & NSWindowStyleMaskFullSizeContentView) != 0;
    widget->setProperty(
        "lingtai_native_titlebar_full_size",
        has_full_size_bit && covers_window);
}

QPoint NativeTrafficLightAnchor(QWidget *widget) {
    auto *view = reinterpret_cast<NSView *>(widget->winId());
    auto *window = view.window;
    auto *zoom = [window standardWindowButton:NSWindowZoomButton];
    if (!window || !zoom || !zoom.superview) {
        return QPoint();
    }
    const auto frame = [zoom.superview convertRect:zoom.frame toView:nil];
    return QPoint(
        qRound(NSMaxX(frame) + 8.0),
        qRound(window.frame.size.height - NSMidY(frame)));
}

} // namespace lingtai::desktop
