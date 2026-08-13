#include "native_shell.h"

#include <QtCore/QDir>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>

#include <filesystem>
#include <iostream>
#include <string_view>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const auto smoke_mode = argc == 2
        && std::string_view(argv[1]) == "--smoke";
    const auto offscreen_mode = smoke_mode || (argc == 2
        && std::string_view(argv[1]) == "--offscreen");

    lingtai::desktop::NativeShell shell;
    shell.set_open_project_request_handler([&shell] {
        const auto selected = QFileDialog::getExistingDirectory(
            QApplication::activeWindow(),
            QStringLiteral("Open LingTai Project"),
            QDir::homePath(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (selected.isEmpty()) {
            return;
        }
        static_cast<void>(shell.open_project(
            std::filesystem::path(selected.toStdU16String())));
    });
    if (offscreen_mode) {
        shell.show_offscreen();
    } else {
        shell.show();
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
