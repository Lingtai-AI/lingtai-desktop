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
        qRound(widget->height() - NSMidY(frame)));
}

} // namespace lingtai::desktop
