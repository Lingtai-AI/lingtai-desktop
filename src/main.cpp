#include "native_shell.h"

#include <QtCore/QTimer>
#include <QtWidgets/QApplication>

#include <iostream>
#include <string_view>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const auto smoke_mode = argc == 2
        && std::string_view(argv[1]) == "--smoke";
    const auto offscreen_mode = smoke_mode || (argc == 2
        && std::string_view(argv[1]) == "--offscreen");

    lingtai::desktop::NativeShell shell;
    if (offscreen_mode) {
        shell.show_offscreen();
    } else {
        shell.show();
    }

    if (smoke_mode) {
        QTimer::singleShot(0, &app, [&] {
            const auto state = shell.snapshot();
            const auto ready = state.window_class == "Ui::RpWindow"
                && state.window_object == "lingtai_desktop_window"
                && state.body_object == "lingtai_desktop_body"
                && state.sidebar_object == "lingtai_desktop_sidebar"
                && state.content_object == "lingtai_desktop_content"
                && state.empty_route_visible
                && state.positioned_offscreen
                && state.shown;
            if (!ready) {
                std::cerr << "LINGTAI_LIB_UI_SMOKE_SHELL_NOT_READY"
                          << " class=" << state.window_class
                          << " window=" << state.window_object
                          << " body=" << state.body_object
                          << " sidebar=" << state.sidebar_object
                          << " content=" << state.content_object
                          << " empty=" << state.empty_route_visible
                          << " offscreen=" << state.positioned_offscreen
                          << " shown=" << state.shown
                          << std::endl;
                app.exit(98);
                return;
            }
            std::cout << "LINGTAI_NATIVE_SHELL_READY" << std::endl;
            std::cout
                << "LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK"
                << " qt=" << qVersion()
                << " class=" << state.window_class
                << " window=" << state.window_object
                << " body=" << state.body_object
                << " sidebar=" << state.sidebar_object
                << " content=" << state.content_object
                << " empty=visible"
                << " offscreen=true"
                << " shown=true"
                << std::endl;
            app.quit();
        });
        QTimer::singleShot(3000, &app, [&] {
            std::cerr << "LINGTAI_LIB_UI_SMOKE_TIMEOUT" << std::endl;
            app.exit(99);
        });
    }

    return app.exec();
}
