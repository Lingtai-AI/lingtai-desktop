#pragma once

#include "agent_projection.h"

#include <filesystem>
#include <optional>
#include <string>

namespace lingtai::desktop {
class ProjectAttachment;

// Exactly the accepted typed manifest facts a pseudo-mail sender needs.
// Unknown manifest fields and raw manifest JSON are never retained here.
struct HumanSenderIdentity {
    std::optional<std::string> agent_id, true_name, nickname, address, state;
};

// One resolved current direct conversation route: the canonical project root
// and target manifest `agent_id` used to anchor mailbox paths, plus the
// current route-authority directory keys, addresses, and human sender
// identity the envelope needs. Directory keys and addresses are current
// route authority only, never a stable thread identity.
struct DirectConversationRoute {
    std::filesystem::path project_root;
    std::string target_agent_id;
    std::filesystem::path human_directory_key, target_directory_key;
    std::string human_address, target_address;
    HumanSenderIdentity human_identity;
};

// Consumes one already-produced accepted Agent snapshot, the accepted
// attachment's canonical root, and the C1 selected directory key. It performs
// no filesystem access, no manifest or status discovery, and no project
// write. A route resolves only for an exact selected valid nonhuman row with
// a present target manifest agent_id and address, exactly one valid
// addressed human row, and human/target addresses that differ.
[[nodiscard]] std::optional<DirectConversationRoute>
resolve_direct_conversation_route(
    const ProjectAttachment &attachment,
    const AgentSnapshot &snapshot,
    const std::optional<std::filesystem::path> &selected_directory_key) noexcept;

} // namespace lingtai::desktop
