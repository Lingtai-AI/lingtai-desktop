#include "native_shell.h"
#include "shell_host.h"
#include "agent_projection.h"
#include "preset_editor_model.h"
#include "project_setup_wizard.h"
#include "runtime_options.h"
#include "ui/object_names.h"

#include "styles/palette.h"
#include "ui/UiTestFonts.h"
#include "ui/platform/mac/ui_window_title_mac.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/rp_window.h"
#include "ui/widgets/shadow.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
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
#include <QtGui/QTextFrame>
#include <QtGui/QTextLayout>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpacerItem>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QTreeWidget>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
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
using lingtai::desktop::AgentManifestKind;
using lingtai::desktop::AgentRole;
using lingtai::desktop::AgentRow;
using lingtai::desktop::AgentSnapshot;
using lingtai::desktop::ProjectOpenDisposition;
using lingtai::desktop::project_agents;

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

void require_within_detail_viewport(
        QWidget &window, QWidget &page, const char *what) {
    auto *scroll = required_child<QScrollArea>(
        window, "lingtai_agent_detail_scroll");
    require(page.isVisible(), std::string(what) + ": page must be visible");
    require(page.width() <= scroll->viewport()->width() + 1,
        std::string(what) + ": width must fit the detail viewport");
    const auto viewport_right = scroll->viewport()->mapTo(
        &window, QPoint(scroll->viewport()->width(), 0)).x();
    const auto page_right = page.mapTo(
        &window, QPoint(page.width(), 0)).x();
    require(viewport_right - page_right >= 0,
        std::string(what) + ": right edge must stay inside the viewport");
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
        // Write via temp+rename so a waiter cannot observe an empty truncated
        // target from `>` and classify the heartbeat as invalid.
        script += "( sleep " + std::to_string(delay_seconds)
            + "; date +%s > \"" + heartbeat_path->string() + ".tmp\""
            + " && mv \"" + heartbeat_path->string() + ".tmp\" \""
            + heartbeat_path->string() + "\" ) &\n";
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

QWidget *roster_rows_canvas(QWidget &window) {
    return required_child<QWidget>(window, "lingtai_agent_roster_rows");
}

int canvas_row_height(QWidget *canvas) {
    auto primary_font = canvas->font();
    primary_font.setPointSize(13);
    primary_font.setWeight(QFont::DemiBold);
    auto secondary_font = canvas->font();
    secondary_font.setPointSize(12);
    const auto text_height = QFontMetrics(primary_font).height()
        + QFontMetrics(secondary_font).height();
    return std::max(40, text_height) + 16;
}

int canvas_row_stride(QWidget *canvas) {
    return canvas_row_height(canvas) + 2;
}

QPointF canvas_row_click_point(QWidget *canvas, int index) {
    const auto row_height = canvas_row_height(canvas);
    return QPointF(
        canvas->width() * 0.5,
        index * canvas_row_stride(canvas) + row_height * 0.5);
}

std::vector<AgentRow> visible_agent_rows(const AgentSnapshot &snapshot) {
    auto rows = snapshot.items;
    rows.erase(std::remove_if(rows.begin(), rows.end(),
        [](const AgentRow &item) { return item.role == AgentRole::human; }),
        rows.end());
    return rows;
}

std::vector<AgentRow> visible_agent_rows(lingtai::desktop::NativeShell &shell) {
    require(shell.selection_state().active_project().has_value(),
        "visible_agent_rows requires an active project");
    return visible_agent_rows(
        project_agents(*shell.selection_state().active_project()));
}

std::size_t visible_row_index(
        lingtai::desktop::NativeShell &shell,
        std::string_view key) {
    const auto rows = visible_agent_rows(shell);
    const auto expected = fs::path(key);
    for (std::size_t index = 0; index != rows.size(); ++index) {
        if (rows[index].directory_key == expected) {
            return index;
        }
    }
    throw std::runtime_error("missing Agent row: " + std::string(key));
}

bool is_agent_selected(
        lingtai::desktop::NativeShell &shell,
        std::string_view key) {
    return shell.selection_state().selected_agent_directory_key()
        == std::optional<fs::path>(std::string(key));
}

bool is_agent_selectable(
        lingtai::desktop::NativeShell &shell,
        std::string_view key) {
    const auto rows = visible_agent_rows(shell);
    const auto expected = fs::path(key);
    for (const auto &row : rows) {
        if (row.directory_key == expected) {
            return row.manifest_kind == AgentManifestKind::valid;
        }
    }
    return false;
}

QString agent_row_facts(
        lingtai::desktop::NativeShell &shell,
        std::string_view key) {
    const auto rows = visible_agent_rows(shell);
    const auto expected = fs::path(key);
    for (const auto &row : rows) {
        if (row.directory_key == expected) {
            return QStringLiteral("%1 — %2 — %3")
                .arg(row.manifest_kind == AgentManifestKind::valid
                        ? QStringLiteral("valid")
                        : QStringLiteral("malformed"),
                    row.role == AgentRole::main
                        ? QStringLiteral("main")
                        : row.role == AgentRole::agent
                            ? QStringLiteral("agent")
                            : QStringLiteral("unknown"),
                    row.presence == lingtai::desktop::AgentPresenceKind::alive
                        ? QStringLiteral("alive")
                        : row.presence
                            == lingtai::desktop::AgentPresenceKind::stale
                            ? QStringLiteral("stale")
                            : row.presence
                                == lingtai::desktop::AgentPresenceKind::missing
                                ? QStringLiteral("missing")
                                : QStringLiteral("unknown"));
        }
    }
    throw std::runtime_error("missing Agent row facts: " + std::string(key));
}

void click_agent_canvas_row(QWidget &window, int index) {
    auto *canvas = roster_rows_canvas(window);
    const auto point = canvas_row_click_point(canvas, index);
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

void click_first_agent_canvas_row(QWidget &window) {
    click_agent_canvas_row(window, 0);
}

void click_agent(lingtai::desktop::NativeShell &shell, std::string_view key) {
    click_agent_canvas_row(
        shell.window(),
        static_cast<int>(visible_row_index(shell, key)));
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
    auto *roster_scroll = required_child<QWidget>(
        window, "lingtai_agent_roster_scroll");
    auto *detail = required_child<Ui::RpWidget>(
        window, "lingtai_agent_detail");
    const auto labels = std::vector<QLabel *>{
        required_child<QLabel>(window, "lingtai_product_title"),
        required_child<QLabel>(window, "lingtai_product_purpose"),
        required_child<QLabel>(window, "lingtai_project_route_heading"),
        required_child<QLabel>(window, "lingtai_project_root"),
        required_child<QLabel>(window, "lingtai_agent_selection_error"),
        required_child<QLabel>(window, "lingtai_agent_roster_heading"),
    };
    auto *roster_state = required_child<QLabel>(
        window, "lingtai_agent_roster_state");
    // The no-project route, open-error and bootstrap-status surfaces are the
    // first-project presentation slice: they now use the same lib_ui label
    // language as the accepted dashboard, exposed through the FlatLabel's
    // accessibility text.
    const auto flat_labels = std::vector<Ui::FlatLabel *>{
        required_ui_child<Ui::FlatLabel>(window, "lingtai_no_project_title"),
        required_ui_child<Ui::FlatLabel>(window, "lingtai_no_project_detail"),
        required_ui_child<Ui::FlatLabel>(window, "lingtai_project_open_error"),
    };
    auto *startup_route_widget = required_child<QWidget>(
        window, "lingtai_startup_route");
    auto *open_button = required_child<QPushButton>(
        window, "lingtai_open_project_button");
    auto *project_selector = required_child<QPushButton>(
        window, "lingtai_project_selector");

    const auto token_window = st::windowBg->c;
    const auto check_token_window_surface = [&](QWidget *surface) {
        const auto actual = surface->palette().color(QPalette::Window);
        if (actual != token_window) {
            throw std::runtime_error(
                std::string("Telegram windowBg must reach every painted shell "
                    "surface; mismatch on ")
                    + surface->objectName().toStdString()
                    + " expected "
                    + token_window.name().toStdString()
                    + " got "
                    + actual.name().toStdString());
        }
    };
    check_token_window_surface(static_cast<QWidget *>(body));
    check_token_window_surface(static_cast<QWidget *>(sidebar));
    check_token_window_surface(static_cast<QWidget *>(content));
    check_token_window_surface(static_cast<QWidget *>(empty_route));
    check_token_window_surface(static_cast<QWidget *>(error_surface));
    check_token_window_surface(static_cast<QWidget *>(project_route));
    check_token_window_surface(static_cast<QWidget *>(directory));
    check_token_window_surface(static_cast<QWidget *>(roster));
    check_token_window_surface(static_cast<QWidget *>(detail));
    check_token_window_surface(startup_route_widget);
    for (const auto *label : labels) {
        const auto actual = label->palette().color(QPalette::WindowText);
        if (actual != window_ink) {
            throw std::runtime_error(
                std::string("dark application WindowText role must reach every "
                    "shell label; mismatch on ")
                    + label->objectName().toStdString()
                    + " expected " + window_ink.name().toStdString()
                    + " got " + actual.name().toStdString());
        }
        require(label->textFormat() == Qt::PlainText,
            "every LingTai label surface must render explicit plain text");
    }
    require(roster_state->palette().color(QPalette::WindowText)
            == st::windowSubTextFg->c,
        "the compact roster count must use the tertiary windowSubTextFg tone");
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
    require(project_selector->palette().color(QPalette::Button) == button_surface,
        "dark application Button role must reach the project selector");
    require(roster_scroll->palette().color(QPalette::Base) == text_surface,
        "dark application Base role must reach the roster scroll surface");

    ProjectFixture fixture(sandbox, "palette");
    static_cast<void>(shell.open_project(fixture.project, std::nullopt));
    auto *canvas = roster_rows_canvas(window);
    require(canvas->palette().color(QPalette::Window) == token_window,
        "Telegram windowBg must reach the virtual roster canvas");
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

    style_hints->setColorScheme(Qt::ColorScheme::Light);
    QApplication::processEvents();
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
    QCoreApplication::processEvents();
    require(label_text(window, "lingtai_agent_roster_state").isEmpty()
            && visible_agent_rows(shell).empty()
            && tree_snapshot(empty_root) == empty_before,
        "an empty complete roster must be distinct and read-only");

    ProjectFixture roster(sandbox, "roster-red");
    write_file(roster.agent / ".agent.json", R"({
        "agent_id":"manifest-agent-id",
        "agent_name":"Immutable Agent Name",
        "nickname":"Research Nickname",
        "address":"agent@example.test",
        "state":"active",
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
        require(agent_row_facts(shell, key).contains(QString::fromStdString(facts)),
            key + " must expose exact manifest, role, and presence truth");
        require(is_agent_selectable(shell, key) == (key != "malformed"),
            key + " selectability must derive from valid manifest truth");
    }
    const auto rendered = visible_agent_rows(shell);
    auto visible_keys = std::vector<std::string>();
    for (const auto &row : rendered) {
        visible_keys.push_back(row.directory_key.string());
    }
    require(visible_keys == std::vector<std::string>{
            ampersand_key, plain_neighbor_key, "agent",
            "b-main", "c-stale", "d-missing", "malformed"},
        "native rows must render in the composite snapshot's deterministic order");
    require(is_agent_selectable(shell, ampersand_key),
        "ampersand row selectability must retain the exact key");
    const auto roster_status = label_text(window, "lingtai_agent_roster_state");
    require(roster_status == QStringLiteral("7"),
        "a complete roster reports the visible Agent count");

    click_agent_canvas_row(
        window,
        static_cast<int>(visible_row_index(shell, "malformed")));
    require(!shell.selection_state().selected_agent_directory_key(),
        "a disabled malformed row must not change C1 truth");

    const auto roster_before_selection = tree_snapshot(roster.project);
    click_agent(shell, ampersand_key);
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>(ampersand_key)
            && is_agent_selected(shell, ampersand_key)
            && !is_agent_selected(shell, plain_neighbor_key)
            && label_text(window, "lingtai_selected_agent_presentation_name")
                == QStringLiteral("A&B-agent")
            && label_text(window, "lingtai_selected_agent_key")
                == QStringLiteral("Agent · Suspended")
            && required_child<QLabel>(window, "lingtai_selected_agent_key")
                ->textFormat() == Qt::PlainText,
        "a key-fallback header must keep one title and manifest role · "
        "state below it");
    require(tree_snapshot(roster.project) == roster_before_selection,
        "ampersand selection at its exact project-relative path must remain read-only");
    click_agent(shell, "agent");
    require(shell.selection_state().selected_agent_directory_key()
            == std::optional<fs::path>("agent")
            && is_agent_selected(shell, "agent"),
        "detail and highlight must derive from sole C1 selected-key truth");
    require(label_text(window, "lingtai_selected_agent_presentation_name")
            == QStringLiteral("Research Nickname"),
        "selected detail must prefer the manifest nickname as its presentation name");
    require(label_text(window, "lingtai_selected_agent_key")
            == QStringLiteral("Agent · Active"),
        "a distinct presentation title must keep one compact role · "
        "lifecycle state line below the name");
    require(label_text(window, "lingtai_selected_agent_manifest_identity")
            == QStringLiteral("Manifest identity\naddress: agent@example.test\n"
                "agent ID: manifest-agent-id\nstate: active"),
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

    click_agent(shell, "b-main");
    require(label_text(window, "lingtai_selected_agent_status_context")
                == QStringLiteral("Status context (source values)\nwindow size: 100\n"
                    "system tokens: -3\ntools tokens: -4\nhistory tokens: -5\n"
                    "total tokens: -12\nusage_percent (source usage_pct): -7.5\n"
                    "fixed tokens: -8\ngrowing tokens: -9")
            && window.findChildren<QProgressBar *>().empty(),
        "odd negative context must remain plain source evidence without a gauge");

    click_agent(shell, "d-missing");
    require(label_text(window, "lingtai_selected_agent_status_activity")
                == QStringLiteral("Status activity unavailable from status source"),
        "absent status must remain explicit unavailable evidence");

    click_agent(shell, "c-stale");
    require(label_text(window, "lingtai_selected_agent_status_context")
                == QStringLiteral(
                    "Status context unavailable (no valid positive window projected)")
            && !label_text(window, "lingtai_selected_agent_status_context")
                .contains("88"),
        "invalid window must suppress the whole unprojected context");
    click_agent(shell, "agent");

    click_agent(shell, "b-main");
    require(shell.selection_state().selected_agent_directory_key()
            == std::optional<fs::path>("b-main")
            && is_agent_selected(shell, "b-main"),
        "row clicks must use the same C1-owning selection handler");
    click_agent(shell, "agent");
    const auto refreshed = open_without_writes(shell, roster, std::nullopt);
    require(refreshed.disposition == ProjectOpenDisposition::opened
            && shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("agent")
            && is_agent_selected(shell, "agent"),
        "same-root refresh must preserve a still-valid selection and detail");
    write_file(roster.agent / ".agent.json", "{");
    const auto malformed_before_refresh = tree_snapshot(roster.project);
    const auto repaired = shell.open_project(roster.project, std::nullopt);
    require(repaired.disposition == ProjectOpenDisposition::opened
            && !shell.selection_state().selected_agent_directory_key()
            && !is_agent_selectable(shell, "agent")
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
    QCoreApplication::processEvents();
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
    const auto prior_presentation = label_text(
        window, "lingtai_selected_agent_presentation_name");
    const auto later_failure = shell.open_project(ordinary);
    require(later_failure.disposition == ProjectOpenDisposition::failed,
        "failed reopen must return failed");
    require(shell.selection_state().active_project()
            && shell.selection_state().active_project()->root() == active_root,
        "failed reopen must preserve the valid active project");
    require(shell.selection_state().selected_agent_directory_key()
            == prior_selection
            && label_text(window, "lingtai_selected_agent_presentation_name")
                == prior_presentation,
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

// Folder menu "Open Project in Another Window" must keep the requester on its
// current project while opening the chosen path in a second shell window.
void verify_open_project_in_another_window(const fs::path &sandbox) {
    using lingtai::desktop::ui_test::kOpenProjectNewWindowButton;
    using lingtai::desktop::ShellHost;
    using lingtai::desktop::RuntimeOptions;

    RuntimeOptions options;
    options.offscreen_mode = true;
    options.ui_test_mode = true;
    ShellHost host(options);
    require(host.shell_count() == 1,
        "ShellHost must start with exactly one window");

    auto &primary = host.primary();
    primary.show_offscreen();
    QCoreApplication::processEvents();

    auto *new_window_button = required_child<QPushButton>(
        primary.window(), kOpenProjectNewWindowButton);
    auto request_count = std::size_t{0};
    primary.set_open_project_in_new_window_request_handler([&] {
        ++request_count;
    });
    new_window_button->click();
    require(request_count == 1,
        "Open Project in Another Window must fire exactly one request");

    const auto first = sandbox / "window-a";
    const auto second = sandbox / "window-b";
    write_file(first / ".lingtai/alpha/.agent.json", R"({"admin":{}})");
    write_file(second / ".lingtai/beta/.agent.json", R"({"admin":{}})");
    const auto first_root = fs::canonical(first);
    const auto second_root = fs::canonical(second);

    const auto opened = primary.open_project(first, std::nullopt);
    require(opened.disposition == ProjectOpenDisposition::opened,
        "primary window must open the first project");
    require(primary.selection_state().active_project()
            && primary.selection_state().active_project()->root() == first_root,
        "primary window must keep the first project root");

    host.open_path_in_new_window(primary, second);
    QCoreApplication::processEvents();
    require(host.shell_count() == 2,
        "Open in Another Window must create a second shell");
    require(primary.selection_state().active_project()
            && primary.selection_state().active_project()->root() == first_root,
        "primary window must remain on its project after spawning another");

    auto &secondary = host.shell_at(1);
    require(secondary.selection_state().active_project()
            && secondary.selection_state().active_project()->root()
                == second_root,
        "new window must open the second project");
    require(label_text(primary.window(), "lingtai_project_root").toStdString()
            == first_root.string(),
        "primary project-root label must stay on the first project");
    require(label_text(secondary.window(), "lingtai_project_root").toStdString()
            == second_root.string(),
        "new window project-root label must show the second project");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "multi-window fixtures must be removed");
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
    auto *startup_route = required_child<QWidget>(
        window, "lingtai_startup_route");
    auto *project_route = required_child<Ui::RpWidget>(
        window, "lingtai_project_route");
    auto *open_error = required_ui_child<Ui::FlatLabel>(
        window, "lingtai_project_open_error");
    auto *startup_heading = required_child<QLabel>(
        window, "lingtai_startup_heading");
    auto *startup_tagline = required_child<QLabel>(
        window, "lingtai_startup_tagline");
    auto *choose_project = required_child<QPushButton>(
        window, "lingtai_startup_choose_project");

    require(window.objectName() == "lingtai_desktop_window",
        "window semantic name changed");
    require(body != &window, "RpWindow body must be a real child RpWidget");
    require(body->objectName() == "lingtai_desktop_body",
        "body semantic name changed");
    require(window.accessibleName() == "LingTai Desktop",
        "window needs an accessible product name");
    require(window.minimumWidth() == 380,
        "window minimum width must be Telegram's source-backed 380px");
    require(window.minimumHeight() >= 480,
        "window minimum height must stay at least Telegram's source-backed "
        "480px once native titlebar chrome is included");
    require(startup_route->accessibleName() == "Choose a LingTai project",
        "startup route needs an accessible region name");
    require(startup_heading->text() == "LingTai Orchestration",
        "startup heading changed");
    require(startup_tagline->text()
            == "Awaken under Bodhi\nOne soul, thousand avatars",
        "startup tagline changed");
    require(choose_project->text() == QStringLiteral("Choose project"),
        "choose-project affordance text changed");
    require(choose_project->accessibleName() == "Choose project",
        "choose-project affordance needs a static accessible name");

    const auto &selection = shell.selection_state();
    require(!selection.active_project().has_value(),
        "new shell must have no active project");
    require(!selection.selected_agent_directory_key().has_value(),
        "new shell must have no selected Agent");
    require(startup_route->isVisible(),
        "no-workspace truth must show the startup route");
    require(!sidebar->isVisible() && !content->isVisible(),
        "no-workspace truth must hide the sidebar and content columns");
    require(!project_route->isVisible(),
        "new shell must hide the project route");
    require(!open_error->isVisible(),
        "new shell must hide the project open error");

    const auto tree_before = project_tree(project_root);
    auto callback_count = std::size_t{0};
    shell.set_open_project_request_handler([&] {
        ++callback_count;
    });
    choose_project->click();
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
    auto *detail = required_child<Ui::RpWidget>(window, "lingtai_agent_detail");
    const auto *detail_layout = detail->layout();
    auto index_of = [&](QWidget *widget) {
        for (auto index = 0; index != detail_layout->count(); ++index) {
            if (detail_layout->itemAt(index)->widget() == widget) return index;
        }
        throw std::runtime_error("detail child is not in the detail layout");
    };
    auto layout_index_for = [&](QWidget *widget) {
        for (auto index = 0; index != detail_layout->count(); ++index) {
            auto *item = detail_layout->itemAt(index);
            if (item->widget() == widget) return index;
            if (auto *nested = item->layout()) {
                for (auto sub = 0; sub != nested->count(); ++sub) {
                    if (nested->itemAt(sub)->widget() == widget) return index;
                }
            }
        }
        throw std::runtime_error("detail child is not in the detail layout");
    };
    auto *composer = required_child<Ui::RpWidget>(window, "lingtai_composer");
    require(index_of(surface) < index_of(required_child<QLabel>(
                window, "lingtai_selected_agent_manifest_identity")),
        "the conversation must not be buried below the manifest detail labels");
    require(index_of(heading) < index_of(surface),
        "the conversation heading must precede the conversation surface");
    require(layout_index_for(composer) > index_of(surface),
        "the composer lane must sit below the conversation surface");
    require(state->parentWidget() == composer,
        "conversation state must live in the composer lane beneath the surface");

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
    require(surface->toPlainText().contains(QStringLiteral("Select an agent")),
        "opening without a selection must prompt for one");

    click_agent(shell, "telegram-bot");
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
    require(state->text() == QStringLiteral("1 skipped"),
        "the compact state must show the skipped count without a message "
        "total");

    // The real QTextDocument must expose the two directions as distinct
    // message lanes: incoming Agent rows carry sender metadata on the first
    // block of a group, while outgoing Human rows are body-only bubbles on the
    // opposite edge. Distinct bubble backgrounds are proven separately in
    // verify_telegram_theme_reset; here only the lane alignment is asserted.
    auto incoming_header = QTextBlock();
    auto outgoing_body = QTextBlock();
    for (auto block = surface->document()->begin();
            block != surface->document()->end();
            block = block.next()) {
        if (block.text().startsWith(QStringLiteral("Telegram Bot ·"))) {
            incoming_header = block;
        } else if (block.text().contains(
                QStringLiteral("Thanks, reviewing tomorrow."))) {
            outgoing_body = block;
        }
    }
    require(incoming_header.isValid() && outgoing_body.isValid(),
        "the conversation must expose real incoming and outgoing message blocks");
    require(incoming_header.blockFormat().leftMargin()
            < incoming_header.blockFormat().rightMargin(),
        "incoming Agent bubbles must stay left-anchored in the reading column");
    require(outgoing_body.blockFormat().leftMargin()
            > outgoing_body.blockFormat().rightMargin(),
        "outgoing Human bubbles must stay right-anchored in the reading column");

    // The real incoming mail body above was written through a JSON `\n`
    // escape, so the kernel-side decode delivers an authentic newline to the
    // surface, never a literal backslash+n. Markdown normalization may split
    // the body across multiple aligned blocks, but the full literal text must
    // remain selectable and copyable as plain text.
    require(surface->toPlainText().contains(
                QStringLiteral("PR published, not merged."))
            && surface->toPlainText().contains(
                QStringLiteral("<b>#1223</b> & <not-a-tag>")),
        "the full literal multiline body must remain selectable and copyable "
        "in toPlainText");
    auto left_anchored = 0;
    auto right_anchored = 0;
    for (auto block = surface->document()->begin();
            block != surface->document()->end();
            block = block.next()) {
        if (block.text().trimmed().isEmpty()) continue;
        const auto left = block.blockFormat().leftMargin();
        const auto right = block.blockFormat().rightMargin();
        if (left + 1 < right) ++left_anchored;
        if (left > right + 1) ++right_anchored;
    }
    require(left_anchored >= 1 && right_anchored >= 1,
        "one incoming multiline mail plus one outgoing mail must render with "
        "both left- and right-anchored message lanes");

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
    while (!surface->toPlainText().contains(QStringLiteral("Filler line one."))
            && std::chrono::steady_clock::now() < filler_deadline) {
        QThread::msleep(50);
        QCoreApplication::processEvents();
    }
    require(surface->toPlainText().contains(QStringLiteral("Filler line one.")),
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
    click_agent(shell, "issue-643");
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

    click_agent(shell, "telegram-bot");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("telegram-bot"),
        "the first Agent must be selectable");
    require(input->isEnabled() && send_button->isEnabled()
            && input->getLastText().isEmpty(),
        "a selected valid route must enable an empty composer");

    auto outbox_entry_count = [&] {
        if (!fs::exists(outbox)) return std::size_t{0};
        return static_cast<std::size_t>(std::distance(
            fs::directory_iterator(outbox), fs::directory_iterator{}));
    };

    input->setText(QStringLiteral("   \t  "));
    const auto before_whitespace = tree_snapshot(project);
    const auto outbox_before_whitespace = outbox_entry_count();
    send_button->clicked(Qt::NoModifier, Qt::LeftButton);
    require(outbox_entry_count() == outbox_before_whitespace,
        "whitespace-only input must be rejected without writing anything");
    require(input->getLastText() == QStringLiteral("   \t  "),
        "a rejected whitespace-only send must preserve the typed input");
    require(status->text() != QStringLiteral("Queued"),
        "a rejected whitespace-only send must not claim success");
    require(tree_snapshot(project) == before_whitespace,
        "a rejected whitespace-only send must write nothing");

    input->setText(QStringLiteral("Ted, the slice is complete."));
    const auto outbox_before_success = outbox_entry_count();
    send_button->clicked(Qt::NoModifier, Qt::LeftButton);
    require(input->getLastText().isEmpty(),
        "a successful send must clear the composer");
    require(status->text().isEmpty(),
        "a successful send attaches a receipt to the bubble instead of a "
        "composer Queued label");
    require(surface->toPlainText().contains(
                QStringLiteral("Ted, the slice is complete.")),
        "a successful send must refresh the conversation to show the new row");
    require(fs::exists(outbox), "a successful send must create the outbox folder");
    require(outbox_entry_count() == outbox_before_success + 1,
        "exactly one leaf must be created by the one successful send");
    auto first_leaves = std::vector<fs::path>();
    for (const auto &entry : fs::directory_iterator(outbox)) {
        first_leaves.push_back(entry.path());
    }
    require(std::ranges::any_of(first_leaves, [](const auto &leaf) {
            const auto body = read_file(leaf / "message.json");
            return body.find("\"to\":[\"telegram-bot\"]") != std::string::npos
                && body.find("Ted, the slice is complete.") != std::string::npos;
        }),
        "the queued entry must address exactly the selected Agent");

    // Pressing Enter in the nonempty composer must submit through the same
    // send path: queue exactly one addressed outbox leaf and clear the input.
    input->setText(QStringLiteral("Entered via the Return key."));
    const auto outbox_before_enter = outbox_entry_count();
    auto enter = QKeyEvent(
        QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(input->rawTextEdit(), &enter);
    require(input->getLastText().isEmpty(),
        "pressing Enter in the nonempty composer must clear the input");
    require(outbox_entry_count() == outbox_before_enter + 1,
        "pressing Enter must queue exactly one more addressed outbox leaf");
    auto enter_leaves = std::vector<fs::path>();
    for (const auto &entry : fs::directory_iterator(outbox)) {
        enter_leaves.push_back(entry.path());
    }
    require(std::ranges::any_of(enter_leaves, [](const auto &leaf) {
            const auto body = read_file(leaf / "message.json");
            return body.find("\"to\":[\"telegram-bot\"]") != std::string::npos
                && body.find("Entered via the Return key.") != std::string::npos;
        }),
        "the Enter-queued leaf must address exactly the selected Agent and "
        "carry the typed text");

    // Selection change must not let a later click target the prior Agent.
    click_agent(shell, "issue-643");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("issue-643"),
        "the second Agent in the same project must be selectable");
    require(input->getLastText().isEmpty() && status->text().isEmpty(),
        "selecting a different Agent must reset the composer, not carry a draft");
    input->setText(QStringLiteral("A message for the other Agent."));
    const auto outbox_before_switch = outbox_entry_count();
    send_button->clicked(Qt::NoModifier, Qt::LeftButton);
    require(status->text().isEmpty(),
        "the send after switching Agents must still succeed without a Queued "
        "composer label");
    require(outbox_entry_count() == outbox_before_switch + 1,
        "the three sends must each allocate a fresh leaf");
    auto second_agent_bodies = std::vector<std::string>();
    for (const auto &entry : fs::directory_iterator(outbox)) {
        second_agent_bodies.push_back(read_file(entry.path() / "message.json"));
    }
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
    click_agent(shell, "telegram-bot");
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

    click_agent(shell, "agent-c");
    require(!button->isEnabled(),
        "a stale (non-alive) selection must leave Request sleep disabled");
    require(!fs::exists(project / ".lingtai/agent-c/.sleep"),
        "a stale ineligible selection must never gain a .sleep marker");

    click_agent(shell, "agent-a");
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

    click_agent(shell, "agent-b");
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

    click_agent(shell, "agent-a");
    require(button->isEnabled() && status->text().isEmpty(),
        "switching back to the still-eligible A must show a fresh, cleared "
        "status, never B's terminal result");

    click_agent(shell, "agent-b");
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

    click_agent(shell, "agent-live");
    require(!button->isVisible(),
        "a live selected Agent must show no Start action at all");

    click_agent(shell, "agent-success");
    require(!button->isVisible() && button->isEnabled()
            && status->text().isEmpty(),
        "Start Agent stays out of the header; a stale Agent still enables "
        "the hidden Start owner for /cpr");

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
    click_agent(shell, "agent-switch");
    require(!button->isVisible() && button->isEnabled(),
        "agent-switch must start fresh and eligible without a header Start action");
    button->click();
    QCoreApplication::processEvents();
    require(status->text() == QStringLiteral("Starting Agent..."),
        "agent-switch's own click must show its own pending wording");

    click_agent(shell, "agent-timeout");
    require(!button->isVisible() && button->isEnabled()
            && status->text().isEmpty(),
        "switching away from a pending launch must show a fresh, cleared "
        "status for the newly selected Agent, never the abandoned launch's "
        "wording");

    // Give agent-switch's detached fixture time to actually write its
    // heartbeat while this shell remains parked on a different selection.
    // Wait for a non-empty heartbeat body (not mere path existence): a plain
    // `>` truncate can briefly create an empty file that projects as
    // `invalid` before `date` finishes writing.
    const auto switch_heartbeat = switch_dir / ".agent.heartbeat";
    const auto isolation_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto heartbeat_ready = false;
    while (std::chrono::steady_clock::now() < isolation_deadline) {
        if (fs::exists(switch_heartbeat)
                && !read_file(switch_heartbeat).empty()) {
            heartbeat_ready = true;
            break;
        }
        QThread::msleep(50);
        QCoreApplication::processEvents();
    }
    require(heartbeat_ready,
        "agent-switch's background fixture must write its heartbeat while "
        "deselected");
    require(status->text().isEmpty(),
        "the parked selection must never show agent-switch's result while "
        "it is not the current selection");

    click_agent(shell, "agent-switch");
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
    click_agent(shell, "agent-timeout");
    require(!button->isVisible() && button->isEnabled()
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
    require(!button->isVisible() && button->isEnabled(),
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
    auto *catalog = required_child<QTreeWidget>(
        window, "lingtai_selected_agent_preset_summary");
    auto *state = required_child<QLabel>(
        window, "lingtai_selected_agent_preset_summary_state");

    require(catalog->editTriggers() == QAbstractItemView::NoEditTriggers,
        "the Presets catalog must be read-only");
    require(!catalog->accessibleName().isEmpty()
            && !heading->accessibleName().isEmpty()
            && !state->accessibleName().isEmpty(),
        "the Presets surface must be accessible");
    require(catalog->objectName() != required_child<QTextEdit>(
                window, "lingtai_selected_agent_conversation")->objectName(),
        "Presets must be a distinct surface from the conversation");

    const auto catalog_names = [&] {
        auto names = QStringList();
        for (auto index = 0; index != catalog->topLevelItemCount(); ++index) {
            auto *item = catalog->topLevelItem(index);
            if (item->data(0, Qt::UserRole).isValid()) {
                names << item->text(0);
            }
        }
        return names;
    };

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
    require(catalog_names().isEmpty(),
        "no Agent is selected yet, so no Presets rows may render");

    click_agent(shell, "telegram-bot");
    require(catalog_names() == QStringList({
            QStringLiteral("deepseek_flash"),
            QStringLiteral("codex"),
            QStringLiteral("zhipu-1")}),
        "selecting Agent A must list only its allowed presets in published "
        "order");
    require(state->text() == QStringLiteral("Resolved"),
        "a supported complete v1 artifact must show the Resolved state "
        "label");
    require(catalog->currentItem() != nullptr
            && catalog->currentItem()->text(0) == QStringLiteral("codex"),
        "the catalog must land on the Agent's active preset");

    // A changed allow-list must become visible through the real one-second
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
          "~/.lingtai-tui/presets/saved/codex.json",
          "~/.lingtai-tui/presets/saved/zhipu-1.json"
        ]
      }
    })JSON");
    write_file(artifact_a, resolved_v2);
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (catalog_names() != QStringList({
                    QStringLiteral("codex"), QStringLiteral("zhipu-1")})
                && std::chrono::steady_clock::now() < deadline) {
            QThread::msleep(50);
            QCoreApplication::processEvents();
        }
        require(catalog_names() == QStringList({
                    QStringLiteral("codex"), QStringLiteral("zhipu-1")}),
            "a changed allow-list must refresh through the real one-second "
            "timer");
    }

    // Selecting B, which has no published artifact, must never show A's
    // summary and must show the Not yet published state.
    click_agent(shell, "issue-643");
    require(catalog_names().isEmpty(),
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
    write_file(argv_record, std::string{});
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
    auto *open_button = required_child<QPushButton>(
        window, "lingtai_open_project_button");
    auto *choose_project = required_child<QPushButton>(
        window, "lingtai_startup_choose_project");
    require(window.findChild<QPushButton *>("lingtai_new_project_button")
            == nullptr,
        "no-project state must not expose a separate New Project control");
    auto *status = required_ui_child<Ui::FlatLabel>(
        window, "lingtai_bootstrap_status");
    auto *wizard = required_child<lingtai::desktop::ProjectSetupWizard>(
        window, "lingtai_project_setup_wizard");
    auto *destination_input = required_ui_child<Ui::InputField>(
        window, "lingtai_bootstrap_destination_input");
    auto *preset_chooser = required_child<QComboBox>(
        window, "lingtai_bootstrap_preset_chooser");
    auto *create_start = required_child<QPushButton>(
        window, "lingtai_bootstrap_create_start");
    auto *preset_continue = required_child<QPushButton>(
        window, "lingtai_setup_preset_continue");
    auto *save_preset = required_child<QPushButton>(
        window, "lingtai_setup_edit_preset_save");
    auto *agents_continue = required_child<QPushButton>(
        window, "lingtai_setup_agents_continue");
    auto *dialog_status = required_ui_child<Ui::FlatLabel>(
        window, "lingtai_bootstrap_dialog_status");
    required_child<QPushButton>(window, "lingtai_bootstrap_cancel");
    required_child<QPushButton>(window, "lingtai_bootstrap_destination_browse");
    require(create_start->accessibleName().isEmpty()
            || create_start->text() == QStringLiteral("Create orchestrator"),
        "the committing wizard action must remain Create orchestrator");

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
    shell.set_tui_executable(fs::absolute(success_tui));

    // Evidence 1: choosing an empty folder through the Open Project path runs
    // the exact separate argv `presets` and keeps the UI responsive/pending.
    auto open_requests = std::size_t{0};
    const auto start_bootstrap_at = [&](const fs::path &folder) {
        shell.set_open_project_request_handler([&] {
            ++open_requests;
            shell.request_new_project_at(folder);
        });
        choose_project->click();
    };
    start_bootstrap_at(destination);
    require(status->accessibilityName() == QStringLiteral("Discovering presets…"),
        "a pending discovery must show one truthful phase status");
    require(!open_button->isEnabled(),
        "Open Project must be disabled while bootstrap is pending");
    auto *open_new_window = required_child<QPushButton>(
        window, "lingtai_open_project_new_window_button");
    require(!open_new_window->isEnabled(),
        "Open Project in Another Window must be disabled while bootstrap "
        "is pending");
    QCoreApplication::processEvents();
    const auto presets_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (read_file(argv_record) != fixture_tui_argv({"presets"})
            && std::chrono::steady_clock::now() < presets_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    require(read_file(argv_record) == fixture_tui_argv({"presets"}),
        "choosing an empty folder must run the exact separate argv `presets`");
    choose_project->click();
    require(open_requests == 1,
        "Choose project must not fire again while bootstrap is pending");

    // Evidence 2: valid preset JSON populates the in-window setup route;
    // dismissing it via reject() (Escape / Cancel) must be the same no-spawn
    // cancellation and must re-enable actions.
    const auto dialog_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!wizard->isVisible()
            && std::chrono::steady_clock::now() < dialog_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    require(wizard->isVisible(),
        "valid preset discovery must show the New Project wizard");
    require(preset_chooser->count() == 2
            && preset_chooser->itemText(0) == "beta"
            && preset_chooser->itemText(1) == "alpha",
        "the preset chooser must list saved presets before templates");
    wizard->reject();
    QCoreApplication::processEvents();
    require(!wizard->isVisible() && open_button->isEnabled(),
        "dismissing the wizard via reject() must close it and re-enable Open "
        "Project");
    require(read_file(argv_record) == fixture_tui_argv({"presets"}),
        "dismissing the dialog must perform no spawn at all");

    // Evidence 3: a destination plus the selected non-first preset and
    // Create & Start produces the exact separate spawn argv, with Open Project
    // staying disabled while pending.
    start_bootstrap_at(destination);
    QCoreApplication::processEvents();
    while (!wizard->isVisible()
            && std::chrono::steady_clock::now() < dialog_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    require(wizard->isVisible(), "the second discovery must reopen the wizard");
    preset_chooser->setCurrentIndex(1);
    preset_continue->click();
    QCoreApplication::processEvents();
    save_preset->click();
    QCoreApplication::processEvents();
    destination_input->setText(QString());
    create_start->click();
    QCoreApplication::processEvents();
    require(dialog_status->accessibilityName()
                .contains(QStringLiteral("nonempty")),
        "Create project with no destination must refuse with a concise "
        "dialog status");
    destination_input->setText(path_text(destination));
    agents_continue->click();
    QCoreApplication::processEvents();
    create_start->click();
    require(status->accessibilityName() == QStringLiteral(
                "Creating project and starting Agent…"),
        "a pending spawn must show one truthful phase status");
    require(!open_button->isEnabled(),
        "Open Project must stay disabled while spawn is pending");
    QCoreApplication::processEvents();
    const auto spawn_argv = std::vector<std::string>{
        "spawn", path_text(destination).toStdString(),
        "--preset", "alpha"};
    const auto spawn_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (read_file(argv_record)
                != fixture_tui_argv({"presets", "presets"})
                    + fixture_tui_argv(spawn_argv)
            && std::chrono::steady_clock::now() < spawn_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
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
    require(!wizard->isVisible(),
        "a successful spawn must close the New Project wizard");

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
    shell.set_tui_executable(fs::absolute(fail_tui));
    const auto attached_root = fs::canonical(destination);
    start_bootstrap_at(sandbox / "partial-destination");
    QCoreApplication::processEvents();
    const auto fail_dialog_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!wizard->isVisible()
            && std::chrono::steady_clock::now() < fail_dialog_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
    }
    require(wizard->isVisible(),
        "the failing fixture's discovery must still show the wizard");
    preset_chooser->setCurrentIndex(0);
    preset_continue->click();
    QCoreApplication::processEvents();
    save_preset->click();
    QCoreApplication::processEvents();
    destination_input->setText(path_text(sandbox / "partial-destination"));
    agents_continue->click();
    QCoreApplication::processEvents();
    create_start->click();
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
    require(open_button->isEnabled(),
        "a spawn failure must re-enable Open Project");
    require(!wizard->isVisible(),
        "a failed spawn must close the New Project wizard");
    require(fs::exists(fail_argv_record)
            && read_file(fail_argv_record)
                == fixture_tui_argv({"presets"})
                    + fixture_tui_argv({"spawn",
                        path_text(sandbox / "partial-destination").toStdString(),
                        "--preset", "beta"}),
        "the failing spawn must still run the exact separate spawn argv");

    // Evidence 6: one malformed preset-list case fails closed before any
    // dialog or spawn.
    const auto malformed_argv_record = sandbox / "tui-malformed-argv.txt";
    const auto malformed_tui = sandbox / "tui-malformed";
    write_fixture_tui(malformed_tui, malformed_argv_record,
        R"(printf '%s' '{this is not json'
exit 0)",
        R"(exit 9)");
    shell.set_tui_executable(fs::absolute(malformed_tui));
    start_bootstrap_at(sandbox / "malformed-destination");
    QCoreApplication::processEvents();
    const auto malformed_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (read_file(malformed_argv_record) != fixture_tui_argv({"presets"})
            && std::chrono::steady_clock::now() < malformed_deadline) {
        QThread::msleep(20);
        QCoreApplication::processEvents();
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
    require(!wizard->isVisible() && open_button->isEnabled(),
        "a malformed preset list must never show the wizard and must "
        "re-enable Open Project");
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

    // Selecting a valid canvas row uses the same path as a real click: detail
    // replaces the roster, Back appears, and the composer is focused.
    click_agent(shell, "alpha");
    require(shell.selection_state().selected_agent_directory_key()
            == std::optional<fs::path>("alpha"),
        "selecting a valid Agent row must activate the detail surface");
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
    auto *resize_handle = required_child<QWidget>(
        window, "lingtai_roster_resize_handle");
    require(!separator_widget->isVisible() && resize_handle->isVisible(),
        "the roster divider must be a visible, grabbable handle in "
        "two-pane mode");
    // The user-resizable handle is a distinct semantic widget the accepted
    // fixed PlainShadow separator never provides; current production must
    // fail exactly this lookup.
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
    click_agent(shell, "alpha");
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
    require(window.findChild<QPushButton *>("lingtai_new_project_button")
            == nullptr,
        "compact project actions must not expose a separate New Project control");

    // Open Project remains reachable through the selector menu even before any
    // project opens.
    require(open_button != nullptr,
        "compact Open Project action must remain wired in the left column");

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
    QCoreApplication::processEvents();

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

    // Wide two-column mode uses the resize handle instead of the legacy
    // full-height separator edge.
    auto *separator_widget = required_child<Ui::RpWidget>(
        window, "lingtai_roster_separator");
    auto *separator = dynamic_cast<Ui::PlainShadow *>(separator_widget);
    require(separator != nullptr,
        "the left/right separator must be a Ui::PlainShadow");
    auto *resize_handle = required_child<QWidget>(
        window, "lingtai_roster_resize_handle");
    require(!separator->isVisible() && resize_handle->isVisible(),
        "wide two-column mode must hide the legacy separator and expose the "
        "resize handle between roster and detail");
    require(resize_handle->geometry().left() >= sidebar->geometry().right() - 1
            && content->geometry().left() >= resize_handle->geometry().right() - 1,
        "the resize handle must sit between the roster column and detail pane");

    // Rows: intrinsic height from the fixed 40px avatar plus two font lines
    // plus stable vertical padding on the virtual canvas.
    auto *canvas = roster_rows_canvas(window);
    const auto row_height = canvas_row_height(canvas);
    require(row_height >= 40 + 2 * 8,
        "the intrinsic canvas row height must accommodate the fixed 40px avatar "
        "disc plus the stable vertical framing");
    require(canvas->sizeHint().height() >= row_height,
        "the virtual roster canvas must size to at least one intrinsic row");

    // Agent selection still drives the same right detail.
    click_agent(shell, "alpha");
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("alpha")
            && is_agent_selected(shell, "alpha")
            && label_text(window, "lingtai_selected_agent_presentation_name")
                == QStringLiteral("alpha")
            && label_text(window, "lingtai_selected_agent_key")
                == QStringLiteral("Agent · Suspended"),
        "Agent selection must still drive the same right detail with a "
        "key-fallback header that never repeats the directory key");

    // An unchanged projection refresh must not rebuild the canvas model, so
    // the selected key and scroll position stay intact.
    auto *scroll = required_child<QScrollArea>(
        window, "lingtai_agent_roster_scroll");
    QCoreApplication::processEvents();
    auto *scroll_bar = scroll->verticalScrollBar();
    require(scroll_bar->maximum() > 0,
        "the tall roster must expose a nonzero scroll range");
    scroll_bar->setValue(scroll_bar->maximum());
    const auto scroll_before = scroll_bar->value();

    const auto refreshed = shell.open_project(project, std::nullopt);
    QCoreApplication::processEvents();
    require(refreshed.disposition == ProjectOpenDisposition::opened
            && is_agent_selected(shell, "alpha"),
        "an unchanged projection refresh must preserve the selected Agent key");
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
        "the friendly role · Status line must scale to the legible 12pt secondary rung");
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
            && (status->text().startsWith(QStringLiteral("Agent · "))
                || status->text().startsWith(QStringLiteral("Main agent · ")))
            && !status->text().contains(QStringLiteral("role:"))
            && !status->text().contains(QStringLiteral("presence:")),
        "the secondary line must read friendly role · Status, matching the "
        "Sidebar semantics without raw fact labels");

    // Header chrome no longer carries Start Agent or Request sleep; those
    // lifecycle actions remain available through `/cpr` and `/sleep`.
    auto *primary = required_child<QPushButton>(
        window, "lingtai_selected_agent_start_agent");
    require(header_child_of(primary),
        "the Start owner must still live inside the selected-Agent header");
    require(!primary->isVisible(),
        "Start Agent must not appear in the selected-Agent header");
    auto visible_caption_actions = 0;
    for (auto *button : top_bar->findChildren<QPushButton *>()) {
        if (button->isVisible() && !button->text().isEmpty()) {
            ++visible_caption_actions;
        }
    }
    require(visible_caption_actions == 0,
        "the compact header must show no caption-carrying action buttons");

    auto *sleep = required_child<QPushButton>(
        window, "lingtai_selected_agent_request_sleep");
    require(header_child_of(sleep),
        "the lifecycle secondary must live inside the selected-Agent header");
    require(!sleep->isVisible(),
        "Request sleep must not appear in the selected-Agent header");
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
    QTreeWidget *surface = nullptr;
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
    result.surface = required_child<QTreeWidget>(
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
    require(section->surface->editTriggers() == QAbstractItemView::NoEditTriggers,
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

    click_agent(shell, "agent-aa");
    QCoreApplication::processEvents();
    require(!start_button->isVisible(),
        "Start Agent must stay out of the selected-Agent header");
    const auto live_region_height = start_row->height();
    const auto live_anchor = manifest_identity->geometry().top();
    const auto live_sleep_top = sleep_row->geometry().top();

    click_agent(shell, "agent-bb");
    QCoreApplication::processEvents();
    require(!start_button->isVisible() && start_button->isEnabled(),
        "a start-eligible stale Agent still enables the hidden Start owner");
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

// The chat-first detail no longer shows a Presets tab. Conversation remains
// the default surface; `/presets` still reveals the read-only Presets page
// and the Conversation control as the way back. The remaining Conversation
// affordance is a plain text tab — never a filled rectangular slab.
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
    require(!presets_nav->isVisible(),
        "the Presets tab must not appear on the selected-Agent detail");
    require(!pages_nav->isVisible(),
        "the chat-first detail must not show a page-tab strip");

    presets_nav->click();
    QCoreApplication::processEvents();
    require(pages_nav->isVisible() && conversation_nav->isVisible()
            && !presets_nav->isVisible(),
        "opening Presets through the slash destination must keep Conversation "
        "as the visible way back, never a Presets tab");
    require(conversation_nav->text() == QStringLiteral("←  Conversation"),
        "/presets must use ← Conversation as the way back, matching /kanban");
    auto *top_bar = required_child<QWidget>(window, "lingtai_chat_top_bar");
    require(!top_bar->isVisible(),
        "/presets must hide the chat top bar so ← Conversation sits at the top");
    require(conversation_nav->font().pointSize() == 13
            && conversation_nav->height() == 28,
        "the Conversation back-tab must keep the 13pt / 28px navigation rung");

    const auto slab = st::windowBgOver->c;
    const auto image = pages_nav->grab().toImage();
    const auto dpr = image.devicePixelRatio();
    const auto at = [&](const QImage &target, const QPoint &logical) {
        return target.pixelColor(QPoint(qRound(logical.x() * dpr),
                                        qRound(logical.y() * dpr)));
    };
    auto slab_count = 0;
    for (auto y = 0; y < conversation_nav->height() - 2; ++y) {
        for (auto x = 0; x < conversation_nav->width(); ++x) {
            if (at(image, conversation_nav->mapTo(pages_nav, QPoint(x, y)))
                    == slab) {
                ++slab_count;
            }
        }
    }
    require(slab_count * 10
            < conversation_nav->width() * (conversation_nav->height() - 2),
        "the Conversation back-tab must never be a filled rectangular slab");

    conversation_nav->click();
    QCoreApplication::processEvents();
    require(!pages_nav->isVisible(),
        "returning to Conversation must hide the page-tab strip again");
    require(top_bar->isVisible(),
        "returning to Conversation must restore the chat top bar");

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
    auto *style_hints = QGuiApplication::styleHints();
    style_hints->setColorScheme(Qt::ColorScheme::Dark);
    QApplication::processEvents();
    style_hints->setColorScheme(Qt::ColorScheme::Light);
    QApplication::processEvents();

    auto &window = shell.window();
    const auto color_close = [](const QColor &a, const QColor &b) {
        return qAbs(a.red() - b.red()) <= 12
            && qAbs(a.green() - b.green()) <= 12
            && qAbs(a.blue() - b.blue()) <= 12;
    };
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
    require(st::windowBg->c == QColor("#ffffff"),
        "theme reset must evaluate against Telegram's canonical light palette");
    require(color_close(sidebar->grab().toImage().pixelColor(2, 2),
            st::windowBg->c),
        "the dialog list surface must be palette-owned (windowBg), not "
        "a raw white Qt surface");
    click_agent(shell, "alpha");
    require(is_agent_selected(shell, "alpha"),
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
    require(!pages_nav->isVisible() && !presets_nav->isVisible(),
        "the chat-first detail must not show page tabs; Presets is slash-only");
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
    conversation->viewport()->repaint();
    QCoreApplication::processEvents();

    // 4. Bubble/composer ownership, black-box: only the rendered viewport
    // pixels and public Qt behavior are consulted -- never document or block
    // internals. Incoming Agent prose stays on the shared windowBg canvas with
    // a left avatar lane; outgoing Human rows get the accepted pale bubble
    // tint (#EEF7F3 in light mode). The composer must remain visible directly
    // below the conversation in one common ancestor coordinate system.
    const auto human_bubble_fill = st::windowBg->c.lightness() >= 128
        ? QColor(QStringLiteral("#EEF7F3"))
        : st::msgOutBg->c;
    const auto surface_image = conversation->viewport()->grab().toImage();
    // The surface owns a transparent Base, so no widget palette role is a
    // meaningful backdrop: sample the painted backdrop directly from the grab
    // and compare it to the light lib_ui palette token the surface paints
    // (`windowBg`, the shared single light-canvas base used across the shell).
    const auto sampled_backdrop = surface_image.pixelColor(
        surface_image.width() / 2, surface_image.height() - 6);
    require(color_close(sampled_backdrop, st::windowBg->c),
        "the sampled chat backdrop pixel must match the lib_ui palette token "
        "the surface paints");
    require(!color_close(sampled_backdrop, human_bubble_fill),
        "the sampled chat backdrop must be distinct from the Human bubble tint");

    struct BubbleTrace {
        int min_x = -1, max_x = -1, min_y = -1, max_y = -1;
        int widest_run = 0;
    };
    const auto bounds_for_color = [&](const QColor &color,
            int x_begin,
            int x_end) {
        auto bounds = QRect();
        const auto pixel = color.rgba();
        if (x_end < 0) x_end = surface_image.width();
        for (auto y = 0; y != surface_image.height(); ++y) {
            for (auto x = x_begin; x != x_end; ++x) {
                if (surface_image.pixel(x, y) == pixel) {
                    bounds = bounds.isNull()
                        ? QRect(x, y, 1, 1)
                        : bounds.united(QRect(x, y, 1, 1));
                }
            }
        }
        return bounds;
    };
    const auto trace_from_bounds = [&](const QRect &bounds) {
        auto trace = BubbleTrace();
        if (bounds.isNull()) {
            return trace;
        }
        trace.min_x = bounds.left();
        trace.max_x = bounds.right();
        trace.min_y = bounds.top();
        trace.max_y = bounds.bottom();
        trace.widest_run = bounds.width();
        return trace;
    };
    const auto outgoing = trace_from_bounds(bounds_for_color(
        human_bubble_fill,
        surface_image.width() / 2,
        surface_image.width()));
    const auto incoming_avatar = trace_from_bounds(bounds_for_color(
        st::dialogsNameFg->c,
        0,
        surface_image.width() / 2));
    require(outgoing.min_y >= 0,
        "the chat surface must render a real outgoing Human bubble component");
    require(incoming_avatar.min_y >= 0,
        "the chat surface must render the incoming Agent avatar lane");

    const auto viewport_text_bounds_containing = [&](const QString &needle) -> QRectF {
        auto result = QRectF();
        for (auto block = conversation->document()->begin(); block.isValid();
             block = block.next()) {
            if (!block.text().contains(needle)) continue;
            QTextCursor cursor(block);
            const auto start = conversation->cursorRect(cursor);
            cursor.movePosition(QTextCursor::EndOfBlock);
            const auto end = conversation->cursorRect(cursor);
            result = result.isNull()
                ? QRectF(start.united(end))
                : result.united(QRectF(start.united(end)));
        }
        return result;
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
    const auto viewport_text_bounds_prefix = [&](const QString &prefix) -> QRectF {
        for (auto block = conversation->document()->begin(); block.isValid();
             block = block.next()) {
            if (!block.text().startsWith(prefix)) continue;
            QTextCursor cursor(block);
            const auto start = conversation->cursorRect(cursor);
            cursor.movePosition(QTextCursor::EndOfBlock);
            const auto end = conversation->cursorRect(cursor);
            return QRectF(start.united(end));
        }
        return QRectF();
    };
    const auto incoming_sender = viewport_text_bounds_prefix(
        QStringLiteral("alpha ·"));
    const auto incoming_body = viewport_text_bounds_containing(
        QStringLiteral("PR published, not merged."));
    const auto outgoing_text = viewport_text_bounds_containing(
        QStringLiteral("Thanks, reviewing tomorrow."));
    require(!incoming_sender.isNull() && !incoming_body.isNull(),
        "incoming Agent metadata and body must be visible on the canvas");
    require(!outgoing_text.isNull(),
        "the outgoing Human message body must be visible");
    require_contains_text(outgoing, outgoing_text, "outgoing");
    const auto incoming_lane = incoming_body.united(incoming_sender);
    require((incoming_lane.left() + incoming_lane.right()) / 2.0
                < surface_image.width() / (2.0 * image_scale)
            && incoming_avatar.max_x < surface_image.width() / 2,
        "incoming Agent content must stay left-anchored with its avatar lane");
    require((outgoing.min_x + outgoing.max_x) / 2
                > surface_image.width() / 2,
        "the outgoing Human bubble must sit on the right");
    require(outgoing.widest_run <= surface_image.width() * 3 / 4,
        "the largest Human bubble span must stay at most 75% of the chat "
        "viewport");
    require(outgoing.min_y / image_scale - qCeil(incoming_lane.bottom()) >= 2,
        std::string("the incoming Agent row and outgoing Human bubble must be "
            "separated by real vertical spacing, not touching (incoming_max_y=")
            + std::to_string(qCeil(incoming_lane.bottom()))
            + " outgoing_min_y=" + std::to_string(outgoing.min_y / image_scale)
            + " gap="
            + std::to_string(outgoing.min_y / image_scale
                - qCeil(incoming_lane.bottom()))
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
    auto *startup_route = required_child<QWidget>(
        window, "lingtai_startup_route");
    auto *startup_illustration = required_child<QWidget>(
        window, "lingtai_startup_illustration");
    auto *startup_heading = required_child<QLabel>(
        window, "lingtai_startup_heading");
    auto *startup_tagline = required_child<QLabel>(
        window, "lingtai_startup_tagline");
    auto *choose_project = required_child<QPushButton>(
        window, "lingtai_startup_choose_project");
    auto *agent_roster = required_child<QWidget>(
        window, "lingtai_agent_roster");
    auto *workspace_content = required_child<QWidget>(
        window, "lingtai_desktop_content");

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
    require(window.property("lingtai_native_titlebar_full_size").toBool(),
        "the post-show NSWindow must keep FullSizeContentView so TitleWidget y=0 shares the traffic-light row");
    require(titlebar_brand->parent() == titlebar,
        "the pure LingTai brand must be painted inside Telegram's TitleWidget owner");
    require(window.body()->geometry().top() == titlebar->height(),
        "the content body must begin below the restored unified title row");
    require(startup_illustration->property("lingtai_light_logo_resource").toString()
                == QStringLiteral(":/lingtai/startup/lingtai-logo-light-4096.png")
            && startup_illustration->property("lingtai_dark_logo_resource").toString()
                == QStringLiteral(":/lingtai/startup/lingtai-logo-dark-4096.png"),
        "startup must bind Ted's exact light and dark logo assets");
    require(startup_heading->text() == QStringLiteral("LingTai Orchestration")
            && startup_tagline->text()
                == QStringLiteral("Awaken under Bodhi\nOne soul, thousand avatars")
            && choose_project->text() == QStringLiteral("Choose project"),
        "startup copy and CTA labels must stay wired");
    const bool on_startup_canvas = startup_route->isVisible()
        && !shell.selection_state().active_project().has_value();
    if (on_startup_canvas) {
        require(startup_illustration->isVisible(),
            "startup must show the reference illustration");
        require(!agent_roster->isVisible() && !workspace_content->isVisible(),
            "startup must be one uninterrupted canvas without the project workspace");
        require(qAbs(titlebar_brand->geometry().center().x()
                    - titlebar->rect().center().x()) <= 1,
            "startup LingTai title must be centered in the native title row");
    }
    require(titlebar_brand->palette().color(QPalette::WindowText)
                == st::dialogsNameFg->c
            && titlebar_brand->palette().color(QPalette::Text)
                == st::dialogsNameFg->c,
        "the title-row brand must own theme foreground ink instead of inheriting platform chrome");
    const auto titlebar_brand_image = titlebar->grab(
        titlebar_brand->geometry()).toImage();
    auto titlebar_brand_has_visible_ink = false;
    for (auto y = 0; y != titlebar_brand_image.height(); ++y) {
        for (auto x = 0; x != titlebar_brand_image.width(); ++x) {
            if (titlebar_brand_image.pixelColor(x, y) != st::windowBg->c) {
                titlebar_brand_has_visible_ink = true;
                break;
            }
        }
        if (titlebar_brand_has_visible_ink) break;
    }
    require(titlebar_brand_has_visible_ink,
        "the title-row brand must render visible ink against the unified title background");
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
    const auto message_center_tolerance = qMax(
        4,
        qCeil(QFontMetrics(composer_input->font()).height()
            * controls_scale / 2.0));
    require(!message_bounds.isNull()
            && qAbs(message_y_delta) <= message_center_tolerance,
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
    QCoreApplication::processEvents();
    require(!startup_route->isVisible()
            && agent_roster->isVisible() && workspace_content->isVisible(),
        "choosing a project must replace startup with the existing workspace");
    const auto native_anchor = titlebar_brand->property(
        "lingtai_native_traffic_light_anchor").toPoint();
    require(native_anchor.x() > 0
            && qAbs(titlebar_brand->geometry().left() - native_anchor.x()) <= 1
            && qAbs(titlebar_brand->geometry().center().y() - native_anchor.y()) <= 1,
        "opened-project LingTai brand must return beside the traffic lights");
    require(tree_snapshot(project) == fixture_before,
        "opening the modern-composer fixture must remain read-only");
    require(project_selector->text() == QStringLiteral("project"),
        "the Sidebar project selector must show the active folder basename");
    click_first_agent_canvas_row(window);
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("alpha"),
        "the modern-composer fixture Agent must be selectable");

    composer_input->setEnabled(true);
    composer_input->setText(QStringLiteral("/"));
    QCoreApplication::processEvents();
    auto *slash_popup = required_child<QListWidget>(
        window, "lingtai_slash_command_popup");
    require(slash_popup->isVisible() && slash_popup->count() >= 8,
        "typing a leading slash must open the slash-command popup");
    composer_input->setText(QStringLiteral("hello"));
    QCoreApplication::processEvents();
    require(!slash_popup->isVisible(),
        "ordinary composer text must dismiss the slash-command popup");
    require(composer_input->accessibilityName().isEmpty(),
        "typed composer text must hide the Message… placeholder");
    composer_input->setFocus();
    QCoreApplication::processEvents();
    composer_input->clear();
    QCoreApplication::processEvents();
    // Focused empty field keeps the Message… string for accessibility but
    // hides it visually; clearing focus restores the visible placeholder.
    require(composer_input->accessibilityName() == QStringLiteral("Message…"),
        "clearing the composer must restore the Message… placeholder name");
    composer_input->clearFocus();
    QCoreApplication::processEvents();
    require(composer_input->accessibilityName() == QStringLiteral("Message…"),
        "an unfocused empty composer must keep the Message… placeholder");

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
    click_agent(shell, "alpha");
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
    click_agent(shell, stale_key);
    require(shell.selection_state().selected_agent_directory_key()
            == std::optional<fs::path>(stale_key),
        "the stale target must be selectable");
    QCoreApplication::processEvents();
    require(presentation_name->isVisible() && detail_key->isVisible(),
        "a wide actual header must show the presentation name and the "
        "secondary key together");
    require(!start_button->isVisible() && !sleep_button->isVisible(),
        "Start Agent and Request sleep must stay out of the selected-Agent header");
    require(!back_button->isVisible(),
        "Back must stay hidden in wide two-column mode");
    require_disjoint(
        {presentation_name, detail_key});

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
    require(!start_button->isVisible() && !sleep_button->isVisible(),
        "a constrained actual header must keep Start Agent and Request sleep hidden");
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
    require(!start_button->isVisible() && !sleep_button->isVisible()
            && !back_button->isVisible(),
        "returning to wide must restore the same wide control set");
    require_disjoint(
        {presentation_name, detail_key});

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
    require(!agent_pages_nav->isVisible() && !presets_nav->isVisible(),
        "the chat-first detail must not show page tabs");
    require(project_route->geometry().top() == 0,
        "the active project route must begin at the content origin so "
        "hidden empty-route branding cannot leave a shared spacer above "
        "the workspace");

    auto *pages_nav_layout = qobject_cast<QHBoxLayout *>(
        agent_pages_nav->layout());
    require(pages_nav_layout != nullptr,
        "the Agent pages nav must own a horizontal layout");
    require(pages_nav_layout->count() >= 2,
        "the Agent pages nav must hold the Conversation control and trailing room");
    require(pages_nav_layout->itemAt(0)->widget() == conversation_nav,
        "the Conversation control must lead the Agent pages nav");
    require(pages_nav_layout->stretch(0) == 0,
        "the Conversation control must not stretch");
    auto *trailing_room = pages_nav_layout->itemAt(1)->spacerItem();
    require(trailing_room != nullptr && pages_nav_layout->stretch(1) > 0,
        "the Agent pages nav must trail a positive-stretch spacer");

    // (A) Conversation is the default surface, so the page-tab strip stays
    // hidden on the chat. Clicking the retained Conversation control keeps
    // that contract.
    required_child<QPushButton>(window, "lingtai_agent_page_nav_conversation")
        ->click();
    QCoreApplication::processEvents();
    require(!agent_pages_nav->isVisible() && !presets_nav->isVisible(),
        "Conversation is the default surface, so the page-tab strip stays hidden");

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

void verify_kanban_page(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *input = required_ui_child<Ui::InputField>(
        window, "lingtai_composer_input");
    auto *conversation = required_child<QTextEdit>(
        window, "lingtai_selected_agent_conversation");
    auto *composer = required_child<Ui::RpWidget>(window, "lingtai_composer");
    auto *conversation_nav = required_child<QPushButton>(
        window, "lingtai_agent_page_nav_conversation");
    auto *presets_nav = required_child<QPushButton>(
        window, "lingtai_agent_page_nav_presets");
    auto *kanban_page = required_child<QWidget>(window, "lingtai_kanban_page");

    const auto project = sandbox / "project";
    const auto alpha = project / ".lingtai/alpha";
    write_file(project / ".lingtai/human/.agent.json",
        R"({"agent_id":"20260101-000000-h001","agent_name":"Ted",)"
        R"("address":"human","state":"active"})");
    write_file(alpha / ".agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-a001",)"
        R"("agent_name":"alpha","nickname":"Alpha",)"
        R"("address":"alpha","state":"active","language":"en"})");
    write_file(alpha / "init.json",
        R"({"model":"claude-opus","provider":"anthropic","mcp":{"fs":{}}})");
    write_file(alpha / ".status.json",
        R"({"tokens":{"context":{"window_size":200000,"system_tokens":1000,)"
        R"("tools_tokens":500,"history_tokens":2500,"total_tokens":4000,)"
        R"("usage_pct":2.0}}})");
    write_file(alpha / "logs/token_ledger.jsonl",
        "{\"ts\":\"2026-08-18T12:00:00Z\",\"input\":100,\"output\":40,"
        "\"thinking\":10,\"cached\":20,\"model\":\"claude-opus\","
        "\"endpoint\":\"https://api.anthropic.com/v1\"}\n");
    write_file(alpha / "daemons/run-1/daemon.json",
        R"({"state":"running","backend":"claude-p",)"
        R"("cli_tokens":{"input":10,"output":5,"thinking":1,"cached":2,"calls":1}})");
    write_file(project / ".lingtai/beta/.agent.json",
        R"({"admin":{},"agent_id":"20260712-191609-b001",)"
        R"("agent_name":"beta","address":"beta","state":"idle"})");
    write_file(project / ".lingtai/human/mailbox/sent/20260818T120000-aa01/message.json",
        conversation_envelope("human", "alpha", "",
            "A long human note that must stay inside the chat after a kanban resize.",
            "sent_at", "2026-08-18T12:00:00Z"));

    window.resize(1200, 800);
    QCoreApplication::processEvents();
    const auto outcome = shell.open_project(project, std::nullopt);
    require(outcome.disposition == ProjectOpenDisposition::opened,
        "the kanban fixture project must open");
    click_first_agent_canvas_row(window);
    require(shell.selection_state().selected_agent_directory_key()
            == std::optional<fs::path>("alpha"),
        "the kanban fixture must select the first Agent");
    require(input->isEnabled(),
        "a selected Agent must enable the composer before /kanban");

    input->setText(QStringLiteral("/kanban"));
    input->setFocus();
    QCoreApplication::processEvents();
    auto enter = QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(input->rawTextEdit(), &enter);
    QCoreApplication::processEvents();

    require(input->getLastText().isEmpty(),
        "/kanban must clear the composer after local submission");
    require(kanban_page->isVisible(),
        "/kanban must reveal the agent properties page");
    auto *kanban_name = required_child<QLabel>(window, "lingtai_kanban_name");
    const auto *kanban_name_before_tick = kanban_name;
    QElapsedTimer idle;
    idle.start();
    while (idle.elapsed() < 1200) {
        QCoreApplication::processEvents();
        QThread::msleep(50);
    }
    require(required_child<QLabel>(window, "lingtai_kanban_name")
                == kanban_name_before_tick,
        "the 1s snapshot timer must not rebuild the kanban widget tree, "
        "because that stalls scrolling on large token ledgers");
    require(!conversation->isVisible() && !composer->isVisible(),
        "/kanban must hide the conversation and composer");
    auto *back = required_child<QPushButton>(window, "lingtai_kanban_back");
    require(back->isVisible() && !conversation_nav->isVisible()
            && !presets_nav->isVisible(),
        "/kanban must use ← Agents as the way back, never a Kanban tab");

    require(required_child<QLabel>(window, "lingtai_kanban_name")
                ->text() == QStringLiteral("Alpha"),
        "the properties header must title the selected agent");
    auto *identity = required_child<QWidget>(
        window, "lingtai_kanban_section_identity");
    require(required_child<QLabel>(*identity, "lingtai_kanban_meta")
                ->text().contains(QStringLiteral("alpha")),
        "the header must fill identity address");
    auto *tokens_stat = required_child<QWidget>(
        window, "lingtai_kanban_stat_tokens");
    require(tokens_stat->findChild<QLabel *>("lingtai_kanban_stat_value")
                ->text() == QStringLiteral("150"),
        "the metric strip must show selected-agent token spend");
    auto *network_stat = required_child<QWidget>(
        window, "lingtai_kanban_metric_network");
    require(network_stat->findChild<QLabel *>("lingtai_kanban_stat_value")
                ->text().contains(QStringLiteral("3")),
        "the metric strip must count network members");
    auto *token_block = required_child<QWidget>(
        window, "lingtai_kanban_section_tokens");
    auto saw_input = false;
    for (auto *label : token_block->findChildren<QLabel *>()) {
        if (label->text() == QStringLiteral("100")) saw_input = true;
    }
    require(saw_input, "token usage must fill selected-agent input totals");
    require(required_child<QWidget>(window, "lingtai_kanban_section_daemons")
                ->findChild<QLabel *>("lingtai_kanban_daemons_value")
                ->text().contains(QStringLiteral("running: 1")),
        "the network sidebar must list daemon counts");

    auto *left_column = required_child<QWidget>(window, "lingtai_kanban_left");
    auto *right_column = required_child<QWidget>(window, "lingtai_kanban_right");
    auto *summary_scroll = required_child<QScrollArea>(
        window, "lingtai_kanban_summary_scroll");
    const auto viewport_right = summary_scroll->viewport()->mapTo(
        &window, QPoint(summary_scroll->viewport()->width(), 0)).x();
    const auto column_right = right_column->mapTo(
        &window, QPoint(right_column->width(), 0)).x();
    require(viewport_right - column_right >= 16,
        "network facts must keep a gutter from the overlay scrollbar");
    window.resize(520, 800);
    QCoreApplication::processEvents();
    require(right_column->isVisible() && left_column->isVisible()
            && right_column->y() > left_column->y(),
        "a narrow window must stack network under the agent facts, never clip them");
    window.resize(1200, 800);
    QCoreApplication::processEvents();

    required_child<QPushButton>(window, "lingtai_kanban_open_detail")->click();
    QCoreApplication::processEvents();
    auto *hero = required_child<QWidget>(window, "lingtai_kanban_hero");
    require(!hero->isVisible(),
        "context detail must hide the summary hero, matching TUI Ctrl+D");
    require(required_child<QLabel>(window, "lingtai_kanban_inspector_title")
                ->text() == QStringLiteral("Agent Detail"),
        "context detail must title the page Agent Detail");
    require(required_child<QPushButton>(window, "lingtai_kanban_detail_back")
                ->isVisible(),
        "context detail must offer ← Back to summary");
    const auto section_mentions = [](QWidget &root, const QString &needle) {
        for (auto *label : root.findChildren<QLabel *>()) {
            if (label->text().contains(needle)) return true;
        }
        return false;
    };
    auto *info = required_child<QWidget>(window, "lingtai_kanban_section_info");
    require(section_mentions(*info, QStringLiteral("Alpha"))
            && section_mentions(*info, QStringLiteral("alpha")),
        "Agent Detail must list identity name and address");
    auto *providers = required_child<QWidget>(
        window, "lingtai_kanban_section_providers");
    require(section_mentions(*providers, QStringLiteral("anthropic")),
        "Open context detail must fill the main-agent provider breakdown");
    require(section_mentions(*providers, QStringLiteral("100.0%")),
        "provider rows must show a spend share, matching TUI");
    require(section_mentions(*providers, QStringLiteral("miss")),
        "main-agent provider rows must include cache miss");
    auto *detail_scroll = required_child<QScrollArea>(
        window, "lingtai_kanban_detail_scroll");
    window.resize(900, 800);
    QCoreApplication::processEvents();
    const auto detail_viewport_right = detail_scroll->viewport()->mapTo(
        &window, QPoint(detail_scroll->viewport()->width(), 0)).x();
    for (auto *label : providers->findChildren<QLabel *>()) {
        if (!label->isVisible() || label->width() <= 0) continue;
        const auto right = label->mapTo(
            &window, QPoint(label->width(), 0)).x();
        require(detail_viewport_right - right >= 8,
            "Agent Detail provider facts must stay inside the viewport, "
            "clear of the overlay scrollbar");
    }
    for (auto *bar : providers->findChildren<QWidget *>(
            "lingtai_kanban_provider_share")) {
        const auto right = bar->mapTo(&window, QPoint(bar->width(), 0)).x();
        require(detail_viewport_right - right >= 8,
            "provider share bars must not run under the overlay scrollbar");
    }
    auto *daemon_providers = required_child<QWidget>(
        window, "lingtai_kanban_section_daemon_providers");
    require(section_mentions(*daemon_providers, QStringLiteral("claude-p")),
        "Agent Detail must break daemon token usage out by backend");
    auto *totals = required_child<QWidget>(window, "lingtai_kanban_section_totals");
    require(section_mentions(*totals, QStringLiteral("168")),
        "combined totals must add main-agent spend to daemon spend");
    auto *session = required_child<QWidget>(
        window, "lingtai_kanban_section_session_current");
    require(section_mentions(*session, QStringLiteral("api_calls")),
        "Agent Detail must show current session API stats");
    auto *recent = required_child<QWidget>(window, "lingtai_kanban_section_recent");
    require(section_mentions(*recent, QStringLiteral("claude-opus")),
        "context detail must list recent main-agent activity");
    auto *mcp = required_child<QWidget>(window, "lingtai_kanban_section_mcp");
    require(section_mentions(*mcp, QStringLiteral("fs")),
        "Open context detail must list MCP servers");
    auto *daemons = required_child<QWidget>(
        window, "lingtai_kanban_detail_daemons");
    require(section_mentions(*daemons, QStringLiteral("1")),
        "context detail must always list daemon running/total counts");

    required_child<QPushButton>(window, "lingtai_kanban_detail_back")->click();
    QCoreApplication::processEvents();
    require(hero->isVisible(),
        "← Back to summary must restore the properties hero");

    auto ctrl_d = QKeyEvent(QEvent::KeyPress, Qt::Key_D, Qt::ControlModifier);
    QApplication::sendEvent(kanban_page, &ctrl_d);
    QCoreApplication::processEvents();
    require(!hero->isVisible()
            && required_child<QLabel>(window, "lingtai_kanban_inspector_title")
                   ->isVisible(),
        "Ctrl+D must open context detail from the properties summary");
    auto escape = QKeyEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(kanban_page, &escape);
    QCoreApplication::processEvents();
    require(hero->isVisible(),
        "Esc on context detail must return to the properties summary");

    window.resize(800, 800);
    QCoreApplication::processEvents();
    back->click();
    QCoreApplication::processEvents();
    require(conversation->isVisible() && composer->isVisible()
            && !kanban_page->isVisible(),
        "← Agents must restore the chat from the properties page");
    auto *chat_scroll = required_child<QScrollArea>(
        window, "lingtai_agent_detail_scroll");
    const auto chat_viewport_right = chat_scroll->viewport()->mapTo(
        &window, QPoint(chat_scroll->viewport()->width(), 0)).x();
    const auto conversation_right = conversation->mapTo(
        &window, QPoint(conversation->width(), 0)).x();
    require(chat_viewport_right - conversation_right >= 0,
        "the conversation surface must not overflow the detail viewport "
        "after resizing on /kanban and going back");
    require(conversation->width() <= chat_scroll->viewport()->width() + 1,
        (QStringLiteral("the restored chat must not be wider than the detail "
            "viewport after resizing on /kanban (chat=%1 viewport=%2)")
            .arg(conversation->width())
            .arg(chat_scroll->viewport()->width())
            .toStdString()).c_str());
    require(std::abs(conversation->width()
            - conversation->parentWidget()->width()) <= 2,
        "the restored chat must span the detail column, not keep a stale "
        "kanban width");
    require(conversation->toPlainText().contains(
            QStringLiteral("A long human note that must stay inside the chat")),
        "the restored chat must still show the selected-Agent conversation");
    auto outgoing_left = 0.0;
    auto outgoing_right = 0.0;
    auto saw_outgoing = false;
    for (auto *frame : conversation->document()->rootFrame()->childFrames()) {
        if (!frame->frameFormat()
                .property(QTextFormat::UserProperty + 5).toBool()) {
            continue;
        }
        auto block = frame->begin().currentBlock();
        if (!block.isValid()) continue;
        outgoing_left = block.blockFormat().leftMargin();
        outgoing_right = block.blockFormat().rightMargin();
        saw_outgoing = true;
        break;
    }
    require(saw_outgoing,
        "the restored chat must keep the Human bubble after /kanban");
    require(outgoing_left + outgoing_right
            <= double(conversation->viewport()->width()) + 1.0,
        "Human bubble margins must fit the current viewport after a kanban "
        "resize, never the previous wider pane");

    auto *top_bar = required_child<QWidget>(window, "lingtai_chat_top_bar");
    require_within_detail_viewport(window, *top_bar,
        "the chat top bar after resizing on /kanban and going back");
    require_within_detail_viewport(window, *composer,
        "the composer after resizing on /kanban and going back");

    const auto submit_slash = [&](const QString &command) {
        input->setText(command);
        input->setFocus();
        QCoreApplication::processEvents();
        auto enter = QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(input->rawTextEdit(), &enter);
        QCoreApplication::processEvents();
    };

    window.resize(1200, 800);
    QCoreApplication::processEvents();
    window.resize(800, 800);
    QCoreApplication::processEvents();
    submit_slash(QStringLiteral("/kanban"));
    require(kanban_page->isVisible() && !conversation->isVisible(),
        "resizing the chat then /kanban must show Agent properties");
    require_within_detail_viewport(window, *kanban_page,
        "kanban after a conversation resize");

    back->click();
    QCoreApplication::processEvents();
    require(conversation->isVisible() && composer->isVisible(),
        "← Agents must restore the composer before /presets");
    submit_slash(QStringLiteral("/presets"));
    auto *presets = required_child<QWidget>(
        window, "lingtai_selected_agent_preset_summary_section");
    require(presets->isVisible() && !conversation->isVisible(),
        "/presets must hide the conversation like /kanban does");
    require_within_detail_viewport(window, *presets,
        "presets after opening from a resized chat");
    window.resize(1200, 800);
    QCoreApplication::processEvents();
    window.resize(800, 800);
    QCoreApplication::processEvents();
    require_within_detail_viewport(window, *presets,
        "presets after a resize while that page is showing");
    conversation_nav->click();
    QCoreApplication::processEvents();
    require(conversation->isVisible(),
        "Conversation must restore the chat from /presets");
    require_within_detail_viewport(window, *conversation,
        "conversation after resizing on /presets and going back");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "kanban fixtures must be removed");
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
    click_agent_canvas_row(window, 0);
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("alpha")
            && input->isEnabled() && send_button->isEnabled(),
        "a selected valid Agent must enable the composer before slash input");

    input->setText(QStringLiteral("/"));
    QCoreApplication::processEvents();
    auto *slash_popup = required_child<QListWidget>(
        window, "lingtai_slash_command_popup");
    require(slash_popup->isVisible() && slash_popup->count() >= 8,
        "typing a leading slash must open the slash-command popup");
    input->setText(QStringLiteral("hello"));
    QCoreApplication::processEvents();
    require(!slash_popup->isVisible(),
        "ordinary composer text must dismiss the slash-command popup");
    require(input->accessibilityName().isEmpty(),
        "typed composer text must hide the Message… placeholder");
    input->clear();
    QCoreApplication::processEvents();

    submit_command(QStringLiteral("/presets"));
    require(input->getLastText().isEmpty(),
        "raw /presets must clear the composer after local submission");
    require(outbox_leaf_count() == 0,
        "raw /presets must stay local and create no human outbox leaf");
    require(presets_nav->isChecked() && !conversation_nav->isChecked(),
        "raw /presets must check the existing Presets page");
    require(conversation_nav->isVisible() && !presets_nav->isVisible(),
        "raw /presets must reveal Conversation as the way back, never a Presets tab");

    // `/setup` is the new-workspace wizard (preset catalog, editor, agent
    // review), not the `/presets` allow-list catalog.
    conversation_nav->click();
    QCoreApplication::processEvents();
    require(conversation_nav->isChecked() && input->isVisible(),
        "Conversation must restore the composer before /setup");
    submit_command(QStringLiteral("/setup"));
    require(input->getLastText().isEmpty(),
        "raw /setup must clear the composer after local submission");
    require(outbox_leaf_count() == 0,
        "raw /setup must stay local and create no human outbox leaf");
    require(!presets_nav->isChecked(),
        "raw /setup must not open the allowed-preset catalog");
    auto *wizard = required_child<lingtai::desktop::ProjectSetupWizard>(
        window, "lingtai_project_setup_wizard");
    auto *bootstrap_status = required_ui_child<Ui::FlatLabel>(
        window, "lingtai_bootstrap_status");
    require(wizard->isVisible()
            || !bootstrap_status->accessibilityName().isEmpty(),
        "raw /setup must start the new-workspace setup wizard, or report "
        "why that wizard cannot run");
    if (wizard->isVisible()) {
        wizard->reject();
        QCoreApplication::processEvents();
    }

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
    require(required_child<QWidget>(window, "lingtai_agent_roster_rows")
                ->isVisible(),
        "raw /agents must return focus to a usable existing Agent roster");

    // Re-enter the same valid route for the command-status checks.
    click_agent_canvas_row(window, 0);
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

    submit_command(QStringLiteral("/btw"));
    require(status->text() == QStringLiteral("Usage: /btw <question>"),
        "raw /btw without a question must show the TUI usage line");
    require(!fs::exists(target / ".inquiry"),
        "raw /btw without a question must write no inquiry");
    submit_command(QStringLiteral("/btw side question"));
    require(read_file(target / ".inquiry") == "human\nside question",
        "raw /btw must write the TUI .inquiry human source and question");
    require(status->text() == QStringLiteral("Inquiry sent: side question"),
        "raw /btw must report the TUI inquiry-sent status");
    submit_command(QStringLiteral("/insights"));
    require(read_file(target / ".inquiry") == "human\nside question",
        "a pending inquiry must remain a one-at-a-time no-op");
    require(status->text() == QStringLiteral("Requesting insight..."),
        "raw /insights must still report the TUI insight-sent status");
    std::error_code inquiry_error;
    fs::remove(target / ".inquiry", inquiry_error);
    submit_command(QStringLiteral("/insights"));
    require(read_file(target / ".inquiry").rfind("insight\n", 0) == 0,
        "raw /insights must write the TUI insight inquiry after the slot clears");
    submit_command(QStringLiteral("/molt"));
    require(read_file(target / ".prompt")
            == "[system] molt immediately",
        "raw /molt must write the TUI English mandatory molt prompt");
    require(status->text() == QStringLiteral("Molt command sent."),
        "raw /molt must report the TUI molt-sent status");
    submit_command(QStringLiteral("/export other"));
    require(status->text()
            == QStringLiteral("[system] Usage: /export — or — /export recipe"),
        "raw /export with an unknown argument must show the TUI usage line");
    submit_command(QStringLiteral("/export recipe"));
    require(read_file(target / ".prompt").find("lingtai-recipe")
            != std::string::npos,
        "raw /export recipe must write the TUI recipe-export prompt");
    require(status->text() == QStringLiteral(
            "[system] Asked the orchestrator to start the recipe export flow."),
        "raw /export must report the TUI recipe-sent status");
    submit_command(QStringLiteral("/goal finish the linked PR"));
    require(read_file(target / ".notification/system.json")
            .find("finish the linked PR") != std::string::npos,
        "raw /goal must append the human request to system.json");
    require(status->text().startsWith(
            QStringLiteral("Goal request notification sent (")),
        "raw /goal must report the TUI goal-sent status");

    submit_command(QStringLiteral("/help"));
    require(input->getLastText().isEmpty(),
        "raw /help must clear the composer after local submission");
    require(outbox_leaf_count() == 0,
        "raw /help must stay local and create no human outbox leaf");
    const auto help = status->text();
    require(!help.isEmpty() && help.size() <= 512,
        "raw /help must expose one bounded nonempty local response");
    for (const auto *command : {
             "/agents", "/presets", "/setup", "/kanban", "/sleep", "/cpr",
             "/clear", "/refresh", "/suspend", "/btw", "/insights", "/goal",
             "/export", "/molt", "/help", "/quit"}) {
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
    // surface under beta. Widen first so the canvas roster can receive a
    // real click while the detail stays open.
    window.resize(1200, 800);
    QCoreApplication::processEvents();
    click_agent_canvas_row(window, 1);
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
    click_agent_canvas_row(window, 0);
    remove_control_done();
    submit_command(QStringLiteral("/refresh codex-preset"));
    click_agent_canvas_row(window, 1);
    click_agent_canvas_row(window, 0);
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

    click_agent_canvas_row(window, 1);
    require(shell.selection_state().selected_agent_directory_key()
                == std::optional<fs::path>("beta")
            && input->isEnabled() && send_button->isEnabled(),
        "the stale beta fixture must be selected before CPR command checks");
    submit_command(QStringLiteral("/btw hello from beta"));
    require(status->text()
            == QStringLiteral("Agent is not running. Try /refresh first."),
        "raw /btw on a not-running Agent must match the TUI not-running status");
    require(!fs::exists(beta / ".inquiry"),
        "raw /btw on a not-running Agent must write no inquiry");
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
    click_agent(shell, "alpha");
    require(shell.selection_state().selected_agent_directory_key()
            == std::optional<fs::path>("alpha"),
        "alpha must be selectable for the two-surface hierarchy");
    QCoreApplication::processEvents();

    const auto color_close = [](const QColor &a, const QColor &b) {
        return qAbs(a.red() - b.red()) <= 12
            && qAbs(a.green() - b.green()) <= 12
            && qAbs(a.blue() - b.blue()) <= 12;
    };
    auto *conversation = required_child<QTextEdit>(
        window, "lingtai_selected_agent_conversation");

    // On the single light canvas every main surface paints the same base
    // windowBg fill: the sidebar, content, and composer all sit on the one
    // single-canvas base token, so no header/nav/conversation/composer surface
    // may paint a boxed fill of its own or an elevated band.
    require(color_close(sidebar->grab().toImage().pixelColor(2, 2),
            st::windowBg->c),
        "the sidebar must sit on the single-canvas base surface (windowBg)");
    const auto conversation_image = conversation->viewport()->grab().toImage();
    require(color_close(
            conversation_image.pixelColor(
                conversation_image.width() / 2,
                conversation_image.height() - 6),
            st::windowBg->c),
        "the conversation canvas must sit on the single-canvas base surface "
        "(windowBg)");
    require(color_close(composer->grab().toImage().pixelColor(2, 2),
            st::windowBg->c),
        "the composer must sit on the single-canvas base surface, never a "
        "full-width dark band of its own");

    // No hard-edged plain-shadow frames around the header/nav/conversation/
    // composer: dashboard section dividers may use one-pixel separators, but
    // the chat header, page nav, conversation, and composer must stay unboxed.
    auto disallowed_shadows = 0;
    auto *top_bar = required_child<QWidget>(window, "lingtai_chat_top_bar");
    auto *pages_nav = window.findChild<Ui::RpWidget *>(
        "lingtai_agent_pages_nav");
    for (auto *child : detail->findChildren<QWidget *>()) {
        auto *shadow = dynamic_cast<Ui::PlainShadow *>(child);
        if (!shadow) continue;
        if (top_bar->isAncestorOf(shadow)
            || (pages_nav && pages_nav->isAncestorOf(shadow))
            || conversation->isAncestorOf(shadow)
            || composer->isAncestorOf(shadow)) {
            ++disallowed_shadows;
        }
    }
    require(disallowed_shadows == 0,
        "no hard-edged plain-shadow frame may box the header, nav, "
        "conversation, or composer inside the detail");

    std::error_code cleanup_error;
    fs::remove_all(sandbox, cleanup_error);
    require(!cleanup_error, "two-surface fixtures must be removed");
}

void verify_preset_editor_model(const fs::path &sandbox) {
    using lingtai::desktop::PresetEditorModel;
    using lingtai::desktop::PresetEditorLoadRequest;
    using lingtai::desktop::auto_saved_preset_name;

    require(auto_saved_preset_name(QStringLiteral("codex"),
            {QStringLiteral("codex"), QStringLiteral("codex-1")})
            == QStringLiteral("codex-2"),
        "template clones gap-fill the next <name>-N saved stem");

    const auto dir = sandbox / "preset-editor-model";
    fs::create_directories(dir);
    const auto path = dir / "codex.json";
    {
        std::ofstream out(path);
        out << R"({
  "name": "codex",
  "description": {
    "summary": "ChatGPT account — vision + web search + tools",
    "tier": "5"
  },
  "manifest": {
    "llm": {
      "provider": "codex",
      "model": "gpt-5.6-sol",
      "base_url": "https://chatgpt.com/backend-api/codex",
      "thinking": "xhigh"
    }
  }
})";
    }
    PresetEditorModel model;
    model.load(PresetEditorLoadRequest{
        QString::fromStdString(path.string()),
        QStringLiteral("codex"),
        QStringLiteral("ChatGPT account — vision + web search + tools"),
        QStringLiteral("template"),
        true,
        {QStringLiteral("codex")},
    });
    require(model.is_codex_provider()
            && model.service_tier_visible()
            && model.thinking_visible()
            && !model.wire_api_visible()
            && model.service_tier() == QStringLiteral("normal")
            && model.thinking() == QStringLiteral("xhigh"),
        "Codex presets expose service tier and xhigh reasoning, not wire_api");
    model.set_model(QStringLiteral("gpt-5.6-terra"));
    model.set_service_tier(QStringLiteral("fast"));
    const auto committed = model.commit({QStringLiteral("codex")});
    require(committed.ok && committed.name == QStringLiteral("codex-1"),
        "semantic edits on a template must clone to an auto-saved name");
    require(committed.document.value(QStringLiteral("manifest")).toObject()
            .value(QStringLiteral("llm")).toObject()
            .value(QStringLiteral("service_tier")).toString()
            == QStringLiteral("fast"),
        "fast Codex service tier is persisted; normal is omitted");

    model.set_provider(QStringLiteral("custom"));
    model.set_api_compat(QStringLiteral("openai"));
    require(model.wire_api_visible() && !model.service_tier_visible(),
        "custom OpenAI shows wire_api and hides Codex service tier");
    require(model.base_url().isEmpty(),
        "switching away from Codex clears a URL that provider does not use");
    model.set_wire_api(QStringLiteral("responses"));
    require(model.responses_transport_visible(),
        "Responses transport appears only for custom OpenAI responses");
    model.set_provider(QStringLiteral("deepseek"));
    require(model.base_url() == QStringLiteral("https://api.deepseek.com")
            && model.model() == QStringLiteral("deepseek-v4-pro"),
        "DeepSeek adopts its first region URL and default model");
    model.set_provider(QStringLiteral("codex"));
    require(model.base_url()
                == QStringLiteral("https://chatgpt.com/backend-api/codex")
            && model.model() == QStringLiteral("gpt-5.6-sol"),
        "Codex restores the ChatGPT backend URL instead of keeping DeepSeek's");
    const auto providers = model.provider_options();
    require(providers.contains(QStringLiteral("gemini"))
            && providers.contains(QStringLiteral("kimi"))
            && providers.contains(QStringLiteral("codex-pool"))
            && providers.contains(QStringLiteral("claude-code")),
        "provider picker includes every TUI builtin, not only the cycle subset");
}

void verify_project_setup_wizard_contract(lingtai::desktop::NativeShell &shell) {
    auto &window = shell.window();
    auto *wizard = required_child<lingtai::desktop::ProjectSetupWizard>(
        window, "lingtai_project_setup_wizard");
    auto *steps = required_child<QWidget>(
        *wizard, "lingtai_setup_steps");
    auto *preset_page = required_child<QWidget>(
        *wizard, "lingtai_setup_preset_page");
    auto *edit_page = required_child<QWidget>(
        *wizard, "lingtai_setup_edit_preset_page");
    auto *agents_page = required_child<QWidget>(
        *wizard, "lingtai_setup_agents_page");
    auto *review_page = required_child<QWidget>(
        *wizard, "lingtai_setup_review_page");
    auto *preset_heading = required_child<QLabel>(
        *preset_page, "lingtai_setup_preset_heading");
    auto *catalog = required_child<QComboBox>(
        *preset_page, "lingtai_bootstrap_preset_chooser");
    auto *preset_catalog = required_child<QTreeWidget>(
        *preset_page, "lingtai_setup_preset_catalog");
    auto *preset_search = required_child<QLineEdit>(
        *preset_page, "lingtai_setup_preset_search");
    auto *preset_footer = required_child<QLabel>(
        *preset_page, "lingtai_setup_preset_footer_summary");
    auto *codex_strip = required_child<QWidget>(
        *preset_page, "lingtai_setup_codex_credentials_strip");
    auto *credentials_page = required_child<QWidget>(
        *wizard, "lingtai_setup_credentials_page");
    auto *edit_heading = required_child<QLabel>(
        *edit_page, "lingtai_setup_edit_preset_heading");
    auto *agent_heading = required_child<QLabel>(
        *agents_page, "lingtai_setup_agents_heading");
    auto *agents_search = required_child<QLineEdit>(
        *agents_page, "lingtai_setup_agents_search");
    if (st::windowBg->c.lightness() < 128) {
        require(!agents_search->styleSheet().contains(QStringLiteral("#FFFFFF"))
                && agents_search->styleSheet().contains(QStringLiteral("#202422")),
            "dark-mode Configure agent presets search must not be a white hole");
        for (auto *chip : agents_page->findChildren<QLabel *>(
                "lingtai_setup_agents_row_caps")) {
            if (chip->text() != QStringLiteral("Vision")
                    && chip->text() != QStringLiteral("Tools")) {
                continue;
            }
            require(!chip->styleSheet().contains(QStringLiteral("#FFFFFF"))
                    && !chip->styleSheet().contains(QStringLiteral("#E7F4EF"))
                    && chip->styleSheet().contains(QStringLiteral("#222A26")),
                "dark-mode Vision/Tools chips must use the muted band, not a white rectangle");
        }
    }
    auto *review_heading = required_child<QLabel>(
        *review_page, "lingtai_setup_review_heading");
    auto *continue_button = required_child<QPushButton>(
        *wizard, "lingtai_setup_preset_continue");
    auto *preset_back = required_child<QPushButton>(
        *wizard, "lingtai_setup_preset_back");
    auto *save_preset = required_child<QPushButton>(
        *wizard, "lingtai_setup_edit_preset_save");
    auto *edit_back = required_child<QPushButton>(
        *wizard, "lingtai_setup_edit_preset_back");
    auto *edit_cancel = required_child<QPushButton>(
        *wizard, "lingtai_setup_edit_preset_cancel");
    auto *review_button = required_child<QPushButton>(
        *wizard, "lingtai_setup_agents_continue");
    auto *create_button = required_child<QPushButton>(
        *wizard, "lingtai_bootstrap_create_start");

    require(wizard->minimumWidth() == 0 && wizard->minimumHeight() == 0
            && wizard->parentWidget()
            && wizard->parentWidget()->objectName()
                == QStringLiteral("lingtai_desktop_content"),
        "new-folder setup must be an in-window route inside the content column, "
        "not a separate dialog with its own inflated minimum size");
    require(dynamic_cast<QVBoxLayout *>(wizard->layout()) != nullptr
            && steps->height() <= 140,
        "Preset / Agents / Review flow must be one horizontal owner above the page, matching Ted's reference");
    require(preset_heading->sizePolicy().verticalPolicy()
                != QSizePolicy::MinimumExpanding
            && !wizard->styleSheet().contains(QStringLiteral("background: #ffffff")),
        "setup content must keep content-height labels and one theme palette, not expansive white-on-white surfaces");
    require(preset_heading->text()
            == QStringLiteral("Choose how your orchestrator runs")
            && edit_heading->text() == QStringLiteral("Edit preset")
            && agent_heading->text() == QStringLiteral("Configure agent presets")
            && review_heading->text() == QStringLiteral("Configure your agent"),
        "setup must expose the accepted Preset, Edit preset, agent-presets, and agent-config pages");
    require(catalog->isHidden()
            && preset_page->findChildren<QTreeWidget *>().size() == 1
            && preset_catalog->columnCount() == 3
            && preset_catalog->headerItem()->text(0) == QStringLiteral("Preset")
            && preset_catalog->headerItem()->text(1) == QStringLiteral("Provider / model")
            && preset_catalog->headerItem()->text(2) == QStringLiteral("Capabilities")
            && preset_catalog->header() != nullptr
            && !preset_catalog->header()->isHidden()
            && preset_catalog->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff
            && preset_catalog->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
        "Preset page must expose one three-column catalog with a sticky header and a single vertical scrollbar");
    require(preset_catalog->topLevelItemCount() >= 2
            && preset_catalog->topLevelItem(0)->text(0)
                == QStringLiteral("Saved presets")
            && preset_catalog->topLevelItem(1)->text(0)
                == QStringLiteral("Preset templates")
            && !preset_catalog->topLevelItem(0)->flags().testFlag(
                Qt::ItemIsSelectable)
            && preset_catalog->topLevelItem(0)->isFirstColumnSpanned(),
        "the catalog must introduce saved presets and templates with non-selectable section rows");
    require(preset_catalog->minimumHeight() == 0
            && wizard->findChild<QWidget *>("lingtai_setup_preset_detail") == nullptr
            && wizard->findChild<QLabel *>("lingtai_setup_step_progress") == nullptr,
        "preset selection must keep one scrollable catalog and a compact footer, "
        "not a boxed inspector or 1 of 3 chrome");
    require(codex_strip != nullptr
            && codex_strip->minimumHeight() >= 52,
        "preset page must expose a Codex credentials strip above the footer");
    require(credentials_page != nullptr,
        "credentials page must live behind the preset catalog stack");
    require(preset_catalog->palette().color(QPalette::Base)
                == QColor(wizard->palette().color(QPalette::Window).lightness() < 128
                    ? QStringLiteral("#181B1A")
                    : QStringLiteral("#FFFFFF"))
            && (preset_catalog->styleSheet().contains(QStringLiteral("#E7F4EF"))
                || preset_catalog->styleSheet().contains(QStringLiteral("#213A31")))
            && (preset_catalog->styleSheet().contains(QStringLiteral("#F1F3F2"))
                || preset_catalog->styleSheet().contains(QStringLiteral("#202422"))),
        "preset catalog chrome must use the supplied light/dark surface, header, and selection tokens");
    require(continue_button->text() == QStringLiteral("Use preset")
            && preset_back->isEnabled()
            && preset_back->text() == QStringLiteral("Back")
            && save_preset->text() == QStringLiteral("Save preset")
            && edit_back->text() == QStringLiteral("← Presets")
            && edit_cancel->text() == QStringLiteral("Cancel")
            && review_button->text() == QStringLiteral("Continue")
            && create_button->text() == QStringLiteral("Create orchestrator"),
        "the first setup step exposes an enabled Back to the homepage; Use/Configure is contextual; Edit preset saves; later pages keep Continue / Create orchestrator");
    require(preset_catalog->sizePolicy().verticalPolicy() == QSizePolicy::Expanding,
        "the preset catalog must stretch with the wizard instead of pinning a fixed height");
    wizard->setParent(nullptr);
    wizard->setAttribute(Qt::WA_DontShowOnScreen, true);
    wizard->resize(920, 840);
    wizard->show();
    QCoreApplication::processEvents();
    const auto below = [](QWidget *upper, QWidget *lower, QWidget *root) {
        return upper->mapTo(root, QPoint(0, upper->height())).y()
            <= lower->mapTo(root, QPoint(0, 0)).y();
    };
    const auto assert_preset_stack = [&]() {
        require(below(preset_catalog, codex_strip, wizard)
                && below(codex_strip, preset_footer, wizard)
                && below(codex_strip, continue_button, wizard),
            "preset catalog, Codex strip, and compact footer must stack without overlapping");
    };
    const auto click_widget = [](QWidget *widget) {
        const auto center = widget->rect().center();
        const auto global = widget->mapToGlobal(center);
        QMouseEvent press(QEvent::MouseButtonPress, center, global, Qt::LeftButton,
            Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, center, global, Qt::LeftButton,
            Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(widget, &press);
        QCoreApplication::sendEvent(widget, &release);
    };
    require(codex_strip->isVisible(),
        "Codex credentials strip must be visible once the wizard is shown");
    auto *pages = required_child<QStackedWidget>(
        *wizard, "lingtai_setup_pages");
    auto *manage = required_child<QPushButton>(
        *codex_strip, "lingtai_setup_codex_credentials_action");
    manage->click();
    QCoreApplication::processEvents();
    require(pages->currentWidget() == credentials_page
            && required_child<QLabel>(*credentials_page,
                "lingtai_setup_credentials_heading")->text()
                == QStringLiteral("Credentials")
            && required_child<QWidget>(*credentials_page,
                "lingtai_setup_credentials_hero") != nullptr,
        "Codex credentials Manage must open the credentials page");
    required_child<QPushButton>(*credentials_page,
        "lingtai_setup_credentials_back")->click();
    QCoreApplication::processEvents();
    require(pages->currentWidget() == preset_page,
        "credentials back must return to preset catalog selection");
    click_widget(codex_strip);
    QCoreApplication::processEvents();
    require(pages->currentWidget() == credentials_page
            && required_child<QLabel>(*credentials_page,
                "lingtai_setup_credentials_heading")->text()
                == QStringLiteral("Credentials"),
        "clicking the Codex credentials strip must open the credentials page");
    required_child<QPushButton>(*credentials_page,
        "lingtai_setup_credentials_back")->click();
    QCoreApplication::processEvents();
    require(pages->currentWidget() == preset_page,
        "credentials back must return to preset catalog selection");
    wizard->resize(1100, 840);
    QCoreApplication::processEvents();
    pages->setCurrentWidget(credentials_page);
    QCoreApplication::processEvents();
    wizard->resize(380, 480);
    QCoreApplication::processEvents();
    pages->setCurrentWidget(preset_page);
    QCoreApplication::processEvents();
    require(preset_page->width() <= pages->width() + 1
            && preset_catalog->width() <= pages->width() + 1,
        "returning to the preset catalog after a hidden resize must keep the "
        "page inside the setup stack, never the previous wider pane");
    assert_preset_stack();
    auto *edit_manage = required_child<QPushButton>(
        *edit_page, "lingtai_setup_edit_preset_manage");
    require(edit_manage->cursor().shape() == Qt::PointingHandCursor
            && edit_manage->styleSheet().contains(QStringLiteral("#16785C")),
        "Edit preset Manage must be a jade action button, not a text label");
    auto *edit_name = required_child<QLineEdit>(
        *edit_page, "lingtai_setup_edit_preset_name");
    if (st::windowBg->c.lightness() < 128) {
        require(edit_back->styleSheet().contains(QStringLiteral("#78C9A7"))
                && !edit_name->styleSheet().contains(QStringLiteral("#FFFFFF"))
                && edit_name->styleSheet().contains(QStringLiteral("#202422")),
            "dark-mode Edit preset must keep ← Presets readable and fields off the white hole fill");
    } else {
        require(edit_back->styleSheet().contains(QStringLiteral("#16785C")),
            "light-mode Edit preset back link stays jade");
    }
    pages->setCurrentWidget(edit_page);
    QCoreApplication::processEvents();
    edit_manage->click();
    QCoreApplication::processEvents();
    require(pages->currentWidget() == credentials_page,
        "Edit preset Manage must open the credentials page");
    required_child<QPushButton>(*credentials_page,
        "lingtai_setup_credentials_back")->click();
    QCoreApplication::processEvents();
    require(pages->currentWidget() == edit_page,
        "credentials back from Edit preset must return to the editor");
    pages->setCurrentWidget(preset_page);
    QCoreApplication::processEvents();
    assert_preset_stack();
    require(preset_search->width() <= 520
            && preset_search->width() < wizard->width(),
        "search must stay narrower than the wizard instead of stretching edge "
        "to edge");
    require(preset_catalog->visualItemRect(preset_catalog->topLevelItem(0)).height()
                <= 36,
        "section labels must be short tinted bands, not full-height data rows");
    require(preset_catalog->height() > preset_catalog->minimumHeight(),
        "the catalog must take leftover wizard height inside one scroll owner");
    const auto compact_catalog = preset_catalog->height();
    wizard->resize(1100, 1040);
    QCoreApplication::processEvents();
    require(preset_catalog->height() > compact_catalog
            && below(preset_catalog, codex_strip, wizard)
            && below(codex_strip, preset_footer, wizard)
            && below(codex_strip, continue_button, wizard),
        "extra wizard height must go into the preset catalog without introducing overlap");
    auto *email_cap = required_child<QCheckBox>(
        *edit_page, "lingtai_setup_edit_preset_cap_email");
    require(email_cap->isChecked() && !email_cap->isEnabled(),
        "capabilities in the preset editor are always included and not toggled here");
    require(pages->indexOf(preset_page) == 0
            && pages->indexOf(edit_page) == 1
            && pages->indexOf(agents_page) == 2
            && pages->indexOf(review_page) == 3
            && pages->indexOf(credentials_page) == 4,
        "Edit preset must be the page after catalog selection, before Agents; credentials is a subpage");
    required_child<QLineEdit>(*agents_page, "lingtai_setup_agents_search");
    required_child<QLineEdit>(*review_page, "lingtai_setup_review_agent_name");
    required_child<QLineEdit>(*review_page, "lingtai_setup_review_folder_name");
    auto *language = required_child<QComboBox>(
        *review_page, "lingtai_setup_review_language");
    auto *context_limit = required_child<QSpinBox>(
        *review_page, "lingtai_setup_review_context_limit");
    auto *karma = required_child<QCheckBox>(
        *review_page, "lingtai_setup_review_karma");
    auto *nirvana = required_child<QCheckBox>(
        *review_page, "lingtai_setup_review_nirvana");
    auto *soul_flow = required_child<QCheckBox>(
        *review_page, "lingtai_setup_review_soul_flow");
    require(language->itemData(0).toString() == QStringLiteral("en")
            && context_limit->value() == 300000
            && wizard->findChild<QLabel *>("lingtai_setup_agent_card") == nullptr
            && wizard->findChild<QLabel *>("lingtai_setup_review_summary") == nullptr
            && wizard->findChild<QLabel *>("lingtai_setup_step_index") != nullptr,
        "agent-presets and agent-config replace the old folder-card review, with TUI-parity fields");
    require(karma->isChecked() && !nirvana->isChecked() && !soul_flow->isChecked(),
        "authority defaults match the TUI: Karma on, Nirvana and Soul flow off");
    preset_back->click();
    QCoreApplication::processEvents();
    require(!wizard->isVisible(),
        "Back on the first setup step must leave setup and return to the homepage");
    wizard->hide();
    for (auto *label : preset_page->findChildren<QLabel *>()) {
        const auto text = label->text();
        require(!text.contains(QStringLiteral("Tier"))
                && !text.contains(QStringLiteral("Context")),
            "V1 must not claim Tier or Context capabilities");
    }
}

namespace {

using lingtai::desktop::NativeShell;

template<typename Fn>
void with_offscreen_shell(Fn &&run) {
    NativeShell shell;
    shell.show_offscreen();
    QCoreApplication::processEvents();
    run(shell);
}

void run_native_shell_journey(
        std::string_view journey,
        const fs::path &project_root) {
    if (journey == "semantics") {
        const auto original_palette = QApplication::palette();
        verify_dark_application_palette_inheritance(
            project_root / "commit-8-palette-fixture");
        require(QApplication::palette() == original_palette,
            "dark palette test must restore the application palette");
        with_offscreen_shell([&](NativeShell &shell) {
            verify_live_system_palette(shell);
            verify_removed_activity_and_task_card_destinations(shell);
            verify_semantics_and_request(shell, project_root);
        });
        return;
    }
    if (journey == "bootstrap") {
        with_offscreen_shell([&](NativeShell &shell) {
            verify_first_project_bootstrap(
                shell, project_root / "commit-22-bootstrap-fixture");
        });
        return;
    }
    if (journey == "roster") {
        with_offscreen_shell([&](NativeShell &shell) {
            verify_persistent_roster_shell(
                shell, project_root / "commit-24-roster-shell-fixture");
            verify_open_project_behavior(
                shell, project_root / "commit-7-open-project-fixtures");
        });
        verify_open_project_in_another_window(
            project_root / "multi-window-open-fixture");
        return;
    }
    if (journey == "conversation") {
        with_offscreen_shell([&](NativeShell &shell) {
            verify_selected_agent_conversation(
                shell, project_root / "commit-13-conversation-fixture");
            verify_composer_send_behavior(
                shell, project_root / "commit-14-composer-fixture");
            verify_conversation_slash_interception(
                shell, project_root / "u2-slash-interception-fixture");
        });
        return;
    }
    if (journey == "lifecycle") {
        with_offscreen_shell([&](NativeShell &shell) {
            verify_request_sleep_action(
                shell, project_root / "commit-16-sleep-fixture");
            verify_start_agent_action(
                shell, project_root / "commit-17-start-fixture");
            verify_agent_preset_summary_panel(
                shell, project_root / "commit-19-preset-summary-fixture");
        });
        return;
    }
    if (journey == "layout") {
        with_offscreen_shell([&](NativeShell &shell) {
            verify_layout(shell, project_root / "commit-30-responsive-fixture");
            verify_resizable_sidebar(
                shell, project_root / "commit-r1-resizable-sidebar-fixture");
            verify_selected_agent_dashboard_layout(
                shell, project_root / "commit-28-dashboard-fixture");
            verify_compact_header_hierarchy(
                shell, project_root / "commit-32-compact-header-fixture");
            verify_responsive_header_priority(
                shell, project_root / "commit-r4-responsive-header-fixture");
        });
        return;
    }
    if (journey == "theme") {
        with_offscreen_shell([&](NativeShell &shell) {
            verify_telegram_theme_reset(
                shell, project_root / "commit-31-theme-reset-fixture");
            verify_two_surface_hierarchy(
                shell, project_root / "commit-s1-two-surface-fixture");
        });
        return;
    }
    if (journey == "composer") {
        with_offscreen_shell([&](NativeShell &shell) {
            verify_modern_composer_surface(
                shell, project_root / "commit-m4-composer-surface-fixture");
            verify_plain_underline_page_tabs(
                shell, project_root / "commit-tab-plain-underline-fixture");
            verify_floating_composer_surface(
                shell, project_root / "commit-fc-floating-composer-fixture");
        });
        return;
    }
    if (journey == "kanban") {
        with_offscreen_shell([&](NativeShell &shell) {
            verify_kanban_page(
                shell, project_root / "kanban-page-fixture");
            verify_preset_editor_model(
                project_root / "preset-editor-model-fixture");
            verify_project_setup_wizard_contract(shell);
        });
        return;
    }
    if (journey == "all") {
        constexpr std::array kStages = {
            "semantics",
            "bootstrap",
            "roster",
            "conversation",
            "lifecycle",
            "layout",
            "theme",
            "composer",
            "kanban",
        };
        for (const auto *stage : kStages) {
            run_native_shell_journey(stage, project_root);
        }
        return;
    }
    throw std::runtime_error(
        std::string("unknown native shell journey: ")
        + std::string(journey));
}

[[nodiscard]] std::optional<std::string_view> parse_journey_flag(
        std::string_view arg) {
    constexpr auto prefix = std::string_view("--journey=");
    if (arg.starts_with(prefix)) {
        return arg.substr(prefix.size());
    }
    return std::nullopt;
}

} // namespace

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
    const auto project_setup_only = argc == 3
        && std::string_view(argv[2]) == "--project-setup-only";
    const auto kanban_only = argc == 3
        && std::string_view(argv[2]) == "--kanban-only";
    const auto journey_flag = argc == 3
        ? parse_journey_flag(argv[2]) : std::nullopt;
    const auto has_legacy_flag = responsive_sidebar_only
        || responsive_header_only || modern_composer_only
        || slash_interception_only || compact_header_only
        || two_surface_only || plain_underline_only
        || floating_composer_only || project_setup_only || kanban_only;
    if (argc != 2 && !has_legacy_flag && !journey_flag) {
        std::cerr << "usage: native_shell_test PROJECT_ROOT "
                     "[--journey=NAME|--journey=all|"
                     "--responsive-sidebar-only|--responsive-header-only|"
                     "--modern-composer-only|--slash-interception-only|"
                     "--compact-header-only|--two-surface-only|"
                     "--plain-underline-only|--floating-composer-only|"
                     "--project-setup-only|--kanban-only]\n"
                     "  journeys: semantics bootstrap roster conversation "
                     "lifecycle layout theme composer kanban all\n";
        return 2;
    }
    try {
        const auto project_root = std::filesystem::canonical(argv[1]);
        std::filesystem::current_path(project_root);
        qputenv("QT_LOGGING_RULES",
            "qt.qpa.fonts.warning=false;qt.qpa.keymapper.warning=false");
        QApplication application(argc, argv);
        lingtai::desktop::ui_test::applyUiTestFontDefaults();
        if (responsive_sidebar_only) {
            lingtai::desktop::NativeShell shell;
            shell.show_offscreen();
            QCoreApplication::processEvents();
            verify_resizable_sidebar(
                shell, project_root / "commit-r1-resizable-sidebar-fixture");
            std::cout << "native shell behavior: OK\n";
            return 0;
        }
        if (responsive_header_only) {
            lingtai::desktop::NativeShell shell;
            shell.show_offscreen();
            QCoreApplication::processEvents();
            verify_responsive_header_priority(
                shell, project_root / "commit-r4-responsive-header-fixture");
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
        if (project_setup_only) {
            lingtai::desktop::NativeShell shell;
            shell.show_offscreen();
            QCoreApplication::processEvents();
            verify_preset_editor_model(project_root / "preset-editor-model-fixture");
            verify_project_setup_wizard_contract(shell);
            std::cout << "native shell behavior: OK\n";
            return 0;
        }
        if (kanban_only) {
            lingtai::desktop::NativeShell shell;
            shell.show_offscreen();
            QCoreApplication::processEvents();
            verify_kanban_page(
                shell, project_root / "kanban-page-fixture");
            std::cout << "native shell behavior: OK\n";
            return 0;
        }
        const auto journey = journey_flag
            ? *journey_flag
            : std::string_view("all");
        run_native_shell_journey(journey, project_root);
        std::cout << "native shell behavior: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "native shell behavior: " << error.what() << '\n';
        return 1;
    }
}
