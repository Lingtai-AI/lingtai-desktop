#include "agent_task_card.h"
#include "posix_descriptor_primitives.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace lingtai::desktop {
namespace {

namespace posix = posix_internal;

// A small independent Desktop bound, not read from producer config: enough
// to hold the exact bytes `active`/`inactive` and reject anything else as
// over-bound rather than partially reading it.
constexpr std::size_t kMaxStatusBytes = 32;
// Comfortably covers the current default 2,000-character producer body even
// at UTF-8's four-byte-per-character worst case.
constexpr std::size_t kMaxBodyBytes = std::size_t{8} * 1024;

// Opens `<attachment root>/.lingtai/<selected key>/taskcard/<leaf>`, one
// no-follow leaf at a time. The selected key is re-validated with the same
// primitive every other reader uses: C1's own grammar is defense in depth,
// not a reason for this boundary to trust the caller.
bool open_taskcard_leaf(
        const ProjectAttachment &attachment,
        const std::filesystem::path &selected_directory_key,
        const char *leaf_name,
        posix::FileDescriptor &file) {
    if (!posix::safe_leaf(selected_directory_key)) return false;
    const auto root = posix::open_root_directory(attachment.root());
    if (root.get() < 0) return false;
    const auto lingtai =
        posix::open_directory_component(root.get(), ".lingtai");
    if (lingtai.get() < 0) return false;
    const auto agent = posix::open_directory_component(
        lingtai.get(), selected_directory_key);
    if (agent.get() < 0) return false;
    const auto taskcard = posix::open_directory_component(agent.get(), "taskcard");
    if (taskcard.get() < 0) return false;
    file = posix::open_regular_file_component(taskcard.get(), leaf_name);
    return file.get() >= 0;
}

// Reads the file completely, bounded by `max_bytes` on the actual read: a
// source at or under the observed size that exceeds the bound is refused
// rather than truncated, and a short read (for example a file shrinking
// mid-read) never yields a partial result mistaken for a complete one.
bool read_bounded_complete(int fd, std::size_t max_bytes, std::string &bytes) {
    struct stat opened {};
    if (::fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode)) return false;
    const auto size = static_cast<std::uintmax_t>(opened.st_size);
    if (size > static_cast<std::uintmax_t>(max_bytes)) return false;
    bytes.resize(static_cast<std::size_t>(size));
    std::size_t total = 0;
    while (total < bytes.size()) {
        const auto count = ::read(fd, bytes.data() + total, bytes.size() - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) break;
        total += static_cast<std::size_t>(count);
    }
    bytes.resize(total);
    return total == size;
}

// Valid UTF-8 and nonblank (ignoring surrounding whitespace), returning the
// original bytes unchanged for display when both hold. `QString::fromUtf8`
// replaces any invalid sequence with U+FFFD, so a byte-exact round trip
// through `toUtf8()` is a strict validity proof with no hand-rolled decoder.
std::optional<std::string> validate_nonblank_utf8_body(const std::string &bytes) {
    const auto original = QByteArray(bytes.data(), static_cast<int>(bytes.size()));
    const auto decoded = QString::fromUtf8(original);
    if (decoded.toUtf8() != original) return std::nullopt;
    if (decoded.trimmed().isEmpty()) return std::nullopt;
    return bytes;
}

} // namespace

AgentTaskCardSnapshot read_agent_task_card(
        const ProjectAttachment &attachment,
        const std::filesystem::path &selected_directory_key) noexcept {
    try {
        posix::FileDescriptor status_file;
        if (!open_taskcard_leaf(
                attachment, selected_directory_key, "status", status_file)) {
            return {};
        }
        std::string status_bytes;
        if (!read_bounded_complete(status_file.get(), kMaxStatusBytes, status_bytes)) {
            return {};
        }
        if (status_bytes == "inactive") {
            return AgentTaskCardSnapshot{AgentTaskCardState::inactive, {}};
        }
        if (status_bytes != "active") {
            return {};
        }

        posix::FileDescriptor body_file;
        if (!open_taskcard_leaf(
                attachment, selected_directory_key, "taskcard.md", body_file)) {
            return {};
        }
        std::string body_bytes;
        if (!read_bounded_complete(body_file.get(), kMaxBodyBytes, body_bytes)) {
            return {};
        }
        auto body = validate_nonblank_utf8_body(body_bytes);
        if (!body) return {};
        return AgentTaskCardSnapshot{AgentTaskCardState::active, std::move(*body)};
    } catch (...) {
        // Only string allocation can throw here. Show nothing rather than a
        // partial projection that looks complete.
        return {};
    }
}

} // namespace lingtai::desktop
