#pragma once

class QColor;
class QPoint;
class QWidget;

namespace lingtai::desktop {

void ApplyNativeWindowBackground(QWidget *widget, const QColor &color);
QPoint NativeTrafficLightAnchor(QWidget *widget);

} // namespace lingtai::desktop
