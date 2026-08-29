#pragma once

#include "agent_lifecycle.h"
#include "agent_projection.h"
#include "agent_setup_store.h"
#include "conversation_session.h"
#include "conversation_unread.h"
#include "direct_conversation_route.h"
#include "direct_conversation_attachment_actions.h"
#include "injected_mail_journal.h"
#include "kanban_model.h"
#include "message_reactions.h"
#include "project_creation.h"
#include "project_setup_wizard.h"
#include "runtime_options.h"
#include "ui/agent_roster.h"
#include "workspace_selection.h"

// Vendored composer widget headers pull in ui/text/text_entity.h, whose
// aggregate brace init omits a field. Targets that include these headers (the
// shell implementation and its behavior test) build with -Werror, which would
// promote that vendored warning to an error. Vendored sources are out of
// scope, so scope the suppression to exactly this include boundary.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#pragma GCC diagnostic pop

#include <rpl/lifetime.h>

#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QLabel;
class QPushButton;
class QTimer;
class QWidget;

namespace Ui {
class PlainShadow;
class RpWidget;
class RpWindow;
} // namespace Ui

namespace lingtai::desktop {

class KanbanPage;
class AgentDetailView;
struct DirectMailPublishedMessage;

enum class ProjectOpenDisposition {
    opened,
    failed,
};

struct ProjectOpenOutcome {
    ProjectOpenDisposition disposition = ProjectOpenDisposition::failed;
    ProjectPathFailure failure = ProjectPathFailure::none;
};

// Conversation is the default selected-Agent surface. Presets and Kanban are
// slash destinations behind the same secondary page host, so only one content
// surface shows at a time.
enum class AgentDetailPage {
    conversation,
    presets,
    kanban,
};

// C5-owned native composition. C1's WorkspaceSelectionState remains the only
// active project/Agent truth; an open request proposes no state transition.
class NativeShell final {
public:
    using OpenProjectRequestHandler = std::function<void()>;
    using AttachmentPicker =
        std::function<std::vector<std::filesystem::path>()>;
    using AttachmentExternalAction =
        std::function<bool(const std::filesystem::path &, bool reveal)>;
    using KanbanRefreshFunction = std::function<KanbanRefreshResult(
        KanbanSnapshotIndex &,
        const ProjectAttachment &,
        const AgentSnapshot &,
        bool force)>;
    using MailboxSnapshotReadFunction = std::function<DirectMailboxSnapshot(
        const DirectMailboxRequest &)>;
    using AgentSetupSaveFunction = std::function<AgentSetupSaveResult(
        const AgentSetupStore &, const AgentSetupState &,
        const AgentSetupDraft &)>;

    explicit NativeShell(RuntimeOptions runtime_options = {});
    ~NativeShell();

    NativeShell(const NativeShell &) = delete;
    NativeShell &operator=(const NativeShell &) = delete;

    void show();
    void show_offscreen();
    void set_open_project_request_handler(OpenProjectRequestHandler handler);
    void set_open_project_in_new_window_request_handler(
        OpenProjectRequestHandler handler);
    void set_attachment_picker(AttachmentPicker picker);
    void set_attachment_external_action(AttachmentExternalAction action);
    // Deterministic worker seam used by the native-shell contract to hold or
    // fail a refresh. Empty restores the production incremental reader.
    void set_kanban_refresh_function(KanbanRefreshFunction refresh);
    // Deterministic worker seam used by the native-shell contract to hold a
    // mailbox generation. Empty restores the production snapshot reader.
    void set_mailbox_snapshot_read_function(MailboxSnapshotReadFunction read);
    // Deterministic UI-contract seam. Empty restores AgentSetupStore::save.
    void set_agent_setup_save_function(AgentSetupSaveFunction save);
    void request_new_project_at(const std::filesystem::path &destination);
    // Application composition's one concrete fallback interpreter, used only
    // when a selected Agent's own `init.json.venv_path` is absent or its
    // platform Python does not exist. Defaults to an empty path.
    void set_agent_start_fallback_python(std::filesystem::path fallback_python);
    // Deterministic lifecycle seam for the native-shell contract. Empty/default
    // production dependencies restore real process, launch, clock, and timer
    // adapters; replacing it cancels only the old controller's pending UI
    // delivery and never attempts lifecycle rollback.
    void set_agent_lifecycle_dependencies(
        AgentLifecycleDependencies dependencies);
    [[nodiscard]] ProjectOpenOutcome open_project(
        const std::filesystem::path &selected_directory,
        const std::optional<std::filesystem::path> &agent_relative_directory
            = std::nullopt);

    [[nodiscard]] Ui::RpWindow &window() noexcept;
    [[nodiscard]] const Ui::RpWindow &window() const noexcept;
    [[nodiscard]] const WorkspaceSelectionState &selection_state()
        const noexcept;

    // True once the window, body, sidebar, and content are constructed,
    // correctly named, and actually shown offscreen. `main.cpp`'s `--smoke`
    // entry point is the sole caller; this is real product readiness, not a
    // public test seam.
    [[nodiscard]] bool smoke_ready() const noexcept;

private:
    void request_open_project();
    void request_open_project_in_new_window();
    void request_new_project();
    void handle_presets_finished(PresetCatalogLoadResult result);
    void handle_creation_finished(ProjectCreationResult result);
    void handle_first_agent_launch_finished(AgentLifecycleResult result);
    void handle_create_and_start();
    void request_existing_agent_setup();
    void handle_save_existing_setup();
    void handle_cancel_bootstrap();
    void handle_browse_destination();
    void set_bootstrap_actions_enabled(bool enabled);
    void set_bootstrap_status(const QString &text);
    void show_setup_wizard(const std::vector<PresetEntry> &presets);
    void show_existing_setup_wizard(AgentSetupState state);
    void populate_setup_preset_catalog(
        const std::vector<PresetEntry> &presets,
        const QString &preferred_reference = {},
        bool include_current_fallback = false);
    void hydrate_existing_preset_policy(const QString &proposed_default);
    void hide_setup_wizard();
    [[nodiscard]] bool in_project_setup() const;
    void refresh_route();
    void render_roster();
    [[nodiscard]] DirectMailboxRequest mailbox_request() const;
    void request_mailbox_snapshot();
    void start_mailbox_snapshot_job(DirectMailboxSnapshotIndex::Job job);
    void apply_mailbox_snapshot_job(
        DirectMailboxSnapshotIndex::Job job,
        DirectMailboxSnapshot snapshot,
        DirectMailboxFingerprint fingerprint_after);
    void invalidate_mailbox_snapshot();
    void record_published_message(const DirectConversationRoute &route,
        const DirectMailPublishedMessage &published);
    void reconcile_published_messages(const DirectMailboxRequest &request,
        const DirectMailboxSnapshot &snapshot);
    struct SelectedConversationView final {
        // Borrowed from the current immutable snapshot and consumed only by
        // the synchronous render call; never retained across snapshot swaps.
        const DirectConversationHistory *history = nullptr;
        DirectMailboxSnapshot::HistoryRevision revision;
        bool snapshot_ready = false;
    };
    struct ProjectedConversationView final {
        const DirectConversationHistory *history = nullptr;
        DirectMailboxSnapshot::HistoryRevision revision;
    };
    [[nodiscard]] ProjectedConversationView project_conversation_history(
        const DirectConversationRoute &route,
        const DirectConversationHistory &authoritative,
        std::uint64_t authoritative_revision);
    [[nodiscard]] SelectedConversationView refresh_unseen_badges();
    void render_conversation(
        std::optional<SelectedConversationView> history = std::nullopt);
    void clear_session_events_cache();
    void invalidate_session_events_cache();
    [[nodiscard]] const std::vector<ConversationSessionEntry> &
        cached_session_events_for(
            const DirectConversationRoute &route,
            bool force_reload = false);
    void request_session_events(bool force);
    void apply_session_events(
        std::uint64_t generation,
        std::vector<ConversationSessionEntry> entries);
    void handle_conversation_verbose_changed(ConversationVerboseLevel level);
    // Reapplies the generated light or canonical Telegram Night palette after
    // the host appearance changes, then refreshes palette-backed descendants.
    void refresh_system_palette();
    void render_agent_preset_summary();
    void render_kanban();
    // Starts or reuses an off-UI-thread board read. When a warm cache exists
    // for the active project, the page paints immediately and refresh runs
    // in the background. `force` coalesces behind an in-flight read and then
    // rebuilds from current project/Agent truth.
    void request_kanban_board(bool force);
    void apply_kanban_board(
        std::uint64_t generation, KanbanRefreshResult result);
    void invalidate_kanban_cache();
    void maybe_warm_kanban_cache();
    void handle_kanban_agent_selected(const std::filesystem::path &directory_key);
    void reset_composer();
    void handle_attachment_selection();
    void handle_attachment_action(
        const DirectConversationAttachmentRequest &request, bool reveal);
    void handle_send_message();
    void handle_agent_selection(const std::filesystem::path &directory_key);
    // The one Desktop-owned lifecycle slash owner for `/sleep`, `/suspend`,
    // `/cpr`, `/clear`, and `/refresh`, including their supported Main/all
    // forms. No lifecycle path invokes the TUI.
    void handle_lifecycle_command(const std::string &name,
        const std::string &args);
    void handle_lifecycle_finished(AgentLifecycleResult result);
    // TUI `/btw`, `/insights`, `/goal`, `/export`, and `/molt`: stay on the
    // conversation and write the same `.inquiry` / `.prompt` /
    // `.notification/system.json` signals. Returns true when the name is
    // one of those commands.
    bool handle_prompt_command(const std::string &name,
        const std::string &args);
    [[nodiscard]] std::string lifecycle_generation() const noexcept;
    void bump_lifecycle_generation() noexcept;
    void render_agent_sleep_status();
    void handle_request_sleep();
    void render_agent_start_status();
    void handle_start_agent();
    // Telegram's one mode recompute: derives OneColumn vs Normal from the
    // body's available column space on every real resize, and shows exactly
    // one full-width surface (roster or selected detail + Back) below the
    // two-surface threshold.
    void recompute_layout(int body_width);
    // Telegram's OneColumn history-back path: the narrow detail returns to
    // the roster and drops the selection. No-op outside the narrow detail.
    void handle_detail_back();
    // The one responsive chat-top-bar fit, owned by the same recompute width
    // owner: allocates remaining width to the identity column and elides the
    // presentation name plus Role · Status line under it. The status row is
    // never hidden for width; action captions may still drop under pressure.
    void update_top_bar_fit(int detail_width);
    // Recomputes the one bounded composer lane's width from the actual detail
    // width just derived by `recompute_layout`: near-full at the narrow
    // minimum, a visibly narrower symmetric-centered lane at wide detail.
    void update_composer_width(int detail_width);
    void fit_kanban_page(int detail_width);
    // Switches the selected-Agent detail to exactly one page: the chat
    // (conversation) by default, or the one retained read-only source
    // (Presets), so only one content surface dominates at a time.
    void show_detail_page(AgentDetailPage page);
    [[nodiscard]] ProjectOpenOutcome show_open_error(
        ProjectPathFailure failure,
        std::string message);

    WorkspaceSelectionState selection_state_;
    RuntimeOptions runtime_options_;
    std::unique_ptr<Ui::RpWindow> window_;
    // The one no-project launch canvas. It owns only the first project CTA;
    // the existing roster/detail workspace replaces it after project open.
    QWidget *startup_route_ = nullptr;
    // The persistent left 260px project/Agent list column. It owns the
    // project identity header, the compact Open/New Project actions, and the
    // Agent rows; the shell wires its row clicks and the action buttons.
    AgentRoster *agent_roster_ = nullptr;
    // The one 8px-wide semantic drag handle between the roster column and its
    // one-pixel shadow separator; it owns no state beyond the runtime-only
    // resize ratio below.
    QWidget *roster_resize_handle_ = nullptr;
    // The runtime-only roster width ratio re-derived from real drags, clamped
    // to 22%-30% of the body; never persisted.
    double roster_width_ratio_ = 0.26;
    // The flexible right content pane beside the roster, hidden in OneColumn
    // roster mode and full-width in OneColumn detail mode.
    Ui::RpWidget *content_ = nullptr;
    Ui::PlainShadow *separator_ = nullptr;
    // The one compact palette-owned Back control in the detail header, visible
    // only in Telegram's narrow OneColumn detail view.
    QPushButton *detail_back_button_ = nullptr;
    // Stable pointer to the extracted selected-Agent detail widget.
    AgentDetailView *detail_view_ = nullptr;
    // Stable pointers to the selected-Agent chat top bar and its status label,
    // retained so the one responsive fit measure in `recompute_layout` can
    // elide the identity column against the actual detail width.
    QWidget *chat_top_bar_ = nullptr;
    QLabel *selected_agent_key_ = nullptr;
    // The one bounded composer lane, retained so the same body-resize owner
    // that drives the responsive sidebar/header recomputes its width from
    // the actual detail width on every real resize.
    Ui::RpWidget *composer_ = nullptr;
    Ui::RpWidget *empty_route_ = nullptr;
    Ui::RpWidget *project_route_ = nullptr;
    Ui::RpWidget *open_error_surface_ = nullptr;
    AgentSnapshot agents_;
    MessageReactionStore reaction_store_;
    ConversationUnreadState conversation_unread_;
    InjectedMailJournal injected_mail_journal_;
    // One complete shared human-mailbox projection for all current Agent
    // routes. The UI thread performs only fixed-count fingerprints; the
    // descriptor scan and JSON parse run in one detached single-flight job.
    DirectMailboxSnapshotIndex mailbox_snapshot_index_;
    struct PendingPublishedMessage final {
        std::filesystem::path project_root;
        std::filesystem::path agent_key;
        DirectConversationMessage message;
    };
    std::vector<PendingPublishedMessage> pending_published_messages_;
    std::uint64_t published_messages_revision_ = 0;
    struct ConversationHistoryProjection final {
        std::filesystem::path project_root;
        std::filesystem::path agent_key;
        std::uint64_t authoritative_revision = 0;
        std::uint64_t published_revision = 0;
        std::uint64_t presentation_revision = 0;
        DirectConversationHistory history;
    };
    std::optional<ConversationHistoryProjection> conversation_history_projection_;
    std::uint64_t next_conversation_presentation_revision_ = 0;
    struct MailboxLoadToken {
        std::atomic<bool> cancelled{false};
    };
    std::shared_ptr<MailboxLoadToken> mailbox_load_token_
        = std::make_shared<MailboxLoadToken>();
    OpenProjectRequestHandler open_project_request_handler_;
    OpenProjectRequestHandler open_project_in_new_window_request_handler_;
    AttachmentPicker attachment_picker_;
    AttachmentExternalAction attachment_external_action_;
    KanbanRefreshFunction kanban_refresh_function_;
    MailboxSnapshotReadFunction mailbox_snapshot_read_function_ =
        read_direct_mailbox_snapshot;
    std::filesystem::path agent_start_fallback_python_;
    // One serial nonblocking Desktop lifecycle owner. Its structured result
    // carries the start-time project/generation for stale-delivery rejection.
    std::unique_ptr<AgentLifecycleController> lifecycle_controller_;
    // The one current selection-generation epoch, bumped on every real
    // project open, successful Agent selection, and Back/selection clear.
    std::uint64_t selection_generation_ = 0;
    // One serial worker for Desktop-owned preset discovery and the bounded
    // first-project creation transaction.
    std::unique_ptr<ProjectCreationRunner> creation_runner_;
    std::optional<std::filesystem::path> created_project_root_;
    enum class SetupMode { none, create_project, rerun_existing };
    SetupMode setup_mode_ = SetupMode::none;
    std::optional<AgentSetupState> existing_setup_state_;
    std::vector<PresetEntry> existing_setup_catalog_;
    QString existing_setup_selected_reference_;
    QJsonObject existing_setup_selected_manifest_;
    AgentSetupSaveFunction agent_setup_save_function_;
    // The one in-window setup route, built once and shared by explicit New
    // Project creation and selected-Agent rerun modes.
    ProjectSetupWizard *setup_route_ = nullptr;
    bool setup_route_visible_ = false;
    Ui::RpWidget *bootstrap_status_surface_ = nullptr;
    // Owns the vendored composer input's Enter-to-send subscription for the
    // whole shell lifetime. The button click uses the button's own lifetime.
    rpl::lifetime submits_lifetime_;
    // Owns the single body-resize mode recompute subscription for the whole
    // shell lifetime; Telegram derives OneColumn vs Normal on every chats
    // resize, so Desktop's one recompute rides the same event stream.
    rpl::lifetime layout_lifetime_;
    // True while a New Project worker operation is pending (preset discovery
    // or staged creation). While true the New Project and Open Project actions are
    // disabled so duplicate activation is impossible.
    bool bootstrap_pending_ = false;
    // View-scoped: exists only for the shell's own lifetime, re-invokes the
    // same stateless snapshot reader every second, and owns no cursor/offset
    // state of its own.
    QTimer *activity_timer_ = nullptr;
    // The one compact selected-Agent page navigation: exactly one nav control
    // per AgentDetailPage, wired by the shell to `show_detail_page`. The
    // secondary section owners that the nav reveals are captured in the
    // same construction order so `show_detail_page` can show exactly one.
    std::vector<QPushButton *> page_nav_buttons_;
    std::vector<QWidget *> secondary_pages_;
    AgentDetailPage current_detail_page_ = AgentDetailPage::conversation;
    KanbanPage *kanban_page_ = nullptr;
    // Session-only warm board for the active project. Cleared on project
    // change. Lets /kanban paint instantly while a background refresh runs.
    std::optional<KanbanBoard> kanban_cache_;
    std::filesystem::path kanban_cache_root_;
    std::shared_ptr<KanbanSnapshotIndex> kanban_index_
        = std::make_shared<KanbanSnapshotIndex>();
    // Bumped to drop stale async results after project change, force reload,
    // or shell teardown.
    std::uint64_t kanban_load_generation_ = 0;
    bool kanban_load_inflight_ = false;
    std::uint64_t kanban_running_generation_ = 0;
    bool kanban_follow_up_ = false;
    bool kanban_follow_up_force_ = false;
    int kanban_warm_ticks_ = 0;
    // Shared cancel token so a detached board read never touches a destroyed
    // shell. Set cancelled in the destructor before members tear down.
    struct KanbanLoadToken {
        std::atomic<bool> cancelled{false};
    };
    std::shared_ptr<KanbanLoadToken> kanban_load_token_
        = std::make_shared<KanbanLoadToken>();

    struct SessionEventsCache final {
        std::filesystem::path project_root;
        std::filesystem::path agent_key;
        std::time_t mtime = 0;
        std::int64_t size = 0;
        std::vector<ConversationSessionEntry> entries;
    };
    SessionEventsCache session_events_cache_;
    bool session_events_force_reload_ = false;
    std::uint64_t session_events_load_generation_ = 0;
    std::uint64_t session_events_revision_ = 0;
    bool session_events_load_inflight_ = false;
    struct SessionEventsLoadToken {
        std::atomic<bool> cancelled{false};
    };
    std::shared_ptr<SessionEventsLoadToken> session_events_load_token_
        = std::make_shared<SessionEventsLoadToken>();

    struct ConversationRenderKey final {
        std::uint64_t selection = 0;
        std::string route;
        QString them;
        QString compact;
        std::uint64_t history = 0;
        std::uint64_t reactions = 0;
        std::uint64_t injected = 0;
        std::uint64_t session_events = 0;
        std::time_t session_mtime = 0;
        std::int64_t session_size = 0;
        bool snapshot_ready = false;
        bool session_present = false;
        bool session_loading = false;
        ConversationVerboseLevel verbose = ConversationVerboseLevel::off;

        friend bool operator==(
            const ConversationRenderKey &, const ConversationRenderKey &)
            = default;
    };
    std::optional<ConversationRenderKey> conversation_render_key_;
    std::uint64_t receipts_history_revision_ = 0;
    std::uint64_t seen_injected_revision_ = 0;

    // Appearance/palette storms re-enter through ApplicationPaletteChange while
    // setPalette runs; never nest a second refresh or a sync conversation rebuild.
    bool refreshing_system_palette_ = false;
    std::uint64_t palette_refresh_generation_ = 0;
};

} // namespace lingtai::desktop
