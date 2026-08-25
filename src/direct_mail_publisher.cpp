#include "direct_mail_publisher.h"
#include "posix_descriptor_primitives.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <optional>
#include <random>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;
namespace posix = posix_internal;

constexpr auto kIdCollisionRetries = 8;

struct StampedId {
    std::string directory_id;
    std::string received_at;
};

struct PreparedAttachment {
    fs::path sent_path;
    DirectMailPublishedAttachment published;
};

[[nodiscard]] DirectMailSendOutcome failure(
        DirectMailFailureReason reason,
        std::error_code error = {}) {
    DirectMailSendOutcome outcome;
    outcome.failure_reason = reason;
    outcome.system_error = error;
    return outcome;
}

[[nodiscard]] DirectMailSendOutcome attachment_failure(
        DirectMailFailureReason reason,
        std::size_t index,
        const fs::path &source_path,
        std::error_code error = {}) {
    auto outcome = failure(reason, error);
    outcome.attachment_failure = DirectMailAttachmentFailure{
        index,
        source_path,
    };
    return outcome;
}

[[nodiscard]] std::error_code last_error() {
    return {errno, std::generic_category()};
}

[[nodiscard]] std::optional<StampedId> fresh_id() {
    const auto seconds = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
    if (::gmtime_r(&seconds, &utc) == nullptr) return std::nullopt;

    std::array<char, 16> id_stamp{};
    if (std::strftime(id_stamp.data(), id_stamp.size(), "%Y%m%dT%H%M%S", &utc)
        == 0) {
        return std::nullopt;
    }
    std::array<char, 21> rfc_stamp{};
    if (std::strftime(
            rfc_stamp.data(), rfc_stamp.size(), "%Y-%m-%dT%H:%M:%SZ", &utc)
        == 0) {
        return std::nullopt;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::random_device entropy;
    std::uniform_int_distribution<unsigned> nibble(0, 15);
    std::array<char, 4> suffix{};
    for (auto &digit : suffix) digit = kHex[nibble(entropy)];

    return StampedId{
        std::string(id_stamp.data()) + "-"
            + std::string(suffix.data(), suffix.size()),
        std::string(rfc_stamp.data()),
    };
}

[[nodiscard]] QJsonObject identity_object(const HumanSenderIdentity &identity) {
    QJsonObject object;
    if (identity.agent_id) {
        object["agent_id"] = QString::fromStdString(*identity.agent_id);
    }
    if (identity.true_name) {
        object["agent_name"] = QString::fromStdString(*identity.true_name);
    }
    if (identity.nickname) {
        object["nickname"] = QString::fromStdString(*identity.nickname);
    }
    if (identity.address) {
        object["address"] = QString::fromStdString(*identity.address);
    }
    if (identity.state) {
        object["state"] = QString::fromStdString(*identity.state);
    }
    return object;
}

[[nodiscard]] std::string build_payload(
        const DirectConversationRoute &route,
        const std::string &text,
        const StampedId &stamp,
        const std::vector<PreparedAttachment> &attachments) {
    QJsonObject payload;
    payload["id"] = QString::fromStdString(stamp.directory_id);
    payload["_mailbox_id"] = QString::fromStdString(stamp.directory_id);
    payload["from"] = QString::fromStdString(route.human_address);
    payload["to"] = QJsonArray{QString::fromStdString(route.target_address)};
    payload["cc"] = QJsonArray{};
    payload["subject"] = QString();
    payload["message"] = QString::fromStdString(text);
    payload["type"] = QStringLiteral("normal");
    payload["received_at"] = QString::fromStdString(stamp.received_at);
    if (const auto identity = identity_object(route.human_identity);
        !identity.isEmpty()) {
        payload["identity"] = identity;
    }
    if (!attachments.empty()) {
        QJsonArray paths;
        for (const auto &attachment : attachments) {
            paths.append(QString::fromStdString(attachment.sent_path.string()));
        }
        payload["attachments"] = paths;
    }
    return QJsonDocument(payload).toJson(QJsonDocument::Compact).toStdString();
}

[[nodiscard]] bool write_all(int descriptor, const char *bytes, std::size_t size) {
    std::size_t total = 0;
    while (total < size) {
        const auto count = ::write(descriptor, bytes + total, size - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) {
            errno = EIO;
            return false;
        }
        total += static_cast<std::size_t>(count);
    }
    return true;
}

[[nodiscard]] bool write_leaf_json(int leaf_fd, const std::string &bytes) {
    const auto raw_fd = ::openat(leaf_fd, "message.json.tmp",
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (raw_fd < 0) return false;
    auto ok = write_all(raw_fd, bytes.data(), bytes.size());
    if (::close(raw_fd) != 0) ok = false;
    if (!ok) return false;
    return ::renameat(leaf_fd, "message.json.tmp", leaf_fd, "message.json") == 0;
}

[[nodiscard]] std::string disambiguated_name(
        const std::string &original,
        std::set<std::string> &used_names) {
    if (used_names.insert(original).second) return original;
    const auto path = fs::path(original);
    const auto stem = path.stem().string();
    const auto extension = path.extension().string();
    for (std::size_t suffix = 1;; ++suffix) {
        const auto candidate = stem + "-" + std::to_string(suffix) + extension;
        if (used_names.insert(candidate).second) return candidate;
    }
}

[[nodiscard]] DirectMailFailureReason open_failure_reason(int error) {
    if (error == ENOENT || error == ENOTDIR) {
        return DirectMailFailureReason::attachment_missing;
    }
    if (error == EACCES || error == EPERM) {
        return DirectMailFailureReason::attachment_unreadable;
    }
    if (error == ELOOP) return DirectMailFailureReason::attachment_symlink;
    return DirectMailFailureReason::attachment_invalid_source;
}

void rollback_leaf(
        int outbox_fd,
        int leaf_fd,
        const std::string &directory_id,
        const std::vector<std::string> &copied_names,
        bool attachments_directory_created) {
    ::unlinkat(leaf_fd, "message.json.tmp", 0);
    ::unlinkat(leaf_fd, "message.json", 0);
    if (attachments_directory_created) {
        const auto attachments = posix::open_directory_component(
            leaf_fd, "attachments");
        if (attachments.get() >= 0) {
            for (const auto &name : copied_names) {
                ::unlinkat(attachments.get(), name.c_str(), 0);
            }
        }
        ::unlinkat(leaf_fd, "attachments", AT_REMOVEDIR);
    }
    ::unlinkat(outbox_fd, directory_id.c_str(), AT_REMOVEDIR);
}

[[nodiscard]] DirectMailSendOutcome copy_one_attachment(
        int attachments_fd,
        const AcceptedAttachment &attachment,
        std::size_t index,
        const std::string &destination_name,
        std::uint64_t measured_total,
        std::uint64_t &measured_size_out,
        DirectMailPublishedAttachment &published_out) {
    if (!attachment.source_path.is_absolute()) {
        return attachment_failure(
            DirectMailFailureReason::attachment_invalid_source,
            index, attachment.source_path);
    }

    const auto raw_source = ::open(attachment.source_path.c_str(),
        O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (raw_source < 0) {
        const auto error = last_error();
        return attachment_failure(open_failure_reason(error.value()),
            index, attachment.source_path, error);
    }

    struct stat status {};
    if (::fstat(raw_source, &status) != 0) {
        const auto error = last_error();
        ::close(raw_source);
        return attachment_failure(
            DirectMailFailureReason::attachment_invalid_source,
            index, attachment.source_path, error);
    }
    if (!S_ISREG(status.st_mode)) {
        ::close(raw_source);
        return attachment_failure(
            DirectMailFailureReason::attachment_not_regular,
            index, attachment.source_path);
    }
    if (status.st_size < 0) {
        ::close(raw_source);
        return attachment_failure(
            DirectMailFailureReason::attachment_invalid_source,
            index, attachment.source_path,
            std::make_error_code(std::errc::value_too_large));
    }
    if (static_cast<std::uint64_t>(status.st_dev) != attachment.device_id
            || static_cast<std::uint64_t>(status.st_ino) != attachment.inode_id) {
        ::close(raw_source);
        return attachment_failure(
            DirectMailFailureReason::attachment_replaced,
            index, attachment.source_path);
    }
    const auto measured_size = static_cast<std::uint64_t>(status.st_size);
    if (measured_size > kAttachmentPerFileLimitBytes) {
        ::close(raw_source);
        return attachment_failure(
            DirectMailFailureReason::attachment_per_file_limit,
            index, attachment.source_path);
    }
    if (measured_size > kAttachmentTotalLimitBytes - measured_total) {
        ::close(raw_source);
        return attachment_failure(
            DirectMailFailureReason::attachment_total_limit,
            index, attachment.source_path);
    }
    if (measured_size != attachment.byte_size) {
        ::close(raw_source);
        return attachment_failure(
            DirectMailFailureReason::attachment_size_changed,
            index, attachment.source_path);
    }

    const auto raw_destination = ::openat(attachments_fd,
        destination_name.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (raw_destination < 0) {
        const auto error = last_error();
        ::close(raw_source);
        return attachment_failure(
            DirectMailFailureReason::attachment_destination_failure,
            index, attachment.source_path, error);
    }

    auto result = DirectMailSendOutcome{
        .result = DirectMailSendResult::queued,
        .failure_reason = DirectMailFailureReason::none,
    };
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t copied = 0;
    while (copied < measured_size) {
        const auto remaining = measured_size - copied;
        const auto request = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), remaining));
        const auto count = ::read(raw_source, buffer.data(), request);
        if (count < 0) {
            if (errno == EINTR) continue;
            result = attachment_failure(
                DirectMailFailureReason::attachment_copy_failure,
                index, attachment.source_path, last_error());
            break;
        }
        if (count == 0) {
            result = attachment_failure(
                DirectMailFailureReason::attachment_size_changed,
                index, attachment.source_path);
            break;
        }
        if (!write_all(raw_destination, buffer.data(),
                static_cast<std::size_t>(count))) {
            result = attachment_failure(
                DirectMailFailureReason::attachment_copy_failure,
                index, attachment.source_path, last_error());
            break;
        }
        copied += static_cast<std::uint64_t>(count);
    }
    if (result.result == DirectMailSendResult::queued) {
        char extra = 0;
        auto count = ::read(raw_source, &extra, 1);
        while (count < 0 && errno == EINTR) count = ::read(raw_source, &extra, 1);
        if (count != 0) {
            result = attachment_failure(
                count < 0
                    ? DirectMailFailureReason::attachment_copy_failure
                    : DirectMailFailureReason::attachment_size_changed,
                index, attachment.source_path,
                count < 0 ? last_error() : std::error_code{});
        }
    }
    struct stat destination_status {};
    if (result.result == DirectMailSendResult::queued
            && ::fstat(raw_destination, &destination_status) != 0) {
        result = attachment_failure(
            DirectMailFailureReason::attachment_copy_failure,
            index, attachment.source_path, last_error());
    }
    if (::close(raw_destination) != 0
            && result.result == DirectMailSendResult::queued) {
        result = attachment_failure(
            DirectMailFailureReason::attachment_copy_failure,
            index, attachment.source_path, last_error());
    }
    if (::close(raw_source) != 0
            && result.result == DirectMailSendResult::queued) {
        result = attachment_failure(
            DirectMailFailureReason::attachment_copy_failure,
            index, attachment.source_path, last_error());
    }
    if (result.result == DirectMailSendResult::queued) {
        measured_size_out = measured_size;
        published_out = {
            .display_filename = destination_name,
            .byte_size = measured_size,
            .media_kind = attachment.media_kind,
            .device_id = static_cast<std::uint64_t>(destination_status.st_dev),
            .inode_id = static_cast<std::uint64_t>(destination_status.st_ino),
        };
    }
    return result;
}

} // namespace

DirectMailSendOutcome send_direct_mail(
        const DirectConversationRoute &route,
        const std::string &text,
        const std::vector<AcceptedAttachment> &attachments) noexcept {
    if (text.empty() && attachments.empty()) {
        return failure(DirectMailFailureReason::empty_content);
    }
    try {
        const auto &key = route.human_directory_key;
        if (key.empty() || key.has_root_path() || !key.parent_path().empty()
            || key == "." || key == "..") {
            return failure(DirectMailFailureReason::unsafe_route);
        }

        const auto root = posix::open_root_directory(route.project_root);
        if (root.get() < 0) {
            return failure(DirectMailFailureReason::mailbox_unavailable,
                last_error());
        }
        const auto lingtai = posix::open_directory_component(root.get(), ".lingtai");
        if (lingtai.get() < 0) {
            return failure(DirectMailFailureReason::mailbox_unavailable,
                last_error());
        }
        const auto human = posix::open_directory_component(lingtai.get(), key);
        if (human.get() < 0) {
            return failure(DirectMailFailureReason::mailbox_unavailable,
                last_error());
        }
        const auto mailbox = posix::open_directory_component(
            human.get(), "mailbox", /*create=*/true);
        if (mailbox.get() < 0) {
            return failure(DirectMailFailureReason::mailbox_unavailable,
                last_error());
        }
        const auto outbox = posix::open_directory_component(
            mailbox.get(), "outbox", /*create=*/true);
        if (outbox.get() < 0) {
            return failure(DirectMailFailureReason::mailbox_unavailable,
                last_error());
        }

        for (auto attempt = 0; attempt != kIdCollisionRetries; ++attempt) {
            const auto stamp = fresh_id();
            if (!stamp) return failure(DirectMailFailureReason::local_failure);
            if (::mkdirat(outbox.get(), stamp->directory_id.c_str(), 0700) != 0) {
                if (errno == EEXIST) continue;
                return failure(DirectMailFailureReason::mailbox_unavailable,
                    last_error());
            }
            const auto leaf = posix::open_directory_component(
                outbox.get(), stamp->directory_id);
            if (leaf.get() < 0) {
                const auto error = last_error();
                ::unlinkat(outbox.get(), stamp->directory_id.c_str(),
                    AT_REMOVEDIR);
                return failure(DirectMailFailureReason::mailbox_unavailable,
                    error);
            }

            std::vector<std::string> copied_names;
            auto attachments_directory_created = false;
            try {
                std::vector<PreparedAttachment> prepared;
                prepared.reserve(attachments.size());
                copied_names.reserve(attachments.size());
                std::set<std::string> used_names;
                std::uint64_t measured_total = 0;

                posix::FileDescriptor attachments_directory;
                if (!attachments.empty()) {
                    if (::mkdirat(leaf.get(), "attachments", 0700) != 0) {
                        const auto outcome = attachment_failure(
                            DirectMailFailureReason::attachment_destination_failure,
                            0, attachments[0].source_path, last_error());
                        rollback_leaf(outbox.get(), leaf.get(), stamp->directory_id,
                            copied_names, attachments_directory_created);
                        return outcome;
                    }
                    attachments_directory_created = true;
                    attachments_directory = posix::open_directory_component(
                        leaf.get(), "attachments");
                    if (attachments_directory.get() < 0) {
                        const auto outcome = attachment_failure(
                            DirectMailFailureReason::attachment_destination_failure,
                            0, attachments[0].source_path, last_error());
                        rollback_leaf(outbox.get(), leaf.get(), stamp->directory_id,
                            copied_names, attachments_directory_created);
                        return outcome;
                    }
                }

                for (std::size_t index = 0; index != attachments.size(); ++index) {
                    const auto &attachment = attachments[index];
                    const auto display_path = fs::path(attachment.display_filename);
                    if (!posix::safe_leaf(display_path)) {
                        const auto outcome = attachment_failure(
                            DirectMailFailureReason::unsafe_attachment_name,
                            index, attachment.source_path);
                        rollback_leaf(outbox.get(), leaf.get(), stamp->directory_id,
                            copied_names, attachments_directory_created);
                        return outcome;
                    }
                    const auto destination_name = disambiguated_name(
                        attachment.display_filename, used_names);

                    std::uint64_t current_size = 0;
                    auto published = DirectMailPublishedAttachment();
                    const auto copy = copy_one_attachment(
                        attachments_directory.get(), attachment, index,
                        destination_name, measured_total, current_size,
                        published);
                    if (copy.result != DirectMailSendResult::queued) {
                        // The destination may have been created before a copy
                        // or close failure, so include it in owned rollback.
                        copied_names.push_back(destination_name);
                        rollback_leaf(outbox.get(), leaf.get(), stamp->directory_id,
                            copied_names, attachments_directory_created);
                        return copy;
                    }
                    copied_names.push_back(destination_name);
                    measured_total += current_size;
                    published.local_path = route.project_root / ".lingtai" / key
                        / "mailbox" / "outbox" / stamp->directory_id
                        / "attachments" / destination_name;
                    prepared.push_back({
                        route.project_root / ".lingtai" / key / "mailbox" / "sent"
                            / stamp->directory_id / "attachments" / destination_name,
                        std::move(published),
                    });
                }

                std::string payload;
                try {
                    payload = build_payload(route, text, *stamp, prepared);
                } catch (...) {
                    rollback_leaf(outbox.get(), leaf.get(), stamp->directory_id,
                        copied_names, attachments_directory_created);
                    return failure(DirectMailFailureReason::payload_failure);
                }
                if (!write_leaf_json(leaf.get(), payload)) {
                    const auto error = last_error();
                    rollback_leaf(outbox.get(), leaf.get(), stamp->directory_id,
                        copied_names, attachments_directory_created);
                    return failure(DirectMailFailureReason::publish_failure, error);
                }
                auto published_attachments =
                    std::vector<DirectMailPublishedAttachment>();
                published_attachments.reserve(prepared.size());
                for (auto &attachment : prepared) {
                    published_attachments.push_back(
                        std::move(attachment.published));
                }
                return {
                    .result = DirectMailSendResult::queued,
                    .failure_reason = DirectMailFailureReason::none,
                    .message_id = stamp->directory_id,
                    .published_message = DirectMailPublishedMessage{
                        .id = stamp->directory_id,
                        .timestamp = stamp->received_at,
                        .subject = {},
                        .text = text,
                        .attachments = std::move(published_attachments),
                    },
                };
            } catch (...) {
                rollback_leaf(outbox.get(), leaf.get(), stamp->directory_id,
                    copied_names, attachments_directory_created);
                return failure(DirectMailFailureReason::local_failure);
            }
        }
        return failure(DirectMailFailureReason::mailbox_unavailable,
            std::make_error_code(std::errc::file_exists));
    } catch (...) {
        return failure(DirectMailFailureReason::local_failure);
    }
}

} // namespace lingtai::desktop
