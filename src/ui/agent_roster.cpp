#include "ui/agent_roster.h"

#include "styles/palette.h"

#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QFontMetrics>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPen>
#include <QtWidgets/QFrame>
#include <QtWidgets/QLabel>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace lingtai::desktop {
namespace {

constexpr auto kRosterColumnWidth = 260;
constexpr auto kNarrowRosterWidth = 120;
constexpr auto kAvatarDiameter = 40;
constexpr auto kAvatarTextGap = 10;
constexpr auto kAvatarLetterPixelSize = 22;
constexpr auto kRowHorizontalFrame = 14;
constexpr auto kRowVerticalFrame = 8;
constexpr auto kRowSpacing = 2;
constexpr auto kStatusDotDiameter = 6;
constexpr auto kStatusDotGap = 6;
constexpr auto kProjectIconSize = 14;
constexpr auto kProjectIconGap = 8;
// Roster avatars share the composer Send fill (`windowBgActive`) so the
// initial disc tracks the same light/dark accent as the up-arrow button.
[[nodiscard]] QColor avatar_fill_color() {
    return st::windowBgActive->c;
}

// Soft selection wash: blend the composer Send accent into the sidebar base
// so the selected row reads as a pale blue tint, not a solid filled pill.
[[nodiscard]] QColor selected_row_fill_color() {
    const auto base = st::windowBg->c;
    const auto accent = st::windowBgActive->c;
    constexpr auto kTint = 0.16;
    return QColor(
        int(base.red() * (1.0 - kTint) + accent.red() * kTint + 0.5),
        int(base.green() * (1.0 - kTint) + accent.green() * kTint + 0.5),
        int(base.blue() * (1.0 - kTint) + accent.blue() * kTint + 0.5));
}

// Telegram lib_ui's FlatButton paints only a flat base/hover background before
// its ripple, while IconButton paints the ripple and glyph without asking the
// platform style for a frame. Keep that same ownership here: the project
// selector paints its own small hover/press surface and chevron, so macOS/Qt
// can never reintroduce a beveled default QPushButton border.
class ProjectSelectorButton final : public QPushButton {
public:
    explicit ProjectSelectorButton(QWidget *parent)
    : QPushButton(parent) {
        setFlat(true);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(34);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto font = this->font();
        font.setPointSize(15);
        font.setWeight(QFont::DemiBold);
        setFont(font);
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

        const auto label = text();
        if (label == QStringLiteral("⋯")) {
            painter.setPen(ink);
            painter.setFont(font());
            painter.drawText(rect(), Qt::AlignCenter, label);
        } else {
            // 2×2 apps/grid glyph matching the design folder row.
            const auto icon_left = 8.0;
            const auto icon_top = (height() - kProjectIconSize) / 2.0;
            const auto cell = 5.5;
            const auto gap = 3.0;
            painter.setPen(Qt::NoPen);
            painter.setBrush(ink);
            painter.drawRoundedRect(
                QRectF(icon_left, icon_top, cell, cell), 1.2, 1.2);
            painter.drawRoundedRect(
                QRectF(icon_left + cell + gap, icon_top, cell, cell), 1.2, 1.2);
            painter.drawRoundedRect(
                QRectF(icon_left, icon_top + cell + gap, cell, cell), 1.2, 1.2);
            painter.drawRoundedRect(
                QRectF(icon_left + cell + gap, icon_top + cell + gap, cell, cell),
                1.2, 1.2);

            const auto label_font = font();
            painter.setFont(label_font);
            painter.setPen(ink);
            const auto left = int(icon_left + kProjectIconSize + kProjectIconGap);
            const auto metrics = QFontMetrics(label_font);
            const auto label_width = metrics.horizontalAdvance(label);
            painter.drawText(
                QRect(left, 0, label_width, height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                label);
            const auto chevron_x = left + label_width + 7.0;
            const auto chevron_y = height() / 2.0;
            auto chevron_pen = QPen(ink, 1.6);
            chevron_pen.setCapStyle(Qt::RoundCap);
            painter.setPen(chevron_pen);
            painter.drawLine(
                QPointF(chevron_x - 3.0, chevron_y - 1.5),
                QPointF(chevron_x, chevron_y + 1.5));
            painter.drawLine(
                QPointF(chevron_x, chevron_y + 1.5),
                QPointF(chevron_x + 3.0, chevron_y - 1.5));
        }

        if (hasFocus()) {
            painter.fillRect(
                QRect(7, height() - 2, qMax(0, width() - 14), 2),
                st::dialogsBgActive);
        }
    }
};

constexpr auto kProjectMenuShadow = 8;
constexpr auto kProjectMenuRadius = 10;
constexpr auto kProjectMenuActionHeight = 42;
constexpr auto kProjectMenuActionTextPx = 14;
constexpr auto kProjectMenuIconBox = 18;

enum class ProjectMenuGlyph {
    folder,
    window,
};

void paint_project_menu_glyph(
        QPainter &painter,
        const QRectF &box,
        ProjectMenuGlyph glyph,
        const QColor &ink) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    auto pen = QPen(ink, 1.5);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    if (glyph == ProjectMenuGlyph::folder) {
        auto path = QPainterPath();
        path.moveTo(box.left() + 1.5, box.top() + 6.0);
        path.lineTo(box.left() + 1.5, box.top() + 4.0);
        path.quadTo(box.left() + 1.5, box.top() + 2.5,
            box.left() + 3.0, box.top() + 2.5);
        path.lineTo(box.left() + 7.5, box.top() + 2.5);
        path.lineTo(box.left() + 9.5, box.top() + 5.0);
        path.lineTo(box.right() - 1.5, box.top() + 5.0);
        path.quadTo(box.right() - 0.2, box.top() + 5.0,
            box.right() - 0.2, box.top() + 6.3);
        path.lineTo(box.right() - 0.2, box.bottom() - 1.5);
        path.quadTo(box.right() - 0.2, box.bottom() - 0.2,
            box.right() - 1.5, box.bottom() - 0.2);
        path.lineTo(box.left() + 1.5, box.bottom() - 0.2);
        path.quadTo(box.left() + 0.2, box.bottom() - 0.2,
            box.left() + 0.2, box.bottom() - 1.5);
        path.lineTo(box.left() + 0.2, box.top() + 6.0);
        path.closeSubpath();
        painter.drawPath(path);
    } else {
        const auto front = QRectF(
            box.left() + 1.0, box.top() + 4.0,
            box.width() - 5.0, box.height() - 5.5);
        const auto back = QRectF(
            box.left() + 4.0, box.top() + 1.5,
            box.width() - 5.0, box.height() - 5.5);
        painter.drawRoundedRect(back, 2.0, 2.0);
        painter.setBrush(st::windowBg->c);
        painter.drawRoundedRect(front, 2.0, 2.0);
    }
    painter.restore();
}

// Themed folder-menu row: 42px tall, 14px label, pale accent hover wash.
class ProjectMenuAction final : public QPushButton {
public:
    ProjectMenuAction(
            const QString &label,
            ProjectMenuGlyph glyph,
            QWidget *parent)
    : QPushButton(label, parent)
    , glyph_(glyph) {
        setFlat(true);
        setCursor(Qt::PointingHandCursor);
        setFixedHeight(kProjectMenuActionHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto font = this->font();
        font.setPointSize(kProjectMenuActionTextPx);
        font.setWeight(QFont::Normal);
        setFont(font);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto row = QRectF(rect()).adjusted(6, 2, -6, -2);
        if (underMouse() || isDown()) {
            auto wash = st::windowBgActive->c;
            wash.setAlpha(isDown() ? 36 : 22);
            painter.setPen(Qt::NoPen);
            painter.setBrush(wash);
            painter.drawRoundedRect(row, 8, 8);
        }
        const auto ink = st::windowFg->c;
        const auto icon_box = QRectF(
            row.left() + 8,
            row.center().y() - kProjectMenuIconBox / 2.0,
            kProjectMenuIconBox,
            kProjectMenuIconBox);
        paint_project_menu_glyph(painter, icon_box, glyph_, ink);
        painter.setFont(font());
        painter.setPen(ink);
        const auto text_left = int(icon_box.right() + 10);
        painter.drawText(
            QRect(text_left, 0, width() - text_left - 12, height()),
            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
            text());
    }

private:
    ProjectMenuGlyph glyph_;
};

// White/dark themed popover that replaces the native QMenu for the project
// selector. Keeps the stable object names tests and shell wiring already use.
class ProjectSelectorPopover final : public QWidget {
public:
    explicit ProjectSelectorPopover(QWidget *parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint) {
        setObjectName("lingtai_project_selector_menu");
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::StrongFocus);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(
            kProjectMenuShadow,
            kProjectMenuShadow,
            kProjectMenuShadow,
            kProjectMenuShadow + 2);
        root->setSpacing(0);

        auto *card = new QWidget(this);
        card->setObjectName("lingtai_project_selector_menu_card");
        card->setAttribute(Qt::WA_TranslucentBackground);
        auto *card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(6, 8, 6, 6);
        card_layout->setSpacing(0);

        path_label_ = new QLabel(card);
        path_label_->setObjectName("lingtai_project_selector_menu_path");
        path_label_->setWordWrap(true);
        path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto path_font = path_label_->font();
        path_font.setPointSize(11);
        path_font.setWeight(QFont::Normal);
        path_label_->setFont(path_font);
        path_label_->setContentsMargins(10, 4, 10, 8);
        card_layout->addWidget(path_label_);

        auto *divider = new QFrame(card);
        divider->setObjectName("lingtai_project_selector_menu_divider");
        divider->setFrameShape(QFrame::HLine);
        divider->setFixedHeight(1);
        divider->setContentsMargins(8, 0, 8, 0);
        card_layout->addWidget(divider);
        divider_ = divider;

        open_action_ = new ProjectMenuAction(
            QStringLiteral("Open Project"),
            ProjectMenuGlyph::folder,
            card);
        open_action_->setObjectName("lingtai_open_project_menu_action");
        open_action_->setAccessibleName(QStringLiteral("Open Project"));
        card_layout->addWidget(open_action_);

        open_new_window_action_ = new ProjectMenuAction(
            QStringLiteral("Open Project in Another Window…"),
            ProjectMenuGlyph::window,
            card);
        open_new_window_action_->setObjectName(
            "lingtai_open_project_new_window_menu_action");
        open_new_window_action_->setAccessibleName(
            QStringLiteral("Open Project in Another Window"));
        card_layout->addWidget(open_new_window_action_);

        root->addWidget(card);
        apply_palette();
        hide();
    }

    [[nodiscard]] QPushButton *open_action() const { return open_action_; }
    [[nodiscard]] QPushButton *open_new_window_action() const {
        return open_new_window_action_;
    }

    void present(const QPoint &global_top_left, const QString &path_text) {
        path_label_->setText(path_text);
        path_label_->setAccessibleName(path_text);
        apply_palette();
        const auto hint = sizeHint();
        const auto anchor_width = parentWidget() ? parentWidget()->width() : 0;
        const auto width = std::max({
            280,
            hint.width(),
            anchor_width + 2 * kProjectMenuShadow,
        });
        resize(width, hint.height());
        move(global_top_left);
        show();
        raise();
        activateWindow();
        setFocus(Qt::PopupFocusReason);
    }

    void apply_palette() {
        auto path_palette = path_label_->palette();
        path_palette.setColor(QPalette::WindowText, st::windowSubTextFg->c);
        path_label_->setPalette(path_palette);
        auto line = st::windowFg->c;
        line.setAlpha(28);
        auto divider_palette = divider_->palette();
        divider_palette.setColor(QPalette::WindowText, line);
        divider_palette.setColor(QPalette::Dark, line);
        divider_palette.setColor(QPalette::Light, line);
        divider_palette.setColor(QPalette::Mid, line);
        divider_->setPalette(divider_palette);
        divider_->setStyleSheet(QStringLiteral(
            "QFrame { border: none; background: rgba(%1,%2,%3,%4); margin: 0 8px; }")
            .arg(line.red()).arg(line.green()).arg(line.blue()).arg(line.alpha()));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto card = rect().adjusted(
            kProjectMenuShadow,
            kProjectMenuShadow - 1,
            -kProjectMenuShadow,
            -(kProjectMenuShadow - 1));
        auto shadow = st::windowFg->c;
        for (auto i = 3; i >= 1; --i) {
            shadow.setAlpha(8 * (4 - i));
            painter.setPen(Qt::NoPen);
            painter.setBrush(shadow);
            painter.drawRoundedRect(
                card.adjusted(-i, i - 1, i, i + 1),
                kProjectMenuRadius,
                kProjectMenuRadius);
        }
        auto border = st::windowFg->c;
        border.setAlpha(36);
        painter.setBrush(st::windowBg->c);
        painter.setPen(QPen(border, 1));
        painter.drawRoundedRect(card, kProjectMenuRadius, kProjectMenuRadius);
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) {
            hide();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    QLabel *path_label_ = nullptr;
    QFrame *divider_ = nullptr;
    ProjectMenuAction *open_action_ = nullptr;
    ProjectMenuAction *open_new_window_action_ = nullptr;
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

// The visible two-line row renders a friendly 1:1 summary (`Main agent ·
// Active`) from the TUI lifecycle resolver, not raw heartbeat presence.
QString row_summary(const AgentRow &item) {
    const auto role = friendly_agent_role_text(item.role);
    const auto state = friendly_agent_lifecycle_text(item);
    if (role.isEmpty()) {
        return row_facts(item);
    }
    return QStringLiteral("%1 · %2").arg(role, state);
}

QColor lifecycle_status_color(const AgentRow &item) {
    const auto state = QString::fromStdString(item.lifecycle_state).toLower();
    if (state == QStringLiteral("active")) {
        return avatar_fill_color();
    }
    if (state == QStringLiteral("suspended")
        || state == QStringLiteral("stuck")) {
        return QColor(QStringLiteral("#D97706"));
    }
    if (state == QStringLiteral("asleep")) {
        return QColor(QStringLiteral("#64748B"));
    }
    if (state == QStringLiteral("idle")
        || state == QStringLiteral("refreshing")) {
        return QColor(QStringLiteral("#94A3B8"));
    }
    return st::windowSubTextFg->c;
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
        const QColor &status_color,
        bool selected,
        bool over,
        int unseen_count) {
    painter.setRenderHint(QPainter::Antialiasing, true);
    constexpr auto kSelectedRadius = 8.0;
    // Unselected rows stay transparent on the sidebar canvas; only hover and
    // selection paint a rounded surface.
    if (selected || over) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(selected
            ? selected_row_fill_color()
            : st::windowBgRipple->c);
        painter.drawRoundedRect(
            QRectF(row_rect).adjusted(0.5, 0.5, -0.5, -0.5),
            kSelectedRadius, kSelectedRadius);
    }

    const auto content_rect = row_rect.adjusted(
        kRowHorizontalFrame, kRowVerticalFrame,
        -kRowHorizontalFrame, -kRowVerticalFrame);
    const auto avatar_rect = QRect(
        content_rect.x(),
        content_rect.y() + (content_rect.height() - kAvatarDiameter) / 2,
        kAvatarDiameter, kAvatarDiameter);

    // Telegram-style unread capsule on the secondary line's trailing edge.
    constexpr auto kUnreadHeight = 19;
    constexpr auto kUnreadPadding = 5;
    auto unread_badge_width = 0;
    auto unread_label = QString();
    if (unseen_count > 0) {
        unread_label = unseen_count > 99
            ? QStringLiteral("99+")
            : QString::number(unseen_count);
        auto badge_font = base_font;
        badge_font.setPointSize(12);
        badge_font.setWeight(QFont::Bold);
        unread_badge_width = std::max(
            kUnreadHeight,
            QFontMetrics(badge_font).horizontalAdvance(unread_label)
                + 2 * kUnreadPadding);
    }
    const auto text_right_inset = unread_badge_width > 0
        ? unread_badge_width + 8
        : 0;
    const auto text_rect = QRect(
        avatar_rect.right() + 1 + kAvatarTextGap,
        content_rect.y(),
        std::max(0,
            content_rect.width() - kAvatarDiameter - 1 - kAvatarTextGap
                - text_right_inset),
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
        ? (over ? st::dialogsNameFgOver : st::dialogsNameFg)
        : over
            ? st::dialogsNameFgOver
            : st::dialogsNameFg;

    const auto avatar_initial = primary_display.trimmed().left(1).toUpper();
    painter.setPen(Qt::NoPen);
    painter.setBrush(avatar_fill_color());
    painter.drawEllipse(avatar_rect);
    painter.setPen(QColor(Qt::white));
    auto avatar_font = base_font;
    avatar_font.setPixelSize(kAvatarLetterPixelSize);
    avatar_font.setWeight(QFont::DemiBold);
    painter.setFont(avatar_font);
    painter.drawText(avatar_rect, Qt::AlignCenter, avatar_initial);

    // Telegram's dialogs painter returns immediately after the userpic in its
    // narrow mode. Mirror that behavior when this row has room for no more than
    // one or two useful name glyphs; above the breakpoint both text lines use
    // their existing right elision instead of widening the canvas.
    const auto minimum_useful_text_width =
        QFontMetrics(primary_font).horizontalAdvance(QStringLiteral("MM…"));
    const auto paint_text = text_rect.width() >= minimum_useful_text_width;
    constexpr auto flags = Qt::AlignLeft | Qt::AlignVCenter;
    if (paint_text) {
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
        // Pale selection keeps normal muted secondary ink (not active-white).
        auto secondary_ink = over
            ? st::windowSubTextFgOver->c
            : st::windowSubTextFg->c;
        secondary_ink = secondary_ink.darker(118);
        const auto dot_top = secondary_rect.y()
            + (secondary_rect.height() - kStatusDotDiameter) / 2;
        painter.setPen(Qt::NoPen);
        painter.setBrush(status_color);
        painter.drawEllipse(
            secondary_rect.x(),
            dot_top,
            kStatusDotDiameter,
            kStatusDotDiameter);

        const auto status_text_rect = secondary_rect.adjusted(
            kStatusDotDiameter + kStatusDotGap, 0, 0, 0);
        painter.setFont(secondary_font);
        painter.setPen(secondary_ink);
        painter.drawText(
            status_text_rect,
            flags,
            QFontMetrics(secondary_font).elidedText(
                secondary_text,
                Qt::ElideRight,
                std::max(0, status_text_rect.width())));
    }

    if (unseen_count > 0 && unread_badge_width > 0) {
        auto badge_font = base_font;
        badge_font.setPointSize(12);
        badge_font.setWeight(QFont::Bold);
        const auto badge_top = paint_text
            ? secondary_rect.y()
                + (secondary_rect.height() - kUnreadHeight) / 2
            : content_rect.y()
                + (content_rect.height() - kUnreadHeight) / 2;
        const auto badge_rect = QRect(
            content_rect.right() - unread_badge_width + 1,
            badge_top,
            unread_badge_width,
            kUnreadHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(st::dialogsUnreadBg->c);
        painter.drawRoundedRect(
            QRectF(badge_rect),
            kUnreadHeight / 2.0,
            kUnreadHeight / 2.0);
        painter.setFont(badge_font);
        painter.setPen(st::dialogsUnreadFg->c);
        painter.drawText(badge_rect, Qt::AlignCenter, unread_label);
    }
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
    case AgentRole::main: return QStringLiteral("Main agent");
    case AgentRole::agent: return QStringLiteral("Agent");
    case AgentRole::human: return QStringLiteral("Human");
    case AgentRole::unknown: return QString();
    }
    return QString();
}

QString friendly_agent_lifecycle_text(const AgentRow &item) {
    if (item.lifecycle_state.empty()) {
        return QStringLiteral("—");
    }
    auto text = QString::fromStdString(item.lifecycle_state).toLower();
    text[0] = text[0].toUpper();
    return text;
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

    void set_unseen_counts(
            std::unordered_map<std::string, int> counts) {
        if (unseen_counts_ == counts) {
            return;
        }
        unseen_counts_ = std::move(counts);
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
    std::unordered_map<std::string, int> unseen_counts_;
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
    painter.fillRect(rect(), st::windowBg);
    const auto row_height = agent_row_height(font());
    for (auto index = std::size_t{0}; index != rows_.size(); ++index) {
        const auto &item = rows_[index];
        auto primary_text = path_text(item.directory_key);
        primary_text.replace(QLatin1Char('&'), QStringLiteral("&&"));
        const auto selected = selected_key_
            && *selected_key_ == item.directory_key;
        const auto over = (hovered_row_ && *hovered_row_ == index)
            || (pressed_row_ && *pressed_row_ == index);
        const auto key = path_text(item.directory_key).toStdString();
        const auto unseen = [&] {
            const auto found = unseen_counts_.find(key);
            return found == unseen_counts_.end() ? 0 : found->second;
        }();
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
            lifecycle_status_color(item),
            selected,
            over,
            unseen);
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
    // keep their identity: the selector menu's Open action drives that
    // button's click, and its disabled path row mirrors the current root.
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
    auto *open_new_window_button = new QPushButton(
        QStringLiteral("Open Project in Another Window…"), this);
    open_new_window_button->setObjectName(
        "lingtai_open_project_new_window_button");
    open_new_window_button->setAccessibleName(
        QStringLiteral("Open Project in Another Window"));
    open_new_window_button->setAccessibleDescription(QStringLiteral(
        "Open a project location in another LingTai window."));
    open_new_window_button->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Fixed);
    open_new_window_button->hide();
    header->addWidget(open_new_window_button);
    auto *selector = new ProjectSelectorButton(this);
    selector->setText(QStringLiteral("LingTai"));
    project_selector_ = selector;
    selector->setObjectName("lingtai_project_selector");
    selector->setAccessibleName(QStringLiteral("LingTai project selector"));
    header->addWidget(selector);
    layout->addLayout(header);

    auto *selector_menu = new ProjectSelectorPopover(selector);
    QObject::connect(selector_menu->open_action(), &QPushButton::clicked,
        [selector_menu, open_button] {
            selector_menu->hide();
            open_button->click();
        });
    QObject::connect(selector_menu->open_new_window_action(),
        &QPushButton::clicked,
        [selector_menu, open_new_window_button] {
            selector_menu->hide();
            open_new_window_button->click();
        });
    QObject::connect(selector, &QPushButton::clicked,
        [selector, selector_menu, project_root] {
            selector_menu->present(
                selector->mapToGlobal(QPoint(0, selector->height() + 4)),
                project_root->text());
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
    // The rows surface and its scroll viewport share the single-canvas
    // `windowBg` base instead of forming an independent raised field.
    auto viewport_palette = scroll_->viewport()->palette();
    viewport_palette.setColor(QPalette::Window, st::windowBg->c);
    scroll_->viewport()->setPalette(viewport_palette);
    scroll_->viewport()->setAutoFillBackground(true);
    roster_layout->addWidget(scroll_, 1);
    canvas_ = new AgentRowsCanvas(scroll_);
    canvas_->setObjectName("lingtai_agent_roster_rows");
    canvas_->setAccessibleName(QStringLiteral("Agent roster rows"));
    auto canvas_palette = canvas_->palette();
    canvas_palette.setColor(QPalette::Window, st::windowBg->c);
    canvas_->setPalette(canvas_palette);
    canvas_->setAutoFillBackground(true);
    scroll_->setWidget(canvas_);
    layout->addWidget(roster, 1);
    update_narrow_mode();
}

AgentRoster::~AgentRoster() = default;

void AgentRoster::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), st::windowBg);
}

void AgentRoster::set_roster_width(int width) {
    setFixedWidth(width);
    update_narrow_mode();
}

void AgentRoster::set_project_display_name(const QString &name) {
    project_display_name_ = name.isEmpty() ? QStringLiteral("LingTai") : name;
    update_narrow_mode();
}

void AgentRoster::update_narrow_mode() {
    const auto narrow = width() <= kNarrowRosterWidth;
    // Reserve the painted grid icon + gaps before eliding the folder name.
    const auto icon_gutter = kProjectIconSize + kProjectIconGap + 16;
    const auto available = narrow
        ? qMax(18, width() - 48 - icon_gutter)
        : qMax(40, width() - 58 - icon_gutter);
    project_selector_->setText(project_selector_->fontMetrics().elidedText(
        project_display_name_, Qt::ElideRight, available));
    roster_heading_->setVisible(!narrow);
    roster_state_->setVisible(!narrow);
}

void AgentRoster::set_row_click_handler(RowClickHandler handler) {
    canvas_->set_row_click_handler(std::move(handler));
}

void AgentRoster::set_unseen_counts(std::unordered_map<std::string, int> counts) {
    canvas_->set_unseen_counts(std::move(counts));
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
                || before.manifest_diagnostic != after.manifest_diagnostic
                || before.lifecycle_state != after.lifecycle_state) {
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
