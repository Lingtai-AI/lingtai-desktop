#pragma once

#include <QtCore/QString>

// Canonical widget objectName constants for UI tests.
// Keep in sync with tests/ui/OBJECT_NAMES.md.
namespace lingtai::desktop::ui_test {

inline constexpr auto kDesktopWindow = "lingtai_desktop_window";
inline constexpr auto kDesktopBody = "lingtai_desktop_body";
inline constexpr auto kDesktopSidebar = "lingtai_desktop_sidebar";
inline constexpr auto kDesktopContent = "lingtai_desktop_content";

inline constexpr auto kStartupRoute = "lingtai_startup_route";
inline constexpr auto kStartupChooseProject = "lingtai_startup_choose_project";
inline constexpr auto kEmptyWorkspaceRoute = "lingtai_empty_workspace_route";
inline constexpr auto kOpenProjectButton = "lingtai_open_project_button";
inline constexpr auto kOpenProjectNewWindowButton =
    "lingtai_open_project_new_window_button";
inline constexpr auto kProjectRoot = "lingtai_project_root";
inline constexpr auto kProjectSelector = "lingtai_project_selector";
inline constexpr auto kProjectSelectorMenu = "lingtai_project_selector_menu";

inline constexpr auto kAgentRoster = "lingtai_agent_roster";
inline constexpr auto kAgentRosterRows = "lingtai_agent_roster_rows";
inline constexpr auto kAgentRosterScroll = "lingtai_agent_roster_scroll";
inline constexpr auto kAgentDirectory = "lingtai_agent_directory";

inline constexpr auto kAgentDetail = "lingtai_agent_detail";
inline constexpr auto kAgentDetailScroll = "lingtai_agent_detail_scroll";
inline constexpr auto kAgentDetailBack = "lingtai_agent_detail_back";
inline constexpr auto kAgentPagesNav = "lingtai_agent_pages_nav";
inline constexpr auto kAgentPagesHost = "lingtai_agent_pages_host";
inline constexpr auto kPageNavConversation = "lingtai_agent_page_nav_conversation";
inline constexpr auto kPageNavPresets = "lingtai_agent_page_nav_presets";

inline constexpr auto kChatTopBar = "lingtai_chat_top_bar";
inline constexpr auto kSelectedAgentIdentity = "lingtai_selected_agent_identity";
inline constexpr auto kSelectedAgentKey = "lingtai_selected_agent_key";
inline constexpr auto kSelectedAgentStatusRow = "lingtai_selected_agent_status_row";
inline constexpr auto kSelectedAgentStatusDot = "lingtai_selected_agent_status_dot";
inline constexpr auto kSelectedAgentPresentationName =
    "lingtai_selected_agent_presentation_name";
inline constexpr auto kSelectedAgentConversation =
    "lingtai_selected_agent_conversation";
inline constexpr auto kSelectedAgentConversationState =
    "lingtai_selected_agent_conversation_state";

inline constexpr auto kComposer = "lingtai_composer";
inline constexpr auto kComposerControls = "lingtai_composer_controls";
inline constexpr auto kComposerAttachmentButton =
    "lingtai_composer_attachment_button";
inline constexpr auto kComposerAttachmentTray =
    "lingtai_composer_attachment_tray";
inline constexpr auto kComposerInput = "lingtai_composer_input";
inline constexpr auto kComposerSendButton = "lingtai_composer_send_button";
inline constexpr auto kComposerStatus = "lingtai_composer_status";

inline constexpr auto kPresetSummary = "lingtai_selected_agent_preset_summary";
inline constexpr auto kPresetSummaryState =
    "lingtai_selected_agent_preset_summary_state";
inline constexpr auto kPresetSummarySection =
    "lingtai_selected_agent_preset_summary_section";

inline constexpr auto kKanbanPage = "lingtai_kanban_page";

inline constexpr auto kSetupPresetCatalog = "lingtai_setup_preset_catalog";
inline constexpr auto kSetupPresetSearch = "lingtai_setup_preset_search";
inline constexpr auto kSetupPresetContinue = "lingtai_setup_preset_continue";
inline constexpr auto kSetupEditPresetSave = "lingtai_setup_edit_preset_save";
inline constexpr auto kSetupEditPresetName = "lingtai_setup_edit_preset_name";

[[nodiscard]] inline QString objectNameLiteral(const char *name) {
    return QString::fromUtf8(name);
}

} // namespace lingtai::desktop::ui_test
