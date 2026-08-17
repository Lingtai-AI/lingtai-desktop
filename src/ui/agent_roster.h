#pragma once

#include "agent_projection.h"
#include "ui/rp_widget.h"

#include <QtCore/QString>

#include <filesystem>
#include <functional>
#include <optional>

class QLabel;
class QPushButton;
class QScrollArea;

namespace lingtai::desktop {

[[nodiscard]] QString friendly_agent_role_text(AgentRole role);
[[nodiscard]] QString friendly_agent_presence_text(
    AgentPresenceKind presence);

// The virtual Agent rows surface. It is forward-declared here so AgentRoster
// owns it through a typed pointer; the definition (row model, selected key,
// and row paint) lives in agent_roster.cpp.
class AgentRowsCanvas;

// The persistent responsive left project/Agent list column. It owns the
// project identity header, the compact Open/New Project actions, and the
// scrollable Agent rows; the selected-content pane lives outside this owner.
// Rows are a fixed 62px with 10px/8px framing and show one primary name line
// plus one compact manifest/role/presence state line. The list surface and
// every row state are painted from the shared lib_ui palette (`windowBgOver`
// list field and neutral selected surface, `windowBgRipple` hover, and a
// narrow `dialogsBgActive` leading accent cue on the selected row).
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
    QPushButton *new_project_button_ = nullptr;
    QString project_display_name_ = QStringLiteral("LingTai");
    QScrollArea *scroll_ = nullptr;
    AgentRowsCanvas *canvas_ = nullptr;
    AgentSnapshot visible_snapshot_;
};

} // namespace lingtai::desktop
