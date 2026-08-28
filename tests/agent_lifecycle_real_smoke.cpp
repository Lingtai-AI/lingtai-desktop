#include "agent_lifecycle.h"
#include "agent_process.h"
#include "agent_setup_store.h"
#include "agent_signal.h"
#include "project_attachment.h"
#include "project_creation.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using lingtai::desktop::AgentIdentityFacts;
using lingtai::desktop::AgentLifecycleCommand;
using lingtai::desktop::AgentLifecycleController;
using lingtai::desktop::AgentLifecycleOutcomeKind;
using lingtai::desktop::AgentLifecycleRequest;
using lingtai::desktop::AgentLifecycleResult;
using lingtai::desktop::AgentLifecycleStartResult;
using lingtai::desktop::AgentManifestKind;
using lingtai::desktop::AgentPresenceKind;
using lingtai::desktop::AgentRole;
using lingtai::desktop::AgentRow;
using lingtai::desktop::AgentSignalKind;
using lingtai::desktop::AgentSnapshot;
using lingtai::desktop::AgentTerminationSignal;
using lingtai::desktop::ProjectAttachment;

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << message << '\n';
    if (!condition) ++failures;
}

void write_file(const fs::path &path, const std::string &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << bytes;
    stream.close();
    if (!stream) throw std::runtime_error("could not write " + path.string());
}

std::string read_file(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)), {});
}

bool wait_until(const std::function<bool()> &predicate,
        std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (predicate()) return true;
        std::this_thread::sleep_for(25ms);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return predicate();
}

AgentRow agent_row(const std::string &key, AgentRole role,
        AgentPresenceKind presence, const std::string &state) {
    AgentRow result;
    result.directory_key = key;
    result.directory_path = fs::path(".lingtai") / key;
    result.manifest_kind = AgentManifestKind::valid;
    result.role = role;
    result.presence = presence;
    result.identity = AgentIdentityFacts{};
    result.identity->state = state;
    return result;
}

AgentSnapshot snapshot(std::vector<AgentRow> rows) {
    AgentSnapshot result;
    result.scan = lingtai::desktop::AgentScanState::complete;
    result.items = std::move(rows);
    return result;
}

bool successful(const AgentLifecycleResult &result,
        AgentLifecycleOutcomeKind outcome) {
    return result.targets.size() == 1
        && result.targets.front().outcome == outcome;
}

std::optional<AgentLifecycleResult> run_command(
        const ProjectAttachment &attachment, const fs::path &runtime_python,
        AgentSnapshot current, AgentLifecycleCommand command,
        std::string argument = {},
        std::optional<fs::path> selected = fs::path("Main"),
        std::chrono::seconds timeout = 90s) {
    AgentLifecycleController controller;
    std::optional<AgentLifecycleResult> result;
    AgentLifecycleRequest request{
        .attachment = attachment,
        .snapshot = std::move(current),
        .selected_agent_key = std::move(selected),
        .command = command,
        .argument = std::move(argument),
        .fallback_python = runtime_python,
        .generation = "real-smoke",
    };
    const auto started = controller.run(std::move(request),
        [&](AgentLifecycleResult value) { result = std::move(value); });
    if (started != AgentLifecycleStartResult::started) {
        std::cerr << "command refused before starting: "
                  << static_cast<int>(started) << '\n';
        return std::nullopt;
    }
    if (!wait_until([&] { return result.has_value(); }, timeout)) {
        controller.cancel();
        return std::nullopt;
    }
    std::cout << "RESULT: "
              << lingtai::desktop::agent_lifecycle_result_text(*result) << '\n';
    return result;
}

QJsonObject preset(const QString &name) {
    return {
        {"name", name},
        {"description", QJsonObject{{"summary", name + " smoke preset"}}},
        {"manifest", QJsonObject{
            {"llm", QJsonObject{
                {"provider", "openai"},
                {"model", "gpt-4o"},
                {"api_key", "isolated-smoke-never-used"},
            }},
            {"capabilities", QJsonObject{}},
        }},
    };
}

void write_json(const fs::path &path, const QJsonObject &object) {
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    write_file(path, std::string(bytes.constData(),
        static_cast<std::size_t>(bytes.size())));
}

void write_init(const fs::path &agent, const fs::path &runtime_python,
        const fs::path &default_preset, const fs::path &alternate_preset,
        const fs::path &active, const std::optional<fs::path> &venv_override = {}) {
    const auto venv = venv_override.value_or(
        runtime_python.parent_path().parent_path());
    QJsonArray disabled;
    for (const auto *name : {"knowledge", "skills", "shell", "avatar",
             "daemon", "mcp", "notification", "plugin", "task_card",
             "file", "vision"}) {
        disabled.append(name);
    }
    const auto default_text = QString::fromStdString(default_preset.string());
    const auto alternate_text = QString::fromStdString(alternate_preset.string());
    write_json(agent / "init.json", {
        {"venv_path", QString::fromStdString(venv.string())},
        {"manifest", QJsonObject{
            {"agent_name", QString::fromStdString(agent.filename().string())},
            {"language", "en"},
            {"llm", QJsonObject{
                {"provider", "openai"},
                {"model", "gpt-4o"},
                {"api_key", "isolated-smoke-never-used"},
            }},
            {"capabilities", QJsonObject{}},
            {"disable", disabled},
            {"admin", QJsonObject{}},
            {"streaming", false},
            {"preset", QJsonObject{
                {"active", QString::fromStdString(active.string())},
                {"default", default_text},
                {"allowed", QJsonArray{default_text, alternate_text}},
            }},
        }},
        {"covenant", "isolated lifecycle smoke"},
        {"pad", ""},
        {"lingtai", "isolated lifecycle smoke agent"},
    });
}

std::optional<std::string> active_preset(const fs::path &agent) {
    const auto document = QJsonDocument::fromJson(
        QByteArray::fromStdString(read_file(agent / "init.json")));
    if (!document.isObject()) return std::nullopt;
    const auto value = document.object().value("manifest").toObject()
        .value("preset").toObject().value("active");
    return value.isString() ? std::optional(value.toString().toStdString())
                            : std::nullopt;
}

bool contains_clear_event(const fs::path &agent) {
    const auto path = agent / "logs/events.jsonl";
    if (!fs::is_regular_file(path)) return false;
    const auto bytes = read_file(path);
    return bytes.find("\"clear_received\"") != std::string::npos
        && bytes.find("\"desktop\"") != std::string::npos;
}

bool tui_is_discoverable() {
    const auto *raw = std::getenv("PATH");
    if (!raw) return false;
    auto paths = std::string(raw);
    auto begin = std::size_t{0};
    for (;;) {
        const auto end = paths.find(':', begin);
        const auto component = paths.substr(begin,
            end == std::string::npos ? std::string::npos : end - begin);
        if (!component.empty()
            && ::access((fs::path(component) / "lingtai-tui").c_str(), X_OK) == 0) {
            return true;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return false;
}

void stop_agent(const ProjectAttachment &attachment, const fs::path &key) {
    static_cast<void>(lingtai::desktop::write_agent_signal(
        attachment, key, AgentSignalKind::suspend));
    const auto dir = attachment.root() / ".lingtai" / key;
    if (wait_until([&] {
            const auto observed =
                lingtai::desktop::observe_exact_agent_processes(dir);
            return observed.available && observed.pids.empty();
        }, 15s)) {
        return;
    }
    for (const auto signal : {AgentTerminationSignal::terminate,
             AgentTerminationSignal::kill}) {
        const auto observed = lingtai::desktop::observe_exact_agent_processes(dir);
        for (const auto pid : observed.pids) {
            static_cast<void>(lingtai::desktop::signal_exact_agent_process(
                dir, pid, signal));
        }
        if (wait_until([&] {
                const auto current =
                    lingtai::desktop::observe_exact_agent_processes(dir);
                return current.available && current.pids.empty();
            }, 3s)) {
            return;
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 3) {
        std::cerr << "usage: agent_lifecycle_real_smoke <empty-project-root> "
                     "<absolute-runtime-python>\n";
        return 2;
    }
    try {
        const auto project = fs::absolute(argv[1]);
        const auto runtime_python = fs::path(argv[2]);
        const auto *home_raw = std::getenv("HOME");
        if (!project.is_absolute() || !runtime_python.is_absolute()
            || !fs::is_regular_file(runtime_python) || !home_raw
            || !fs::path(home_raw).is_absolute() || tui_is_discoverable()) {
            std::cerr << "refused: smoke requires absolute isolated paths and "
                         "a PATH with no executable lingtai-tui\n";
            return 2;
        }
        fs::create_directories(project);
        const auto isolated_home = fs::path(home_raw);
        const auto global = isolated_home / ".lingtai-tui";
        const auto default_path = global / "presets/saved/default.json";
        const auto alternate_path = global / "presets/saved/alternate.json";
        const auto env_path = global / ".env";
        const auto covenant_path = global / "covenant/en/covenant.md";
        write_json(default_path, preset("default"));
        write_json(alternate_path, preset("alternate"));
        write_file(env_path, "ISOLATED_SMOKE=1\n");
        write_file(covenant_path, "isolated lifecycle smoke\n");

        lingtai::desktop::AgentSetupDraft setup{
            .agent_name = "Main",
            .language = "en",
            .context_limit = 300000,
            .max_rpm = 60,
            .max_aed_attempts = 5,
            .karma = true,
            .nirvana = false,
            .soul_flow_enabled = false,
            .covenant_file = covenant_path.string(),
        };
        const auto created = lingtai::desktop::create_project({
            .destination = project,
            .preset_path = default_path,
            .allowed_preset_paths = {default_path, alternate_path},
            .runtime_python = runtime_python,
            .env_file = env_path,
            .covenant_file = covenant_path,
            .agent_name = "Main",
            .agent_directory = "Main",
            .setup = setup,
            .comment = "isolated lifecycle smoke agent",
        });
        expect(created.created,
            "Desktop creation transaction commits the fake-HOME project");
        if (!created.created) {
            std::cerr << "creation failed: " << created.detail << '\n';
            return 1;
        }
        const auto main_agent = project / ".lingtai/Main";
        expect(fs::is_regular_file(main_agent / "init.json")
                && fs::is_regular_file(main_agent / ".agent.json")
                && fs::is_directory(project / ".lingtai/human/mailbox/inbox")
                && fs::is_directory(project / ".lingtai/.library_shared")
                && !fs::exists(project / ".lingtai/.tui-asset"),
            "created project has the exact Desktop/kernel shape and no TUI state");

        const auto attached = lingtai::desktop::attach_project(project);
        if (!attached) {
            std::cerr << "could not attach isolated project\n";
            return 2;
        }
        const auto attachment = *attached.attachment;

        auto result = run_command(attachment, runtime_python,
            snapshot({agent_row("Main", AgentRole::main,
                AgentPresenceKind::missing, "suspended")}),
            AgentLifecycleCommand::cpr);
        expect(result && successful(*result, AgentLifecycleOutcomeKind::applied),
            "Desktop CPR launches the real kernel Agent and observes heartbeat");
        auto first = lingtai::desktop::observe_exact_agent_processes(main_agent);
        expect(first.available && first.pids.size() == 1,
            "the launched process has the exact production argv");
        for (const auto pid : first.pids) {
            std::cout << "EXACT_LAUNCHED_PID: " << pid << '\n';
        }
        const auto launched_identity = QJsonDocument::fromJson(
            QByteArray::fromStdString(read_file(main_agent / ".agent.json")))
            .object();
        expect(!launched_identity.value("agent_id").toString().isEmpty()
                && fs::is_regular_file(
                    main_agent / "system/manifest.resolved.json"),
            "the real kernel publishes durable identity and resolved system state");
        const lingtai::desktop::AgentSetupStore setup_store(attachment);
        const auto loaded_setup = setup_store.load("Main");
        expect(static_cast<bool>(loaded_setup),
            "the real kernel identity and Desktop-created init load as setup state");
        if (loaded_setup) {
            const auto unchanged = setup_store.save(
                *loaded_setup.state, loaded_setup.state->draft);
            expect(unchanged.status
                    == lingtai::desktop::AgentSetupSaveStatus::no_change,
                "unchanged setup is byte-preserving after real launch");
        }

        result = run_command(attachment, runtime_python,
            snapshot({agent_row("Main", AgentRole::main,
                AgentPresenceKind::alive, "idle")}),
            AgentLifecycleCommand::sleep);
        expect(result && successful(*result, AgentLifecycleOutcomeKind::applied),
            "Desktop sleep observes real-kernel marker consumption");
        auto sleeping = lingtai::desktop::observe_exact_agent_processes(main_agent);
        expect(sleeping.available && sleeping.pids == first.pids,
            "sleep leaves the Agent process/listeners alive");

        result = run_command(attachment, runtime_python,
            snapshot({agent_row("Main", AgentRole::main,
                AgentPresenceKind::alive, "asleep")}),
            AgentLifecycleCommand::suspend);
        expect(result && successful(*result, AgentLifecycleOutcomeKind::applied),
            "Desktop suspend observes real process and heartbeat shutdown");

        result = run_command(attachment, runtime_python,
            snapshot({agent_row("Main", AgentRole::main,
                AgentPresenceKind::stale, "suspended")}),
            AgentLifecycleCommand::cpr);
        expect(result && successful(*result, AgentLifecycleOutcomeKind::applied),
            "Desktop CPR revives the suspended real Agent");
        auto revived = lingtai::desktop::observe_exact_agent_processes(main_agent);
        expect(revived.available && revived.pids.size() == 1
                && revived.pids != first.pids,
            "CPR produces a fresh exact process");
        for (const auto pid : revived.pids) {
            std::cout << "EXACT_REVIVED_PID: " << pid << '\n';
        }

        result = run_command(attachment, runtime_python,
            snapshot({agent_row("Main", AgentRole::main,
                AgentPresenceKind::alive, "asleep")}),
            AgentLifecycleCommand::clear);
        expect(result && successful(*result, AgentLifecycleOutcomeKind::requested),
            "live clear reports requested, not prematurely applied");
        expect(wait_until([&] {
                return !fs::exists(main_agent / ".clear")
                    && contains_clear_event(main_agent);
            }, 12s),
            "the real kernel consumes desktop\\n clear and records completion");

        result = run_command(attachment, runtime_python,
            snapshot({agent_row("Main", AgentRole::main,
                AgentPresenceKind::alive, "asleep")}),
            AgentLifecycleCommand::refresh);
        expect(result && successful(*result, AgentLifecycleOutcomeKind::applied),
            "empty hard refresh restarts the real Agent");
        expect(active_preset(main_agent) == default_path.string(),
            "empty refresh atomically activates the default preset");

        result = run_command(attachment, runtime_python,
            snapshot({agent_row("Main", AgentRole::main,
                AgentPresenceKind::alive, "asleep")}),
            AgentLifecycleCommand::refresh, "alternate");
        expect(result && successful(*result, AgentLifecycleOutcomeKind::applied),
            "named hard refresh restarts with an allowed preset");
        expect(active_preset(main_agent) == alternate_path.string(),
            "named refresh activates only the uniquely allowed preset");

        const auto broken_venv = project / "broken-runtime";
        write_file(broken_venv / "bin/python", "not executable\n");
        const auto broken_agent = project / ".lingtai/broken";
        fs::create_directories(broken_agent / "logs");
        write_init(broken_agent, runtime_python, default_path, alternate_path,
            default_path, broken_venv);
        result = run_command(attachment, runtime_python,
            snapshot({
                agent_row("Main", AgentRole::main,
                    AgentPresenceKind::alive, "asleep"),
                agent_row("broken", AgentRole::agent,
                    AgentPresenceKind::stale, "suspended"),
            }), AgentLifecycleCommand::cpr, "all", fs::path("Main"));
        expect(result && result->all && result->targets.size() == 2
                && result->targets[0].outcome
                    == AgentLifecycleOutcomeKind::already_online
                && result->targets[1].outcome
                    == AgentLifecycleOutcomeKind::failed,
            "CPR all aggregates one real success and one intentional launch failure");
        if (result) {
            const auto text = lingtai::desktop::agent_lifecycle_result_text(*result);
            expect(text.find("1/2") != std::string::npos
                    && text.find("broken") != std::string::npos
                    && text.find("launch") != std::string::npos,
                "aggregate output names the failing Agent and launch phase");
        }

        stop_agent(attachment, "Main");
        stop_agent(attachment, "broken");
        const auto remaining =
            lingtai::desktop::observe_exact_agent_processes(main_agent);
        std::cout << "EXACT_REMAINING_PIDS: " << remaining.pids.size() << '\n';
        expect(remaining.available && remaining.pids.empty(),
            "smoke cleanup leaves no real Agent process behind");
    } catch (const std::exception &error) {
        std::cerr << "FAIL: smoke exception: " << error.what() << '\n';
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
