#include "native_window_background.h"

#import <AppKit/AppKit.h>

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

} // namespace lingtai::desktop
