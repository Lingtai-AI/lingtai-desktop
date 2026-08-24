#include "injected_mail_journal.h"

#include "posix_descriptor_primitives.h"

#include <QtCore/QByteArrayView>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>

#include <algorithm>
#include <cerrno>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace lingtai::desktop {
namespace {

namespace posix = posix_internal;

constexpr auto kEventType = "notification_block_injected";
constexpr off_t kMaxPollBytes = 1 << 20;

void collect_ids_from_email_object(
        const QJsonObject &email, std::unordered_set<std::string> &ids) {
    const auto raw_ids = email.value(QLatin1StringView("email_ids"));
    if (raw_ids.isArray()) {
        for (const auto &entry : raw_ids.toArray()) {
            if (!entry.isString()) continue;
            auto id = entry.toString().toStdString();
            if (!id.empty()) ids.insert(std::move(id));
        }
    }
    const auto emails = email.value(QLatin1StringView("emails"));
    if (!emails.isArray()) return;
    for (const auto &entry : emails.toArray()) {
        if (!entry.isObject()) continue;
        const auto id = entry.toObject().value(QLatin1StringView("id"));
        if (!id.isString()) continue;
        auto text = id.toString().toStdString();
        if (!text.empty()) ids.insert(std::move(text));
    }
}

[[nodiscard]] QJsonObject object_field(
        const QJsonObject &parent, const char *key) {
    const auto value = parent.value(QLatin1StringView(key));
    return value.isObject() ? value.toObject() : QJsonObject();
}

} // namespace

void collect_injected_mail_ids_from_event_json(
        std::string_view json_line, std::unordered_set<std::string> &ids) {
    if (json_line.empty()) return;
    QJsonParseError error;
    const auto value = QJsonValue::fromJson(
        QByteArrayView(json_line.data(),
            static_cast<qsizetype>(json_line.size())),
        &error);
    if (error.error != QJsonParseError::NoError || !value.isObject()) return;
    const auto event = value.toObject();
    if (event.value(QLatin1StringView("type")).toString()
            != QLatin1StringView(kEventType)) {
        return;
    }

    const auto persistent_email = object_field(
        object_field(
            object_field(
                object_field(
                    object_field(event, "_meta"), "agent_meta"),
                "notifications"),
            "persistent"),
        "email");
    collect_ids_from_email_object(persistent_email, ids);

    if (!persistent_email.isEmpty()) return;

    // Older injected envelopes carried unread mail under payload.notifications
    // instead of the persistent lane; the event type still means the model saw
    // that block.
    const auto payload_email_data = object_field(
        object_field(
            object_field(
                object_field(event, "payload"), "notifications"),
            "email"),
        "data");
    collect_ids_from_email_object(payload_email_data, ids);
}

void InjectedMailJournal::reset() noexcept {
    const auto changed = !ids_.empty();
    anchored_ = false;
    inode_ = 0;
    offset_ = 0;
    carry_.clear();
    ids_.clear();
    if (changed) ++revision_;
}

void InjectedMailJournal::poll(
        const std::filesystem::path &project_root,
        const std::filesystem::path &target_directory_key) {
    if (project_root.empty() || target_directory_key.empty()
            || target_directory_key.has_root_path()
            || !target_directory_key.parent_path().empty()
            || target_directory_key == "." || target_directory_key == "..") {
        return;
    }

    const auto root = posix::open_root_directory(project_root);
    if (root.get() < 0) return;
    const auto lingtai = posix::open_directory_component(root.get(), ".lingtai");
    if (lingtai.get() < 0) return;
    const auto agent = posix::open_directory_component(
        lingtai.get(), target_directory_key);
    if (agent.get() < 0) return;
    const auto logs = posix::open_directory_component(agent.get(), "logs");
    if (logs.get() < 0) return;
    const auto file = posix::open_regular_file_component(logs.get(), "events.jsonl");
    if (file.get() < 0) return;

    struct stat opened {};
    if (::fstat(file.get(), &opened) != 0) return;
    if (!S_ISREG(opened.st_mode)) return;

    if (!anchored_ || inode_ != opened.st_ino) {
        anchored_ = true;
        inode_ = opened.st_ino;
        offset_ = opened.st_size;
        carry_.clear();
        return;
    }
    if (opened.st_size < offset_) {
        offset_ = 0;
        carry_.clear();
    }
    if (opened.st_size == offset_ && carry_.empty()) return;

    if (::lseek(file.get(), offset_, SEEK_SET) < 0) return;

    const auto want = static_cast<std::size_t>(
        std::min<off_t>(opened.st_size - offset_, kMaxPollBytes));
    std::string chunk(want, '\0');
    std::size_t total = 0;
    while (total < chunk.size()) {
        const auto count = ::read(
            file.get(), chunk.data() + total, chunk.size() - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (count == 0) break;
        total += static_cast<std::size_t>(count);
    }
    chunk.resize(total);
    offset_ += static_cast<off_t>(total);

    auto text = std::move(carry_);
    text.append(chunk);
    carry_.clear();
    const auto ids_before = ids_.size();
    std::size_t line_start = 0;
    for (std::size_t index = 0; index != text.size(); ++index) {
        if (text[index] != '\n') continue;
        collect_injected_mail_ids_from_event_json(
            std::string_view(text).substr(line_start, index - line_start),
            ids_);
        line_start = index + 1;
    }
    if (line_start < text.size()) {
        carry_ = text.substr(line_start);
    }
    if (ids_.size() != ids_before) ++revision_;
}

} // namespace lingtai::desktop
