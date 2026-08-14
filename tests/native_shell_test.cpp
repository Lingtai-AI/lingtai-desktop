#include "native_shell.h"

#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/rp_window.h"
#include "ui/widgets/shadow.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtCore/QThread>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QKeyEvent>
#include <QtGui/QPalette>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtGui/QTextFormat>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>

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

void append_file(const fs::path &path, std::string_view bytes) {
    auto stream = std::ofstream(path, std::ios::binary | std::ios::app);
    stream << bytes;
    require(stream.good(), "fixture file must be appended: " + path.string());
}

std::string read_file(const fs::path &path) {
    auto stream = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

// Writes one executable POSIX shell "python" stand-in used only by the
// Start Agent journey below. It never runs a real interpreter: it records
// its exact received argv, then -- only when `heartbeat_path` is given --
// simulates the target kernel's later fresh-heartbeat write after a short
// delay, from a detached background subshell independent of this test
// process, exactly like the real spawned `lingtai run` would eventually do.
void write_fixture_python(
        const fs::path &python_path,
        const fs::path &argv_record_path,
        const std::optional<fs::path> &heartbeat_path,
        int delay_seconds) {
    auto script = std::string("#!/bin/sh\nprintf '%s\\n' \"$@\" >> \"")
        + argv_record_path.string() + "\"\n"
        + "echo lingtai-fixture-stdout-marker\n"
        + "echo lingtai-fixture-stderr-marker 1>&2\n";
    if (heartbeat_path) {
        script += "( sleep " + std::to_string(delay_seconds)
            + "; date +%s > \"" + heartbeat_path->string() + "\" ) &\n";
    }
    script += "exit 0\n";
    write_file(python_path, script);
    std::error_code error;
    fs::permissions(python_path,
        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
            | fs::perms::others_read | fs::perms::others_exec,
        fs::perm_options::replace, error);
    require(!error, "fixture python must be made executable");
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

QString path_text(const fs::path &path) {
    const auto bytes = path.u8string();
    return QString::fromUtf8(
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<qsizetype>(bytes.size()));
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
    require(agent_row(window, "malformed")->accessibleDescription()
                .contains("invalid JSON"),
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
            && label_text(window, "lingtai_selected_agent_presentation_name")
                == QStringLiteral("A&B-agent")
            && label_text(window, "lingtai_selected_agent_key")
                == QStringLiteral("role: agent · presence: missing")
            && required_child<QLabel>(window, "lingtai_selected_agent_key")
                ->textFormat() == Qt::PlainText,
        "a key-fallback header must show the directory key once and never "
        "repeat it inside the detail header");
    require(tree_snapshot(roster.project) == roster_before_selection,
        "ampersand selection at its exact project-relative path must remain read-only");
    click_agent(window, "agent");
    require(shell.selection_state().selected_agent_directory_key()
            == std::optional<fs::path>("agent")
            && agent_row(window, "agent")->isChecked(),
        "detail and highlight must derive from sole C1 selected-key truth");
    require(label_text(window, "lingtai_selected_agent_presentation_name")
            == QStringLiteral("Research Nickname"),
        "selected detail must prefer the manifest nickname as its presentation name");
    require(label_text(window, "lingtai_selected_agent_key")
            == QStringLiteral("agent · role: agent · presence: alive"),
        "a distinct presentation title must keep one compact secondary line "
        "with the directory key plus the existing role/presence summary");
    require(label_text(window, "lingtai_selected_agent_manifest_identity")
            == QStringLiteral("Manifest identity\naddress: agent@example.test\n"
                "agent ID: manifest-agent-id\nstate: manifest-ready"),
        "manifest identity must not repeat the true name already used as the "
        "prominent title");
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

    // The real QTextDocument must expose the two directions as distinct
    // message blocks: the incoming row under its sender header and the
    // outgoing row under the "You" header, oppositely aligned with distinct
    // real backgrounds.
    auto incoming_block = QTextBlock();
    auto outgoing_block = QTextBlock();
    for (auto block = surface->document()->begin();
            block != surface->document()->end();
            block = block.next()) {
        if (block.text().startsWith(QStringLiteral("Telegram Bot ·"))) {
            incoming_block = block;
        } else if (block.text().startsWith(QStringLiteral("You ·"))) {
            outgoing_block = block;
        }
    }
    require(incoming_block.isValid() && outgoing_block.isValid(),
        "the conversation must expose real incoming and outgoing message blocks");
    const auto incoming_alignment = incoming_block.blockFormat().alignment();
    const auto outgoing_alignment = outgoing_block.blockFormat().alignment();
    require((incoming_alignment == Qt::AlignLeft
                && outgoing_alignment == Qt::AlignRight)
            || (incoming_alignment == Qt::AlignRight
                && outgoing_alignment == Qt::AlignLeft),
        "incoming and outgoing message blocks must be oppositely aligned");
    const auto incoming_background = incoming_block.blockFormat().background();
    const auto outgoing_background = outgoing_block.blockFormat().background();
    require(incoming_background.style() != Qt::NoBrush
            && outgoing_background.style() != Qt::NoBrush
            && incoming_background.color() != outgoing_background.color(),
        "incoming and outgoing message blocks must have distinct real backgrounds");

    require(tree_snapshot(project) == fixture_before,
        "opening and selecting the first Agent must never write to the project");

    // Overflow the pane before establishing where the human is scrolled:
    // without genuine overflow, "the pane follows the bottom" below would be
    // vacuously true. Each filler message contributes several wrapped lines
    // via embedded newlines, so a small count already exceeds any plausible
    // panel height.
    for (auto index = 0; index != 120; ++index) {
        const auto minute = 10 + index / 60;
        const auto second = index % 60;
        const auto timestamp = QStringLiteral("2026-08-07T19:%1:%2Z")
            .arg(minute, 2, 10, QLatin1Char('0'))
            .arg(second, 2, 10, QLatin1Char('0'));
        const auto key = QStringLiteral("20260807T19%1%2-fl%3")
            .arg(minute, 2, 10, QLatin1Char('0'))
            .arg(second, 2, 10, QLatin1Char('0'))
            .arg(index, 3, 10, QLatin1Char('0'));
        write_file(mailbox / "inbox" / key.toStdString() / "message.json",
            conversation_envelope("telegram-bot", "human", "",
                "Filler line one.\\nFiller line two.\\nFiller line three.",
                "received_at", timestamp.toStdString()));
    }
    const auto filler_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!state->text().contains(QStringLiteral("122 messages"))
            && std::chrono::steady_clock::now() < filler_deadline) {
        QThread::msleep(50);
        QCoreApplication::processEvents();
    }
    require(state->text().contains(QStringLiteral("122 messages")),
        "the filler fixture must render through the same one-second view "
        "timer before the pane-overflow assertions below are meaningful");

    auto *conversation_scrollbar = surface->verticalScrollBar();
    require(conversation_scrollbar->maximum() > 0,
        "the fixture must genuinely overflow the pane, or the bottom-follow "
        "assertions below would be vacuous");

    // Establish the human at the bottom with an active selection, matching
    // Ted's exact acceptance state before the reply below arrives.
    auto selection_cursor = surface->textCursor();
    selection_cursor.movePosition(QTextCursor::Start);
    selection_cursor.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor, 2);
    surface->setTextCursor(selection_cursor);
    require(surface->textCursor().hasSelection(),
        "test setup must actually create a selection to be meaningful");
    conversation_scrollbar->setValue(conversation_scrollbar->maximum());
    const auto established_value = conversation_scrollbar->value();
    const auto established_max = conversation_scrollbar->maximum();
    require(established_value == established_max && established_value > 0,
        "the human must be established at the bottom of a genuinely "
        "overflowing pane before the append below");

    // A single cheap idle tick with unchanged content must not reset the
    // viewport or destroy the selection.
    QThread::msleep(1200);
    QCoreApplication::processEvents();
    require(conversation_scrollbar->value() == established_value
            && conversation_scrollbar->maximum() == established_max,
        "an idle one-second tick with unchanged content must not move the "
        "scroll position");
    require(surface->textCursor().hasSelection(),
        "an idle one-second tick with unchanged content must not clear the "
        "human's text selection");

    // A new real incoming direct Agent reply, appended after the initial
    // conversation already rendered, with no reselection: only the real
    // one-second view timer can surface it in the message pane.
    write_file(mailbox / "inbox" / "20260807T193000-nr01" / "message.json",
        conversation_envelope("telegram-bot", "human", "Re: \xe5\x9c\xa8\xe5\x90\x97",
            "\xe5\x9c\xa8\xe5\x90\x97\xef\xbc\x9f Yes, awake now.", "received_at",
            "2026-08-07T19:30:00Z"));
    const auto reply_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!surface->toPlainText().contains(QStringLiteral("Yes, awake now."))
            && std::chrono::steady_clock::now() < reply_deadline) {
        QThread::msleep(50);
        QCoreApplication::processEvents();
    }
    require(surface->toPlainText().contains(QStringLiteral("Yes, awake now.")),
        "a new incoming direct reply appended with no reselection must "
        "become visible through the real one-second view timer");
    require(conversation_scrollbar->value() == conversation_scrollbar->maximum(),
        "the newly arrived reply must be visible: the pane must follow the "
        "bottom when the human was already there, not leave the reply below "
        "the fold");

    // A human scrolled above the bottom before a real append must keep that
    // exact non-bottom position: only a human already at the bottom follows
    // the new bottom.
    conversation_scrollbar->setValue(conversation_scrollbar->maximum() / 2);
    const auto scrolled_value = conversation_scrollbar->value();
    require(scrolled_value > 0
            && scrolled_value < conversation_scrollbar->maximum(),
        "the human must be established above the bottom before the "
        "scrolled-up append");
    write_file(mailbox / "inbox" / "20260807T193100-sc01" / "message.json",
        conversation_envelope("telegram-bot", "human", "Re: scrolled up",
            "Scrolled-up reply text.", "received_at", "2026-08-07T19:31:00Z"));
    const auto scrolled_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!surface->toPlainText().contains(
                QStringLiteral("Scrolled-up reply text."))
            && std::chrono::steady_clock::now() < scrolled_deadline) {
        QThread::msleep(50);
        QCoreApplication::processEvents();
    }
    require(surface->toPlainText().contains(
                QStringLiteral("Scrolled-up reply text.")),
        "a scrolled-up append must still surface through the real one-second "
        "view timer");
    require(conversation_scrollbar->value() == scrolled_value,
        "a scrolled-up append must preserve the prior non-bottom position");
    const auto after_reply = tree_snapshot(project);

    // A valid route whose Agent has no mail is an ordinary empty conversation.
    click_agent(window, "issue-643");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("issue-643"),
        "the second Agent in the same project must be selectable");
    require(surface->toPlainText() == QStringLiteral("No messages yet."),
        "a valid route with no rows must say so exactly");

    require(tree_snapshot(project) == after_reply,
        "selecting the second Agent must never write to the project");

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
    auto *input = static_cast<Ui::InputField *>(
        required_child<QObject>(window, "lingtai_composer_input"));
    auto *send_button = static_cast<Ui::RoundButton *>(
        required_child<QObject>(window, "lingtai_composer_send_button"));
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
    require(input->isEnabled() && send_button->isEnabled()
            && input->getLastText().isEmpty(),
        "a selected valid route must enable an empty composer");

    input->setText(QStringLiteral("   \t  "));
    const auto before_whitespace = tree_snapshot(project);
    send_button->clicked(Qt::NoModifier, Qt::LeftButton);
    require(!fs::exists(outbox),
        "whitespace-only input must be rejected without writing anything");
    require(input->getLastText() == QStringLiteral("   \t  "),
        "a rejected whitespace-only send must preserve the typed input");
    require(status->text() != QStringLiteral("Queued"),
        "a rejected whitespace-only send must not claim success");
    require(tree_snapshot(project) == before_whitespace,
        "a rejected whitespace-only send must write nothing");

    input->setText(QStringLiteral("Ted, the slice is complete."));
    send_button->clicked(Qt::NoModifier, Qt::LeftButton);
    require(input->getLastText().isEmpty(),
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

    // Pressing Enter in the nonempty composer must submit through the same
    // send path: queue exactly one addressed outbox leaf and clear the input.
    input->setText(QStringLiteral("Entered via the Return key."));
    auto enter = QKeyEvent(
        QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(input->rawTextEdit(), &enter);
    require(input->getLastText().isEmpty(),
        "pressing Enter in the nonempty composer must clear the input");
    auto enter_leaves = std::vector<fs::path>();
    for (const auto &entry : fs::directory_iterator(outbox)) {
        enter_leaves.push_back(entry.path());
    }
    require(enter_leaves.size() == 2,
        "pressing Enter must queue exactly one more addressed outbox leaf");
    require(std::ranges::any_of(enter_leaves, [](const auto &leaf) {
            const auto body = read_file(leaf / "message.json");
            return body.find("\"to\":[\"telegram-bot\"]") != std::string::npos
                && body.find("Entered via the Return key.") != std::string::npos;
        }),
        "the Enter-queued leaf must address exactly the selected Agent and "
        "carry the typed text");

    // Selection change must not let a later click target the prior Agent.
    click_agent(window, "issue-643");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("issue-643"),
        "the second Agent in the same project must be selectable");
    require(input->getLastText().isEmpty() && status->text().isEmpty(),
        "selecting a different Agent must reset the composer, not carry a draft");
    input->setText(QStringLiteral("A message for the other Agent."));
    send_button->clicked(Qt::NoModifier, Qt::LeftButton);
    require(status->text() == QStringLiteral("Queued"),
        "the send after switching Agents must still succeed");
    auto second_agent_bodies = std::vector<std::string>();
    for (const auto &entry : fs::directory_iterator(outbox)) {
        second_agent_bodies.push_back(read_file(entry.path() / "message.json"));
    }
    require(second_agent_bodies.size() == 3,
        "the three sends must each allocate a fresh leaf");
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
    send_button->clicked(Qt::NoModifier, Qt::LeftButton);
    require(input->getLastText() == QStringLiteral("Should never be queued."),
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

// The Step-4 Agent Activity slice: a separate bounded read-only snapshot of
// the selected Agent's own `logs/events.jsonl`, distinct from the mailbox
// conversation above. Covers no-selection, real selection, a same-selection
// live append becoming visible through the real one-second timer with no
// reselection, and A->B replacement leaving no A text behind.
void verify_agent_activity_panel(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *heading = required_child<QLabel>(
        window, "lingtai_selected_agent_activity_heading");
    auto *surface = required_child<QPlainTextEdit>(
        window, "lingtai_selected_agent_activity");
    auto *state = required_child<QLabel>(
        window, "lingtai_selected_agent_activity_state");

    require(surface->isReadOnly(), "the Activity surface must be read-only");
    require(!surface->accessibleName().isEmpty()
            && !heading->accessibleName().isEmpty()
            && !state->accessibleName().isEmpty(),
        "the Activity surface must be accessible");
    require(surface->objectName() != required_child<QPlainTextEdit>(
                window, "lingtai_selected_agent_conversation")->objectName(),
        "Activity must be a distinct surface from the mailbox conversation");

    const auto project = sandbox / "project";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    const auto target_a = project / ".lingtai/telegram-bot";
    write_file(target_a / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-d0c8",)"
        R"("agent_name":"telegram-bot","nickname":"Telegram Bot",)"
        R"("address":"telegram-bot","state":"active"})");
    const auto target_b = project / ".lingtai/issue-643";
    write_file(target_b / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191610-q001",)"
        R"("agent_name":"issue-643","address":"issue-643","state":"active"})");

    const auto events_a = target_a / "logs/events.jsonl";
    write_file(events_a,
        R"({"type":"diary","text":"AGENT_A_FIRST <b>&amp;</b> not-a-tag"})"
        "\n");
    const auto events_b = target_b / "logs/events.jsonl";
    write_file(events_b, R"({"type":"diary","text":"AGENT_B_ONLY"})" "\n");

    static_cast<void>(shell.open_project(project, std::nullopt));
    require(!surface->toPlainText().contains(QStringLiteral("AGENT_A_FIRST")),
        "no Agent is selected yet, so no Activity text may render");

    click_agent(window, "telegram-bot");
    require(surface->toPlainText().contains(
                QStringLiteral("AGENT_A_FIRST <b>&amp;</b> not-a-tag")),
        "selecting Agent A must render A's own bounded public Activity, "
        "literally as plain text with no markup interpretation");

    // Append a complete new row with no reselection, then wait for the real
    // one-second timer: only a live timer tick can make this visible, and
    // the reader journey alone cannot prove it.
    append_file(events_a, R"({"type":"diary","text":"AGENT_A_APPENDED"})" "\n");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!surface->toPlainText().contains(QStringLiteral("AGENT_A_APPENDED"))
            && std::chrono::steady_clock::now() < deadline) {
        QThread::msleep(50);
        QCoreApplication::processEvents();
    }
    require(surface->toPlainText().contains(QStringLiteral("AGENT_A_APPENDED")),
        "an appended row must become visible through the real one-second "
        "timer with no reselection");

    click_agent(window, "issue-643");
    require(surface->toPlainText().contains(QStringLiteral("AGENT_B_ONLY")),
        "selecting Agent B must render B's own Activity");
    require(!surface->toPlainText().contains(QStringLiteral("AGENT_A")),
        "A's Activity text must never remain visible after selecting B");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "Activity fixtures must be removed");
}

// The Step-5 Request sleep action row: one button plus one status label
// after Activity, before the low-level manifest/status facts. Covers
// no-selection disablement, a real write targeting exactly the selected
// Agent, the immediate "Sleep requested." text, the real one-second timer
// observing a simulated target-side application, A->B->A carrying no stale
// result, and one representative ineligible (stale) selection writing
// nothing.
void verify_request_sleep_action(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *button = required_child<QPushButton>(
        window, "lingtai_selected_agent_request_sleep");
    auto *status = required_child<QLabel>(
        window, "lingtai_selected_agent_sleep_status");

    const auto project = sandbox / "project";
    const auto fresh_heartbeat = [] {
        return std::to_string(std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    };

    write_file(project / ".lingtai/agent-a/.agent.json",
        R"({"admin":{},"state":"idle"})");
    write_file(project / ".lingtai/agent-a/.agent.heartbeat", fresh_heartbeat());

    write_file(project / ".lingtai/agent-b/.agent.json",
        R"({"admin":{},"state":"idle"})");
    write_file(project / ".lingtai/agent-b/.agent.heartbeat", fresh_heartbeat());
    write_file(project / ".lingtai/agent-b/logs/events.jsonl", "");

    write_file(project / ".lingtai/agent-c/.agent.json",
        R"({"admin":{},"state":"idle"})");
    write_file(project / ".lingtai/agent-c/.agent.heartbeat", "0");

    static_cast<void>(shell.open_project(project, std::nullopt));
    require(!button->isEnabled(),
        "Request sleep must be disabled with no Agent selected");
    require(status->text() == QStringLiteral(
                "Select a live Agent that is not already asleep."),
        "the no-selection status must show the concise eligibility reason");

    click_agent(window, "agent-c");
    require(!button->isEnabled(),
        "a stale (non-alive) selection must leave Request sleep disabled");
    require(!fs::exists(project / ".lingtai/agent-c/.sleep"),
        "a stale ineligible selection must never gain a .sleep marker");

    click_agent(window, "agent-a");
    require(button->isEnabled(),
        "an eligible selected Agent must enable Request sleep");

    // The click-boundary refresh must be load-bearing, not decorative: make
    // A ineligible on disk without reselecting, so the cached `agents_` row
    // the button's enabled state was drawn from is now stale. A click must
    // rerun the projection, see the fresh ineligible row, and write nothing
    // -- proving the defensive re-check in handle_request_sleep actually
    // runs rather than trusting the cached snapshot.
    write_file(project / ".lingtai/agent-a/.agent.json",
        R"({"admin":{},"state":"asleep"})");
    const auto stale_cache_before = tree_snapshot(project);
    button->click();
    QCoreApplication::processEvents();
    require(!fs::exists(project / ".lingtai/agent-a/.sleep"),
        "a click whose cached row went stale before the click must never "
        "write a marker");
    require(tree_snapshot(project) == stale_cache_before,
        "a click-boundary rejection must write nothing at all");
    require(status->text() == QStringLiteral(
                "Select a live Agent that is not already asleep."),
        "a click-boundary rejection must show the concise eligibility "
        "reason, not a write status");
    require(!button->isEnabled(),
        "the button must reflect the freshly discovered ineligibility "
        "after the rejected click");

    // Restore A to eligible for the later A<->B selection-clearing checks;
    // this fixture's only remaining job is proving those, not staying
    // asleep.
    write_file(project / ".lingtai/agent-a/.agent.json",
        R"({"admin":{},"state":"idle"})");

    click_agent(window, "agent-b");
    require(button->isEnabled(),
        "the second eligible Agent must also enable Request sleep");
    require(!fs::exists(project / ".lingtai/agent-a/.sleep"),
        "selecting B must never have written A's marker");

    button->click();
    QCoreApplication::processEvents();
    require(fs::exists(project / ".lingtai/agent-b/.sleep"),
        "a click on an eligible selection must write exactly B's marker");
    require(read_file(project / ".lingtai/agent-b/.sleep").empty(),
        "the written marker must be exactly zero bytes");
    require(!fs::exists(project / ".lingtai/agent-a/.sleep"),
        "clicking Request sleep for B must never write A's marker");
    require(status->text() == QStringLiteral("Sleep requested."),
        "the immediate status after a successful write must be exactly "
        "\"Sleep requested.\", not applied or asleep");
    require(!button->isEnabled(),
        "the button must disable itself while an observation is pending");

    // Simulate the target kernel's own canonical behavior: it consumes the
    // marker, updates its own manifest state, and appends its own event.
    write_file(project / ".lingtai/agent-b/.agent.json",
        R"({"admin":{},"state":"asleep"})");
    append_file(project / ".lingtai/agent-b/logs/events.jsonl",
        R"({"type":"sleep_received","source":"signal_file"})" "\n");

    const auto applied_text = QStringLiteral(
        "Sleep request applied. Current state: asleep.");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (status->text() != applied_text
            && std::chrono::steady_clock::now() < deadline) {
        QThread::msleep(50);
        QCoreApplication::processEvents();
    }
    require(status->text() == applied_text,
        "the real one-second timer must observe the appended sleep_received "
        "and show the fresh current manifest state, with no reselection");
    require(!button->isEnabled(),
        "B is now asleep, so the button must stay disabled after the "
        "terminal observation");

    click_agent(window, "agent-a");
    require(button->isEnabled() && status->text().isEmpty(),
        "switching back to the still-eligible A must show a fresh, cleared "
        "status, never B's terminal result");

    click_agent(window, "agent-b");
    require(!button->isEnabled()
            && status->text() == QStringLiteral(
                   "Select a live Agent that is not already asleep."),
        "reselecting the now-asleep B must show fresh ineligibility, not "
        "the stale applied text");

    // Ordinary-message wake: the target kernel later flips B's own manifest
    // back to an eligible state on its own, entirely outside any pending
    // observation. With no reselection, only the same real one-second timer
    // can make Request sleep re-enable for the still-selected B.
    write_file(project / ".lingtai/agent-b/.agent.json",
        R"({"admin":{},"state":"idle"})");
    write_file(project / ".lingtai/agent-b/.agent.heartbeat", fresh_heartbeat());
    const auto wake_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!button->isEnabled()
            && std::chrono::steady_clock::now() < wake_deadline) {
        QThread::msleep(50);
        QCoreApplication::processEvents();
    }
    require(button->isEnabled(),
        "an ordinary-message wake observed with no reselection must "
        "re-enable Request sleep for the still-selected Agent");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "Request sleep fixtures must be removed");
}

// The Step-6 Start Agent action: one explicit, nonblocking start for the
// selected non-human Agent whose current projection is not heartbeat-live.
// Covers a live Agent's total absence of a Start action, immediate pending
// UI with disabled repeat/sleep proven before any responsiveness-blocking
// wait, exact recorded argv proving the selected Agent's own configured
// `venv_path` runtime, a delayed real heartbeat resolving to "Agent is
// online." with normal Request sleep control returning, selection-change
// isolation of a still-pending launch, and the concise ten-second
// no-heartbeat failure with an explicit retry allowed afterward.
void verify_start_agent_action(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *button = required_child<QPushButton>(
        window, "lingtai_selected_agent_start_agent");
    auto *status = required_child<QLabel>(
        window, "lingtai_selected_agent_start_status");
    auto *sleep_button = required_child<QPushButton>(
        window, "lingtai_selected_agent_request_sleep");

    const auto project = sandbox / "project";
    const auto fresh_heartbeat = [] {
        return std::to_string(std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    };

    write_file(project / ".lingtai/agent-live/.agent.json",
        R"({"admin":{},"state":"idle"})");
    write_file(project / ".lingtai/agent-live/.agent.heartbeat",
        fresh_heartbeat());

    const auto success_dir = project / ".lingtai/agent-success";
    write_file(success_dir / ".agent.json", R"({"admin":{},"state":"idle"})");
    const auto success_python = sandbox / "runtime-success/bin/python";
    const auto success_argv = sandbox / "argv-success.txt";
    write_fixture_python(success_python, success_argv,
        success_dir / ".agent.heartbeat", 1);
    write_file(success_dir / "init.json",
        QStringLiteral(R"({"venv_path":"%1"})")
            .arg(path_text(success_python.parent_path().parent_path()))
            .toStdString());

    const auto switch_dir = project / ".lingtai/agent-switch";
    write_file(switch_dir / ".agent.json", R"({"admin":{},"state":"idle"})");
    const auto switch_python = sandbox / "runtime-switch/bin/python";
    const auto switch_argv = sandbox / "argv-switch.txt";
    write_fixture_python(switch_python, switch_argv,
        switch_dir / ".agent.heartbeat", 2);
    write_file(switch_dir / "init.json",
        QStringLiteral(R"({"venv_path":"%1"})")
            .arg(path_text(switch_python.parent_path().parent_path()))
            .toStdString());

    const auto timeout_dir = project / ".lingtai/agent-timeout";
    write_file(timeout_dir / ".agent.json", R"({"admin":{},"state":"idle"})");
    const auto timeout_python = sandbox / "runtime-timeout/bin/python";
    const auto timeout_argv = sandbox / "argv-timeout.txt";
    write_fixture_python(timeout_python, timeout_argv, std::nullopt, 0);
    write_file(timeout_dir / "init.json",
        QStringLiteral(R"({"venv_path":"%1"})")
            .arg(path_text(timeout_python.parent_path().parent_path()))
            .toStdString());

    static_cast<void>(shell.open_project(project, std::nullopt));

    click_agent(window, "agent-live");
    require(!button->isVisible(),
        "a live selected Agent must show no Start action at all");

    click_agent(window, "agent-success");
    require(button->isVisible() && button->isEnabled()
            && status->text().isEmpty(),
        "a stale/missing-heartbeat selected Agent must offer a fresh, "
        "enabled Start action");

    const auto click_start = std::chrono::steady_clock::now();
    button->click();
    QCoreApplication::processEvents();
    const auto click_elapsed = std::chrono::steady_clock::now() - click_start;
    require(click_elapsed < std::chrono::seconds(1),
        "the Start click must return immediately, never blocking the GUI "
        "on the spawned process or the heartbeat wait");
    require(status->text() == QStringLiteral("Starting Agent..."),
        "a successful local start must immediately show the pending wording");
    require(!button->isEnabled(),
        "Start must disable duplicate activation while an observation is pending");
    require(!sleep_button->isEnabled(),
        "Request sleep must stay disabled for a selection with a pending "
        "Start observation");

    const auto argv_deadline_a =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!fs::exists(success_argv)
            && std::chrono::steady_clock::now() < argv_deadline_a) {
        QThread::msleep(20);
    }
    require(fs::exists(success_argv),
        "the fixture runtime must have been invoked");
    require(read_file(success_argv) == QStringLiteral("-m\nlingtai\nrun\n%1\n")
            .arg(path_text(fs::canonical(success_dir))).toStdString(),
        "the exact argv must be the four distinct elements `-m`, "
        "`lingtai`, `run`, <absolute selected Agent dir>, not a joined "
        "string, using the selected Agent's own configured venv_path "
        "runtime");

    const auto online_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (status->text() != QStringLiteral("Agent is online.")
            && std::chrono::steady_clock::now() < online_deadline) {
        QThread::msleep(50);
        QCoreApplication::processEvents();
    }
    require(status->text() == QStringLiteral("Agent is online."),
        "the real one-second timer must observe the delayed fresh heartbeat "
        "through the sole project_agents projection and report success");

    const auto success_log = success_dir / "logs/agent.log";
    const auto log_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!fs::exists(success_log)
            && std::chrono::steady_clock::now() < log_deadline) {
        QThread::msleep(20);
    }
    const auto success_log_contents = read_file(success_log);
    require(success_log_contents.find("lingtai-fixture-stdout-marker")
                != std::string::npos
            && success_log_contents.find("lingtai-fixture-stderr-marker")
                != std::string::npos,
        "the selected Agent's logs/agent.log must actually receive the "
        "spawned process's real stdout and stderr, proving the redirection "
        "both failure messages point users to is genuine, not merely "
        "spawned-and-ignored");
    require(!button->isVisible(),
        "a now-live Agent must return to showing no Start action");
    require(sleep_button->isEnabled(),
        "normal Request sleep control must return once the Agent is online");

    // The success wording must survive further idle ticks, not just the
    // instant it first appears: the ambient per-second refresh (armed once
    // this observation resolves and nothing else is pending) must never
    // silently erase it.
    QThread::msleep(2500);
    QCoreApplication::processEvents();
    require(status->text() == QStringLiteral("Agent is online."),
        "\"Agent is online.\" must persist across idle ambient ticks, not "
        "just the instant it is first observed");

    // Switching selection while a launch is pending must never surface its
    // result under a different selection, and must never kill the detached
    // process: agent-switch's own fixture keeps running in the background
    // the whole time.
    click_agent(window, "agent-switch");
    require(button->isVisible() && button->isEnabled(),
        "agent-switch must start fresh and eligible");
    button->click();
    QCoreApplication::processEvents();
    require(status->text() == QStringLiteral("Starting Agent..."),
        "agent-switch's own click must show its own pending wording");

    click_agent(window, "agent-timeout");
    require(button->isVisible() && button->isEnabled()
            && status->text().isEmpty(),
        "switching away from a pending launch must show a fresh, cleared "
        "status for the newly selected Agent, never the abandoned launch's "
        "wording");

    // Give agent-switch's detached fixture time to actually write its
    // heartbeat while this shell remains parked on a different selection.
    // One check before and one after the wait is enough to prove isolation
    // held throughout; polling the identical assertion on every tick in
    // between proves nothing more.
    const auto isolation_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < isolation_deadline) {
        QThread::msleep(50);
        QCoreApplication::processEvents();
    }
    require(status->text().isEmpty(),
        "the parked selection must never show agent-switch's result while "
        "it is not the current selection");

    click_agent(window, "agent-switch");
    require(!button->isVisible(),
        "re-selecting agent-switch after its abandoned observation's "
        "target actually came online must show the fresh truth, not a "
        "stale 'Starting Agent...' leftover");
    require(status->text().isEmpty(),
        "re-selecting agent-switch must never replay the abandoned "
        "observation's terminal wording");
    require(sleep_button->isEnabled(),
        "agent-switch's real background success must be visible through "
        "fresh projection once reselected");

    // The concise ten-second no-heartbeat failure: agent-timeout's own
    // fixture runtime records its argv but never writes a heartbeat.
    click_agent(window, "agent-timeout");
    require(button->isVisible() && button->isEnabled()
            && status->text().isEmpty(),
        "agent-timeout must remain a fresh, eligible, untouched selection");
    button->click();
    QCoreApplication::processEvents();
    require(status->text() == QStringLiteral("Starting Agent..."),
        "agent-timeout's click must show the pending wording");

    const auto argv_deadline_b =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!fs::exists(timeout_argv)
            && std::chrono::steady_clock::now() < argv_deadline_b) {
        QThread::msleep(20);
    }
    require(fs::exists(timeout_argv),
        "the fixture runtime must have been invoked");
    require(read_file(timeout_argv) == QStringLiteral("-m\nlingtai\nrun\n%1\n")
            .arg(path_text(fs::canonical(timeout_dir))).toStdString(),
        "the no-heartbeat launch must still use the exact four-element "
        "canonical argv, not a joined string");

    const auto timeout_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (status->text() == QStringLiteral("Starting Agent...")
            && std::chrono::steady_clock::now() < timeout_deadline) {
        QThread::msleep(100);
        QCoreApplication::processEvents();
    }
    require(status->text() == QStringLiteral(
                "Agent did not come online. See %1/logs/agent.log.")
                .arg(path_text(fs::canonical(timeout_dir))),
        "a no-heartbeat launch must reach the concise ten-second failure "
        "with the exact Agent log path, never claiming success");
    require(button->isVisible() && button->isEnabled(),
        "the ten-second failure must allow an explicit retry");

    // The failure wording must likewise survive further idle ticks.
    QThread::msleep(2500);
    QCoreApplication::processEvents();
    require(status->text() == QStringLiteral(
                "Agent did not come online. See %1/logs/agent.log.")
                .arg(path_text(fs::canonical(timeout_dir))),
        "the ten-second failure wording must persist across idle ambient "
        "ticks, not just the instant it is first shown");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "Start Agent fixtures must be removed");
}

// The Step-18 read-only selected-Agent Task Card panel: one heading,
// surface, and state label distinct from Agent Activity. Covers exact
// active-body rendering, a changed body refreshing through the real
// one-second timer with no reselection, a transient unavailable
// observation preserving the same target's last valid active projection,
// exact inactive clearing a preserved active body, and selection isolation
// (B must never show A's card).
void verify_agent_task_card_panel(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *heading = required_child<QLabel>(
        window, "lingtai_selected_agent_task_card_heading");
    auto *surface = required_child<QPlainTextEdit>(
        window, "lingtai_selected_agent_task_card");
    auto *state = required_child<QLabel>(
        window, "lingtai_selected_agent_task_card_state");

    require(surface->isReadOnly(), "the Task Card surface must be read-only");
    require(!surface->accessibleName().isEmpty()
            && !heading->accessibleName().isEmpty()
            && !state->accessibleName().isEmpty(),
        "the Task Card surface must be accessible");
    require(surface->objectName() != required_child<QPlainTextEdit>(
                window, "lingtai_selected_agent_activity")->objectName(),
        "Task Card must be a distinct surface from Agent Activity");

    const auto project = sandbox / "project";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    const auto target_a = project / ".lingtai/telegram-bot";
    write_file(target_a / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-d0c8",)"
        R"("agent_name":"telegram-bot","nickname":"Telegram Bot",)"
        R"("address":"telegram-bot","state":"active"})");
    const auto target_b = project / ".lingtai/issue-643";
    write_file(target_b / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191610-q001",)"
        R"("agent_name":"issue-643","address":"issue-643","state":"active"})");

    const auto status_a = target_a / "taskcard" / "status";
    const auto body_a = target_a / "taskcard" / "taskcard.md";
    write_file(status_a, "active");
    write_file(body_a, "TASK_CARD_A_V1");

    static_cast<void>(shell.open_project(project, std::nullopt));
    require(!surface->toPlainText().contains(QStringLiteral("TASK_CARD_A")),
        "no Agent is selected yet, so no Task Card text may render");

    click_agent(window, "telegram-bot");
    require(surface->toPlainText() == QStringLiteral("TASK_CARD_A_V1"),
        "selecting Agent A must render A's own active Task Card body, "
        "literally as plain text");
    require(state->text() == QStringLiteral("Active"),
        "an exact active projection must show the active state label");

    // A changed body must become visible through the real one-second timer
    // with no reselection, mirroring Agent Activity's own append journey.
    write_file(body_a, "TASK_CARD_A_V2");
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (surface->toPlainText() != QStringLiteral("TASK_CARD_A_V2")
                && std::chrono::steady_clock::now() < deadline) {
            QThread::msleep(50);
            QCoreApplication::processEvents();
        }
        require(surface->toPlainText() == QStringLiteral("TASK_CARD_A_V2"),
            "a changed active body must refresh through the real "
            "one-second timer");
    }

    // A transient unavailable observation (an active status with a
    // now-blank body) must preserve the same target's last valid
    // projection rather than clearing or erroring it.
    write_file(body_a, "");
    QThread::msleep(1200);
    QCoreApplication::processEvents();
    require(surface->toPlainText() == QStringLiteral("TASK_CARD_A_V2"),
        "a transient invalid observation must preserve the same target's "
        "last valid active body rather than clearing or erroring it");
    require(state->text() == QStringLiteral("Active"),
        "the preserved last-good projection must keep its active state "
        "label");

    // Exact inactive must clear the preserved active body.
    write_file(status_a, "inactive");
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (state->text() != QStringLiteral("Inactive")
                && std::chrono::steady_clock::now() < deadline) {
            QThread::msleep(50);
            QCoreApplication::processEvents();
        }
        require(state->text() == QStringLiteral("Inactive"),
            "an exact inactive status must clear the preserved active "
            "body through the real one-second timer");
    }
    require(!surface->toPlainText().contains(QStringLiteral("TASK_CARD_A")),
        "an exact inactive projection must never keep showing a preserved "
        "active body as current");

    // Selecting B must never show A's Task Card content.
    click_agent(window, "issue-643");
    require(!surface->toPlainText().contains(QStringLiteral("TASK_CARD_A")),
        "selecting a different Agent must never retain the previous "
        "selection's Task Card content");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "Task Card fixtures must be removed");
}

// The Step-19 read-only selected-Agent Presets summary panel: one heading,
// surface, and state label distinct from both Task Card and Agent Activity.
// Covers exact resolved rendering (ordered allowed refs, independent
// active/default badges, active-effective fields, kernel provenance), a
// changed artifact becoming visible through the real one-second timer with
// no reselection, and selection isolation -- B, with no published artifact,
// must never show A's summary and must show "Not yet published".
void verify_agent_preset_summary_panel(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *heading = required_child<QLabel>(
        window, "lingtai_selected_agent_preset_summary_heading");
    auto *surface = required_child<QPlainTextEdit>(
        window, "lingtai_selected_agent_preset_summary");
    auto *state = required_child<QLabel>(
        window, "lingtai_selected_agent_preset_summary_state");

    require(surface->isReadOnly(), "the Presets surface must be read-only");
    require(!surface->accessibleName().isEmpty()
            && !heading->accessibleName().isEmpty()
            && !state->accessibleName().isEmpty(),
        "the Presets surface must be accessible");
    require(surface->objectName() != required_child<QPlainTextEdit>(
                window, "lingtai_selected_agent_task_card")->objectName(),
        "Presets must be a distinct surface from Task Card");

    const auto project = sandbox / "project";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    const auto target_a = project / ".lingtai/telegram-bot";
    write_file(target_a / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-d0c8",)"
        R"("agent_name":"telegram-bot","nickname":"Telegram Bot",)"
        R"("address":"telegram-bot","state":"active"})");
    const auto target_b = project / ".lingtai/issue-643";
    write_file(target_b / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191610-q001",)"
        R"("agent_name":"issue-643","address":"issue-643","state":"active"})");

    const auto artifact_a = target_a / "system" / "manifest.resolved.json";
    const auto resolved_v1 = std::string_view(R"JSON({
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
    write_file(artifact_a, resolved_v1);

    static_cast<void>(shell.open_project(project, std::nullopt));
    require(!surface->toPlainText().contains(QStringLiteral("codex.json")),
        "no Agent is selected yet, so no Presets text may render");

    click_agent(window, "telegram-bot");
    const auto expected_v1 = QStringLiteral(
        "Active:  ~/.lingtai-tui/presets/saved/codex.json\n"
        "Default: ~/.lingtai-tui/presets/saved/codex.json\n"
        "Allowed:\n"
        "  • ~/.lingtai-tui/presets/saved/deepseek_flash.json\n"
        "  • [Active, Default] ~/.lingtai-tui/presets/saved/codex.json\n"
        "  • ~/.lingtai-tui/presets/saved/zhipu-1.json\n"
        "\n"
        "Active effective\n"
        "  Provider: codex\n"
        "  Model: gpt-5.6-sol\n"
        "  Context limit: 250000\n"
        "  Capabilities: avatar, shell, web\n"
        "\n"
        "Source: kernel · generated 2026-08-13T19:53:34Z");
    require(surface->toPlainText() == expected_v1,
        "selecting Agent A must render its exact ordered allowed refs, "
        "independent active/default badges, active-effective fields, and "
        "kernel provenance");
    require(state->text() == QStringLiteral("Resolved"),
        "a supported complete v1 artifact must show the Resolved state "
        "label");

    // A changed artifact must become visible through the real one-second
    // timer with no reselection.
    const auto resolved_v2 = std::string_view(R"JSON({
      "schema": "lingtai.manifest.resolved/v1",
      "schema_version": 1,
      "source": "kernel",
      "generated_at": "2026-08-13T20:10:00Z",
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
    write_file(artifact_a, resolved_v2);
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!surface->toPlainText().contains(
                    QStringLiteral("2026-08-13T20:10:00Z"))
                && std::chrono::steady_clock::now() < deadline) {
            QThread::msleep(50);
            QCoreApplication::processEvents();
        }
        require(surface->toPlainText().contains(
                    QStringLiteral("2026-08-13T20:10:00Z")),
            "a changed artifact must refresh through the real one-second "
            "timer");
    }

    // Selecting B, which has no published artifact, must never show A's
    // summary and must show the Not yet published state.
    click_agent(window, "issue-643");
    require(!surface->toPlainText().contains(QStringLiteral("codex.json")),
        "selecting a different Agent must never retain the previous "
        "selection's Presets content");
    require(state->text() == QStringLiteral("Not yet published"),
        "a selected Agent with no published resolved artifact must show "
        "the Not yet published state, never a stale carry-over");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "Presets fixtures must be removed");
}

// Writes one executable POSIX fixture executable used only by the Commit-22
// New Project journey. It records its exact separate argv, then dispatches
// on `$1` (`presets` or `spawn <dir> --preset <name>`) to the two caller-
// provided shell fragments. It never runs a real TUI and never touches a
// real project/Agent/config/credential.
void write_fixture_tui(
        const fs::path &tui_path,
        const fs::path &argv_record,
        std::string_view presets_fragment,
        std::string_view spawn_fragment) {
    auto script = std::string("#!/bin/sh\nprintf '%s\\n' \"$@\" >> \"")
        + argv_record.string() + "\"\n"
        + "if [ \"$1\" = \"presets\" ]; then\n"
        + std::string(presets_fragment) + "\nfi\n"
        + "if [ \"$1\" = \"spawn\" ]; then\n"
        + std::string(spawn_fragment) + "\nfi\n"
        + "exit 2\n";
    write_file(tui_path, script);
    std::error_code error;
    fs::permissions(tui_path,
        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
            | fs::perms::others_read | fs::perms::others_exec,
        fs::perm_options::replace, error);
    require(!error, "fixture TUI must be made executable");
}

std::string fixture_tui_argv(const std::vector<std::string> &args) {
    auto text = std::string();
    for (const auto &arg : args) {
        text += arg + "\n";
    }
    return text;
}

// The Commit-22 journey: a user starting with no project can explicitly
// create a new project and its first Agent through the canonical TUI
// headless `presets`/`spawn` surface. The smallest native assertion first:
// the no-project window must expose a visible `New Project` action beside
// `Open Project`.
void verify_first_project_bootstrap(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *new_project_button = required_child<QPushButton>(
        window, "lingtai_new_project_button");
    require(new_project_button->isVisible(),
        "no-project state must expose a visible New Project action");
    require(new_project_button->text() == QStringLiteral("New Project\u2026"),
        "new project affordance text changed");
    require(new_project_button->accessibleName() == "New Project",
        "new project affordance needs a static accessible name");
    auto *open_button = required_child<QPushButton>(
        window, "lingtai_open_project_button");
    require(open_button->isVisible(),
        "no-project state must keep the Open Project action visible");
    auto *status = required_child<QLabel>(
        window, "lingtai_bootstrap_status");
    auto *dialog = required_child<QDialog>(
        window, "lingtai_new_project_dialog");
    auto *destination_input = required_child<QLineEdit>(
        window, "lingtai_bootstrap_destination_input");
    auto *preset_chooser = required_child<QComboBox>(
        window, "lingtai_bootstrap_preset_chooser");
    auto *create_start = required_child<QPushButton>(
        window, "lingtai_bootstrap_create_start");
    required_child<QPushButton>(window, "lingtai_bootstrap_cancel");
    auto *dialog_status = required_child<QLabel>(
        window, "lingtai_bootstrap_dialog_status");
    auto *browse_button = required_child<QPushButton>(
        window, "lingtai_bootstrap_destination_browse");
    auto *dialog_note = required_child<QLabel>(
        window, "lingtai_bootstrap_dialog_note");
    require(browse_button->text() == QStringLiteral("Browse\u2026"),
        "destination row must offer a Browse affordance");
    require(create_start->text() == QStringLiteral("Create & Start"),
        "the committing dialog action must be explicitly Create & Start");
    require(dialog_note->text().contains(QStringLiteral("first Agent")),
        "the dialog note must truthfully state the first-Agent naming rule");

    const auto argv_record = sandbox / "tui-argv.txt";
    fs::create_directories(sandbox);
    const auto destination = fs::canonical(sandbox) / "created-project";
    const auto success_tui = sandbox / "tui-success";
    // A real fixture process: records its exact argv; `presets` prints the
    // current two-entry JSON contract and `spawn <dir> --preset <name>`
    // creates a minimal valid returned project and emits valid launch JSON.
    write_fixture_tui(success_tui, argv_record,
        R"(printf '%s' '{"presets":[{"name":"alpha","description":"Alpha preset","tier":"t1","source":"template","path":"/tmp/a.json"},{"name":"beta","description":"Beta preset","tier":"t2","source":"saved","path":"/tmp/b.json"}]}'
exit 0)",
        R"(mkdir -p "$2/.lingtai/agent"
printf '%s' '{"admin":{}}' > "$2/.lingtai/agent/.agent.json"
printf '%s' "{\"status\":\"ready\",\"project_dir\":\"$2\",\"agent_name\":\"agent\",\"agent_dir\":\"$2/.lingtai/agent\",\"preset\":\"$4\",\"recipe\":\"plain\",\"pid\":0}"
exit 0)");
    shell.set_tui_executable(success_tui);

    // Evidence 1: no-project window exposes New Project; clicking it runs the
    // exact separate argv `presets` and keeps the UI responsive/pending.
    auto open_requests = std::size_t{0};
    shell.set_open_project_request_handler([&] { ++open_requests; });
    new_project_button->click();
    require(status->text() == QStringLiteral("Discovering presets…"),
        "a pending discovery must show one truthful phase status");
    require(!new_project_button->isEnabled() && !open_button->isEnabled(),
        "duplicate New Project and Open Project must be disabled while pending");
    QCoreApplication::processEvents();
    const auto presets_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (read_file(argv_record) != fixture_tui_argv({"presets"})
            && std::chrono::steady_clock::now() < presets_deadline) {
        QThread::msleep(20);
    }
    require(read_file(argv_record) == fixture_tui_argv({"presets"}),
        "clicking New Project must run the exact separate argv `presets`");
    open_button->click();
    require(open_requests == 0,
        "Open Project must not fire while New Project is pending");

    // Evidence 2: valid preset JSON populates the dialog; dismissing it via
    // the real QDialog rejected path (standard window close control / Escape)
    // must be the same no-spawn cancellation and must re-enable actions.
    const auto dialog_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!dialog->isVisible()
            && std::chrono::steady_clock::now() < dialog_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    require(dialog->isVisible(),
        "valid preset discovery must show the New Project dialog");
    require(preset_chooser->count() == 2
            && preset_chooser->itemText(0) == "alpha"
            && preset_chooser->itemText(1) == "beta",
        "the preset chooser must list the returned preset names");
    dialog->reject();
    QCoreApplication::processEvents();
    require(!dialog->isVisible() && new_project_button->isEnabled()
            && open_button->isEnabled(),
        "dismissing the dialog via reject() must close it and re-enable both "
        "actions");
    require(read_file(argv_record) == fixture_tui_argv({"presets"}),
        "dismissing the dialog must perform no spawn at all");

    // Evidence 3: a destination plus the selected non-first preset and
    // Create & Start produces the exact separate spawn argv, with duplicate
    // New/Open actions staying disabled while pending.
    new_project_button->click();
    QCoreApplication::processEvents();
    while (!dialog->isVisible()
            && std::chrono::steady_clock::now() < dialog_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    require(dialog->isVisible(), "the second discovery must reopen the dialog");
    create_start->click();
    QCoreApplication::processEvents();
    require(dialog_status->text().contains(QStringLiteral("nonempty")),
        "Create & Start with no destination must refuse with a concise "
        "dialog status");
    destination_input->setText(path_text(destination));
    preset_chooser->setCurrentIndex(1); // beta: non-first preset
    create_start->click();
    require(status->text() == QStringLiteral(
                "Creating project and starting Agent…"),
        "a pending spawn must show one truthful phase status");
    require(!new_project_button->isEnabled() && !open_button->isEnabled(),
        "duplicate New/Open actions must stay disabled while spawn is pending");
    QCoreApplication::processEvents();
    const auto spawn_argv = std::vector<std::string>{
        "spawn", path_text(destination).toStdString(),
        "--preset", "beta"};
    const auto spawn_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (read_file(argv_record)
                != fixture_tui_argv({"presets", "presets"})
                    + fixture_tui_argv(spawn_argv)
            && std::chrono::steady_clock::now() < spawn_deadline) {
        QThread::msleep(20);
    }
    require(read_file(argv_record)
            == fixture_tui_argv({"presets", "presets"})
                + fixture_tui_argv(spawn_argv),
        "Create & Start must run the exact separate argv `spawn <destination> "
        "--preset beta` with no shell joining");

    // Evidence 4: fixture success creates a minimal valid returned project,
    // emits valid launch JSON, and Desktop attaches that exact returned
    // project and reports created+started.
    const auto attached_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (status->text() != QStringLiteral("Project created and Agent started.")
            && std::chrono::steady_clock::now() < attached_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    require(status->text() == QStringLiteral(
                "Project created and Agent started."),
        "spawn success must report the concise created-and-started status");
    require(shell.selection_state().active_project()
            && shell.selection_state().active_project()->root()
                == fs::canonical(destination),
        "Desktop must attach the exact returned project directory");
    require(!dialog->isVisible(),
        "a successful spawn must close the New Project dialog");

    // Evidence 5: a nonzero structured spawn failure must leave the currently
    // attached project unchanged, re-enable actions, and show the structured
    // error plus a generic partial-state warning.
    const auto fail_argv_record = sandbox / "tui-fail-argv.txt";
    const auto fail_tui = sandbox / "tui-fail";
    write_fixture_tui(fail_tui, fail_argv_record,
        R"(printf '%s' '{"presets":[{"name":"alpha","description":"Alpha preset","tier":"t1","source":"template","path":"/tmp/a.json"},{"name":"beta","description":"Beta preset","tier":"t2","source":"saved","path":"/tmp/b.json"}]}'
exit 0)",
        R"(printf '%s\n' 'warning: recipe copy: fixture recipe copy failed' >&2
printf '%s\n' '{' >&2
printf '%s\n' '  "error": "fixture spawn refused",' >&2
printf '%s\n' '  "code": "launch_failed"' >&2
printf '%s\n' '}' >&2
exit 7)");
    shell.set_tui_executable(fail_tui);
    const auto attached_root = fs::canonical(destination);
    new_project_button->click();
    QCoreApplication::processEvents();
    while (!dialog->isVisible()
            && std::chrono::steady_clock::now() < dialog_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    require(dialog->isVisible(),
        "the failing fixture's discovery must still show the dialog");
    destination_input->setText(path_text(sandbox / "partial-destination"));
    preset_chooser->setCurrentIndex(0);
    create_start->click();
    QCoreApplication::processEvents();
    const auto failure_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!status->text().contains(QStringLiteral("launch_failed"))
            && std::chrono::steady_clock::now() < failure_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    require(status->text().contains(QStringLiteral("launch_failed"))
            && status->text().contains(
                QStringLiteral("fixture spawn refused"))
            && status->text().contains(QStringLiteral(
                "partially initialized LingTai project")),
        "a structured spawn failure must surface the code and error plus the "
        "generic partial-state warning");
    require(shell.selection_state().active_project()
            && shell.selection_state().active_project()->root() == attached_root,
        "a spawn failure must leave the currently attached project unchanged");
    require(new_project_button->isEnabled() && open_button->isEnabled(),
        "a spawn failure must re-enable both actions");
    require(!dialog->isVisible(),
        "a failed spawn must close the New Project dialog");
    require(fs::exists(fail_argv_record)
            && read_file(fail_argv_record)
                == fixture_tui_argv({"presets"})
                    + fixture_tui_argv({"spawn",
                        path_text(sandbox / "partial-destination").toStdString(),
                        "--preset", "alpha"}),
        "the failing spawn must still run the exact separate spawn argv");

    // Evidence 6: one malformed preset-list case fails closed before any
    // dialog or spawn.
    const auto malformed_argv_record = sandbox / "tui-malformed-argv.txt";
    const auto malformed_tui = sandbox / "tui-malformed";
    write_fixture_tui(malformed_tui, malformed_argv_record,
        R"(printf '%s' '{this is not json'
exit 0)",
        R"(exit 9)");
    shell.set_tui_executable(malformed_tui);
    new_project_button->click();
    QCoreApplication::processEvents();
    const auto malformed_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (read_file(malformed_argv_record) != fixture_tui_argv({"presets"})
            && std::chrono::steady_clock::now() < malformed_deadline) {
        QThread::msleep(20);
    }
    require(read_file(malformed_argv_record) == fixture_tui_argv({"presets"}),
        "the malformed-presets fixture must be invoked with the exact argv");
    const auto fail_closed_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!status->text().contains(QStringLiteral("could not be used"))
            && std::chrono::steady_clock::now() < fail_closed_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    require(status->text().contains(QStringLiteral("could not be used")),
        "malformed preset output must fail closed with one concise failure");
    require(!dialog->isVisible() && new_project_button->isEnabled()
            && open_button->isEnabled(),
        "a malformed preset list must never show the dialog and must "
        "re-enable actions");
    require(read_file(malformed_argv_record) == fixture_tui_argv({"presets"}),
        "a malformed preset list must never reach spawn");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "bootstrap fixtures must be removed");
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
    require(sidebar->width() == 260,
        "sidebar must remain bounded");
    require(sidebar->height() == content->height()
            && sidebar->height() == body->height(),
        "sidebar and content must fill the body");
    require(sidebar->geometry().right() < content->geometry().left(),
        "sidebar and content must be distinct regions");

    window.resize(1200, 800);
    QCoreApplication::processEvents();
    require(sidebar->width() == 260,
        "sidebar width must stay bounded after resize");
    require(content->width() > narrow_content_width,
        "content region must absorb added window width");
    require(sidebar->height() == content->height()
            && sidebar->height() == body->height(),
        "both regions must continue filling the resized body");
}

// The Commit-24 shell slice: one persistent 260px left project/Agent list
// column replaces the action-only rail and the nested roster route. The
// roster is the left column's own content (never a second list inside the
// right content route), its rows are a fixed 62px with one primary name line
// plus one compact secondary/state line, a plain-shadow separator divides
// list from content, the compact project actions stay reachable, selection
// still drives the same right detail, and an unchanged projection refresh
// must not rebuild the row tree so selection/focus/scroll survive.
void verify_persistent_roster_shell(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *sidebar = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_sidebar");
    auto *content = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_content");
    auto *open_button = required_child<QPushButton>(
        window, "lingtai_open_project_button");
    auto *new_button = required_child<QPushButton>(
        window, "lingtai_new_project_button");

    // Compact project actions must be reachable even before any project opens.
    require(open_button->isVisible() && new_button->isVisible(),
        "compact project actions must be reachable in the left column");

    const auto project = sandbox / "project";
    write_file(project / ".lingtai/alpha/.agent.json", R"({"admin":{}})");
    write_file(project / ".lingtai/beta/.agent.json", R"({"admin":{}})");
    for (auto index = 0; index != 12; ++index) {
        write_file(project / ".lingtai"
                / ("z-extra-" + std::to_string(index)) / ".agent.json",
            R"({"admin":{}})");
    }
    const auto fixture_before = tree_snapshot(project);
    const auto outcome = shell.open_project(project, std::nullopt);
    require(tree_snapshot(project) == fixture_before,
        "opening the roster-shell fixture must remain read-only");
    require(outcome.disposition == ProjectOpenDisposition::opened,
        "the roster-shell fixture project must open");

    // The persistent left list column is exactly 260px wide when a project is
    // open, and fills the body height.
    require(sidebar->width() == 260,
        "the persistent left list column must be 260px wide");
    require(sidebar->height() == content->height(),
        "the persistent left column must fill the body height");

    // The roster is the left column's own content, never a second nested list
    // inside the right content route.
    require(content->findChild<Ui::RpWidget *>("lingtai_agent_roster") == nullptr,
        "the Agent roster must not be nested inside the right content route");
    auto *roster = required_child<Ui::RpWidget>(window, "lingtai_agent_roster");
    require(sidebar->findChild<Ui::RpWidget *>("lingtai_agent_roster") == roster,
        "the Agent roster must be a child of the persistent left column");

    // A plain-shadow separator divides the list column from the content pane.
    auto *separator_widget = required_child<Ui::RpWidget>(
        window, "lingtai_roster_separator");
    auto *separator = dynamic_cast<Ui::PlainShadow *>(separator_widget);
    require(separator != nullptr,
        "the left/right separator must be a Ui::PlainShadow");
    require(separator->isVisible(), "the left/right separator must be visible");
    require(separator->geometry().left() >= sidebar->geometry().right() - 1
            && separator->geometry().left() <= content->geometry().left(),
        "the separator must sit between the left column and the content pane");

    // Rows: a fixed 62px tall, with exactly one primary name line plus one
    // compact secondary/state line.
    auto *row = agent_row(window, "alpha");
    require(row->minimumHeight() == 62 && row->maximumHeight() == 62,
        "Agent rows must be a fixed 62px tall");
    require(row->text().split(QLatin1Char('\n')).size() == 2,
        "each Agent row must show one primary name line plus one compact "
        "secondary/state line");

    // Agent selection still drives the same right detail.
    click_agent(window, "alpha");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("alpha")
            && row->isChecked()
            && label_text(window, "lingtai_selected_agent_presentation_name")
                == QStringLiteral("alpha")
            && label_text(window, "lingtai_selected_agent_key")
                == QStringLiteral("role: agent · presence: missing"),
        "Agent selection must still drive the same right detail with a "
        "key-fallback header that never repeats the directory key");

    // An unchanged projection refresh must not rebuild the row tree, so the
    // selected row keeps its identity, selected state, scroll, and focus.
    auto *row_before = agent_row(window, "alpha");
    auto *scroll = required_child<QScrollArea>(
        window, "lingtai_agent_roster_scroll");
    QCoreApplication::processEvents();
    auto *scroll_bar = scroll->verticalScrollBar();
    require(row_before->focusPolicy() == Qt::StrongFocus,
        "the selected row must remain keyboard-focusable");
    require(scroll_bar->maximum() > 0,
        "the tall roster must expose a nonzero scroll range");
    scroll_bar->setValue(scroll_bar->maximum());
    const auto scroll_before = scroll_bar->value();

    const auto refreshed = shell.open_project(project, std::nullopt);
    QCoreApplication::processEvents();
    require(refreshed.disposition == ProjectOpenDisposition::opened
            && agent_row(window, "alpha") == row_before
            && row_before->isChecked()
            && row_before->focusPolicy() == Qt::StrongFocus,
        "an unchanged projection refresh must preserve the selected row's "
        "identity, selected state, and focus eligibility");
    require(scroll_bar->value() == scroll_before,
        "an unchanged projection refresh must preserve roster scroll");
    require(tree_snapshot(project) == fixture_before,
        "the unchanged refresh must remain read-only");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "roster-shell fixtures must be removed");
}

} // namespace

struct DashboardSectionShape {
    Ui::RpWidget *owner = nullptr;
    QLayout *layout = nullptr;
    QLabel *heading = nullptr;
    QPlainTextEdit *surface = nullptr;
    QLabel *state = nullptr;
    Ui::PlainShadow *separator = nullptr;
};

// Captures one of the three shared selected-Agent source sections by its
// `lingtai_selected_agent_<kind>` family of object names.
DashboardSectionShape dashboard_section(QWidget &window, const char *kind) {
    const auto base = QStringLiteral("lingtai_selected_agent_")
        + QString::fromLatin1(kind);
    auto result = DashboardSectionShape{};
    result.owner = required_child<Ui::RpWidget>(
        window, (base + QStringLiteral("_section")).toUtf8().constData());
    result.layout = result.owner->layout();
    require(result.layout != nullptr,
        "every dashboard section must own one inner layout");
    result.heading = required_child<QLabel>(
        *result.owner,
        (base + QStringLiteral("_heading")).toUtf8().constData());
    result.surface = required_child<QPlainTextEdit>(
        *result.owner, base.toUtf8().constData());
    result.state = required_child<QLabel>(
        *result.owner, (base + QStringLiteral("_state")).toUtf8().constData());
    auto *separator_widget = required_child<Ui::RpWidget>(
        *result.owner, (base + QStringLiteral("_separator")).toUtf8().constData());
    result.separator = dynamic_cast<Ui::PlainShadow *>(separator_widget);
    require(result.separator != nullptr,
        "each dashboard section must use the same Ui::PlainShadow separator");
    return result;
}

// The Commit-28 cut: the three selected-Agent read-only sources (Activity,
// Task Card, Presets) are presented through one shared local section framing,
// and the Start/Sleep action region keeps stable geometry when the selected
// Agent switches between heartbeat-live (no Start action) and start-eligible
// stale/missing (Start action present).
void verify_selected_agent_dashboard_layout(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    const auto activity = dashboard_section(window, "activity");
    const auto task_card = dashboard_section(window, "task_card");
    const auto preset_summary = dashboard_section(window, "preset_summary");

    for (const auto *section : { &activity, &task_card, &preset_summary }) {
        require(section->heading->parent() == section->owner
                && section->surface->parent() == section->owner
                && section->state->parent() == section->owner
                && section->separator->parent() == section->owner,
            "each dashboard section must directly own its heading, surface, "
            "state, and separator");
        require(section->heading->font().weight() == QFont::DemiBold,
            "each dashboard heading must stay semibold");
        require(section->surface->isReadOnly(),
            "each dashboard surface must stay read-only");
        require(!section->heading->accessibleName().isEmpty()
                && !section->surface->accessibleName().isEmpty()
                && !section->state->accessibleName().isEmpty(),
            "each dashboard section must stay accessible");
    }

    const auto section_margins = activity.layout->contentsMargins();
    const auto section_spacing = activity.layout->spacing();
    require(task_card.layout->contentsMargins() == section_margins
            && preset_summary.layout->contentsMargins() == section_margins,
        "all dashboard sections must share one structural margin treatment");
    require(task_card.layout->spacing() == section_spacing
            && preset_summary.layout->spacing() == section_spacing,
        "all dashboard sections must share one structural spacing treatment");

    const auto surface_minimum = activity.surface->minimumHeight();
    require(task_card.surface->minimumHeight() == surface_minimum
            && preset_summary.surface->minimumHeight() == surface_minimum,
        "all dashboard surfaces must share one consistent minimum height");

    const auto separator_minimum = activity.separator->minimumHeight();
    require(separator_minimum > 0
            && separator_minimum == activity.separator->maximumHeight(),
        "each dashboard separator must be one fixed thin line");
    require(task_card.separator->minimumHeight() == separator_minimum
            && preset_summary.separator->minimumHeight() == separator_minimum,
        "all dashboard separators must share one structural treatment");

    const auto project = sandbox / "project";
    const auto fresh_heartbeat = [] {
        return std::to_string(std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    };
    write_file(project / ".lingtai/agent-aa/.agent.json",
        R"({"admin":{},"state":"idle"})");
    write_file(project / ".lingtai/agent-aa/.agent.heartbeat",
        fresh_heartbeat());
    write_file(project / ".lingtai/agent-bb/.agent.json",
        R"({"admin":{},"state":"idle"})");
    write_file(project / ".lingtai/agent-bb/.agent.heartbeat",
        std::to_string(std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count()
            - 3600.0));

    static_cast<void>(shell.open_project(project, std::nullopt));

    auto *start_button = required_child<QPushButton>(
        window, "lingtai_selected_agent_start_agent");
    auto *start_row = required_child<Ui::RpWidget>(
        window, "lingtai_selected_agent_start_row");
    auto *sleep_row = required_child<Ui::RpWidget>(
        window, "lingtai_selected_agent_sleep_row");
    auto *manifest_identity = required_child<QLabel>(
        window, "lingtai_selected_agent_manifest_identity");

    click_agent(window, "agent-aa");
    QCoreApplication::processEvents();
    require(!start_button->isVisible(),
        "a heartbeat-live selected Agent must show no Start action at all");
    const auto live_region_height = start_row->height();
    const auto live_anchor = manifest_identity->geometry().top();
    const auto live_sleep_top = sleep_row->geometry().top();

    click_agent(window, "agent-bb");
    QCoreApplication::processEvents();
    require(start_button->isVisible() && start_button->isEnabled(),
        "a start-eligible stale selected Agent must offer the Start action");
    require(start_row->height() == live_region_height,
        "the Start action region must keep stable geometry when the Start "
        "button appears or disappears");
    require(sleep_row->geometry().top() == live_sleep_top,
        "the Request sleep row must keep stable geometry when the Start "
        "button appears or disappears");
    require(manifest_identity->geometry().top() == live_anchor,
        "downstream detail content must not jump when the Start button "
        "appears or disappears");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "dashboard fixtures must be removed");
}

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
        verify_persistent_roster_shell(
            shell, project_root / "commit-24-roster-shell-fixture");
        verify_first_project_bootstrap(
            shell, project_root / "commit-22-bootstrap-fixture");
        verify_open_project_behavior(
            shell, project_root / "commit-7-open-project-fixtures");
        verify_selected_agent_conversation(
            shell, project_root / "commit-13-conversation-fixture");
        verify_composer_send_behavior(
            shell, project_root / "commit-14-composer-fixture");
        verify_agent_activity_panel(
            shell, project_root / "commit-15-activity-fixture");
        verify_request_sleep_action(
            shell, project_root / "commit-16-sleep-fixture");
        verify_start_agent_action(
            shell, project_root / "commit-17-start-fixture");
        verify_agent_task_card_panel(
            shell, project_root / "commit-18-task-card-fixture");
        verify_agent_preset_summary_panel(
            shell, project_root / "commit-19-preset-summary-fixture");
        verify_layout(shell);
        verify_selected_agent_dashboard_layout(
            shell, project_root / "commit-28-dashboard-fixture");
        std::cout << "native shell behavior: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "native shell behavior: " << error.what() << '\n';
        return 1;
    }
}
