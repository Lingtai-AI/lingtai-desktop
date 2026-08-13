#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace lingtai::desktop {

// One user-authored, Desktop-opaque local working document: at most 64 KiB
// of encoded UTF-8 text, never parsed/validated/imported/exported, or
// claimed kernel-ready. Persisted only beneath an explicitly supplied
// Desktop application-data root, keyed by exactly the canonical project
// root plus the selected Agent's own nonempty stable `agent_id` -- never a
// directory name, nickname, address, or preset ref, so a directory rename
// that preserves `agent_id` resolves to the same saved document.
constexpr std::size_t kLocalPresetDraftMaxBytes = 64U * 1024U;

struct LocalPresetDraftLoad {
    // True only for a complete, self-verifying envelope for exactly this
    // key. A missing/unsafe/unreadable/oversized/mismatched source simply
    // is not found, matching "absent" rather than a distinct error state.
    bool found = false;
    std::string document; // exact bytes; empty when !found
};

// Reads the one envelope this exact (project, agent_id) key owns, always
// re-verifying the envelope's own recorded fields rather than trusting the
// deterministic digest filename alone. Stateless and read-only.
[[nodiscard]] LocalPresetDraftLoad load_local_preset_draft(
    const std::filesystem::path &app_data_root,
    const std::filesystem::path &canonical_project_root,
    const std::string &agent_id) noexcept;

enum class LocalPresetDraftSaveResult { saved, failed };

// Persists exactly `document`'s encoded UTF-8 bytes for this key beneath
// `app_data_root`, refusing -- never truncating -- anything over
// `kLocalPresetDraftMaxBytes`. A refused or failed save never touches
// previously saved content: the write lands in a private temp leaf first,
// then replaces the target only by one atomic rename.
[[nodiscard]] LocalPresetDraftSaveResult save_local_preset_draft(
    const std::filesystem::path &app_data_root,
    const std::filesystem::path &canonical_project_root,
    const std::string &agent_id,
    const std::string &document) noexcept;

} // namespace lingtai::desktop
