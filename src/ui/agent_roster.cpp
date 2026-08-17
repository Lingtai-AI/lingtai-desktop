#include "ui/agent_roster.h"

#include "styles/palette.h"

#include <QtCore/QString>
#include <QtGui/QAction>
#include <QtGui/QFont>
#include <QtGui/QMouseEvent>
#include <QtGui/QFontMetrics>
#include <QtGui/QPainter>
#include <QtGui/QPen>
#include <QtWidgets/QMenu>
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
constexpr auto kNarrowRosterWidth = 120;
constexpr auto kAvatarDiameter = 40;
constexpr auto kAvatarTextGap = 10;
constexpr auto kRowHorizontalFrame = 14;
constexpr auto kRowVerticalFrame = 8;
constexpr auto kRowSpacing = 2;

enum class ProjectToolbarControl {
    selector,
    add,
};

// Telegram lib_ui's FlatButton paints only a flat base/hover background before
// its ripple, while IconButton paints the ripple and glyph without asking the
// platform style for a frame. Keep that same ownership here: the project
// controls paint their own small hover/press surface and glyphs, so macOS/Qt
// can never reintroduce a beveled default QPushButton border.
class ProjectToolbarButton final : public QPushButton {
public:
    ProjectToolbarButton(
        QWidget *parent,
        ProjectToolbarControl control)
    : QPushButton(parent)
    , control_(control) {
        setFlat(true);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(30);
        if (control_ == ProjectToolbarControl::add) {
            setFixedWidth(30);
            setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        } else {
            setMinimumWidth(0);
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        }
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        if (underMouse() || isDown()) {
            auto background = st::windowBgRipple->c;
            if (isDown()) {
                background = background.darker(112);
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(background);
            painter.drawRoundedRect(
                QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        }

        const auto ink = (underMouse() || isDown())
            ? st::dialogsNameFgOver->c
            : st::dialogsNameFg->c;
        painter.setPen(ink);

        if (control_ == ProjectToolbarControl::selector) {
            auto font = this->font();
            font.setPointSize(11);
            font.setWeight(QFont::DemiBold);
            painter.setFont(font);
            const auto label = text();
            if (label == QStringLiteral("⋯")) {
                painter.drawText(rect(), Qt::AlignCenter, label);
            } else {
                const auto left = 9;
                const auto metrics = QFontMetrics(font);
                const auto label_width = metrics.horizontalAdvance(label);
                painter.drawText(
                    QRect(left, 0, label_width, height()),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    label);
                const auto chevron_x = left + label_width + 7.0;
                const auto chevron_y = height() / 2.0;
                auto chevron_pen = QPen(ink, 1.4);
                chevron_pen.setCapStyle(Qt::RoundCap);
                painter.setPen(chevron_pen);
                painter.drawLine(
                    QPointF(chevron_x - 3.0, chevron_y - 1.5),
                    QPointF(chevron_x, chevron_y + 1.5));
                painter.drawLine(
                    QPointF(chevron_x, chevron_y + 1.5),
                    QPointF(chevron_x + 3.0, chevron_y - 1.5));
            }
        } else {
            auto icon_pen = QPen(ink, 1.6);
            icon_pen.setCapStyle(Qt::RoundCap);
            painter.setPen(icon_pen);
            const auto center = QPointF(width() / 2.0, height() / 2.0);
            painter.drawLine(
                center + QPointF(-4.0, 0.0),
                center + QPointF(4.0, 0.0));
            painter.drawLine(
                center + QPointF(0.0, -4.0),
                center + QPointF(0.0, 4.0));
        }

        if (hasFocus()) {
            painter.fillRect(
                QRect(7, height() - 2, qMax(0, width() - 14), 2),
                st::dialogsBgActive);
        }
    }

private:
    ProjectToolbarControl control_;
};

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

QString row_facts(const AgentRow &item) {
    return QStringLiteral("%1 — %2 — %3")
        .arg(manifest_text(item.manifest_kind), role_text(item.role),
            presence_text(item.presence));
}

// The visible two-line row renders a friendly 1:1 summary (`Main Agent ·
// Active`) instead of the raw fact codes, so the compact row stays
// human-readable; the accessible description and tooltip keep the raw facts
// verbatim. An unknown role or presence falls back to the raw facts rather
// than inventing a friendly label.
QString row_summary(const AgentRow &item) {
    const auto role = friendly_agent_role_text(item.role);
    const auto presence = friendly_agent_presence_text(item.presence);
    if (role.isEmpty() || presence.isEmpty()) {
        return row_facts(item);
    }
    return QStringLiteral("%1 · %2").arg(role, presence);
}

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
    painter.setBrush(selected
        ? st::dialogsBgActive
        : over
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
    const auto primary_color = selected
        ? st::dialogsNameFgActive
        : over
            ? st::dialogsNameFgOver
            : st::dialogsNameFg;

    const auto avatar_initial = primary_display.trimmed().left(1).toUpper();
    painter.setPen(Qt::NoPen);
    painter.setBrush(primary_color);
    painter.drawEllipse(avatar_rect);
    painter.setPen(selected
        ? st::dialogsBgActive
        : over
            ? st::windowBgRipple
            : st::windowBg);
    painter.setFont(primary_font);
    painter.drawText(avatar_rect, Qt::AlignCenter, avatar_initial);

    // Telegram's dialogs painter returns immediately after the userpic in its
    // narrow mode. Mirror that behavior when this row has room for no more than
    // one or two useful name glyphs; above the breakpoint both text lines use
    // their existing right elision instead of widening the canvas.
    const auto minimum_useful_text_width =
        QFontMetrics(primary_font).horizontalAdvance(QStringLiteral("MM…"));
    if (text_rect.width() < minimum_useful_text_width) {
        return;
    }

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
    painter.setPen(selected
        ? st::dialogsTextFgActive
        : st::windowSubTextFg);
    painter.drawText(
        secondary_rect,
        flags,
        QFontMetrics(secondary_font).elidedText(
            secondary_text,
            Qt::ElideRight,
            std::max(0, secondary_rect.width())));
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

QString friendly_agent_role_text(AgentRole role) {
    switch (role) {
    case AgentRole::main: return QStringLiteral("Main Agent");
    case AgentRole::agent: return QStringLiteral("Agent");
    case AgentRole::human: return QStringLiteral("Human");
    case AgentRole::unknown: return QString();
    }
    return QString();
}

QString friendly_agent_presence_text(AgentPresenceKind presence) {
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

// The virtual Agent rows surface: one canvas owns the visible row model plus
// the selected key and paints every row itself from the shared row geometry,
// instead of owning one QPushButton per row. It exposes a minimal model-update
// and selected-update API; an unchanged model only moves the selected
// highlight. The canvas also stores the row-click callback and the keyboard
// focus target so the dedicated mouse and keyboard commits can drive them
// without a QWidget row tree.
class AgentRowsCanvas final : public Ui::RpWidget {
public:
    explicit AgentRowsCanvas(QWidget *parent)
    : Ui::RpWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
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

    void set_row_click_handler(
            std::function<void(const std::filesystem::path &)> handler) {
        row_click_handler_ = std::move(handler);
    }

    void set_focus_key(
            const std::optional<std::filesystem::path> &key) {
        focus_key_ = key;
    }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEventHook(QEvent *event) override;

private:
    int content_height() const;

    // Exact row hit-testing from canvas-local coordinates: only the interior
    // of a real row (not the `kRowSpacing` gap, a negative coordinate, or a
    // coordinate past the painted model content) yields an index.
    std::optional<std::size_t> row_index_at(int y) const;

    bool is_valid_row(std::size_t index) const;

    QRect row_rect(std::size_t index) const;

    void set_hovered_row(std::optional<std::size_t> index);

    void set_pressed_row(
        std::optional<std::size_t> index,
        std::optional<std::filesystem::path> key);

    std::vector<AgentRow> rows_;
    std::optional<std::filesystem::path> selected_key_;
    std::function<void(const std::filesystem::path &)> row_click_handler_;
    std::optional<std::filesystem::path> focus_key_;
    std::optional<std::size_t> hovered_row_;
    std::optional<std::size_t> pressed_row_;
    std::optional<std::filesystem::path> pressed_key_;
};

QSize AgentRowsCanvas::sizeHint() const {
    return QSize(kRosterColumnWidth, content_height());
}

QSize AgentRowsCanvas::minimumSizeHint() const {
    // The scroll viewport owns the horizontal width. Keeping the 260px
    // preferred width as a minimum makes the canvas exceed the inset viewport
    // by the Sidebar margins and creates a pointless horizontal scrollbar.
    return QSize(0, content_height());
}

int AgentRowsCanvas::content_height() const {
    if (rows_.empty()) {
        return 0;
    }
    const auto row_count = static_cast<int>(rows_.size());
    return row_count * agent_row_height(font())
        + (row_count - 1) * kRowSpacing;
}

std::optional<std::size_t> AgentRowsCanvas::row_index_at(int y) const {
    if (y < 0 || rows_.empty()) {
        return std::nullopt;
    }
    const auto row_height = agent_row_height(font());
    const auto stride = row_height + kRowSpacing;
    const auto index = y / stride;
    if (static_cast<std::size_t>(index) >= rows_.size()) {
        return std::nullopt;
    }
    if (y - index * stride >= row_height) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(index);
}

bool AgentRowsCanvas::is_valid_row(std::size_t index) const {
    return rows_[index].manifest_kind == AgentManifestKind::valid;
}

QRect AgentRowsCanvas::row_rect(std::size_t index) const {
    const auto row_height = agent_row_height(font());
    return QRect(
        0,
        static_cast<int>(index) * (row_height + kRowSpacing),
        width(),
        row_height);
}

void AgentRowsCanvas::set_hovered_row(
        std::optional<std::size_t> index) {
    if (index == hovered_row_) {
        return;
    }
    if (hovered_row_) {
        update(row_rect(*hovered_row_));
    }
    hovered_row_ = index;
    if (hovered_row_) {
        update(row_rect(*hovered_row_));
    }
}

void AgentRowsCanvas::set_pressed_row(
        std::optional<std::size_t> index,
        std::optional<std::filesystem::path> key) {
    const auto previous = pressed_row_;
    pressed_row_ = index;
    pressed_key_ = std::move(key);
    if (previous != pressed_row_) {
        if (previous) {
            update(row_rect(*previous));
        }
        if (pressed_row_) {
            update(row_rect(*pressed_row_));
        }
    }
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
        const auto over = (hovered_row_ && *hovered_row_ == index)
            || (pressed_row_ && *pressed_row_ == index);
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
            over);
    }
}

void AgentRowsCanvas::mouseMoveEvent(QMouseEvent *event) {
    set_hovered_row(row_index_at(event->pos().y()));
    QWidget::mouseMoveEvent(event);
}

void AgentRowsCanvas::mousePressEvent(QMouseEvent *event) {
    const auto hover = row_index_at(event->pos().y());
    set_hovered_row(hover);
    if (event->button() == Qt::LeftButton) {
        if (hover && is_valid_row(*hover)) {
            set_pressed_row(hover, rows_[*hover].directory_key);
        } else {
            set_pressed_row(std::nullopt, std::nullopt);
        }
    } else {
        set_pressed_row(std::nullopt, std::nullopt);
    }
    QWidget::mousePressEvent(event);
}

void AgentRowsCanvas::mouseReleaseEvent(QMouseEvent *event) {
    const auto pressed = pressed_row_;
    const auto pressed_key = pressed_key_;
    auto clicked_row = std::optional<std::size_t>{};
    if (event->button() == Qt::LeftButton) {
        const auto index = row_index_at(event->pos().y());
        if (index && pressed && *index == *pressed
            && is_valid_row(*index)
            && pressed_key
            && rows_[*index].directory_key == *pressed_key) {
            clicked_row = index;
        }
    }
    set_pressed_row(std::nullopt, std::nullopt);
    if (clicked_row && row_click_handler_) {
        row_click_handler_(rows_[*clicked_row].directory_key);
    }
    QWidget::mouseReleaseEvent(event);
}

void AgentRowsCanvas::leaveEventHook(QEvent *event) {
    set_hovered_row(std::nullopt);
    set_pressed_row(std::nullopt, std::nullopt);
    Ui::RpWidget::leaveEventHook(event);
}

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
    header->setSpacing(4);
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
    auto *new_button = new ProjectToolbarButton(
        this, ProjectToolbarControl::add);
    new_project_button_ = new_button;
    new_button->setObjectName("lingtai_new_project_button");
    new_button->setAccessibleName(QStringLiteral("New Project"));
    new_button->setAccessibleDescription(QStringLiteral(
        "Creates a new LingTai project and its first Agent through the "
        "canonical TUI, then starts that Agent."));
    auto *selector = new ProjectToolbarButton(
        this, ProjectToolbarControl::selector);
    selector->setText(QStringLiteral("LingTai"));
    project_selector_ = selector;
    selector->setObjectName("lingtai_project_selector");
    selector->setAccessibleName(QStringLiteral("LingTai project selector"));
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
    roster_heading_ = make_label(
        roster, QStringLiteral("Agents"), "lingtai_agent_roster_heading", 12);
    heading_row->addWidget(roster_heading_);
    roster_state_ = make_label(
        roster, QString(), "lingtai_agent_roster_state", 10);
    auto roster_state_palette = roster_state_->palette();
    roster_state_palette.setColor(
        QPalette::WindowText, st::windowSubTextFg->c);
    roster_state_->setPalette(roster_state_palette);
    roster_state_->setAccessibleName(QStringLiteral("Agent roster status"));
    heading_row->addStretch();
    heading_row->addWidget(roster_state_);
    roster_layout->addLayout(heading_row);
    scroll_ = new QScrollArea(roster);
    scroll_->setObjectName("lingtai_agent_roster_scroll");
    scroll_->setAccessibleName(QStringLiteral("Agent roster rows"));
    scroll_->setWidgetResizable(true);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setFrameShape(QFrame::NoFrame);
    // The rows surface and its scroll viewport share the Sidebar's
    // `windowBgOver` background instead of forming an independent white field.
    auto viewport_palette = scroll_->viewport()->palette();
    viewport_palette.setColor(QPalette::Window, st::windowBgOver->c);
    scroll_->viewport()->setPalette(viewport_palette);
    scroll_->viewport()->setAutoFillBackground(true);
    roster_layout->addWidget(scroll_, 1);
    canvas_ = new AgentRowsCanvas(scroll_);
    canvas_->setObjectName("lingtai_agent_roster_rows");
    canvas_->setAccessibleName(QStringLiteral("Agent roster rows"));
    auto canvas_palette = canvas_->palette();
    canvas_palette.setColor(QPalette::Window, st::windowBgOver->c);
    canvas_->setPalette(canvas_palette);
    canvas_->setAutoFillBackground(true);
    scroll_->setWidget(canvas_);
    layout->addWidget(roster, 1);
    update_narrow_mode();
}

AgentRoster::~AgentRoster() = default;

void AgentRoster::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), st::windowBgOver);
}

void AgentRoster::set_roster_width(int width) {
    setFixedWidth(width);
    update_narrow_mode();
}

void AgentRoster::update_narrow_mode() {
    const auto narrow = width() <= kNarrowRosterWidth;
    project_selector_->setText(narrow
        ? QStringLiteral("⋯")
        : QStringLiteral("LingTai"));
    new_project_button_->setVisible(!narrow);
    roster_heading_->setVisible(!narrow);
    roster_state_->setVisible(!narrow);
}

void AgentRoster::set_row_click_handler(RowClickHandler handler) {
    canvas_->set_row_click_handler(std::move(handler));
}

void AgentRoster::update_state_label(const AgentSnapshot &snapshot) {
    const auto visible = visible_rows(snapshot);
    roster_state_->setText(snapshot.scan == AgentScanState::complete
            && !visible.empty()
        ? QStringLiteral("%1").arg(visible.size())
        : QString());
}

void AgentRoster::set_rows(
        const AgentSnapshot &snapshot,
        const std::optional<std::filesystem::path> &selected_key) {
    update_state_label(snapshot);

    // The visible model is unchanged when every row's identity, facts, and
    // diagnostic match what is already shown. In that case only the selected
    // state may have moved: never rebuild the canvas model, so an unchanged
    // one-second projection refresh preserves scroll, focus, and row identity.
    // The comparison covers the visible set (the human pseudo-agent omitted),
    // so a human-only projection change never churns the real rows.
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
        canvas_->set_selected_key(selected_key);
        return;
    }
    canvas_->set_rows(snapshot, selected_key);
}

void AgentRoster::focus_row(
        const std::optional<std::filesystem::path> &key) {
    canvas_->set_focus_key(key);
}

} // namespace lingtai::desktop
