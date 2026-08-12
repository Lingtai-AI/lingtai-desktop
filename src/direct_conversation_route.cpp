#include "direct_conversation_route.h"

#include "project_attachment.h"

#include <string>
#include <utility>

namespace lingtai::desktop {
namespace {

// A row can route only when the accepted C3 projection parsed its manifest and
// retained typed identity facts. Malformed, unsafe, and unknown-role rows stay
// visible in the snapshot but are never a route participant.
[[nodiscard]] bool is_valid_row(const AgentManifestDiscoveryItem &item) {
    return item.manifest_kind == AgentManifestKind::valid
        && item.identity.has_value() && item.role != AgentRole::unknown;
}

// An empty manifest string carries no routing authority and is treated as
// absent rather than as a usable address or identifier.
[[nodiscard]] const std::string *present_value(
        const std::optional<std::string> &field) {
    return field.has_value() && !field->empty() ? &*field : nullptr;
}

[[nodiscard]] DirectConversationRouteReport fail(
        DirectConversationRouteReport report, DirectRouteFailure failure) {
    report.route.reset();
    report.failure = failure;
    return report;
}

[[nodiscard]] DirectConversationRouteReport resolve_route(
        const ProjectAttachment &attachment,
        const AgentIdentityStatusSnapshot &identity,
        const std::optional<std::filesystem::path> &selected_directory_key) {
    DirectConversationRouteReport report;
    if (!identity.projection_complete || !identity.roster.projection_complete)
        return report;
    const auto &items = identity.roster.discovery.items;

    // Bounded human evidence is collected first so a degraded presentation can
    // still name the sender candidates behind any later failure.
    const AgentManifestIdentityFacts *human_facts = nullptr;
    const std::filesystem::path *human_key = nullptr;
    for (const auto &item : items) {
        if (!is_valid_row(item) || item.role != AgentRole::human) continue;
        ++report.human_candidate_count;
        if (report.human_candidate_keys.size() < human_candidate_key_limit)
            report.human_candidate_keys.push_back(item.directory_key);
        if (report.human_candidate_count == 1) {
            human_facts = &*item.identity;
            human_key = &item.directory_key;
        }
    }

    if (!selected_directory_key || selected_directory_key->empty())
        return fail(std::move(report), DirectRouteFailure::no_selected_agent);
    const AgentManifestDiscoveryItem *selected = nullptr;
    for (const auto &item : items) {
        // The selected key must match one discovered key exactly; a prefix or
        // any other near match never falls back to a different Agent.
        if (item.directory_key == *selected_directory_key) {
            selected = &item;
            break;
        }
    }
    if (!selected)
        return fail(std::move(report), DirectRouteFailure::selected_agent_not_found);
    if (!is_valid_row(*selected))
        return fail(std::move(report), DirectRouteFailure::selected_agent_not_valid);
    if (selected->role == AgentRole::human)
        return fail(std::move(report), DirectRouteFailure::selected_agent_is_human);

    const auto *target_agent_id = present_value(selected->identity->agent_id);
    if (!target_agent_id)
        return fail(std::move(report), DirectRouteFailure::target_agent_id_missing);
    const auto *target_address = present_value(selected->identity->address);
    if (!target_address)
        return fail(std::move(report), DirectRouteFailure::target_address_missing);
    if (report.human_candidate_count == 0)
        return fail(std::move(report), DirectRouteFailure::human_missing);
    if (report.human_candidate_count > 1)
        return fail(std::move(report), DirectRouteFailure::human_ambiguous);
    const auto *human_address = present_value(human_facts->address);
    if (!human_address)
        return fail(std::move(report), DirectRouteFailure::human_address_missing);
    if (*human_address == *target_address)
        return fail(std::move(report),
            DirectRouteFailure::human_target_address_conflict);

    DirectConversationRoute route;
    // Current directory keys and addresses are route authority only.
    route.human_directory_key = *human_key;
    route.target_directory_key = selected->directory_key;
    route.human_address = *human_address;
    route.target_address = *target_address;
    // Stable identity is deliberately independent of both of those.
    route.thread_key = DirectThreadKey{attachment.root(), *target_agent_id};
    route.human_identity = HumanSenderIdentity{human_facts->agent_id,
        human_facts->true_name, human_facts->nickname, human_facts->address,
        human_facts->state, AgentRole::human};
    report.route = std::move(route);
    report.failure = DirectRouteFailure::none;
    return report;
}

} // namespace

DirectConversationRouteReport resolve_direct_conversation_route(
        const ProjectAttachment &attachment,
        const AgentIdentityStatusSnapshot &identity,
        const std::optional<std::filesystem::path> &selected_directory_key) noexcept {
    try {
        return resolve_route(attachment, identity, selected_directory_key);
    } catch (...) {
        // Only evidence allocation can throw here. Fail closed with the default
        // unavailable report rather than terminating or inventing a route.
        return {};
    }
}

} // namespace lingtai::desktop
