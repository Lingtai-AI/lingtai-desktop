#pragma once

#include "agent_projection.h"
#include "agent_preset_summary.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lingtai::desktop {

struct KanbanTokenTotals {
    std::int64_t input = 0;
    std::int64_t output = 0;
    std::int64_t thinking = 0;
    std::int64_t cached = 0;
    std::int64_t api_calls = 0;

    [[nodiscard]] std::int64_t spend() const noexcept {
        return input + output + thinking;
    }

    [[nodiscard]] std::int64_t miss() const noexcept {
        const auto value = input - cached;
        return value < 0 ? 0 : value;
    }
};

struct KanbanLedgerEntry {
    std::string ts;
    std::string model;
    std::string endpoint;
    std::string provider;
    std::string source;
    std::string em_id;
    std::string run_id;
    std::string codex_transfer_mode;
    std::string codex_ws_delta_reason;
    std::int64_t input = 0;
    std::int64_t output = 0;
    std::int64_t thinking = 0;
    std::int64_t cached = 0;

    [[nodiscard]] std::int64_t miss() const noexcept {
        const auto value = input - cached;
        return value < 0 ? 0 : value;
    }
};

struct KanbanDaemonLedgerEntry : KanbanLedgerEntry {
    std::string handle;
    std::string state;
    std::string backend;
};

struct KanbanSessionStats {
    KanbanTokenTotals tokens;
    std::int64_t tool_calls = 0;
    bool has_codex_transfer_mode = false;
    std::int64_t codex_full = 0;
    std::int64_t codex_incremental = 0;
};

struct KanbanProviderSpend {
    std::string name;
    KanbanTokenTotals totals;
};

struct KanbanToolCount {
    std::string name;
    int calls = 0;
    int results = 0;
};

struct KanbanContextStats {
    int entries = 0;
    int system_messages = 0;
    int assistant_messages = 0;
    int user_messages = 0;
    int text_inputs = 0;
    int text_outputs = 0;
    int tool_calls = 0;
    int tool_results = 0;
    std::vector<KanbanToolCount> tool_counts;
};

struct KanbanDaemonCounts {
    int running = 0;
    int total = 0;
};

struct KanbanField {
    std::string label;
    std::string value;
};

enum class KanbanBoardColumn {
    active,
    idle,
    stuck,
    asleep,
    suspended,
};

struct KanbanAgent {
    std::filesystem::path directory_key;
    std::filesystem::path directory_path;
    std::string display_name;
    std::string state;
    bool is_human = false;
    AgentRole role = AgentRole::unknown;
    AgentPresenceKind presence = AgentPresenceKind::unknown;
    std::optional<std::string> model;
    std::optional<std::string> provider;
    std::optional<AgentContextFacts> context;
    KanbanTokenTotals tokens;
    bool tokens_estimated = false;
    std::vector<KanbanField> identity_fields;
    std::vector<KanbanField> llm_fields;
    std::vector<KanbanField> runtime_fields;
    AgentPresetSummary presets;
    std::vector<std::string> capabilities;
    std::vector<KanbanField> admin_fields;
    std::vector<KanbanProviderSpend> providers;
    std::vector<KanbanProviderSpend> daemon_providers;
    std::vector<KanbanLedgerEntry> recent;
    std::vector<KanbanDaemonLedgerEntry> daemon_recent;
    KanbanSessionStats current_session;
    KanbanSessionStats last_session;
    KanbanContextStats context_stats;
    std::vector<std::string> mcp_names;
    KanbanDaemonCounts daemons;
    int daemon_runs_scanned = 0;
    int daemon_runs_total = 0;
    std::vector<std::int64_t> molt_times_ms;
    std::vector<std::int64_t> refresh_times_ms;
};

struct KanbanBoard {
    int agent_count = 0;
    int human_count = 0;
    int active = 0;
    int idle = 0;
    int stuck = 0;
    int asleep = 0;
    int suspended = 0;
    // Always 0 on the Desktop board path: inbox scanning is skipped (same
    // rationale as TUI SkipMailEdges). Retained for shape compatibility.
    int total_mails = 0;
    int running_daemons = 0;
    std::string activity_status;
    std::string network_created;
    std::filesystem::path network_root;
    std::filesystem::path orchestrator_path;
    KanbanTokenTotals network_tokens;
    std::vector<KanbanAgent> agents;
    std::vector<std::string> tree_lines;
};

struct KanbanRefreshMetrics {
    std::uint64_t payload_opens = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t appended_bytes = 0;
    std::uint64_t appended_lines = 0;
    std::uint64_t daemon_enumerations = 0;
    std::uint64_t daemon_records_opened = 0;
    std::uint64_t full_agent_rebuilds = 0;
};

struct KanbanRefreshResult {
    KanbanBoard board;
    KanbanRefreshMetrics metrics;
    bool changed = false;
    bool current = true;
    bool follow_up = false;
};

// Session-owned, descriptor-safe incremental board projection. The first
// refresh builds a complete board. Later refreshes fingerprint fixed leaves,
// consume only complete appended JSONL rows, and retain immutable daemon-run
// summaries. The object is used by one worker at a time; NativeShell owns the
// single-flight/generation policy around it.
class KanbanSnapshotIndex final {
public:
    using RebuildReadCompleteHook = std::function<void(const AgentRow &)>;

    KanbanSnapshotIndex();
    ~KanbanSnapshotIndex();
    KanbanSnapshotIndex(KanbanSnapshotIndex &&) noexcept;
    KanbanSnapshotIndex &operator=(KanbanSnapshotIndex &&) noexcept;
    KanbanSnapshotIndex(const KanbanSnapshotIndex &) = delete;
    KanbanSnapshotIndex &operator=(const KanbanSnapshotIndex &) = delete;

    [[nodiscard]] KanbanRefreshResult refresh(
        const ProjectAttachment &attachment,
        const AgentSnapshot &snapshot,
        bool force = false) noexcept;
    void reset() noexcept;
    [[nodiscard]] const KanbanBoard *current() const noexcept;
    // Deterministic test seam: invoked after a full Agent read and before its
    // incremental cursors are captured. Empty restores production behavior.
    void set_rebuild_read_complete_hook(RebuildReadCompleteHook hook);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] KanbanBoardColumn kanban_column_for_state(
    std::string_view state) noexcept;

[[nodiscard]] std::string_view kanban_column_title(
    KanbanBoardColumn column) noexcept;

[[nodiscard]] std::string derive_ledger_provider(
    std::string_view endpoint, std::string_view model);

[[nodiscard]] KanbanBoard read_kanban_board(
    const ProjectAttachment &attachment,
    const AgentSnapshot &snapshot) noexcept;

} // namespace lingtai::desktop
