#include "agent_launch.h"
#include "posix_descriptor_primitives.h"

#include <QtCore/QByteArray>
#include <QtCore/QDir>
#include <QtCore/QIODeviceBase>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QLatin1StringView>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstddef>
#include <cerrno>
#include <fcntl.h>
#include <optional>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;
namespace posix = posix_internal;

// Bounds the one ordinary `init.json` read on the actual bytes read, the
// same shape as discovery's existing per-manifest bound. This is a plain
// config read, not a new descriptor-security subsystem: an oversized,
// unreadable, or malformed `init.json` simply means no configured runtime,
// exactly like a missing one, and the spawned kernel remains the validating
// authority for its own config.
constexpr auto kMaxInitBytes = std::size_t{1} << 20;

// The selected Agent's own top-level `init.json.venv_path`, accepted only
// when it is an absolute path: the current TUI's own plain-JSON reader
// resolves a relative `venv_path` against its own process's working
// directory rather than the target workdir, a known implementation
// divergence this slice deliberately does not reproduce.
std::optional<std::string> read_bounded(int fd, std::size_t cap) {
    std::string buffer(cap + 1, '\0');
    auto total = std::size_t{0};
    while (total < buffer.size()) {
        const auto count = ::read(fd, buffer.data() + total,
            buffer.size() - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            return std::nullopt;
        }
        if (count == 0) break;
        total += static_cast<std::size_t>(count);
    }
    if (total > cap) return std::nullopt;
    buffer.resize(total);
    return buffer;
}

std::optional<fs::path> configured_venv_path(int agent_fd) {
    auto file = posix::open_regular_file_component(agent_fd, "init.json");
    if (file.get() < 0) return std::nullopt;
    const auto content = read_bounded(file.get(), kMaxInitBytes);
    if (!content) return std::nullopt;

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(content->data(), static_cast<int>(content->size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const auto value =
        document.object().value(QLatin1StringView("venv_path"));
    if (!value.isString() || value.toString().isEmpty()) return std::nullopt;
    auto candidate = fs::path(value.toString().toStdString());
    if (!candidate.is_absolute()) return std::nullopt;
    return candidate;
}

fs::path venv_platform_python(const fs::path &venv_path) {
    return venv_path / "bin" / "python";
}

// Selected Agent `venv_path` platform Python when that exact file exists,
// otherwise the one Desktop fallback. Existence-only: a present but broken
// configured interpreter is still selected and simply fails to launch,
// never silently replaced.
fs::path select_runtime_python(int agent_fd,
        const fs::path &fallback_python) {
    if (const auto venv_path = configured_venv_path(agent_fd)) {
        std::error_code error;
        const auto candidate = venv_platform_python(*venv_path);
        if (fs::exists(candidate, error)) return candidate;
    }
    return fallback_python;
}

QString path_text(const fs::path &path) {
    const auto bytes = path.u8string();
    return QString::fromUtf8(
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<qsizetype>(bytes.size()));
}

QProcessEnvironment child_environment() {
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto separator = QDir::listSeparator();
    auto path_entries = environment.value(QStringLiteral("PATH"))
        .split(separator, Qt::KeepEmptyParts);
    const auto additions = QStringList{
        QStringLiteral("/usr/local/bin"),
        QDir::homePath() + QStringLiteral("/.local/bin"),
    };
    for (const auto &addition : additions) {
        if (!path_entries.contains(addition)) path_entries.push_back(addition);
    }
    environment.insert(QStringLiteral("PATH"), path_entries.join(separator));
    return environment;
}

} // namespace

AgentLaunchOutcome launch_agent(
        const ProjectAttachment &attachment,
        const fs::path &selected_directory_key,
        const fs::path &fallback_python) noexcept {
    try {
        if (!posix::safe_leaf(selected_directory_key)) return {};
        const auto target =
            attachment.resolve(fs::path(".lingtai") / selected_directory_key);
        if (!target) return {};

        const auto root = posix::open_root_directory(attachment.root());
        if (root.get() < 0) return {};
        const auto lingtai =
            posix::open_directory_component(root.get(), ".lingtai");
        if (lingtai.get() < 0) return {};
        const auto agent = posix::open_directory_component(
            lingtai.get(), selected_directory_key);
        if (agent.get() < 0) return {};

        const auto python =
            select_runtime_python(agent.get(), fallback_python);

        // Matches the current TUI launcher's own `os.MkdirAll(logPath)` plus
        // `os.OpenFile(.../agent.log, O_CREATE|O_WRONLY|O_APPEND)`: the
        // kernel process itself never creates `logs/agent.log`, so the
        // Desktop-shown failure path pointing there must create it.
        const auto logs =
            posix::open_directory_component(agent.get(), "logs", true);
        if (logs.get() < 0) return {};
        posix::FileDescriptor log(::openat(logs.get(), "agent.log",
            O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC,
            0644));
        if (log.get() < 0) return {};
        struct stat opened {};
        const auto regular = ::fstat(log.get(), &opened) == 0
            && S_ISREG(opened.st_mode);
        if (!regular) return {};
        const auto log_path_fs = target.path / "logs" / "agent.log";

        QProcess process;
        process.setProgram(path_text(python));
        process.setArguments(QStringList{
            QStringLiteral("-m"), QStringLiteral("lingtai"),
            QStringLiteral("run"), path_text(target.path)});
        process.setProcessEnvironment(child_environment());
        // Inherit this descriptor-relative O_NOFOLLOW append descriptor.
        // Reopening its pathname in QProcess would create a symlink swap race
        // after validation; dup2 keeps the already-verified inode instead.
        const auto log_fd = log.get();
        process.setChildProcessModifier([&process, log_fd] {
            if (::dup2(log_fd, STDOUT_FILENO) < 0
                || ::dup2(log_fd, STDERR_FILENO) < 0) {
                const auto error = errno;
                process.failChildProcessModifier("dup2 agent.log", error);
            }
        });
        qint64 pid = 0;
        if (!process.startDetached(&pid)) return {};
        return {
            .result = AgentLaunchResult::started,
            .pid = static_cast<std::int64_t>(pid),
            .log_path = std::move(log_path_fs),
        };
    } catch (...) {
        return {};
    }
}

AgentLaunchResult start_agent(const ProjectAttachment &attachment,
        const fs::path &selected_directory_key,
        const fs::path &fallback_python) noexcept {
    return launch_agent(
        attachment, selected_directory_key, fallback_python).result;
}

} // namespace lingtai::desktop
