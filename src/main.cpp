#include "native_shell.h"
#include "runtime_options.h"

#include <QtCore/QDir>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtGui/QFont>
#include <QtGui/QIcon>

#include <filesystem>
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

    lingtai::desktop::NativeShell shell(runtime_options);
    // The one Desktop fallback interpreter, used only when a selected
    // Agent's own `init.json.venv_path` is absent or its platform Python
    // does not exist: the same managed LingTai runtime location the current
    // local development setup already uses.
    shell.set_agent_start_fallback_python(
        std::filesystem::path(QDir::homePath().toStdU16String())
            / ".lingtai-tui" / "runtime" / "venv" / "bin" / "python");
    shell.set_open_project_request_handler([&shell] {
        const auto selected = QFileDialog::getExistingDirectory(
            QApplication::activeWindow(),
            QStringLiteral("Open LingTai Project"),
            QDir::homePath(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (selected.isEmpty()) {
            return;
        }
        const auto directory = std::filesystem::path(selected.toStdU16String());
        std::error_code metadata_error;
        const auto metadata = directory / ".lingtai";
        if (std::filesystem::is_directory(metadata, metadata_error)) {
            static_cast<void>(shell.open_project(directory));
        } else {
            shell.request_new_project_at(directory);
        }
    });
    // The shipped default configured TUI executable for the explicit New
    // Project flow: the `lingtai-tui` found on PATH, resolved once at
    // startup. This is a path to a subprocess, never a shell command string.
    const auto configured_tui = QStandardPaths::findExecutable(
        QStringLiteral("lingtai-tui"));
    if (!configured_tui.isEmpty()) {
        shell.set_tui_executable(
            std::filesystem::path(configured_tui.toStdU16String()));
    }
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
