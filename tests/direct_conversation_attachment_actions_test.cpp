#include "direct_conversation_attachment_actions.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using lingtai::desktop::DirectConversationAttachmentRequest;
using lingtai::desktop::DirectConversationRoute;
using lingtai::desktop::read_direct_conversation;
using lingtai::desktop::revalidate_direct_conversation_attachment;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void write_file(const fs::path &path, std::string_view bytes) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    require(!error, "fixture directory must be created");
    auto stream = std::ofstream(path, std::ios::binary);
    stream << bytes;
    require(stream.good(), "fixture file must be written");
}

DirectConversationRoute route_for(const fs::path &project) {
    return {
        .human_directory_key = "human",
        .target_directory_key = "agent",
        .human_address = "human",
        .target_address = "agent",
        .project_root = project,
        .target_agent_id = "agent-id",
    };
}

fs::path attachment_path(const fs::path &project) {
    return project / ".lingtai/human/mailbox/inbox/message-1/attachments/report.txt";
}

void prepare(const fs::path &project) {
    write_file(attachment_path(project), "report-v1");
    write_file(
        project / ".lingtai/human/mailbox/inbox/message-1/message.json",
        R"({"from":"agent","to":["human"],"message":"See report.",)"
        R"("received_at":"2026-08-24T10:00:00Z",)"
        R"("identity":{"agent_id":"agent-id"},)"
        R"("attachments":["/stale/parent/report.txt"]})");
}

DirectConversationAttachmentRequest request_for(const fs::path &project) {
    const auto history = read_direct_conversation(route_for(project));
    require(history.messages.size() == 1
            && history.messages[0].attachments.size() == 1,
        "fixture must project one message attachment");
    return {
        history.messages[0].id,
        0,
        history.messages[0].attachments[0],
    };
}

void verify_success_and_fail_closed_cases(const fs::path &sandbox) {
    const auto route = route_for(sandbox);
    prepare(sandbox);
    const auto request = request_for(sandbox);
    require(revalidate_direct_conversation_attachment(route, request)
            == attachment_path(sandbox).lexically_normal(),
        "unchanged current-entry-relative regular file must revalidate");

    auto wrong_identity = request;
    ++wrong_identity.presented.inode_id;
    require(!revalidate_direct_conversation_attachment(route, wrong_identity),
        "presentation identity mismatch must fail closed");

    fs::remove(attachment_path(sandbox));
    require(!revalidate_direct_conversation_attachment(route, request),
        "missing file must fail closed");

    write_file(attachment_path(sandbox), "replacement");
    require(!revalidate_direct_conversation_attachment(route, request),
        "replacement inode/size must fail closed");

    fs::remove(attachment_path(sandbox));
    std::error_code error;
    fs::create_symlink("outside.txt", attachment_path(sandbox), error);
    require(!error, "symlink fixture must be created");
    require(!revalidate_direct_conversation_attachment(route, request),
        "symlink must fail closed");

    fs::remove(attachment_path(sandbox));
    fs::create_directory(attachment_path(sandbox), error);
    require(!error, "directory fixture must be created");
    require(!revalidate_direct_conversation_attachment(route, request),
        "non-regular file must fail closed");

    auto escape = request;
    escape.message_id = "../message-1";
    require(!revalidate_direct_conversation_attachment(route, escape),
        "escaping entry id must fail closed");
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected sandbox path");
        const auto sandbox = fs::path(argv[1]);
        std::error_code error;
        fs::remove_all(sandbox, error);
        fs::create_directories(sandbox, error);
        require(!error, "sandbox must be clean");
        verify_success_and_fail_closed_cases(sandbox);
        fs::remove_all(sandbox, error);
        std::cout << "direct_conversation_attachment_actions: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
