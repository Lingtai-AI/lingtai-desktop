#include "agent_lifecycle.h"
#include "agent_process.h"
#include "agent_signal.h"
#include "project_attachment.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using lingtai::desktop::AgentIdentityFacts;
using lingtai::desktop::AgentLaunchOutcome;
using lingtai::desktop::AgentLaunchResult;
using lingtai::desktop::AgentLifecycleCommand;
using lingtai::desktop::AgentLifecycleController;
using lingtai::desktop::AgentLifecycleDependencies;
using lingtai::desktop::AgentLifecycleOutcomeKind;
using lingtai::desktop::AgentLifecyclePhase;
using lingtai::desktop::AgentLifecycleRequest;
using lingtai::desktop::AgentLifecycleResult;
using lingtai::desktop::AgentLifecycleStartResult;
using lingtai::desktop::AgentManifestKind;
using lingtai::desktop::AgentPresenceKind;
using lingtai::desktop::AgentProcessId;
using lingtai::desktop::AgentProcessObservation;
using lingtai::desktop::AgentRole;
using lingtai::desktop::AgentRow;
using lingtai::desktop::AgentSignalKind;
using lingtai::desktop::AgentSignalRemoveResult;
using lingtai::desktop::AgentSignalWriteResult;
using lingtai::desktop::AgentSnapshot;
using lingtai::desktop::AgentTerminationSignal;
using lingtai::desktop::ProjectAttachment;

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void require(bool condition, const char *message) {
    expect(condition, message);
    if (!condition) std::exit(2);
}

void write_file(const fs::path &path, const std::string &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
}

std::string read_file(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)), {});
}

AgentRow row(const std::string &key, AgentRole role,
        AgentPresenceKind presence, std::string state = "idle") {
    AgentRow result;
    result.directory_key = key;
    result.directory_path = fs::path(".lingtai") / key;
    result.manifest_kind = AgentManifestKind::valid;
    result.role = role;
    result.presence = presence;
    result.identity = AgentIdentityFacts{};
    result.identity->state = std::move(state);
    return result;
}

struct ProjectFixture {
    fs::path root;
    ProjectAttachment attachment;
    AgentSnapshot snapshot;

    static ProjectFixture create(const fs::path &sandbox,
            const std::string &name,
            std::vector<AgentRow> rows) {
        auto root = sandbox / name;
        fs::remove_all(root);
        fs::create_directories(root / ".lingtai");
        for (const auto &item : rows) {
            const auto dir = root / ".lingtai" / item.directory_key;
            fs::create_directories(dir / "logs");
            const auto manifest = std::string(
                "{\"agent_name\":\"") + item.directory_key.string()
                + "\",\"admin\":{},\"state\":\"idle\",\"molt_count\":0}";
            write_file(dir / ".agent.json", manifest);
            write_file(dir / "init.json",
                "{\n  \"unrelated\": {\"keep\": 7},\n"
                "  \"manifest\": {\"preset\": {"
                "\"allowed\": [\"/presets/default.json\", \"/presets/other.json\"], "
                "\"default\": \"/presets/default.json\", "
                "\"active\": \"/presets/other.json\"}}\n}\n");
            if (item.presence == AgentPresenceKind::alive) {
                write_file(dir / ".agent.heartbeat", "1000");
            }
        }
        const auto attached = lingtai::desktop::attach_project(root);
        require(static_cast<bool>(attached), "fixture project attaches");
        AgentSnapshot snapshot;
        snapshot.scan = lingtai::desktop::AgentScanState::complete;
        snapshot.items = std::move(rows);
        return {root, *attached.attachment, std::move(snapshot)};
    }

    fs::path agent(const std::string &key) const {
        return root / ".lingtai" / key;
    }
};

struct FakeRuntime {
    std::chrono::steady_clock::time_point mono{};
    double wall = 1000.0;
    std::map<fs::path, std::vector<AgentProcessId>> processes;
    std::set<fs::path> launch_failures;
    std::vector<std::pair<AgentProcessId, AgentTerminationSignal>> sent_signals;
    std::vector<fs::path> launches;
    bool process_table_available = true;
    bool term_removes = false;
    AgentProcessId next_pid = 4000;

    void advance(std::chrono::milliseconds amount) {
        mono += amount;
        wall += std::chrono::duration<double>(amount).count();
    }

    AgentLifecycleDependencies dependencies() {
        AgentLifecycleDependencies result;
        result.automatic_poll = false;
        result.processes.observe = [this](const fs::path &dir) {
            return AgentProcessObservation{
                .available = process_table_available,
                .pids = process_table_available ? processes[dir]
                                                : std::vector<AgentProcessId>(),
            };
        };
        result.processes.signal = [this](const fs::path &dir,
                AgentProcessId pid, AgentTerminationSignal signal) {
            auto &current = processes[dir];
            if (std::ranges::find(current, pid) == current.end()) return false;
            sent_signals.emplace_back(pid, signal);
            if (signal == AgentTerminationSignal::kill || term_removes) {
                std::erase(current, pid);
            }
            return true;
        };
        result.launcher.launch = [this](const ProjectAttachment &attachment,
                const fs::path &key, const fs::path &) {
            const auto dir = attachment.root() / ".lingtai" / key;
            launches.push_back(dir);
            if (launch_failures.contains(dir)) return AgentLaunchOutcome{};
            const auto pid = next_pid++;
            processes[dir] = {pid};
            return AgentLaunchOutcome{
                .result = AgentLaunchResult::started,
                .pid = pid,
                .log_path = dir / "logs/agent.log",
            };
        };
        result.clock.monotonic_now = [this] { return mono; };
        result.clock.wall_seconds = [this] { return wall; };
        return result;
    }
};

AgentLifecycleRequest request(const ProjectFixture &fixture,
        AgentLifecycleCommand command, std::string argument = {},
        std::optional<fs::path> selected = fs::path("main"),
        std::string generation = "generation-1") {
    return {
        .attachment = fixture.attachment,
        .snapshot = fixture.snapshot,
        .selected_agent_key = std::move(selected),
        .command = command,
        .argument = std::move(argument),
        .fallback_python = "/runtime/bin/python",
        .generation = std::move(generation),
    };
}

struct Capture {
    bool done = false;
    AgentLifecycleResult result;
};

AgentLifecycleStartResult run(AgentLifecycleController &controller,
        AgentLifecycleRequest operation, Capture &capture) {
    return controller.run(std::move(operation), [&](AgentLifecycleResult result) {
        capture.done = true;
        capture.result = std::move(result);
    });
}

void fresh_heartbeat(ProjectFixture &fixture, FakeRuntime &runtime,
        const std::string &key) {
    runtime.wall += 1.0;
    write_file(fixture.agent(key) / ".agent.heartbeat",
        std::to_string(runtime.wall));
}

void test_signal_safety(const fs::path &sandbox) {
    auto fixture = ProjectFixture::create(sandbox, "signal-safety", {
        row("main", AgentRole::main, AgentPresenceKind::alive),
        row("sibling", AgentRole::agent, AgentPresenceKind::alive),
    });
    const auto outside = sandbox / "outside-signal";
    write_file(outside, "outside");

    expect(lingtai::desktop::write_agent_signal(fixture.attachment, "main",
               AgentSignalKind::suspend) == AgentSignalWriteResult::written,
        "suspend marker writes through the descriptor seam");
    expect(fs::file_size(fixture.agent("main") / ".suspend") == 0,
        "suspend marker is zero bytes");
    expect(!fs::exists(fixture.agent("sibling") / ".suspend"),
        "signal writes only the exact target");
    expect(lingtai::desktop::write_agent_signal(fixture.attachment, "main",
               AgentSignalKind::clear) == AgentSignalWriteResult::written,
        "clear marker writes through the same seam");
    expect(read_file(fixture.agent("main") / ".clear") == "desktop\n",
        "clear marker carries the exact Desktop source tag");

    fs::remove(fixture.agent("main") / ".clear");
    fs::create_symlink(outside, fixture.agent("main") / ".clear");
    expect(lingtai::desktop::write_agent_signal(fixture.attachment, "main",
               AgentSignalKind::clear) == AgentSignalWriteResult::refused,
        "a symlinked signal leaf is refused");
    expect(read_file(outside) == "outside",
        "a refused symlink never mutates its target");
    expect(lingtai::desktop::remove_agent_signal(fixture.attachment, "main",
               AgentSignalKind::clear) == AgentSignalRemoveResult::refused,
        "cleanup refuses rather than unlinking a symlink signal leaf");
    expect(lingtai::desktop::write_agent_signal(fixture.attachment, "../main",
               AgentSignalKind::sleep) == AgentSignalWriteResult::refused,
        "an unsafe Agent key is refused");
}

void test_target_and_argument_matrix(const fs::path &sandbox) {
    auto fixture = ProjectFixture::create(sandbox, "targets", {
        row("human", AgentRole::human, AgentPresenceKind::alive_human),
        row("main", AgentRole::main, AgentPresenceKind::alive),
        row("live", AgentRole::agent, AgentPresenceKind::alive),
        row("dead", AgentRole::agent, AgentPresenceKind::stale),
    });
    expect(lingtai::desktop::resolve_lifecycle_targets(
               fixture.snapshot, fs::path("live"), false, false)
            == std::vector<fs::path>{"live"},
        "the exact selected Agent wins target resolution");
    expect(lingtai::desktop::resolve_lifecycle_targets(
               fixture.snapshot, std::nullopt, false, false)
            == std::vector<fs::path>{"main"},
        "Main is the no-selection single-target fallback");
    expect(lingtai::desktop::resolve_lifecycle_targets(
               fixture.snapshot, std::nullopt, true, true)
            == std::vector<fs::path>({"main", "live"}),
        "all/live excludes human and dead Agents");
    expect(lingtai::desktop::resolve_lifecycle_targets(
               fixture.snapshot, std::nullopt, true, false)
            == std::vector<fs::path>({"main", "live", "dead"}),
        "all/dead-capable includes every non-human Agent");

    FakeRuntime runtime;
    AgentLifecycleController controller(runtime.dependencies());
    Capture capture;
    expect(run(controller, request(fixture, AgentLifecycleCommand::clear,
                   "all"), capture)
            == AgentLifecycleStartResult::invalid_argument,
        "/clear all is rejected locally");
    expect(run(controller, request(fixture, AgentLifecycleCommand::cpr,
                   "later"), capture)
            == AgentLifecycleStartResult::invalid_argument,
        "/cpr accepts only empty or all");
    expect(run(controller, request(fixture, AgentLifecycleCommand::refresh,
                   "two words"), capture)
            == AgentLifecycleStartResult::invalid_argument,
        "refresh rejects a multi-token preset argument");
}

void test_process_exactness() {
    const auto target = fs::canonical(fs::current_path());
    const auto exact = std::vector<std::string>{
        "/runtime/bin/python3.13", "-m", "lingtai", "run", target.string()};
    expect(lingtai::desktop::matches_exact_agent_process(exact, target),
        "exact python module argv matches the canonical Agent directory");
    auto extra = exact;
    extra.push_back("--lookalike");
    expect(!lingtai::desktop::matches_exact_agent_process(extra, target),
        "an extra argument cannot match an Agent process");
    auto wrapper = exact;
    wrapper[0] = "/bin/sh";
    expect(!lingtai::desktop::matches_exact_agent_process(wrapper, target),
        "a wrapper executable cannot match");
    auto sibling = exact;
    sibling[4] = target.parent_path().string();
    expect(!lingtai::desktop::matches_exact_agent_process(sibling, target),
        "a sibling/parent directory cannot match by substring");

    const auto child = ::fork();
    require(child >= 0, "unrelated child process starts");
    if (child == 0) {
        ::execl("/bin/sleep", "sleep", "30", nullptr);
        std::_Exit(127);
    }
    std::this_thread::sleep_for(50ms);
    expect(!lingtai::desktop::signal_exact_agent_process(
               target, child, AgentTerminationSignal::terminate),
        "production signaling refuses an unrelated live PID after argv recheck");
    expect(::kill(child, 0) == 0,
        "the unrelated process remains alive after refused signaling");
    static_cast<void>(::kill(child, SIGTERM));
    int status = 0;
    static_cast<void>(::waitpid(child, &status, 0));
}

void test_sleep_suspend_and_cpr(const fs::path &sandbox) {
    auto fixture = ProjectFixture::create(sandbox, "basic", {
        row("main", AgentRole::main, AgentPresenceKind::alive),
    });
    FakeRuntime runtime;
    runtime.processes[fixture.agent("main")] = {101};

    {
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        expect(run(controller, request(fixture, AgentLifecycleCommand::sleep),
                   capture) == AgentLifecycleStartResult::started,
            "sleep starts nonblocking");
        expect(!capture.done && fs::exists(fixture.agent("main") / ".sleep"),
            "sleep remains pending after the marker write");
        fs::remove(fixture.agent("main") / ".sleep");
        controller.tick();
        expect(capture.done
                && capture.result.targets[0].outcome
                    == AgentLifecycleOutcomeKind::applied,
            "marker consumption proves sleep applied");
    }

    {
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::sleep), capture);
        runtime.advance(4s);
        controller.tick();
        expect(capture.done
                && capture.result.targets[0].outcome
                    == AgentLifecycleOutcomeKind::timed_out
                && fs::exists(fixture.agent("main") / ".sleep"),
            "sleep times out truthfully when the kernel does not consume its marker");
        fs::remove(fixture.agent("main") / ".sleep");
    }

    {
        const auto outside = sandbox / "sleep-race-outside";
        write_file(outside, "unchanged");
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::sleep), capture);
        fs::remove(fixture.agent("main") / ".sleep");
        fs::create_symlink(outside, fixture.agent("main") / ".sleep");
        controller.tick();
        expect(capture.done
                && capture.result.targets[0].outcome
                    == AgentLifecycleOutcomeKind::failed
                && read_file(outside) == "unchanged",
            "a post-write symlink swap is refused, never mistaken for consumption");
        fs::remove(fixture.agent("main") / ".sleep");
    }

    {
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::suspend), capture);
        runtime.advance(11s);
        controller.tick();
        expect(capture.done
                && capture.result.targets[0].outcome
                    == AgentLifecycleOutcomeKind::timed_out,
            "suspend reports timeout while an exact process remains");
        fs::remove(fixture.agent("main") / ".suspend");
    }

    {
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::cpr), capture);
        expect(capture.done
                && capture.result.targets[0].outcome
                    == AgentLifecycleOutcomeKind::already_online
                && runtime.launches.empty(),
            "CPR refuses duplicate launch from exact process evidence");
    }

    runtime.processes[fixture.agent("main")].clear();
    fs::remove(fixture.agent("main") / ".agent.heartbeat");
    const auto lock_path = fixture.agent("main") / ".agent.lock";
    write_file(lock_path, "");
    const auto held_fd = ::open(lock_path.c_str(), O_RDWR | O_CLOEXEC);
    require(held_fd >= 0 && ::flock(held_fd, LOCK_EX | LOCK_NB) == 0,
        "test holds the real advisory Agent lease");
    {
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::cpr), capture);
        controller.tick();
        expect(!capture.done && runtime.launches.empty(),
            "a held advisory lease blocks CPR without blocking the caller");
        static_cast<void>(::flock(held_fd, LOCK_UN));
        static_cast<void>(::close(held_fd));
        controller.tick();
        expect(!capture.done && runtime.launches.size() == 1,
            "a free stale lock path is not a CPR blocker");
        fresh_heartbeat(fixture, runtime, "main");
        controller.tick();
        expect(capture.done
                && capture.result.targets[0].outcome
                    == AgentLifecycleOutcomeKind::applied,
            "CPR completes only after a fresh heartbeat");
    }

    runtime.processes[fixture.agent("main")].clear();
    fs::remove(fixture.agent("main") / ".agent.heartbeat");
    {
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::cpr), capture);
        controller.tick();
        runtime.advance(600ms);
        runtime.processes[fixture.agent("main")].clear();
        controller.tick();
        expect(capture.done
                && capture.result.targets[0].phase
                    == AgentLifecyclePhase::heartbeat_wait,
            "a child exit before heartbeat is a phase-specific failure");
    }

    runtime.processes[fixture.agent("main")].clear();
    {
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::cpr), capture);
        controller.tick();
        runtime.advance(11s);
        controller.tick();
        expect(!capture.done,
            "an exact slow-start process extends heartbeat observation past 10s");
        fresh_heartbeat(fixture, runtime, "main");
        controller.tick();
        expect(capture.done, "a slow-start Agent can still complete before the cap");
    }

    runtime.processes[fixture.agent("main")].clear();
    fs::remove(fixture.agent("main") / ".agent.heartbeat");
    {
        const auto timeout_fd = ::open(lock_path.c_str(), O_RDWR | O_CLOEXEC);
        require(timeout_fd >= 0 && ::flock(timeout_fd, LOCK_EX | LOCK_NB) == 0,
            "test re-holds the advisory lease for timeout coverage");
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::cpr), capture);
        runtime.advance(61s);
        controller.tick();
        expect(capture.done
                && capture.result.targets[0].outcome
                    == AgentLifecycleOutcomeKind::timed_out
                && capture.result.targets[0].phase
                    == AgentLifecyclePhase::lease_wait,
            "CPR reports the held-lease timeout phase without launching");
        static_cast<void>(::flock(timeout_fd, LOCK_UN));
        static_cast<void>(::close(timeout_fd));
    }

    {
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::cpr), capture);
        controller.tick();
        runtime.advance(61s);
        controller.tick();
        expect(capture.done
                && capture.result.targets[0].outcome
                    == AgentLifecycleOutcomeKind::timed_out
                && capture.result.targets[0].phase
                    == AgentLifecyclePhase::heartbeat_wait,
            "CPR reports the startup-cap timeout while an exact process survives");
    }
}

void test_clear_paths(const fs::path &sandbox) {
    {
        auto fixture = ProjectFixture::create(sandbox, "clear-live", {
            row("main", AgentRole::main, AgentPresenceKind::alive),
        });
        FakeRuntime runtime;
        runtime.processes[fixture.agent("main")] = {201};
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::clear), capture);
        expect(capture.done
                && capture.result.targets[0].outcome
                    == AgentLifecycleOutcomeKind::requested
                && read_file(fixture.agent("main") / ".clear") == "desktop\n",
            "live clear writes the exact source and reports only requested");
    }

    {
        auto fixture = ProjectFixture::create(sandbox, "clear-dead-molt", {
            row("main", AgentRole::main, AgentPresenceKind::stale),
        });
        fs::remove(fixture.agent("main") / ".agent.heartbeat");
        FakeRuntime runtime;
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::clear), capture);
        controller.tick();
        fresh_heartbeat(fixture, runtime, "main");
        controller.tick();
        expect(fs::exists(fixture.agent("main") / ".clear"),
            "dead clear revives before writing its fresh clear marker");
        write_file(fixture.agent("main") / ".agent.json",
            "{\"agent_name\":\"main\",\"admin\":{},\"molt_count\":1}");
        controller.tick();
        runtime.processes[fixture.agent("main")].clear();
        fs::remove(fixture.agent("main") / ".agent.heartbeat");
        controller.tick();
        expect(capture.done
                && capture.result.targets[0].outcome
                    == AgentLifecycleOutcomeKind::applied,
            "dead clear completes by molt_count then suspends its temporary Agent");
    }

    {
        auto fixture = ProjectFixture::create(sandbox, "clear-dead-event", {
            row("main", AgentRole::main, AgentPresenceKind::stale),
        });
        fs::remove(fixture.agent("main") / ".agent.heartbeat");
        write_file(fixture.agent("main") / "logs/events.jsonl",
            "{\"type\":\"old\"}\n");
        FakeRuntime runtime;
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::clear), capture);
        controller.tick();
        fresh_heartbeat(fixture, runtime, "main");
        controller.tick();
        std::ofstream(fixture.agent("main") / "logs/events.jsonl",
            std::ios::app) << "{\"type\":\"clear_received\",\"source\":\"desktop\"}\n";
        controller.tick();
        runtime.advance(11s);
        controller.tick();
        expect(capture.done
                && capture.result.targets[0].outcome
                    == AgentLifecycleOutcomeKind::partial
                && capture.result.targets[0].phase
                    == AgentLifecyclePhase::temporary_suspend,
            "clear completion by event remains truthful when final suspend times out");
    }

    {
        auto fixture = ProjectFixture::create(sandbox, "clear-timeout", {
            row("main", AgentRole::main, AgentPresenceKind::stale),
        });
        fs::remove(fixture.agent("main") / ".agent.heartbeat");
        FakeRuntime runtime;
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::clear), capture);
        controller.tick();
        fresh_heartbeat(fixture, runtime, "main");
        controller.tick();
        runtime.advance(31s);
        controller.tick();
        runtime.processes[fixture.agent("main")].clear();
        fs::remove(fixture.agent("main") / ".agent.heartbeat");
        controller.tick();
        expect(capture.done
                && capture.result.targets[0].outcome
                    == AgentLifecycleOutcomeKind::timed_out
                && capture.result.targets[0].phase
                    == AgentLifecyclePhase::clear_observation,
            "dead clear reports completion timeout after suspending its temporary Agent");
    }
}

QJsonObject read_json_object(const fs::path &path) {
    const auto bytes = QByteArray::fromStdString(read_file(path));
    return QJsonDocument::fromJson(bytes).object();
}

void test_refresh_paths(const fs::path &sandbox) {
    {
        auto fixture = ProjectFixture::create(sandbox, "refresh-unknown", {
            row("main", AgentRole::main, AgentPresenceKind::stale),
        });
        fs::remove(fixture.agent("main") / ".agent.heartbeat");
        FakeRuntime runtime;
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::refresh,
            "unknown"), capture);
        expect(capture.done
                && capture.result.targets[0].phase
                    == AgentLifecyclePhase::validation
                && !fs::exists(fixture.agent("main") / ".suspend"),
            "unknown preset is rejected before any destructive signal");
    }

    {
        auto fixture = ProjectFixture::create(sandbox, "refresh-default", {
            row("main", AgentRole::main, AgentPresenceKind::stale),
        });
        fs::remove(fixture.agent("main") / ".agent.heartbeat");
        for (const auto leaf : {".agent.lock", ".refresh", ".refresh.taken"}) {
            write_file(fixture.agent("main") / leaf, "stale");
        }
        FakeRuntime runtime;
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::refresh), capture);
        controller.tick();
        const auto root = read_json_object(fixture.agent("main") / "init.json");
        const auto manifest = root.value("manifest").toObject();
        const auto preset = manifest.value("preset").toObject();
        expect(preset.value("active").toString() == "/presets/default.json",
            "empty refresh atomically resets active to default");
        expect(root.value("unrelated").toObject().value("keep").toInt() == 7,
            "preset update preserves unrelated init.json content");
        expect(!fs::exists(fixture.agent("main") / ".agent.lock")
                && !fs::exists(fixture.agent("main") / ".refresh")
                && !fs::exists(fixture.agent("main") / ".refresh.taken")
                && !fs::exists(fixture.agent("main") / ".suspend"),
            "hard refresh removes every stale handshake leaf before/after launch");
        fresh_heartbeat(fixture, runtime, "main");
        controller.tick();
        expect(capture.done
                && capture.result.targets[0].outcome
                    == AgentLifecycleOutcomeKind::applied,
            "default hard refresh completes on a fresh heartbeat");
    }

    {
        auto fixture = ProjectFixture::create(sandbox, "refresh-escalate", {
            row("main", AgentRole::main, AgentPresenceKind::alive),
        });
        FakeRuntime runtime;
        runtime.processes[fixture.agent("main")] = {301};
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::refresh,
            "other"), capture);
        controller.tick();
        expect(runtime.sent_signals.size() == 1
                && runtime.sent_signals[0].second
                    == AgentTerminationSignal::terminate,
            "refresh sends TERM only after a free lease and exact process match");
        runtime.advance(2s);
        controller.tick();
        expect(runtime.sent_signals.size() == 2
                && runtime.sent_signals[1].second == AgentTerminationSignal::kill,
            "refresh escalates a surviving exact process to KILL");
        controller.tick();
        controller.tick();
        const auto root = read_json_object(fixture.agent("main") / "init.json");
        const auto preset = root.value("manifest").toObject()
            .value("preset").toObject();
        expect(preset.value("active").toString() == "/presets/other.json",
            "named refresh activates the uniquely allowed preset");
        fresh_heartbeat(fixture, runtime, "main");
        controller.tick();
        expect(capture.done, "TERM-to-KILL refresh still reaches fresh heartbeat");
    }

    {
        auto fixture = ProjectFixture::create(sandbox, "refresh-term", {
            row("main", AgentRole::main, AgentPresenceKind::alive),
        });
        FakeRuntime runtime;
        runtime.processes[fixture.agent("main")] = {302};
        runtime.term_removes = true;
        AgentLifecycleController controller(runtime.dependencies());
        Capture capture;
        run(controller, request(fixture, AgentLifecycleCommand::refresh,
            "other"), capture);
        controller.tick();
        controller.tick();
        controller.tick();
        fresh_heartbeat(fixture, runtime, "main");
        controller.tick();
        expect(capture.done
                && runtime.sent_signals.size() == 1
                && runtime.sent_signals[0].second
                    == AgentTerminationSignal::terminate,
            "refresh proceeds after TERM success without sending KILL");
    }
}

void test_aggregate_and_generation(const fs::path &sandbox) {
    auto fixture = ProjectFixture::create(sandbox, "aggregate", {
        row("main", AgentRole::main, AgentPresenceKind::stale),
        row("worker", AgentRole::agent, AgentPresenceKind::stale),
    });
    fs::remove(fixture.agent("main") / ".agent.heartbeat");
    fs::remove(fixture.agent("worker") / ".agent.heartbeat");
    FakeRuntime runtime;
    runtime.launch_failures.insert(fixture.agent("worker"));
    AgentLifecycleController controller(runtime.dependencies());
    Capture capture;
    run(controller, request(fixture, AgentLifecycleCommand::cpr, "all",
        fs::path("worker"), "selection-42"), capture);
    controller.tick();
    fresh_heartbeat(fixture, runtime, "main");
    controller.tick();
    controller.tick();
    expect(capture.done && capture.result.targets.size() == 2,
        "all continues after an independent target failure");
    expect(capture.result.targets[0].outcome == AgentLifecycleOutcomeKind::applied
            && capture.result.targets[1].outcome
                == AgentLifecycleOutcomeKind::failed,
        "all preserves each Agent's truthful result");
    expect(capture.result.bound_generation == "selection-42"
            && capture.result.bound_project_root == fixture.root,
        "completion retains the exact selection/project generation binding");
    const auto text = lingtai::desktop::agent_lifecycle_result_text(capture.result);
    expect(text.find("1/2") != std::string::npos
            && text.find("worker") != std::string::npos
            && text.find("launch") != std::string::npos,
        "aggregate text names the failing Agent and phase");
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "expected one temporary-directory argument\n";
        return 2;
    }
    QCoreApplication app(argc, argv);
    const auto sandbox = fs::path(argv[1]);
    fs::remove_all(sandbox);
    fs::create_directories(sandbox);

    test_signal_safety(sandbox);
    test_target_and_argument_matrix(sandbox);
    test_process_exactness();
    test_sleep_suspend_and_cpr(sandbox);
    test_clear_paths(sandbox);
    test_refresh_paths(sandbox);
    test_aggregate_and_generation(sandbox);

    fs::remove_all(sandbox);
    if (failures == 0) std::cout << "PASS: agent_lifecycle_test\n";
    return failures == 0 ? 0 : 1;
}
