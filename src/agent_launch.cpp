#include "agent_launch.h"

#include <QtCore/QByteArray>
#include <QtCore/QIODeviceBase>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QLatin1StringView>
#include <QtCore/QProcess>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstddef>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;

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
std::optional<fs::path> configured_venv_path(const fs::path &agent_directory) {
    auto stream = std::ifstream(agent_directory / "init.json", std::ios::binary);
    if (!stream.is_open()) return std::nullopt;
    auto buffer = std::string(kMaxInitBytes + 1, '\0');
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto read_count = stream.gcount();
    if (read_count < 0
        || static_cast<std::size_t>(read_count) > kMaxInitBytes) {
        return std::nullopt;
    }
    buffer.resize(static_cast<std::size_t>(read_count));

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(buffer.data(), static_cast<int>(buffer.size())), &error);
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
fs::path select_runtime_python(
        const fs::path &agent_directory, const fs::path &fallback_python) {
    if (const auto venv_path = configured_venv_path(agent_directory)) {
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

} // namespace

AgentLaunchResult start_agent(
        const ProjectAttachment &attachment,
        const fs::path &selected_directory_key,
        const fs::path &fallback_python) noexcept {
    try {
        const auto target =
            attachment.resolve(fs::path(".lingtai") / selected_directory_key);
        if (!target) return AgentLaunchResult::refused;

        const auto python =
            select_runtime_python(target.path, fallback_python);

        // Matches the current TUI launcher's own `os.MkdirAll(logPath)` plus
        // `os.OpenFile(.../agent.log, O_CREATE|O_WRONLY|O_APPEND)`: the
        // kernel process itself never creates `logs/agent.log`, so the
        // Desktop-shown failure path pointing there must create it.
        const auto logs_dir = target.path / "logs";
        std::error_code error;
        fs::create_directory(logs_dir, error);
        if (error) return AgentLaunchResult::refused;
        const auto log_path = path_text(logs_dir / "agent.log");

        QProcess process;
        process.setProgram(path_text(python));
        process.setArguments(QStringList{
            QStringLiteral("-m"), QStringLiteral("lingtai"),
            QStringLiteral("run"), path_text(target.path)});
        process.setStandardOutputFile(log_path, QIODeviceBase::Append);
        process.setStandardErrorFile(log_path, QIODeviceBase::Append);
        return process.startDetached()
            ? AgentLaunchResult::started
            : AgentLaunchResult::refused;
    } catch (...) {
        return AgentLaunchResult::refused;
    }
}

} // namespace lingtai::desktop
