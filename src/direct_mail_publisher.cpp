#include "direct_mail_publisher.h"
#include "posix_descriptor_primitives.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <optional>
#include <random>
#include <sys/stat.h>
#include <unistd.h>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;
namespace posix = posix_internal;

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

// Write-temp-then-rename inside the already-exclusively-created leaf,
// descriptor-relative to it, mirroring the current sender's own
// `writeJSONAtomic`. The temp name is never `message.json`, so the
// pseudo-outbox poller -- which claims any directory containing a
// parseable, addressed `message.json` -- cannot observe a partial write.
[[nodiscard]] bool write_leaf_json(int leaf_fd, const std::string &bytes) {
    const auto raw_fd = ::openat(leaf_fd, "message.json.tmp",
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (raw_fd < 0) return false;
    auto ok = true;
    std::size_t total = 0;
    while (ok && total < bytes.size()) {
        const auto count = ::write(raw_fd, bytes.data() + total,
            bytes.size() - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        total += static_cast<std::size_t>(count);
    }
    // Explicit close-and-check before the rename: a write that looked
    // successful can still fail to land at close.
    if (::close(raw_fd) != 0) ok = false;
    if (!ok) return false;
    return ::renameat(leaf_fd, "message.json.tmp", leaf_fd, "message.json") == 0;
}

} // namespace

DirectMailSendOutcome send_direct_mail(
        const DirectConversationRoute &route, const std::string &text) noexcept {
    try {
        // Paths derive only from the accepted canonical root and the accepted
        // human directory key, which must stay one relative component -- the
        // same containment discipline the conversation reader applies.
        const auto &key = route.human_directory_key;
        if (key.empty() || key.has_root_path() || !key.parent_path().empty()
            || key == "." || key == "..") {
            return {};
        }
        // Anchored, descriptor-relative walk: every component from the
        // project root down to the outbox is opened no-follow, one leaf at a
        // time, so no intermediate `.lingtai`, human-directory, `mailbox`,
        // or `outbox` symlink can redirect this publish outside the project.
        // The `.lingtai` and human directories are opened, never created:
        // only the current sender's own `mailbox`/`outbox` folders are
        // created here if missing.
        const auto root = posix::open_root_directory(
            route.project_root);
        if (root.get() < 0) return {};
        const auto lingtai = posix::open_directory_component(root.get(), ".lingtai");
        if (lingtai.get() < 0) return {};
        const auto human = posix::open_directory_component(lingtai.get(), key);
        if (human.get() < 0) return {};
        const auto mailbox = posix::open_directory_component(
            human.get(), "mailbox", /*create=*/true);
        if (mailbox.get() < 0) return {};
        const auto outbox = posix::open_directory_component(
            mailbox.get(), "outbox", /*create=*/true);
        if (outbox.get() < 0) return {};

        for (auto attempt = 0; attempt != kIdCollisionRetries; ++attempt) {
            const auto stamp = fresh_id();
            if (!stamp) return {};

            // Exclusive create: mkdirat(2) fails rather than replacing an
            // existing leaf, exactly mirroring the current sender's
            // os.Mkdir-plus-retry-on-collision leaf allocation.
            if (::mkdirat(outbox.get(), stamp->directory_id.c_str(), 0700) != 0) {
                if (errno == EEXIST) continue; // another writer owns this id
                return {};
            }
            const auto leaf = posix::open_directory_component(
                outbox.get(), stamp->directory_id);
            if (leaf.get() < 0) {
                ::unlinkat(outbox.get(), stamp->directory_id.c_str(),
                    AT_REMOVEDIR);
                return {};
            }

            if (write_leaf_json(leaf.get(), build_payload(route, text, *stamp))) {
                return {
                    DirectMailSendResult::queued,
                    stamp->directory_id,
                };
            }

            // The leaf never gained a parseable message.json, so the poller
            // could never have claimed it; only this attempt's own fresh,
            // still-private leaf is removed. Pre-existing content is never
            // touched, because this exact id was exclusively created by this
            // call moments earlier.
            ::unlinkat(leaf.get(), "message.json.tmp", 0);
            ::unlinkat(outbox.get(), stamp->directory_id.c_str(), AT_REMOVEDIR);
            return {};
        }
        return {};
    } catch (...) {
        // Only string/JSON allocation can throw here. Fail closed rather than
        // terminating or leaving an ambiguous partial write.
        return {};
    }
}

} // namespace lingtai::desktop
