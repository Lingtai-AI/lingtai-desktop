#include "native_shell.h"

#include "ui/rp_widget.h"
#include "ui/widgets/rp_window.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::ProjectOpenDisposition;

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ScopedApplicationPalette final {
public:
    explicit ScopedApplicationPalette(const QPalette &palette)
    : original_(QApplication::palette()) {
        QApplication::setPalette(palette);
    }

    ~ScopedApplicationPalette() {
        QApplication::setPalette(original_);
    }

    ScopedApplicationPalette(const ScopedApplicationPalette &) = delete;
    ScopedApplicationPalette &operator=(
        const ScopedApplicationPalette &) = delete;

private:
    QPalette original_;
};

template <typename Widget>
Widget *required_child(QWidget &root, const char *object_name) {
    auto *result = root.findChild<Widget *>(object_name);
    require(result != nullptr, std::string("missing child: ") + object_name);
    return result;
}

std::vector<std::string> project_tree(
        const fs::path &root) {
    auto result = std::vector<std::string>();
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
             root)) {
        const auto kind = entry.is_directory() ? "directory:" : "file:";
        result.push_back(
            kind + std::filesystem::relative(entry.path(), root).string());
    }
    std::ranges::sort(result);
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

std::string read_file(const fs::path &path) {
    auto stream = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

std::map<std::string, std::string> tree_snapshot(const fs::path &root) {
    auto result = std::map<std::string, std::string>();
    if (!fs::exists(root)) {
        return result;
    }
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        const auto key = entry.path().lexically_relative(root).generic_string();
        const auto status = entry.symlink_status();
        if (fs::is_symlink(status)) {
            result[key] = "symlink:" + fs::read_symlink(entry.path()).string();
        } else if (fs::is_directory(status)) {
            result[key] = "directory";
        } else if (fs::is_regular_file(status)) {
            result[key] = "file:" + read_file(entry.path());
        } else {
            result[key] = "other";
        }
    }
    return result;
}

// Builds one project with one valid, selectable Agent directory.
struct ProjectFixture {
    fs::path root;
    fs::path project;
    fs::path agent;

    ProjectFixture(const fs::path &base, std::string_view name)
    : root(base / name)
    , project(root / "project")
    , agent(project / ".lingtai" / "agent") {
        write_file(agent / ".agent.json", R"({"admin":{}})");
    }
};

lingtai::desktop::ProjectOpenOutcome open_without_writes(
        lingtai::desktop::NativeShell &shell,
        const ProjectFixture &fixture,
        const std::optional<fs::path> &agent) {
    const auto project_before = tree_snapshot(fixture.project);
    const auto outcome = shell.open_project(fixture.project, agent);
    require(tree_snapshot(fixture.project) == project_before,
        "every open must preserve project bytes and paths");
    return outcome;
}

QString label_text(QWidget &window, const char *object_name) {
    return required_child<QLabel>(window, object_name)->text();
}

QPushButton *agent_row(QWidget &window, std::string_view key) {
    const auto expected = QString::fromUtf8(key.data(), key.size());
    for (auto *row : window.findChildren<QPushButton *>()) {
        if (row->property("directory_key").toString() == expected) return row;
    }
    throw std::runtime_error("missing Agent row: " + std::string(key));
}

// The public `select_agent` test seam is gone; every selection below drives
// the same handler a real row click uses.
void click_agent(QWidget &window, std::string_view key) {
    agent_row(window, key)->click();
    QCoreApplication::processEvents();
}

void verify_dark_application_palette_inheritance(const fs::path &sandbox) {
    const auto window_surface = QColor(QStringLiteral("#121820"));
    const auto window_ink = QColor(QStringLiteral("#F1F5F9"));
    const auto text_surface = QColor(QStringLiteral("#0B1118"));
    const auto text_ink = QColor(QStringLiteral("#E2E8F0"));
    const auto button_surface = QColor(QStringLiteral("#263241"));
    const auto button_ink = QColor(QStringLiteral("#F8FAFC"));
    auto dark_palette = QApplication::palette();
    dark_palette.setColor(QPalette::Window, window_surface);
    dark_palette.setColor(QPalette::WindowText, window_ink);
    dark_palette.setColor(QPalette::Base, text_surface);
    dark_palette.setColor(QPalette::Text, text_ink);
    dark_palette.setColor(QPalette::Button, button_surface);
    dark_palette.setColor(QPalette::ButtonText, button_ink);

    const auto palette_scope = ScopedApplicationPalette(dark_palette);
    lingtai::desktop::NativeShell shell;
    auto &window = shell.window();
    auto *body = window.body().get();
    auto *sidebar = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_sidebar");
    auto *content = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_content");
    auto *empty_route = required_child<Ui::RpWidget>(
        window, "lingtai_empty_workspace_route");
    auto *error_surface = required_child<Ui::RpWidget>(
        window, "lingtai_project_open_error_surface");
    auto *project_route = required_child<Ui::RpWidget>(
        window, "lingtai_project_route");
    auto *directory = required_child<Ui::RpWidget>(
        window, "lingtai_agent_directory");
    auto *roster = required_child<Ui::RpWidget>(
        window, "lingtai_agent_roster");
    auto *rows = required_child<Ui::RpWidget>(
        window, "lingtai_agent_roster_rows");
    auto *roster_scroll = required_child<QWidget>(
        window, "lingtai_agent_roster_scroll");
    auto *detail = required_child<Ui::RpWidget>(
        window, "lingtai_agent_detail");
    const auto labels = std::vector<QLabel *>{
        required_child<QLabel>(window, "lingtai_sidebar_brand"),
        required_child<QLabel>(window, "lingtai_sidebar_workspace_label"),
        required_child<QLabel>(window, "lingtai_product_title"),
        required_child<QLabel>(window, "lingtai_product_purpose"),
        required_child<QLabel>(window, "lingtai_no_project_title"),
        required_child<QLabel>(window, "lingtai_no_project_detail"),
        required_child<QLabel>(window, "lingtai_project_open_error"),
        required_child<QLabel>(window, "lingtai_project_route_heading"),
        required_child<QLabel>(window, "lingtai_project_root"),
        required_child<QLabel>(window, "lingtai_agent_selection_error"),
        required_child<QLabel>(window, "lingtai_agent_roster_heading"),
        required_child<QLabel>(window, "lingtai_agent_roster_state"),
        required_child<QLabel>(window, "lingtai_agent_detail_heading"),
        required_child<QLabel>(window, "lingtai_selected_agent_key"),
        required_child<QLabel>(window, "lingtai_selected_agent_presentation_name"),
        required_child<QLabel>(window, "lingtai_selected_agent_manifest_identity"),
        required_child<QLabel>(window, "lingtai_selected_agent_manifest_llm"),
        required_child<QLabel>(window, "lingtai_selected_agent_manifest_capabilities"),
        required_child<QLabel>(window, "lingtai_selected_agent_status_activity"),
        required_child<QLabel>(window, "lingtai_selected_agent_status_context"),
        required_child<QLabel>(window, "lingtai_selected_agent_facts"),
    };
    auto *open_button = required_child<QPushButton>(
        window, "lingtai_open_project_button");

    for (const auto *surface : {
             body, sidebar, content, empty_route, error_surface,
             project_route, directory, roster, rows, detail }) {
        require(surface->palette().color(QPalette::Window) == window_surface,
            "dark application Window role must reach every shell surface");
    }
    for (const auto *label : labels) {
        require(label->palette().color(QPalette::WindowText) == window_ink,
            "dark application WindowText role must reach every shell label");
        require(label->textFormat() == Qt::PlainText,
            "every LingTai label surface must render explicit plain text");
    }
    require(open_button->palette().color(QPalette::Button) == button_surface,
        "dark application Button role must reach the open affordance");
    require(open_button->palette().color(QPalette::ButtonText) == button_ink,
        "dark application ButtonText role must reach the open affordance");
    require(roster_scroll->palette().color(QPalette::Base) == text_surface,
        "dark application Base role must reach the roster scroll surface");

    ProjectFixture fixture(sandbox, "palette");
    static_cast<void>(shell.open_project(fixture.project, std::nullopt));
    auto *row = agent_row(window, "agent");
    require(row->palette().color(QPalette::Button) == button_surface
            && row->palette().color(QPalette::ButtonText) == button_ink,
        "dark application button roles must reach Agent selection rows");
    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "palette fixture must be removed");
}

void verify_open_project_behavior(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *empty_route = required_child<Ui::RpWidget>(
        window, "lingtai_empty_workspace_route");
    auto *project_route = required_child<Ui::RpWidget>(
        window, "lingtai_project_route");
    auto *error_surface = required_child<Ui::RpWidget>(
        window, "lingtai_project_open_error_surface");

    const auto empty_root = sandbox / "empty-roster/project";
    fs::create_directories(empty_root / ".lingtai");
    const auto empty_before = tree_snapshot(empty_root);
    static_cast<void>(shell.open_project(empty_root));
    require(label_text(window, "lingtai_agent_roster_state")
            .contains("No Agents found — scan complete")
            && tree_snapshot(empty_root) == empty_before,
        "an empty complete roster must be distinct and read-only");

    ProjectFixture roster(sandbox, "roster-red");
    write_file(roster.agent / ".agent.json", R"({
        "agent_id":"manifest-agent-id",
        "agent_name":"Immutable Agent Name",
        "nickname":"Research Nickname",
        "address":"agent@example.test",
        "state":"manifest-ready",
        "llm":{"provider":"openai","model":"gpt-test",
            "base_url":"https://api.example.test/v1",
            "api_compat":"openai","context_limit":200000},
        "capabilities":["shell",["calendar",{}],"shell",42],
        "admin":{}
    })");
    write_file(roster.agent / ".status.json", R"({
        "identity":{"agent_id":"status-agent-id"},
        "runtime":{"state":"status-working","running":true,"pid":4242,
            "state_changed_at":1700000001,"last_progress_at":1700000002,
            "no_progress_seconds":7.25},
        "active_turn":{"kind":"analysis","id":"turn-42",
            "started_at":1700000003,"elapsed_seconds":9.5},
        "tokens":{"context":{"system_tokens":10,"tools_tokens":11,
            "history_tokens":39,"total_tokens":60,"window_size":1000,
            "usage_pct":41.5,"fixed_tokens":20,"growing_tokens":40}}
    })");
    write_file(roster.agent / ".agent.heartbeat", std::to_string(
        std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count()));
    const auto human_agent = roster.project / ".lingtai/a-human";
    write_file(human_agent / ".agent.json", R"({"admin":null})");
    write_file(human_agent / ".status.json",
        R"({"runtime":{"state":"human-idle","running":false}})");
    const auto main_agent = roster.project / ".lingtai/b-main";
    write_file(main_agent / ".agent.json",
        R"({"agent_id":"main-id","state":"manifest-main","admin":{"manage":true}})");
    write_file(main_agent / ".status.json", R"({
        "identity":{"agent_id":"main-id"},
        "runtime":{"state":"manifest-main","running":false},
        "tokens":{"context":{"system_tokens":-3,"tools_tokens":-4,
            "history_tokens":-5,"total_tokens":-12,"window_size":100,
            "usage_pct":-7.5,"fixed_tokens":-8,"growing_tokens":-9}}
    })");
    write_file(roster.project / ".lingtai/b-main/.agent.heartbeat",
        std::to_string(std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count()));
    write_file(roster.project / ".lingtai/c-stale/.agent.json",
        R"({"admin":{}})");
    write_file(roster.project / ".lingtai/c-stale/.agent.heartbeat", "0");
    write_file(roster.project / ".lingtai/c-stale/.status.json",
        R"({"tokens":{"context":{"total_tokens":60,"window_size":"invalid","usage_pct":88}}})");
    write_file(roster.project / ".lingtai/d-missing/.agent.json",
        R"({"admin":{}})");
    write_file(roster.project / ".lingtai/malformed/.agent.json", "{");
    constexpr auto ampersand_key = "A&B-agent";
    constexpr auto plain_neighbor_key = "AB-agent";
    const auto ampersand_agent = roster.project / ".lingtai" / ampersand_key;
    write_file(ampersand_agent / ".agent.json", R"({"admin":{}})");
    const auto plain_neighbor_agent =
        roster.project / ".lingtai" / plain_neighbor_key;
    write_file(plain_neighbor_agent / ".agent.json", R"({"admin":{}})");
    const auto roster_outcome = open_without_writes(
        shell, roster, std::nullopt);
    require(roster_outcome.disposition == ProjectOpenDisposition::opened,
        "roster fixture must open through the existing no-Agent API");
    required_child<Ui::RpWidget>(window, "lingtai_agent_roster");
    const auto expected_rows = std::vector<std::pair<std::string, std::string>>{
        {ampersand_key, "valid — agent — missing"},
        {plain_neighbor_key, "valid — agent — missing"},
        {"a-human", "valid — human — alive_human"},
        {"agent", "valid — agent — alive"},
        {"b-main", "valid — main — alive"},
        {"c-stale", "valid — agent — stale"},
        {"d-missing", "valid — agent — missing"},
        {"malformed", "malformed — unknown — unknown"},
    };
    for (const auto &[key, facts] : expected_rows) {
        auto *row = agent_row(window, key);
        require(row->text().contains(QString::fromStdString(facts)),
            key + " must expose exact manifest, role, and presence truth");
        require(row->isEnabled() == (key != "malformed"),
            key + " selectability must derive from valid manifest truth");
    }
    auto visible_keys = std::vector<std::string>();
    const auto *rows_layout = required_child<Ui::RpWidget>(
        window, "lingtai_agent_roster_rows")->layout();
    for (auto index = 0; index != rows_layout->count(); ++index) {
        if (const auto *row = qobject_cast<QPushButton *>(
                rows_layout->itemAt(index)->widget())) {
            visible_keys.push_back(
                row->property("directory_key").toString().toStdString());
        }
    }
    require(visible_keys == std::vector<std::string>{
            ampersand_key, plain_neighbor_key, "a-human", "agent",
            "b-main", "c-stale", "d-missing", "malformed"},
        "native rows must render in the composite snapshot's deterministic order");
    auto *ampersand_row = agent_row(window, ampersand_key);
    auto *plain_neighbor_row = agent_row(window, plain_neighbor_key);
    require(ampersand_row->property("directory_key").toString()
                == QStringLiteral("A&B-agent")
            && ampersand_row->accessibleName()
                == QStringLiteral("Agent A&B-agent")
            && ampersand_row->isEnabled(),
        "ampersand row identity and selectability must retain the exact key");
    require(ampersand_row->text().startsWith(
                QStringLiteral("A&&B-agent\n")),
        "Agent row button text must escape ampersands for lossless display");
    require(plain_neighbor_row->text().startsWith(
                QStringLiteral("AB-agent\n"))
            && ampersand_row->text() != plain_neighbor_row->text(),
        "ampersand and plain neighboring Agent rows must remain distinguishable");
    require(agent_row(window, "malformed")->text().contains("invalid JSON"),
        "a malformed row must expose its typed repair diagnostic");
    const auto roster_status = label_text(window, "lingtai_agent_roster_state");
    require(roster_status.contains("scan complete"),
        "a complete roster reports scan completion");

    agent_row(window, "malformed")->click();
    require(!shell.selection_state().selected_agent_directory_key(),
        "a disabled malformed row must not change C1 truth");

    const auto roster_before_selection = tree_snapshot(roster.project);
    ampersand_row->click();
    QCoreApplication::processEvents();
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>(ampersand_key)
            && agent_row(window, ampersand_key)->isChecked()
            && !agent_row(window, plain_neighbor_key)->isChecked()
            && label_text(window, "lingtai_selected_agent_key")
                == QStringLiteral("A&B-agent")
            && required_child<QLabel>(window, "lingtai_selected_agent_key")
                ->textFormat() == Qt::PlainText,
        "ampersand row selection must preserve exact C1, detail, and plain-text truth");
    require(tree_snapshot(roster.project) == roster_before_selection,
        "ampersand selection at its exact project-relative path must remain read-only");
    click_agent(window, "agent");
    require(shell.selection_state().selected_agent_directory_key()
            == std::optional<fs::path>("agent")
            && agent_row(window, "agent")->isChecked()
            && label_text(window, "lingtai_selected_agent_key") == "agent",
        "detail and highlight must derive from sole C1 selected-key truth");
    require(label_text(window, "lingtai_selected_agent_presentation_name")
            == QStringLiteral("Research Nickname"),
        "selected detail must prefer the manifest nickname as its presentation name");
    require(label_text(window, "lingtai_selected_agent_manifest_identity")
            == QStringLiteral("Manifest identity\ntrue name: Immutable Agent Name\n"
                "address: agent@example.test\nagent ID: manifest-agent-id\n"
                "state: manifest-ready"),
        "manifest identity must retain true name, address, ID, and state without status reconciliation");
    require(label_text(window, "lingtai_selected_agent_manifest_llm")
            == QStringLiteral("Manifest live LLM\nprovider: openai\nmodel: gpt-test\n"
                "base URL: https://api.example.test/v1\nAPI compatibility: openai\n"
                "context limit: 200000"),
        "manifest live LLM fields must remain source-labelled and exact");
    require(label_text(window, "lingtai_selected_agent_manifest_capabilities")
            == QStringLiteral("Manifest capabilities\ndisplay names: system, soul, "
                "email, psyche, shell, calendar"),
        "manifest capability display projection de-duplicates and leads with "
        "intrinsics, without exposing raw manifest evidence");
    require(label_text(window, "lingtai_selected_agent_status_activity")
            == QStringLiteral("Status activity\nstate: status-working\nrunning: true\n"
                "PID: 4242\nstate changed at: 1700000001\n"
                "last progress at: 1700000002\nno progress seconds: 7.25\n"
                "active turn kind: analysis\nactive turn ID: turn-42\n"
                "active turn started at: 1700000003\n"
                "active turn elapsed seconds: 9.5"),
        "status activity and active turn must remain independent projected evidence");
    require(label_text(window, "lingtai_selected_agent_status_context")
            == QStringLiteral("Status context (source values)\nwindow size: 1000\n"
                "system tokens: 10\ntools tokens: 11\nhistory tokens: 39\n"
                "total tokens: 60\nusage_percent (source usage_pct): 41.5\n"
                "fixed tokens: 20\ngrowing tokens: 40"),
        "status context must display source usage_pct rather than recomputing 6 percent");
    require(required_child<QLabel>(window,
                "lingtai_selected_agent_manifest_identity")->accessibleName()
                == QStringLiteral("Manifest identity")
            && required_child<QLabel>(window,
                "lingtai_selected_agent_status_activity")->accessibleName()
                == QStringLiteral("Status activity"),
        "semantic source labels must retain stable accessibility names");
    require(tree_snapshot(roster.project) == roster_before_selection,
        "selection must preserve the project across a full detail render");

    click_agent(window, "b-main");
    require(label_text(window, "lingtai_selected_agent_status_context")
                == QStringLiteral("Status context (source values)\nwindow size: 100\n"
                    "system tokens: -3\ntools tokens: -4\nhistory tokens: -5\n"
                    "total tokens: -12\nusage_percent (source usage_pct): -7.5\n"
                    "fixed tokens: -8\ngrowing tokens: -9")
            && window.findChildren<QProgressBar *>().empty(),
        "odd negative context must remain plain source evidence without a gauge");

    click_agent(window, "d-missing");
    require(label_text(window, "lingtai_selected_agent_status_activity")
                == QStringLiteral("Status activity unavailable from status source"),
        "absent status must remain explicit unavailable evidence");

    click_agent(window, "c-stale");
    require(label_text(window, "lingtai_selected_agent_status_context")
                == QStringLiteral(
                    "Status context unavailable (no valid positive window projected)")
            && !label_text(window, "lingtai_selected_agent_status_context")
                .contains("88"),
        "invalid window must suppress the whole unprojected context");
    click_agent(window, "agent");

    agent_row(window, "b-main")->click();
    require(shell.selection_state().selected_agent_directory_key()
            == std::optional<fs::path>("b-main")
            && agent_row(window, "b-main")->isChecked(),
        "row clicks must use the same C1-owning selection handler");
    click_agent(window, "agent");
    const auto refreshed = open_without_writes(shell, roster, std::nullopt);
    require(refreshed.disposition == ProjectOpenDisposition::opened
            && shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("agent")
            && agent_row(window, "agent")->isChecked(),
        "same-root refresh must preserve a still-valid selection and detail");
    write_file(roster.agent / ".agent.json", "{");
    const auto malformed_before_refresh = tree_snapshot(roster.project);
    const auto repaired = shell.open_project(roster.project, std::nullopt);
    require(repaired.disposition == ProjectOpenDisposition::opened
            && !shell.selection_state().selected_agent_directory_key()
            && !agent_row(window, "agent")->isEnabled()
            && label_text(window, "lingtai_selected_agent_presentation_name").isEmpty(),
        "same-root refresh must clear a selected key that became malformed");
    require(tree_snapshot(roster.project) == malformed_before_refresh,
        "same-root repair refresh must remain read-only");

    ProjectFixture recognized(sandbox, "recognized");
    const auto recognized_outcome = open_without_writes(
        shell, recognized, fs::path(".lingtai/agent"));
    const auto recognized_root = fs::canonical(recognized.project);
    require(recognized_outcome.disposition == ProjectOpenDisposition::opened,
        "recognized project inputs must open");
    require(shell.selection_state().active_project()
            && shell.selection_state().active_project()->root() == recognized_root,
        "successful open must activate the canonical C1 project root");
    require(!empty_route->isVisible() && project_route->isVisible(),
        "successful open must transition to the project route");
    require(!error_surface->isVisible(),
        "successful open must clear the open error");
    require(label_text(window, "lingtai_project_root").toStdString()
            == recognized_root.string(),
        "project route must display the canonical root");

    lingtai::desktop::NativeShell failed_shell;
    failed_shell.show_offscreen();
    QCoreApplication::processEvents();
    auto &failed_window = failed_shell.window();
    auto *failed_error = required_child<Ui::RpWidget>(
        failed_window, "lingtai_project_open_error_surface");
    const auto invalid_root = sandbox / "invalid-opens";
    const auto ordinary = invalid_root / "ordinary";
    fs::create_directories(ordinary);
    const auto ordinary_before = tree_snapshot(ordinary);
    auto failed = failed_shell.open_project(ordinary);
    require(failed.disposition == ProjectOpenDisposition::failed
            && failed.failure
                == lingtai::desktop::ProjectPathFailure::target_not_found,
        "ordinary directory without .lingtai must fail visibly");
    require(tree_snapshot(ordinary) == ordinary_before,
        "failed ordinary-directory open must write nothing");
    require(!failed_shell.selection_state().active_project()
            && failed_error->isVisible(),
        "fresh failed open must retain empty C1 state and show an error");

    const auto invalid_metadata = invalid_root / "invalid-metadata";
    fs::create_directories(invalid_metadata);
    write_file(invalid_metadata / ".lingtai", "not a directory");
    const auto invalid_metadata_before = tree_snapshot(invalid_metadata);
    failed = failed_shell.open_project(invalid_metadata);
    require(failed.failure
            == lingtai::desktop::ProjectPathFailure::target_not_directory,
        "non-directory .lingtai must have a typed visible failure");
    require(tree_snapshot(invalid_metadata) == invalid_metadata_before,
        "non-directory .lingtai failure must write nothing");

    const auto non_directory = invalid_root / "not-a-directory";
    write_file(non_directory, "file");
    failed = failed_shell.open_project(non_directory);
    require(failed.failure
            == lingtai::desktop::ProjectPathFailure::selection_not_directory,
        "non-directory selection must have a typed visible failure");
    failed = failed_shell.open_project(invalid_root / "absent");
    require(failed.failure
            == lingtai::desktop::ProjectPathFailure::selection_not_found,
        "missing selection must have a typed visible failure");

    const auto escape_project = invalid_root / "escape-project";
    const auto outside = invalid_root / "outside-lingtai";
    fs::create_directories(escape_project);
    fs::create_directories(outside);
    std::error_code link_error;
    fs::create_directory_symlink(
        outside, escape_project / ".lingtai", link_error);
    require(!link_error, "escaping .lingtai symlink fixture must be created");
    const auto escape_before = tree_snapshot(escape_project);
    failed = failed_shell.open_project(escape_project);
    require(failed.failure
            == lingtai::desktop::ProjectPathFailure::outside_project,
        "escaping .lingtai symlink must be rejected as outside project");
    require(tree_snapshot(escape_project) == escape_before,
        "escaping .lingtai failure must write nothing");
    require(!failed_shell.selection_state().active_project(),
        "all fresh failed opens must preserve empty C1 state");

    const auto active_root = shell.selection_state().active_project()->root();
    const auto prior_selection = shell.selection_state()
        .selected_agent_directory_key();
    const auto prior_row_text = agent_row(window, "agent")->text();
    const auto later_failure = shell.open_project(ordinary);
    require(later_failure.disposition == ProjectOpenDisposition::failed,
        "failed reopen must return failed");
    require(shell.selection_state().active_project()
            && shell.selection_state().active_project()->root() == active_root,
        "failed reopen must preserve the valid active project");
    require(shell.selection_state().selected_agent_directory_key()
            == prior_selection
            && agent_row(window, "agent")->text() == prior_row_text,
        "failed reopen must preserve selection, roster, and selected detail");
    require(error_surface->isVisible()
            && label_text(window, "lingtai_project_open_error")
                .contains("not a LingTai project"),
        "failed reopen must show its error beside the preserved roster");

    const auto reopened = open_without_writes(
        shell, recognized, fs::path(".lingtai/agent"));
    require(reopened.disposition == ProjectOpenDisposition::opened
            && !error_surface->isVisible(),
        "a later successful open must clear the transient open error");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "focused fixtures must be removed");
}

void verify_semantics_and_request(
        lingtai::desktop::NativeShell &shell,
        const std::filesystem::path &project_root) {
    static_assert(std::is_same_v<
        decltype(shell.window()), Ui::RpWindow &>);

    auto &window = shell.window();
    auto *body = window.body().get();
    auto *sidebar = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_sidebar");
    auto *content = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_content");
    auto *empty_route = required_child<Ui::RpWidget>(
        window, "lingtai_empty_workspace_route");
    auto *project_route = required_child<Ui::RpWidget>(
        window, "lingtai_project_route");
    auto *open_error = required_child<QLabel>(
        window, "lingtai_project_open_error");
    auto *title = required_child<QLabel>(
        window, "lingtai_product_title");
    auto *purpose = required_child<QLabel>(
        window, "lingtai_product_purpose");
    auto *empty_title = required_child<QLabel>(
        window, "lingtai_no_project_title");
    auto *empty_detail = required_child<QLabel>(
        window, "lingtai_no_project_detail");
    auto *open_button = required_child<QPushButton>(
        window, "lingtai_open_project_button");

    require(window.objectName() == "lingtai_desktop_window",
        "window semantic name changed");
    require(body != &window, "RpWindow body must be a real child RpWidget");
    require(body->objectName() == "lingtai_desktop_body",
        "body semantic name changed");
    require(window.accessibleName() == "LingTai Desktop",
        "window needs an accessible product name");
    require(sidebar->accessibleName() == "Workspace navigation",
        "sidebar needs an accessible region name");
    require(content->accessibleName() == "Workspace content",
        "content needs an accessible region name");
    require(title->text() == "LingTai Desktop",
        "product title changed");
    require(purpose->text()
            == "A clear view of the project and Agents you choose.",
        "product purpose changed");
    require(empty_title->text() == "No project open",
        "empty-route title changed");
    require(empty_detail->text()
            == "Open a LingTai project to inspect its Agents.",
        "empty-route explanation changed");
    require(open_button->text() == QStringLiteral("Open Project\u2026"),
        "open affordance text changed");
    require(open_button->accessibleName() == "Open Project",
        "open affordance needs a static accessible name");

    const auto &selection = shell.selection_state();
    require(!selection.active_project().has_value(),
        "new shell must have no active project");
    require(!selection.selected_agent_directory_key().has_value(),
        "new shell must have no selected Agent");
    require(empty_route->isVisible(),
        "no-workspace truth must show the empty route");
    require(!project_route->isVisible(),
        "new shell must hide the project route");
    require(label_text(window, "lingtai_agent_roster_state")
            .contains("Roster unavailable"),
        "an unopened project must remain distinct from an empty scanned roster");
    require(!open_error->isVisible(),
        "new shell must hide the project open error");

    const auto tree_before = project_tree(project_root);
    auto callback_count = std::size_t{0};
    shell.set_open_project_request_handler([&] {
        ++callback_count;
    });
    open_button->click();
    require(callback_count == 1,
        "one click must emit exactly one open request");
    require(!selection.active_project().has_value(),
        "open request must not activate a project");
    require(!selection.selected_agent_directory_key().has_value(),
        "open request must not select an Agent");
    require(project_tree(project_root) == tree_before,
        "open request must not write the project tree");
}

// One real kernel envelope, including the misleading legacy `body` field that
// must never be rendered in place of `message`.
std::string conversation_envelope(
        std::string_view from,
        std::string_view to,
        std::string_view subject,
        std::string_view message,
        std::string_view timestamp_key,
        std::string_view timestamp) {
    return std::string(R"({"from":")") + std::string(from)
        + R"(","to":[")" + std::string(to)
        + R"("],"subject":")" + std::string(subject)
        + R"(","message":")" + std::string(message)
        + R"(","body":"MISLEADING LEGACY BODY FIELD","type":"normal")"
        + R"(,"mode":"peer","attachments":["never-touched.bin"],")"
        + std::string(timestamp_key) + R"(":")" + std::string(timestamp)
        + R"("})";
}

// The visible product slice: opening a real project and selecting an Agent must
// show that Agent's current direct conversation as read-only plain text.
void verify_selected_agent_conversation(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *heading = required_child<QLabel>(
        window, "lingtai_selected_agent_conversation_heading");
    auto *surface = required_child<QPlainTextEdit>(
        window, "lingtai_selected_agent_conversation");
    auto *state = required_child<QLabel>(
        window, "lingtai_selected_agent_conversation_state");

    require(surface->isReadOnly(), "the conversation surface must be read-only");
    require(surface->textInteractionFlags().testFlag(
                Qt::TextSelectableByMouse),
        "conversation text must remain selectable for copying");
    require(!surface->accessibleName().isEmpty()
            && !heading->accessibleName().isEmpty()
            && !state->accessibleName().isEmpty(),
        "the conversation surface must be accessible");
    require(surface->minimumHeight() >= 120,
        "the conversation surface must have a usable minimum height");

    // It must be reachable without scrolling past every manifest/status label.
    const auto *detail_layout = required_child<Ui::RpWidget>(
        window, "lingtai_agent_detail")->layout();
    auto index_of = [&](QWidget *widget) {
        for (auto index = 0; index != detail_layout->count(); ++index) {
            if (detail_layout->itemAt(index)->widget() == widget) return index;
        }
        throw std::runtime_error("detail child is not in the detail layout");
    };
    require(index_of(surface) < index_of(required_child<QLabel>(
                window, "lingtai_selected_agent_manifest_identity")),
        "the conversation must not be buried below the manifest detail labels");
    require(index_of(heading) < index_of(surface)
            && index_of(surface) < index_of(state),
        "heading, surface, and state must read in that order");

    const auto project = sandbox / "project";
    const auto mailbox = project / ".lingtai" / "human" / "mailbox";
    write_file(project / ".lingtai" / "human" / ".agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    const auto target = project / ".lingtai" / "telegram-bot";
    write_file(target / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-d0c8",)"
        R"("agent_name":"telegram-bot","nickname":"Telegram Bot",)"
        R"("address":"telegram-bot","state":"active"})");
    // A second Agent in the same project: a valid route with no mail at all.
    const auto quiet = project / ".lingtai" / "issue-643";
    write_file(quiet / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191610-q001",)"
        R"("agent_name":"issue-643","address":"issue-643","state":"active"})");
    write_file(mailbox / "inbox" / "20260807T184852-0d13" / "message.json",
        conversation_envelope("telegram-bot", "human", "Slice done",
            "PR published, not merged. <b>#1223</b> & <not-a-tag>",
            "received_at", "2026-08-07T18:48:52Z"));
    write_file(mailbox / "sent" / "20260807T190000-aa01" / "message.json",
        conversation_envelope("human", "telegram-bot", "Re: Slice done",
            "Thanks, reviewing tomorrow.", "sent_at",
            "2026-08-07T19:00:00Z"));
    // Present but not part of this conversation, and one unusable neighbor.
    write_file(mailbox / "inbox" / "20260807T185000-zz99" / "message.json",
        conversation_envelope("codex", "human", "Unrelated",
            "SHOULD NOT APPEAR", "received_at", "2026-08-07T18:50:00Z"));
    write_file(mailbox / "inbox" / "20260807T185500-bad0" / "message.json",
        R"({"from":"telegram-bot","to":["human"],"message":)");

    // Every fixture byte is on disk by this point, so the one comparison at the
    // end of this path covers opening, selecting, and reselecting.
    const auto fixture_before = tree_snapshot(project);
    const auto open_outcome = shell.open_project(project, std::nullopt);
    require(open_outcome.disposition != ProjectOpenDisposition::failed,
        "the conversation fixture project must open");
    require(surface->toPlainText().contains(QStringLiteral("Select an Agent")),
        "opening without a selection must prompt for one");

    click_agent(window, "telegram-bot");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("telegram-bot"),
        "the target Agent must be selectable");

    const auto conversation = surface->toPlainText();
    const auto incoming = conversation.indexOf(
        QStringLiteral("PR published, not merged."));
    const auto outgoing = conversation.indexOf(
        QStringLiteral("Thanks, reviewing tomorrow."));
    require(incoming >= 0 && outgoing >= 0,
        "both real messages must be visible in the conversation surface");
    require(incoming < outgoing,
        "the conversation must read in chronological order");
    // Literal plain text: markup in a real message is never interpreted.
    require(conversation.contains(
                QStringLiteral("<b>#1223</b> & <not-a-tag>")),
        "the surface must preserve message text literally as plain text");
    require(!conversation.contains(QStringLiteral("MISLEADING")),
        "the legacy `body` field must never be rendered");
    require(!conversation.contains(QStringLiteral("SHOULD NOT APPEAR")),
        "mail for another conversation must be absent");
    require(state->text() == QStringLiteral("2 messages · 1 skipped"),
        "the compact state must show the count and the generic skipped count");

    // A valid route whose Agent has no mail is an ordinary empty conversation.
    click_agent(window, "issue-643");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("issue-643"),
        "the second Agent in the same project must be selectable");
    require(surface->toPlainText() == QStringLiteral("No messages yet."),
        "a valid route with no rows must say so exactly");

    require(tree_snapshot(project) == fixture_before,
        "opening and selecting must never write to the project");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "conversation fixtures must be removed");
}

// The final Step-3 product action: a visible composer that queues one real
// plain-text human outbox entry and refreshes the conversation. One journey
// covers enablement, the whitespace guard, a real send, a stale-selection
// guard, and a failure that preserves the typed text.
void verify_composer_send_behavior(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *surface = required_child<QPlainTextEdit>(
        window, "lingtai_selected_agent_conversation");
    auto *input = required_child<QLineEdit>(
        window, "lingtai_composer_input");
    auto *send_button = required_child<QPushButton>(
        window, "lingtai_composer_send_button");
    auto *status = required_child<QLabel>(
        window, "lingtai_composer_status");

    const auto project = sandbox / "project";
    const auto outbox = project / ".lingtai/human/mailbox/outbox";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    const auto target = project / ".lingtai/telegram-bot";
    write_file(target / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-d0c8",)"
        R"("agent_name":"telegram-bot","nickname":"Telegram Bot",)"
        R"("address":"telegram-bot","state":"active"})");
    const auto other = project / ".lingtai/issue-643";
    write_file(other / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191610-q001",)"
        R"("agent_name":"issue-643","address":"issue-643","state":"active"})");

    static_cast<void>(shell.open_project(project, std::nullopt));
    require(!input->isEnabled() && !send_button->isEnabled(),
        "the composer must stay disabled until a valid route is selected");

    click_agent(window, "telegram-bot");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("telegram-bot"),
        "the first Agent must be selectable");
    require(input->isEnabled() && send_button->isEnabled() && input->text().isEmpty(),
        "a selected valid route must enable an empty composer");

    input->setText(QStringLiteral("   \t  "));
    const auto before_whitespace = tree_snapshot(project);
    send_button->click();
    require(!fs::exists(outbox),
        "whitespace-only input must be rejected without writing anything");
    require(input->text() == QStringLiteral("   \t  "),
        "a rejected whitespace-only send must preserve the typed input");
    require(status->text() != QStringLiteral("Queued"),
        "a rejected whitespace-only send must not claim success");
    require(tree_snapshot(project) == before_whitespace,
        "a rejected whitespace-only send must write nothing");

    input->setText(QStringLiteral("Ted, the slice is complete."));
    send_button->click();
    require(input->text().isEmpty(),
        "a successful send must clear the composer");
    require(status->text() == QStringLiteral("Queued"),
        "a successful send must show the concise success status");
    require(surface->toPlainText().contains(QStringLiteral("You ·"))
            && surface->toPlainText().contains(
                QStringLiteral("Ted, the slice is complete.")),
        "a successful send must refresh the conversation to show the new row");
    require(fs::exists(outbox), "a successful send must create the outbox folder");
    auto first_leaves = std::vector<fs::path>();
    for (const auto &entry : fs::directory_iterator(outbox)) {
        first_leaves.push_back(entry.path());
    }
    require(first_leaves.size() == 1,
        "exactly one leaf must be created by the one successful send");
    const auto first_body = read_file(first_leaves.front() / "message.json");
    require(first_body.find("\"to\":[\"telegram-bot\"]") != std::string::npos,
        "the queued entry must address exactly the selected Agent");

    // Selection change must not let a later click target the prior Agent.
    click_agent(window, "issue-643");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("issue-643"),
        "the second Agent in the same project must be selectable");
    require(input->text().isEmpty() && status->text().isEmpty(),
        "selecting a different Agent must reset the composer, not carry a draft");
    input->setText(QStringLiteral("A message for the other Agent."));
    send_button->click();
    require(status->text() == QStringLiteral("Queued"),
        "the send after switching Agents must still succeed");
    auto second_agent_bodies = std::vector<std::string>();
    for (const auto &entry : fs::directory_iterator(outbox)) {
        second_agent_bodies.push_back(read_file(entry.path() / "message.json"));
    }
    require(second_agent_bodies.size() == 2,
        "the second send must allocate its own fresh leaf");
    require(std::ranges::any_of(second_agent_bodies, [](const auto &body) {
            return body.find("\"to\":[\"issue-643\"]") != std::string::npos
                && body.find("A message for the other Agent.") != std::string::npos;
        }),
        "the send after switching selection must target the newly selected "
        "Agent, never the stale prior one");
    require(std::ranges::none_of(second_agent_bodies, [](const auto &body) {
            return body.find("A message for the other Agent.") != std::string::npos
                && body.find("\"to\":[\"telegram-bot\"]") != std::string::npos;
        }),
        "the post-switch send must never reach the previously selected Agent");

    // A blocked outbox path makes the send fail closed while preserving text.
    const auto blocked_project = sandbox / "blocked-project";
    write_file(blocked_project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    const auto blocked_target = blocked_project / ".lingtai/telegram-bot";
    write_file(blocked_target / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-d0c8",)"
        R"("agent_name":"telegram-bot","address":"telegram-bot","state":"active"})");
    write_file(blocked_project / ".lingtai/human/mailbox/outbox",
        "not a directory");

    static_cast<void>(shell.open_project(blocked_project, std::nullopt));
    click_agent(window, "telegram-bot");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("telegram-bot"),
        "the blocked-outbox fixture Agent must still be selectable");
    input->setText(QStringLiteral("Should never be queued."));
    const auto blocked_before = tree_snapshot(blocked_project);
    send_button->click();
    require(input->text() == QStringLiteral("Should never be queued."),
        "a failed send must preserve the typed text");
    require(status->text() != QStringLiteral("Queued")
            && !status->text().isEmpty(),
        "a failed send must show a concise, non-empty, non-success status");
    require(tree_snapshot(blocked_project) == blocked_before,
        "a failed send must write nothing");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "composer fixtures must be removed");
}

void verify_layout(lingtai::desktop::NativeShell &shell) {
    auto &window = shell.window();
    auto *body = window.body().get();
    auto *sidebar = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_sidebar");
    auto *content = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_content");

    require(window.minimumSize() == QSize(720, 480),
        "window minimum size must protect the two-region layout");
    window.resize(720, 480);
    QCoreApplication::processEvents();
    const auto narrow_content_width = content->width();
    require(window.width() >= 720 && window.height() >= 480,
        "window must honor its minimum size");
    require(sidebar->width() == 264,
        "sidebar must remain bounded");
    require(sidebar->height() == content->height()
            && sidebar->height() == body->height(),
        "sidebar and content must fill the body");
    require(sidebar->geometry().right() < content->geometry().left(),
        "sidebar and content must be distinct regions");

    window.resize(1200, 800);
    QCoreApplication::processEvents();
    require(sidebar->width() == 264,
        "sidebar width must stay bounded after resize");
    require(content->width() > narrow_content_width,
        "content region must absorb added window width");
    require(sidebar->height() == content->height()
            && sidebar->height() == body->height(),
        "both regions must continue filling the resized body");
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: native_shell_test PROJECT_ROOT\n";
        return 2;
    }
    try {
        const auto project_root = std::filesystem::canonical(argv[1]);
        std::filesystem::current_path(project_root);
        QApplication application(argc, argv);
        const auto original_palette = QApplication::palette();
        verify_dark_application_palette_inheritance(
            project_root / "commit-8-palette-fixture");
        require(QApplication::palette() == original_palette,
            "dark palette test must restore the application palette");
        lingtai::desktop::NativeShell shell;
        shell.show_offscreen();
        QCoreApplication::processEvents();
        verify_semantics_and_request(shell, project_root);
        verify_open_project_behavior(
            shell, project_root / "commit-7-open-project-fixtures");
        verify_selected_agent_conversation(
            shell, project_root / "commit-13-conversation-fixture");
        verify_composer_send_behavior(
            shell, project_root / "commit-14-composer-fixture");
        verify_layout(shell);
        std::cout << "native shell behavior: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "native shell behavior: " << error.what() << '\n';
        return 1;
    }
}
