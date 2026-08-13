#include "direct_conversation_route.h"

#include "project_attachment.h"

#include <string>

namespace lingtai::desktop {
namespace {

// A row can route only when the accepted composite projection parsed its
// manifest and retained typed identity facts. Malformed, unsafe, and
// unknown-role rows stay visible in the snapshot but are never a route
// participant.
[[nodiscard]] bool is_valid_row(const AgentRow &item) {
    return item.manifest_kind == AgentManifestKind::valid
        && item.identity.has_value() && item.role != AgentRole::unknown;
}

// An empty manifest string carries no routing authority and is treated as
// absent rather than as a usable address or identifier.
[[nodiscard]] const std::string *present_value(
        const std::optional<std::string> &field) {
    return field.has_value() && !field->empty() ? &*field : nullptr;
}

[[nodiscard]] std::optional<DirectConversationRoute> resolve_route(
        const ProjectAttachment &attachment,
        const AgentSnapshot &snapshot,
        const std::optional<std::filesystem::path> &selected_directory_key) {
    if (!selected_directory_key || selected_directory_key->empty())
        return std::nullopt;
    const auto &items = snapshot.items;

    const AgentRow *selected = nullptr;
    for (const auto &item : items) {
        // The selected key must match one discovered key exactly; a prefix or
        // any other near match never falls back to a different Agent.
        if (item.directory_key == *selected_directory_key) {
            selected = &item;
            break;
        }
    }
    if (!selected || !is_valid_row(*selected) || selected->role == AgentRole::human)
        return std::nullopt;

    const auto *target_agent_id = present_value(selected->identity->agent_id);
    const auto *target_address = present_value(selected->identity->address);
    if (!target_agent_id || !target_address) return std::nullopt;

    const AgentIdentityFacts *human_facts = nullptr;
    const std::filesystem::path *human_key = nullptr;
    for (const auto &item : items) {
        if (!is_valid_row(item) || item.role != AgentRole::human) continue;
        if (human_facts) return std::nullopt; // more than one valid human
        human_facts = &*item.identity;
        human_key = &item.directory_key;
    }
    if (!human_facts) return std::nullopt;
    const auto *human_address = present_value(human_facts->address);
    if (!human_address || *human_address == *target_address) return std::nullopt;

    DirectConversationRoute route;
    // Stable across the current call only: derived from the manifest's own
    // agent_id and canonical root, never from the current address or key.
    route.project_root = attachment.root();
    route.target_agent_id = *target_agent_id;
    // Current directory keys and addresses are route authority only.
    route.human_directory_key = *human_key;
    route.target_directory_key = selected->directory_key;
    route.human_address = *human_address;
    route.target_address = *target_address;
    route.human_identity = HumanSenderIdentity{human_facts->agent_id,
        human_facts->true_name, human_facts->nickname, human_facts->address,
        human_facts->state};
    return route;
}

} // namespace

std::optional<DirectConversationRoute> resolve_direct_conversation_route(
        const ProjectAttachment &attachment,
        const AgentSnapshot &snapshot,
        const std::optional<std::filesystem::path> &selected_directory_key) noexcept {
    try {
        return resolve_route(attachment, snapshot, selected_directory_key);
    } catch (...) {
        // Only evidence allocation can throw here. Fail closed with no route
        // rather than terminating or inventing one.
        return std::nullopt;
    }
}

} // namespace lingtai::desktop
