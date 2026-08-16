#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QLatin1StringView>
#include <QtCore/QProcess>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <filesystem>
#include <functional>
#include <string>
#include <utility>

namespace lingtai::desktop {

// The narrow outcome of one headless control command: only `succeeded` or
// `failed`. Success requires a normal exit code 0 plus one stdout JSON object
// whose `command` and `agent` string fields match the requested action and
// Agent exactly and whose `status` is `"signaled"`. Every malformed, nonzero,
// or error outcome is `failed`; there is no finer taxonomy.
enum class AgentCommandResultKind { succeeded, failed };

// The one result delivered through the per-call callback. `stdout_bytes` and
// `stderr_bytes` preserve the raw emitted bytes independently on every
// outcome; the bound fields are the exact project root, Agent key, and
// selection/project generation captured when the command was accepted. The
// generation is a NativeShell epoch and never enters the argv.
struct AgentCommandResult {
    AgentCommandResultKind kind = AgentCommandResultKind::failed;
    QByteArray stdout_bytes;
    QByteArray stderr_bytes;
    std::string bound_project_root;
    std::string bound_agent_key;
    std::string bound_generation;
};

// The one async owner of the headless control command `<exe> control
// --project <project-root> --agent <agent-key>
// <sleep|suspend|cpr|clear|refresh> [arg]`. Each accepted `run` starts
// exactly one nonblocking QProcess with the exact separate argv -- never a
// shell string and never a joined command line -- and reports exactly one
// result through the one per-call callback, bound to the exact start-time
// project root, Agent key, and generation. Only one command may be pending:
// a second `run` while pending is rejected without invoking its callback.
// It tracks no PID, lock, retry, cancellation, or lifecycle state beyond the
// one pending bit.
class AgentCommandRunner final {
public:
    using Done = std::function<void(AgentCommandResult)>;

    AgentCommandRunner() {
        process_.setProcessChannelMode(QProcess::SeparateChannels);
        QObject::connect(
            &process_, &QProcess::errorOccurred, &process_,
            [this](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || !pending_) return;
                finish(AgentCommandResultKind::failed, {}, {});
            });
        QObject::connect(
            &process_, &QProcess::finished, &process_,
            [this](int exit_code, QProcess::ExitStatus exit_status) {
                if (!pending_) return;
                const auto stdout_bytes = process_.readAllStandardOutput();
                const auto stderr_bytes = process_.readAllStandardError();
                const auto effective_exit = exit_status == QProcess::NormalExit
                    ? exit_code
                    : 1;
                finish(parse(effective_exit, stdout_bytes), stdout_bytes,
                    stderr_bytes);
            });
    }

    AgentCommandRunner(const AgentCommandRunner &) = delete;
    AgentCommandRunner &operator=(const AgentCommandRunner &) = delete;

    [[nodiscard]] bool is_pending() const noexcept { return pending_; }

    // Accepted only when no command is pending. Starts one nonblocking
    // QProcess with the exact separate argv and captures the project root,
    // Agent key, action, and generation at start. The generation never enters
    // the argv; the optional arg joins the argv only when nonempty.
    bool run(const std::filesystem::path &executable,
             const std::string &project_root, const std::string &agent_key,
             const std::string &action, const std::string &optional_arg,
             const std::string &generation, Done done) {
        if (pending_) return false;
        bound_project_root_ = project_root;
        bound_agent_key_ = agent_key;
        bound_action_ = action;
        bound_generation_ = generation;
        done_ = std::move(done);
        auto arguments = QStringList{
            QStringLiteral("control"),
            QStringLiteral("--project"),
            QString::fromStdString(project_root),
            QStringLiteral("--agent"),
            QString::fromStdString(agent_key),
            QString::fromStdString(action),
        };
        if (!optional_arg.empty()) {
            arguments.push_back(QString::fromStdString(optional_arg));
        }
        process_.setProgram(path_text(executable));
        process_.setArguments(arguments);
        pending_ = true;
        process_.start();
        return true;
    }

private:
    static QString path_text(const std::filesystem::path &path) {
        const auto bytes = path.u8string();
        return QString::fromUtf8(
            reinterpret_cast<const char *>(bytes.data()),
            static_cast<qsizetype>(bytes.size()));
    }

    AgentCommandResultKind parse(int exit_code,
                                 const QByteArray &stdout_bytes) {
        if (exit_code != 0) return AgentCommandResultKind::failed;
        QJsonParseError parse_error;
        const auto document = QJsonDocument::fromJson(stdout_bytes, &parse_error);
        if (parse_error.error != QJsonParseError::NoError
            || !document.isObject()) {
            return AgentCommandResultKind::failed;
        }
        const auto object = document.object();
        if (object.value(QLatin1StringView("command")).toString().toStdString()
            != bound_action_) {
            return AgentCommandResultKind::failed;
        }
        if (object.value(QLatin1StringView("agent")).toString().toStdString()
            != bound_agent_key_) {
            return AgentCommandResultKind::failed;
        }
        if (object.value(QLatin1StringView("status")).toString()
            != QLatin1StringView("signaled")) {
            return AgentCommandResultKind::failed;
        }
        return AgentCommandResultKind::succeeded;
    }

    // The one terminal delivery: clears pending before the callback so a
    // second QProcess signal can never double-call. The current callback is
    // moved to a local and the member bindings are cleared before invocation,
    // so a reentrant run owns a new untouched member callback/bindings.
    void finish(AgentCommandResultKind kind, QByteArray stdout_bytes,
                QByteArray stderr_bytes) {
        if (!pending_) return;
        pending_ = false;
        Done callback = std::move(done_);
        done_ = {};
        AgentCommandResult result{kind, std::move(stdout_bytes),
            std::move(stderr_bytes), std::move(bound_project_root_),
            std::move(bound_agent_key_), std::move(bound_generation_)};
        bound_project_root_ = {};
        bound_agent_key_ = {};
        bound_action_ = {};
        bound_generation_ = {};
        callback(std::move(result));
    }

    QProcess process_;
    bool pending_ = false;
    Done done_;
    std::string bound_project_root_;
    std::string bound_agent_key_;
    std::string bound_action_;
    std::string bound_generation_;
};

} // namespace lingtai::desktop
