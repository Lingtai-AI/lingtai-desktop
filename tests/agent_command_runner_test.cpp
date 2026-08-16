// RED contract test for the future narrow asynchronous AgentCommandRunner.
//
// The runner is the one async owner of the documented headless control
// command `<exe> control --project <project-root> --agent <agent-key>
// <sleep|suspend|cpr|clear|refresh> [arg]`: exact separate argv, never a
// shell string. It starts exactly one nonblocking QProcess, keeps stdout and
// stderr in their own channels, and parses exactly one documented headless
// JSON result (success `{"command": "...", "agent": "...", "status":
// "signaled"}` indented on stdout; failure `{"code": "...", "error": "..."}`
// indented with sorted keys on stderr) through the one per-call callback,
// bound to the exact project root, Agent key, and selection/project
// generation captured at start. The generation is a NativeShell epoch and
// never enters the argv.
//
// `src/agent_command_runner.h` does not exist yet: this target is RED and is
// expected not to compile until the runner is implemented to this contract.

#include "src/agent_command_runner.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QThread>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using lingtai::desktop::AgentCommandResult;
using lingtai::desktop::AgentCommandResultKind;
using lingtai::desktop::AgentCommandRunner;

namespace {

const char *kAgentKey = "writer";
const char *kAction = "refresh";
const char *kPresetArg = "sonnet-4";

// The literal headless JSON wire: success is indented JSON plus newline on
// stdout with `"status":"signaled"`; failure is indented JSON plus newline
// with sorted map keys on stderr.
const char *kSuccessJson =
    "{\n  \"command\": \"refresh\",\n  \"agent\": \"writer\",\n"
    "  \"status\": \"signaled\"\n}\n";
const char *kFailureJson =
    "{\n  \"code\": \"control_failed\",\n  \"error\": \"fixture failure\"\n}\n";

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

fs::path fixture_path(const fs::path &sandbox, const std::string &kind) {
    return sandbox / ("control-" + kind);
}

// Writes a fake TUI control executable that records its exact separate argv
// (one per line) into <sandbox>/argv.log and then emits exactly one
// documented JSON result on stdout or stderr. Fixture only.
void write_fixture(const fs::path &sandbox, const std::string &kind) {
    std::string script = "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"";
    script += (sandbox / "argv.log").string();
    script += "\"\n";
    if (kind == "success") {
        script += std::string("printf '%s' '") + kSuccessJson + "'\n";
    } else if (kind == "failure") {
        script += std::string("printf '%s' '") + kFailureJson + "' >&2\n";
    } else if (kind == "malformed") {
        script += "printf '%s' 'not-json: garbled control reply'\n";
    } else {
        script += std::string("printf '%s' '") + kSuccessJson + "'\n";
    }
    script += kind == "failure" ? "exit 1\n"
             : (kind == "nonzero" ? "exit 3\n" : "exit 0\n");
    const auto path = fixture_path(sandbox, kind);
    std::ofstream(path) << script;
    fs::permissions(path,
        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
            | fs::perms::others_read | fs::perms::others_exec,
        fs::perm_options::replace);
}

std::vector<std::string> recorded_argv(const fs::path &sandbox) {
    std::ifstream stream(sandbox / "argv.log");
    std::vector<std::string> argv;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) argv.push_back(line);
    }
    return argv;
}

struct Captured { bool done = false; AgentCommandResult result; };

void finish(QCoreApplication &app, Captured &captured) {
    QElapsedTimer timer;
    timer.start();
    while (!captured.done && timer.elapsed() < 10000)
        app.processEvents(QEventLoop::AllEvents, 50), QThread::msleep(10);
}

bool launch(AgentCommandRunner &runner, const fs::path &exe,
            const std::string &root, const std::string &generation,
            Captured &captured) {
    return runner.run(
        exe.string(), root, kAgentKey, kAction, kPresetArg, generation,
        [&](AgentCommandResult result) {
            captured.done = true;
            captured.result = std::move(result);
        });
}

Captured run_case(QCoreApplication &app, AgentCommandRunner &runner,
                  const fs::path &sandbox, const std::string &kind,
                  const std::string &root, const std::string &generation) {
    write_fixture(sandbox, kind);
    Captured captured;
    launch(runner, fixture_path(sandbox, kind), root, generation, captured);
    finish(app, captured);
    expect(captured.done, "the one callback fires on completion");
    return captured;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "expected one temporary-directory argument\n";
        return 2;
    }

    QCoreApplication app(argc, argv);

    const fs::path sandbox = argv[1];
    fs::create_directories(sandbox);

    const std::string project_root = (sandbox / "workspace").string();

    // Exact separate argv for the refresh action, plus the one success parse.
    {
        AgentCommandRunner runner;
        const auto captured = run_case(app, runner, sandbox, "success",
                                       project_root, "gen-4");
        expect(recorded_argv(sandbox)
                   == std::vector<std::string>{"control", "--project",
                                               project_root, "--agent",
                                               kAgentKey, kAction, kPresetArg},
               "argv is the exact separate documented control command");
        expect(captured.result.kind == AgentCommandResultKind::succeeded,
               "success JSON parses to succeeded");
        expect(captured.result.stdout_bytes == QByteArray(kSuccessJson),
               "stdout holds exactly the emitted success JSON");
        expect(captured.result.stderr_bytes.isEmpty(),
               "stderr stays empty on canonical success");
    }

    // Exactly one pending command: a duplicate start is rejected.
    {
        AgentCommandRunner runner;
        Captured first;
        expect(launch(runner, fixture_path(sandbox, "success"), project_root,
                      "gen-4", first),
               "the first run() is accepted");
        Captured second;
        expect(!launch(runner, fixture_path(sandbox, "success"), project_root,
                       "gen-9", second),
               "a second run() while pending is rejected");
        expect(runner.is_pending(), "the runner stays pending");
        finish(app, first);
        expect(first.done, "the first callback fires");
        expect(!second.done, "the rejected callback never fires");
        expect(!runner.is_pending(), "pending clears after the first completes");
        expect(recorded_argv(sandbox).size() == 7,
               "exactly one invocation was started");
    }

    // Late completion binds the start-time project root, Agent key, and
    // selection/project generation.
    {
        AgentCommandRunner runner;
        const auto active_root = (sandbox / "active").string();
        const auto captured = run_case(app, runner, sandbox, "success",
                                       active_root, "gen-7");
        expect(captured.result.bound_project_root == active_root,
               "late completion binds the exact active project root");
        expect(captured.result.bound_agent_key == kAgentKey,
               "late completion binds the exact selected Agent key");
        expect(captured.result.bound_generation == "gen-7",
               "late completion binds the exact start-time generation");
    }

    // Malformed, nonzero, and failure are never success; stdout and stderr
    // truth is preserved independently.
    {
        AgentCommandRunner runner;
        const auto run = [&](const std::string &kind) {
            return run_case(app, runner, sandbox, kind, project_root,
                            "gen-4");
        };
        const auto failure = run("failure");
        expect(failure.result.kind != AgentCommandResultKind::succeeded,
               "failure JSON is not success");
        expect(failure.result.stdout_bytes.isEmpty(),
               "stdout stays empty on failure");
        expect(failure.result.stderr_bytes == QByteArray(kFailureJson),
               "stderr holds exactly the emitted failure JSON");
        const auto malformed = run("malformed");
        expect(malformed.result.kind != AgentCommandResultKind::succeeded,
               "malformed output is not success");
        expect(malformed.result.stdout_bytes
                   == QByteArray("not-json: garbled control reply"),
               "stdout holds exactly the garbled bytes");
        const auto nonzero = run("nonzero");
        expect(nonzero.result.kind != AgentCommandResultKind::succeeded,
               "a nonzero exit is not success even with success-shaped stdout");
        expect(nonzero.result.stdout_bytes == QByteArray(kSuccessJson),
               "stdout truth is preserved on a nonzero exit");
    }

    if (failures == 0) {
        std::cout << "PASS: agent_command_runner_test\n";
    }
    return failures == 0 ? 0 : 1;
}
