#include "ui/agent_roster.h"

#include "styles/palette.h"

#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtGui/QFont>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOptionFocusRect>
#include <QtWidgets/QLabel>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>

#include <utility>

namespace lingtai::desktop {
namespace {

constexpr auto kRosterColumnWidth = 260;
constexpr auto kRowHeight = 62;
constexpr auto kRowHorizontalFrame = 10;
constexpr auto kRowVerticalFrame = 8;

QString path_text(const std::filesystem::path &path) {
    const auto bytes = path.u8string();
    return QString::fromUtf8(
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<qsizetype>(bytes.size()));
}

QString manifest_text(AgentManifestKind kind) {
    switch (kind) {
    case AgentManifestKind::valid: return QStringLiteral("valid");
    case AgentManifestKind::malformed: return QStringLiteral("malformed");
    case AgentManifestKind::unsafe: return QStringLiteral("unsafe");
    }
    return QStringLiteral("malformed");
}

QString role_text(AgentRole role) {
    switch (role) {
    case AgentRole::unknown: return QStringLiteral("unknown");
    case AgentRole::human: return QStringLiteral("human");
    case AgentRole::main: return QStringLiteral("main");
    case AgentRole::agent: return QStringLiteral("agent");
    }
    return QStringLiteral("unknown");
}

QString presence_text(AgentPresenceKind presence) {
    switch (presence) {
    case AgentPresenceKind::unknown: return QStringLiteral("unknown");
    case AgentPresenceKind::alive_human: return QStringLiteral("alive_human");
    case AgentPresenceKind::alive: return QStringLiteral("alive");
    case AgentPresenceKind::stale: return QStringLiteral("stale");
    case AgentPresenceKind::missing: return QStringLiteral("missing");
    case AgentPresenceKind::invalid: return QStringLiteral("invalid");
    case AgentPresenceKind::unavailable: return QStringLiteral("unavailable");
    }
    return QStringLiteral("unknown");
}

QString diagnostic_text(AgentManifestDiagnosticKind diagnostic) {
    switch (diagnostic) {
    case AgentManifestDiagnosticKind::none: return QString();
    case AgentManifestDiagnosticKind::unsafe_symlink:
        return QStringLiteral("unsafe symlink");
    case AgentManifestDiagnosticKind::unreadable:
        return QStringLiteral("unreadable");
    case AgentManifestDiagnosticKind::invalid_json:
        return QStringLiteral("invalid JSON");
    case AgentManifestDiagnosticKind::not_object:
        return QStringLiteral("JSON root is not an object");
    }
    return QStringLiteral("unreadable");
}

QString row_facts(const AgentRow &item) {
    return QStringLiteral("%1 — %2 — %3")
        .arg(manifest_text(item.manifest_kind), role_text(item.role),
            presence_text(item.presence));
}

QString row_accessible(const AgentRow &item) {
    const auto diagnostic = diagnostic_text(item.manifest_diagnostic);
    return diagnostic.isEmpty()
        ? row_facts(item)
        : row_facts(item) + QLatin1Char('\n')
            + QStringLiteral("manifest diagnostic: %1").arg(diagnostic);
}

// A LingTai-owned checkable row button. It keeps the plain checkable-button
// semantics the shell and its tests rely on while painting the selected,
// hover, pressed, and focus states from the shared lib_ui palette
// (`dialogsBgActive` selected, `dialogsBgOver` hover/pressed) rather than a
// QSS clone of Telegram.
class AgentRowButton final : public QPushButton {
public:
    explicit AgentRowButton(QWidget *parent)
    : QPushButton(parent) {
        setFixedHeight(kRowHeight);
        setFocusPolicy(Qt::StrongFocus);
    }

protected:
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *event) override;
};

void AgentRowButton::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const auto selected = isChecked();
    const auto over = isDown() || underMouse();
    painter.fillRect(rect(), selected
        ? st::dialogsBgActive
        : over
            ? st::windowBgRipple
            : st::windowBgOver);

    const auto lines = text().split(QLatin1Char('\n'));
    const auto text_rect = rect().adjusted(
        kRowHorizontalFrame, kRowVerticalFrame,
        -kRowHorizontalFrame, -kRowVerticalFrame);
    const auto primary_height = text_rect.height() / 2;
    const auto primary_rect = QRect(
        text_rect.x(), text_rect.y(), text_rect.width(), primary_height);
    const auto secondary_rect = QRect(
        text_rect.x(), text_rect.y() + primary_height,
        text_rect.width(), text_rect.height() - primary_height);
    constexpr auto flags = Qt::AlignLeft | Qt::AlignVCenter
        | Qt::TextShowMnemonic;

    auto primary_font = font();
    primary_font.setPointSize(13);
    primary_font.setWeight(QFont::DemiBold);
    painter.setFont(primary_font);
    painter.setPen(selected
        ? st::dialogsNameFgActive
        : over
            ? st::dialogsNameFgOver
            : st::dialogsNameFg);
    painter.drawText(primary_rect, flags, lines.value(0));

    auto secondary_font = font();
    secondary_font.setPointSize(10);
    painter.setFont(secondary_font);
    painter.setPen(selected
        ? st::dialogsTextFgActive
        : over
            ? st::dialogsTextFgOver
            : st::dialogsTextFg);
    painter.drawText(secondary_rect, flags, lines.value(1));

    if (hasFocus()) {
        QStyleOptionFocusRect option;
        option.initFrom(this);
        style()->drawPrimitive(
            QStyle::PE_FrameFocusRect, &option, &painter, this);
    }
}

// Telegram activates a focused chats row on Return/Enter and routes it
// through the exact same selection callback a click uses. QPushButton's own
// key handling only activates autoDefault buttons, which these rows never
// are, so a focused valid row's Return/Enter is forwarded through `click()`
// to the existing clicked/selection path.
void AgentRowButton::keyPressEvent(QKeyEvent *event) {
    if ((event->key() == Qt::Key_Return
            || event->key() == Qt::Key_Enter)
        && isEnabled()) {
        click();
        event->accept();
        return;
    }
    QPushButton::keyPressEvent(event);
}

QLabel *make_label(
        QWidget *parent,
        const QString &text,
        const char *object_name,
        int point_size,
        QFont::Weight weight = QFont::Normal) {
    auto *label = new QLabel(text, parent);
    label->setObjectName(object_name);
    label->setTextFormat(Qt::PlainText);
    label->setAccessibleName(text);
    auto font = label->font();
    font.setPointSize(point_size);
    font.setWeight(weight);
    label->setFont(font);
    return label;
}

} // namespace

AgentRoster::AgentRoster(QWidget *parent)
: Ui::RpWidget(parent) {
    setObjectName("lingtai_desktop_sidebar");
    setAccessibleName(QStringLiteral("Workspace navigation"));
    setAccessibleDescription(QStringLiteral(
        "The persistent project and Agent list column."));
    setFixedWidth(kRosterColumnWidth);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    layout->addWidget(make_label(
        this, QStringLiteral("LingTai"), "lingtai_sidebar_brand", 16,
        QFont::DemiBold));
    layout->addWidget(make_label(
        this, QStringLiteral("No project open"), "lingtai_project_root", 11));

    // The compact project actions beside the project identity header. The
    // shell wires their clicks; the owner only composes them.
    auto *actions = new QHBoxLayout;
    actions->setSpacing(6);
    auto *open_button = new QPushButton(
        QStringLiteral("Open Project…"), this);
    open_button->setObjectName("lingtai_open_project_button");
    open_button->setAccessibleName(QStringLiteral("Open Project"));
    open_button->setAccessibleDescription(QStringLiteral(
        "Request a project location. No project is changed by this request."));
    open_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    actions->addWidget(open_button);
    auto *new_button = new QPushButton(
        QStringLiteral("New Project…"), this);
    new_button->setObjectName("lingtai_new_project_button");
    new_button->setAccessibleName(QStringLiteral("New Project"));
    new_button->setAccessibleDescription(QStringLiteral(
        "Creates a new LingTai project and its first Agent through the "
        "canonical TUI, then starts that Agent."));
    new_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    actions->addWidget(new_button);
    layout->addLayout(actions);

    layout->addWidget(make_label(
        this, QStringLiteral("Workspace"), "lingtai_sidebar_workspace_label",
        11, QFont::Medium));

    auto *roster = new Ui::RpWidget(this);
    roster->setObjectName("lingtai_agent_roster");
    roster->setAccessibleName(QStringLiteral("Agent roster"));
    auto *roster_layout = new QVBoxLayout(roster);
    roster_layout->setContentsMargins(0, 0, 0, 0);
    roster_layout->setSpacing(6);
    roster_layout->addWidget(make_label(
        roster, QStringLiteral("Agents"), "lingtai_agent_roster_heading", 12,
        QFont::DemiBold));
    roster_state_ = make_label(
        roster, QString(), "lingtai_agent_roster_state", 10);
    roster_state_->setAccessibleName(QStringLiteral("Agent roster status"));
    roster_layout->addWidget(roster_state_);
    scroll_ = new QScrollArea(roster);
    scroll_->setObjectName("lingtai_agent_roster_scroll");
    scroll_->setAccessibleName(QStringLiteral("Agent roster rows"));
    scroll_->setWidgetResizable(true);
    // The dialog-list surface paints one uniform light-gray field, so the
    // scroll viewport must not fill a raw white Base surface over it.
    scroll_->viewport()->setAutoFillBackground(false);
    roster_layout->addWidget(scroll_, 1);
    auto *rows = new Ui::RpWidget(scroll_);
    rows->setObjectName("lingtai_agent_roster_rows");
    rows->setAccessibleName(QStringLiteral("Agent roster rows"));
    rows_layout_ = new QVBoxLayout(rows);
    rows_layout_->setContentsMargins(0, 0, 0, 0);
    rows_layout_->setSpacing(4);
    rows_layout_->setSizeConstraint(QLayout::SetMinAndMaxSize);
    scroll_->setWidget(rows);
    layout->addWidget(roster, 1);
}

AgentRoster::~AgentRoster() = default;

void AgentRoster::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), st::windowBgOver);
}

void AgentRoster::set_row_click_handler(RowClickHandler handler) {
    row_click_handler_ = std::move(handler);
}

void AgentRoster::update_state_label(const AgentSnapshot &snapshot) {
    roster_state_->setText(snapshot.scan != AgentScanState::complete
        ? QStringLiteral("Roster unavailable")
        : snapshot.items.empty()
            ? QStringLiteral("No Agents found — scan complete")
            : QStringLiteral("%1 Agent(s) — scan complete")
                .arg(snapshot.items.size()));
}

void AgentRoster::update_checked_states(
        const std::optional<std::filesystem::path> &selected_key) {
    const auto selected_text = selected_key ? path_text(*selected_key)
                                            : QString();
    for (auto index = 0; index != rows_layout_->count(); ++index) {
        if (auto *row = qobject_cast<QPushButton *>(
                rows_layout_->itemAt(index)->widget())) {
            row->setChecked(!selected_text.isEmpty()
                && row->property("directory_key").toString() == selected_text);
        }
    }
}

void AgentRoster::set_rows(
        const AgentSnapshot &snapshot,
        const std::optional<std::filesystem::path> &selected_key) {
    update_state_label(snapshot);

    // The visible model is unchanged when every row's identity, facts, and
    // diagnostic match what is already shown. In that case only the checked
    // state may have moved: never rebuild the row tree, so an unchanged
    // one-second projection refresh preserves scroll, focus, and row identity.
    const auto rows_match = snapshot.items.size()
        == visible_snapshot_.items.size();
    auto model_unchanged = rows_match;
    if (rows_match) {
        for (auto index = std::size_t{0}; index != snapshot.items.size();
                ++index) {
            const auto &before = visible_snapshot_.items[index];
            const auto &after = snapshot.items[index];
            if (before.directory_key != after.directory_key
                || before.manifest_kind != after.manifest_kind
                || before.role != after.role
                || before.presence != after.presence
                || before.manifest_diagnostic != after.manifest_diagnostic) {
                model_unchanged = false;
                break;
            }
        }
    }
    visible_snapshot_ = snapshot;
    if (model_unchanged) {
        update_checked_states(selected_key);
        return;
    }

    while (auto *child = rows_layout_->takeAt(0)) {
        delete child->widget();
        delete child;
    }
    for (auto index = std::size_t{0}; index != snapshot.items.size(); ++index) {
        const auto &item = snapshot.items[index];
        auto button_key = path_text(item.directory_key);
        button_key.replace(QLatin1Char('&'), QStringLiteral("&&"));
        auto *row = new AgentRowButton(rows_layout_->parentWidget());
        row->setObjectName(
            QStringLiteral("lingtai_agent_row_%1").arg(index));
        row->setAccessibleName(
            QStringLiteral("Agent %1").arg(path_text(item.directory_key)));
        row->setText(QStringLiteral("%1\n%2").arg(button_key, row_facts(item)));
        row->setAccessibleDescription(row_accessible(item));
        row->setProperty("directory_key", path_text(item.directory_key));
        row->setCheckable(true);
        row->setChecked(selected_key && *selected_key == item.directory_key);
        row->setEnabled(item.manifest_kind == AgentManifestKind::valid);
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        // The selected row resolves its selection color from the same shared
        // palette token its paint uses, so the ownership is observable without
        // replacing any inherited palette role.
        auto row_palette = row->palette();
        row_palette.setColor(QPalette::Highlight, st::dialogsBgActive->c);
        row->setPalette(row_palette);
        const auto key = item.directory_key;
        QObject::connect(row, &QPushButton::clicked, [this, key] {
            if (row_click_handler_) {
                row_click_handler_(key);
            }
        });
        rows_layout_->addWidget(row);
    }
    rows_layout_->addStretch();
}

void AgentRoster::focus_row(
        const std::optional<std::filesystem::path> &key) {
    const auto key_text = key ? path_text(*key) : QString();
    auto *target = static_cast<QPushButton *>(nullptr);
    for (auto index = 0; index != rows_layout_->count(); ++index) {
        auto *row = qobject_cast<QPushButton *>(
            rows_layout_->itemAt(index)->widget());
        if (!row || !row->isEnabled()) continue;
        if (key && !key_text.isEmpty()
            && row->property("directory_key").toString() != key_text) {
            continue;
        }
        target = row;
        break;
    }
    if (target) {
        target->setFocus();
    }
}

} // namespace lingtai::desktop
