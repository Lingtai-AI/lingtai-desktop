#include "native_shell.h"

#include "ui/widgets/rp_window.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointF>
#include <QtCore/QString>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>

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

void click_agent_canvas_row(QWidget &window, int index) {
    auto *canvas = required_child<QWidget>(
        window, "lingtai_agent_roster_rows");
    const auto point = QPointF(20.0, 24.0 + static_cast<double>(index) * 64.0);
    auto press = QMouseEvent(
        QEvent::MouseButtonPress,
        point,
        point,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
    auto release = QMouseEvent(
        QEvent::MouseButtonRelease,
        point,
        point,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    QApplication::sendEvent(canvas, &release);
    QCoreApplication::processEvents();
}

QStringList catalog_preset_names(QTreeWidget *catalog) {
    auto names = QStringList();
    for (auto index = 0; index != catalog->topLevelItemCount(); ++index) {
        auto *item = catalog->topLevelItem(index);
        if (item->data(0, Qt::UserRole).isValid()) {
            names << item->text(0);
        }
    }
    return names;
}

// `/presets` reuses the setup catalog table and lists only this Agent's
// allowed presets, in published order.
void verify_presets_simplified_surface(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *catalog = required_child<QTreeWidget>(
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

    click_agent_canvas_row(window, 0);
    require(catalog_preset_names(catalog)
            == QStringList({
                QStringLiteral("deepseek_flash"),
                QStringLiteral("codex"),
                QStringLiteral("zhipu-1")}),
        "selecting Agent A must list only its allowed presets, in published "
        "order, using the setup catalog table");
    require(state->text() == QStringLiteral("Resolved"),
        "a supported complete v1 artifact must show the Resolved state "
        "label");
    require(catalog->editTriggers() == QAbstractItemView::NoEditTriggers,
        "the allowed-preset catalog must stay read-only");
    require(catalog->currentItem() != nullptr
            && catalog->currentItem()->text(0) == QStringLiteral("codex"),
        "the catalog must land on the Agent's active preset");

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
