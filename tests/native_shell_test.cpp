#include "native_shell.h"

#include "styles/palette.h"
#include "ui/platform/mac/ui_window_title_mac.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/rp_window.h"
#include "ui/widgets/shadow.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtCore/QThread>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QFontMetrics>
#include <QtGui/QImage>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPalette>
#include <QtGui/QPixmap>
#include <QtGui/QStyleHints>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtGui/QTextFormat>
#include <QtGui/QTextLayout>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpacerItem>
#include <QtWidgets/QTextEdit>

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

// The vendored lib_ui controls (InputField, RoundButton, FlatLabel) carry no
// Q_OBJECT macro, so Qt's templated findChild<Ui::X *> cannot name them. The
// composer already resolves them through a QObject lookup plus a cast. These
// classes are polymorphic, so dynamic_cast also stays safe on raw Qt
// production: when the object is still a QLineEdit/QPushButton/QLabel it
// resolves to null and the require below reports the missing semantic type
// instead of dereferencing a mis-typed pointer.
template <typename Widget>
Widget *required_ui_child(QWidget &root, const char *object_name) {
    auto *result = dynamic_cast<Widget *>(
        required_child<QObject>(root, object_name));
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
// Start Agent journeys below. It never runs a real interpreter: it records
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

// Writes one fake `lingtai-tui control` executable used only by the U5
// lifecycle-command journey. It never runs a real TUI or Agent: it records
// its exact separate argv to a file, then -- only after a short deterministic
// delay -- emits one canonical success JSON object on stdout and writes a
// completion marker, exactly like a real headless control command would
// eventually answer. The emitted agent stays `alpha` because the journey only
// ever runs the control command under the alpha Agent.
void write_fixture_control(
        const fs::path &executable_path,
        const fs::path &argv_record_path,
        const fs::path &done_marker_path,
        int delay_seconds) {
    auto script = std::string("#!/bin/sh\nprintf '%s\\n' \"$@\" >> \"")
        + argv_record_path.string() + "\"\n"
        + "sleep " + std::to_string(delay_seconds) + "\n"
        + "printf '%s\\n' '{\"command\":\"refresh\",\"agent\":\"alpha\","
          "\"status\":\"signaled\"}'\n"
        + "printf 'done\\n' > \"" + done_marker_path.string() + "\"\n"
        + "exit 0\n";
    write_file(executable_path, script);
    std::error_code error;
    fs::permissions(executable_path,
        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
            | fs::perms::others_read | fs::perms::others_exec,
        fs::perm_options::replace, error);
    require(!error, "fixture control executable must be made executable");
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

// The first-project surfaces now speak the same lib_ui label language as the
// accepted composer/dashboard: their readable text is the FlatLabel's own
// accessibility text, read back through accessibilityName().
QString flat_label_text(QWidget &window, const char *object_name) {
    return required_ui_child<Ui::FlatLabel>(window, object_name)
        ->accessibilityName();
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

void click_first_agent_canvas_row(QWidget &window) {
    auto *canvas = required_child<QWidget>(
        window, "lingtai_agent_roster_rows");
    const auto point = QPointF(20.0, 24.0);
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
        Qt::NoButton,
        Qt::NoModifier);
    QApplication::sendEvent(canvas, &release);
    QCoreApplication::processEvents();
}

void verify_dark_application_palette_inheritance(const fs::path &sandbox) {
    // Capture the harness's original color scheme and application palette
    // before any mutation and restore both on every exit. The later
    // ScopedApplicationPalette only snapshots the post-mutation palette, so
    // without this guard the function would leak the Dark scheme and palette
    // into the following journey.
    const auto original_color_scheme =
        QGuiApplication::styleHints()->colorScheme();
    const auto original_palette = QApplication::palette();
    struct RestoreSchemeAndPalette final {
        Qt::ColorScheme scheme;
        QPalette palette;
        ~RestoreSchemeAndPalette() {
            QGuiApplication::styleHints()->setColorScheme(scheme);
            QApplication::setPalette(palette);
        }
    } restore{original_color_scheme, original_palette};

    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
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
    require(st::windowBg->c == QColor(QStringLiteral("#17212b"))
            && st::windowFg->c == QColor(QStringLiteral("#f5f5f5"))
            && st::dialogsBgActive->c == QColor(QStringLiteral("#2b5278"))
            && st::msgInBg->c == QColor(QStringLiteral("#182533"))
            && st::msgOutBg->c == QColor(QStringLiteral("#2b5278")),
        "a system-dark startup must select Telegram's canonical night palette");
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
    // The no-project route, open-error and bootstrap-status surfaces are the
    // first-project presentation slice: they now use the same lib_ui label
    // language as the accepted dashboard, exposed through the FlatLabel's
    // accessibility text.
    const auto flat_labels = std::vector<Ui::FlatLabel *>{
        required_ui_child<Ui::FlatLabel>(window, "lingtai_no_project_title"),
        required_ui_child<Ui::FlatLabel>(window, "lingtai_no_project_detail"),
        required_ui_child<Ui::FlatLabel>(window, "lingtai_project_open_error"),
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
    for (auto *label : flat_labels) {
        require(label->palette().color(QPalette::WindowText) == window_ink,
            "dark application WindowText role must reach every first-project "
            "flat label");
        require(!label->accessibleName().isEmpty(),
            "every first-project flat label must keep its exact accessible "
            "name");
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

void verify_live_system_palette(lingtai::desktop::NativeShell &shell) {
    static_cast<void>(shell);
    auto *style_hints = QGuiApplication::styleHints();

    style_hints->setColorScheme(Qt::ColorScheme::Light);
    QApplication::processEvents();
    require(st::windowBg->c == QColor("#ffffff")
            && st::windowFg->c == QColor("#000000")
            && st::msgInBg->c == QColor("#ffffff")
            && st::msgOutBg->c == QColor("#effdde"),
        "a live system-light change must restore Telegram's canonical light "
        "palette");

    style_hints->setColorScheme(Qt::ColorScheme::Dark);
    QApplication::processEvents();
    require(st::windowBg->c == QColor("#17212b")
            && st::windowFg->c == QColor("#f5f5f5")
            && st::msgInBg->c == QColor("#182533")
            && st::msgOutBg->c == QColor("#2b5278"),
        "a live system-dark change must select Telegram's canonical night "
        "palette");
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
            ampersand_key, plain_neighbor_key, "agent",
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
                == QStringLiteral("Missing · Agent")
            && required_child<QLabel>(window, "lingtai_selected_agent_key")
                ->textFormat() == Qt::PlainText,
        "a key-fallback header must keep one title and friendly Status · "
        "role below it");
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
            == QStringLiteral("Active · Agent"),
        "a distinct presentation title must keep one compact friendly "
        "Status · role line below the name");
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
            && flat_label_text(window, "lingtai_project_open_error")
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
    auto *open_error = required_ui_child<Ui::FlatLabel>(
        window, "lingtai_project_open_error");
    auto *title = required_child<QLabel>(
        window, "lingtai_product_title");
    auto *purpose = required_child<QLabel>(
        window, "lingtai_product_purpose");
    auto *empty_title = required_ui_child<Ui::FlatLabel>(
        window, "lingtai_no_project_title");
    auto *empty_detail = required_ui_child<Ui::FlatLabel>(
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
    require(empty_title->accessibilityName() == "No project open",
        "empty-route title changed");
    require(empty_detail->accessibilityName()
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
    auto *surface = required_child<QTextEdit>(
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
            "PR published, not merged.\\n<b>#1223</b> & <not-a-tag>",
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
    // outgoing row under the "You" header, oppositely aligned. Distinct real
    // bubble backgrounds are Commit31's own rounded viewport painting and are
    // already proven by the black-box rendered-pixel coverage in
    // verify_telegram_theme_reset, so only the alignment is asserted here.
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

    // The real incoming mail body above was written through a JSON `\n`
    // escape, so the kernel-side decode delivers an authentic newline (U+000A)
    // to the surface, never a literal backslash+n. The two logical messages
    // must still occupy exactly two aligned message blocks, one bubble each,
    // with the full literal multiline body kept inside the one incoming block
    // so it remains selectable and copyable as plain text.
    require(incoming_block.text().contains(QStringLiteral(
                "PR published, not merged.\u2028<b>#1223</b> & <not-a-tag>")),
        "the single incoming message block must contain the full literal "
        "multiline body with its decoded line break preserved inside that "
        "one block, never split across extra blocks");
    auto aligned_message_blocks = 0;
    for (auto block = surface->document()->begin();
            block != surface->document()->end();
            block = block.next()) {
        const auto alignment = block.blockFormat().alignment();
        if (alignment == Qt::AlignLeft || alignment == Qt::AlignRight) {
            ++aligned_message_blocks;
        }
    }
    require(aligned_message_blocks == 2,
        "one incoming multiline mail plus one outgoing mail must render as "
        "exactly two aligned message blocks/bubbles, never one block per "
        "decoded body line");
    require(surface->toPlainText().contains(
                QStringLiteral("PR published, not merged."))
            && surface->toPlainText().contains(
                QStringLiteral("<b>#1223</b> & <not-a-tag>")),
        "the full literal multiline body must remain selectable and copyable "
        "in toPlainText");

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
    auto *surface = required_child<QTextEdit>(
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

// The Step-5 Request sleep action row: one button plus one status label
// in the selected-Agent detail, before the low-level manifest/status facts.
// Covers no-selection disablement, a real write targeting exactly the
// selected Agent, the immediate "Sleep requested." text, the real one-second
// timer observing a simulated target-side application, A->B->A carrying no
// stale result, and one representative ineligible (stale) selection writing
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

// The Step-19 read-only selected-Agent Presets summary panel: one heading,
// surface, and state label distinct from the mailbox conversation. Covers
// exact resolved rendering (ordered allowed refs, independent active/default
// badges, active-effective fields, kernel provenance), a changed artifact
// becoming visible through the real one-second timer with no reselection,
// and selection isolation -- B, with no published artifact, must never show
// A's summary and must show "Not yet published".
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
    require(surface->objectName() != required_child<QTextEdit>(
                window, "lingtai_selected_agent_conversation")->objectName(),
        "Presets must be a distinct surface from the conversation");

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
    auto *status = required_ui_child<Ui::FlatLabel>(
        window, "lingtai_bootstrap_status");
    auto *dialog = required_child<QDialog>(
        window, "lingtai_new_project_dialog");
    auto *destination_input = required_ui_child<Ui::InputField>(
        window, "lingtai_bootstrap_destination_input");
    auto *preset_chooser = required_child<QComboBox>(
        window, "lingtai_bootstrap_preset_chooser");
    auto *create_start = required_ui_child<Ui::RoundButton>(
        window, "lingtai_bootstrap_create_start");
    required_ui_child<Ui::RoundButton>(window, "lingtai_bootstrap_cancel");
    auto *dialog_status = required_ui_child<Ui::FlatLabel>(
        window, "lingtai_bootstrap_dialog_status");
    auto *browse_button = required_ui_child<Ui::RoundButton>(
        window, "lingtai_bootstrap_destination_browse");
    auto *dialog_note = required_ui_child<Ui::FlatLabel>(
        window, "lingtai_bootstrap_dialog_note");
    required_ui_child<Ui::FlatLabel>(window, "lingtai_bootstrap_preset_label");
    // One named plain-shadow divider separates the dialog form from its action
    // row. PlainShadow carries no Q_OBJECT, so the lookup resolves the named
    // RpWidget and confirms the runtime type, exactly like the roster and
    // dashboard separators.
    auto *divider_widget = required_child<Ui::RpWidget>(
        *dialog, "lingtai_bootstrap_dialog_divider");
    require(dynamic_cast<Ui::PlainShadow *>(divider_widget) != nullptr,
        "the dialog form/action divider must be a Ui::PlainShadow");
    require(browse_button->accessibleName()
            == QStringLiteral("Browse destination folder"),
        "the Browse affordance must keep its exact accessible name");
    require(create_start->accessibleName() == QStringLiteral("Create and Start"),
        "the committing dialog action must keep its exact accessible name");
    require(dialog_note->accessibilityName()
            .contains(QStringLiteral("first Agent")),
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
    require(status->accessibilityName() == QStringLiteral("Discovering presets…"),
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
    create_start->clicked(Qt::NoModifier, Qt::LeftButton);
    QCoreApplication::processEvents();
    require(dialog_status->accessibilityName()
                .contains(QStringLiteral("nonempty")),
        "Create & Start with no destination must refuse with a concise "
        "dialog status");
    destination_input->setText(path_text(destination));
    preset_chooser->setCurrentIndex(1); // beta: non-first preset
    // The current useful default path is the filled destination field's
    // Return submission: it must reach the same Create & Start handler and
    // produce the exact same separated spawn argv.
    destination_input->setFocus();
    QCoreApplication::processEvents();
    auto enter = QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(destination_input->rawTextEdit(), &enter);
    require(status->accessibilityName() == QStringLiteral(
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
    while (status->accessibilityName()
                != QStringLiteral("Project created and Agent started.")
            && std::chrono::steady_clock::now() < attached_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    require(status->accessibilityName() == QStringLiteral(
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
    const auto fail_dialog_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!dialog->isVisible()
            && std::chrono::steady_clock::now() < fail_dialog_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    require(dialog->isVisible(),
        "the failing fixture's discovery must still show the dialog");
    destination_input->setText(path_text(sandbox / "partial-destination"));
    preset_chooser->setCurrentIndex(0);
    create_start->clicked(Qt::NoModifier, Qt::LeftButton);
    QCoreApplication::processEvents();
    const auto failure_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!status->accessibilityName().contains(QStringLiteral("launch_failed"))
            && std::chrono::steady_clock::now() < failure_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    require(status->accessibilityName().contains(QStringLiteral("launch_failed"))
            && status->accessibilityName().contains(
                QStringLiteral("fixture spawn refused"))
            && status->accessibilityName().contains(QStringLiteral(
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
    while (!status->accessibilityName().contains(
                QStringLiteral("could not be used"))
            && std::chrono::steady_clock::now() < fail_closed_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    require(status->accessibilityName().contains(
                QStringLiteral("could not be used")),
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

// The Commit-30 Telegram-derived responsive slice: one journey proves the
// source-backed 380x480 minimum, the OneColumn <-> Normal transition at the
// two-surface minima, the Return keyboard activation of a focused valid row
// through the existing click callback, the detail Back path, and the
// composer focus Telegram's HistoryWidget::setInnerFocus provides.
void verify_layout(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *sidebar = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_sidebar");
    auto *content = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_content");
    auto *separator_widget = required_child<Ui::RpWidget>(
        window, "lingtai_roster_separator");
    auto *detail = required_child<Ui::RpWidget>(
        window, "lingtai_agent_detail");
    auto *back_button = required_child<QPushButton>(
        window, "lingtai_agent_detail_back");
    auto *composer = static_cast<Ui::InputField *>(
        required_child<QObject>(window, "lingtai_composer_input"));

    require(window.minimumSize() == QSize(380, 480),
        "window minimum size must be Telegram's source-backed 380x480");

    const auto project = sandbox / "project";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    write_file(project / ".lingtai/alpha/.agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-a001",)"
        R"("agent_name":"alpha","address":"alpha","state":"active"})");
    write_file(project / ".lingtai/beta/.agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-b001",)"
        R"("agent_name":"beta","address":"beta","state":"active"})");
    const auto fixture_before = tree_snapshot(project);
    const auto outcome = shell.open_project(project, std::nullopt);
    require(outcome.disposition == ProjectOpenDisposition::opened,
        "the responsive fixture project must open");
    require(tree_snapshot(project) == fixture_before,
        "opening the responsive fixture must remain read-only");

    // Wide: both surfaces visible, Back hidden, roster responsive past its
    // 260px minimum, detail >= 380.
    require(sidebar->isVisible() && content->isVisible(),
        "a wide window must show roster and detail together");
    require(!back_button->isVisible(),
        "Back must stay hidden in wide two-column mode");
    require(sidebar->width() >= 260,
        "wide mode must keep the roster at or beyond the source-backed "
        "260px minimum");
    require(detail->width() >= 380,
        "wide mode must keep the detail column at least 380px");

    // Narrow to the 380x480 minimum: exactly one surface, the roster.
    window.resize(380, 480);
    QCoreApplication::processEvents();
    require(window.width() >= 380 && window.height() >= 480,
        "window must honor its 380x480 minimum");
    require(sidebar->isVisible() && !content->isVisible(),
        "a narrow window must show only the roster surface");
    require(!separator_widget->isVisible(),
        "a narrow window must hide the column separator");

    // Keyboard-activating a focused valid row with Return uses the existing
    // selection path: detail replaces the roster, Back appears, and the
    // composer is focused.
    auto *row = agent_row(window, "alpha");
    row->setFocus();
    auto return_key = QKeyEvent(
        QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(row, &return_key);
    QCoreApplication::processEvents();
    require(shell.selection_state().selected_agent_directory_key()
            == std::optional<fs::path>("alpha"),
        "Return must activate the focused valid Agent row");
    require(!sidebar->isVisible() && content->isVisible(),
        "a selected Agent in a narrow window must replace the roster with "
        "the detail");
    require(back_button->isVisible(),
        "a selected Agent in a narrow window must expose Back");
    require(window.focusWidget() == composer->rawTextEdit(),
        "selecting an Agent must focus the composer");

    // Narrow -> wide shows both and keeps the selection; wide -> narrow with
    // the active selected Agent keeps the detail.
    window.resize(1200, 800);
    QCoreApplication::processEvents();
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("alpha")
            && sidebar->isVisible() && content->isVisible()
            && !back_button->isVisible(),
        "widening must show both surfaces and keep the selected Agent");
    require(sidebar->width() >= 260 && detail->width() >= 380,
        "wide mode must keep roster past its 260 minimum and detail at "
        "least 380 after narrowing");

    window.resize(380, 480);
    QCoreApplication::processEvents();
    require(!sidebar->isVisible() && content->isVisible()
            && back_button->isVisible(),
        "narrowing with an active selected Agent must keep the detail");

    // Back returns to the roster and focuses a usable row.
    back_button->click();
    QCoreApplication::processEvents();
    require(!shell.selection_state().selected_agent_directory_key(),
        "Back must clear the narrow-window selection");
    require(sidebar->isVisible() && !content->isVisible()
            && !back_button->isVisible(),
        "Back must return a narrow window to the roster surface");
    auto *focus = window.focusWidget();
    require(focus && qobject_cast<QPushButton *>(focus),
        "Back must return keyboard focus to a usable roster row");

    // Widen once more: both surfaces return, Back hides, roster responsive
    // past its minimum and detail at least 380.
    window.resize(1200, 800);
    QCoreApplication::processEvents();
    require(sidebar->isVisible() && content->isVisible(),
        "a wide window must show roster and detail together after narrow");
    require(!back_button->isVisible(),
        "Back must stay hidden in wide two-column mode");
    require(sidebar->width() >= 260,
        "wide roster must stay at or beyond 260px after narrow");
    require(detail->width() >= 380,
        "wide detail must stay at least 380px after narrow");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "responsive fixtures must be removed");
}

// The R1 RED: the roster/divider must be genuinely user-resizable. At a
// two-pane width the accepted `lingtai_roster_separator` visual divider must
// expose a distinct user-resizable `lingtai_roster_resize_handle` widget; a
// real horizontal drag on it must move the roster width inside the source
// contract band (a 22-30% share with roster >= 260 and detail >= 380), the
// chosen share must survive an ordinary window resize (or clamp at the band
// edge), and the narrow OneColumn selected-detail + Back behavior must stay
// truthful. Current production has only the fixed PlainShadow divider, so it
// must fail the resize-handle lookup.
void verify_resizable_sidebar(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *sidebar = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_sidebar");
    auto *detail = required_child<Ui::RpWidget>(
        window, "lingtai_agent_detail");
    auto *separator_widget = required_child<Ui::RpWidget>(
        window, "lingtai_roster_separator");
    auto *back_button = required_child<QPushButton>(
        window, "lingtai_agent_detail_back");

    const auto project = sandbox / "project";
    write_file(project / ".lingtai/alpha/.agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-a001",)"
        R"("agent_name":"alpha","address":"alpha","state":"active"})");
    write_file(project / ".lingtai/beta/.agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-b001",)"
        R"("agent_name":"beta","address":"beta","state":"active"})");
    const auto outcome = shell.open_project(project, std::nullopt);
    require(outcome.disposition == ProjectOpenDisposition::opened,
        "the resizable-sidebar fixture project must open");

    window.resize(1200, 800);
    QCoreApplication::processEvents();
    require(sidebar->isVisible() && detail->isVisible(),
        "a wide window must show roster and detail together");
    require(separator_widget->isVisible(),
        "the roster divider must be a visible, grabbable handle in "
        "two-pane mode");
    // The user-resizable handle is a distinct semantic widget the accepted
    // fixed PlainShadow separator never provides; current production must
    // fail exactly this lookup.
    auto *resize_handle = window.findChild<QWidget *>(
        "lingtai_roster_resize_handle");
    require(resize_handle != nullptr,
        "the two-pane roster must expose a user-resizable "
        "lingtai_roster_resize_handle divider handle instead of the "
        "fixed-width PlainShadow separator");
    const auto initial_roster = sidebar->width();

    // Drag the visible resize handle to the right: one real press/move/release
    // on the handle widget. A user-resizable divider must move the roster
    // width itself; the source contract keeps a 22-30% share with roster >= 260
    // and detail >= 380.
    const auto grab_local = resize_handle->rect().center();
    const auto grab_global = resize_handle->mapToGlobal(grab_local);
    auto press = QMouseEvent(QEvent::MouseButtonPress,
        QPointF(grab_local), QPointF(grab_global),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(resize_handle, &press);
    const auto drop_local = grab_local + QPoint(80, 0);
    const auto drop_global = resize_handle->mapToGlobal(drop_local);
    auto drag = QMouseEvent(QEvent::MouseMove,
        QPointF(drop_local), QPointF(drop_global),
        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(resize_handle, &drag);
    auto release = QMouseEvent(QEvent::MouseButtonRelease,
        QPointF(drop_local), QPointF(drop_global),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(resize_handle, &release);
    QCoreApplication::processEvents();

    require(sidebar->width() != initial_roster,
        "dragging the visible resize handle must move the roster width");
    const auto chosen_ratio =
        double(sidebar->width()) / double(window.body()->width());
    require(chosen_ratio >= 0.22 && chosen_ratio <= 0.30
            && sidebar->width() >= 260 && detail->width() >= 380,
        "after the divider drag the roster must sit inside the contract "
        "band: a 22-30% share with roster at least 260px and detail at "
        "least 380px");

    // The chosen share survives an ordinary window resize (or clamps at the
    // band edge) rather than snapping back to the default ratio.
    window.resize(1400, 800);
    QCoreApplication::processEvents();
    require(sidebar->isVisible() && detail->isVisible(),
        "resizing a two-pane window must keep both surfaces");
    require(sidebar->width() >= 260 && detail->width() >= 380,
        "after an ordinary resize the roster must stay in the contract band");
    const auto resized_ratio =
        double(sidebar->width()) / double(window.body()->width());
    require(qAbs(resized_ratio - chosen_ratio) < 0.02,
        "the chosen roster ratio must survive a subsequent window resize or "
        "clamp at the band edge");

    // Narrow OneColumn selected-detail + Back stays truthful.
    window.resize(380, 480);
    QCoreApplication::processEvents();
    click_agent(window, "alpha");
    require(shell.selection_state().selected_agent_directory_key()
            == std::optional<fs::path>("alpha"),
        "narrow OneColumn must still select an Agent");
    require(!sidebar->isVisible() && detail->isVisible()
            && back_button->isVisible(),
        "narrow OneColumn selected-detail must show the detail with Back");
    back_button->click();
    QCoreApplication::processEvents();
    require(!shell.selection_state().selected_agent_directory_key()
            && sidebar->isVisible() && !detail->isVisible()
            && !back_button->isVisible(),
        "Back must still return a narrow window to the roster surface");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "resizable-sidebar fixtures must be removed");
}

// The Commit-24 shell slice: one persistent left project/Agent list column
// (responsive from a 260px minimum) replaces the action-only rail and the
// nested roster route. The roster is the left column's own content (never a
// second list inside the right content route), its rows are intrinsically
// sized from the fixed 40px avatar plus two font lines plus stable padding
// (never a hard min=max62 box), with one primary name line plus one compact
// secondary/state line, a plain-shadow separator divides list from content,
// the compact project actions stay reachable, selection still drives the same
// right detail, and an unchanged projection refresh must not rebuild the row
// tree so selection/focus/scroll survive.
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

    // The persistent left list column is responsive at or beyond its 260px
    // minimum when a project is open, and fills the body height.
    require(sidebar->width() >= 260,
        "the persistent left list column must keep its 260px minimum");
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

    // Rows: intrinsic height from the fixed 40px avatar plus two font lines
    // plus stable vertical padding, with exactly one primary name line plus
    // one compact secondary/state line.
    auto *row = agent_row(window, "alpha");
    require(row->minimumHeight() != row->maximumHeight(),
        "Agent rows must not be a hard min=max62 box: their height must be "
        "intrinsic from the avatar plus two font lines plus stable padding");
    require(row->sizeHint().height() >= 40 + 2 * 8,
        "the intrinsic row sizeHint must accommodate the fixed 40px avatar "
        "disc plus the stable vertical framing");
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

// The header-unit RED: the selected-Agent header must be a compact hierarchy
// -- one title plus one small muted status line, exactly one primary action,
// and lifecycle secondary controls as subtle compact buttons/icons, never a
// second full-caption action button. Current production keeps the status line
// in the same prominent ink as the title and offers both Start Agent and
// Request sleep as full-caption buttons, so this journey must fail exactly
// those assertions, never fonts, metrics, or a hidden control.
void verify_compact_header_hierarchy(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *top_bar = required_child<QWidget>(window, "lingtai_chat_top_bar");
    auto *presentation_name = required_child<QLabel>(
        window, "lingtai_selected_agent_presentation_name");
    auto *status = required_child<QLabel>(
        window, "lingtai_selected_agent_key");
    auto *avatar = required_child<QWidget>(
        window, "lingtai_selected_agent_avatar");

    const auto project = sandbox / "project";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    const auto target = project / ".lingtai/alpha";
    write_file(target / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-a001",)"
        R"("agent_name":"alpha","nickname":"Alpha Agent",)"
        R"("address":"alpha","state":"idle"})");
    write_file(target / ".agent.heartbeat", "0");
    const auto fixture_before = tree_snapshot(project);
    const auto outcome = shell.open_project(project, std::nullopt);
    require(outcome.disposition == ProjectOpenDisposition::opened,
        "the compact-header fixture project must open");
    require(tree_snapshot(project) == fixture_before,
        "opening the compact-header fixture must remain read-only");

    window.resize(1200, 800);
    QCoreApplication::processEvents();
    click_first_agent_canvas_row(window);
    require(shell.selection_state().selected_agent_directory_key()
            == std::optional<fs::path>("alpha"),
        "the compact-header fixture Agent must be selectable");
    QCoreApplication::processEvents();

    const auto header_child_of = [&](QWidget *widget) {
        for (auto *ancestor = widget; ancestor != nullptr;
             ancestor = ancestor->parentWidget()) {
            if (ancestor == top_bar) return true;
        }
        return false;
    };

    // Title + small muted status: the presentation name is the prominent
    // title and the status line reads small and in a distinct muted ink.
    require(presentation_name->isVisible(),
        "the compact header must keep the selected-Agent title visible");
    require(status->isVisible(),
        "the compact header must show a small status line under the title");
    require(presentation_name->font().pointSize() == 16,
        "the selected-Agent title must scale with the 16px Conversation body");
    require(status->font().pointSize() == 12,
        "the friendly Status · role line must scale to the legible 12pt secondary rung");
    require(status->font().pointSize() < presentation_name->font().pointSize(),
        "the header status must stay smaller than the title");
    require(status->palette().color(QPalette::WindowText)
            != presentation_name->palette().color(QPalette::WindowText),
        "the header status must render in a distinct muted ink, never the "
        "same prominent color as the title");
    require(avatar->isVisible()
            && avatar->width() == avatar->height()
            && avatar->width() <= 40,
        "the selected-Agent header must show one compact square avatar");
    require(top_bar->layout()->itemAt(0)->widget() == avatar,
        "the selected-Agent avatar must be the leftmost header item");
    require(avatar->accessibleDescription()
            == presentation_name->text(),
        "the header avatar must identify the same selected Agent as the "
        "prominent name");
    require(status->text().contains(QStringLiteral(" · "))
            && status->text().endsWith(QStringLiteral("Agent"))
            && !status->text().contains(QStringLiteral("role:"))
            && !status->text().contains(QStringLiteral("presence:")),
        "the secondary line must read friendly Status · role, matching the "
        "Sidebar semantics without raw fact labels");

    // Exactly one primary action: the header owns one primary-action anchor
    // and, in wide mode, exactly one visible action button carries a caption.
    auto *primary = required_child<QPushButton>(
        window, "lingtai_selected_agent_start_agent");
    require(header_child_of(primary),
        "the one primary action must live inside the selected-Agent header");
    auto primary_in_header = 0;
    for (auto *button : top_bar->findChildren<QPushButton *>()) {
        if (button->objectName()
                == QStringLiteral("lingtai_selected_agent_start_agent")) {
            ++primary_in_header;
        }
    }
    require(primary_in_header == 1,
        "the compact header must expose exactly one primary action");
    require(primary->isVisible() && primary->isEnabled(),
        "the one primary action must be visible and enabled for an "
        "eligible selected Agent");
    auto visible_caption_actions = 0;
    for (auto *button : top_bar->findChildren<QPushButton *>()) {
        if (button->isVisible() && !button->text().isEmpty()) {
            ++visible_caption_actions;
        }
    }
    require(visible_caption_actions == 1 && primary->isVisible()
            && !primary->text().isEmpty(),
        "the compact header must show exactly one caption-carrying action "
        "button: the one primary action, never a second full-caption action");

    // Lifecycle secondaries: subtle compact buttons/icons, never a second
    // full-caption text button.
    auto *sleep = required_child<QPushButton>(
        window, "lingtai_selected_agent_request_sleep");
    require(header_child_of(sleep),
        "the lifecycle secondary must live inside the selected-Agent header");
    require(sleep->text().isEmpty(),
        "the lifecycle secondary must be a subtle compact icon button, "
        "never a second full-caption action");
    require(sleep->accessibleName() == QStringLiteral("Request sleep"),
        "the compact icon lifecycle secondary must keep its accessible "
        "identity");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "compact-header fixtures must be removed");
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

// The Commit-28 cut: the one retained selected-Agent read-only source
// (Presets) is presented through the shared local section framing, and the
// Start/Sleep action region keeps stable geometry when the selected Agent
// switches between heartbeat-live (no Start action) and start-eligible
// stale/missing (Start action present).
void verify_selected_agent_dashboard_layout(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    const auto preset_summary = dashboard_section(window, "preset_summary");

    const auto *section = &preset_summary;
    require(section->heading->parent() == section->owner
            && section->surface->parent() == section->owner
            && section->state->parent() == section->owner
            && section->separator->parent() == section->owner,
        "the Presets dashboard section must directly own its heading, "
        "surface, state, and separator");
    require(section->heading->font().weight() == QFont::DemiBold,
        "the Presets dashboard heading must stay semibold");
    require(section->surface->isReadOnly(),
        "the Presets dashboard surface must stay read-only");
    require(!section->heading->accessibleName().isEmpty()
            && !section->surface->accessibleName().isEmpty()
            && !section->state->accessibleName().isEmpty(),
        "the Presets dashboard section must stay accessible");
    require(section->surface->minimumHeight() > 0,
        "the Presets dashboard surface must keep a consistent minimum height");

    const auto separator_minimum = preset_summary.separator->minimumHeight();
    require(separator_minimum > 0
            && separator_minimum == preset_summary.separator->maximumHeight(),
        "the Presets dashboard separator must be one fixed thin line");

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

// The Repair3 removal contract: the Agent Activity and Task Card destinations
// are gone from the selected-Agent shell, so their page-nav buttons and their
// panel surfaces/state lines/section owners must be absent; the page
// navigation retains exactly Conversation + Presets; and the low-level
// `status_activity` fact label (distinct from the removed Agent Activity
// destination) stays present with its stable identity.
void verify_removed_activity_and_task_card_destinations(
        lingtai::desktop::NativeShell &shell) {
    auto &window = shell.window();

    // Absence: neither removed destination may expose a page-nav button.
    require(window.findChild<QPushButton *>("lingtai_agent_page_nav_activity")
            == nullptr,
        "the Activity page-nav button must be absent");
    require(window.findChild<QPushButton *>("lingtai_agent_page_nav_task_card")
            == nullptr,
        "the Task Card page-nav button must be absent");
    // Nor any of their panel surfaces, headings, state lines, or section
    // owners.
    for (const char *name : {
            "lingtai_selected_agent_activity",
            "lingtai_selected_agent_activity_heading",
            "lingtai_selected_agent_activity_state",
            "lingtai_selected_agent_activity_section",
            "lingtai_selected_agent_task_card",
            "lingtai_selected_agent_task_card_heading",
            "lingtai_selected_agent_task_card_state",
            "lingtai_selected_agent_task_card_section" }) {
        require(window.findChild<QObject *>(name) == nullptr,
            std::string("the removed panel surface must be absent: ") + name);
    }

    // The page navigation retains exactly Conversation + Presets.
    required_child<QPushButton>(window, "lingtai_agent_page_nav_conversation");
    required_child<QPushButton>(window, "lingtai_agent_page_nav_presets");
    auto page_nav_count = 0;
    for (const auto *button : window.findChildren<QPushButton *>()) {
        if (button->objectName().startsWith(
                QStringLiteral("lingtai_agent_page_nav_"))) {
            ++page_nav_count;
        }
    }
    require(page_nav_count == 2,
        "the selected-Agent page navigation must retain exactly two "
        "buttons: Conversation and Presets");

    // The low-level selected-Agent status_activity fact label remains present
    // with its stable identity; its exact projected state semantics stay
    // proven by the retained open-project journey above.
    auto *status_activity = required_child<QLabel>(
        window, "lingtai_selected_agent_status_activity");
    require(status_activity->accessibleName()
            == QStringLiteral("Status activity"),
        "the low-level selected-Agent status_activity fact label must retain "
        "its stable identity");
}

// The tabs unit contract: Conversation and Presets are plain text tabs — the
// selected tab paints only its caption glyphs on the shell backdrop plus one
// subtle accent underline along its bottom edge, and is never a filled
// rectangular slab. Real rendered pixels in the shared content coordinate
// space prove it: the selected tab's interior must read the exact adjacent
// backdrop (the tab is transparent there) with no pixel of the `windowBgOver`
// slab token anywhere in it, and the accent must sit only on its bottom row
// and follow the selection.
void verify_plain_underline_page_tabs(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *conversation_nav = required_child<QPushButton>(
        window, "lingtai_agent_page_nav_conversation");
    auto *presets_nav = required_child<QPushButton>(
        window, "lingtai_agent_page_nav_presets");

    const auto project = sandbox / "project";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    write_file(project / ".lingtai/alpha/.agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-a001",)"
        R"("agent_name":"alpha","address":"alpha","state":"active"})");
    static_cast<void>(shell.open_project(project, std::nullopt));
    window.resize(1200, 800);
    QCoreApplication::processEvents();
    click_first_agent_canvas_row(window);
    QCoreApplication::processEvents();

    auto *pages_nav = presets_nav->parentWidget();
    require(conversation_nav->font().pointSize() == 13
            && presets_nav->font().pointSize() == 13,
        "Conversation and Presets must share the 13pt main-window navigation rung");
    require(pages_nav->height() == 28
            && conversation_nav->height() == 28
            && presets_nav->height() == 28,
        "the larger plain-text tab strip must own one exact 28px line box");

    const auto slab = st::windowBgOver->c;
    const auto accent = st::dialogsBgActive->c;
    const auto image = pages_nav->grab().toImage();
    // The grab backing is physical device pixels, but every coordinate below
    // is a logical pages_nav point: scale by the image's devicePixelRatio
    // (Retina) once so each pixelColor sample lands on the physical pixel.
    const auto dpr = image.devicePixelRatio();
    const auto at = [&](const QImage &target, const QPoint &logical) {
        return target.pixelColor(QPoint(qRound(logical.x() * dpr),
                                        qRound(logical.y() * dpr)));
    };
    // The page-nav row's trailing stretch is guaranteed unpainted, so it is
    // the real shell backdrop the transparent tabs sit on, in the same shared
    // coordinate space as every tab sample below.
    const auto backdrop = [&] {
        return at(image, QPoint(presets_nav->geometry().right() + 8,
                                presets_nav->geometry().center().y()));
    }();
    require(backdrop == st::windowBg->c,
        "the page-nav row must sit directly on the shell backdrop token");
    const auto interior = [&](const QImage &target, QPushButton *button) {
        // Sample the quiet upper-right corner, not the caption's center column:
        // the larger 13pt glyph ink legitimately reaches the old y=3 center
        // probe while the tab surface itself remains transparent.
        return at(target, button->mapTo(
            pages_nav, QPoint(button->width() - 3, 2)));
    };
    const auto bottom = [&](const QImage &target, QPushButton *button) {
        return at(target, button->mapTo(
            pages_nav, QPoint(button->width() / 2, button->height() - 1)));
    };
    const auto slab_is_bounded = [&](const QImage &target, QPushButton *button) {
        auto count = 0;
        for (auto y = 0; y < button->height() - 2; ++y) {
            for (auto x = 0; x < button->width(); ++x) {
                if (at(target, button->mapTo(pages_nav, QPoint(x, y)))
                        == slab) {
                    ++count;
                }
            }
        }
        // A few antialiased caption pixels may numerically equal windowBgOver;
        // a real filled slab would occupy almost the entire button interior.
        return count * 10 < button->width() * (button->height() - 2);
    };
    require(slab_is_bounded(image, conversation_nav),
        "the selected Conversation tab must never be a filled rectangular slab");
    require(interior(image, conversation_nav) == backdrop,
        "the selected Conversation tab must stay plain text on the backdrop");
    require(bottom(image, conversation_nav) == accent,
        "the selected tab must carry only the subtle underline accent");
    require(slab_is_bounded(image, presets_nav),
        "the unselected Presets tab must stay plain text too");
    require(bottom(image, presets_nav) != accent,
        "the underline accent must belong only to the selected tab");

    require(conversation_nav->font().weight() == QFont::Normal
            && presets_nav->font().weight() == QFont::Normal,
        "both page captions must use regular weight before selection moves");
    presets_nav->click();
    QCoreApplication::processEvents();
    const auto after = pages_nav->grab().toImage();
    require(conversation_nav->font().weight() == QFont::Normal
            && presets_nav->font().weight() == QFont::Normal,
        "both page captions must keep regular weight after selection moves; "
        "color and underline alone mark selection");
    require(bottom(after, presets_nav) == accent
            && bottom(after, conversation_nav) != accent,
        "the underline accent must follow the selection");
    require(slab_is_bounded(after, presets_nav),
        "the newly selected tab must stay slab-free");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "plain-underline tab fixtures must be removed");
}

// The Commit-31 cut: one coherent Telegram-family theme/layout reset for the
// normal-width chats shell. A wide window must give the dialog list a
// source-backed responsive width beyond its 260px minimum, all major surfaces
// and the selected row must be owned by the shared lib_ui palette (not raw
// white Qt surfaces), the selected-Agent detail must be chat-first -- the
// conversation dominates while Presets sits behind one compact secondary page
// navigation so only one content surface shows at a time -- the conversation
// must be a real bubble surface with a lib_ui-owned composer at the bottom,
// and every existing object/accessibility anchor must survive the reset.
void verify_telegram_theme_reset(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *sidebar = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_sidebar");
    auto *content = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_content");
    auto *conversation_heading = required_child<QLabel>(
        window, "lingtai_selected_agent_conversation_heading");
    auto *conversation = required_child<QTextEdit>(
        window, "lingtai_selected_agent_conversation");
    auto *conversation_state = required_child<QLabel>(
        window, "lingtai_selected_agent_conversation_state");
    auto *composer_input = required_ui_child<Ui::InputField>(
        window, "lingtai_composer_input");
    auto *send_button = required_ui_child<Ui::RoundButton>(
        window, "lingtai_composer_send_button");
    auto *preset_owner = required_child<Ui::RpWidget>(
        window, "lingtai_selected_agent_preset_summary_section");

    const auto project = sandbox / "project";
    const auto mailbox = project / ".lingtai" / "human" / "mailbox";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    write_file(project / ".lingtai/alpha/.agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-a001",)"
        R"("agent_name":"alpha","address":"alpha","state":"active"})");
    write_file(mailbox / "inbox" / "20260807T184852-0d13" / "message.json",
        conversation_envelope("alpha", "human", "Slice done",
            "PR published, not merged.", "received_at",
            "2026-08-07T18:48:52Z"));
    write_file(mailbox / "sent" / "20260807T190000-aa01" / "message.json",
        conversation_envelope("human", "alpha", "Re: Slice done",
            "Thanks, reviewing tomorrow.", "sent_at",
            "2026-08-07T19:00:00Z"));

    window.resize(1200, 800);
    QCoreApplication::processEvents();
    const auto body_width = window.body()->width();
    const auto outcome = shell.open_project(project, std::nullopt);
    require(outcome.disposition == ProjectOpenDisposition::opened,
        "the theme-reset fixture project must open");
    QCoreApplication::processEvents();

    // 1. Responsive list/content proportion: at a normal reference width the
    // dialog list must expand past its source-backed 260px minimum toward the
    // screenshot's ~38.7% normal-width share while staying narrower than the
    // chat content.
    require(sidebar->width() > 260,
        "a wide window must give the dialog list a responsive width beyond "
        "the 260px source minimum");
    require(sidebar->width() < content->width()
            && sidebar->width() > body_width / 5,
        "the list column must stay a bounded, narrower-than-chat proportion "
        "of the body");
    require(content->width() >= 380,
        "the chat content must keep the source-backed 380px detail minimum");

    // 2. Palette-owned surfaces and selected row: the list surface is painted
    // from the shared lib_ui palette, the chat surface carries the palette
    // background token, and the selected row resolves its selection color
    // from the same palette -- never raw/white Qt defaults. Both main
    // surfaces sit on the single light canvas base token st::windowBg.
    require(sidebar->grab().toImage().pixelColor(2, 2)
            == st::windowBg->c,
        "the dialog list surface must be palette-owned (windowBg), not "
        "a raw white Qt surface");
    require(content->grab().toImage().pixelColor(2, 2) == st::windowBg->c,
        "the chat content surface must be palette-owned (windowBg), not a "
        "raw white Qt surface");
    auto *row = agent_row(window, "alpha");
    row->setChecked(true);
    require(row->palette().color(QPalette::Highlight) == st::dialogsBgActive->c,
        "the selected Agent row must resolve its selection color from the "
        "shared lib_ui palette");

    // 3. Chat-first page instead of simultaneous dashboard stacking: the
    // conversation surface dominates the detail while the Presets read-only
    // source sits behind one compact page navigation.
    click_agent(window, "alpha");
    require(shell.selection_state().selected_agent_directory_key()
            == std::optional<fs::path>("alpha"),
        "alpha must be selectable for the theme reset");
    require(conversation->isVisible(),
        "the conversation must be the visible default content of a selected "
        "Agent");
    require(!preset_owner->isVisible(),
        "Presets must not stack under the conversation in the chat-first "
        "detail");

    auto *pages_nav = required_child<Ui::RpWidget>(
        window, "lingtai_agent_pages_nav");
    auto *conversation_nav = required_child<QPushButton>(
        window, "lingtai_agent_page_nav_conversation");
    auto *presets_nav = required_child<QPushButton>(
        window, "lingtai_agent_page_nav_presets");
    require(pages_nav->isVisible() && conversation_nav->isVisible()
            && presets_nav->isVisible(),
        "the compact secondary page navigation must be reachable in the "
        "selected-Agent detail");
    presets_nav->click();
    QCoreApplication::processEvents();
    require(preset_owner->isVisible()
            && !conversation->isVisible(),
        "activating the Presets page must show exactly that one secondary "
        "surface");
    conversation_nav->click();
    QCoreApplication::processEvents();
    require(conversation->isVisible()
            && !preset_owner->isVisible(),
        "returning to the Conversation page must restore the chat surface "
        "and hide the secondary page");

    // 4. Bubble/composer ownership, black-box: only the rendered viewport
    // pixels and public Qt behavior are consulted -- never document or block
    // internals. The chat surface must paint a palette-derived non-white,
    // non-bubble backdrop, distinct incoming/outgoing bubble components
    // (msgInBg on the left, msgOutBg on the right), a largest bubble span of
    // at most 75% of the viewport, real vertical spacing between the two
    // bubbles, and the lib_ui composer visible directly below the conversation
    // in one common ancestor coordinate system.
    const auto surface_image = conversation->viewport()->grab().toImage();
    const auto color_close = [](const QColor &a, const QColor &b) {
        return qAbs(a.red() - b.red()) <= 12
            && qAbs(a.green() - b.green()) <= 12
            && qAbs(a.blue() - b.blue()) <= 12;
    };
    // The surface owns a transparent Base, so no widget palette role is a
    // meaningful backdrop: sample the painted backdrop directly from the grab
    // and compare it to the light lib_ui palette token the surface paints
    // (`windowBg`, the shared single light-canvas base used across the shell).
    const auto sampled_backdrop = surface_image.pixelColor(
        surface_image.width() / 2, surface_image.height() - 6);
    require(color_close(sampled_backdrop, st::windowBg->c),
        "the sampled chat backdrop pixel must match the lib_ui palette token "
        "the surface paints");
    require(sampled_backdrop != st::msgInBg->c
            && sampled_backdrop != st::msgOutBg->c,
        "the sampled chat backdrop must be distinct from both bubble colors");

    struct BubbleTrace {
        int min_x = -1, max_x = -1, min_y = -1, max_y = -1;
        int widest_run = 0;
    };
    // Each row's bounds come only from its dominant contiguous target-colored
    // run when that run is at least 20 pixels wide; sparse antialiased
    // cross-color pixels never move the bubble bounds or widest span.
    const auto trace_bubbles = [&](const QColor &target) {
        auto trace = BubbleTrace();
        for (auto y = 0; y != surface_image.height(); ++y) {
            auto run = 0;
            auto run_start = 0;
            auto row_widest = 0;
            auto row_widest_start = 0;
            auto row_widest_end = 0;
            for (auto x = 0; x != surface_image.width(); ++x) {
                if (surface_image.pixelColor(x, y) == target) {
                    if (run == 0) run_start = x;
                    ++run;
                    if (run > row_widest) {
                        row_widest = run;
                        row_widest_start = run_start;
                        row_widest_end = x;
                    }
                } else {
                    run = 0;
                }
            }
            if (row_widest >= 20) {
                if (trace.min_x < 0 || row_widest_start < trace.min_x) {
                    trace.min_x = row_widest_start;
                }
                if (row_widest_end > trace.max_x) {
                    trace.max_x = row_widest_end;
                }
                if (trace.min_y < 0 || y < trace.min_y) trace.min_y = y;
                trace.max_y = y;
                if (row_widest > trace.widest_run) {
                    trace.widest_run = row_widest;
                }
            }
        }
        return trace;
    };
    const auto incoming = trace_bubbles(st::msgInBg->c);
    const auto outgoing = trace_bubbles(st::msgOutBg->c);
    require(incoming.min_y >= 0 && outgoing.min_y >= 0,
        "the chat surface must render real incoming and outgoing bubble "
        "components");

    const auto text_bounds = [&](const QString &prefix) {
        auto result = QRectF();
        for (auto block = conversation->document()->begin(); block.isValid();
             block = block.next()) {
            if (!block.text().startsWith(prefix)) continue;
            const auto *layout = block.layout();
            for (auto index = 0; index != layout->lineCount(); ++index) {
                const auto line = layout->lineAt(index);
                const auto line_rect = line.naturalTextRect()
                    .translated(layout->position());
                result = result.isNull() ? line_rect : result.united(line_rect);
            }
            break;
        }
        return result.translated(
            -conversation->horizontalScrollBar()->value(),
            -conversation->verticalScrollBar()->value());
    };
    const auto image_scale = surface_image.devicePixelRatio();
    const auto require_contains_text = [&](const BubbleTrace &bubble,
            const QRectF &text, const char *direction) {
        const auto bubble_left = bubble.min_x / image_scale;
        const auto bubble_right = (bubble.max_x + 1) / image_scale;
        const auto bubble_top = bubble.min_y / image_scale;
        const auto bubble_bottom = (bubble.max_y + 1) / image_scale;
        require(!text.isNull()
                && bubble_left <= qFloor(text.left()) - 10
                && bubble_right >= qCeil(text.right()) + 10
                && bubble_top <= qFloor(text.top()) - 7
                && bubble_bottom >= qCeil(text.bottom()) + 7,
            std::string(direction)
                + " bubble must contain its laid-out text with Telegram's "
                  "11x8 padding (one-pixel raster tolerance): bubble=("
                + std::to_string(bubble_left) + ","
                + std::to_string(bubble_top) + ")-("
                + std::to_string(bubble_right) + ","
                + std::to_string(bubble_bottom) + ") text=("
                + std::to_string(text.left()) + ","
                + std::to_string(text.top()) + ")-("
                + std::to_string(text.right()) + ","
                + std::to_string(text.bottom()) + ") scale="
                + std::to_string(image_scale));
    };
    require_contains_text(incoming,
        text_bounds(QStringLiteral("alpha ·")), "incoming");
    require_contains_text(outgoing,
        text_bounds(QStringLiteral("You ·")), "outgoing");
    require((incoming.min_x + incoming.max_x) / 2 < surface_image.width() / 2
            && (outgoing.min_x + outgoing.max_x) / 2
                > surface_image.width() / 2,
        "the incoming bubble must sit on the left and the outgoing bubble on "
        "the right");
    require(std::max(incoming.widest_run, outgoing.widest_run)
            <= surface_image.width() * 3 / 4,
        "the largest bubble span must stay at most 75% of the chat viewport");
    // Roundedness is owned by the production drawRoundedRect source review and
    // the real same-state render comparison, not by a brittle pixel heuristic;
    // this journey proves the rendered bubble components are palette-derived
    // and distinct, oppositely placed, bounded, and vertically spaced.
    require(outgoing.min_y - incoming.max_y >= 2,
        std::string("the incoming and outgoing bubbles must be separated by "
            "real vertical spacing, not touching (incoming_min_y=")
            + std::to_string(incoming.min_y)
            + " incoming_max_y=" + std::to_string(incoming.max_y)
            + " outgoing_min_y=" + std::to_string(outgoing.min_y)
            + " outgoing_max_y=" + std::to_string(outgoing.max_y)
            + " gap=" + std::to_string(outgoing.min_y - incoming.max_y)
            + ")");

    const auto composer_top = composer_input->mapTo(content, QPoint(0, 0)).y();
    const auto conversation_bottom = conversation->mapTo(
        content, QPoint(0, conversation->height())).y();
    require(composer_input->isVisible() && send_button->isVisible()
            && composer_top >= conversation_bottom - 40,
        std::string("the lib_ui composer must sit at the bottom under the "
            "conversation in one common ancestor coordinate system "
            "(composer_top=")
            + std::to_string(composer_top)
            + " conversation_bottom="
            + std::to_string(conversation_bottom)
            + " composer_visible="
            + std::to_string(composer_input->isVisible())
            + " send_visible=" + std::to_string(send_button->isVisible())
            + ")");

    // 5. Stable existing object/accessibility anchors must survive the reset.
    require(!conversation_heading->accessibleName().isEmpty()
            && !conversation->accessibleName().isEmpty()
            && !conversation_state->accessibleName().isEmpty()
            && !composer_input->accessibleName().isEmpty()
            && !send_button->accessibleName().isEmpty(),
        "every existing conversation/composer anchor must keep its "
        "accessibility identity");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "theme-reset fixtures must be removed");
}

// The M4 modern-composer presentation contract, defined as one focused
// relational geometry journey. The `lingtai_composer_input` and
// `lingtai_composer_send_button` controls remain one compact, vertically
// aligned action row inside the `lingtai_composer` lane, visible and
// responsive from a representative wide state down to the narrow 380
// minimum where the input/Send row stays near-full width within the composer
// lane. The `lingtai_composer_status` read-out is
// owned by the lane (a descendant of `lingtai_composer`), never a separate
// detail row. All geometry is relational to current object names in one
// common ancestor coordinate system; no screenshot pixels are consulted.
void verify_modern_composer_surface(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *composer = required_child<Ui::RpWidget>(
        window, "lingtai_composer");
    auto *controls = required_child<Ui::RpWidget>(
        window, "lingtai_composer_controls");
    auto *attachment_button = required_child<QPushButton>(
        window, "lingtai_composer_attachment_button");
    auto *composer_input = required_ui_child<Ui::InputField>(
        window, "lingtai_composer_input");
    auto *send_button = required_ui_child<Ui::RoundButton>(
        window, "lingtai_composer_send_button");
    auto *composer_status = required_child<QLabel>(
        window, "lingtai_composer_status");
    auto *resize_handle = required_child<QWidget>(
        window, "lingtai_roster_resize_handle");
    auto *separator = required_ui_child<Ui::PlainShadow>(
        window, "lingtai_roster_separator");
    auto *detail_scroll = required_child<QScrollArea>(
        window, "lingtai_agent_detail_scroll");
    auto *project_selector = required_child<QPushButton>(
        window, "lingtai_project_selector");

    auto *titlebar = [&]() -> Ui::Platform::TitleWidget * {
        for (auto *child : window.findChildren<QWidget *>(
                QString(), Qt::FindDirectChildrenOnly)) {
            if (auto *candidate = dynamic_cast<Ui::Platform::TitleWidget *>(child)) {
                return candidate;
            }
        }
        return nullptr;
    }();
    auto *titlebar_brand = required_child<QLabel>(
        window, "lingtai_titlebar_brand");
    require(titlebar != nullptr && titlebar->isVisible() && titlebar->height() > 0,
        "Telegram's real macOS TitleWidget row must be restored instead of zero-height hidden");
    require(titlebar_brand->parent() == titlebar,
        "the pure LingTai brand must be painted inside Telegram's TitleWidget owner");
    require(window.body()->geometry().top() == titlebar->height(),
        "the content body must begin below the restored unified title row");
    const auto native_anchor = titlebar_brand->property(
        "lingtai_native_traffic_light_anchor").toPoint();
    require(native_anchor.x() > 0
            && qAbs(titlebar_brand->geometry().left() - native_anchor.x()) <= 1
            && qAbs(titlebar_brand->geometry().center().y() - native_anchor.y()) <= 1,
        "the title-row brand must begin after the green button and share its vertical center");
    require(project_selector->parent() != window.body().get(),
        "the functional project selector must remain in the Sidebar");

    // Ted's real-window acceptance is stricter than the source oracle: the
    // macOS titlebar must visibly share the app base, and the white columns
    // must meet without even the optional one-pixel Telegram side shadow.
    require(window.windowFlags().testFlag(Qt::NoTitleBarBackgroundHint),
        "the macOS title bar must stay transparent over the app-owned base");
    require(window.palette().color(QPalette::Window) == st::windowBg->c,
        "the Qt window palette must own the shared windowBg base");
    require(window.windowHandle() != nullptr
            && window.property("lingtai_window_surface_color").value<QColor>()
                == st::windowBg->c,
        "the native window surface must receive windowBg, not only QWidget palette");
    const auto handle_image = resize_handle->grab().toImage();
    require(handle_image.pixelColor(
                handle_image.width() / 2, handle_image.height() / 2)
            == st::windowBg->c,
        "the wide resize target must paint windowBg");
    require(!separator->isVisible(),
        "responsive recompute must not re-show the center divider");
    require(detail_scroll->frameShape() == QFrame::NoFrame,
        "the main detail scroll must not draw a rectangular pane border");

    const auto painted_bounds = [](const QImage &image, QColor surface) {
        auto bounds = QRect();
        for (auto y = 0; y != image.height(); ++y) {
            for (auto x = 0; x != image.width(); ++x) {
                if (image.pixelColor(x, y) != surface) {
                    bounds = bounds.isNull()
                        ? QRect(x, y, 1, 1)
                        : bounds.united(QRect(x, y, 1, 1));
                }
            }
        }
        return bounds;
    };
    const auto require_centered_paint = [&](QWidget *widget, const char *message) {
        const auto image = widget->grab().toImage();
        const auto bounds = painted_bounds(image, image.pixelColor(0, 0));
        const auto tolerance = qMax(1, qCeil(image.devicePixelRatio()));
        require(!bounds.isNull()
                && qAbs(bounds.left() + bounds.right() - (image.width() - 1))
                    <= tolerance
                && qAbs(bounds.top() + bounds.bottom() - (image.height() - 1))
                    <= tolerance,
            message);
    };
    require_centered_paint(attachment_button,
        "the paperclip ink must be mathematically centered in its 40px lane");
    require_centered_paint(send_button,
        "the circular Send paint must be centered in its 40px lane");

    const auto bounds_for_color = [](const QImage &image, QColor target) {
        auto bounds = QRect();
        for (auto y = 0; y != image.height(); ++y) {
            for (auto x = 0; x != image.width(); ++x) {
                if (image.pixelColor(x, y) != target) {
                    continue;
                }
                bounds = bounds.isNull()
                    ? QRect(x, y, 1, 1)
                    : bounds.united(QRect(x, y, 1, 1));
            }
        }
        return bounds;
    };
    const auto require_color_ink_centered = [&](
            QWidget *widget,
            QColor ink,
            bool horizontal,
            const char *message) {
        const auto image = widget->grab().toImage();
        const auto bounds = bounds_for_color(image, ink);
        const auto tolerance = qMax(1, qCeil(image.devicePixelRatio()));
        const auto x_delta = bounds.isNull()
            ? image.width()
            : bounds.left() + bounds.right() - (image.width() - 1);
        const auto y_delta = bounds.isNull()
            ? image.height()
            : bounds.top() + bounds.bottom() - (image.height() - 1);
        require(!bounds.isNull()
                && (!horizontal || qAbs(x_delta) <= tolerance)
                && qAbs(y_delta) <= tolerance,
            std::string(message)
                + ": x_delta=" + std::to_string(x_delta)
                + ", y_delta=" + std::to_string(y_delta));
    };
    const auto message_controls_image = controls->grab().toImage();
    const auto controls_scale = message_controls_image.devicePixelRatio();
    const auto input_left = qRound(
        composer_input->mapTo(controls, QPoint()).x() * controls_scale);
    const auto input_right = input_left
        + qRound(composer_input->width() * controls_scale) - 1;
    auto message_bounds = QRect();
    for (auto y = 0; y != message_controls_image.height(); ++y) {
        for (auto x = input_left; x <= input_right; ++x) {
            if (message_controls_image.pixelColor(x, y)
                != st::defaultInputField.placeholderFg->c) {
                continue;
            }
            message_bounds = message_bounds.isNull()
                ? QRect(x, y, 1, 1)
                : message_bounds.united(QRect(x, y, 1, 1));
        }
    }
    const auto message_y_delta = message_bounds.isNull()
        ? message_controls_image.height()
        : message_bounds.top() + message_bounds.bottom()
            - (message_controls_image.height() - 1);
    require(!message_bounds.isNull()
            && qAbs(message_y_delta) <= qMax(1, qCeil(controls_scale)),
        "the Message ink must be vertically centered in the whole Composer capsule: y_delta="
            + std::to_string(message_y_delta));
    require_color_ink_centered(
        send_button,
        st::defaultActiveButton.textFg->c,
        true,
        "the white Send arrow ink must be centered inside the blue circle");

    const auto project = sandbox / "project";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    write_file(project / ".lingtai/alpha/.agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-a001",)"
        R"("agent_name":"alpha","address":"alpha","state":"active"})");
    const auto fixture_before = tree_snapshot(project);
    const auto outcome = shell.open_project(project, std::nullopt);
    require(outcome.disposition == ProjectOpenDisposition::opened,
        "the modern-composer fixture project must open");
    require(tree_snapshot(project) == fixture_before,
        "opening the modern-composer fixture must remain read-only");
    require(project_selector->text() == QStringLiteral("project"),
        "the Sidebar project selector must show the active folder basename");
    click_first_agent_canvas_row(window);
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("alpha"),
        "the modern-composer fixture Agent must be selectable");

    require(attachment_button->parent() == controls
            && composer_input->parent() == controls
            && send_button->parent() == controls,
        "attachment, Message input and Send must share one immediate rounded "
        "controls container");
    require(attachment_button->text().isEmpty()
            && attachment_button->accessibleName() == QStringLiteral("Attach file"),
        "attachment must be icon-only while retaining its accessible name");
    require(send_button->width() == send_button->height()
            && send_button->width() <= 44,
        "Send must keep one compact square layout box around its 40px painted circle");

    // The empty/status read-out is owned by the composer lane, not a
    // separate dashboard row under the detail.
    auto *status_owner = composer_status->parent();
    while (status_owner != nullptr && status_owner != composer) {
        status_owner = status_owner->parent();
    }
    require(status_owner == composer,
        "lingtai_composer_status must be a descendant of "
        "lingtai_composer, never a separate detail row");

    // Wide state: the input and Send action rects, captured in one common
    // composer-lane coordinate system, keep a compact aligned action row.
    window.resize(1200, 800);
    QCoreApplication::processEvents();
    require(composer->isVisible() && composer_input->isVisible()
            && send_button->isVisible(),
        "the modern composer and its action row must be visible in a wide "
        "detail");
    const auto attachment_rect = QRect(
        attachment_button->mapTo(controls, QPoint(0, 0)),
        attachment_button->size());
    const auto input_rect = QRect(
        composer_input->mapTo(controls, QPoint(0, 0)),
        composer_input->size());
    const auto send_rect = QRect(
        send_button->mapTo(controls, QPoint(0, 0)),
        send_button->size());

    // Attachment, field and circular Send stay one compact aligned action row
    // inside the shared container.
    require(qAbs(attachment_rect.center().y() - input_rect.center().y()) <= 2,
        "attachment and Message input must align on one compact row");
    require(attachment_rect.right() < input_rect.left(),
        "attachment must sit to the left of Message in the shared container");
    require(qAbs(input_rect.center().y() - send_rect.center().y()) <= 2,
        "the composer input and Send action must align on one compact row");
    const auto attachment_center_delta =
        attachment_rect.center().y() - controls->rect().center().y();
    const auto send_center_delta =
        send_rect.center().y() - controls->rect().center().y();
    require(qAbs(attachment_center_delta) <= 1 && qAbs(send_center_delta) <= 1,
        "both 40px icon lanes must be vertically centered in the shared Composer: attachment="
            + std::to_string(attachment_center_delta)
            + ", send=" + std::to_string(send_center_delta));
    require(input_rect.right() < send_rect.left(),
        "the composer input must sit to the left of Send in the same row");

    const auto controls_image = controls->grab().toImage();
    require(controls_image.width() >= 3 && controls_image.height() >= 3,
        "the shared Composer controls container must render a real surface");
    const auto interior = controls_image.pixelColor(
        controls_image.width() / 2, controls_image.height() / 2);
    const auto border = controls_image.pixelColor(controls_image.width() / 2, 0);
    const auto corner = controls_image.pixelColor(0, 0);
    require(border != interior,
        "the shared Composer container must paint one subtle visible border");
    require(corner == interior,
        "the shared Composer container must leave rounded outer corners on "
        "the common background");
    auto *detail = required_child<Ui::RpWidget>(window, "lingtai_agent_detail");
    const auto composer_bottom = composer->mapTo(detail, QPoint(0, 0)).y()
        + composer->height();
    require(detail->height() - composer_bottom >= 8,
        "the Composer must stay inset from the bottom edge");

    // Narrow 380 minimum: the input/Send row stays near-full relative to
    // its immediate composer-lane owner.
    window.resize(380, 480);
    QCoreApplication::processEvents();
    require(composer->isVisible() && composer_input->isVisible()
            && send_button->isVisible(),
        "the modern composer must stay visible at the narrow minimum");
    const auto narrow_attachment_rect = QRect(
        attachment_button->mapTo(controls, QPoint(0, 0)),
        attachment_button->size());
    const auto narrow_input_rect = QRect(
        composer_input->mapTo(controls, QPoint(0, 0)),
        composer_input->size());
    const auto narrow_send_rect = QRect(
        send_button->mapTo(controls, QPoint(0, 0)),
        send_button->size());
    const auto narrow_lane = narrow_attachment_rect.united(
        narrow_input_rect).united(narrow_send_rect);
    require(narrow_lane.width() * 4 >= controls->width() * 3,
        "a narrow composer must keep attachment+input+Send near-full");

    // The Vision HIGH Send contract at the real visual sizes (1100x720,
    // 820x620, 640x520): the composer field/status surface stays present,
    // and the Send action remains visible with its rect fully contained
    // inside the composer lane and a real painted foreground
    // distinguishable from its surface -- never a blank object-tree
    // presence.
    for (const auto &[width, height] : std::vector<std::pair<int, int>>{
             {1100, 720}, {820, 620}, {640, 520}}) {
        window.resize(width, height);
        QCoreApplication::processEvents();
        require(composer->isVisible() && composer_input->isVisible()
                && send_button->isVisible(),
            "the composer field and Send action must stay visible at every "
            "real visual size");
        const auto send_rect = QRect(
            send_button->mapTo(composer, QPoint(0, 0)),
            send_button->size());
        require(composer->rect().contains(send_rect),
            "the Send action rect must stay fully contained inside the "
            "composer band at every real visual size");
        const auto send_image = send_button->grab().toImage();
        const auto dpr = std::max(1.0, double(send_image.devicePixelRatio()));
        const auto surface = send_image.pixelColor(0, 0);
        auto foreground_pixels = 0;
        for (auto y = 0; y != send_image.height(); ++y) {
            for (auto x = 0; x != send_image.width(); ++x) {
                if (send_image.pixelColor(x, y) != surface) {
                    ++foreground_pixels;
                }
            }
        }
        require(foreground_pixels >= int(4.0 * dpr * dpr),
            "the Send action must paint a distinguishable foreground over "
            "its surface, not a single-color blank grab");
    }

    // A large detail must use the horizontal room for the shared Composer rail.
    // The fixed accessory and action lanes keep their accepted sizes; only the
    // flex input lane absorbs this additional width.
    window.resize(1600, 900);
    QCoreApplication::processEvents();
    require(controls->width() * 100 >= detail->width() * 94,
        "a very wide Composer rail must occupy at least 94% of the detail pane");
    require(attachment_button->width() == 40
            && send_button->width() == send_button->height()
            && send_button->width() <= 44,
        "widening the outer rail must not scale the fixed accessory/action lanes");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "modern-composer fixtures must be removed");
}

// The floating-composer contract: the composer stays one bounded floating
// surface inset from both detail edges with a compact arrow send -- never
// fonts, focus, or the send flow itself (already proven by
// verify_composer_send_behavior and verify_modern_composer_surface).
void verify_floating_composer_surface(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *detail = required_child<Ui::RpWidget>(
        window, "lingtai_agent_detail");
    auto *composer = required_child<Ui::RpWidget>(
        window, "lingtai_composer");
    auto *send_button = required_ui_child<Ui::RoundButton>(
        window, "lingtai_composer_send_button");

    const auto project = sandbox / "project";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    write_file(project / ".lingtai/alpha/.agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-a001",)"
        R"("agent_name":"alpha","address":"alpha","state":"active"})");
    const auto fixture_before = tree_snapshot(project);
    const auto outcome = shell.open_project(project, std::nullopt);
    require(outcome.disposition == ProjectOpenDisposition::opened,
        "the floating-composer fixture project must open");
    require(tree_snapshot(project) == fixture_before,
        "opening the floating-composer fixture must remain read-only");
    click_agent(window, "alpha");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("alpha"),
        "the floating-composer fixture Agent must be selectable");

    window.resize(1200, 800);
    QCoreApplication::processEvents();

    // One bounded floating composer, never a full-width dark band under the
    // conversation: the surface must stay inset from both detail edges.
    require(composer->isVisible() && send_button->isVisible(),
        "the floating composer and its send action must be visible");
    require(composer->width() < detail->width(),
        "the composer must be a bounded floating surface, never the "
        "full-width st::msgInBg dark band");
    const auto composer_left = composer->mapTo(detail, QPoint(0, 0)).x();
    const auto composer_right =
        detail->width() - composer_left - composer->width();
    require(composer_left > 0 && composer_right > 0,
        "the floating composer must sit inset from both detail edges");

    // Compact icon send: an arrow-sized control with no literal text caption.
    require(send_button->width() <= 48,
        "the send action must be a compact icon control, never a wide text "
        "Send button");
    require(send_button->accessibilityName() != QStringLiteral("Send"),
        "the send action must be an arrow icon, not a literal Send caption");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "floating-composer fixtures must be removed");
}

// The R4 RED: the selected-Agent chat top bar must drop its secondary
// `lingtai_selected_agent_key` metadata before any primary control when the
// actual header width is constrained. A wide actual header shows the
// presentation name, the secondary key, and the applicable primary controls
// (Start for a stale target plus always-present Request sleep; Back only in
// the narrow OneColumn detail) together without overlap; a constrained
// OneColumn actual detail header keeps the presentation name and every
// primary control visible while the secondary key hides first; returning to
// wide restores the key. Current production keeps `lingtai_selected_agent_key`
// visible at the constrained width, so this journey must fail exactly that
// visibility, never fonts, icon substitution, or a hidden primary control.
void verify_responsive_header_priority(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *top_bar = required_child<QWidget>(window, "lingtai_chat_top_bar");
    auto *detail_scroll = required_child<QScrollArea>(
        window, "lingtai_agent_detail_scroll");
    auto *presentation_name = required_child<QLabel>(
        window, "lingtai_selected_agent_presentation_name");
    auto *detail_key = required_child<QLabel>(
        window, "lingtai_selected_agent_key");
    auto *back_button = required_child<QPushButton>(
        window, "lingtai_agent_detail_back");
    auto *start_button = required_child<QPushButton>(
        window, "lingtai_selected_agent_start_agent");
    auto *sleep_button = required_child<QPushButton>(
        window, "lingtai_selected_agent_request_sleep");

    // An eligible non-human stale Agent with the same moderate-length distinct
    // directory key/presentation-name shape as the validated narrow render, so
    // metadata priority is proved before a single trailing value can wrap.
    const auto stale_key = "personal_research_companion";
    const auto project = sandbox / "project";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    const auto stale_dir = project / ".lingtai" / stale_key;
    write_file(stale_dir / ".agent.json",
        R"({"agent_id":"20260101-000000-a001",)"
        R"("agent_name":"personal_research_companion",)"
        R"("nickname":"Personal Research Companion",)"
        R"("address":"personal_research_companion","admin":{},"state":"idle"})");
    write_file(stale_dir / ".agent.heartbeat", "0");
    const auto fixture_before = tree_snapshot(project);
    const auto outcome = shell.open_project(project, std::nullopt);
    require(outcome.disposition == ProjectOpenDisposition::opened,
        "the responsive-header fixture project must open");
    require(tree_snapshot(project) == fixture_before,
        "opening the responsive-header fixture must remain read-only");

    // Public widget geometry in one common ancestor coordinate system.
    const auto header_rect = [&](QWidget *widget) {
        return QRect(widget->mapTo(top_bar, QPoint(0, 0)), widget->size());
    };
    const auto require_disjoint = [&](std::vector<QWidget *> widgets) {
        for (auto index = std::size_t{0}; index != widgets.size(); ++index) {
            require(!widgets[index]->size().isEmpty(),
                "every visible header widget must own real geometry");
            for (auto other = index + 1; other != widgets.size(); ++other) {
                require(!header_rect(widgets[index])
                            .intersects(header_rect(widgets[other])),
                    "visible header widgets must never overlap");
            }
        }
    };

    // Wide actual header: presentation name and secondary key beside the
    // applicable primary controls, all visible without overlap.
    window.resize(1200, 800);
    QCoreApplication::processEvents();
    click_agent(window, stale_key);
    require(shell.selection_state().selected_agent_directory_key()
            == std::optional<fs::path>(stale_key),
        "the stale target must be selectable");
    QCoreApplication::processEvents();
    require(presentation_name->isVisible() && detail_key->isVisible(),
        "a wide actual header must show the presentation name and the "
        "secondary key together");
    require(start_button->isVisible() && start_button->isEnabled(),
        "the stale target must show the applicable Start primary control");
    require(sleep_button->isVisible(),
        "Request sleep must remain visible in the wide header");
    require(!back_button->isVisible(),
        "Back must stay hidden in wide two-column mode");
    require_disjoint(
        {presentation_name, detail_key, start_button, sleep_button});

    // The validated 640x520 narrow render must already prioritize the primary
    // identity and controls before the long directory-key metadata can wrap or
    // make the vertical detail pane horizontally scrollable.
    window.resize(640, 520);
    QCoreApplication::processEvents();
    require(!detail_key->wordWrap(),
        "secondary header metadata must stay single-line before fit priority "
        "hides it");
    require(detail_scroll->horizontalScrollBarPolicy()
            == Qt::ScrollBarAlwaysOff,
        "the selected-Agent detail pane is vertical-only at narrow widths");
    require(detail_scroll->horizontalScrollBar()->maximum() == 0
            && !detail_scroll->horizontalScrollBar()->isVisible(),
        "the validated 640px narrow detail pane must not overflow horizontally");

    // Minimum OneColumn actual detail header: the presentation name and every
    // primary control stay visible while the secondary key remains hidden.
    window.resize(380, 480);
    QCoreApplication::processEvents();
    require(presentation_name->isVisible(),
        "a constrained actual header must keep the presentation name");
    require(back_button->isVisible(),
        "a constrained OneColumn actual detail header must keep Back");
    require(start_button->isVisible() && start_button->isEnabled(),
        "a constrained actual header must keep the Start primary control");
    require(sleep_button->isVisible(),
        "a constrained actual header must keep Request sleep");
    require(!detail_key->isVisible(),
        "a constrained actual header must hide the secondary key first, "
        "before any primary control or the presentation name");
    require(detail_scroll->horizontalScrollBar()->maximum() == 0
            && !detail_scroll->horizontalScrollBar()->isVisible(),
        "the vertical Agent detail surface must not overflow horizontally at "
        "a valid narrow size");

    // Returning to wide restores the secondary key beside the presentation
    // name and the primary controls.
    window.resize(1200, 800);
    QCoreApplication::processEvents();
    require(presentation_name->isVisible() && detail_key->isVisible(),
        "returning to wide must restore the secondary key");
    require(start_button->isVisible() && sleep_button->isVisible()
            && !back_button->isVisible(),
        "returning to wide must restore the same wide primary control set");
    require_disjoint(
        {presentation_name, detail_key, start_button, sleep_button});

    // Wide selected-Agent workspace hierarchy: the content surface, the
    // active project route, and the Agent pages nav are the durable
    // pre-production contract for the modern workspace.
    auto *project_route = required_child<QWidget>(
        window, "lingtai_project_route");
    auto *agent_pages_nav = required_child<QWidget>(
        window, "lingtai_agent_pages_nav");
    auto *conversation_nav = required_child<QWidget>(
        window, "lingtai_agent_page_nav_conversation");
    auto *presets_nav = required_child<QWidget>(
        window, "lingtai_agent_page_nav_presets");
    auto *conversation_heading = required_child<QWidget>(
        window, "lingtai_selected_agent_conversation_heading");

    require(!conversation_heading->isVisible(),
        "the Agent detail page must show exactly one Conversation "
        "affordance, hiding the duplicate selected-Agent heading");
    require(conversation_nav->isVisible(),
        "the Conversation page nav must remain the visible affordance");
    require(project_route->geometry().top() == 0,
        "the active project route must begin at the content origin so "
        "hidden empty-route branding cannot leave a shared spacer above "
        "the workspace");

    auto *pages_nav_layout = qobject_cast<QHBoxLayout *>(
        agent_pages_nav->layout());
    require(pages_nav_layout != nullptr,
        "the Agent pages nav must own a horizontal layout");
    require(pages_nav_layout->count() >= 3,
        "the Agent pages nav must hold the two buttons and trailing room");
    require(pages_nav_layout->itemAt(0)->widget() == conversation_nav
            && pages_nav_layout->itemAt(1)->widget() == presets_nav,
        "the two page buttons must lead the Agent pages nav");
    require(pages_nav_layout->stretch(0) == 0
            && pages_nav_layout->stretch(1) == 0,
        "the two page buttons must not stretch");
    auto *trailing_room = pages_nav_layout->itemAt(2)->spacerItem();
    require(trailing_room != nullptr && pages_nav_layout->stretch(2) > 0,
        "the Agent pages nav must trail a positive-stretch spacer");

    // (A) The checked Conversation page affordance must paint its active
    // state through real semantic palette accents: some
    // `st::dialogsTextFgService` accent text over at most a bounded minority
    // of `st::dialogsBgActive` surface -- never a full-slab solid background.
    required_child<QPushButton>(window, "lingtai_agent_page_nav_conversation")
        ->click();
    QCoreApplication::processEvents();
    const auto conversation_nav_image = conversation_nav->grab().toImage();
    auto accent_pixels = 0;
    auto active_pixels = 0;
    for (auto y = 0; y != conversation_nav_image.height(); ++y) {
        for (auto x = 0; x != conversation_nav_image.width(); ++x) {
            if (conversation_nav_image.pixelColor(x, y)
                    == st::dialogsTextFgService->c) {
                ++accent_pixels;
            } else if (conversation_nav_image.pixelColor(x, y)
                    == st::dialogsBgActive->c) {
                ++active_pixels;
            }
        }
    }
    require(accent_pixels > 0,
        "the checked Conversation page affordance must paint real "
        "st::dialogsTextFgService accent pixels, not a blank grab");
    require(active_pixels * 4 < conversation_nav_image.width()
                * conversation_nav_image.height(),
        "the checked Conversation page must keep st::dialogsBgActive to a "
        "bounded minority (<25%) of its surface, never the full-slab "
        "background");

    // (B) The Start/Sleep status read-outs belong inside their own action
    // rows, never as separate full-width detail siblings.
    auto *start_status = required_child<QLabel>(
        window, "lingtai_selected_agent_start_status");
    auto *sleep_status = required_child<QLabel>(
        window, "lingtai_selected_agent_sleep_status");
    auto *start_row = required_child<Ui::RpWidget>(
        window, "lingtai_selected_agent_start_row");
    auto *sleep_row = required_child<Ui::RpWidget>(
        window, "lingtai_selected_agent_sleep_row");
    require(start_status->parentWidget() == start_row,
        "the Start status must be parented inside its own "
        "lingtai_selected_agent_start_row, never a separate full-width "
        "detail band");
    require(sleep_status->parentWidget() == sleep_row,
        "the Request sleep status must be parented inside its own "
        "lingtai_selected_agent_sleep_row, never a separate full-width "
        "detail band");

    // (C) The Vision HIGH at the real visual sizes (1100x720, 820x620,
    // 640x520): the selected presentation name must keep a meaningful share
    // of the actual top bar -- derived from the current top bar width and
    // capped at a modest readable width, never hardcoded screenshot pixels --
    // and must retain a readable prefix of its full identity instead of
    // collapsing to a few characters. Elision may shorten the visible text
    // but must never replace the full selected-Agent identity production
    // owns on the accessible description and the retained dynamic property.
    // Every visible nonempty Start/Sleep status must also fit its own
    // allocated width, and secondary guidance may be hidden before the
    // identity is ever destroyed.
    const auto full_identity = QStringLiteral("Personal Research Companion");
    for (const auto &[width, height] : std::vector<std::pair<int, int>>{
             {1100, 720}, {820, 620}, {640, 520}}) {
        window.resize(width, height);
        QCoreApplication::processEvents();
        require(presentation_name->isVisible(),
            "the selected presentation name must stay visible at every real "
            "visual size");
        const auto meaningful_share = std::min(top_bar->width() / 3, 240);
        require(presentation_name->width() >= meaningful_share,
            "the selected presentation name must keep a meaningful share of "
            "the actual top bar, capped at a modest readable width, instead "
            "of collapsing far below the available space");
        auto visible_identity = presentation_name->text();
        if (visible_identity.endsWith(QChar(0x2026))) {
            visible_identity.chop(1);
        }
        require(visible_identity.size()
                    >= std::min(full_identity.size(), qsizetype(12))
                && full_identity.startsWith(visible_identity),
            "visible elision must retain a readable prefix of the full "
            "selected identity, never collapsing to a few characters");
        require(presentation_name->property("lingtai_full_text").toString()
                    == full_identity
                && presentation_name->accessibleDescription() == full_identity,
            "visible elision must never replace the full selected-Agent "
            "identity production keeps on the accessible description");
        for (auto *status : {start_status, sleep_status}) {
            if (status->isVisible() && !status->text().isEmpty()) {
                require(QFontMetrics(status->font())
                        .horizontalAdvance(status->text()) <= status->width(),
                    "every visible nonempty Start/Sleep status must fit its "
                    "own allocated width, never clip inside its action row");
            }
        }
    }

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "responsive-header fixtures must be removed");
}

// The U1/U2 composer-command contract: every leading-slash command in this
// journey is submitted through the real InputField Return path. Local
// navigation/help/quit and selected-Agent sleep/start must never become
// DirectPublisher mail, while argument-bearing lifecycle and later commands
// remain deliberately unavailable in this slice.
void verify_conversation_slash_interception(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *sidebar = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_sidebar");
    auto *content = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_content");
    auto *input = required_ui_child<Ui::InputField>(
        window, "lingtai_composer_input");
    auto *send_button = required_ui_child<Ui::RoundButton>(
        window, "lingtai_composer_send_button");
    auto *status = required_child<QLabel>(
        window, "lingtai_composer_status");
    auto *start_status = required_child<QLabel>(
        window, "lingtai_selected_agent_start_status");
    auto *sleep_status = required_child<QLabel>(
        window, "lingtai_selected_agent_sleep_status");
    auto *conversation_nav = required_child<QPushButton>(
        window, "lingtai_agent_page_nav_conversation");
    auto *presets_nav = required_child<QPushButton>(
        window, "lingtai_agent_page_nav_presets");

    const auto project = sandbox / "project";
    const auto target = project / ".lingtai/alpha";
    const auto outbox = project / ".lingtai/human/mailbox/outbox";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    write_file(target / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-a001",)"
        R"("agent_name":"alpha","address":"alpha","state":"active"})");
    write_file(target / ".agent.heartbeat", std::to_string(
        std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count()));

    const auto beta = project / ".lingtai/beta";
    write_file(beta / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-b001",)"
        R"("agent_name":"beta","address":"beta","state":"idle"})");
    const auto beta_python = sandbox / "runtime-beta/bin/python";
    const auto beta_argv = sandbox / "argv-beta.txt";
    write_fixture_python(beta_python, beta_argv, std::nullopt, 0);
    const auto beta_runtime = beta_python.parent_path().parent_path();
    for (const auto &agent : {target, beta}) {
        write_file(agent / "init.json",
            QStringLiteral(R"({"venv_path":"%1"})")
                .arg(path_text(beta_runtime)).toStdString());
    }

    const auto outbox_leaf_count = [&] {
        auto count = std::size_t{0};
        if (!fs::exists(outbox)) return count;
        for (const auto &entry : fs::directory_iterator(outbox)) {
            static_cast<void>(entry);
            ++count;
        }
        return count;
    };
    const auto submit_command = [&](const QString &command) {
        input->setText(command);
        input->setFocus();
        QCoreApplication::processEvents();
        auto enter = QKeyEvent(
            QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(input->rawTextEdit(), &enter);
        QCoreApplication::processEvents();
    };

    window.resize(380, 480);
    QCoreApplication::processEvents();
    const auto outcome = shell.open_project(project, std::nullopt);
    require(outcome.disposition == ProjectOpenDisposition::opened,
        "the slash-interception fixture project must open");
    click_agent(window, "alpha");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("alpha")
            && input->isEnabled() && send_button->isEnabled(),
        "a selected valid Agent must enable the composer before slash input");

    submit_command(QStringLiteral("/presets"));
    require(input->getLastText().isEmpty(),
        "raw /presets must clear the composer after local submission");
    require(outbox_leaf_count() == 0,
        "raw /presets must stay local and create no human outbox leaf");
    require(presets_nav->isChecked() && !conversation_nav->isChecked(),
        "raw /presets must check the existing Presets page");

    // Presets intentionally hides the composer. Return through the existing
    // page button, then use `/agents` from a real narrow selected-detail state
    // so the roster and focus transition are observable.
    conversation_nav->click();
    QCoreApplication::processEvents();
    require(conversation_nav->isChecked() && input->isVisible(),
        "the existing Conversation page must restore the real composer");
    submit_command(QStringLiteral("/agents"));
    require(input->getLastText().isEmpty(),
        "raw /agents must clear the composer after local submission");
    require(outbox_leaf_count() == 0,
        "raw /agents must stay local and create no human outbox leaf");
    require(!shell.selection_state().selected_agent_directory_key()
            && sidebar->isVisible() && !content->isVisible(),
        "raw /agents must return a narrow selected detail to the existing roster");
    require(window.focusWidget() == agent_row(window, "alpha"),
        "raw /agents must return focus to a usable existing Agent row");

    // Re-enter the same valid route for the command-status checks.
    click_agent(window, "alpha");
    require(input->isEnabled() && send_button->isEnabled(),
        "reselecting the valid Agent must re-enable the composer");
    submit_command(QStringLiteral("/sleep"));
    require(input->getLastText().isEmpty(),
        "raw /sleep must clear the composer after local submission");
    require(outbox_leaf_count() == 0,
        "raw /sleep must stay local and create no human outbox leaf");
    require(fs::exists(target / ".sleep"),
        "raw empty-form /sleep must create the selected alpha/.sleep "
        "lifecycle signal");
    require(read_file(target / ".sleep").empty(),
        "raw empty-form /sleep must create the exact zero-byte .sleep marker");
    require(sleep_status->text() == QStringLiteral("Sleep requested."),
        "raw empty-form /sleep must report the existing owner's exact "
        "Sleep requested. status");

    submit_command(QStringLiteral("/cpr"));
    require(input->getLastText().isEmpty(),
        "raw /cpr must clear the composer after local submission");
    require(outbox_leaf_count() == 0,
        "raw /cpr on live alpha must stay local and create no outbox leaf");
    require(start_status->text() == QStringLiteral("Agent is already online."),
        "raw empty-form /cpr on live alpha must report the exact truthful "
        "TUI-equivalent status in the existing Start surface");
    require(!fs::exists(beta_argv)
            && !fs::exists(target / "logs/agent.log"),
        "raw empty-form /cpr on live alpha must launch nothing");

    submit_command(QStringLiteral("/help"));
    require(input->getLastText().isEmpty(),
        "raw /help must clear the composer after local submission");
    require(outbox_leaf_count() == 0,
        "raw /help must stay local and create no human outbox leaf");
    const auto help = status->text();
    require(!help.isEmpty() && help.size() <= 512,
        "raw /help must expose one bounded nonempty local response");
    for (const auto *command : {
             "/agents", "/presets", "/sleep", "/cpr", "/clear", "/refresh",
             "/suspend", "/help", "/quit"}) {
        require(help.contains(QString::fromLatin1(command)),
            std::string("raw /help must expose the available command ")
                + command);
    }
    for (const auto *command : {"/start", "/wake"}) {
        require(!help.contains(
                    QString::fromLatin1(command), Qt::CaseInsensitive),
            std::string("raw /help must not claim the unavailable command ")
                + command);
    }
    require(fs::exists(target / ".sleep")
            && !fs::exists(beta / ".sleep") && !fs::exists(beta_argv),
        "raw /help must not create, remove, or launch any Agent lifecycle signal");

    // The U5 lifecycle-command contract, injected through the same
    // `set_tui_executable` seam the shipped TUI uses: the fake control
    // executable never runs a real TUI or Agent, records exact separate argv,
    // and answers one canonical success JSON after a short deterministic
    // delay.
    const auto control_executable = sandbox / "lingtai-tui-control";
    const auto control_argv = sandbox / "argv-control.txt";
    const auto control_done = sandbox / "control-done.txt";
    write_fixture_control(control_executable, control_argv, control_done, 1);
    shell.set_tui_executable(control_executable);

    const auto exact_control_argv = QStringLiteral(
        "control\n--project\n%1\n--agent\nalpha\nrefresh\ncodex-preset\n")
        .arg(path_text(fs::canonical(project / ".lingtai"))).toStdString();
    const auto pending_status = QStringLiteral("Agent command pending.");
    const auto duplicate_status =
        QStringLiteral("Agent command already pending.");
    const auto success_status = QStringLiteral("Refresh signaled.");
    const auto failure_status = QStringLiteral("Agent command failed.");
    const auto wait_for_control_done = [&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!fs::exists(control_done)
                && std::chrono::steady_clock::now() < deadline) {
            QThread::msleep(20);
            QCoreApplication::processEvents();
        }
        require(fs::exists(control_done),
            "the fake control command must complete within the bound");
        // The marker is written just before the child exits, so the runner's
        // QProcess finished callback lands only after a short bounded event
        // drain; never assert a status while the callback could still arrive.
        const auto drain_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (std::chrono::steady_clock::now() < drain_deadline) {
            QThread::msleep(20);
            QCoreApplication::processEvents();
        }
    };
    const auto remove_control_done = [&] {
        std::error_code ignore;
        fs::remove(control_done, ignore);
    };

    // Exact separate argv for `/refresh codex-preset` through the real
    // composer/Send path, with the preset argument passed through unchanged.
    submit_command(QStringLiteral("/refresh codex-preset"));
    require(input->getLastText().isEmpty(),
        "raw /refresh codex-preset must clear the composer after local "
        "submission");
    require(outbox_leaf_count() == 0,
        "raw /refresh codex-preset must stay local and create no outbox leaf");
    const auto refresh_argv_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!fs::exists(control_argv)
            && std::chrono::steady_clock::now() < refresh_argv_deadline) {
        QThread::msleep(20);
    }
    require(fs::exists(control_argv),
        "raw /refresh codex-preset must invoke the injected fake control "
        "executable");
    require(read_file(control_argv) == exact_control_argv,
        "raw /refresh codex-preset must pass the exact separate argv control, "
        "--project, the fixture .lingtai root, --agent, alpha, refresh, and "
        "the unchanged codex-preset argument");
    require(status->text() == pending_status,
        "an accepted lifecycle command must show only the exact pending "
        "status while running, never a predeclared success");

    // A duplicate lifecycle slash while the first command is still pending is
    // rejected without a second fake invocation.
    submit_command(QStringLiteral("/clear"));
    require(input->getLastText().isEmpty(),
        "raw /clear during a pending lifecycle command must clear the "
        "composer");
    require(read_file(control_argv) == exact_control_argv,
        "a duplicate lifecycle slash while pending must not invoke the fake "
        "control executable a second time");
    require(status->text() == duplicate_status,
        "a duplicate lifecycle slash while pending must report the exact "
        "truthful already-pending status");

    // Stale-result isolation: the alpha command finishes only after the
    // roster has moved away to beta, so its success/failure must never
    // surface under beta.
    click_agent(window, "beta");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("beta"),
        "beta must be selected while the alpha control command is pending");
    wait_for_control_done();
    require(status->text() != success_status
            && status->text() != failure_status,
        "an old alpha completion must not surface as a success or failure "
        "under the later-selected beta");

    // Away-and-back isolation: returning to alpha while the next command is
    // still pending must also discard the stale completion.
    click_agent(window, "alpha");
    remove_control_done();
    submit_command(QStringLiteral("/refresh codex-preset"));
    click_agent(window, "beta");
    click_agent(window, "alpha");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("alpha"),
        "the away-and-back re-selection must leave alpha selected");
    wait_for_control_done();
    require(status->text() != success_status
            && status->text() != failure_status,
        "an old alpha completion must not surface as a success or failure "
        "after an away-and-back re-selection");

    // The exact successful refresh outcome is shown only when the project,
    // Agent, and generation still match.
    remove_control_done();
    submit_command(QStringLiteral("/refresh codex-preset"));
    wait_for_control_done();
    require(status->text() == success_status,
        "a matching project/Agent/generation must show the exact successful "
        "refresh outcome after the callback");
    require(read_file(control_argv) == exact_control_argv
            + exact_control_argv + exact_control_argv,
        "the journey must have invoked the fake control executable exactly "
        "three times with identical exact separate argv");

    click_agent(window, "beta");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("beta")
            && input->isEnabled() && send_button->isEnabled(),
        "the stale beta fixture must be selected before CPR command checks");
    const auto unavailable = QStringLiteral(
        "Command not available in this Desktop build.");
    for (const auto &command : {
             QStringLiteral("/sleep all"), QStringLiteral("/cpr all"),
             QStringLiteral("/sleep later"), QStringLiteral("/cpr later")}) {
        submit_command(command);
        require(input->getLastText().isEmpty(),
            "argument-bearing lifecycle commands must remain commands and "
            "clear the composer");
        require(outbox_leaf_count() == 0,
            "argument-bearing lifecycle commands must stay local and create "
            "no human outbox leaf");
        require(status->text() == unavailable,
            "all and other lifecycle arguments must report the exact "
            "unavailable-in-this-build status");
    }
    require(!fs::exists(beta / ".sleep") && !fs::exists(beta_argv),
        "argument-bearing /sleep and /cpr must neither signal nor launch beta");

    submit_command(QStringLiteral("/definitely-unknown"));
    require(input->getLastText().isEmpty(),
        "an unknown slash command must remain a command and clear the composer");
    require(outbox_leaf_count() == 0,
        "an unknown slash command must stay local and create no outbox leaf");
    require(status->text() == unavailable,
        "an unknown slash command must keep the exact unavailable status");
    require(!fs::exists(beta / ".sleep") && !fs::exists(beta_argv),
        "an unknown slash command must create no Agent lifecycle signal");

    const auto cpr_started = std::chrono::steady_clock::now();
    submit_command(QStringLiteral("/cpr"));
    const auto cpr_elapsed = std::chrono::steady_clock::now() - cpr_started;
    require(input->getLastText().isEmpty(),
        "raw /cpr must clear the composer after local submission");
    require(outbox_leaf_count() == 0,
        "raw /cpr on stale beta must stay local and create no outbox leaf");
    require(cpr_elapsed < std::chrono::seconds(1),
        "raw empty-form /cpr must return promptly through the existing Start "
        "owner, never blocking the composer");
    require(start_status->text() == QStringLiteral("Starting Agent..."),
        "raw empty-form /cpr on stale beta must show the existing Start "
        "owner's exact pending status");
    require(!fs::exists(beta / ".sleep"),
        "raw empty-form /cpr must not create a sleep signal");

    const auto argv_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!fs::exists(beta_argv)
            && std::chrono::steady_clock::now() < argv_deadline) {
        QThread::msleep(20);
    }
    require(fs::exists(beta_argv),
        "raw empty-form /cpr must invoke beta's configured fixture runtime");
    const auto exact_beta_argv = QStringLiteral("-m\nlingtai\nrun\n%1\n")
        .arg(path_text(fs::canonical(beta))).toStdString();
    require(read_file(beta_argv) == exact_beta_argv,
        "raw empty-form /cpr argv must be the exact separate four lines -m, "
        "lingtai, run, and beta's absolute canonical directory");

    submit_command(QStringLiteral("/quit"));
    require(input->getLastText().isEmpty(),
        "raw /quit must clear the composer after local submission");
    require(outbox_leaf_count() == 0,
        "raw /quit must stay local and create no human outbox leaf");
    require(!fs::exists(beta / ".sleep")
            && read_file(beta_argv) == exact_beta_argv,
        "raw /quit must create no additional Agent lifecycle signal");
    require(!window.isVisible(),
        "raw /quit must close the real Desktop window locally");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "slash-interception fixtures must be removed");
}

// The Stage-1 two-surface shell contract on the single light canvas: every
// main surface (`lingtai_desktop_sidebar`, `lingtai_desktop_content`, and the
// composer) sits on the shared base `windowBg` token. The selected-Agent
// header, page nav, conversation, and composer regions must not paint their
// own boxed dark backgrounds or hard-edged plain-shadow frames, and no
// surface may paint an elevated (`windowBgOver`) band.
void verify_two_surface_hierarchy(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *sidebar = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_sidebar");
    auto *content = required_child<Ui::RpWidget>(
        window, "lingtai_desktop_content");
    auto *detail = required_child<Ui::RpWidget>(
        window, "lingtai_agent_detail");
    auto *composer = required_child<Ui::RpWidget>(
        window, "lingtai_composer");

    const auto project = sandbox / "project";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    write_file(project / ".lingtai/alpha/.agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-a001",)"
        R"("agent_name":"alpha","address":"alpha","state":"active"})");
    window.resize(1200, 800);
    QCoreApplication::processEvents();
    const auto outcome = shell.open_project(project, std::nullopt);
    require(outcome.disposition == ProjectOpenDisposition::opened,
        "the two-surface fixture project must open");
    click_agent(window, "alpha");
    require(shell.selection_state().selected_agent_directory_key()
            == std::optional<fs::path>("alpha"),
        "alpha must be selectable for the two-surface hierarchy");
    QCoreApplication::processEvents();

    // On the single light canvas every main surface paints the same base
    // windowBg fill: the sidebar, content, and composer all sit on the one
    // single-canvas base token, so no header/nav/conversation/composer surface
    // may paint a boxed fill of its own or an elevated band.
    require(sidebar->grab().toImage().pixelColor(2, 2)
            == st::windowBg->c,
        "the sidebar must sit on the single-canvas base surface (windowBg)");
    require(content->grab().toImage().pixelColor(2, 2) == st::windowBg->c,
        "the content must be on the single-canvas base surface (windowBg)");
    require(composer->grab().toImage().pixelColor(2, 2) == st::windowBg->c,
        "the composer must sit on the single-canvas base surface, never a "
        "full-width dark band of its own");

    // No hard-edged plain-shadow frames around the header/nav/conversation/
    // composer: the only plain-shadow separator in the whole shell divides
    // the two main surfaces, so no plain shadow may live inside the detail.
    auto shadow_frames = 0;
    for (auto *child : detail->findChildren<QWidget *>()) {
        if (dynamic_cast<Ui::PlainShadow *>(child)) ++shadow_frames;
    }
    require(shadow_frames == 0,
        "no hard-edged plain-shadow frame may box the header, nav, "
        "conversation, or composer inside the detail");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "two-surface fixtures must be removed");
}

int main(int argc, char **argv) {
    // Test-local execution modes: the exact binary with a fresh fixture root
    // and one literal flag runs only the R1 resizable-sidebar, R4
    // responsive-header, M4 modern-composer, or U1/U2 slash-interception journey,
    // so the warm RED/GREEN never has to pass unrelated accepted-base debt.
    const auto responsive_sidebar_only = argc == 3
        && std::string_view(argv[2]) == "--responsive-sidebar-only";
    const auto responsive_header_only = argc == 3
        && std::string_view(argv[2]) == "--responsive-header-only";
    const auto modern_composer_only = argc == 3
        && std::string_view(argv[2]) == "--modern-composer-only";
    const auto slash_interception_only = argc == 3
        && std::string_view(argv[2]) == "--slash-interception-only";
    const auto compact_header_only = argc == 3
        && std::string_view(argv[2]) == "--compact-header-only";
    const auto two_surface_only = argc == 3
        && std::string_view(argv[2]) == "--two-surface-only";
    const auto plain_underline_only = argc == 3
        && std::string_view(argv[2]) == "--plain-underline-only";
    const auto floating_composer_only = argc == 3
        && std::string_view(argv[2]) == "--floating-composer-only";
    if (argc != 2 && !responsive_sidebar_only && !responsive_header_only
            && !modern_composer_only && !slash_interception_only
            && !compact_header_only && !two_surface_only
            && !plain_underline_only && !floating_composer_only) {
        std::cerr << "usage: native_shell_test PROJECT_ROOT "
                     "[--responsive-sidebar-only|--responsive-header-only|"
                     "--modern-composer-only|--slash-interception-only|"
                     "--compact-header-only|--two-surface-only|"
                     "--plain-underline-only|--floating-composer-only]\n";
        return 2;
    }
    try {
        const auto project_root = std::filesystem::canonical(argv[1]);
        std::filesystem::current_path(project_root);
        QApplication application(argc, argv);
        if (responsive_sidebar_only) {
            lingtai::desktop::NativeShell shell;
            shell.show_offscreen();
            QCoreApplication::processEvents();
            verify_resizable_sidebar(shell, project_root);
            std::cout << "native shell behavior: OK\n";
            return 0;
        }
        if (responsive_header_only) {
            lingtai::desktop::NativeShell shell;
            shell.show_offscreen();
            QCoreApplication::processEvents();
            verify_responsive_header_priority(shell, project_root);
            std::cout << "native shell behavior: OK\n";
            return 0;
        }
        if (modern_composer_only) {
            lingtai::desktop::NativeShell shell;
            shell.show_offscreen();
            QCoreApplication::processEvents();
            verify_modern_composer_surface(
                shell, project_root / "commit-m4-composer-surface-fixture");
            std::cout << "native shell behavior: OK\n";
            return 0;
        }
        if (slash_interception_only) {
            lingtai::desktop::NativeShell shell;
            shell.show_offscreen();
            QCoreApplication::processEvents();
            verify_conversation_slash_interception(
                shell, project_root / "u2-slash-interception-fixture");
            std::cout << "native shell behavior: OK\n";
            return 0;
        }
        if (compact_header_only) {
            lingtai::desktop::NativeShell shell;
            shell.show_offscreen();
            QCoreApplication::processEvents();
            verify_compact_header_hierarchy(
                shell, project_root / "commit-32-compact-header-fixture");
            std::cout << "native shell behavior: OK\n";
            return 0;
        }
        if (two_surface_only) {
            lingtai::desktop::NativeShell shell;
            shell.show_offscreen();
            QCoreApplication::processEvents();
            verify_two_surface_hierarchy(
                shell, project_root / "commit-s1-two-surface-fixture");
            std::cout << "native shell behavior: OK\n";
            return 0;
        }
        if (plain_underline_only) {
            lingtai::desktop::NativeShell shell;
            shell.show_offscreen();
            QCoreApplication::processEvents();
            verify_plain_underline_page_tabs(
                shell, project_root / "commit-tab-plain-underline-fixture");
            std::cout << "native shell behavior: OK\n";
            return 0;
        }
        if (floating_composer_only) {
            lingtai::desktop::NativeShell shell;
            shell.show_offscreen();
            QCoreApplication::processEvents();
            verify_floating_composer_surface(
                shell, project_root / "commit-fc-floating-composer-fixture");
            std::cout << "native shell behavior: OK\n";
            return 0;
        }
        const auto original_palette = QApplication::palette();
        verify_dark_application_palette_inheritance(
            project_root / "commit-8-palette-fixture");
        require(QApplication::palette() == original_palette,
            "dark palette test must restore the application palette");
        lingtai::desktop::NativeShell shell;
        shell.show_offscreen();
        QCoreApplication::processEvents();
        verify_live_system_palette(shell);
        verify_removed_activity_and_task_card_destinations(shell);
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
        verify_request_sleep_action(
            shell, project_root / "commit-16-sleep-fixture");
        verify_start_agent_action(
            shell, project_root / "commit-17-start-fixture");
        verify_agent_preset_summary_panel(
            shell, project_root / "commit-19-preset-summary-fixture");
        verify_layout(shell, project_root / "commit-30-responsive-fixture");
        verify_resizable_sidebar(
            shell, project_root / "commit-r1-resizable-sidebar-fixture");
        verify_selected_agent_dashboard_layout(
            shell, project_root / "commit-28-dashboard-fixture");
        verify_telegram_theme_reset(
            shell, project_root / "commit-31-theme-reset-fixture");
        verify_two_surface_hierarchy(
            shell, project_root / "commit-s1-two-surface-fixture");
        verify_modern_composer_surface(
            shell, project_root / "commit-m4-composer-surface-fixture");
        verify_plain_underline_page_tabs(
            shell, project_root / "commit-tab-plain-underline-fixture");
        verify_floating_composer_surface(
            shell, project_root / "commit-fc-floating-composer-fixture");
        std::cout << "native shell behavior: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "native shell behavior: " << error.what() << '\n';
        return 1;
    }
}
