#include "native_shell.h"

#include "ui/rp_widget.h"
#include "ui/widgets/rp_window.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

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

std::vector<std::string> project_tree(
        const std::filesystem::path &root) {
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
        lingtai::desktop::NativeShell shell;
        shell.show_offscreen();
        QCoreApplication::processEvents();
        verify_semantics_and_request(shell, project_root);
        verify_layout(shell);
        std::cout << "native shell behavior: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "native shell behavior: " << error.what() << '\n';
        return 1;
    }
}
