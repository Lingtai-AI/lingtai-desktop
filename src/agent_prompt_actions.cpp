#include "agent_prompt_actions.h"
#include "posix_descriptor_primitives.h"

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QLatin1StringView>
#include <QtCore/QRandomGenerator>
#include <QtCore/QString>
#include <QtCore/QTimeZone>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;
namespace posix = posix_internal;

constexpr std::size_t kMaxJsonBytes = std::size_t{64} * 1024;

constexpr std::string_view kInsightQuestion =
    "You are producing insights for the human operator. Based on everything "
    "in this conversation — the task, the context, domain knowledge, risks, "
    "and opportunities — produce exactly 2-3 bullet points of things the "
    "human would benefit from knowing. Each bullet should be a concrete, "
    "specific insight: a pattern you've noticed, relevant background "
    "knowledge, a risk they may not see, or an opportunity to explore. "
    "Format: start each bullet with '- ' on its own line. No preamble, no "
    "summary — just the bullets.";

constexpr std::string_view kExportRecipePrompt =
    "The human wants to export a recipe for sharing. Use the lingtai-recipe "
    "skill (read its SKILL.md first, then assets/export-recipe.md for the "
    "full recipe-export procedure) to walk them through the process. Start "
    "by greeting the human via email and explaining what you're about to do.";

constexpr std::string_view kMoltEn = "[system] molt immediately";
constexpr std::string_view kMoltZh = "[系统] 立即凝蜕";
constexpr std::string_view kMoltWen = "〔系统〕即刻凝蜕";

constexpr std::string_view kGoalInstructions =
    "System events are multiplexed in data.events. For source=goal.request, "
    "read the goal manual under system-manual, then guide the human to "
    "define a goal before writing .notification/goal.json. Dismiss this "
    "request with system(action=\"dismiss\", channel=\"system\", "
    "ref_id=\"<ref_id>\") after handling it.";

constexpr std::string_view kGoalBodyPrefix =
    "Human wants to set or revise an active goal. Read the goal manual under "
    "system-manual before acting. Guide the human to create a goal by "
    "clarifying objective, success criteria, optional "
    "reminder_delay_seconds, and any constraints. Explain clearly that "
    "canceling a goal requires deleting .notification/goal.json or marking "
    "data.status inactive/cancelled/done; dismissing a goal.reminder only "
    "hides that reminder and does not cancel the goal. Do not create or "
    "overwrite .notification/goal.json until the human confirms the goal "
    "details.";

bool open_selected_agent_directory(
        const ProjectAttachment &attachment,
        const fs::path &selected_directory_key,
        posix::FileDescriptor &directory) {
    if (!posix::safe_leaf(selected_directory_key)) return false;
    const auto root = posix::open_root_directory(attachment.root());
    if (root.get() < 0) return false;
    const auto lingtai =
        posix::open_directory_component(root.get(), ".lingtai");
    if (lingtai.get() < 0) return false;
    directory =
        posix::open_directory_component(lingtai.get(), selected_directory_key);
    return directory.get() >= 0;
}

bool leaf_exists(int directory_fd, const char *name) {
    struct stat opened {};
    return ::fstatat(directory_fd, name, &opened, AT_SYMLINK_NOFOLLOW) == 0;
}

bool write_leaf(
        int directory_fd, const char *name, std::string_view bytes) {
    const auto raw_fd = ::openat(directory_fd, name,
        O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK,
        0666);
    if (raw_fd < 0) return false;
    struct stat opened {};
    if (::fstat(raw_fd, &opened) != 0 || !S_ISREG(opened.st_mode)) {
        ::close(raw_fd);
        return false;
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(
            raw_fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            ::close(raw_fd);
            return false;
        }
        if (count == 0) {
            ::close(raw_fd);
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return ::close(raw_fd) == 0;
}

std::string read_leaf(int directory_fd, const char *name) {
    auto file = posix::open_regular_file_component(directory_fd, name);
    if (file.get() < 0) return {};
    struct stat opened {};
    if (::fstat(file.get(), &opened) != 0 || !S_ISREG(opened.st_mode)
            || opened.st_size < 0) {
        return {};
    }
    const auto size = std::min(
        static_cast<std::size_t>(opened.st_size), kMaxJsonBytes);
    std::string bytes(size, '\0');
    std::size_t total = 0;
    while (total < bytes.size()) {
        const auto count =
            ::read(file.get(), bytes.data() + total, bytes.size() - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            return {};
        }
        if (count == 0) break;
        total += static_cast<std::size_t>(count);
    }
    bytes.resize(total);
    return bytes;
}

std::string_view molt_prompt_for(std::string_view language) {
    if (language == "zh") return kMoltZh;
    if (language == "wen") return kMoltWen;
    return kMoltEn;
}

std::string read_language(int agent_fd) {
    const auto bytes = read_leaf(agent_fd, "init.json");
    if (bytes.empty()) return "en";
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(bytes.data(), static_cast<int>(bytes.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return "en";
    }
    const auto object = document.object();
    const auto top = object.value(QLatin1StringView("language")).toString();
    if (!top.isEmpty()) return top.toStdString();
    const auto nested = object.value(QLatin1StringView("manifest")).toObject()
        .value(QLatin1StringView("language")).toString();
    if (!nested.isEmpty()) return nested.toStdString();
    return "en";
}

std::string goal_body(std::string_view human_request) {
    auto body = std::string(kGoalBodyPrefix);
    if (human_request.empty()) {
        body += " No inline goal text was provided; ask the human what goal "
            "they want to create.";
    } else {
        body += " Human request: ";
        body += human_request;
    }
    return body;
}

} // namespace

AgentPromptWriteResult write_agent_prompt(
        const ProjectAttachment &attachment,
        const fs::path &selected_directory_key,
        std::string_view content) noexcept {
    try {
        posix::FileDescriptor agent;
        if (!open_selected_agent_directory(
                attachment, selected_directory_key, agent)) {
            return AgentPromptWriteResult::failed_local;
        }
        return write_leaf(agent.get(), ".prompt", content)
            ? AgentPromptWriteResult::written
            : AgentPromptWriteResult::failed_local;
    } catch (...) {
        return AgentPromptWriteResult::failed_local;
    }
}

AgentPromptWriteResult write_agent_inquiry(
        const ProjectAttachment &attachment,
        const fs::path &selected_directory_key,
        std::string_view source,
        std::string_view question) noexcept {
    try {
        posix::FileDescriptor agent;
        if (!open_selected_agent_directory(
                attachment, selected_directory_key, agent)) {
            return AgentPromptWriteResult::failed_local;
        }
        if (leaf_exists(agent.get(), ".inquiry")
                || leaf_exists(agent.get(), ".inquiry.taken")) {
            return AgentPromptWriteResult::already_pending;
        }
        auto content = std::string(source);
        content += '\n';
        content += question;
        return write_leaf(agent.get(), ".inquiry", content)
            ? AgentPromptWriteResult::written
            : AgentPromptWriteResult::failed_local;
    } catch (...) {
        return AgentPromptWriteResult::failed_local;
    }
}

AgentPromptWriteResult write_insight_inquiry(
        const ProjectAttachment &attachment,
        const fs::path &selected_directory_key) noexcept {
    return write_agent_inquiry(
        attachment, selected_directory_key, "insight", kInsightQuestion);
}

AgentPromptWriteResult write_molt_prompt(
        const ProjectAttachment &attachment,
        const fs::path &selected_directory_key) noexcept {
    try {
        posix::FileDescriptor agent;
        if (!open_selected_agent_directory(
                attachment, selected_directory_key, agent)) {
            return AgentPromptWriteResult::failed_local;
        }
        const auto language = read_language(agent.get());
        return write_leaf(agent.get(), ".prompt", molt_prompt_for(language))
            ? AgentPromptWriteResult::written
            : AgentPromptWriteResult::failed_local;
    } catch (...) {
        return AgentPromptWriteResult::failed_local;
    }
}

AgentPromptWriteResult write_export_recipe_prompt(
        const ProjectAttachment &attachment,
        const fs::path &selected_directory_key) noexcept {
    return write_agent_prompt(
        attachment, selected_directory_key, kExportRecipePrompt);
}

AgentGoalRequestResult write_agent_goal_request(
        const ProjectAttachment &attachment,
        const fs::path &selected_directory_key,
        std::string_view human_request) noexcept {
    AgentGoalRequestResult result;
    try {
        posix::FileDescriptor agent;
        if (!open_selected_agent_directory(
                attachment, selected_directory_key, agent)) {
            return result;
        }
        auto notification = posix::open_directory_component(
            agent.get(), ".notification", true);
        if (notification.get() < 0) return result;

        QJsonObject payload;
        const auto existing = read_leaf(notification.get(), "system.json");
        if (!existing.empty()) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(
                QByteArray(existing.data(), static_cast<int>(existing.size())),
                &error);
            if (error.error == QJsonParseError::NoError && document.isObject()) {
                payload = document.object();
            }
        }

        auto data = payload.value(QLatin1StringView("data")).toObject();
        auto events = data.value(QLatin1StringView("events")).toArray();
        QJsonArray kept;
        for (const auto &entry : events) {
            if (!entry.isNull()) kept.push_back(entry);
        }

        const auto now = std::chrono::system_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        const auto entropy = QRandomGenerator::global()->generate();
        const auto event_id = QStringLiteral("evt_%1_%2")
            .arg(ms, 0, 16)
            .arg(entropy & 0xffff, 4, 16, QLatin1Char('0'));
        const auto ref_id = QStringLiteral("goal.request:%1").arg(ms, 0, 16);
        const auto at = QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::utc())
            .toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ"));

        QJsonObject event;
        event.insert(QStringLiteral("event_id"), event_id);
        event.insert(QStringLiteral("source"), QStringLiteral("goal.request"));
        event.insert(QStringLiteral("ref_id"), ref_id);
        event.insert(QStringLiteral("body"),
            QString::fromStdString(goal_body(human_request)));
        event.insert(QStringLiteral("at"), at);
        kept.push_back(event);
        while (kept.size() > 20) kept.removeAt(0);

        data.insert(QStringLiteral("events"), kept);
        payload.insert(QStringLiteral("data"), data);
        const auto count = kept.size();
        payload.insert(QStringLiteral("header"),
            QStringLiteral("%1 system notification%2")
                .arg(count)
                .arg(count == 1 ? QString() : QStringLiteral("s")));
        payload.insert(QStringLiteral("icon"), QStringLiteral("🔔"));
        payload.insert(QStringLiteral("priority"), QStringLiteral("normal"));
        payload.insert(QStringLiteral("published_at"), at);
        payload.insert(QStringLiteral("instructions"),
            QString::fromUtf8(kGoalInstructions.data(),
                static_cast<qsizetype>(kGoalInstructions.size())));

        const auto encoded = QJsonDocument(payload).toJson();
        if (!write_leaf(notification.get(), "system.json.tmp",
                std::string_view(encoded.constData(),
                    static_cast<std::size_t>(encoded.size())))) {
            return result;
        }
        if (::renameat(notification.get(), "system.json.tmp",
                notification.get(), "system.json") != 0) {
            return result;
        }
        result.ok = true;
        result.event_id = event_id.toStdString();
        return result;
    } catch (...) {
        return result;
    }
}

} // namespace lingtai::desktop
