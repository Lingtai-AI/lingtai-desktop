#include "ui/agent_roster.h"

#include "styles/palette.h"

#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtGui/QAction>
#include <QtGui/QFont>
#include <QtGui/QFontMetrics>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QMenu>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOptionFocusRect>
#include <QtWidgets/QLabel>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <utility>

namespace lingtai::desktop {
namespace {

constexpr auto kRosterColumnWidth = 260;
constexpr auto kAvatarDiameter = 40;
constexpr auto kAvatarTextGap = 10;
constexpr auto kRowHorizontalFrame = 14;
constexpr auto kRowVerticalFrame = 8;
constexpr auto kRowSpacing = 2;

// The presentation row set: the shared snapshot keeps the human pseudo-agent
// (routing, mailbox, and detail truth consume it), but the roster never
// renders a human row, so the visible rows omit `AgentRole::human` while
// preserving the snapshot's deterministic order.
std::vector<AgentRow> visible_rows(const AgentSnapshot &snapshot) {
    auto rows = snapshot.items;
    rows.erase(std::remove_if(rows.begin(), rows.end(),
        [](const AgentRow &item) { return item.role == AgentRole::human; }),
        rows.end());
    return rows;
}

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

QString friendly_role_text(AgentRole role) {
    switch (role) {
    case AgentRole::main: return QStringLiteral("Main Agent");
    case AgentRole::agent: return QStringLiteral("Agent");
    case AgentRole::human: return QStringLiteral("Human");
    case AgentRole::unknown: return QString();
    }
    return QString();
}

QString friendly_presence_text(AgentPresenceKind presence) {
    switch (presence) {
    case AgentPresenceKind::alive_human:
    case AgentPresenceKind::alive: return QStringLiteral("Active");
    case AgentPresenceKind::stale: return QStringLiteral("Stale");
    case AgentPresenceKind::missing: return QStringLiteral("Missing");
    case AgentPresenceKind::invalid: return QStringLiteral("Invalid");
    case AgentPresenceKind::unavailable: return QStringLiteral("Unavailable");
    case AgentPresenceKind::unknown: return QString();
    }
    return QString();
}

// The visible two-line row renders a friendly 1:1 summary (`Main Agent ·
// Active`) instead of the raw fact codes, so the compact row stays
// human-readable; the accessible description and tooltip keep the raw facts
// verbatim. An unknown role or presence falls back to the raw facts rather
// than inventing a friendly label.
QString row_summary(const AgentRow &item) {
    const auto role = friendly_role_text(item.role);
    const auto presence = friendly_presence_text(item.presence);
    if (role.isEmpty() || presence.isEmpty()) {
        return row_facts(item);
    }
    return QStringLiteral("%1 · %2").arg(role, presence);
}

// A LingTai-owned checkable row button. It keeps the plain checkable-button
// semantics the shell and its tests rely on while painting the selected,
// hover, pressed, and focus states from the shared lib_ui palette. Idle rows
// merge into the Sidebar's `windowBgOver`; selected/hover rows alone use the
// soft rounded `windowBgRipple` surface rather than a QSS clone of Telegram.
class AgentRowButton final : public QPushButton {
public:
    explicit AgentRowButton(QWidget *parent)
    : QPushButton(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setFocusPolicy(Qt::StrongFocus);
    }

protected:
    QSize sizeHint() const override;
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *event) override;
};

int agent_row_height(const QFont &base_font) {
    auto primary_font = base_font;
    primary_font.setPointSize(13);
    primary_font.setWeight(QFont::DemiBold);
    auto secondary_font = base_font;
    secondary_font.setPointSize(12);
    const auto text_height = QFontMetrics(primary_font).height()
        + QFontMetrics(secondary_font).height();
    return std::max(kAvatarDiameter, text_height) + 2 * kRowVerticalFrame;
}

void paint_agent_row(
        QPainter &painter,
        const QRect &row_rect,
        const QFont &base_font,
        const QString &primary_text,
        const QString &secondary_text,
        bool selected,
        bool over) {
    painter.setRenderHint(QPainter::Antialiasing, true);
    constexpr auto kSelectedRadius = 8.0;
    painter.setPen(Qt::NoPen);
    painter.setBrush((selected || over)
        ? st::windowBgRipple
        : st::windowBgOver);
    painter.drawRoundedRect(
        QRectF(row_rect).adjusted(0.5, 0.5, -0.5, -0.5),
        kSelectedRadius, kSelectedRadius);

    const auto content_rect = row_rect.adjusted(
        kRowHorizontalFrame, kRowVerticalFrame,
        -kRowHorizontalFrame, -kRowVerticalFrame);
    const auto avatar_rect = QRect(
        content_rect.x(),
        content_rect.y() + (content_rect.height() - kAvatarDiameter) / 2,
        kAvatarDiameter, kAvatarDiameter);
    const auto text_rect = QRect(
        avatar_rect.right() + 1 + kAvatarTextGap,
        content_rect.y(),
        content_rect.width() - kAvatarDiameter - 1 - kAvatarTextGap,
        content_rect.height());
    const auto primary_height = text_rect.height() / 2;
    const auto primary_rect = QRect(
        text_rect.x(), text_rect.y(), text_rect.width(), primary_height);
    const auto secondary_rect = QRect(
        text_rect.x(), text_rect.y() + primary_height,
        text_rect.width(), text_rect.height() - primary_height);

    const auto primary_display = QString(primary_text)
        .replace(QLatin1String("&&"), QLatin1String("&"));

    auto primary_font = base_font;
    primary_font.setPointSize(13);
    primary_font.setWeight(QFont::DemiBold);
    const auto primary_color = over
        ? st::dialogsNameFgOver
        : st::dialogsNameFg;

    const auto avatar_initial = primary_display.trimmed().left(1).toUpper();
    painter.setPen(Qt::NoPen);
    painter.setBrush(primary_color);
    painter.drawEllipse(avatar_rect);
    painter.setPen(over
        ? st::windowBgRipple
        : st::windowBg);
    painter.setFont(primary_font);
    painter.drawText(avatar_rect, Qt::AlignCenter, avatar_initial);

    constexpr auto flags = Qt::AlignLeft | Qt::AlignVCenter;
    painter.setFont(primary_font);
    painter.setPen(primary_color);
    painter.drawText(
        primary_rect,
        flags,
        QFontMetrics(primary_font).elidedText(
            primary_display,
            Qt::ElideRight,
            std::max(0, primary_rect.width())));

    auto secondary_font = base_font;
    secondary_font.setPointSize(12);
    painter.setFont(secondary_font);
    painter.setPen(st::windowSubTextFg);
    painter.drawText(
        secondary_rect,
        flags,
        QFontMetrics(secondary_font).elidedText(
            secondary_text,
            Qt::ElideRight,
            std::max(0, secondary_rect.width())));
}

// The virtual Agent rows surface: one canvas owns the visible row model plus
// the selected key and paints every row itself from the shared row geometry,
// instead of owning one QPushButton per row. It exposes a minimal model-update
// and selected-update API; an unchanged model only moves the selected
// highlight. The canvas is not wired into AgentRoster yet — the ownership
// swap lands in a follow-up.
class AgentRowsCanvas final : public Ui::RpWidget {
public:
    explicit AgentRowsCanvas(QWidget *parent)
    : Ui::RpWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void set_rows(
            const AgentSnapshot &snapshot,
            const std::optional<std::filesystem::path> &selected_key) {
        rows_ = visible_rows(snapshot);
        selected_key_ = selected_key;
        updateGeometry();
        update();
    }

    void set_selected_key(
            const std::optional<std::filesystem::path> &selected_key) {
        if (selected_key_ == selected_key) {
            return;
        }
        selected_key_ = selected_key;
        update();
    }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    int content_height() const;

    std::vector<AgentRow> rows_;
    std::optional<std::filesystem::path> selected_key_;
};

QSize AgentRowsCanvas::sizeHint() const {
    return QSize(kRosterColumnWidth, content_height());
}

QSize AgentRowsCanvas::minimumSizeHint() const {
    return sizeHint();
}

int AgentRowsCanvas::content_height() const {
    if (rows_.empty()) {
        return 0;
    }
    const auto row_count = static_cast<int>(rows_.size());
    return row_count * agent_row_height(font())
        + (row_count - 1) * kRowSpacing;
}

void AgentRowsCanvas::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), st::windowBgOver);
    const auto row_height = agent_row_height(font());
    for (auto index = std::size_t{0}; index != rows_.size(); ++index) {
        const auto &item = rows_[index];
        auto primary_text = path_text(item.directory_key);
        primary_text.replace(QLatin1Char('&'), QStringLiteral("&&"));
        const auto selected = selected_key_
            && *selected_key_ == item.directory_key;
        paint_agent_row(
            painter,
            QRect(
                0,
                static_cast<int>(index) * (row_height + kRowSpacing),
                width(),
                row_height),
            font(),
            primary_text,
            row_summary(item),
            selected,
            false);
    }
}

QSize AgentRowButton::sizeHint() const {
    return QSize(
        std::min(QPushButton::sizeHint().width(), kRosterColumnWidth),
        agent_row_height(font()));
}

void AgentRowButton::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const auto selected = isChecked();
    const auto over = isDown() || underMouse();
    const auto lines = text().split(QLatin1Char('\n'));
    paint_agent_row(
        painter, rect(), font(),
        lines.value(0), lines.value(1),
        selected, over);

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

    // The compact project selector header replaces the old stacked brand and
    // identity rows. The shell's typed callback lookup still targets the
    // original object names, so the hidden Open button and project-root label
    // keep their identity: the selector menu's Open/New actions drive those
    // buttons' clicks, and its disabled path row mirrors the current root.
    auto *header = new QHBoxLayout;
    header->setSpacing(6);
    auto *project_root = make_label(
        this, QStringLiteral("No project open"), "lingtai_project_root", 11);
    project_root->hide();
    header->addWidget(project_root);
    auto *open_button = new QPushButton(
        QStringLiteral("Open Project…"), this);
    open_button->setObjectName("lingtai_open_project_button");
    open_button->setAccessibleName(QStringLiteral("Open Project"));
    open_button->setAccessibleDescription(QStringLiteral(
        "Request a project location. No project is changed by this request."));
    open_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    open_button->hide();
    header->addWidget(open_button);
    auto *new_button = new QPushButton(QStringLiteral("+"), this);
    new_button->setObjectName("lingtai_new_project_button");
    new_button->setAccessibleName(QStringLiteral("New Project"));
    new_button->setAccessibleDescription(QStringLiteral(
        "Creates a new LingTai project and its first Agent through the "
        "canonical TUI, then starts that Agent."));
    new_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto *selector = new QPushButton(QStringLiteral("LingTai ▾"), this);
    selector->setObjectName("lingtai_project_selector");
    selector->setAccessibleName(QStringLiteral("LingTai project selector"));
    selector->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    header->addWidget(selector);
    header->addWidget(new_button);
    layout->addLayout(header);

    auto *selector_menu = new QMenu(selector);
    selector_menu->setObjectName("lingtai_project_selector_menu");
    auto *path_action = selector_menu->addAction(project_root->text());
    path_action->setEnabled(false);
    auto *open_action =
        selector_menu->addAction(QStringLiteral("Open Project"));
    open_action->setObjectName("lingtai_open_project_button");
    QObject::connect(open_action, &QAction::triggered,
        open_button, &QPushButton::click);
    auto *new_action =
        selector_menu->addAction(QStringLiteral("New Project"));
    new_action->setObjectName("lingtai_new_project_button");
    QObject::connect(new_action, &QAction::triggered,
        new_button, &QPushButton::click);
    QObject::connect(selector, &QPushButton::clicked,
        [selector, selector_menu, path_action, project_root] {
            path_action->setText(project_root->text());
            selector_menu->popup(selector->mapToGlobal(
                QPoint(0, selector->height())));
        });

    auto *roster = new Ui::RpWidget(this);
    roster->setObjectName("lingtai_agent_roster");
    roster->setAccessibleName(QStringLiteral("Agent roster"));
    auto *roster_layout = new QVBoxLayout(roster);
    roster_layout->setContentsMargins(0, 0, 0, 0);
    roster_layout->setSpacing(6);
    auto *heading_row = new QHBoxLayout;
    heading_row->setSpacing(6);
    heading_row->addWidget(make_label(
        roster, QStringLiteral("Agents"), "lingtai_agent_roster_heading", 12,
        QFont::DemiBold));
    roster_state_ = make_label(
        roster, QString(), "lingtai_agent_roster_state", 10);
    roster_state_->setAccessibleName(QStringLiteral("Agent roster status"));
    heading_row->addStretch();
    heading_row->addWidget(roster_state_);
    roster_layout->addLayout(heading_row);
    scroll_ = new QScrollArea(roster);
    scroll_->setObjectName("lingtai_agent_roster_scroll");
    scroll_->setAccessibleName(QStringLiteral("Agent roster rows"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    // The rows surface and its scroll viewport share the Sidebar's
    // `windowBgOver` background instead of forming an independent white field.
    auto viewport_palette = scroll_->viewport()->palette();
    viewport_palette.setColor(QPalette::Window, st::windowBgOver->c);
    scroll_->viewport()->setPalette(viewport_palette);
    scroll_->viewport()->setAutoFillBackground(true);
    roster_layout->addWidget(scroll_, 1);
    auto *rows = new Ui::RpWidget(scroll_);
    rows->setObjectName("lingtai_agent_roster_rows");
    rows->setAccessibleName(QStringLiteral("Agent roster rows"));
    auto rows_palette = rows->palette();
    rows_palette.setColor(QPalette::Window, st::windowBgOver->c);
    rows->setPalette(rows_palette);
    rows->setAutoFillBackground(true);
    rows_layout_ = new QVBoxLayout(rows);
    rows_layout_->setContentsMargins(0, 0, 0, 0);
    rows_layout_->setSpacing(2);
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
    const auto visible = visible_rows(snapshot);
    roster_state_->setText(snapshot.scan == AgentScanState::complete
            && !visible.empty()
        ? QStringLiteral("%1").arg(visible.size())
        : QString());
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
    // The comparison and the row tree both cover the visible set (the human
    // pseudo-agent omitted), so a human-only projection change never churns
    // the real rows.
    const auto visible = visible_rows(snapshot);
    const auto shown = visible_rows(visible_snapshot_);
    const auto rows_match = visible.size() == shown.size();
    auto model_unchanged = rows_match;
    if (rows_match) {
        for (auto index = std::size_t{0}; index != visible.size(); ++index) {
            const auto &before = shown[index];
            const auto &after = visible[index];
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
    for (auto index = std::size_t{0}; index != visible.size(); ++index) {
        const auto &item = visible[index];
        auto button_key = path_text(item.directory_key);
        button_key.replace(QLatin1Char('&'), QStringLiteral("&&"));
        auto *row = new AgentRowButton(rows_layout_->parentWidget());
        row->setObjectName(
            QStringLiteral("lingtai_agent_row_%1").arg(index));
        row->setAccessibleName(
            QStringLiteral("Agent %1").arg(path_text(item.directory_key)));
        row->setText(QStringLiteral("%1\n%2").arg(button_key, row_summary(item)));
        row->setAccessibleDescription(row_accessible(item));
        row->setToolTip(row_facts(item));
        row->setProperty("directory_key", path_text(item.directory_key));
        row->setCheckable(true);
        row->setChecked(selected_key && *selected_key == item.directory_key);
        row->setEnabled(item.manifest_kind == AgentManifestKind::valid);
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
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
