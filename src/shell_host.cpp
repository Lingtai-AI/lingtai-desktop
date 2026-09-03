#include "shell_host.h"

#include "desktop_status_item.h"

#include "base/event_filter.h"
#include "ui/widgets/rp_window.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QPoint>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lingtai::desktop {
namespace {

std::filesystem::path default_fallback_python() {
    return std::filesystem::path(QDir::homePath().toStdU16String())
        / ".lingtai-tui" / "runtime" / "venv" / "bin" / "python";
}

} // namespace

ShellHost::ShellHost(RuntimeOptions runtime_options, QObject *parent)
: QObject(parent)
, runtime_options_(std::move(runtime_options))
, agent_start_fallback_python_(default_fallback_python()) {
    static_cast<void>(spawn_shell());
    status_item_ = new DesktopStatusItem(
        [this] { show_lingtai(); },
        [] { QCoreApplication::quit(); },
        this);
    if (!runtime_options_.offscreen_mode) {
        status_item_->show();
    }
    refresh_unread_presentations();
}

ShellHost::~ShellHost() {
    // Drop owned shells without scheduling remove_shell / quit callbacks
    // against a half-destroyed host.
    shutting_down_ = true;
    active_shell_ = nullptr;
    most_recent_active_shell_ = nullptr;
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

std::size_t ShellHost::unread_total() const noexcept {
    return unread_total_;
}

std::size_t ShellHost::open_project_count() const noexcept {
    return open_project_count_;
}

void ShellHost::configure_shell(NativeShell &shell) {
    shell.set_agent_start_fallback_python(agent_start_fallback_python_);
    shell.set_open_project_request_handler([this, &shell] {
        open_project_for(shell);
    });
    shell.set_open_project_in_new_window_request_handler([this, &shell] {
        open_project_in_new_window(shell);
    });
    shell.set_unread_view_eligibility([this, &shell] {
        return active_shell_ == &shell
            && shell.window().isVisible()
            && !shell.window().isMinimized();
    });
    shell.set_unread_presentation_changed_handler([this] {
        if (!shutting_down_) {
            refresh_unread_presentations();
        }
    });
}

NativeShell *ShellHost::spawn_shell() {
    auto shell = std::make_unique<NativeShell>(
        unread_session_, runtime_options_);
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
            if (event->type() == QEvent::WindowActivate && !shutting_down_) {
                active_shell_ = shell;
                most_recent_active_shell_ = shell;
                shell->refresh_unread_view_state();
                refresh_unread_presentations();
            }
            if ((event->type() == QEvent::WindowDeactivate
                    || event->type() == QEvent::Show
                    || event->type() == QEvent::Hide
                    || event->type() == QEvent::WindowStateChange)
                && !shutting_down_
                && !closing_shells_.contains(shell)) {
                if ((event->type() == QEvent::WindowDeactivate
                        || event->type() == QEvent::Hide
                        || (event->type() == QEvent::WindowStateChange
                            && shell->window().isMinimized()))
                    && active_shell_ == shell) {
                    active_shell_ = nullptr;
                }
                shell->refresh_unread_view_state();
                refresh_unread_presentations();
            }
            if (event->type() == QEvent::Close && !shutting_down_
                && closing_shells_.insert(shell).second) {
                // Exclude the closing window synchronously; retain deferred
                // destruction so Qt can finish dispatching this close event.
                refresh_unread_presentations();
                QTimer::singleShot(0, this, [this, shell] {
                    remove_shell(shell);
                });
            }
            return base::EventFilterResult::Continue;
        }));
}

void ShellHost::show_lingtai() {
    if (shells_.empty()) {
        return;
    }
    const auto recent = std::ranges::find_if(shells_, [this](const auto &owned) {
        return owned.get() == most_recent_active_shell_;
    });
    auto *selected = recent != shells_.end()
        ? recent->get()
        : shells_.front().get();
    auto &shell = *selected;
    auto &window = shell.window();
    if (window.isMinimized()) {
        window.setWindowState(
            window.windowState() & ~Qt::WindowMinimized);
    }
    if (window.isHidden()) {
        if (runtime_options_.offscreen_mode) {
            shell.show_offscreen();
        } else {
            shell.show();
        }
    }
    window.raise();
    window.activateWindow();
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
    if (most_recent_active_shell_ == shell) {
        most_recent_active_shell_ = nullptr;
    }
    if (active_shell_ == shell) {
        active_shell_ = nullptr;
    }
    shells_.erase(found);
    closing_shells_.erase(shell);
    refresh_unread_presentations();
    if (shells_.empty()) {
        QApplication::quit();
    }
}

void ShellHost::refresh_unread_presentations() {
    auto project_agents = std::unordered_map<std::filesystem::path,
        std::unordered_set<std::string>>{};
    for (const auto &owned : shells_) {
        auto *shell = owned.get();
        if (closing_shells_.contains(shell)) {
            continue;
        }
        const auto project_root = shell->active_project_root();
        if (!project_root) {
            continue;
        }
        auto &agent_keys = project_agents[*project_root];
        const auto shell_keys = shell->valid_unread_agent_keys();
        agent_keys.insert(shell_keys.begin(), shell_keys.end());
    }

    auto requests = std::vector<OpenProjectUnreadRequest>{};
    requests.reserve(project_agents.size());
    for (const auto &[project_root, agent_keys] : project_agents) {
        requests.push_back({
            .canonical_project_root = project_root,
            .valid_agent_keys = std::vector<std::string>(
                agent_keys.begin(), agent_keys.end()),
        });
    }
    for (const auto &owned : shells_) {
        auto *shell = owned.get();
        if (closing_shells_.contains(shell)) {
            continue;
        }
        const auto project_root = shell->active_project_root();
        if (!project_root) {
            shell->apply_unread_snapshot({});
            continue;
        }
        const auto found = project_agents.find(*project_root);
        if (found == project_agents.end()) {
            shell->apply_unread_snapshot({});
            continue;
        }
        const auto valid_agent_keys = std::vector<std::string>(
            found->second.begin(), found->second.end());
        shell->apply_unread_snapshot(
            unread_session_.snapshot(*project_root, valid_agent_keys));
    }
    unread_total_ = unread_session_.total_for(requests);
    open_project_count_ = requests.size();
    if (status_item_) {
        status_item_->set_unread_count(unread_total_, open_project_count_);
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
