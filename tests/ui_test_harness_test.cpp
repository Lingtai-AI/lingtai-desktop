#include "native_shell.h"

#include "ui/UiTestHarness.h"
#include "ui/UiTestUtils.h"
#include "ui/object_names.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main(int argc, char **argv) {
    lingtai::desktop::ui_test::applyMacTestPlatformDefaults();
    QApplication app(argc, argv);

    const auto fixtures_root = std::filesystem::path(
        LINGTAI_UI_TEST_FIXTURES_DIR);
    const auto project = lingtai::desktop::ui_test::fixtureProjectRoot(
        fixtures_root, "empty");

    lingtai::desktop::ConversationUnreadSession unread_session;
    lingtai::desktop::NativeShell shell(
        unread_session, lingtai::desktop::ui_test::defaultUiTestOptions());
    lingtai::desktop::ui_test::showShellAt(
        shell, lingtai::desktop::ui_test::kNormalViewport);

    const auto outcome = lingtai::desktop::ui_test::openFixtureProject(
        shell,
        project,
        std::filesystem::path(".lingtai/agent"));
    require(
        outcome.disposition
            == lingtai::desktop::ProjectOpenDisposition::opened,
        "empty fixture must open");

    auto &window = static_cast<QWidget &>(shell.window());
    auto *agent_detail = lingtai::desktop::ui_test::findNamed<QWidget>(
        window,
        lingtai::desktop::ui_test::kAgentDetail);
    require(agent_detail->isVisible(), "agent detail must be visible");

    auto *presets_nav = lingtai::desktop::ui_test::findNamed<QWidget>(
        window,
        lingtai::desktop::ui_test::kPageNavPresets);
    require(
        !presets_nav->isVisible(),
        "presets nav must stay hidden on conversation page");

    std::cout << "LINGTAI_UI_TEST_HARNESS_OK\n";
    return 0;
}
