#pragma once

#include "native_shell.h"
#include "runtime_options.h"

#include "UiTestUtils.h"

#include "ui/widgets/rp_window.h"

#include <QtWidgets/QApplication>

#include <filesystem>
#include <optional>

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

} // namespace lingtai::desktop::ui_test
