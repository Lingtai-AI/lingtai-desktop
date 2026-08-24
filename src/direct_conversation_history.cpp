#include "direct_conversation_history.h"
#include "posix_descriptor_primitives.h"

#include <QtCore/QByteArrayView>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <dirent.h>
#include <optional>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;
namespace posix = posix_internal;

// Fixed read folders for this slice. `sent` precedes `outbox` so that an entry
// the kernel has already moved from outbox to sent collapses onto the sent
// copy rather than the still-pending one.
constexpr const char *kFolders[] = {"inbox", "sent", "outbox"};

// The kernel stamps a different key per folder: `received_at` on delivery,
// `sent_at` when outbox/<id> is moved to sent/<id>, and `deliver_at` while the
// entry is still pending. Precedence matters, because a sent entry keeps the
// original `deliver_at` beside the `sent_at` that actually ordered it.
constexpr const char *kTimestampKeys[] = {
    "received_at", "sent_at", "deliver_at"};

// Exactly the envelope facts this conversation needs. Unknown fields, the
// identity block beyond `agent_id`, and attachments are never retained.
struct Envelope {
    std::string from, subject, text, timestamp;
    // Absent when `to` is not exactly one address: a group thread is not a
    // direct conversation, but it is ordinary mail rather than a bad entry.
    std::optional<std::string> sole_recipient;
    bool carbon_copied = false;
    std::optional<std::string> identity_agent_id;
    std::vector<std::string> attachment_hints;
    std::size_t skipped_attachments = 0;
};

enum class Membership { incoming, outgoing, absent };

[[nodiscard]] std::optional<std::string> string_field(
        const QJsonObject &object, const char *key) {
    const auto value = object.value(QLatin1StringView(key));
    if (!value.isString()) return std::nullopt;
    return value.toString().toStdString();
}

// One recipient, written either as a string or as a one-element array.
[[nodiscard]] std::optional<std::string> sole_recipient(const QJsonValue &to) {
    if (to.isString()) return to.toString().toStdString();
    if (!to.isArray()) return std::nullopt;
    const auto array = to.toArray();
    if (array.size() != 1 || !array.at(0).isString()) return std::nullopt;
    return array.at(0).toString().toStdString();
}

// A copied message is not a direct one; absent, null, and empty all mean none.
[[nodiscard]] bool carbon_copied(const QJsonObject &object) {
    const auto value = object.value(QLatin1StringView("cc"));
    if (value.isUndefined() || value.isNull()) return false;
    return !value.isArray() || !value.toArray().isEmpty();
}

// A well-formed entry needs a sender, a `message` body, and one timestamp the
// display can order on. The kernel never names the body `body`.
[[nodiscard]] std::optional<Envelope> parse_envelope(const std::string &bytes) {
    QJsonParseError error;
    const auto value = QJsonValue::fromJson(
        QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())),
        &error);
    if (error.error != QJsonParseError::NoError || !value.isObject())
        return std::nullopt;
    const auto object = value.toObject();
    const auto from = string_field(object, "from");
    const auto text = string_field(object, "message");
    if (!from || !text) return std::nullopt;
    Envelope envelope;
    for (const auto *key : kTimestampKeys) {
        auto stamp = string_field(object, key);
        if (stamp && !stamp->empty()) {
            envelope.timestamp = std::move(*stamp);
            break;
        }
    }
    if (envelope.timestamp.empty()) return std::nullopt;
    envelope.from = *from;
    envelope.text = *text;
    envelope.sole_recipient = sole_recipient(object.value(
        QLatin1StringView("to")));
    envelope.carbon_copied = carbon_copied(object);
    if (auto subject = string_field(object, "subject"))
        envelope.subject = std::move(*subject);
    if (const auto identity = object.value(QLatin1StringView("identity"));
        identity.isObject()) {
        envelope.identity_agent_id = string_field(
            identity.toObject(), "agent_id");
    }
    const auto attachments = object.value(QLatin1StringView("attachments"));
    if (!attachments.isUndefined()) {
        if (!attachments.isArray()) {
            ++envelope.skipped_attachments;
        } else {
            const auto array = attachments.toArray();
            envelope.attachment_hints.reserve(
                static_cast<std::size_t>(array.size()));
            for (const auto &attachment : array) {
                if (attachment.isString()) {
                    envelope.attachment_hints.push_back(
                        attachment.toString().toStdString());
                } else {
                    ++envelope.skipped_attachments;
                }
            }
        }
    }
    return envelope;
}

// Membership is exactly envelope based: one sender, one recipient, no CC. An
// incoming entry that names its sender identity must name the selected Agent;
// one that names none falls back to the exact address.
[[nodiscard]] Membership membership_of(
        const Envelope &envelope, const DirectConversationRoute &route) {
    if (envelope.carbon_copied || !envelope.sole_recipient)
        return Membership::absent;
    const auto &to = *envelope.sole_recipient;
    if (envelope.from == route.target_address && to == route.human_address) {
        return !envelope.identity_agent_id
                || *envelope.identity_agent_id
                    == route.target_agent_id
            ? Membership::incoming : Membership::absent;
    }
    if (envelope.from == route.human_address && to == route.target_address)
        return Membership::outgoing;
    return Membership::absent;
}

// Reads one immediate message.json descriptor-relative to its already open,
// already type-checked entry directory, no-follow at every component walked
// to reach it, bounded to the byte limit on the actual read.
[[nodiscard]] bool read_message_at(int entry_fd, std::string &bytes) {
    const auto file = posix::open_regular_file_component(entry_fd, "message.json");
    if (file.get() < 0) return false;
    struct stat opened {};
    if (::fstat(file.get(), &opened) != 0) return false;
    if (static_cast<std::uintmax_t>(opened.st_size) > direct_message_byte_limit)
        return false;
    bytes.resize(static_cast<std::size_t>(opened.st_size));
    std::size_t total = 0;
    while (total < bytes.size()) {
        const auto count = ::read(file.get(), bytes.data() + total,
            bytes.size() - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) break;
        total += static_cast<std::size_t>(count);
    }
    // Bound the read itself, not just the size check: an entry that grows
    // after its size is taken still cannot be read past the limit.
    bytes.resize(total);
    return true;
}

// Serialized attachment parents are protocol hints only. Each final basename
// is validated as one safe leaf, then opened under this already-open entry's
// own attachments directory. Metadata is retained only after fstat succeeds
// on that opened regular file; content is never read.
void project_attachments(
        int entry_fd,
        const fs::path &entry_path,
        const Envelope &envelope,
        DirectConversationMessage &message,
        DirectConversationHistory &history) {
    history.skipped_attachments += envelope.skipped_attachments;
    if (envelope.attachment_hints.empty()) return;

    const auto directory = posix::open_directory_component(
        entry_fd, "attachments");
    if (directory.get() < 0) {
        history.skipped_attachments += envelope.attachment_hints.size();
        return;
    }
    message.attachments.reserve(envelope.attachment_hints.size());
    for (const auto &hint : envelope.attachment_hints) {
        const auto basename = fs::path(hint).filename();
        if (!posix::safe_leaf(basename)) {
            ++history.skipped_attachments;
            continue;
        }
        const auto file = posix::open_regular_file_component(
            directory.get(), basename);
        if (file.get() < 0) {
            ++history.skipped_attachments;
            continue;
        }
        struct stat opened {};
        if (::fstat(file.get(), &opened) != 0 || opened.st_size < 0) {
            ++history.skipped_attachments;
            continue;
        }
        const auto display_filename = basename.string();
        message.attachments.push_back({
            (entry_path / "attachments" / basename).lexically_normal(),
            display_filename,
            static_cast<std::uint64_t>(opened.st_size),
            classify_attachment_media_kind(display_filename),
            static_cast<std::uint64_t>(opened.st_dev),
            static_cast<std::uint64_t>(opened.st_ino),
        });
    }
}

// One folder contributes its immediate <entry>/message.json files, opened
// relative to the already open mailbox descriptor so that no intermediate
// path component -- however it is later replaced on disk -- is ever
// resolved by anything but this single descriptor-relative step. A missing
// folder is simply no rows; an unsafe or unreadable folder or entry is one
// generic skip, and never hides its valid neighbors.
void read_folder(
        int mailbox_fd,
        const char *folder_name,
        const DirectConversationRoute &route,
        DirectConversationHistory &history,
        std::set<std::string> &outgoing_ids) {
    const auto folder = posix::open_directory_component(mailbox_fd, folder_name);
    if (folder.get() < 0) {
        // A missing folder is simply no rows; a symlink, a non-directory, or
        // any other open failure at this single leaf is one generic skip.
        if (errno != ENOENT) ++history.skipped;
        return;
    }
    const auto duplicate = ::dup(folder.get());
    if (duplicate < 0) { ++history.skipped; return; }
    posix::DirectoryStream stream(::fdopendir(duplicate));
    if (!stream.get()) {
        ::close(duplicate);
        ++history.skipped;
        return;
    }
    for (;;) {
        errno = 0;
        const auto raw_entry = ::readdir(stream.get());
        if (!raw_entry) {
            if (errno != 0) ++history.skipped;
            break;
        }
        const auto name = std::string(raw_entry->d_name);
        if (name == "." || name == "..") continue;
        const auto entry_dir = posix::open_directory_component(folder.get(), name);
        if (entry_dir.get() < 0) {
            ++history.skipped;
            continue;
        }
        auto bytes = std::string();
        if (!read_message_at(entry_dir.get(), bytes)) {
            ++history.skipped;
            continue;
        }
        const auto envelope = parse_envelope(bytes);
        if (!envelope) {
            ++history.skipped;
            continue;
        }
        const auto membership = membership_of(*envelope, route);
        if (membership == Membership::absent) continue;
        // The entry directory basename is the stable, displayed message ID.
        const auto outgoing = membership == Membership::outgoing;
        if (outgoing && !outgoing_ids.insert(name).second) continue;
        DirectConversationMessage message{name, outgoing,
            envelope->timestamp, envelope->subject, envelope->text, {}, {}};
        message.mailbox_folder = folder_name;
        const auto entry_path = route.project_root / ".lingtai"
            / route.human_directory_key / "mailbox" / folder_name / name;
        project_attachments(entry_dir.get(), entry_path,
            *envelope, message, history);
        history.messages.push_back(std::move(message));
    }
}

} // namespace

DirectConversationHistory read_direct_conversation(
        const DirectConversationRoute &route) noexcept {
    DirectConversationHistory history;
    try {
        // Paths derive only from the accepted canonical root and the accepted
        // human directory key, which must stay one relative component.
        const auto &key = route.human_directory_key;
        if (key.empty() || key.has_root_path()
            || !key.parent_path().empty()
            || key == "." || key == "..") {
            return {};
        }
        // Anchored, descriptor-relative walk: every component from the
        // project root down to the mailbox is opened no-follow, one leaf at
        // a time, so no intermediate `.lingtai`, human-directory, or
        // `mailbox` symlink can redirect this read outside the project.
        const auto root = posix::open_root_directory(
            route.project_root);
        if (root.get() < 0) return {};
        const auto lingtai = posix::open_directory_component(root.get(), ".lingtai");
        if (lingtai.get() < 0) return {};
        const auto human = posix::open_directory_component(lingtai.get(), key);
        if (human.get() < 0) return {};
        const auto mailbox = posix::open_directory_component(human.get(), "mailbox");
        if (mailbox.get() < 0) return {};

        auto outgoing_ids = std::set<std::string>();
        for (const auto *folder : kFolders)
            read_folder(mailbox.get(), folder, route, history, outgoing_ids);
        // Chronological, with the directory ID as the deterministic tie-break.
        std::ranges::sort(history.messages, [](const auto &a, const auto &b) {
            return a.timestamp != b.timestamp
                ? a.timestamp < b.timestamp : a.id < b.id;
        });
    } catch (...) {
        // Only row allocation can throw here. Show nothing rather than a
        // partial conversation that looks complete.
        return {};
    }
    return history;
}

} // namespace lingtai::desktop
