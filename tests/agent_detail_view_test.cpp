#include "agent_detail_view.h"
#include "runtime_options.h"

#include "styles/palette.h"

#include <iostream>

#include <QtCore/QCoreApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>

namespace {
int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main(int argc, char **argv) {
    (void)argv;
    QApplication app(argc, argv);

    // The view relies on style/palette globals owned by lib_ui.
    style::internal::init_palette(style::kScaleDefault);

    QScrollArea outer;
    outer.setWidgetResizable(true);
    outer.resize(800, 600);

    const auto options = lingtai::desktop::RuntimeOptions{
        .offscreen_mode = false,
        .smoke_mode = false,
        .deterministic_ui = true,
    };

    lingtai::desktop::AgentDetailView view(options, &outer);

    // Avoid showing widgets: widget-local `hide()`/`show()` state is enough
    // for this visibility contract, and showing can trigger macOS platform
    // services (clipboard, notifications) during headless CI.
    QCoreApplication::processEvents();

    auto *pages_nav
        = view.findChild<Ui::RpWidget *>("lingtai_agent_pages_nav");
    auto *conversation
        = view.findChild<QWidget *>("lingtai_selected_agent_conversation");
    auto *conversation_heading
        = view.findChild<QLabel *>(
            "lingtai_selected_agent_conversation_heading");
    auto *nav_conversation = view.findChild<QPushButton *>(
        "lingtai_agent_page_nav_conversation");
    auto *nav_presets = view.findChild<QPushButton *>(
        "lingtai_agent_page_nav_presets");

    expect(pages_nav != nullptr, "page nav container must exist");
    expect(conversation != nullptr, "conversation surface must exist");
    expect(conversation_heading != nullptr,
        "conversation anchor heading must exist");
    expect(nav_conversation != nullptr, "conversation nav button must exist");
    expect(nav_presets != nullptr, "presets nav button must exist");

    // Initial page is Conversation.
    expect(pages_nav->isHidden(),
        "page nav must be hidden on Conversation");
    expect(conversation_heading->isHidden(),
        "conversation anchor heading must be hidden on Conversation");
    expect(!conversation->isHidden(),
        "conversation surface must be visible on Conversation");

    // Switch to Presets: nav should appear as "← Conversation", and the
    // conversation surface must be hidden.
    view.set_page(lingtai::desktop::AgentDetailPage::presets);
    QCoreApplication::processEvents();

    expect(!pages_nav->isHidden(),
        "page nav must be visible on Presets");
    expect(!nav_conversation->isHidden(),
        "back (" "← Conversation" ") nav must be visible on Presets");
    expect(nav_presets->isHidden(),
        "Presets tab (" "Presets" ") must be hidden on Presets");
    expect(conversation->isHidden(),
        "conversation surface must be hidden on Presets");

    if (failures != 0) return 1;
    std::cout << "agent_detail_view: OK\n";
    return 0;
}

