#include "native_shell.h"

#include "ui/widgets/rp_window.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>

#include <iostream>
#include <stdexcept>
#include <string>

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

// The Repair3 destination contract: a fresh real NativeShell with no project
// and no external fixture must not expose any surviving Activity or Task Card
// destination, while the retained Conversation/Presets navigation and the
// low-level status_activity fact label keep their exact anchors.
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
    // with its stable identity.
    auto *status_activity = required_child<QLabel>(
        window, "lingtai_selected_agent_status_activity");
    require(status_activity->accessibleName()
            == QStringLiteral("Status activity"),
        "the low-level selected-Agent status_activity fact label must retain "
        "its stable identity");
}

} // namespace

int main(int argc, char **argv) {
    try {
        QApplication application(argc, argv);
        lingtai::desktop::ConversationUnreadSession unread_session;
        lingtai::desktop::NativeShell shell(unread_session);
        shell.show_offscreen();
        QCoreApplication::processEvents();
        verify_removed_activity_and_task_card_destinations(shell);
        std::cout << "native shell destinations: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "native shell destinations: " << error.what() << '\n';
        return 1;
    }
}
