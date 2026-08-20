#pragma once

#include "agent_projection.h"
#include "ui/rp_widget.h"

#include <QtCore/QString>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

class QLabel;
class QPushButton;
class QScrollArea;

namespace lingtai::desktop {

[[nodiscard]] QString friendly_agent_role_text(AgentRole role);
[[nodiscard]] QString friendly_agent_lifecycle_text(const AgentRow &item);
[[nodiscard]] QString friendly_agent_presence_text(
    AgentPresenceKind presence);

// The virtual Agent rows surface. It is forward-declared here so AgentRoster
// owns it through a typed pointer; the definition (row model, selected key,
// and row paint) lives in agent_roster.cpp.
class AgentRowsCanvas;

// The persistent responsive left project/Agent list column. It owns the
// project identity header, the compact Open Project action, and the
// scrollable Agent rows; the selected-content pane lives outside this owner.
// Rows are a fixed 62px with 10px/8px framing and show one primary name line
// plus one compact manifest/role/presence state line. Unselected rows stay
// transparent on the sidebar canvas; hover uses `windowBgRipple`, and the
// selected row uses a pale `windowBgActive` tint over `windowBg`. Secondary
// role · status ink is a slightly darkened `windowSubTextFg` so it stays
// muted but readable.
//
// The visible rows omit the human pseudo-agent: the shared `AgentSnapshot`
// keeps the human for routing/mailbox/detail truth, but the roster never
// renders it as a row, and the status label counts only the visible rows.
// `set_rows` swaps the virtual canvas model only when the visible row set
// actually changed; an unchanged projection refresh only moves the selected
// state, so scroll, focus, and row identity survive the shell's one-second
// refresh.
class AgentRoster final : public Ui::RpWidget {
public:
    using RowClickHandler = std::function<void(const std::filesystem::path &)>;

    explicit AgentRoster(QWidget *parent);
    ~AgentRoster() override;

    AgentRoster(const AgentRoster &) = delete;
    AgentRoster &operator=(const AgentRoster &) = delete;

    void set_rows(const AgentSnapshot &snapshot,
        const std::optional<std::filesystem::path> &selected_key);
    // Telegram-style unseen inbound counts keyed by directory_key UTF-8.
    // Updates independently of set_rows so the 1s refresh can move badges
    // without rebuilding the row model.
    void set_unseen_counts(std::unordered_map<std::string, int> counts);
    void set_row_click_handler(RowClickHandler handler);
    // The shell owns the actual column width. This setter applies it and keeps
    // the project/Agent header in sync with the avatar-only compact state.
    void set_roster_width(int width);
    void set_project_display_name(const QString &name);
    // Keyboard focus: focuses the enabled row for `key` when present,
    // otherwise the first enabled (valid-manifest) row. The narrow OneColumn
    // Back path hands keyboard navigation back to the roster through this.
    void focus_row(
        const std::optional<std::filesystem::path> &key = std::nullopt);

private:
    void paintEvent(QPaintEvent *event) override;
    void update_narrow_mode();
    void update_state_label(const AgentSnapshot &snapshot);

    QLabel *roster_heading_ = nullptr;
    QLabel *roster_state_ = nullptr;
    QPushButton *project_selector_ = nullptr;
    QString project_display_name_ = QStringLiteral("LingTai");
    QScrollArea *scroll_ = nullptr;
    AgentRowsCanvas *canvas_ = nullptr;
    AgentSnapshot visible_snapshot_;
};

} // namespace lingtai::desktop
