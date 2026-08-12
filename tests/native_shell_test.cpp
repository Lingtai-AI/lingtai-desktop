#include "native_shell.h"

#include "ui/rp_widget.h"
#include "ui/widgets/rp_window.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
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

constexpr auto kReceipt = R"({"schema":"lingtai.tui.install/v1","schema_version":1,"install_method":"source","stamped_version":"v1","resolved_commit":"abc","kernel_source":"bundle","kernel_version":"0.1"})";
constexpr auto kInit = R"({"manifest":{"capabilities":{"shell":{"enabled":true}}}})";
constexpr auto kResolved = R"({"schema":"lingtai.manifest.resolved/v1","schema_version":1,"source":"kernel","manifest":{"llm":{}}})";

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

struct CompatibilityFixture {
    fs::path root;
    fs::path project;
    fs::path agent;
    fs::path receipt;

    CompatibilityFixture(const fs::path &base, std::string_view name)
    : root(base / name)
    , project(root / "project")
    , agent(project / ".lingtai" / "agent")
    , receipt(root / "install.json") {
        write_fresh_config(kInit, kResolved);
        write_file(receipt, kReceipt);
    }

    void write_fresh_config(
            std::string_view init,
            std::string_view resolved) const {
        const auto init_path = agent / "init.json";
        const auto resolved_path = agent / "system/manifest.resolved.json";
        write_file(init_path, init);
        write_file(resolved_path, resolved);
        std::error_code error;
        const auto now = fs::file_time_type::clock::now();
        fs::last_write_time(init_path, now - std::chrono::seconds(2), error);
        require(!error, "init fixture timestamp must be set");
        fs::last_write_time(resolved_path, now, error);
        require(!error, "resolved fixture timestamp must be set");
    }
};

lingtai::desktop::ProjectOpenOutcome open_without_writes(
        lingtai::desktop::NativeShell &shell,
        const CompatibilityFixture &fixture,
        const std::optional<fs::path> &agent,
        const fs::path &receipt) {
    const auto project_before = tree_snapshot(fixture.project);
    const auto receipt_existed = fs::exists(receipt);
    const auto receipt_before = receipt_existed ? read_file(receipt) : std::string();
    const auto outcome = shell.open_project(fixture.project, receipt, agent);
    require(tree_snapshot(fixture.project) == project_before,
        "every open/probe must preserve project bytes and paths");
    require(fs::exists(receipt) == receipt_existed,
        "every open/probe must preserve receipt existence");
    if (receipt_existed) {
        require(read_file(receipt) == receipt_before,
            "every open/probe must preserve receipt bytes");
    }
    return outcome;
}

QString label_text(QWidget &window, const char *object_name) {
    return required_child<QLabel>(window, object_name)->text();
}

void verify_dark_application_palette_inheritance() {
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
        window, "lingtai_project_compatibility_route");
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
        required_child<QLabel>(window, "lingtai_compatibility_title"),
        required_child<QLabel>(window, "lingtai_commands_availability"),
        required_child<QLabel>(window, "lingtai_compatibility_findings"),
    };
    auto *open_button = required_child<QPushButton>(
        window, "lingtai_open_project_button");

    for (const auto *surface : {
             body, sidebar, content, empty_route, error_surface,
             project_route }) {
        require(surface->palette().color(QPalette::Window) == window_surface,
            "dark application Window role must reach every shell surface");
    }
    for (const auto *label : labels) {
        require(label->palette().color(QPalette::WindowText) == window_ink,
            "dark application WindowText role must reach every shell label");
    }
    require(open_button->palette().color(QPalette::Button) == button_surface,
        "dark application Button role must reach the open affordance");
    require(open_button->palette().color(QPalette::ButtonText) == button_ink,
        "dark application ButtonText role must reach the open affordance");
}

void verify_open_project_behavior(
        lingtai::desktop::NativeShell &shell,
        const fs::path &sandbox) {
    auto &window = shell.window();
    auto *empty_route = required_child<Ui::RpWidget>(
        window, "lingtai_empty_workspace_route");
    auto *project_route = required_child<Ui::RpWidget>(
        window, "lingtai_project_compatibility_route");
    auto *error_surface = required_child<Ui::RpWidget>(
        window, "lingtai_project_open_error_surface");

    CompatibilityFixture compatible(sandbox, "compatible");
    const auto compatible_outcome = open_without_writes(
        shell, compatible, fs::path(".lingtai/agent"), compatible.receipt);
    const auto compatible_root = fs::canonical(compatible.project);
    require(compatible_outcome.disposition == ProjectOpenDisposition::compatible,
        "recognized project inputs must be Compatible");
    require(compatible_outcome.commands_allowed,
        "recognized project inputs must allow commands");
    require(shell.selection_state().active_project()
            && shell.selection_state().active_project()->root() == compatible_root,
        "successful open must activate the canonical C1 project root");
    require(!empty_route->isVisible() && project_route->isVisible(),
        "successful open must transition to the compatibility route");
    require(!error_surface->isVisible(),
        "successful open must clear the open error");
    require(label_text(window, "lingtai_project_root").toStdString()
            == compatible_root.string(),
        "compatibility route must display the canonical root");
    require(label_text(window, "lingtai_compatibility_title") == "Compatible",
        "compatible route title must be exact");
    require(label_text(window, "lingtai_commands_availability")
            == "Commands available: Yes",
        "compatible route must visibly allow commands");

    CompatibilityFixture no_agent(sandbox, "no-agent");
    const auto no_agent_outcome = open_without_writes(
        shell, no_agent, std::nullopt, no_agent.receipt);
    require(no_agent_outcome.disposition == ProjectOpenDisposition::degraded
            && !no_agent_outcome.commands_allowed,
        "no Agent must be Degraded read-only with commands disabled");
    require(label_text(window, "lingtai_compatibility_title")
            == QString::fromUtf8("Degraded — read-only"),
        "degraded title must make read-only status explicit");
    require(label_text(window, "lingtai_compatibility_findings")
            .contains("Select an Agent"),
        "no-Agent degradation must give the Agent-selection reason");

    CompatibilityFixture missing_receipt(sandbox, "missing-receipt");
    fs::remove(missing_receipt.receipt);
    const auto missing_receipt_outcome = open_without_writes(
        shell, missing_receipt, fs::path(".lingtai/agent"),
        missing_receipt.receipt);
    require(missing_receipt_outcome.disposition
            == ProjectOpenDisposition::degraded
            && !missing_receipt_outcome.commands_allowed,
        "missing receipt must attach read-only and disable commands");
    require(label_text(window, "lingtai_compatibility_findings")
            .contains("install receipt is missing"),
        "missing receipt must remain a distinct visible reason");

    CompatibilityFixture unsupported(sandbox, "unsupported-resolved");
    unsupported.write_fresh_config(kInit,
        R"({"schema":"other","schema_version":1,"source":"kernel","manifest":{}})");
    const auto unsupported_outcome = open_without_writes(
        shell, unsupported, fs::path(".lingtai/agent"), unsupported.receipt);
    require(unsupported_outcome.disposition == ProjectOpenDisposition::degraded
            && !unsupported_outcome.commands_allowed,
        "unsupported resolved schema must degrade and disable commands");
    require(label_text(window, "lingtai_compatibility_findings")
            .contains("resolved Agent manifest schema is unsupported"),
        "unsupported resolved schema must retain its distinct reason");

    CompatibilityFixture legacy(sandbox, "legacy-alias");
    legacy.write_fresh_config(
        R"({"manifest":{"capabilities":{"bash":{"enabled":true}}}})",
        kResolved);
    const auto legacy_outcome = open_without_writes(
        shell, legacy, fs::path(".lingtai/agent"), legacy.receipt);
    require(legacy_outcome.disposition == ProjectOpenDisposition::degraded
            && !legacy_outcome.commands_allowed,
        "legacy bash alias must remain a read-only degradation");
    require(label_text(window, "lingtai_compatibility_findings")
            .contains("legacy bash capability alias"),
        "legacy alias must retain its distinct visible reason");

    CompatibilityFixture conflict(sandbox, "capability-conflict");
    conflict.write_fresh_config(
        R"({"manifest":{"capabilities":{"bash":{"x":1},"shell":{"x":2}}}})",
        kResolved);
    const auto conflict_outcome = open_without_writes(
        shell, conflict, fs::path(".lingtai/agent"), conflict.receipt);
    require(conflict_outcome.disposition == ProjectOpenDisposition::blocked
            && !conflict_outcome.commands_allowed,
        "conflicting capabilities must be Blocked with commands disabled");
    require(label_text(window, "lingtai_compatibility_title") == "Blocked",
        "blocked state must be visibly distinct");
    require(label_text(window, "lingtai_compatibility_findings")
            .contains("conflicting bash and shell capabilities"),
        "blocked conflict must retain its explicit reason");

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
    const auto shared_receipt_before = read_file(compatible.receipt);
    auto failed = failed_shell.open_project(ordinary, compatible.receipt);
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
    failed = failed_shell.open_project(invalid_metadata, compatible.receipt);
    require(failed.failure
            == lingtai::desktop::ProjectPathFailure::target_not_directory,
        "non-directory .lingtai must have a typed visible failure");
    require(tree_snapshot(invalid_metadata) == invalid_metadata_before,
        "non-directory .lingtai failure must write nothing");

    const auto non_directory = invalid_root / "not-a-directory";
    write_file(non_directory, "file");
    failed = failed_shell.open_project(non_directory, compatible.receipt);
    require(failed.failure
            == lingtai::desktop::ProjectPathFailure::selection_not_directory,
        "non-directory selection must have a typed visible failure");
    failed = failed_shell.open_project(
        invalid_root / "absent", compatible.receipt);
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
    failed = failed_shell.open_project(escape_project, compatible.receipt);
    require(failed.failure
            == lingtai::desktop::ProjectPathFailure::outside_project,
        "escaping .lingtai symlink must be rejected as outside project");
    require(tree_snapshot(escape_project) == escape_before,
        "escaping .lingtai failure must write nothing");
    require(!failed_shell.selection_state().active_project(),
        "all fresh failed opens must preserve empty C1 state");

    const auto active_root = shell.selection_state().active_project()->root();
    const auto prior_title = label_text(window, "lingtai_compatibility_title");
    const auto prior_commands = label_text(
        window, "lingtai_commands_availability");
    const auto prior_findings = label_text(
        window, "lingtai_compatibility_findings");
    const auto later_failure = shell.open_project(ordinary, compatible.receipt);
    require(later_failure.disposition == ProjectOpenDisposition::failed,
        "failed reopen must return failed");
    require(shell.selection_state().active_project()
            && shell.selection_state().active_project()->root() == active_root,
        "failed reopen must preserve the valid active project");
    require(label_text(window, "lingtai_compatibility_title") == prior_title
            && label_text(window, "lingtai_commands_availability")
                == prior_commands
            && label_text(window, "lingtai_compatibility_findings")
                == prior_findings,
        "failed reopen must preserve the established compatibility report");
    require(error_surface->isVisible()
            && label_text(window, "lingtai_project_open_error")
                .contains("not a LingTai project"),
        "failed reopen must show its error beside the preserved report");

    const auto reopened = open_without_writes(
        shell, compatible, fs::path(".lingtai/agent"), compatible.receipt);
    require(reopened.disposition == ProjectOpenDisposition::compatible
            && !error_surface->isVisible(),
        "a later successful open must clear the transient open error");
    require(read_file(compatible.receipt) == shared_receipt_before,
        "all failed and successful opens must preserve global receipt bytes");

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
        window, "lingtai_project_compatibility_route");
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
    require(selection.recent_project_roots().empty(),
        "new shell must have no recents");
    require(empty_route->isVisible(),
        "no-workspace truth must show the empty route");
    require(!project_route->isVisible(),
        "new shell must hide the project compatibility route");
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
    require(shell.open_project_request_count() == 1,
        "shell request count must observe exactly one request");
    require(!selection.active_project().has_value(),
        "open request must not activate a project");
    require(!selection.selected_agent_directory_key().has_value(),
        "open request must not select an Agent");
    require(selection.recent_project_roots().empty(),
        "open request must not change recents");
    require(project_tree(project_root) == tree_before,
        "open request must not write the project tree");
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
        verify_dark_application_palette_inheritance();
        require(QApplication::palette() == original_palette,
            "dark palette test must restore the application palette");
        lingtai::desktop::NativeShell shell;
        shell.show_offscreen();
        QCoreApplication::processEvents();
        verify_semantics_and_request(shell, project_root);
        verify_open_project_behavior(
            shell, project_root / "commit-7-open-project-fixtures");
        verify_layout(shell);
        std::cout << "native shell behavior: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "native shell behavior: " << error.what() << '\n';
        return 1;
    }
}
