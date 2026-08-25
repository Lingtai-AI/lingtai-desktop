#pragma once

#include "project_attachment.h"

#include <QtCore/QJsonObject>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lingtai::desktop {

enum class AgentSetupPresetChoice { keep_current, select_preset };

struct AgentSetupPresetSelection {
    AgentSetupPresetChoice choice = AgentSetupPresetChoice::keep_current;
    std::string reference;
    QJsonObject manifest;
};

struct AgentSetupPresetPolicy {
    std::string active;
    std::string default_ref;
    std::vector<std::string> allowed;
};

// Setup owns exactly these draft fields. Everything else in init.json and
// .agent.json remains in the full source documents and is never replaced as a
// broad object: manifest agent_name/language/llm/capabilities,
// admin.{karma,nirvana}, context_limit, max_rpm, max_aed_attempts,
// soul.delay, non-empty covenant_file/comment_file updates, and preset policy;
// .agent.json agent_name + admin.{karma,nirvana}; and the single env key
// LINGTAI_SOUL_FLOW_ENABLED. soul_file is not setup-owned.
struct AgentSetupDraft {
    AgentSetupPresetSelection preset;
    std::vector<std::string> allowed_presets;
    std::string agent_name;
    std::string language;
    std::int64_t context_limit = 0;
    std::int64_t max_rpm = 0;
    std::int64_t max_aed_attempts = 0;
    std::optional<double> soul_delay;
    bool karma = false;
    bool nirvana = false;
    bool soul_flow_enabled = false;
    std::string covenant_file;
    std::string comment_file;
};

struct AgentSetupPeerState {
    std::filesystem::path directory_key;
    QJsonObject init_document;
    std::string init_bytes;
};

struct AgentSetupState {
    std::filesystem::path agent_key;
    QJsonObject init_document;
    QJsonObject agent_document;
    std::string init_bytes;
    std::string agent_bytes;
    std::optional<std::filesystem::path> env_path;
    std::string env_bytes;
    std::vector<AgentSetupPeerState> peers;
    AgentSetupDraft draft;
    bool has_virtual_keep_current = true;
};

enum class AgentSetupFailure {
    none,
    unsafe_agent_key,
    project_unavailable,
    missing_required_file,
    unsafe_path,
    symlink_rejected,
    not_regular,
    oversized,
    malformed_json,
    invalid_shape,
    missing_identity,
    source_changed,
    invalid_draft,
    staging_failed,
    publish_failed,
    rollback_failed,
    local_failure,
};

struct AgentSetupLoadResult {
    std::optional<AgentSetupState> state;
    AgentSetupFailure failure = AgentSetupFailure::none;
    std::string detail;
    [[nodiscard]] explicit operator bool() const noexcept {
        return state.has_value();
    }
};

enum class AgentSetupSaveStatus { saved, no_change, failed };
enum class AgentSetupFailurePoint { none, staging_after_first, publish_after_first };

struct AgentSetupSaveResult {
    AgentSetupSaveStatus status = AgentSetupSaveStatus::failed;
    AgentSetupFailure failure = AgentSetupFailure::local_failure;
    std::string detail;
    [[nodiscard]] explicit operator bool() const noexcept {
        return status != AgentSetupSaveStatus::failed;
    }
};

[[nodiscard]] AgentSetupPresetPolicy reconcile_agent_setup_presets(
    const AgentSetupPresetPolicy &current,
    const AgentSetupPresetSelection &selection,
    const std::vector<std::string> &requested_allowed);

class AgentSetupStore final {
public:
    explicit AgentSetupStore(const ProjectAttachment &attachment);

    [[nodiscard]] AgentSetupLoadResult load(
        const std::filesystem::path &agent_key) const noexcept;
    [[nodiscard]] AgentSetupSaveResult save(
        const AgentSetupState &state,
        const AgentSetupDraft &draft,
        AgentSetupFailurePoint failure_point =
            AgentSetupFailurePoint::none) const noexcept;

private:
    std::filesystem::path project_root_;
};

} // namespace lingtai::desktop
