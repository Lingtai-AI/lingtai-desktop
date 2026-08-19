#include "native_shell.h"

#include "ui/UiTestHarness.h"
#include "ui/UiTestFonts.h"
#include "ui/object_names.h"
#include "visual/VisualSnapshotHarness.h"
#include "visual/VisualTestUtils.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void requireVisual(
        const lingtai::desktop::visual_test::VisualDiffResult &result) {
    if (result.passed) {
        return;
    }
    throw std::runtime_error(result.message.toStdString());
}

void verify_conversation_snapshot(
        lingtai::desktop::ui_test::ViewportSize viewport,
        std::string_view viewport_name) {
    const auto fixtures_root = std::filesystem::path(
        LINGTAI_UI_TEST_FIXTURES_DIR);
    const auto project = lingtai::desktop::ui_test::fixtureProjectRoot(
        fixtures_root, "typical");

    lingtai::desktop::NativeShell shell(
        lingtai::desktop::ui_test::defaultUiTestOptions());
    lingtai::desktop::ui_test::showShellAt(shell, viewport);

    const auto outcome = lingtai::desktop::ui_test::openFixtureProject(
        shell,
        project,
        std::filesystem::path(".lingtai/alpha"));
    if (outcome.disposition
            != lingtai::desktop::ProjectOpenDisposition::opened) {
        throw std::runtime_error("typical fixture must open for visual test");
    }

    auto &window = static_cast<QWidget &>(shell.window());
    auto *detail = lingtai::desktop::ui_test::findNamed<QWidget>(
        window, lingtai::desktop::ui_test::kAgentDetail);

    requireVisual(lingtai::desktop::visual_test::assertWidgetSnapshot(
        *detail,
        QString(LINGTAI_VISUAL_BASELINE_ROOT),
        "agent_detail_visual",
        "conversation",
        viewport_name));

    lingtai::desktop::ui_test::submitSlashCommand(
        window, QStringLiteral("/presets"));
    requireVisual(lingtai::desktop::visual_test::assertWidgetSnapshot(
        *detail,
        QString(LINGTAI_VISUAL_BASELINE_ROOT),
        "agent_detail_visual",
        "presets",
        viewport_name));

    std::cout << "agent_detail_visual@" << viewport_name << ": OK\n";
}

} // namespace

int main(int argc, char **argv) {
    qputenv("QT_LOGGING_RULES",
        "qt.qpa.fonts.warning=false;qt.qpa.keymapper.warning=false");
    lingtai::desktop::ui_test::applyMacTestPlatformDefaults();
    QApplication app(argc, argv);
    lingtai::desktop::ui_test::applyUiTestFontDefaults();

    try {
        verify_conversation_snapshot(
            lingtai::desktop::ui_test::kNormalViewport, "normal");
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
