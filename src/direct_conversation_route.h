#pragma once

#include "agent_identity_status.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lingtai::desktop {
class ProjectAttachment;

enum class DirectRouteFailure {
    none,
    identity_projection_incomplete,
    no_selected_agent,
    selected_agent_not_found,
    selected_agent_not_valid,
    selected_agent_is_human,
    target_agent_id_missing,
    target_address_missing,
    human_missing,
    human_ambiguous,
    human_address_missing,
    human_target_address_conflict,
};

// Transparent stable Desktop thread identity: the canonical project root plus
// the target's accepted manifest `agent_id`. It is deliberately not hashed and
// is derived from neither the current address nor the current directory key, so
// it survives an address or directory rename. Root comparison is lexical, so
// the key is exactly as stable as the accepted canonical root path itself.
struct DirectThreadKey {
    std::filesystem::path project_root;
    std::string target_agent_id;

    [[nodiscard]] friend bool operator==(
        const DirectThreadKey &, const DirectThreadKey &) = default;
};

// Exactly the accepted typed manifest facts a pseudo-mail sender needs.
// Unknown manifest fields and raw manifest JSON are never retained here.
struct HumanSenderIdentity {
    std::optional<std::string> agent_id, true_name, nickname, address, state;
    // Why this row is the human sender: the accepted C3 manifest role itself.
    AgentRole role_evidence = AgentRole::unknown;
};

struct DirectConversationRoute {
    // Current route authority for later mailbox access, never thread identity.
    std::filesystem::path human_directory_key, target_directory_key;
    std::string human_address, target_address;
    DirectThreadKey thread_key;
    HumanSenderIdentity human_identity;
};

// Retained human candidate keys are bounded; the reported count stays exact.
inline constexpr std::size_t human_candidate_key_limit = 8;

struct DirectConversationRouteReport {
    std::optional<DirectConversationRoute> route;
    DirectRouteFailure failure =
        DirectRouteFailure::identity_projection_incomplete;
    // Bounded degraded-presentation evidence: the first
    // `human_candidate_key_limit` valid human-role keys in discovery order.
    std::vector<std::filesystem::path> human_candidate_keys;
    std::size_t human_candidate_count = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return route.has_value();
    }
};

// Consumes one already-produced accepted C3 identity snapshot, the accepted
// attachment's canonical root, and the C1 selected directory key. It performs
// no filesystem access, no manifest or status discovery, and no project write.
[[nodiscard]] DirectConversationRouteReport resolve_direct_conversation_route(
    const ProjectAttachment &attachment,
    const AgentIdentityStatusSnapshot &identity,
    const std::optional<std::filesystem::path> &selected_directory_key) noexcept;

} // namespace lingtai::desktop
