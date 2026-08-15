#include "agent_sleep.h"
#include "posix_descriptor_primitives.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QLatin1StringView>
#include <QtCore/QString>

#include <algorithm>
#include <cerrno>
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

// A small operation-local read cap for the post-baseline observation window:
// bounded, not a durable cursor. Missing a record beyond this cap only
// reduces to "not observed".
constexpr std::size_t kMaxObserveBytes = std::size_t{64} * 1024;

// Opens `<root>/.lingtai/<selected key>` descriptor-relative and no-follow,
// re-validating the key with the same primitive every other reader/writer in
// this seam uses. Neither `.lingtai` nor the selected key's own directory is
// ever created here.
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

// Opens the selected Agent's own `logs/events.jsonl` for sleep observation.
// `logs` is opened, never created.
bool open_events_file(
        const ProjectAttachment &attachment,
        const fs::path &selected_directory_key,
        posix::FileDescriptor &file) {
    posix::FileDescriptor agent;
    if (!open_selected_agent_directory(
            attachment, selected_directory_key, agent)) {
        return false;
    }
    const auto logs = posix::open_directory_component(agent.get(), "logs");
    if (logs.get() < 0) return false;
    file = posix::open_regular_file_component(logs.get(), "events.jsonl");
    return file.get() >= 0;
}

bool read_exact_byte(int fd, off_t offset, char &out) {
    if (::lseek(fd, offset, SEEK_SET) < 0) return false;
    for (;;) {
        const auto count = ::read(fd, &out, 1);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        return count == 1;
    }
}

} // namespace

AgentSleepRequestResult request_agent_sleep(
        const ProjectAttachment &attachment,
        const fs::path &selected_directory_key) noexcept {
    try {
        posix::FileDescriptor agent;
        if (!open_selected_agent_directory(
                attachment, selected_directory_key, agent)) {
            return AgentSleepRequestResult::failed_local;
        }
        // `Path.write_text("", encoding="utf-8")`-equivalent create-or-
        // truncate: no `O_EXCL`, so an existing marker is overwritten to
        // zero bytes exactly like the canonical coalescing single-slot
        // marker. `O_NOFOLLOW` refuses a symlink at this exact leaf.
        const auto raw_fd = ::openat(agent.get(), ".sleep",
            O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK,
            0666);
        if (raw_fd < 0) return AgentSleepRequestResult::failed_local;
        struct stat opened {};
        const auto regular =
            ::fstat(raw_fd, &opened) == 0 && S_ISREG(opened.st_mode);
        // Explicit close-and-check before reporting success: a write that
        // looked successful can still fail to land at close.
        const auto closed_ok = ::close(raw_fd) == 0;
        return regular && closed_ok
            ? AgentSleepRequestResult::requested
            : AgentSleepRequestResult::failed_local;
    } catch (...) {
        return AgentSleepRequestResult::failed_local;
    }
}

AgentSleepEventBaseline capture_agent_sleep_event_baseline(
        const ProjectAttachment &attachment,
        const fs::path &selected_directory_key) noexcept {
    try {
        posix::FileDescriptor file;
        if (!open_events_file(attachment, selected_directory_key, file)) {
            return {};
        }
        struct stat opened {};
        if (::fstat(file.get(), &opened) != 0 || !S_ISREG(opened.st_mode)) {
            return {};
        }
        AgentSleepEventBaseline baseline;
        baseline.available = true;
        baseline.byte_size = static_cast<std::uintmax_t>(opened.st_size);
        if (baseline.byte_size == 0) {
            baseline.ends_with_newline = true;
            return baseline;
        }
        char last = 0;
        if (!read_exact_byte(
                file.get(), static_cast<off_t>(baseline.byte_size - 1),
                last)) {
            return {};
        }
        baseline.ends_with_newline = last == '\n';
        return baseline;
    } catch (...) {
        return {};
    }
}

bool observe_agent_sleep_received(
        const ProjectAttachment &attachment,
        const fs::path &selected_directory_key,
        const AgentSleepEventBaseline &baseline) noexcept {
    if (!baseline.available) return false;
    try {
        posix::FileDescriptor file;
        if (!open_events_file(attachment, selected_directory_key, file)) {
            return false;
        }
        struct stat opened {};
        if (::fstat(file.get(), &opened) != 0 || !S_ISREG(opened.st_mode)) {
            return false;
        }
        const auto size = static_cast<std::uintmax_t>(opened.st_size);
        if (size <= baseline.byte_size) return false; // nothing appended yet

        const auto to_read = static_cast<std::size_t>(std::min<std::uintmax_t>(
            size - baseline.byte_size, kMaxObserveBytes));
        if (::lseek(file.get(), static_cast<off_t>(baseline.byte_size),
                SEEK_SET) < 0) {
            return false;
        }
        std::string bytes(to_read, '\0');
        std::size_t total = 0;
        while (total < bytes.size()) {
            const auto count =
                ::read(file.get(), bytes.data() + total, bytes.size() - total);
            if (count < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (count == 0) break;
            total += static_cast<std::size_t>(count);
        }
        bytes.resize(total);

        // A pre-existing partial tail line continues here; its record
        // started before the baseline, so its completion must never be
        // attributed to this request regardless of its content.
        if (!baseline.ends_with_newline) {
            const auto first_lf = bytes.find('\n');
            bytes.erase(0,
                first_lf == std::string::npos ? bytes.size() : first_lf + 1);
        }

        std::size_t begin = 0;
        for (;;) {
            const auto newline = bytes.find('\n', begin);
            if (newline == std::string::npos) break;
            const auto line =
                std::string_view(bytes).substr(begin, newline - begin);
            begin = newline + 1;
            if (line.empty()) continue;

            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(
                QByteArray(line.data(), static_cast<int>(line.size())),
                &error);
            if (error.error != QJsonParseError::NoError
                || !document.isObject()) {
                continue;
            }
            const auto object = document.object();
            const auto type = object.value(QLatin1StringView("type"));
            const auto source = object.value(QLatin1StringView("source"));
            if (type.isString()
                && type.toString() == QStringLiteral("sleep_received")
                && source.isString()
                && source.toString() == QStringLiteral("signal_file")) {
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

} // namespace lingtai::desktop
