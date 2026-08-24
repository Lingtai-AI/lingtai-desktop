#pragma once

#include "attachment_selection.h"
#include "direct_conversation_route.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lingtai::desktop {

// Read-only metadata established from a descriptor-opened regular file owned
// by one mailbox entry. It is a history observation, not authorization for a
// later open/reveal action, which must revalidate the path again.
struct DirectConversationAttachment {
    std::filesystem::path local_path;
    std::string display_filename;
    std::uint64_t byte_size = 0;
    AttachmentMediaKind media_kind = AttachmentMediaKind::file;
    std::uint64_t device_id = 0;
    std::uint64_t inode_id = 0;
};

// One accepted direct message, already reduced to exactly what the selected-
// Agent conversation surface renders. Delivery, processing, reply chains, and
// unread state are deliberately not modelled: nothing here is inferred.
struct DirectConversationMessage {
    // The mailbox entry directory basename: the stable, displayed message ID.
    std::string id;
    bool outgoing = false;
    // The kernel's own envelope timestamp, preserved exactly as written.
    std::string timestamp;
    std::string subject;
    // The envelope's `message` field. The kernel never names this `body`.
    std::string text;
    // Valid entries retain the envelope array's order, including duplicates.
    std::vector<DirectConversationAttachment> attachments;
    // Immediate mailbox folder containing the currently observed entry. This
    // safe leaf is retained only so an action can locate the current entry by
    // id and re-walk it descriptor-relative; it is never itself trusted as an
    // authorization or persisted outside this observation.
    std::string mailbox_folder;
};

// A message.json larger than this is rejected unread; a conversation entry has
// no legitimate reason to approach it.
inline constexpr std::size_t direct_message_byte_limit = std::size_t{1} << 20;

struct DirectConversationHistory {
    std::vector<DirectConversationMessage> messages;
    // One generic count of entries that were unsafe, unreadable, or not a
    // well-formed envelope. Well-formed mail belonging to another conversation
    // is simply absent and is never counted here.
    std::size_t skipped = 0;
    // Invalid attachment elements/fields are counted independently and never
    // turn an otherwise valid envelope into a skipped message.
    std::size_t skipped_attachments = 0;
};

// Reads only the immediate `inbox`, `outbox`, and `sent` entries under
// `<route project root>/.lingtai/<route human directory key>/mailbox`, and
// only their immediate `message.json` files. It follows no symlink, reads no
// attachment content, and writes nothing. Attachment metadata is rooted to
// and descriptor-validated beneath each current mailbox entry.
[[nodiscard]] DirectConversationHistory read_direct_conversation(
    const DirectConversationRoute &route) noexcept;

// One current Agent route in a shared human-mailbox projection. `agent_key`
// is the stable in-snapshot lookup key used by the shell; filesystem and
// envelope authority remains entirely in `route`.
struct DirectMailboxRoute {
    std::string agent_key;
    DirectConversationRoute route;

    friend bool operator==(
        const DirectMailboxRoute &, const DirectMailboxRoute &) = default;
};

struct DirectMailboxRequest {
    std::vector<DirectMailboxRoute> routes;

    friend bool operator==(
        const DirectMailboxRequest &, const DirectMailboxRequest &) = default;
};

// Fixed-count metadata for the mailbox plus inbox/sent/outbox. No child is
// enumerated to obtain this fingerprint. `state` distinguishes a descriptor-
// opened directory, an absent leaf, and an unsafe/unreadable leaf.
struct DirectMailboxDirectoryFingerprint {
    int state = 0;
    std::uint64_t device_id = 0;
    std::uint64_t inode_id = 0;
    std::uint64_t byte_size = 0;
    std::uint64_t link_count = 0;
    std::int64_t modified_seconds = 0;
    std::int64_t modified_nanoseconds = 0;

    friend bool operator==(const DirectMailboxDirectoryFingerprint &,
        const DirectMailboxDirectoryFingerprint &) = default;
};

struct DirectMailboxFingerprint {
    DirectMailboxDirectoryFingerprint mailbox;
    std::array<DirectMailboxDirectoryFingerprint, 3> folders;

    friend bool operator==(
        const DirectMailboxFingerprint &, const DirectMailboxFingerprint &)
        = default;
};

struct DirectMailboxSnapshot {
    std::map<std::string, DirectConversationHistory> histories;
};

// Opens only the fixed mailbox path and three fixed folder leaves. It never
// enumerates or opens an entry and is therefore safe for the one-second UI
// tick. All walks remain descriptor-relative and no-follow.
[[nodiscard]] DirectMailboxFingerprint direct_mailbox_fingerprint(
    const DirectMailboxRequest &request) noexcept;

// One descriptor-safe mailbox scan. Each message.json is opened and parsed
// once, then classified against every current route. Complete per-Agent
// histories retain the same membership, dedupe, attachment, skip, and sort
// semantics as `read_direct_conversation`.
[[nodiscard]] DirectMailboxSnapshot read_direct_mailbox_snapshot(
    const DirectMailboxRequest &request) noexcept;

// Deterministic single-flight/generation state. Thread creation and Qt
// delivery stay in NativeShell; this value owner decides which job may run,
// whether a result is current, and whether a changed/in-scan fingerprint
// requires one follow-up generation.
class DirectMailboxSnapshotIndex final {
public:
    struct Job {
        std::uint64_t generation = 0;
        DirectMailboxRequest request;
        DirectMailboxFingerprint fingerprint;
    };
    struct Completion {
        bool accepted = false;
        std::optional<Job> follow_up;
    };

    [[nodiscard]] std::optional<Job> request(
        DirectMailboxRequest request,
        DirectMailboxFingerprint fingerprint);
    [[nodiscard]] Completion complete(
        const Job &job,
        DirectMailboxSnapshot snapshot,
        DirectMailboxFingerprint fingerprint_after);
    void reset() noexcept;
    [[nodiscard]] const DirectMailboxSnapshot *current() const noexcept;
    [[nodiscard]] bool inflight() const noexcept;

private:
    [[nodiscard]] std::optional<Job> launch_if_needed();

    std::optional<DirectMailboxRequest> desired_request_;
    DirectMailboxFingerprint desired_fingerprint_;
    std::optional<DirectMailboxRequest> current_request_;
    DirectMailboxFingerprint current_fingerprint_;
    std::optional<DirectMailboxSnapshot> current_snapshot_;
    std::uint64_t desired_generation_ = 0;
    std::uint64_t running_generation_ = 0;
    bool inflight_ = false;
};

} // namespace lingtai::desktop
