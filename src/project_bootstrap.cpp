#include "project_bootstrap.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QLatin1StringView>
#include <QtCore/QString>

#include <utility>

namespace lingtai::desktop {
namespace {

QString path_text(const std::filesystem::path &path) {
    const auto bytes = path.u8string();
    return QString::fromUtf8(
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<qsizetype>(bytes.size()));
}

// The current TUI headless error contract: one JSON object on stderr with
// `code` and `error` strings. The real TUI can print plain `warning:` lines
// (e.g. `warning: recipe copy: ...`) before `WriteError` emits the JSON
// object, so the object is parsed from the final/current nonempty JSON block
// rather than from the whole stream. Anything else is ignored (the caller
// falls back to a concise generic message).
std::pair<std::string, std::string> structured_error(
        const QByteArray &stderr_bytes) {
    const auto lines = stderr_bytes.split('\n');
    for (qsizetype index = lines.size() - 1; index >= 0; --index) {
        if (lines[index].trimmed().isEmpty()) continue;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(
            lines.mid(index).join('\n'), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            continue;
        }
        const auto object = document.object();
        return {
            object.value(QLatin1StringView("code")).toString().toStdString(),
            object.value(QLatin1StringView("error")).toString().toStdString(),
        };
    }
    return {};
}

// Parses only the current `presets` JSON contract: top-level `presets` array,
// each usable entry requiring a nonempty `name`. Every other entry field is a
// presentation fact kept verbatim. A malformed body, a missing/non-array
// `presets`, an empty list, or a nonzero exit all fail closed.
PresetDiscoveryResult parse_presets_result(
        int exit_code,
        const QByteArray &stdout_bytes,
        const QByteArray &stderr_bytes) {
    if (exit_code != 0) {
        auto [code, error] = structured_error(stderr_bytes);
        return {PresetDiscoveryKind::process_failed, {}, std::move(code),
            std::move(error)};
    }
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(stdout_bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return {PresetDiscoveryKind::malformed, {}, {}, {}};
    }
    const auto presets = document.object().value(QLatin1StringView("presets"));
    if (!presets.isArray()) {
        return {PresetDiscoveryKind::malformed, {}, {}, {}};
    }
    auto entries = std::vector<PresetEntry>();
    for (const auto &value : presets.toArray()) {
        if (!value.isObject()) continue;
        const auto object = value.toObject();
        const auto name = object.value(QLatin1StringView("name")).toString();
        if (name.isEmpty()) continue; // unusable entry: ignore, never present it
        entries.push_back(PresetEntry{
            name.toStdString(),
            object.value(QLatin1StringView("description")).toString().toStdString(),
            object.value(QLatin1StringView("tier")).toString().toStdString(),
            object.value(QLatin1StringView("source")).toString().toStdString(),
            object.value(QLatin1StringView("path")).toString().toStdString(),
        });
    }
    if (entries.empty()) {
        return {PresetDiscoveryKind::empty, {}, {}, {}};
    }
    return {PresetDiscoveryKind::succeeded, std::move(entries), {}, {}};
}

// Success requires exit 0 plus valid stdout JSON with `status == "launched"`
// and a nonempty `project_dir`. The PID is deliberately ignored as an
// ownership/liveness authority. Any other shape fails closed.
SpawnOutcome parse_spawn_outcome(
        int exit_code,
        const QByteArray &stdout_bytes,
        const QByteArray &stderr_bytes) {
    if (exit_code != 0) {
        auto [code, error] = structured_error(stderr_bytes);
        return {SpawnOutcomeKind::process_failed, {}, std::move(code),
            std::move(error)};
    }
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(stdout_bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return {SpawnOutcomeKind::malformed, {}, {}, {}};
    }
    const auto object = document.object();
    if (object.value(QLatin1StringView("status")).toString()
        != QLatin1StringView("launched")) {
        return {SpawnOutcomeKind::malformed, {}, {}, {}};
    }
    const auto project_dir =
        object.value(QLatin1StringView("project_dir")).toString();
    if (project_dir.isEmpty()) {
        return {SpawnOutcomeKind::malformed, {}, {}, {}};
    }
    return {SpawnOutcomeKind::launched,
        std::filesystem::path(project_dir.toStdString()), {}, {}};
}

} // namespace

ProjectBootstrapRunner::ProjectBootstrapRunner() {
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(
        &process_, &QProcess::errorOccurred, &process_,
        [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || !pending_) return;
            // The executable could not be started at all; `finished` will not
            // fire, so fail closed exactly once so the UI never hangs pending.
            const auto spawn = spawn_mode_;
            pending_ = false;
            if (spawn) {
                spawn_done_(SpawnOutcome{SpawnOutcomeKind::process_failed, {},
                    {}, "the configured TUI executable could not be started"});
            } else {
                preset_done_(PresetDiscoveryResult{
                    PresetDiscoveryKind::process_failed, {},
                    "failed_to_start",
                    "the configured TUI executable could not be started"});
            }
        });
    QObject::connect(
        &process_, &QProcess::finished, &process_,
        [this](int exit_code, QProcess::ExitStatus exit_status) {
            if (!pending_) return;
            const auto spawn = spawn_mode_;
            pending_ = false;
            const auto stdout_bytes = process_.readAllStandardOutput();
            const auto stderr_bytes = process_.readAllStandardError();
            const auto effective_exit = exit_status == QProcess::NormalExit
                ? exit_code
                : 1;
            if (spawn) {
                spawn_done_(parse_spawn_outcome(
                    effective_exit, stdout_bytes, stderr_bytes));
            } else {
                preset_done_(parse_presets_result(
                    effective_exit, stdout_bytes, stderr_bytes));
            }
        });
}

ProjectBootstrapRunner::~ProjectBootstrapRunner() = default;

bool ProjectBootstrapRunner::is_pending() const noexcept {
    return pending_;
}

void ProjectBootstrapRunner::run_presets(
        const std::filesystem::path &executable,
        PresetDone done) {
    if (pending_) return;
    spawn_mode_ = false;
    preset_done_ = std::move(done);
    spawn_done_ = {};
    start(path_text(executable), {QStringLiteral("presets")});
}

void ProjectBootstrapRunner::run_spawn(
        const std::filesystem::path &executable,
        const std::filesystem::path &destination,
        const std::string &preset_name,
        SpawnDone done) {
    if (pending_) return;
    spawn_mode_ = true;
    preset_done_ = {};
    spawn_done_ = std::move(done);
    start(path_text(executable), {
        QStringLiteral("spawn"),
        path_text(destination),
        QStringLiteral("--preset"),
        QString::fromStdString(preset_name),
    });
}

void ProjectBootstrapRunner::start(
        const QString &program,
        const QStringList &arguments) {
    process_.setProgram(program);
    process_.setArguments(arguments);
    pending_ = true;
    process_.start();
}

} // namespace lingtai::desktop
