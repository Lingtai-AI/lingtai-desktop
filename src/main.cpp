#include "shell_host.h"
#include "runtime_options.h"

#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtGui/QFont>
#include <QtGui/QIcon>

#include <iostream>

int main(int argc, char **argv) {
    // QT_LOGGING_RULES is semicolon-separated. A newline makes Qt treat both
    // lines as one malformed rule, so the Open Sans / keymapper warnings
    // still print.
    qputenv("QT_LOGGING_RULES",
        "qt.qpa.fonts.warning=false;qt.qpa.keymapper.warning=false");
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("LingTai"));
    app.setOrganizationName(QStringLiteral("LingTai"));
    app.setWindowIcon(QIcon(
        QStringLiteral(":/lingtai/macos/AppIcon-1024.png")));
    QFont::insertSubstitutions(QStringLiteral("Open Sans"), {
        QStringLiteral(".AppleSystemUIFont"),
        QStringLiteral("Helvetica Neue"),
        QStringLiteral("Helvetica"),
        QStringLiteral("Arial"),
    });
    const auto runtime_options =
        lingtai::desktop::resolve_runtime_options(argc, argv);
    const auto offscreen_mode = runtime_options.offscreen_mode;
    const auto smoke_mode = runtime_options.smoke_mode;

    lingtai::desktop::ShellHost host(runtime_options);
    auto &shell = host.primary();
    if (offscreen_mode) {
        shell.show_offscreen();
    } else {
        shell.show();
    }

    if (runtime_options.ui_test_mode && !runtime_options.fixture_path.empty()) {
        static_cast<void>(shell.open_project(runtime_options.fixture_path));
    }

    if (smoke_mode) {
        QTimer::singleShot(0, &app, [&] {
            if (!shell.smoke_ready()) {
                std::cerr << "LINGTAI_LIB_UI_SMOKE_SHELL_NOT_READY" << std::endl;
                app.exit(98);
                return;
            }
            std::cout << "LINGTAI_NATIVE_SHELL_READY" << std::endl;
            std::cout
                << "LINGTAI_LIB_UI_FULL_TARGET_SMOKE_OK"
                << " qt=" << qVersion()
                << " class=Ui::RpWindow"
                << " window=lingtai_desktop_window"
                << " body=lingtai_desktop_body"
                << " sidebar=lingtai_desktop_sidebar"
                << " content=lingtai_desktop_content"
                << " separator=lingtai_roster_separator"
                << " roster=lingtai_agent_roster"
                << " startup=visible"
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
