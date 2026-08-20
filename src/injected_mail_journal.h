#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <sys/types.h>

namespace lingtai::desktop {

// Pull mailbox ids out of one events.jsonl line when it is a committed
// notification_block_injected event whose model-visible email lane names them.
void collect_injected_mail_ids_from_event_json(
    std::string_view json_line, std::unordered_set<std::string> &ids);

// Session-scoped tail of the selected Agent's logs/events.jsonl. The first
// observation of a file skips existing bytes so Desktop never full-scans a
// large journal; later polls consume only newly appended complete lines.
class InjectedMailJournal {
public:
    void reset() noexcept;
    void poll(
        const std::filesystem::path &project_root,
        const std::filesystem::path &target_directory_key);
    [[nodiscard]] const std::unordered_set<std::string> &ids() const noexcept {
        return ids_;
    }

private:
    bool anchored_ = false;
    ino_t inode_ = 0;
    off_t offset_ = 0;
    std::string carry_;
    std::unordered_set<std::string> ids_;
};

} // namespace lingtai::desktop
