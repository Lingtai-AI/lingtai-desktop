#include "agent_signal.h"

#include "posix_descriptor_primitives.h"

#include <cerrno>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace lingtai::desktop {
namespace {

namespace fs = std::filesystem;
namespace posix = posix_internal;

const char *leaf_name(AgentSignalKind kind) {
    switch (kind) {
    case AgentSignalKind::sleep: return ".sleep";
    case AgentSignalKind::suspend: return ".suspend";
    case AgentSignalKind::interrupt: return ".interrupt";
    case AgentSignalKind::clear: return ".clear";
    case AgentSignalKind::refresh: return ".refresh";
    case AgentSignalKind::refresh_taken: return ".refresh.taken";
    }
    return "";
}

std::string_view marker_content(AgentSignalKind kind) {
    return kind == AgentSignalKind::clear
        ? std::string_view("desktop\n") : std::string_view();
}

bool open_agent_directory(const ProjectAttachment &attachment,
        const fs::path &agent_key, posix::FileDescriptor &agent) {
    if (!posix::safe_leaf(agent_key)) return false;
    const auto root = posix::open_root_directory(attachment.root());
    if (root.get() < 0) return false;
    const auto lingtai = posix::open_directory_component(root.get(), ".lingtai");
    if (lingtai.get() < 0) return false;
    agent = posix::open_directory_component(lingtai.get(), agent_key);
    return agent.get() >= 0;
}

bool write_all(int fd, std::string_view bytes) {
    auto offset = std::size_t{0};
    while (offset < bytes.size()) {
        const auto count = ::write(fd, bytes.data() + offset,
            bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

} // namespace

AgentSignalWriteResult write_agent_signal(
        const ProjectAttachment &attachment, const fs::path &agent_key,
        AgentSignalKind kind) noexcept {
    if (kind == AgentSignalKind::refresh_taken) {
        return AgentSignalWriteResult::refused;
    }
    try {
        posix::FileDescriptor agent;
        if (!open_agent_directory(attachment, agent_key, agent)) {
            return AgentSignalWriteResult::refused;
        }
        const auto raw_fd = ::openat(agent.get(), leaf_name(kind),
            O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC
                | O_NONBLOCK,
            0666);
        if (raw_fd < 0) return AgentSignalWriteResult::refused;
        struct stat opened {};
        const auto regular = ::fstat(raw_fd, &opened) == 0
            && S_ISREG(opened.st_mode) && opened.st_nlink == 1;
        const auto wrote = regular && ::ftruncate(raw_fd, 0) == 0
            && write_all(raw_fd, marker_content(kind));
        const auto closed = ::close(raw_fd) == 0;
        return wrote && closed
            ? AgentSignalWriteResult::written
            : AgentSignalWriteResult::refused;
    } catch (...) {
        return AgentSignalWriteResult::refused;
    }
}

AgentSignalRemoveResult remove_agent_signal(
        const ProjectAttachment &attachment, const fs::path &agent_key,
        AgentSignalKind kind) noexcept {
    try {
        posix::FileDescriptor agent;
        if (!open_agent_directory(attachment, agent_key, agent)) {
            return AgentSignalRemoveResult::refused;
        }
        struct stat observed {};
        if (::fstatat(agent.get(), leaf_name(kind), &observed,
                AT_SYMLINK_NOFOLLOW) != 0) {
            return errno == ENOENT
                ? AgentSignalRemoveResult::absent
                : AgentSignalRemoveResult::refused;
        }
        if (!S_ISREG(observed.st_mode)) return AgentSignalRemoveResult::refused;
        return ::unlinkat(agent.get(), leaf_name(kind), 0) == 0
            ? AgentSignalRemoveResult::removed
            : AgentSignalRemoveResult::refused;
    } catch (...) {
        return AgentSignalRemoveResult::refused;
    }
}

AgentSignalObservation observe_agent_signal(const ProjectAttachment &attachment,
        const fs::path &agent_key, AgentSignalKind kind) noexcept {
    try {
        posix::FileDescriptor agent;
        if (!open_agent_directory(attachment, agent_key, agent)) {
            return AgentSignalObservation::refused;
        }
        struct stat observed {};
        if (::fstatat(agent.get(), leaf_name(kind), &observed,
                AT_SYMLINK_NOFOLLOW) != 0) {
            return errno == ENOENT
                ? AgentSignalObservation::absent
                : AgentSignalObservation::refused;
        }
        return S_ISREG(observed.st_mode)
            ? AgentSignalObservation::present
            : AgentSignalObservation::refused;
    } catch (...) {
        return AgentSignalObservation::refused;
    }
}

} // namespace lingtai::desktop
