#include "shell_host.h"

#include "base/event_filter.h"
#include "ui/widgets/rp_window.h"

#include <QtCore/QDir>
#include <QtCore/QPoint>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <utility>

namespace lingtai::desktop {
namespace {

std::filesystem::path default_fallback_python() {
    return std::filesystem::path(QDir::homePath().toStdU16String())
        / ".lingtai-tui" / "runtime" / "venv" / "bin" / "python";
}

std::filesystem::path default_tui_executable() {
    const auto configured = QStandardPaths::findExecutable(
        QStringLiteral("lingtai-tui"));
    if (configured.isEmpty()) {
        return {};
    }
    return std::filesystem::path(configured.toStdU16String());
}

} // namespace

ShellHost::ShellHost(RuntimeOptions runtime_options, QObject *parent)
: QObject(parent)
, runtime_options_(std::move(runtime_options))
, agent_start_fallback_python_(default_fallback_python())
, tui_executable_(default_tui_executable()) {
    static_cast<void>(spawn_shell());
}

ShellHost::~ShellHost() {
    // Drop owned shells without scheduling remove_shell / quit callbacks
    // against a half-destroyed host.
    shutting_down_ = true;
    shells_.clear();
}

NativeShell &ShellHost::primary() {
    return *shells_.front();
}

NativeShell &ShellHost::shell_at(std::size_t index) {
    return *shells_.at(index);
}

std::size_t ShellHost::shell_count() const {
    return shells_.size();
}

void ShellHost::configure_shell(NativeShell &shell) {
    shell.set_agent_start_fallback_python(agent_start_fallback_python_);
    if (!tui_executable_.empty()) {
        shell.set_tui_executable(tui_executable_);
    }
    shell.set_open_project_request_handler([this, &shell] {
        open_project_for(shell);
    });
    shell.set_open_project_in_new_window_request_handler([this, &shell] {
        open_project_in_new_window(shell);
    });
}

NativeShell *ShellHost::spawn_shell() {
    auto shell = std::make_unique<NativeShell>(runtime_options_);
    auto *raw = shell.get();
    configure_shell(*raw);
    shells_.push_back(std::move(shell));
    watch_shell(raw);
    return raw;
}

void ShellHost::watch_shell(NativeShell *shell) {
    // Closing a window must retire that shell without touching the others.
    // Defer erasure so Qt can finish the close before ~NativeShell runs.
    static_cast<void>(base::install_event_filter(
        &shell->window(),
        &shell->window(),
        [this, shell](not_null<QEvent *> event) {
            if (event->type() == QEvent::Close && !shutting_down_) {
                QTimer::singleShot(0, this, [this, shell] {
                    remove_shell(shell);
                });
            }
            return base::EventFilterResult::Continue;
        }));
}

void ShellHost::remove_shell(NativeShell *shell) {
    if (shutting_down_) {
        return;
    }
    const auto found = std::ranges::find_if(shells_,
        [shell](const auto &owned) { return owned.get() == shell; });
    if (found == shells_.end()) {
        return;
    }
    shells_.erase(found);
    if (shells_.empty()) {
        QApplication::quit();
    }
}

std::optional<std::filesystem::path> ShellHost::pick_project_directory(
        QWidget *parent) const {
    const auto selected = QFileDialog::getExistingDirectory(
        parent,
        QStringLiteral("Open LingTai Project"),
        QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) {
        return std::nullopt;
    }
    return std::filesystem::path(selected.toStdU16String());
}

void ShellHost::open_or_bootstrap(
        NativeShell &shell,
        const std::filesystem::path &directory) {
    std::error_code metadata_error;
    const auto metadata = directory / ".lingtai";
    if (std::filesystem::is_directory(metadata, metadata_error)) {
        static_cast<void>(shell.open_project(directory));
    } else {
        shell.request_new_project_at(directory);
    }
}

void ShellHost::open_project_for(NativeShell &requester) {
    const auto directory = pick_project_directory(&requester.window());
    if (!directory) {
        return;
    }
    open_or_bootstrap(requester, *directory);
}

void ShellHost::open_project_in_new_window(NativeShell &requester) {
    const auto directory = pick_project_directory(&requester.window());
    if (!directory) {
        return;
    }
    open_path_in_new_window(requester, *directory);
}

void ShellHost::open_path_in_new_window(
        NativeShell &requester,
        const std::filesystem::path &directory) {
    auto *created = spawn_shell();
    // Offset from the requester so the new window is not fully stacked.
    const auto origin =
        requester.window().geometry().topLeft() + QPoint(36, 36);
    created->window().move(origin);
    if (runtime_options_.offscreen_mode) {
        created->show_offscreen();
    } else {
        created->show();
        created->window().raise();
        created->window().activateWindow();
    }
    open_or_bootstrap(*created, directory);
}

} // namespace lingtai::desktop
