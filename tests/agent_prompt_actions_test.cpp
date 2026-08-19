#include "agent_prompt_actions.h"
#include "project_attachment.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::AgentGoalRequestResult;
using lingtai::desktop::AgentPromptWriteResult;
using lingtai::desktop::ProjectAttachment;
using lingtai::desktop::attach_project;
using lingtai::desktop::write_agent_goal_request;
using lingtai::desktop::write_agent_inquiry;
using lingtai::desktop::write_agent_prompt;
using lingtai::desktop::write_export_recipe_prompt;
using lingtai::desktop::write_insight_inquiry;
using lingtai::desktop::write_molt_prompt;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void write_file(const fs::path &path, std::string_view bytes) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    require(!error, "fixture parent must be created: " + path.string());
    auto stream = std::ofstream(path, std::ios::binary);
    stream << bytes;
    require(stream.good(), "fixture must be written: " + path.string());
}

std::string read_file(const fs::path &path) {
    auto stream = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

ProjectAttachment attach(const fs::path &project) {
    auto attached = attach_project(project);
    require(static_cast<bool>(attached),
        "project attachment must succeed: " + project.string());
    return std::move(*attached.attachment);
}

void verify_prompt_and_inquiry(const fs::path &sandbox) {
    const auto project = sandbox / "prompt";
    write_file(project / ".lingtai/alpha/.agent.json", R"({"admin":{}})");
    write_file(project / ".lingtai/beta/.agent.json", R"({"admin":{}})");
    const auto attachment = attach(project);
    const auto prompt = write_agent_prompt(attachment, "alpha", "hello");
    require(prompt == AgentPromptWriteResult::written,
        "a prompt write against an existing Agent directory must succeed");
    require(read_file(project / ".lingtai/alpha/.prompt") == "hello",
        "the selected Agent must receive the exact .prompt bytes");
    require(!fs::exists(project / ".lingtai/beta/.prompt"),
        "an unselected sibling must never gain a .prompt");

    const auto first = write_agent_inquiry(
        attachment, "alpha", "human", "side question");
    require(first == AgentPromptWriteResult::written,
        "the first inquiry must write .inquiry");
    require(read_file(project / ".lingtai/alpha/.inquiry")
            == "human\nside question",
        "an inquiry must be source then a newline then the question");

    const auto second = write_insight_inquiry(attachment, "alpha");
    require(second == AgentPromptWriteResult::already_pending,
        "a second inquiry must be a one-at-a-time no-op");
    require(read_file(project / ".lingtai/alpha/.inquiry")
            == "human\nside question",
        "a pending inquiry must keep the original .inquiry bytes");

    require(write_agent_prompt(attachment, "never-existed", "x")
            == AgentPromptWriteResult::failed_local,
        "a missing Agent directory must fail rather than be created");
}

void verify_molt_language_and_export(const fs::path &sandbox) {
    const auto project = sandbox / "molt";
    write_file(project / ".lingtai/alpha/.agent.json", R"({"admin":{}})");
    write_file(project / ".lingtai/alpha/init.json",
        R"({"manifest":{"language":"zh"}})");
    const auto attachment = attach(project);
    require(write_molt_prompt(attachment, "alpha")
            == AgentPromptWriteResult::written,
        "molt must write the localized prompt");
    require(read_file(project / ".lingtai/alpha/.prompt")
            == "[系统] 立即凝蜕",
        "zh molt must use the TUI Chinese mandatory prompt");
    require(write_export_recipe_prompt(attachment, "alpha")
            == AgentPromptWriteResult::written,
        "export must overwrite .prompt with the recipe prompt");
    require(read_file(project / ".lingtai/alpha/.prompt")
                .find("lingtai-recipe") != std::string::npos,
        "export must ask the Agent to use the lingtai-recipe skill");
}

void verify_goal_request(const fs::path &sandbox) {
    const auto project = sandbox / "goal";
    write_file(project / ".lingtai/alpha/.agent.json", R"({"admin":{}})");
    const auto attachment = attach(project);
    const auto first = write_agent_goal_request(
        attachment, "alpha", "finish the linked /goal PR");
    require(first.ok && first.event_id.find("evt_") == 0,
        "a goal request must return a TUI-shaped event id");
    const auto payload = QJsonDocument::fromJson(
        QByteArray::fromStdString(
            read_file(project / ".lingtai/alpha/.notification/system.json")))
        .object();
    const auto events =
        payload.value(QStringLiteral("data")).toObject()
            .value(QStringLiteral("events")).toArray();
    require(events.size() == 1, "the first goal write must append one event");
    const auto event = events.at(0).toObject();
    require(event.value(QStringLiteral("source")).toString()
            == QStringLiteral("goal.request"),
        "the event source must be goal.request");
    require(event.value(QStringLiteral("body")).toString()
            .contains(QStringLiteral("finish the linked /goal PR")),
        "the event body must retain the human request");
    require(payload.value(QStringLiteral("instructions")).toString()
            .contains(QStringLiteral("source=goal.request")),
        "system.json must carry the TUI goal-request instructions");

    QJsonArray seed;
    for (auto index = 0; index != 20; ++index) {
        QJsonObject old;
        old.insert(QStringLiteral("ref_id"),
            QStringLiteral("old-%1").arg(index));
        old.insert(QStringLiteral("source"), QStringLiteral("daemon.done"));
        seed.push_back(old);
    }
    QJsonObject seeded;
    QJsonObject data;
    data.insert(QStringLiteral("events"), seed);
    data.insert(QStringLiteral("other"), QStringLiteral("preserved"));
    seeded.insert(QStringLiteral("data"), data);
    write_file(project / ".lingtai/alpha/.notification/system.json",
        QJsonDocument(seeded).toJson().toStdString());
    const auto capped = write_agent_goal_request(attachment, "alpha", "new goal");
    require(capped.ok, "a follow-up goal request must succeed");
    const auto again = QJsonDocument::fromJson(
        QByteArray::fromStdString(
            read_file(project / ".lingtai/alpha/.notification/system.json")))
        .object();
    const auto kept = again.value(QStringLiteral("data")).toObject()
        .value(QStringLiteral("events")).toArray();
    require(kept.size() == 20, "goal events must cap at the TUI 20-event window");
    require(kept.at(0).toObject().value(QStringLiteral("ref_id")).toString()
            == QStringLiteral("old-1"),
        "the oldest seeded event must drop when the cap is exceeded");
    require(kept.at(19).toObject().value(QStringLiteral("source")).toString()
            == QStringLiteral("goal.request"),
        "the newest event must be the goal request");
    require(again.value(QStringLiteral("data")).toObject()
            .value(QStringLiteral("other")).toString()
            == QStringLiteral("preserved"),
        "unrelated data fields must be preserved");
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: lingtai_agent_prompt_actions_test <sandbox>\n";
        return 2;
    }
    try {
        const auto sandbox = fs::path(argv[1]);
        std::error_code error;
        fs::remove_all(sandbox, error);
        fs::create_directories(sandbox, error);
        require(!error, "sandbox must be created");
        verify_prompt_and_inquiry(sandbox);
        verify_molt_language_and_export(sandbox);
        verify_goal_request(sandbox);
    } catch (const std::exception &ex) {
        std::cerr << "FAIL: " << ex.what() << '\n';
        return 1;
    }
    std::cout << "agent prompt actions: OK\n";
    return 0;
}
