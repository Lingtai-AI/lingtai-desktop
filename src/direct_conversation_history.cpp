#include "direct_conversation_history.h"

#include <QtCore/QByteArrayView>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <optional>
#include <set>
#include <string>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;

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
                    == route.thread_key.target_agent_id
            ? Membership::incoming : Membership::absent;
    }
    if (envelope.from == route.human_address && to == route.target_address)
        return Membership::outgoing;
    return Membership::absent;
}

// Reads one immediate message.json after explicit symlink and regular-file
// checks, rejecting anything unsafe, unreadable, or oversized.
[[nodiscard]] bool read_message(const fs::path &path, std::string &bytes) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error || fs::is_symlink(status) || !fs::is_regular_file(status))
        return false;
    const auto size = fs::file_size(path, error);
    if (error || size > direct_message_byte_limit) return false;
    auto stream = std::ifstream(path, std::ios::binary);
    if (!stream) return false;
    // Bound the read itself, not just the size check: an entry that grows
    // after its size is taken still cannot be read past the limit.
    bytes.resize(static_cast<std::size_t>(size));
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(stream.gcount()));
    return !stream.bad();
}

// One folder contributes its immediate <entry>/message.json files. A missing
// folder is simply no rows; an unreadable folder or entry is one generic skip,
// and never hides its valid neighbors.
void read_folder(
        const fs::path &folder,
        const DirectConversationRoute &route,
        DirectConversationHistory &history,
        std::set<std::string> &outgoing_ids) {
    std::error_code error;
    const auto status = fs::symlink_status(folder, error);
    if (error || !fs::exists(status)) return;
    if (fs::is_symlink(status) || !fs::is_directory(status)) {
        ++history.skipped;
        return;
    }
    auto entry = fs::directory_iterator(folder, error);
    for (; !error && entry != fs::directory_iterator(); entry.increment(error)) {
        const auto entry_status = entry->symlink_status(error);
        if (error || fs::is_symlink(entry_status)
            || !fs::is_directory(entry_status)) {
            error.clear();
            ++history.skipped;
            continue;
        }
        auto bytes = std::string();
        if (!read_message(entry->path() / "message.json", bytes)) {
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
        auto id = entry->path().filename().string();
        const auto outgoing = membership == Membership::outgoing;
        if (outgoing && !outgoing_ids.insert(id).second) continue;
        history.messages.push_back({std::move(id), outgoing,
            envelope->timestamp, envelope->subject, envelope->text});
    }
    if (error) ++history.skipped;
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
        const auto mailbox = route.thread_key.project_root / ".lingtai" / key
            / "mailbox";
        auto outgoing_ids = std::set<std::string>();
        for (const auto *folder : kFolders)
            read_folder(mailbox / folder, route, history, outgoing_ids);
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
