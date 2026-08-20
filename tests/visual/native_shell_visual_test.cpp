#include "native_shell.h"

#include "ui/UiTestFonts.h"
#include "ui/UiTestHarness.h"
#include "ui/object_names.h"
#include "visual/VisualScenario.h"
#include "visual/VisualSnapshotHarness.h"
#include "visual/VisualTestUtils.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using lingtai::desktop::ui_test::ThemeMode;

void requireVisual(
        const lingtai::desktop::visual_test::VisualDiffResult &result) {
    if (result.passed) {
        return;
    }
    throw std::runtime_error(result.message.toStdString());
}

void captureSurface(
        std::string_view surface,
        ThemeMode theme) {
    const auto fixtures_root = std::filesystem::path(
        LINGTAI_UI_TEST_FIXTURES_DIR);
    const auto viewport = lingtai::desktop::ui_test::kNormalViewport;
    const auto viewport_name = std::string_view("normal");
    const auto snapshot_id =
        lingtai::desktop::visual_test::surfaceSnapshotId(surface);
    const auto theme_label = theme == ThemeMode::dark ? "dark" : "light";

    lingtai::desktop::visual_test::ThemeScope theme_scope(theme);
    lingtai::desktop::NativeShell shell(
        lingtai::desktop::ui_test::defaultUiTestOptions());
    lingtai::desktop::ui_test::showShellAt(shell, viewport);
    auto &window = static_cast<QWidget &>(shell.window());

    QWidget *target = nullptr;
    if (surface == "startup-idle") {
        target = &lingtai::desktop::visual_test::snapshotTargetForContent(window);
    } else if (surface == "setup-preset") {
        const auto sandbox =
            lingtai::desktop::visual_test::makeSetupSandbox();
        lingtai::desktop::visual_test::startSetupWizard(shell, sandbox);
        target = &lingtai::desktop::visual_test::snapshotTargetForContent(window);
    } else if (surface == "setup-agents") {
        const auto sandbox =
            lingtai::desktop::visual_test::makeSetupSandbox();
        lingtai::desktop::visual_test::startSetupWizard(shell, sandbox);
        lingtai::desktop::visual_test::advanceSetupToAgentsPage(shell);
        target = &lingtai::desktop::visual_test::snapshotTargetForContent(window);
    } else if (surface == "setup-review") {
        const auto sandbox =
            lingtai::desktop::visual_test::makeSetupSandbox();
        lingtai::desktop::visual_test::startSetupWizard(shell, sandbox);
        lingtai::desktop::visual_test::advanceSetupToReviewPage(shell, sandbox);
        target = &lingtai::desktop::visual_test::snapshotTargetForContent(window);
    } else if (surface == "conversation") {
        const auto project = lingtai::desktop::ui_test::fixtureProjectRoot(
            fixtures_root, "typical");
        const auto outcome = lingtai::desktop::ui_test::openFixtureProject(
            shell, project, std::filesystem::path(".lingtai/alpha"));
        if (outcome.disposition
                != lingtai::desktop::ProjectOpenDisposition::opened) {
            throw std::runtime_error("typical fixture must open");
        }
        target = &lingtai::desktop::visual_test::snapshotTargetForAgentDetail(
            window);
    } else if (surface == "presets") {
        const auto project = lingtai::desktop::ui_test::fixtureProjectRoot(
            fixtures_root, "typical");
        const auto outcome = lingtai::desktop::ui_test::openFixtureProject(
            shell, project, std::filesystem::path(".lingtai/alpha"));
        if (outcome.disposition
                != lingtai::desktop::ProjectOpenDisposition::opened) {
            throw std::runtime_error("typical fixture must open");
        }
        lingtai::desktop::ui_test::submitSlashCommand(
            window, QStringLiteral("/presets"));
        target = &lingtai::desktop::visual_test::snapshotTargetForAgentDetail(
            window);
    } else if (surface == "kanban") {
        const auto sandbox = std::filesystem::temp_directory_path()
            / ("lingtai-visual-kanban-"
                + std::to_string(static_cast<unsigned long long>(
                    QCoreApplication::applicationPid())));
        const auto project = sandbox / "project";
        std::filesystem::create_directories(project);
        lingtai::desktop::visual_test::prepareKanbanProject(project);
        const auto outcome = shell.open_project(project, std::nullopt);
        if (outcome.disposition
                != lingtai::desktop::ProjectOpenDisposition::opened) {
            throw std::runtime_error("kanban fixture must open");
        }
        lingtai::desktop::visual_test::selectFirstAgentRow(window);
        lingtai::desktop::ui_test::submitSlashCommand(
            window, QStringLiteral("/kanban"));
        target = &lingtai::desktop::visual_test::snapshotTargetForAgentDetail(
            window);
    } else if (surface == "empty-conversation") {
        const auto project = lingtai::desktop::ui_test::fixtureProjectRoot(
            fixtures_root, "empty");
        const auto outcome = lingtai::desktop::ui_test::openFixtureProject(
            shell, project, std::filesystem::path(".lingtai/agent"));
        if (outcome.disposition
                != lingtai::desktop::ProjectOpenDisposition::opened) {
            throw std::runtime_error("empty fixture must open");
        }
        target = &lingtai::desktop::visual_test::snapshotTargetForAgentDetail(
            window);
    } else {
        throw std::runtime_error("unknown visual surface");
    }

    requireVisual(lingtai::desktop::visual_test::assertWidgetSnapshot(
        *target,
        QString(LINGTAI_VISUAL_BASELINE_ROOT),
        "native_shell_visual",
        snapshot_id,
        viewport_name,
        theme));

    std::cout << "native_shell_visual@" << snapshot_id << '@' << theme_label
              << ": OK\n";
}

constexpr std::array<std::string_view, 8> kSurfaces{{
    "startup-idle",
    "setup-preset",
    "setup-agents",
    "setup-review",
    "conversation",
    "presets",
    "kanban",
    "empty-conversation",
}};

} // namespace

int main(int argc, char **argv) {
    qputenv("QT_LOGGING_RULES",
        "qt.qpa.fonts.warning=false;qt.qpa.keymapper.warning=false");
    lingtai::desktop::ui_test::applyMacTestPlatformDefaults();
    QApplication app(argc, argv);
    lingtai::desktop::ui_test::applyUiTestFontDefaults();

    try {
        for (const auto theme : {ThemeMode::light, ThemeMode::dark}) {
            for (const auto surface : kSurfaces) {
                captureSurface(surface, theme);
            }
        }
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
