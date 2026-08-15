#include "native_shell.h"

#include "ui/widgets/rp_window.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::ProjectOpenDisposition;

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Widget>
Widget *required_child(QWidget &root, const char *object_name) {
    auto *result = root.findChild<Widget *>(object_name);
    require(result != nullptr, std::string("missing child: ") + object_name);
    return result;
}

void write_file(const fs::path &path, std::string_view bytes) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    require(!error, "fixture parent must be created");
    auto stream = std::ofstream(path, std::ios::binary);
    stream << bytes;
    require(stream.good(), "fixture file must be written: " + path.string());
}

QPushButton *agent_row(QWidget &window, std::string_view key) {
    const auto expected = QString::fromUtf8(key.data(), key.size());
    for (auto *row : window.findChildren<QPushButton *>()) {
        if (row->property("directory_key").toString() == expected) return row;
    }
    throw std::runtime_error("missing Agent row: " + std::string(key));
}

void click_agent(QWidget &window, std::string_view key) {
    agent_row(window, key)->click();
    QCoreApplication::processEvents();
}

// The Repair4 dedicated Presets presentation contract: a real NativeShell
// with one selected Agent, opening the Presets page, showing exactly the
// minimal Provider/Model/Default/Allowed surface with ordered refs and the
// Resolved state, proving the removed active-ref/badge/context/capability/
// generated/source text is absent, and proving the six generic
// identity/status/facts widgets stay hidden on the Presets page.
void verify_presets_simplified_surface(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *surface = required_child<QPlainTextEdit>(
        window, "lingtai_selected_agent_preset_summary");
    auto *state = required_child<QLabel>(
        window, "lingtai_selected_agent_preset_summary_state");

    const auto project = sandbox / "project";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    const auto target_a = project / ".lingtai/agent-a";
    write_file(target_a / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-d0c8",)"
        R"("agent_name":"agent-a","nickname":"Agent A",)"
        R"("address":"agent-a","state":"active"})");
    write_file(target_a / "init.json", "{}");
    write_file(target_a / "system" / "manifest.resolved.json",
        R"JSON({
      "schema": "lingtai.manifest.resolved/v1",
      "schema_version": 1,
      "source": "kernel",
      "generated_at": "2026-08-13T19:53:34Z",
      "manifest": {
        "llm": {"provider": "codex", "model": "gpt-5.6-sol"},
        "context_limit": 250000,
        "capabilities": {"web": {}, "avatar": {}, "shell": {}}
      },
      "preset": {
        "active": "~/.lingtai-tui/presets/saved/codex.json",
        "default": "~/.lingtai-tui/presets/saved/codex.json",
        "allowed": [
          "~/.lingtai-tui/presets/saved/deepseek_flash.json",
          "~/.lingtai-tui/presets/saved/codex.json",
          "~/.lingtai-tui/presets/saved/zhipu-1.json"
        ]
      }
    })JSON");

    const auto outcome = shell.open_project(project, std::nullopt);
    require(outcome.disposition == ProjectOpenDisposition::opened,
        "the Presets fixture project must open");

    click_agent(window, "agent-a");
    const auto expected_minimal = QStringLiteral(
        "Provider: codex\n"
        "Model: gpt-5.6-sol\n"
        "Default: ~/.lingtai-tui/presets/saved/codex.json\n"
        "Allowed:\n"
        "  • ~/.lingtai-tui/presets/saved/deepseek_flash.json\n"
        "  • ~/.lingtai-tui/presets/saved/codex.json\n"
        "  • ~/.lingtai-tui/presets/saved/zhipu-1.json");
    require(surface->toPlainText() == expected_minimal,
        "selecting Agent A must render exactly the minimal "
        "Provider/Model/Default/Allowed surface");
    require(state->text() == QStringLiteral("Resolved"),
        "a supported complete v1 artifact must show the Resolved state "
        "label");

    const auto text = surface->toPlainText();
    for (const QString &absent : {
            QStringLiteral("Active:"),
            QStringLiteral("Active effective"),
            QStringLiteral("Context limit:"),
            QStringLiteral("Capabilities:"),
            QStringLiteral("Source: kernel"),
            QStringLiteral("generated 2026-08-13T19:53:34Z") }) {
        require(!text.contains(absent),
            "the Presets surface must not render removed active-ref/badge/"
            "context/capability/source-provenance text");
    }
    require(!text.contains(QStringLiteral("[Active"))
            && !text.contains(QStringLiteral("[Default")),
        "the Presets surface must not render per-ref active/default badges");

    // The six generic identity/status/facts widgets are owned by their
    // non-Presets destination and must not be visible on the Presets page.
    required_child<QPushButton>(window, "lingtai_agent_page_nav_presets")
        ->click();
    QCoreApplication::processEvents();
    for (const char *name : {
            "lingtai_selected_agent_manifest_identity",
            "lingtai_selected_agent_manifest_llm",
            "lingtai_selected_agent_manifest_capabilities",
            "lingtai_selected_agent_status_activity",
            "lingtai_selected_agent_status_context",
            "lingtai_selected_agent_facts" }) {
        auto *label = required_child<QLabel>(window, name);
        require(!label->isVisible(),
            std::string("a generic fact widget must not be visible on the "
                        "Presets page: ") + name);
    }

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "Presets fixtures must be removed");
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: native_shell_presets_test FIXTURE_ROOT\n";
        return 2;
    }
    try {
        const auto sandbox = std::filesystem::canonical(argv[1]);
        std::filesystem::current_path(sandbox);
        QApplication application(argc, argv);
        lingtai::desktop::NativeShell shell;
        shell.show_offscreen();
        QCoreApplication::processEvents();
        verify_presets_simplified_surface(shell, sandbox);
        std::cout << "native shell presets: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "native shell presets: " << error.what() << '\n';
        return 1;
    }
}
