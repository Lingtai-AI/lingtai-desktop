#pragma once

#include "agent_projection.h"
#include "ui/rp_widget.h"

#include <filesystem>
#include <functional>
#include <optional>

class QLabel;
class QScrollArea;
class QVBoxLayout;

namespace lingtai::desktop {

// The persistent responsive left project/Agent list column. It owns the
// project identity header, the compact Open/New Project actions, and the
// scrollable Agent rows; the selected-content pane lives outside this owner.
// Rows are a fixed 62px with 10px/8px framing and show one primary name line
// plus one compact manifest/role/presence state line. The list surface and
// every row state are painted from the shared lib_ui palette (`windowBgOver`
// list field, `windowBgRipple` hover, `dialogsBgActive` selected).
//
// The visible rows omit the human pseudo-agent: the shared `AgentSnapshot`
// keeps the human for routing/mailbox/detail truth, but the roster never
// renders it as a row, and the status label counts only the visible rows.
// `set_rows` rebuilds the row tree only when the visible row set actually
// changed; an unchanged projection refresh only updates checked state, so
// scroll, focus, and row identity survive the shell's one-second refresh.
class AgentRoster final : public Ui::RpWidget {
public:
    using RowClickHandler = std::function<void(const std::filesystem::path &)>;

    explicit AgentRoster(QWidget *parent);
    ~AgentRoster() override;

    AgentRoster(const AgentRoster &) = delete;
    AgentRoster &operator=(const AgentRoster &) = delete;

    void set_rows(const AgentSnapshot &snapshot,
        const std::optional<std::filesystem::path> &selected_key);
    void set_row_click_handler(RowClickHandler handler);
    // Keyboard focus: focuses the enabled row for `key` when present,
    // otherwise the first enabled (valid-manifest) row. The narrow OneColumn
    // Back path hands keyboard navigation back to the roster through this.
    void focus_row(
        const std::optional<std::filesystem::path> &key = std::nullopt);

private:
    void paintEvent(QPaintEvent *event) override;
    void update_state_label(const AgentSnapshot &snapshot);
    void update_checked_states(
        const std::optional<std::filesystem::path> &selected_key);

    RowClickHandler row_click_handler_;
    QLabel *roster_state_ = nullptr;
    QScrollArea *scroll_ = nullptr;
    QVBoxLayout *rows_layout_ = nullptr;
    AgentSnapshot visible_snapshot_;
};

} // namespace lingtai::desktop
