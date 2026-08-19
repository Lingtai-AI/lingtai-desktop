#pragma once

#include "native_shell.h"
#include "runtime_options.h"

#include "UiTestUtils.h"
#include "object_names.h"

#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/rp_window.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTextEdit>

#include <array>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lingtai::desktop::ui_test {

[[nodiscard]] inline RuntimeOptions defaultUiTestOptions() {
    return normalize_runtime_options(RuntimeOptions{
        .offscreen_mode = true,
        .ui_test_mode = true,
        .animations_enabled = false,
        .network_enabled = false,
    });
}

[[nodiscard]] inline std::filesystem::path fixtureProjectRoot(
        const std::filesystem::path &fixtures_root,
        std::string_view name) {
    return fixtures_root / name / "project";
}

inline void showShellAt(NativeShell &shell, ViewportSize viewport) {
    shell.window().resize(toQSize(viewport));
    shell.show_offscreen();
    QApplication::processEvents();
}

[[nodiscard]] inline ProjectOpenOutcome openFixtureProject(
        NativeShell &shell,
        const std::filesystem::path &project_root,
        const std::optional<std::filesystem::path> &agent_key
            = std::nullopt) {
    const auto outcome = shell.open_project(project_root, agent_key);
    QApplication::processEvents();
    return outcome;
}

template <typename Widget>
Widget *findNamed(QWidget &root, const char *object_name) {
    return requireChild<Widget>(root, object_name);
}

inline void requireCondition(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline void requireVisible(QWidget &widget, const char *what) {
    requireCondition(widget.isVisible(), what);
}

inline void requireHidden(QWidget &widget, const char *what) {
    requireCondition(widget.isHidden(), what);
}

inline void clickButton(QPushButton &button) {
    button.click();
    QCoreApplication::processEvents();
}

inline void submitSlashCommand(QWidget &window, const QString &command) {
    auto *input = dynamic_cast<Ui::InputField *>(
        requireChild<QObject>(window, kComposerInput));
    if (!input) {
        throw std::runtime_error("composer input must be an InputField");
    }
    input->setText(command);
    input->setFocus();
    QCoreApplication::processEvents();
    auto enter = QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(input->rawTextEdit().get(), &enter);
    QCoreApplication::processEvents();
}

struct NamedViewport {
    std::string_view name;
    ViewportSize size;
};

[[nodiscard]] inline constexpr auto standardViewports() {
    return std::array<NamedViewport, 3>{{
        {"compact", kCompactViewport},
        {"normal", kNormalViewport},
        {"wide", kWideViewport},
    }};
}

} // namespace lingtai::desktop::ui_test
