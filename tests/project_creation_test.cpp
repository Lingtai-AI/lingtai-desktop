#include "agent_projection.h"
#include "agent_setup_store.h"
#include "project_attachment.h"
#include "project_creation.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void write_file(const fs::path &path, const std::string &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << bytes;
    require(stream.good(), "write failed: " + path.string());
}

std::string read_file(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

QJsonObject read_object(const fs::path &path) {
    const auto bytes = read_file(path);
    const auto document = QJsonDocument::fromJson(
        QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())));
    require(document.isObject(), "expected JSON object: " + path.string());
    return document.object();
}

bool has_stage(const fs::path &destination) {
    for (const auto &entry : fs::directory_iterator(destination)) {
        if (entry.path().filename().string().starts_with(".lingtai.create-")) {
            return true;
        }
    }
    return false;
}

bool has_unresolved_placeholder(std::string_view bytes) {
    return bytes.find("{{") != std::string_view::npos
        || bytes.find("}}") != std::string_view::npos;
}

void replace_all(std::string &bytes, std::string_view token,
        std::string_view replacement) {
    auto offset = std::size_t{0};
    while ((offset = bytes.find(token, offset)) != std::string::npos) {
        bytes.replace(offset, token.size(), replacement);
        offset += replacement.size();
    }
}

fs::path adaptive_fixture() {
    return fs::path(LINGTAI_PROJECT_CREATION_FIXTURE_DIR)
        / "adaptive-ead292d48703192c31f0abda791a666ffc6c0263/.recipe";
}

fs::path adaptive_greeting(std::string_view language) {
    auto path = adaptive_fixture() / "greet";
    if (language == "zh" || language == "wen") path /= language;
    return path / "greet.md";
}

fs::path adaptive_comment(std::string_view language) {
    auto path = adaptive_fixture() / "comment";
    if (language == "zh" || language == "wen") path /= language;
    return path / "comment.md";
}

lingtai::desktop::ProjectCreationRequest request_for(
        const fs::path &destination, const fs::path &global,
        const fs::path &runtime_python) {
    lingtai::desktop::AgentSetupDraft setup;
    setup.language = "zh";
    setup.context_limit = 300000;
    setup.max_rpm = 60;
    setup.max_aed_attempts = 5;
    setup.karma = true;
    setup.nirvana = false;
    setup.soul_delay = 7200.0;
    setup.covenant_file = (global / "covenant/zh/covenant.md").string();
    return {
        .destination = destination,
        .preset_path = global / "presets/saved/alpha.json",
        .allowed_preset_paths = {
            global / "presets/saved/beta.json",
        },
        .runtime_python = runtime_python,
        .env_file = global / ".env",
        .covenant_file = global / "covenant/zh/covenant.md",
        .agent_name = "orchestrator",
        .agent_directory = "main",
        .setup = setup,
        .comment = "Keep the project scientifically honest.\n",
        .guidance_local_time = [] { return "2031-04-05 06:07"; },
        .guidance_cached_location = [] {
            return "Chicago, Illinois, US";
        },
    };
}

void assert_creation_shape(const fs::path &destination,
        const fs::path &global, const fs::path &runtime_python) {
    const auto lingtai = destination / ".lingtai";
    const auto agent = lingtai / "main";
    require(fs::is_directory(lingtai / "human/mailbox/inbox")
            && fs::is_directory(lingtai / "human/mailbox/sent")
            && fs::is_directory(lingtai / "human/mailbox/archive"),
        "human mailbox shape missing");
    require(fs::is_directory(agent / "mailbox/inbox")
            && fs::is_directory(agent / "mailbox/sent")
            && fs::is_directory(agent / "mailbox/archive")
            && fs::is_directory(lingtai / ".library_shared"),
        "Agent mailbox/shared-library shape missing");
    require(!fs::exists(lingtai / ".tui-asset"),
        "Desktop must not recreate TUI-only recipe state");
    require(!fs::exists(lingtai / ".recipe"),
        "Desktop must not publish project-root recipe state");

    const auto init = read_object(agent / "init.json");
    const auto manifest = init.value("manifest").toObject();
    require(manifest.value("agent_name").toString() == "orchestrator"
            && manifest.value("language").toString() == "zh"
            && manifest.value("context_limit").toInteger() == 300000
            && manifest.value("max_aed_attempts").toInteger() == 5,
        "reviewed Agent configuration not applied");
    const auto policy = manifest.value("preset").toObject();
    const auto selected = QString::fromStdString(
        (global / "presets/saved/alpha.json").string());
    require(policy.value("active").toString() == selected
            && policy.value("default").toString() == selected,
        "selected absolute preset must be active and default");
    const auto allowed = policy.value("allowed").toArray();
    require(allowed.size() == 2
            && allowed.at(0).toString() == QString::fromStdString(
                (global / "presets/saved/beta.json").string())
            && allowed.at(1).toString() == selected,
        "allowed policy must preserve requested order and include default");
    require(init.value("env_file").toString()
                == QString::fromStdString((global / ".env").string())
            && init.value("venv_path").toString()
                == QString::fromStdString(
                    runtime_python.parent_path().parent_path().string())
            && init.value("comment_file").toString()
                == QString::fromStdString((agent / "comment.md").string()),
        "runtime and comment references have the wrong shape");
    require(read_file(agent / "comment.md")
            == "Keep the project scientifically honest.\n",
        "reviewed comment bytes changed");
    const auto greeting = read_file(agent / ".prompt");
    require(greeting.find("欢迎人类") != std::string::npos
            && greeting.find("human") != std::string::npos
            && greeting.find("2031-04-05 06:07") != std::string::npos
            && greeting.find("Chicago, Illinois, US") != std::string::npos
            && !has_unresolved_placeholder(greeting),
        "custom-comment creation lost its localized resolved greeting");

    const auto identity = read_object(agent / ".agent.json");
    require(identity.value("agent_name").toString() == "orchestrator"
            && identity.value("address").toString() == "main"
            && identity.value("state").toString().isEmpty()
            && identity.value("admin").toObject().value("karma").toBool(),
        "first-Agent identity shape is invalid");
}

} // namespace

int main(int argc, char **argv) {
    try {
        QCoreApplication application(argc, argv);
        require(argc == 2, "usage: project_creation_test <fixture-root>");
        auto root = fs::canonical(fs::path(argv[1]).parent_path())
            / fs::path(argv[1]).filename();
        std::error_code error;
        fs::remove_all(root, error);
        require(!error, "fixture cleanup failed");
        fs::create_directories(root);

        const auto global = root / "global";
        const auto runtime = root / "runtime/venv/bin/python";
        write_file(global / ".env", "TEST_API_KEY=fake\n");
        write_file(global / "covenant/zh/covenant.md", "# Covenant\n");
        const auto preset = [](const char *name, const char *model) {
            return std::string("{\n  \"name\": \"") + name
                + "\",\n  \"manifest\": {\n    \"llm\": {\"provider\": \"openai\", \"model\": \""
                + model
                + "\", \"api_key_env\": \"TEST_API_KEY\"},\n"
                  "    \"capabilities\": {\"system\": {}, \"email\": {}}\n"
                  "  }\n}\n";
        };
        write_file(global / "presets/saved/alpha.json",
            preset("alpha", "alpha-model"));
        write_file(global / "presets/saved/beta.json",
            preset("beta", "beta-model"));
        write_file(runtime, "#!/bin/sh\nexit 0\n");
        fs::permissions(runtime,
            fs::perms::owner_all | fs::perms::group_read
                | fs::perms::group_exec | fs::perms::others_read
                | fs::perms::others_exec,
            fs::perm_options::replace);

        const auto destination = root / "project";
        fs::create_directories(destination / "notes");
        write_file(destination / "notes/keep.txt", "preserve me\n");
        auto request = request_for(destination, global, runtime);
        const auto created = lingtai::desktop::create_project(request);
        require(created && created.project_dir == destination
                && created.agent_key == "main",
            "valid project creation failed: " + created.detail);
        require(read_file(destination / "notes/keep.txt") == "preserve me\n",
            "pre-existing destination contents changed");
        require(!has_stage(destination), "successful creation left staging residue");
        assert_creation_shape(destination, global, runtime);

        struct LocalizedCase {
            const char *language;
        };
        for (const auto &localized : {
                LocalizedCase{"en"},
                LocalizedCase{"zh"},
                LocalizedCase{"wen"},
            }) {
            const auto localized_destination = root
                / (std::string("localized-") + localized.language);
            fs::create_directories(localized_destination);
            request = request_for(localized_destination, global, runtime);
            request.setup.language = localized.language;
            request.comment.clear();
            const auto localized_result =
                lingtai::desktop::create_project(request);
            require(static_cast<bool>(localized_result),
                std::string("localized empty-comment creation failed for ")
                    + localized.language + ": " + localized_result.detail);

            const auto agent = localized_destination / ".lingtai/main";
            const auto greeting = read_file(agent / ".prompt");
            const auto playbook = read_file(agent / "comment.md");
            const auto init = read_object(agent / "init.json");
            auto expected_greeting = read_file(
                adaptive_greeting(localized.language));
            replace_all(expected_greeting, "{{time}}", "2031-04-05 06:07");
            replace_all(expected_greeting, "{{location}}",
                "Chicago, Illinois, US");
            replace_all(expected_greeting, "{{lang}}", localized.language);
            replace_all(expected_greeting, "{{soul_delay}}", "7200");
            replace_all(expected_greeting, "{{addr}}", "human");
            const auto commands = expected_greeting.find("{{commands}}");
            if (commands == std::string::npos) {
                require(greeting == expected_greeting,
                    "English greeting did not exactly follow the pinned adaptive fixture");
            } else {
                const auto prefix = expected_greeting.substr(0, commands);
                const auto suffix = expected_greeting.substr(
                    commands + std::string_view("{{commands}}").size());
                require(greeting.starts_with(prefix)
                        && greeting.ends_with(suffix)
                        && greeting.find("  - /btw — ") != std::string::npos
                        && greeting.find("  - /quit — ") != std::string::npos,
                    std::string("localized command expansion drifted for ")
                        + localized.language);
            }
            require(playbook == read_file(
                        adaptive_comment(localized.language))
                    && !has_unresolved_placeholder(playbook),
                std::string("localized adaptive comment was not the pinned fixture for ")
                    + localized.language);
            require(init.value("manifest").toObject().value("language")
                        .toString().toStdString() == localized.language
                    && init.value("comment_file").toString().toStdString()
                        == (agent / "comment.md").string(),
                std::string("localized final comment reference was wrong for ")
                    + localized.language);
        }

        const auto whitespace_comment_destination = root
            / "whitespace-comment-default";
        fs::create_directories(whitespace_comment_destination);
        request = request_for(
            whitespace_comment_destination, global, runtime);
        request.setup.language = "wen";
        request.comment = " \n\t\r\n";
        const auto whitespace_comment_result =
            lingtai::desktop::create_project(request);
        require(static_cast<bool>(whitespace_comment_result),
            "whitespace-only optional Comment must select the adaptive playbook: "
                + whitespace_comment_result.detail);
        const auto whitespace_agent = whitespace_comment_destination
            / ".lingtai/main";
        const auto whitespace_playbook = read_file(
            whitespace_agent / "comment.md");
        const auto whitespace_init = read_object(
            whitespace_agent / "init.json");
        require(whitespace_playbook == read_file(adaptive_comment("wen"))
                && !has_unresolved_placeholder(whitespace_playbook)
                && whitespace_init.value("comment_file")
                    .toString().toStdString()
                    == (whitespace_agent / "comment.md").string(),
            "whitespace-only Comment did not publish the selected localized playbook");

        // Publication is independent of runtime readiness. The configured
        // paths remain useful launch inputs, but a missing interpreter, env,
        // or covenant must be reported only by the post-commit launch owner.
        const auto runtime_independent = root / "runtime-independent";
        fs::create_directories(runtime_independent);
        request = request_for(
            runtime_independent, global, root / "missing/venv/bin/python");
        request.env_file = global / "missing.env";
        request.covenant_file = global / "missing-covenant.md";
        request.setup.covenant_file = request.covenant_file.string();
        const auto created_without_runtime =
            lingtai::desktop::create_project(request);
        require(created_without_runtime
                && created_without_runtime.stage
                    == lingtai::desktop::ProjectCreationStage::complete
                && fs::is_regular_file(
                    runtime_independent / ".lingtai/main/init.json"),
            "valid project draft must publish without a runnable kernel: "
                + created_without_runtime.detail);

        // Home shorthand belongs only to the NativeShell boundary. Direct
        // transaction calls retain the existing strict absolute-path shape.
        for (const auto &strict_destination : {
                fs::path("~"), fs::path("~/Documents"),
                fs::path("relative-project")}) {
            request = request_for(strict_destination, global, runtime);
            const auto strict_rejection =
                lingtai::desktop::create_project(request);
            require(!strict_rejection
                    && strict_rejection.failure
                        == lingtai::desktop::ProjectCreationFailure::invalid_destination
                    && strict_rejection.stage
                        == lingtai::desktop::ProjectCreationStage::draft_validation,
                "ProjectCreation accepted a UI-only or relative destination: "
                    + strict_destination.string());
        }
        const auto traversal_target = root / "traversal-target";
        fs::create_directories(traversal_target);
        request = request_for(
            root / "traversal-parent/../traversal-target", global, runtime);
        const auto traversal_rejection =
            lingtai::desktop::create_project(request);
        require(!traversal_rejection
                && traversal_rejection.failure
                    == lingtai::desktop::ProjectCreationFailure::invalid_destination
                && traversal_rejection.stage
                    == lingtai::desktop::ProjectCreationStage::draft_validation
                && !fs::exists(traversal_target / ".lingtai"),
            "ProjectCreation weakened absolute traversal rejection");

        const auto invalid_orchestrator = root / "invalid-orchestrator";
        fs::create_directories(invalid_orchestrator);
        request = request_for(invalid_orchestrator, global, runtime);
        request.setup.karma = false;
        request.setup.nirvana = false;
        const auto rejected_orchestrator =
            lingtai::desktop::create_project(request);
        require(!rejected_orchestrator
                && rejected_orchestrator.stage
                    == lingtai::desktop::ProjectCreationStage::staged_validation
                && !fs::exists(invalid_orchestrator / ".lingtai")
                && !has_stage(invalid_orchestrator),
            "staged exactly-one-orchestrator validation did not fail closed");

        auto attached = lingtai::desktop::attach_project(destination);
        require(static_cast<bool>(attached), "created project must attach");
        auto rows = lingtai::desktop::project_agents(*attached.attachment);
        require(rows.scan == lingtai::desktop::AgentScanState::complete
                && rows.items.size() == 2
                && rows.items.at(1).directory_key == "main"
                && rows.items.at(1).role == lingtai::desktop::AgentRole::main,
            "created first Agent must project as the Main row");

        // The current kernel adds durable identity on first construction.
        // Simulate only that kernel-owned augmentation, then prove setup loads
        // the created init and a no-change save preserves every byte.
        auto identity = read_object(destination / ".lingtai/main/.agent.json");
        identity["agent_id"] = "fixture-id";
        write_file(destination / ".lingtai/main/.agent.json",
            QJsonDocument(identity).toJson(QJsonDocument::Indented).toStdString());
        const lingtai::desktop::AgentSetupStore store(*attached.attachment);
        const auto loaded = store.load("main");
        require(static_cast<bool>(loaded), "AgentSetupStore rejected created configuration: "
            + loaded.detail);
        const auto init_before = read_file(destination / ".lingtai/main/init.json");
        const auto identity_before = read_file(
            destination / ".lingtai/main/.agent.json");
        const auto unchanged = store.save(*loaded.state, loaded.state->draft);
        require(unchanged.status == lingtai::desktop::AgentSetupSaveStatus::no_change
                && read_file(destination / ".lingtai/main/init.json") == init_before
                && read_file(destination / ".lingtai/main/.agent.json")
                    == identity_before,
            "no-change setup must preserve created policy bytes");

        const auto conflict = root / "conflict";
        fs::create_directories(conflict / ".lingtai");
        write_file(conflict / ".lingtai/keep", "owned elsewhere\n");
        request = request_for(conflict, global, runtime);
        const auto refused = lingtai::desktop::create_project(request);
        require(!refused
                && refused.failure
                    == lingtai::desktop::ProjectCreationFailure::existing_project
                && read_file(conflict / ".lingtai/keep") == "owned elsewhere\n",
            "conflicting .lingtai must be refused without mutation");

        for (const auto point : {
                lingtai::desktop::ProjectCreationFailurePoint::after_staging,
                lingtai::desktop::ProjectCreationFailurePoint::after_generation,
                lingtai::desktop::ProjectCreationFailurePoint::after_marker_removal,
                lingtai::desktop::ProjectCreationFailurePoint::publish_refused}) {
            const auto failed_destination = root
                / (point == lingtai::desktop::ProjectCreationFailurePoint::after_staging
                    ? "fail-stage"
                    : point == lingtai::desktop::ProjectCreationFailurePoint::after_generation
                        ? "fail-generation"
                    : point == lingtai::desktop::ProjectCreationFailurePoint::after_marker_removal
                        ? "fail-marker-removed" : "fail-publish-refused");
            fs::create_directories(failed_destination);
            write_file(failed_destination / "keep.txt", "unrelated\n");
            request = request_for(failed_destination, global, runtime);
            request.failure_point = point;
            const auto failed = lingtai::desktop::create_project(request);
            require(!failed && !fs::exists(failed_destination / ".lingtai")
                    && !has_stage(failed_destination)
                    && read_file(failed_destination / "keep.txt")
                        == "unrelated\n",
                "pre-commit failure left partial project or staging residue");
            require(failed.stage
                    == (point == lingtai::desktop::ProjectCreationFailurePoint::after_staging
                            ? lingtai::desktop::ProjectCreationStage::staging
                        : point == lingtai::desktop::ProjectCreationFailurePoint::after_generation
                            ? lingtai::desktop::ProjectCreationStage::staged_generation
                        : lingtai::desktop::ProjectCreationStage::publication),
                "injected failure lost its typed transaction stage");
        }

        // The asynchronous boundary must preserve both the typed stage and
        // exact safe detail. A no-follow preset rejection is deterministic
        // and fails before the first staging mutation.
        const auto runner_failure_destination = root / "runner-failure";
        fs::create_directories(runner_failure_destination);
        auto runner_request = request_for(
            runner_failure_destination, global, runtime);
        runner_request.preset_path = global / "presets/saved/link.json";
        fs::create_symlink(
            global / "presets/saved/alpha.json", runner_request.preset_path);
        auto runner_delivered = false;
        auto runner_result = lingtai::desktop::ProjectCreationResult{};
        {
            lingtai::desktop::ProjectCreationRunner runner;
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout,
                &loop, &QEventLoop::quit);
            runner.run_create(runner_request, [&](auto result) {
                runner_result = std::move(result);
                runner_delivered = true;
                loop.quit();
            });
            timeout.start(5000);
            loop.exec();
        }
        require(runner_delivered
                && runner_result.stage
                    == lingtai::desktop::ProjectCreationStage::draft_validation
                && runner_result.detail
                    == "selected preset is unreadable, unsafe, oversized, or malformed"
                && !fs::exists(runner_failure_destination / ".lingtai")
                && !has_stage(runner_failure_destination),
            "ProjectCreationRunner lost typed draft failure evidence");
        fs::remove(runner_request.preset_path);

        auto suppressed_callbacks = 0;
        {
            lingtai::desktop::ProjectCreationRunner runner;
            runner.run_catalog(QString::fromStdString(global.string()),
                [&](auto) { ++suppressed_callbacks; });
        }
        QCoreApplication::processEvents();
        require(suppressed_callbacks == 0,
            "catalog callback ran after runner destruction");

        const auto destroyed_runner_destination = root / "runner-destroy";
        fs::create_directories(destroyed_runner_destination);
        write_file(destroyed_runner_destination / "keep.txt", "unrelated\n");
        request = request_for(destroyed_runner_destination, global, runtime);
        request.failure_point =
            lingtai::desktop::ProjectCreationFailurePoint::after_marker_removal;
        {
            lingtai::desktop::ProjectCreationRunner runner;
            runner.run_create(request,
                [&](auto) { ++suppressed_callbacks; });
        }
        QCoreApplication::processEvents();
        require(suppressed_callbacks == 0
                && !fs::exists(destroyed_runner_destination / ".lingtai")
                && !has_stage(destroyed_runner_destination)
                && read_file(destroyed_runner_destination / "keep.txt")
                    == "unrelated\n",
            "create worker destruction raced cleanup or callback suppression");

        const auto outside = root / "outside";
        const auto symlink_destination = root / "destination-link";
        fs::create_directories(outside);
        fs::create_directory_symlink(outside, symlink_destination);
        request = request_for(symlink_destination, global, runtime);
        const auto symlink_refused = lingtai::desktop::create_project(request);
        require(!symlink_refused && !fs::exists(outside / ".lingtai"),
            "destination symlink must not be followed");

        const auto preset_link = global / "presets/saved/link.json";
        fs::create_symlink(global / "presets/saved/alpha.json", preset_link);
        const auto safe_destination = root / "preset-link-project";
        fs::create_directories(safe_destination);
        request = request_for(safe_destination, global, runtime);
        request.preset_path = preset_link;
        const auto preset_refused = lingtai::desktop::create_project(request);
        require(!preset_refused && !fs::exists(safe_destination / ".lingtai"),
            "preset symlink must be rejected before commit");

        fs::remove_all(root, error);
        require(!error, "fixture final cleanup failed");
        std::cout << "project creation contract passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
