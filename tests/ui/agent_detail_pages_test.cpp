#include "native_shell.h"

#include "ui/UiTestHarness.h"
#include "ui/object_names.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <iostream>
#include <optional>

namespace {

void verify_presets_page_switch(
        lingtai::desktop::ui_test::ViewportSize viewport,
        std::string_view viewport_name) {
    const auto fixtures_root = std::filesystem::path(
        LINGTAI_UI_TEST_FIXTURES_DIR);
    const auto project = lingtai::desktop::ui_test::fixtureProjectRoot(
        fixtures_root, "typical");
    const auto alpha_key = std::filesystem::path(".lingtai/alpha");
    const auto alpha_directory_key = std::filesystem::path("alpha");

    lingtai::desktop::ConversationUnreadSession unread_session;
    lingtai::desktop::NativeShell shell(
        unread_session, lingtai::desktop::ui_test::defaultUiTestOptions());
    lingtai::desktop::ui_test::showShellAt(shell, viewport);

    const auto outcome = lingtai::desktop::ui_test::openFixtureProject(
        shell, project, alpha_key);
    lingtai::desktop::ui_test::requireCondition(
        outcome.disposition
            == lingtai::desktop::ProjectOpenDisposition::opened,
        "typical fixture must open");

    const auto &selection = shell.selection_state();
    lingtai::desktop::ui_test::requireCondition(
        selection.selected_agent_directory_key() == alpha_directory_key,
        "typical fixture must select alpha");

    auto &window = static_cast<QWidget &>(shell.window());
    auto *conversation = lingtai::desktop::ui_test::findNamed<QWidget>(
        window, lingtai::desktop::ui_test::kSelectedAgentConversation);
    auto *presets_section = lingtai::desktop::ui_test::findNamed<QWidget>(
        window, lingtai::desktop::ui_test::kPresetSummarySection);
    auto *pages_nav = lingtai::desktop::ui_test::findNamed<QWidget>(
        window, lingtai::desktop::ui_test::kAgentPagesNav);
    auto *presets_nav = lingtai::desktop::ui_test::findNamed<QPushButton>(
        window, lingtai::desktop::ui_test::kPageNavPresets);
    auto *conversation_nav = lingtai::desktop::ui_test::findNamed<QPushButton>(
        window, lingtai::desktop::ui_test::kPageNavConversation);
    auto *composer_input = lingtai::desktop::ui_test::findNamed<QWidget>(
        window, lingtai::desktop::ui_test::kComposerInput);

    lingtai::desktop::ui_test::requireVisible(
        *conversation,
        "conversation must be visible before leaving Conversation page");
    lingtai::desktop::ui_test::requireHidden(
        *pages_nav, "page nav must stay hidden on Conversation");

    lingtai::desktop::ui_test::clickButton(*presets_nav);
    lingtai::desktop::ui_test::requireVisible(
        *pages_nav, "page nav must appear on Presets");
    lingtai::desktop::ui_test::requireVisible(
        *conversation_nav, "Conversation back-tab must appear on Presets");
    lingtai::desktop::ui_test::requireHidden(
        *presets_nav, "Presets tab must stay hidden on Presets page");
    lingtai::desktop::ui_test::requireHidden(
        *conversation, "conversation must hide on Presets");
    lingtai::desktop::ui_test::requireVisible(
        *presets_section, "preset summary section must show on Presets");
    lingtai::desktop::ui_test::requireCondition(
        selection.selected_agent_directory_key() == alpha_directory_key,
        "agent selection must survive a Presets page switch");

    lingtai::desktop::ui_test::clickButton(*conversation_nav);
    lingtai::desktop::ui_test::requireVisible(
        *conversation, "conversation must return from Presets");
    lingtai::desktop::ui_test::requireHidden(
        *pages_nav, "page nav must hide after returning to Conversation");

    lingtai::desktop::ui_test::requireCondition(
        composer_input->isEnabled(),
        "composer must stay enabled for slash navigation");
    lingtai::desktop::ui_test::submitSlashCommand(
        window, QStringLiteral("/presets"));
    lingtai::desktop::ui_test::requireHidden(
        *conversation, "/presets slash must hide conversation");
    lingtai::desktop::ui_test::requireVisible(
        *presets_section, "/presets slash must show preset summary");

    std::cout << "agent_detail_pages@" << viewport_name << ": OK\n";
}

} // namespace

int main(int argc, char **argv) {
    lingtai::desktop::ui_test::applyMacTestPlatformDefaults();
    QApplication app(argc, argv);

    try {
        for (const auto &[name, size] :
                lingtai::desktop::ui_test::standardViewports()) {
            verify_presets_page_switch(size, name);
        }
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
