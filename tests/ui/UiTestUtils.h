#pragma once

#include <QtCore/QSize>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QWidget>

#include <stdexcept>
#include <string>

namespace lingtai::desktop::ui_test {

struct ViewportSize {
    int width = 0;
    int height = 0;
};

inline constexpr ViewportSize kCompactViewport{1000, 700};
inline constexpr ViewportSize kNormalViewport{1280, 800};
inline constexpr ViewportSize kWideViewport{1600, 1000};

enum class ThemeMode {
    light,
    dark,
};

inline QSize toQSize(ViewportSize size) {
    return {size.width, size.height};
}

template <typename Widget>
Widget *requireChild(QWidget &root, const char *object_name) {
    auto *result = root.findChild<Widget *>(object_name);
    if (!result) {
        throw std::runtime_error(
            std::string("missing widget: ") + object_name);
    }
    return result;
}

// Agent roster rows are painted on a canvas, not separate QPushButtons.
// Tests should prefer AgentRoster::focus_row() plus keyboard activation,
// or the existing canvas click helpers, until row widgets exist.
inline void applyMacTestPlatformDefaults() {
#if defined(Q_OS_MAC)
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "cocoa");
    }
#endif
}

} // namespace lingtai::desktop::ui_test
