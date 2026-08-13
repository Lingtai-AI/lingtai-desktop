#include "direct_mail_publisher.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <optional>
#include <random>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;

// Matches the current Go TUI pseudo-agent sender's per-folder attempt budget
// (`mailboxIDCollisionRetries`): enough to absorb a same-second 4-hex-suffix
// collision without retrying indefinitely against a genuinely failing
// filesystem.
constexpr auto kIdCollisionRetries = 8;

struct StampedId {
    std::string directory_id;  // YYYYMMDDTHHMMSS-xxxx, matching the current
                                // interoperable mailbox id shape.
    std::string received_at;   // Second-precision RFC3339 for the same
                                // instant; the field name matches what the
                                // current Go pseudo-agent sender stamps.
};

// One fresh id per attempt: a new wall-clock read plus a fresh random suffix,
// exactly like the current sender generating a brand-new id per retry rather
// than reusing one across collisions.
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

// Only the accepted typed human sender facts, under their real manifest field
// names; unknown manifest fields and raw JSON are never retained or sent.
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

// The current interoperable text-only envelope: real field names and values
// only. No status, deliver_at, thread/reply, receipt, version, fingerprint,
// provenance, or attachments field is ever added.
[[nodiscard]] std::string build_payload(
        const DirectConversationRoute &route,
        const std::string &text,
        const StampedId &stamp) {
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
    return QJsonDocument(payload).toJson(QJsonDocument::Compact).toStdString();
}

// Write-temp-then-rename inside the already-exclusively-created leaf, mirroring
// the current sender's own `writeJSONAtomic`. The temp name is never
// `message.json`, so the pseudo-outbox poller -- which claims any directory
// containing a parseable, addressed `message.json` -- cannot observe a partial
// write.
[[nodiscard]] bool write_leaf_json(const fs::path &leaf, const std::string &bytes) {
    const auto tmp = leaf / "message.json.tmp";
    {
        auto stream = std::ofstream(tmp, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream.good()) return false;
        stream.close();
        if (!stream.good()) return false;
    }
    std::error_code rename_error;
    fs::rename(tmp, leaf / "message.json", rename_error);
    return !rename_error;
}

} // namespace

DirectMailSendResult send_direct_mail(
        const DirectConversationRoute &route, const std::string &text) noexcept {
    try {
        // Paths derive only from the accepted canonical root and the accepted
        // human directory key, which must stay one relative component -- the
        // same containment discipline the conversation reader applies.
        const auto &key = route.human_directory_key;
        if (key.empty() || key.has_root_path() || !key.parent_path().empty()
            || key == "." || key == "..") {
            return DirectMailSendResult::failed_local;
        }
        const auto outbox = route.thread_key.project_root / ".lingtai" / key
            / "mailbox" / "outbox";
        std::error_code parent_error;
        fs::create_directories(outbox, parent_error);
        if (parent_error) return DirectMailSendResult::failed_local;

        for (auto attempt = 0; attempt != kIdCollisionRetries; ++attempt) {
            const auto stamp = fresh_id();
            if (!stamp) return DirectMailSendResult::failed_local;
            const auto leaf = outbox / stamp->directory_id;

            // Exclusive create: mkdir(2) fails rather than replacing an
            // existing leaf, exactly mirroring the current sender's
            // os.Mkdir-plus-retry-on-collision leaf allocation.
            std::error_code create_error;
            const auto created = fs::create_directory(leaf, create_error);
            if (create_error) return DirectMailSendResult::failed_local;
            if (!created) continue; // another writer already owns this id

            if (write_leaf_json(leaf, build_payload(route, text, *stamp))) {
                return DirectMailSendResult::queued;
            }

            // The leaf never gained a parseable message.json, so the poller
            // could never have claimed it; only this attempt's own fresh,
            // still-private leaf is removed. Pre-existing content is never
            // touched because `created` being true is exactly the proof that
            // this leaf did not exist before this call.
            std::error_code cleanup_error;
            fs::remove_all(leaf, cleanup_error);
            return DirectMailSendResult::failed_local;
        }
        return DirectMailSendResult::failed_local;
    } catch (...) {
        // Only string/JSON allocation can throw here. Fail closed rather than
        // terminating or leaving an ambiguous partial write.
        return DirectMailSendResult::failed_local;
    }
}

} // namespace lingtai::desktop
