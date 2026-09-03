#pragma once

#include "agent_preset_summary.h"
#include "attachment_selection.h"
#include "conversation_session.h"
#include "direct_conversation_attachment_actions.h"
#include "direct_conversation_history.h"
#include "kanban_model.h"
#include "message_reactions.h"
#include "native_shell.h" // for AgentDetailPage enum
#include "runtime_options.h"

#include "ui/palette_action_button.h"
#include "ui/rp_widget.h"

#include <QtWidgets/QLabel>
#include <QtWidgets/QScrollArea>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class QPalette;
class QPushButton;
class QGridLayout;
class QTextEdit;
class QTimer;
class QWidget;

namespace Ui {
class InputField;
class RoundButton;
class RpWidget;
} // namespace Ui

namespace lingtai::desktop {

enum class ComposerNoticeKind { success, warning, error };

class ConversationSurface;
class KanbanPage;

// Resize-fit for the selected-Agent chat top bar: keep the Sidebar-matching
// Role · Status line under the name, allocate remaining width to the identity
// column, and elide title + status text. Never hide the status row for width.
void fit_selected_agent_chat_top_bar(QWidget *top_bar, int detail_width);

// Refactors NativeShell's "selected agent detail" into a dedicated widget
// (conversation + composer + secondary pages) so tests can instantiate/render
// the detail area without constructing the whole shell.
class AgentDetailView final : public Ui::RpWidget {
    Q_OBJECT

public:
    explicit AgentDetailView(
        RuntimeOptions runtime_options,
        QScrollArea *outer_scroll,
        QWidget *parent = nullptr);
    ~AgentDetailView() override = default;

    [[nodiscard]] AgentDetailPage page() const noexcept { return page_; }

    // Telegram-like secondary page switch: only one of Conversation / Presets
    // / Kanban is visible at a time.
    void set_page(AgentDetailPage page);

    // Responsive fitting owner: adjusts top bar, composer lane, and kanban
    // sizing based on the detail column width derived by the shell.
    void set_detail_width(int detail_width);

    // Re-apply widget-level chrome (palettes / borders) after theme changes.
    void refresh_chrome();

    // Conversation UI (read-only surface + composer enablement).
    void render_conversation(
        const QString &them,
        const DirectConversationHistory &history,
        bool selection_present,
        bool conversation_route_available,
        const std::unordered_map<std::string, MessageReactions> &reactions = {},
        const QString &main_agent_name = {},
        const std::vector<ConversationSessionEntry> &session_events = {},
        bool session_log_present = false,
        std::optional<ConversationPresentationRevision> revision = std::nullopt);

    [[nodiscard]] ConversationVerboseLevel conversation_verbose_level() const;

    // Persistent composer runtime footer: selected-Agent `model` and
    // `Context used / window (pct)`, fed on every accepted roster projection
    // refresh independent of conversation/mail render invalidation. Both
    // segments arrive pre-formatted (including honest "Model —"/"Context —"
    // placeholders); this view only owns layout, elision, and accessible/
    // tooltip truth. Passing two empty strings clears the footer (no valid
    // Agent selected).
    void set_runtime_footer(
        const QString &model_segment, const QString &context_segment);

    // Rebuilds verbose interleaves from freshly loaded session events without
    // replacing the cached mail rows.
    void apply_conversation_session_events(
        const std::vector<ConversationSessionEntry> &session_events);

    void set_conversation_detail_loading(bool loading);

    void scroll_conversation_to_bottom();

    [[nodiscard]] const std::vector<AcceptedAttachment> &pending_attachments()
        const noexcept { return pending_attachments_; }
    [[nodiscard]] bool has_pending_attachments() const noexcept {
        return !pending_attachments_.empty();
    }
    void merge_pending_attachments(
        const std::vector<std::filesystem::path> &selected_paths);
    void remove_pending_attachment(std::size_t index);
    void clear_pending_attachments();
    void clear_attachment_errors();
    void mark_attachment_error(std::size_t index, const QString &message);
    void show_composer_notice(
        const QString &message, ComposerNoticeKind kind);
    void clear_composer_notice();

    // Preset summary UI (read-only preset catalog + state line).
    void render_preset_summary(
        const std::optional<AgentPresetSummary> &summary);

    // Kanban board UI (widget refresh + scroll restoration).
    void render_kanban(
        const KanbanBoard &board,
        const std::optional<std::filesystem::path> &selected_agent_key);

signals:
    // Composer + slash UI actions (the shell performs the actual side-effects
    // and state reads).
    void send_message_requested(const QString &text);
    void attachment_selection_requested();
    void attachment_action_requested(
        const DirectConversationAttachmentRequest &request, bool reveal);
    void back_requested();
    void start_requested();
    void sleep_requested();

    // Reload session events when verbose detail is toggled on.
    void conversation_verbose_changed(ConversationVerboseLevel level);

    // View-owned page switching.
    void page_changed(AgentDetailPage previous, AgentDetailPage current);

    // Kanban detail selection.
    void kanban_agent_selected(
        const std::filesystem::path &directory_key);

private:
    RuntimeOptions runtime_options_;
    QScrollArea *outer_scroll_ = nullptr;

    AgentDetailPage page_ = AgentDetailPage::conversation;

    // Owns rpl subscriptions for the composer/slash UI wiring.
    rpl::lifetime composer_lifetime_;

    // View-owned widget tree (constructed in later refactor steps).
    QWidget *chat_top_bar_ = nullptr;
    QLabel *selected_agent_key_ = nullptr;
    QPushButton *detail_back_button_ = nullptr;
    QLabel *conversation_heading_ = nullptr;

    Ui::RpWidget *composer_ = nullptr;
    Ui::InputField *composer_input_ = nullptr;
    Ui::RoundButton *composer_send_button_ = nullptr;
    QPushButton *composer_attachment_button_ = nullptr;
    QWidget *composer_attachment_tray_ = nullptr;
    QGridLayout *composer_attachment_layout_ = nullptr;
    QLabel *composer_status_ = nullptr;
    QTimer *composer_notice_timer_ = nullptr;
    // Persistent composer runtime footer (repurposes the former conversation-
    // state anchor; object name stays "lingtai_selected_agent_conversation_state").
    QLabel *runtime_footer_ = nullptr;
    PaletteActionButton *conversation_detail_button_ = nullptr;
    ConversationSurface *conversation_surface_ = nullptr;

    Ui::RpWidget *pages_host_ = nullptr;
    Ui::RpWidget *pages_nav_ = nullptr;
    std::vector<QPushButton *> page_nav_buttons_;
    std::vector<QWidget *> secondary_pages_;
    QLabel *preset_summary_state_ = nullptr;
    QWidget *kanban_page_holder_ = nullptr;
    KanbanPage *kanban_page_ = nullptr;

    // Preset-scope facts labels (under the page host).
    std::vector<QLabel *> source_facts_labels_;

    // Composer slash popup object name remains stable across the extraction.
    static constexpr const char *kSlashPopupObjectName =
        "lingtai_slash_command_popup";

    // Stubs: detailed implementations move in follow-up plan steps.
    void refresh_composer_enablement(bool composer_eligible);
    void rebuild_attachment_cards();
    void reflow_attachment_cards(int available_width);
    void refresh_conversation_detail_button();
    void refresh_runtime_footer_fit();

    QString runtime_footer_full_text_;
    int runtime_footer_available_width_ = 0;
    bool session_log_present_ = false;
    bool conversation_route_available_ = false;
    bool composer_eligible_ = false;
    int composer_detail_width_ = 0;
    std::vector<AcceptedAttachment> pending_attachments_;
    std::vector<QString> attachment_errors_;
};

} // namespace lingtai::desktop
