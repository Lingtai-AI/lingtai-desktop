#include "native_shell.h"

#include "agent_detail_view.h"

#include "native_window_background.h"
#include "agent_preset_summary.h"
#include "agent_prompt_actions.h"
#include "agent_sleep.h"
#include "direct_conversation_history.h"
#include "direct_mail_publisher.h"
#include "message_reactions.h"
#include "preset_catalog_presentation.h"
#include "preset_editor_page.h"
#include "agent_presets_page.h"
#include "agent_config_page.h"
#include "codex_credentials_strip.h"
#include "credentials_page.h"
#include "kanban_page.h"
#include "project_setup_wizard.h"
#include "setup_style.h"
#include "slash_command.h"
#include "ui/palette_action_button.h"
#include "ui/palette_icon_button.h"
#include "ui/palette_surface.h"
#include "ui/preset_row_delegate.h"
#include "ui/roster_resize_handle.h"
#include "ui/selected_agent_avatar.h"
#include "ui/slash_command_card.h"

#include "base/event_filter.h"
#include "base/integration.h"

#include "styles/palette.h"
#include "styles/style_widgets.h"
#include "ui/conversation_surface.h"
#include "ui/effects/animations.h"
#include "ui/integration.h"
#include "ui/platform/mac/ui_window_title_mac.h"
#include "ui/rp_widget.h"
#include "ui/style/style_core_palette.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/rp_window.h"
#include "ui/widgets/shadow.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QLineF>
#include <QtCore/QPoint>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QFontMetrics>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtCore/QEvent>
#include <QtGui/QFontMetrics>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPen>
#include <QtGui/QPixmap>
#include <QtGui/QPalette>
#include <QtGui/QStyleHints>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QBoxLayout>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QWidget>

#include <rpl/range.h>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

namespace lingtai::desktop {
namespace {

constexpr auto kMinimumWindowWidth = 380;
constexpr auto kMinimumWindowHeight = 480;
constexpr auto kDefaultWindowWidth = 1100;
constexpr auto kDefaultWindowHeight = 720;

// Telegram's source-backed wide two-surface minima, from the pinned
// `computeColumnLayout` / `window.style`: the preferred list column is 260px
// and the detail column remains at least 380px. Unlike the old hard lower
// bound, a direct drag may collapse the roster to one 40px avatar plus the
// row and Sidebar framing (96px total), matching Telegram's narrow row paint.
// The default runtime ratio stays in the 22%-30% wide band; only an explicit
// drag below that band enters the collapsed range.
constexpr auto kRosterColumnWidth = 260;
constexpr auto kCollapsedRosterColumnWidth = 96;
constexpr auto kDetailColumnMinimumWidth = 380;
constexpr auto kRosterSeparatorWidth = 1;
constexpr auto kWideRosterWidthRatio = 0.22;
constexpr auto kMaximumRosterWidthRatio = 0.30;
constexpr auto kRosterResizeHandleWidth = 8;
constexpr auto kTwoColumnAvailableThreshold =
    kRosterColumnWidth + kDetailColumnMinimumWidth;

namespace fs = std::filesystem;

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
    label->setWordWrap(true);
    // A wrapped label only reports its true wrapped height to the layout when
    // its policy opts into height-for-width; without this the detail column
    // under-measures every label and draws them over one another.
    auto policy = label->sizePolicy();
    policy.setHeightForWidth(true);
    policy.setVerticalPolicy(QSizePolicy::MinimumExpanding);
    label->setSizePolicy(policy);
    auto font = label->font();
    font.setPointSize(point_size);
    font.setWeight(weight);
    label->setFont(font);
    return label;
}

// The first-project surfaces use the same mature lib_ui label language as the
// accepted composer/dashboard: a FlatLabel whose accessible text is its own
// readable text, with the exact current object name and accessibility label.
Ui::FlatLabel *make_flat_label(
        QWidget *parent,
        const QString &text,
        const char *object_name) {
    auto *label = new Ui::FlatLabel(parent, text);
    label->setObjectName(object_name);
    label->setAccessibleName(text);
    return label;
}

void apply_preset_catalog_chrome(QWidget *root);

void apply_project_setup_palette(QWidget *root) {
    if (!root) return;
    auto palette = root->palette();
    const auto dark = st::windowBg->c.lightness() < 128;
    palette.setColor(QPalette::Window,
        dark ? QColor(QStringLiteral("#181B1A"))
             : QColor(QStringLiteral("#F7F7F7")));
    palette.setColor(QPalette::WindowText, st::windowFg->c);
    palette.setColor(QPalette::Text, st::windowFg->c);
    palette.setColor(QPalette::Base, dark
        ? QColor(QStringLiteral("#181B1A"))
        : QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::AlternateBase, st::windowBgOver->c);
    palette.setColor(QPalette::ButtonText, st::windowFg->c);
    root->setPalette(palette);
    root->setAutoFillBackground(false);
    apply_preset_catalog_chrome(root);
}

// Preset catalog constants: see ui/preset_row_delegate.h
// Search layout constants remain local.
constexpr auto kPresetSearchPreferredWidth = 480;
constexpr auto kPresetSearchMinWidth = 240;
constexpr auto kPresetSearchMaxWidth = 520;

// PresetCatalogTokens, preset_catalog_tokens: see ui/preset_row_delegate.h

void apply_one_preset_catalog_chrome(QTreeWidget *table, const QPalette &root_palette) {
    if (!table) return;
    const auto tokens = preset_catalog_tokens(root_palette);
    auto palette = table->palette();
    palette.setColor(QPalette::Base, tokens.surface);
    palette.setColor(QPalette::AlternateBase, tokens.surface);
    table->setPalette(palette);
    table->viewport()->setPalette(palette);
    table->viewport()->setAutoFillBackground(true);
    auto header_palette = table->header()->palette();
    header_palette.setColor(QPalette::Button, tokens.header);
    header_palette.setColor(QPalette::Window, tokens.header);
    header_palette.setColor(QPalette::ButtonText, tokens.section_text);
    header_palette.setColor(QPalette::WindowText, tokens.section_text);
    table->header()->setPalette(header_palette);
    table->header()->setAutoFillBackground(true);
    const auto divider = tokens.divider.alpha() < 255
        ? QStringLiteral("rgba(%1, %2, %3, %4)")
            .arg(tokens.divider.red())
            .arg(tokens.divider.green())
            .arg(tokens.divider.blue())
            .arg(tokens.divider.alpha())
        : tokens.divider.name(QColor::HexRgb).toUpper();
    const auto border = tokens.border.alpha() < 255
        ? divider
        : tokens.border.name(QColor::HexRgb).toUpper();
    table->setStyleSheet(QStringLiteral(
        "QTreeWidget { background: %1; border: 1px solid %2; "
        "border-radius: 10px; outline: none; } "
        "QTreeWidget::item { padding: 0px; border: none; } "
        "QTreeWidget::item:selected { background: %3; } "
        "QHeaderView::section { background: %4; color: %5; border: none; "
        "border-bottom: 1px solid %2; padding: 7px 12px; "
        "font-size: 11px; font-weight: 600; }")
        .arg(tokens.surface.name(QColor::HexRgb).toUpper(),
            border,
            tokens.selected_row.name(QColor::HexRgb).toUpper(),
            tokens.header.name(QColor::HexRgb).toUpper(),
            tokens.section_text.name(QColor::HexRgb).toUpper()));
}

void apply_preset_catalog_chrome(QWidget *root) {
    if (!root) return;
    for (auto *table : root->findChildren<QTreeWidget *>()) {
        const auto name = table->objectName();
        if (name != QStringLiteral("lingtai_setup_preset_catalog")
                && name != QStringLiteral(
                    "lingtai_selected_agent_preset_summary")) {
            continue;
        }
        apply_one_preset_catalog_chrome(table, root->palette());
    }
    if (auto *search = root->findChild<QLineEdit *>(
            "lingtai_setup_preset_search")) {
        apply_setup_line_edit(search, setup_tokens(root->palette()));
    }
}

// is_preset_section_index, preset_section_band_rect: see ui/preset_row_delegate.h

// PresetRowDelegate: see ui/preset_row_delegate.h

QStringList preset_capability_tags(bool has_vision, bool has_tools) {
    QStringList tags;
    if (has_vision) {
        tags << QStringLiteral("Vision");
    }
    if (has_tools) {
        tags << QStringLiteral("Tools");
    }
    return tags;
}

QString preset_footer_plain(
        const QString &name,
        const QString &provider_model,
        const QStringList &tags) {
    auto parts = QStringList();
    if (!name.isEmpty()) {
        parts << name;
    }
    if (!provider_model.isEmpty()) {
        parts << provider_model;
    }
    if (!tags.isEmpty()) {
        parts << tags.join(QStringLiteral(", "));
    }
    return parts.join(QStringLiteral(" · "));
}

QString preset_footer_rich(
        const QString &name,
        const QString &provider_model,
        const QStringList &tags,
        const QColor &accent,
        const QColor &muted) {
    const auto rest = preset_footer_plain({}, provider_model, tags);
    if (name.isEmpty()) {
        if (rest.isEmpty()) return {};
        return QStringLiteral("<span style=\"color:%1\">%2</span>")
            .arg(muted.name(QColor::HexRgb), rest.toHtmlEscaped());
    }
    auto html = QStringLiteral(
        "<span style=\"color:%1;font-weight:600\">%2</span>")
        .arg(accent.name(QColor::HexRgb), name.toHtmlEscaped());
    if (!rest.isEmpty()) {
        html += QStringLiteral(" · <span style=\"color:%1\">%2</span>")
            .arg(muted.name(QColor::HexRgb), rest.toHtmlEscaped());
    }
    return html;
}

// is_preset_section, add_preset_section, configure_preset_table: see ui/preset_row_delegate.h

void add_preset_catalog_row(
        QTreeWidget *table, const PresetCatalogRow &row, int index) {
    const auto name = QString::fromStdString(row.entry.name);
    const auto tags = preset_capability_tags(row.has_vision, row.has_tools);
    auto *item = new QTreeWidgetItem(table);
    item->setData(0, Qt::UserRole, index);
    item->setData(0, kPresetSummaryRole, row.summary);
    item->setData(0, kPresetCapabilitiesRole, tags);
    item->setData(0, Qt::UserRole + 8, QString::fromStdString(row.entry.path));
    item->setText(0, name);
    item->setText(1, row.provider_model);
    item->setText(2, tags.join(QStringLiteral(", ")));
    item->setToolTip(0, row.summary);
    item->setToolTip(1, row.provider_model);
}

// adjacent_preset_row: see ui/preset_row_delegate.h

void recompute_setup_layout(QWidget *root) {
    if (!root) return;
    const auto size = root->size();
    const auto t_w = std::clamp(size.width() / 920.0, 0.85, 1.25);
    const auto t_h = std::clamp(size.height() / 840.0, 0.85, 1.25);
    const auto margin = qRound(32 * t_w);
    if (auto *steps = root->findChild<QWidget *>("lingtai_setup_steps")) {
        steps->setFixedHeight(48);
        if (auto *layout = qobject_cast<QHBoxLayout *>(steps->layout())) {
            layout->setContentsMargins(margin, qRound(8 * t_h), margin, 0);
            layout->setSpacing(qRound(10 * t_w));
        }
        for (auto *connector : steps->findChildren<QLabel *>()) {
            if (connector->text().startsWith(QStringLiteral("─"))) {
                connector->setFixedWidth(qRound(36 * t_w));
            }
        }
    }
    if (auto *body = root->findChild<QWidget *>("lingtai_setup_body")) {
        if (auto *layout = body->layout()) {
            const auto pad = qRound(32 * t_w);
            layout->setContentsMargins(
                pad, qRound(4 * t_h), pad, qRound(16 * t_h));
        }
    }
    if (auto *search = root->findChild<QLineEdit *>(
            "lingtai_setup_preset_search")) {
        const auto available = std::max(0, root->width() - 2 * margin);
        search->setFixedWidth(std::clamp(
            available, kPresetSearchMinWidth, kPresetSearchPreferredWidth));
        search->setMaximumWidth(kPresetSearchMaxWidth);
    }
    if (auto *search = root->findChild<QLineEdit *>(
            "lingtai_setup_agents_search")) {
        const auto available = std::max(0, root->width() - 2 * margin);
        search->setFixedWidth(std::clamp(
            available, kPresetSearchMinWidth, kPresetSearchPreferredWidth));
        search->setMaximumWidth(kPresetSearchMaxWidth);
    }
}

void update_setup_step_indicator(QWidget *steps, int active_index) {
    if (!steps) return;
    const auto names = std::array<const char *, 3>{
        "lingtai_setup_step_preset",
        "lingtai_setup_step_agents",
        "lingtai_setup_step_review",
    };
    const auto badges = std::array<const char *, 3>{
        "lingtai_setup_step_badge_preset",
        "lingtai_setup_step_badge_agents",
        "lingtai_setup_step_badge_review",
    };
    const auto tokens = setup_tokens(steps->palette());
    const auto accent = setup_color_css(tokens.selection_accent);
    for (auto index = 0; index != 3; ++index) {
        const auto active = index == active_index;
        const auto complete = index < active_index;
        if (auto *badge = steps->findChild<QLabel *>(badges[index])) {
            badge->setText(complete
                ? QStringLiteral("✓")
                : QString::number(index + 1));
            badge->setStyleSheet(active || complete
                ? QStringLiteral(
                    "background: %1; color: white; border-radius: 11px;")
                    .arg(accent)
                : QStringLiteral(
                    "background: transparent; color: #8a8f98; "
                    "border: 1px solid palette(mid); border-radius: 11px;"));
        }
        if (auto *label = steps->findChild<QLabel *>(names[index])) {
            label->setStyleSheet(active || complete
                ? QStringLiteral("color: %1;").arg(accent)
                : QStringLiteral("color: #8a8f98;"));
            auto font = label->font();
            font.setWeight(active ? QFont::DemiBold : QFont::Normal);
            label->setFont(font);
        }
    }
    if (auto *index_label = steps->findChild<QLabel *>("lingtai_setup_step_index")) {
        index_label->setText(QStringLiteral("%1 of 3").arg(active_index + 1));
    }
}

// The vendored lib_ui controls (InputField, RoundButton, FlatLabel) carry no
// Q_OBJECT macro, so Qt's templated findChild<Ui::X *> cannot name them. These
// classes are polymorphic, so resolving by object name through a QObject
// lookup plus dynamic_cast stays safe and type-checked.
template <typename Widget>
Widget *find_ui_child(QObject &root, const char *object_name) {
    return dynamic_cast<Widget *>(root.findChild<QObject *>(object_name));
}

bool open_error_active(Ui::RpWindow &window) {
    auto *label = find_ui_child<Ui::FlatLabel>(
        window, "lingtai_project_open_error");
    return label && !label->accessibilityName().isEmpty();
}

// PaletteSurface: see ui/palette_surface.h

// One Telegram-shaped Composer control envelope: attachment, field and Send
// share this single rounded base and its one-pixel adaptive palette border.
// Status text stays in the outer Composer lane rather than widening this frame.
bool system_prefers_dark_palette();

// Ted's two canonical startup marks are packaged with every native-shell
// consumer. The widget selects at paint time so the existing palette-change
// pass can switch the logo without reconstructing the launch route.
class StartupIllustration final : public QWidget {
public:
    explicit StartupIllustration(QWidget *parent)
    : QWidget(parent)
    , light_(QStringLiteral(":/lingtai/startup/lingtai-logo-light-4096.png"))
    , dark_(QStringLiteral(":/lingtai/startup/lingtai-logo-dark-4096.png")) {
        setFixedSize(220, 190);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setProperty("lingtai_light_logo_resource",
            QStringLiteral(":/lingtai/startup/lingtai-logo-light-4096.png"));
        setProperty("lingtai_dark_logo_resource",
            QStringLiteral(":/lingtai/startup/lingtai-logo-dark-4096.png"));
    }

protected:
    void paintEvent(QPaintEvent *) override {
        const auto &logo = system_prefers_dark_palette() ? dark_ : light_;
        if (logo.isNull()) return;
        const auto scaled = logo.size().scaled(size(), Qt::KeepAspectRatio);
        const auto target = QRect(
            QPoint((width() - scaled.width()) / 2,
                (height() - scaled.height()) / 2),
            scaled);
        // Recolor the packaged jade glyph to the composer Send accent so the
        // launch mark stays on the same blue family as chat chrome.
        auto tinted = logo;
        {
            QPainter tint(&tinted);
            tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tint.fillRect(tinted.rect(), st::windowBgActive->c);
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawPixmap(target, tinted);
    }

private:
    QPixmap light_;
    QPixmap dark_;
};

class ComposerControls final : public Ui::RpWidget {
public:
    explicit ComposerControls(QWidget *parent)
    : Ui::RpWidget(parent) {
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto outline = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const auto radius = outline.height() / 2.0;
        painter.setPen(QPen(st::shadowFg->c, 1.0));
        painter.setBrush(st::windowBg->c);
        painter.drawRoundedRect(outline, radius, radius);
    }
};

// A flat vector paperclip avoids both the former platform-framed `+` button
// and an icon-font/emoji dependency while retaining the semantic Attach name.
class ComposerAttachmentButton final : public QPushButton {
public:
    explicit ComposerAttachmentButton(QWidget *parent)
    : QPushButton(parent) {
        setFixedSize(40, 40);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (underMouse() && isEnabled()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(st::windowBgOver->c);
            painter.drawEllipse(rect().adjusted(2, 2, -2, -2));
        }
        QPainterPath clip;
        clip.moveTo(11.0, 19.0);
        clip.lineTo(20.5, 9.5);
        clip.cubicTo(23.0, 7.0, 27.0, 10.5, 24.5, 13.0);
        clip.lineTo(14.0, 23.5);
        clip.cubicTo(10.0, 27.5, 4.5, 22.0, 8.5, 18.0);
        clip.lineTo(18.0, 8.5);
        const auto clip_center = clip.boundingRect().center();
        const auto button_center = QRectF(rect()).center();
        // Its stroke is geometrically centered already; this quarter-point
        // optical correction balances the heavier lower-right hook at 2x.
        painter.translate(
            button_center - clip_center + QPointF(0.25, -0.25));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(st::windowSubTextFg->c, 1.8, Qt::SolidLine,
            Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(clip);
    }
};

// Keep the Telegram RoundButton interaction/ripple surface, but paint the Send
// arrow as vector ink so its visible bounds share the blue circle's true center
// instead of inheriting a font glyph's asymmetric bearings and baseline.
class ComposerSendButton final : public Ui::RoundButton {
public:
    ComposerSendButton(QWidget *parent, const style::RoundButton &style)
    : Ui::RoundButton(parent, rpl::single(QString()), style) {
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Ui::RoundButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(st::defaultActiveButton.textFg->c, 2.0,
            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const auto center = QPointF(width() / 2.0, height() / 2.0);
        QPainterPath arrow;
        arrow.moveTo(center.x(), center.y() + 5.5);
        arrow.lineTo(center.x(), center.y() - 5.5);
        arrow.moveTo(center.x() - 4.5, center.y() - 1.0);
        arrow.lineTo(center.x(), center.y() - 5.5);
        arrow.lineTo(center.x() + 4.5, center.y() - 1.0);
        painter.drawPath(arrow);
    }
};

// RosterResizeHandle: see ui/roster_resize_handle.h

// Compact selected-Agent page navigation: a plain text tab that is never a
// filled rectangular slab, or a Kanban-style back link (`←  …`) when the
// caption starts with an arrow. Tabs paint caption glyphs on the transparent
// shell backdrop, with a short `dialogsBgActive` underline on the selected
// page. Back links use primary ink, left alignment, and no underline so they
// read as a way back from `/presets`.
class PageNavButton final : public QPushButton {
public:
    explicit PageNavButton(QWidget *parent, const QString &text)
    : QPushButton(text, parent) {
        setCheckable(true);
        setFixedHeight(28);
        setCursor(Qt::PointingHandCursor);
        auto font = this->font();
        font.setPointSize(13);
        font.setWeight(QFont::Normal);
        setFont(font);
        if (text.startsWith(QStringLiteral("←"))) {
            setMinimumWidth(QFontMetrics(font).horizontalAdvance(text) + 12);
        }
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setFont(font());
        const auto back_link = text().startsWith(QStringLiteral("←"));
        painter.setPen(back_link || isChecked()
            ? st::dialogsNameFg->c
            : st::windowSubTextFg->c);
        painter.drawText(
            rect().adjusted(back_link ? 2 : 0, 0, 0, 0),
            (back_link ? Qt::AlignLeft : Qt::AlignCenter) | Qt::AlignVCenter,
            text());
        if (isChecked() && !back_link) {
            const auto underline_width = qMin(width(), 24);
            painter.fillRect(
                (width() - underline_width) / 2, height() - 2,
                underline_width, 2, st::dialogsBgActive);
        }
    }
};

// One compact palette-owned action button shared by the three detail-top-bar
// actions (Back, Start Agent, Request sleep): it paints its resting/hover text
// and backgrounds from the same shared lib_ui light-button tokens the dialog
// actions use, and the disabled state from the existing disabled text token,
// so the three controls never fall back to the raw platform button style.
// PaletteActionButton: see ui/palette_action_button.h

// PaletteIconButton: see ui/palette_icon_button.h

// paint_slash_glyph: see ui/slash_command_card.cpp

// SlashOfferDelegate, SlashCommandCard: see ui/slash_command_card.h

void apply_slash_popup_palette(QListWidget *popup) {
    if (!popup) return;
    auto *parent = popup->parentWidget();
    if (parent
            && parent->objectName()
                == QStringLiteral("lingtai_slash_command_card")) {
        if (auto *card = static_cast<SlashCommandCard *>(parent)) {
            card->apply_palette();
        }
    }
}

void hide_slash_command_popup(QWidget *window) {
    if (auto *popup = window->findChild<QListWidget *>(
            "lingtai_slash_command_popup")) {
        if (auto *card = popup->parentWidget()) {
            card->hide();
        } else {
            popup->hide();
        }
    }
}

[[maybe_unused]] void apply_slash_popup_choice(
        Ui::InputField *input, QListWidget *popup) {
    if (!input || !popup) return;
    const auto *item = popup->currentItem();
    if (!item) return;
    const auto name = item->data(Qt::UserRole).toString();
    if (auto *card = popup->parentWidget()) {
        card->hide();
    } else {
        popup->hide();
    }
    input->setText(QStringLiteral("/") + name);
    input->setFocus();
}

void position_slash_command_popup(
        QListWidget *popup, Ui::InputField *input) {
    auto *card = popup ? popup->parentWidget() : nullptr;
    if (!popup || !input || !card || !card->parentWidget()) return;
    const auto origin = input->mapTo(card->parentWidget(), QPoint(0, 0));
    const auto width = std::clamp(input->width() + 2 * kSlashCardShadow, 340, 460);
    const auto rows = std::min(popup->count(), 8);
    const auto height = 44 + rows * kSlashRowHeight + 28 + kSlashCardShadow;
    card->setFixedSize(width, height);
    auto top = origin.y() - card->height() + 10;
    if (top < 4) {
        top = origin.y() + input->height() - 4;
    }
    const auto left = origin.x() - kSlashCardShadow;
    card->move(std::max(8, left), top);
    card->raise();
}

[[maybe_unused]] void refresh_slash_command_popup(
        QWidget *window, Ui::InputField *input) {
    auto *popup = window->findChild<QListWidget *>(
        "lingtai_slash_command_popup");
    if (!popup || !input) return;
    const auto matches = matching_slash_commands(
        input->getLastText().toStdString());
    auto *card = popup->parentWidget();
    if (matches.empty()) {
        if (card) card->hide();
        else popup->hide();
        return;
    }
    popup->clear();
    for (const auto &offer : matches) {
        auto *item = new QListWidgetItem();
        item->setData(Qt::UserRole, QString::fromUtf8(offer.name));
        item->setData(Qt::UserRole + 1, QString::fromUtf8(offer.description));
        item->setText(QStringLiteral("/%1  %2")
            .arg(QString::fromUtf8(offer.name),
                QString::fromUtf8(offer.description)));
        popup->addItem(item);
    }
    popup->setCurrentRow(0);
    position_slash_command_popup(popup, input);
    if (card) {
        card->show();
        card->raise();
    }
    popup->show();
}

// SelectedAgentAvatar: see ui/selected_agent_avatar.h

// One shared structural owner for the one retained read-only selected-Agent
// source section (Presets). Each section directly owns its own semibold
// heading, read-only plain-text surface, and state line, with the same inner
// margins and spacing, so the retained source uses one consistent local
// framing instead of a hand-built heading/surface/state sequence. Sections
// never frame themselves with a plain-shadow block: they separate by the
// surrounding layout spacing alone.
struct DashboardSection {
    Ui::RpWidget *owner = nullptr;
    QLabel *heading = nullptr;
    QTreeWidget *surface = nullptr;
    QLabel *state = nullptr;
};

constexpr auto kDashboardSectionSurfaceHeight = 300;

[[maybe_unused]] DashboardSection add_dashboard_section(
        Ui::RpWidget *detail,
        QVBoxLayout *detail_layout,
        const char *kind,
        const QString &heading_text,
        const QString &surface_accessible_name,
        const QString &surface_accessible_description) {
    const auto base = QStringLiteral("lingtai_selected_agent_")
        + QString::fromLatin1(kind);
    auto *owner = new Ui::RpWidget(detail);
    owner->setObjectName(base + QStringLiteral("_section"));
    owner->setAccessibleName(heading_text);
    owner->setMinimumWidth(0);
    owner->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto *layout = new QVBoxLayout(owner);
    layout->setContentsMargins(0, 8, 0, 8);
    layout->setSpacing(6);
    auto *heading = make_label(
        owner, heading_text,
        (base + QStringLiteral("_heading")).toUtf8().constData(), 12,
        QFont::DemiBold);
    layout->addWidget(heading);
    auto *surface = new QTreeWidget(owner);
    surface->setObjectName(base);
    surface->setAccessibleName(surface_accessible_name);
    surface->setAccessibleDescription(surface_accessible_description);
    surface->setColumnCount(3);
    surface->setHeaderLabels({QStringLiteral("Preset"),
        QStringLiteral("Provider · Model"), QStringLiteral("Capabilities")});
    configure_preset_table(surface);
    surface->setMinimumWidth(0);
    surface->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    surface->setMinimumHeight(kDashboardSectionSurfaceHeight);
    layout->addWidget(surface, 1);
    auto *state = make_label(
        owner, QString(), (base + QStringLiteral("_state")).toUtf8().constData(),
        10);
    state->setAccessibleName(surface_accessible_name + QStringLiteral(" state"));
    layout->addWidget(state);
    auto *separator = new Ui::PlainShadow(owner);
    separator->setObjectName(
        (base + QStringLiteral("_separator")).toUtf8().constData());
    separator->setFixedHeight(1);
    layout->addWidget(separator);
    detail_layout->addWidget(owner, 1);
    return {owner, heading, surface, state};
}

QString path_text(const fs::path &path) {
    const auto bytes = path.u8string();
    return QString::fromUtf8(
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<qsizetype>(bytes.size()));
}

QString value_text(const std::optional<std::string> &value) {
    return value ? QString::fromStdString(*value) : QStringLiteral("unavailable");
}

QString value_text(const std::optional<std::int64_t> &value) {
    return value ? QString::number(*value) : QStringLiteral("unavailable");
}

QString value_text(const std::optional<double> &value) {
    return value ? QString::number(*value, 'g', 15)
                 : QStringLiteral("unavailable");
}

QString value_text(const std::optional<bool> &value) {
    return value ? (*value ? QStringLiteral("true") : QStringLiteral("false"))
                 : QStringLiteral("unavailable");
}

QString joined_names(const std::vector<std::string> &names) {
    auto text = QStringList();
    for (const auto &name : names) text.push_back(QString::fromStdString(name));
    return names.empty() ? QStringLiteral("none") : text.join(QStringLiteral(", "));
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

const AgentRow *selectable_item(const AgentSnapshot &snapshot, const fs::path &key) {
    const auto found = std::ranges::find_if(snapshot.items,
        [&](const auto &item) {
            return item.directory_key == key
                && item.manifest_kind == AgentManifestKind::valid;
        });
    return found == snapshot.items.end() ? nullptr : &*found;
}

// Desktop's own product gate, not kernel-core enforcement: a valid manifest,
// a main/agent role (never human/unknown), the canonical strict `< 5.0 s`
// heartbeat predicate, and a known current manifest state -- from
// `.agent.json.state`, not the best-effort `.status.json` -- other than
// `asleep`/`suspended`. No `.status.json.running`, PID, active turn,
// `admin.karma`, or compatibility probe is ever consulted.
bool agent_sleep_eligible(const AgentRow &item) {
    if (item.manifest_kind != AgentManifestKind::valid) return false;
    if (item.role != AgentRole::main && item.role != AgentRole::agent) {
        return false;
    }
    if (item.presence != AgentPresenceKind::alive) return false;
    if (!item.identity || !item.identity->state) return false;
    return *item.identity->state != "asleep"
        && *item.identity->state != "suspended";
}

// Desktop's own product gate for showing Start Agent at all: a valid
// manifest, a main/agent role (never human/unknown), and exactly a stale or
// missing heartbeat -- the two presence kinds a genuine new heartbeat write
// can honestly transition out of. `invalid` (for example a future-dated
// heartbeat) and `unavailable` are deliberately excluded: either could
// later read as `alive` from wall-clock movement alone or a transient read
// failure, with no real new write, which would let the post-click success
// check in `tick_agent_start_observation()` claim a false "Agent is
// online." without ever consulting a heartbeat timestamp baseline.
bool agent_start_eligible(const AgentRow &item) {
    if (item.manifest_kind != AgentManifestKind::valid) return false;
    if (item.role != AgentRole::main && item.role != AgentRole::agent) {
        return false;
    }
    return item.presence == AgentPresenceKind::stale
        || item.presence == AgentPresenceKind::missing;
}

// Desktop never installs the base::Integration the vendored InputField's Qt
// signal producer needs: it delivers QTextDocument::contentsChange through
// base::Integration::Instance(), and with no instance installed the first
// composer clear() trips the base assertion and crashes. One process-lifetime
// minimum adapter suffices, mirroring how Core::BaseIntegration wraps the
// base contract: direct event-loop delivery plus no-op logging.
class DesktopBaseIntegration final : public base::Integration {
public:
    DesktopBaseIntegration()
    : base::Integration(0, nullptr) {
        base::Integration::Set(this);
    }

    void enterFromEventLoop(FnMut<void()> &&method) override {
        std::move(method)();
    }

    bool logSkipDebug() override {
        return true;
    }

    void logMessageDebug(const QString &message) override {
    }

    void logMessage(const QString &message) override {
    }

};

// lib_ui's own Integration is equally required: the vendored InputField calls
// Ui::Integration::Instance() from its contents-changed handler, and without
// an installed instance that assertion fires on the first composer clear().
// The base class implements every non-pure virtual with a safe default, so
// this adapter only supplies the eight pure slots with no-op semantics.
class DesktopUiIntegration final : public Ui::Integration {
public:
    DesktopUiIntegration() {
        Ui::Integration::Set(this);
    }

    void postponeCall(FnMut<void()> &&callable) override {
        std::move(callable)();
    }

    void registerLeaveSubscription(not_null<QWidget *> widget) override {
    }

    void unregisterLeaveSubscription(not_null<QWidget *> widget) override {
    }

    [[nodiscard]] QString emojiCacheFolder() override {
        return QString();
    }

    [[nodiscard]] QString openglCheckFilePath() override {
        return QString();
    }

    [[nodiscard]] QString angleBackendFilePath() override {
        return QString();
    }

    void touchCounterIncrement() override {
    }

    [[nodiscard]] int touchCounterNow() override {
        return 0;
    }

};

bool system_prefers_dark_palette() {
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Dark) return true;
    if (scheme == Qt::ColorScheme::Light) return false;
    const auto palette = QGuiApplication::palette();
    return palette.color(QPalette::WindowText).lightness()
        > palette.color(QPalette::Window).lightness();
}

void apply_telegram_night_palette() {
    const auto set = [](const char *name, const char *hex) {
        const auto color = QColor(QString::fromLatin1(hex));
        const auto result = style::main_palette::setColor(
            QLatin1String(name),
            static_cast<uchar>(color.red()),
            static_cast<uchar>(color.green()),
            static_cast<uchar>(color.blue()),
            static_cast<uchar>(color.alpha()));
        Q_ASSERT(result == style::palette::SetResult::Ok
            || result == style::palette::SetResult::Duplicate);
        (void)result;
    };

    set("windowBg", "#17212b");
    set("windowFg", "#f5f5f5");
    set("windowBgOver", "#232e3c");
    set("windowBgRipple", "#24303d");
    set("windowFgOver", "#f5f5f5");
    set("windowSubTextFg", "#708499");
    set("windowSubTextFgOver", "#7f91a4");
    set("windowBoldFg", "#e9e8e8");
    set("windowBoldFgOver", "#e9e8e8");
    set("windowBgActive", "#2b5278");
    set("windowFgActive", "#ffffff");
    set("windowActiveTextFg", "#6ab3f3");
    set("activeButtonBg", "#2b5278");
    set("activeButtonBgOver", "#356487");
    set("activeButtonBgRipple", "#3b6d91");
    set("activeButtonFg", "#ffffff");
    set("activeButtonFgOver", "#ffffff");
    set("activeLineFg", "#6ab3f3");
    set("lightButtonBg", "#17212b");
    set("lightButtonBgOver", "#232e3c");
    set("lightButtonBgRipple", "#24303d");
    set("lightButtonFg", "#6ab3f3");
    set("lightButtonFgOver", "#6ab3f3");
    set("placeholderFg", "#708499");
    set("placeholderFgActive", "#7f91a4");
    set("inputBorderFg", "#24303d");
    set("dialogsBg", "#17212b");
    set("dialogsNameFg", "#f5f5f5");
    set("dialogsNameFgOver", "#f5f5f5");
    set("dialogsNameFgActive", "#ffffff");
    set("dialogsTextFg", "#7f91a4");
    set("dialogsTextFgOver", "#7f91a4");
    set("dialogsTextFgActive", "#e4ecf2");
    set("dialogsBgOver", "#202b36");
    set("dialogsBgActive", "#2b5278");
    set("historyTextInFg", "#f5f5f5");
    set("historyTextOutFg", "#e4ecf2");
    set("msgInBg", "#182533");
    set("msgOutBg", "#2b5278");
    set("msgServiceFg", "#708499");
}

void apply_system_palette() {
    style::main_palette::reset();
    if (system_prefers_dark_palette()) {
        apply_telegram_night_palette();
    }
}

void apply_titlebar_brand_palette(QWidget *window) {
    if (!window) return;
    auto *brand = window->findChild<QLabel *>(
        QStringLiteral("lingtai_titlebar_brand"));
    if (!brand) return;
    auto palette = brand->palette();
    palette.setColor(QPalette::WindowText, st::dialogsNameFg->c);
    palette.setColor(QPalette::Text, st::dialogsNameFg->c);
    brand->setPalette(palette);
}

std::unique_ptr<Ui::RpWindow> make_native_window() {
    // Install the adapters before any vendored widget is constructed, unless
    // a hosting environment already installed them.
    static const auto integration_installed = [] {
        if (!base::Integration::Exists()) {
            static DesktopBaseIntegration base_integration;
        }
        if (!Ui::Integration::Exists()) {
            static DesktopUiIntegration ui_integration;
        }
        // The vendored InputField's placeholder animation asserts unless the
        // process-global animations manager exists, so own exactly one.
        static Ui::Animations::Manager animations_manager;
        return true;
    }();
    (void)integration_installed;

    // The roster paints generated palette tokens; that palette must be ready
    // before any window is built. The vendored widget-style module is started
    // only after the window exists: its generated custom-title style would
    // otherwise give the mac window helper a nonzero title height and change
    // the explicit window minimum, while the composer controls (which need
    // those styles) are constructed only after this function returns.
    static const auto palette_started = [] {
        style::internal::init_palette(style::kScaleDefault);
        apply_system_palette();
        return true;
    }();
    (void)palette_started;
    auto result = std::make_unique<Ui::RpWindow>();
    static const auto widget_styles_started = [] {
        style::internal::init_style_widgets(style::kScaleDefault);
        return true;
    }();
    (void)widget_styles_started;
    // Keep Telegram's real macOS TitleWidget owner. Its height comes from the
    // native contentLayoutRect, so the traffic lights, app brand and body all
    // share one borderless window canvas instead of a Qt label being pasted
    // into the content row below it.
    static const auto desktop_title_style = [] {
        auto style_copy = st::defaultWindowTitle;
        style_copy.bg = st::windowBg;
        style_copy.bgActive = st::windowBg;
        style_copy.fg = st::dialogsNameFg;
        style_copy.fgActive = st::dialogsNameFg;
        return style_copy;
    }();
    result->setTitleStyle(desktop_title_style);
    // Telegram's MainWindow::updatePalette assigns one app-owned base to the
    // transparent macOS title bar and the content canvas. lib_ui already owns
    // NoTitleBarBackgroundHint/full-size native title behavior here.
    auto palette = result->palette();
    palette.setColor(QPalette::Window, st::windowBg->c);
    result->setPalette(palette);
    ApplyNativeWindowBackground(result.get(), st::windowBg->c);
    result->setProperty("lingtai_window_surface_color", st::windowBg->c);
    return result;
}

} // namespace

NativeShell::NativeShell(RuntimeOptions runtime_options)
: runtime_options_(runtime_options)
, window_(make_native_window()) {
    window_->setObjectName("lingtai_desktop_window");
    window_->setTitle(QString());
    window_->setWindowTitle(QStringLiteral("LingTai Desktop"));
    window_->setAccessibleName(QStringLiteral("LingTai Desktop"));
    window_->setAccessibleDescription(QStringLiteral(
        "A native desktop workspace for inspecting LingTai projects and Agents."));
    window_->setMinimumSize(QSize(kMinimumWindowWidth, kMinimumWindowHeight));
    window_->resize(kDefaultWindowWidth, kDefaultWindowHeight);

    auto *body = window_->body().get();
    body->setObjectName("lingtai_desktop_body");
    body->setAccessibleName(QStringLiteral("LingTai Desktop workspace"));

    auto *shell_layout = new QHBoxLayout(body);
    shell_layout->setContentsMargins(0, 0, 0, 0);
    shell_layout->setSpacing(0);

    auto *startup_route = new PaletteSurface(body, st::windowBg);
    startup_route_ = startup_route;
    startup_route->setObjectName("lingtai_startup_route");
    startup_route->setAccessibleName(QStringLiteral("Choose a LingTai project"));
    auto *startup_layout = new QVBoxLayout(startup_route);
    startup_layout->setContentsMargins(32, 24, 32, 40);
    startup_layout->setSpacing(0);
    startup_layout->addStretch(4);
    auto *startup_illustration = new StartupIllustration(startup_route);
    startup_illustration->setObjectName("lingtai_startup_illustration");
    startup_layout->addWidget(startup_illustration, 0, Qt::AlignHCenter);
    startup_layout->addSpacing(18);
    auto *startup_heading = new QLabel(
        QStringLiteral("LingTai Orchestration"), startup_route);
    startup_heading->setObjectName("lingtai_startup_heading");
    startup_heading->setAlignment(Qt::AlignCenter);
    auto heading_font = startup_heading->font();
    heading_font.setFamily(QStringLiteral("Menlo"));
    heading_font.setStyleHint(QFont::Monospace);
    heading_font.setPixelSize(16);
    heading_font.setWeight(QFont::DemiBold);
    startup_heading->setFont(heading_font);
    auto startup_palette = startup_heading->palette();
    startup_palette.setColor(QPalette::WindowText, st::windowBgActive->c);
    startup_heading->setPalette(startup_palette);
    startup_layout->addWidget(startup_heading);
    startup_layout->addSpacing(16);
    auto *startup_tagline = new QLabel(
        QStringLiteral("Awaken under Bodhi\nOne soul, thousand avatars"),
        startup_route);
    startup_tagline->setObjectName("lingtai_startup_tagline");
    startup_tagline->setAlignment(Qt::AlignCenter);
    auto tagline_font = startup_tagline->font();
    tagline_font.setPixelSize(15);
    tagline_font.setWeight(QFont::Normal);
    startup_tagline->setFont(tagline_font);
    auto tagline_palette = startup_tagline->palette();
    tagline_palette.setColor(QPalette::WindowText, st::windowFg->c);
    startup_tagline->setPalette(tagline_palette);
    startup_layout->addWidget(startup_tagline);
    startup_layout->addSpacing(30);
    auto *choose_project = new QPushButton(
        QStringLiteral("Choose project"), startup_route);
    choose_project->setObjectName("lingtai_startup_choose_project");
    choose_project->setAccessibleName(QStringLiteral("Choose project"));
    choose_project->setCursor(Qt::PointingHandCursor);
    choose_project->setFixedSize(236, 46);
    choose_project->setStyleSheet(QStringLiteral(
        "QPushButton { background: #1769e0; color: white; border: none; "
        "border-radius: 8px; font-size: 15px; font-weight: 600; } "
        "QPushButton:hover { background: #0d5fd6; } "
        "QPushButton:pressed { background: #0a51b8; }"));
    QObject::connect(choose_project, &QPushButton::clicked, [this] {
        request_open_project();
    });
    startup_layout->addWidget(choose_project, 0, Qt::AlignHCenter);
    startup_layout->addStretch(5);
    shell_layout->addWidget(startup_route, 1);

    // The persistent left 260px project/Agent list column: project identity
    // header, compact Open Project action, and the scrollable Agent
    // rows. The shell wires the owner's row clicks and its action buttons.
    agent_roster_ = new AgentRoster(body);
    agent_roster_->set_row_click_handler([this](const fs::path &key) {
        handle_agent_selection(key);
    });
    if (auto *open_button = agent_roster_->findChild<QPushButton *>(
            "lingtai_open_project_button")) {
        QObject::connect(open_button, &QPushButton::clicked, [this] {
            request_open_project();
        });
    }
    if (auto *open_new_window = agent_roster_->findChild<QPushButton *>(
            "lingtai_open_project_new_window_button")) {
        QObject::connect(open_new_window, &QPushButton::clicked, [this] {
            request_open_project_in_new_window();
        });
    }
    shell_layout->addWidget(agent_roster_);

    // One semantic drag handle between the roster and its shadow: a fixed 8px
    // strip that reports the pointer's real global x while the primary button
    // is held, so the shell re-derives the runtime-only roster width ratio. It
    // is distinct from the passive one-pixel shadow that immediately follows
    // it.
    auto *resize_handle = new RosterResizeHandle(body, kRosterResizeHandleWidth, [this, body](int global_x) {
        const auto body_width = body->width();
        const auto usable = body_width - kRosterResizeHandleWidth
            - kRosterSeparatorWidth;
        if (usable < kTwoColumnAvailableThreshold) return;
        const auto local_x = body->mapFromGlobal(QPoint(global_x, 0)).x();
        const auto chosen_px = std::clamp(local_x,
            kCollapsedRosterColumnWidth,
            body_width - kDetailColumnMinimumWidth
                - kRosterResizeHandleWidth - kRosterSeparatorWidth);
        roster_width_ratio_ = std::clamp(
            double(chosen_px) / double(body_width),
            double(kCollapsedRosterColumnWidth) / double(body_width),
            kMaximumRosterWidthRatio);
        recompute_layout(body_width);
    });
    roster_resize_handle_ = resize_handle;
    resize_handle->setObjectName("lingtai_roster_resize_handle");
    resize_handle->setAccessibleName(QStringLiteral("Resize Agent list"));
    resize_handle->setAccessibleDescription(QStringLiteral(
        "Drag to resize the Agent list"));
    shell_layout->addWidget(resize_handle);

    // Keep the semantic divider object for layout/state inspection, but the
    // accepted single canvas has no visible rule between the two white panes.
    auto *separator = new Ui::PlainShadow(body);
    separator_ = separator;
    separator->setObjectName("lingtai_roster_separator");
    separator->setAccessibleName(QStringLiteral("Project list divider"));
    separator->setFixedWidth(0);
    separator->hide();
    shell_layout->addWidget(separator);

    auto *content = new PaletteSurface(body, st::windowBg);
    content_ = content;
    content->setObjectName("lingtai_desktop_content");
    content->setAccessibleName(QStringLiteral("Workspace content"));
    auto palette = content->palette();
    palette.setColor(QPalette::Window, st::windowBg->c);
    content->setPalette(palette);
    content->setAutoFillBackground(true);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    shell_layout->addWidget(content, 1);

    auto *content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);
    auto *title = make_label(
        content,
        QStringLiteral("LingTai Desktop"),
        "lingtai_product_title",
        24,
        QFont::DemiBold);
    auto *purpose = make_label(
        content,
        QStringLiteral("A clear view of the project and Agents you choose."),
        "lingtai_product_purpose",
        12);
    purpose->setAccessibleDescription(QStringLiteral(
        "LingTai Desktop reads a selected project without changing it."));
    content_layout->addWidget(title);
    content_layout->addWidget(purpose);

    open_error_surface_ = new Ui::RpWidget(content);
    open_error_surface_->setObjectName("lingtai_project_open_error_surface");
    auto *error_layout = new QVBoxLayout(open_error_surface_);
    error_layout->setContentsMargins(0, 0, 0, 0);
    auto *open_error = make_flat_label(
        open_error_surface_,
        QString(),
        "lingtai_project_open_error");
    open_error->setAccessibleName(QStringLiteral("Project open error"));
    error_layout->addWidget(open_error);
    open_error_surface_->hide();
    content_layout->addWidget(open_error_surface_);

    // The one truthful New Project status surface, above both routes so a
    // pending phase, a failure, or a created-and-started success stays
    // visible regardless of which route is showing.
    bootstrap_status_surface_ = new Ui::RpWidget(content);
    bootstrap_status_surface_->setObjectName(
        "lingtai_bootstrap_status_surface");
    auto *bootstrap_status_layout = new QVBoxLayout(bootstrap_status_surface_);
    bootstrap_status_layout->setContentsMargins(0, 0, 0, 0);
    auto *bootstrap_status = make_flat_label(
        bootstrap_status_surface_,
        QString(),
        "lingtai_bootstrap_status");
    bootstrap_status->setAccessibleName(QStringLiteral("New project status"));
    bootstrap_status_layout->addWidget(bootstrap_status);
    bootstrap_status_surface_->hide();
    content_layout->addWidget(bootstrap_status_surface_);

    empty_route_ = new Ui::RpWidget(content);
    empty_route_->setObjectName("lingtai_empty_workspace_route");
    empty_route_->setAccessibleName(QStringLiteral("No project open"));
    empty_route_->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Expanding);
    content_layout->addWidget(empty_route_, 1);

    auto *empty_layout = new QVBoxLayout(empty_route_);
    empty_layout->setContentsMargins(24, 24, 24, 24);
    empty_layout->setSpacing(12);
    auto *empty_title = make_flat_label(
        empty_route_,
        QStringLiteral("No project open"),
        "lingtai_no_project_title");
    auto *empty_detail = make_flat_label(
        empty_route_,
        QStringLiteral("Open a LingTai project to inspect its Agents."),
        "lingtai_no_project_detail");
    // The branding rhythm between the no-project title/purpose and this empty
    // route belongs to the empty-route-only layout, so a selected project's
    // route never inherits a shared spacer above the active workspace.
    empty_layout->addSpacing(40);
    empty_layout->addStretch();
    empty_layout->addWidget(empty_title);
    empty_layout->addWidget(empty_detail);
    empty_layout->addStretch(2);

    setup_route_ = new ProjectSetupWizard(content);
    setup_route_->hide();
    content_layout->insertWidget(content_layout->indexOf(empty_route_), setup_route_, 1);

    project_route_ = new Ui::RpWidget(content);
    project_route_->setObjectName("lingtai_project_route");
    project_route_->setAccessibleName(QStringLiteral("Project"));
    project_route_->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Expanding);
    project_route_->hide();
    content_layout->addWidget(project_route_, 1);

    auto *project_layout = new QVBoxLayout(project_route_);
    project_layout->setContentsMargins(0, 0, 0, 0);
    project_layout->setSpacing(0);
    // The old full-width "Project" heading stays as a hidden semantic anchor:
    // a selected project's right pane is now one chat-first surface whose
    // identity lives in the top bar, not a dashboard heading.
    auto *project_heading = make_label(
        project_route_,
        QStringLiteral("Project"),
        "lingtai_project_route_heading",
        18,
        QFont::DemiBold);
    project_heading->hide();
    project_layout->addWidget(project_heading);
    auto *selection_error = make_label(
        project_route_, QString(), "lingtai_agent_selection_error", 11,
        QFont::Medium);
    selection_error->setAccessibleName(QStringLiteral("Agent selection error"));
    selection_error->hide();
    project_layout->addWidget(selection_error);

    auto *directory = new Ui::RpWidget(project_route_);
    directory->setObjectName("lingtai_agent_directory");
    directory->setAccessibleName(QStringLiteral("Agent directory"));
    directory->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    project_layout->addWidget(directory, 1);
    auto *directory_layout = new QHBoxLayout(directory);
    directory_layout->setContentsMargins(0, 0, 0, 0);

    // The detail column carries more evidence than any window is tall, so it
    // scrolls like the roster instead of overflowing and overpainting itself.
    auto *detail_scroll = new QScrollArea(directory);
    detail_scroll->setObjectName("lingtai_agent_detail_scroll");
    detail_scroll->setAccessibleName(QStringLiteral("Selected Agent detail"));
    detail_scroll->setWidgetResizable(true);
    detail_scroll->setFrameShape(QFrame::NoFrame);
    detail_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    directory_layout->addWidget(detail_scroll, 1);
    #if 0
    auto *detail = new Ui::RpWidget(detail_scroll);
    detail->setObjectName("lingtai_agent_detail");
    detail->setAccessibleName(QStringLiteral("Selected Agent detail"));
    detail->setMinimumWidth(0);
    detail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    detail_scroll->setWidget(detail);
    auto *detail_layout = new QVBoxLayout(detail);
    detail_layout->setContentsMargins(0, 0, 0, 0);
    detail_layout->setSpacing(4);
    // The old "Selected Agent" header heading stays as a hidden semantic
    // anchor: the chat top bar below now owns the selected-Agent identity.
    auto *detail_heading = make_label(
        detail, QStringLiteral("Selected Agent"),
        "lingtai_agent_detail_heading", 14, QFont::DemiBold);
    detail_heading->hide();
    detail_layout->addWidget(detail_heading);

    // One Telegram-like chat top bar: selected Agent identity and presence on
    // the left, and the compact Start/Sleep controls plus the narrow-mode
    // Back control on the right.
    auto *top_bar = new QWidget(detail);
    top_bar->setObjectName("lingtai_chat_top_bar");
    top_bar->setAccessibleName(QStringLiteral("Selected Agent"));
    top_bar->setMinimumWidth(0);
    top_bar->setFixedHeight(54);
    auto *top_bar_layout = new QHBoxLayout(top_bar);
    top_bar_layout->setContentsMargins(12, 8, 12, 8);
    top_bar_layout->setSpacing(8);
    auto *selected_avatar = new SelectedAgentAvatar(top_bar);
    selected_avatar->setObjectName("lingtai_selected_agent_avatar");
    selected_avatar->hide();
    top_bar_layout->addWidget(selected_avatar);
    auto *identity_column = new QVBoxLayout;
    identity_column->setContentsMargins(0, 0, 0, 0);
    identity_column->setSpacing(2);
    auto *presentation_name = make_label(
        top_bar, QString(), "lingtai_selected_agent_presentation_name", 16,
        QFont::DemiBold);
    presentation_name->setAccessibleName(
        QStringLiteral("Selected Agent presentation name"));
    identity_column->addWidget(presentation_name);
    auto *detail_key = make_label(
        top_bar, QString(), "lingtai_selected_agent_key", 12);
    detail_key->setAccessibleName(
        QStringLiteral("Selected Agent status and role"));
    detail_key->setWordWrap(false);
    detail_key->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    // The status line stays visually subordinate to the title: a smaller
    // point size above and a distinct muted ink drawn from the same
    // secondary-text token the page nav and disabled actions already use,
    // never the prominent title ink.
    auto key_palette = detail_key->palette();
    key_palette.setColor(QPalette::WindowText, st::windowSubTextFg->c);
    detail_key->setPalette(key_palette);
    identity_column->addWidget(detail_key);
    top_bar_layout->addLayout(identity_column, 1);
    // One compact palette-owned Back control in the chat top bar, visible only
    // in Telegram's narrow OneColumn detail view; it returns to the roster
    // through the same narrow-mode path Telegram's history-back uses.
    detail_back_button_ = new PaletteActionButton(
        top_bar, QStringLiteral("Back"));
    detail_back_button_->setObjectName("lingtai_agent_detail_back");
    detail_back_button_->setAccessibleName(QStringLiteral("Back to Agent list"));
    QObject::connect(detail_back_button_, &QPushButton::clicked, [this] {
        handle_detail_back();
    });
    top_bar_layout->addWidget(detail_back_button_);

    // The one Step-6 action on the exact selected Agent: an explicit,
    // nonblocking start for a selected non-human Agent whose current
    // projection is not heartbeat-live. Hidden entirely (not merely
    // disabled) for a live Agent, matching the product contract's "no
    // Start action" rather than Request sleep's always-visible/disabled
    // shape. The status label below shows only truthful, evidence-backed
    // claims -- spawn acceptance is never "online" on its own.
    auto *start_row = new Ui::RpWidget(top_bar);
    start_row->setObjectName("lingtai_selected_agent_start_row");
    start_row->setAccessibleName(QStringLiteral("Start Agent"));
    auto *start_row_layout = new QVBoxLayout(start_row);
    start_row_layout->setContentsMargins(0, 0, 0, 0);
    start_row_layout->setSpacing(0);
    auto *start_button = new PaletteActionButton(
        start_row, QStringLiteral("Start Agent"));
    start_button->setObjectName("lingtai_selected_agent_start_agent");
    start_button->setAccessibleName(QStringLiteral("Start Agent"));
    start_button->setAccessibleDescription(QStringLiteral(
        "Starts the selected Agent's own configured runtime as a detached "
        "local process. It does not provision, install, or repair a "
        "runtime, and never auto-starts any Agent on its own."));
    QObject::connect(start_button, &QPushButton::clicked, [this] {
        handle_start_agent();
    });
    start_row_layout->addWidget(start_button);
    auto *start_status = make_label(
        start_row, QString(), "lingtai_selected_agent_start_status", 10);
    start_status->setAccessibleName(QStringLiteral("Start Agent status"));
    start_status->setMaximumWidth(160);
    start_status->setMaximumHeight(12);
    start_status->setWordWrap(false);
    start_row_layout->addWidget(start_status);
    // Reserve the action region's height from the row's own layout so the
    // chat surface below never jumps when the Start button is hidden for a
    // heartbeat-live Agent; visibility/enablement still track eligibility
    // exactly, only the button is ever absent.
    start_row->setMinimumHeight(start_row->sizeHint().height());
    start_button->setVisible(false);
    start_row->hide();
    top_bar_layout->addWidget(start_row);

    // The one Step-5 action on the exact selected Agent: reproduces only the
    // canonical empty `.sleep` marker write plus a best-effort target-side
    // observation, as a subtle compact icon-only secondary -- never a second
    // full-caption action button -- whose accessible name preserves the
    // "Request sleep" identity. Disabled while ineligible or while a
    // just-clicked observation is still pending; the status label below shows
    // only truthful, evidence-backed claims, never a lifecycle status inferred
    // from the write or a timeout alone.
    auto *sleep_row = new Ui::RpWidget(top_bar);
    sleep_row->setObjectName("lingtai_selected_agent_sleep_row");
    sleep_row->setAccessibleName(QStringLiteral("Request sleep"));
    auto *sleep_row_layout = new QVBoxLayout(sleep_row);
    sleep_row_layout->setContentsMargins(0, 0, 0, 0);
    sleep_row_layout->setSpacing(0);
    auto *sleep_button = new PaletteIconButton(sleep_row);
    sleep_button->setObjectName("lingtai_selected_agent_request_sleep");
    sleep_button->setAccessibleName(QStringLiteral("Request sleep"));
    sleep_button->setAccessibleDescription(QStringLiteral(
        "Writes one empty local sleep-request marker for the selected "
        "Agent. It does not queue, cancel, suspend, or restart anything."));
    sleep_button->setEnabled(false);
    QObject::connect(sleep_button, &QPushButton::clicked, [this] {
        handle_request_sleep();
    });
    sleep_row_layout->addWidget(sleep_button);
    auto *sleep_status = make_label(
        sleep_row, QString(), "lingtai_selected_agent_sleep_status", 10);
    sleep_status->setAccessibleName(QStringLiteral("Sleep request status"));
    sleep_status->setMaximumWidth(160);
    sleep_status->setMaximumHeight(12);
    sleep_status->setWordWrap(false);
    sleep_row_layout->addWidget(sleep_status);
    sleep_row->hide();
    top_bar_layout->addWidget(sleep_row);
    detail_layout->addWidget(top_bar);
    // Retained once for the whole shell lifetime so the responsive fit measure
    // in `recompute_layout` can evaluate the full natural row against the
    // actual detail width without re-deriving these two anchors.
    chat_top_bar_ = top_bar;
    selected_agent_key_ = detail_key;

    // Conversation is the only visible selected-Agent page tab. Presets
    // remains a slash destination (`/presets`) behind a hidden nav button so
    // the chat chrome no longer shows a Presets tab. On `/presets` the
    // Conversation control becomes a Kanban-style `←  Conversation` back link.
    auto *pages_nav = new PaletteSurface(detail, st::windowBg);
    pages_nav->setObjectName("lingtai_agent_pages_nav");
    pages_nav->setAccessibleName(QStringLiteral("Selected Agent pages"));
    auto *pages_nav_layout = new QHBoxLayout(pages_nav);
    pages_nav_layout->setContentsMargins(12, 8, 12, 4);
    pages_nav_layout->setSpacing(4);
    const auto nav_specs = std::array<std::pair<const char *, const char *>, 2>{{
        std::pair{"lingtai_agent_page_nav_conversation", "←  Conversation"},
        std::pair{"lingtai_agent_page_nav_presets", "Presets"},
    }};
    for (auto index = std::size_t{0}; index != nav_specs.size(); ++index) {
        const auto &[object_name, text] = nav_specs[index];
        const auto page = static_cast<AgentDetailPage>(index);
        auto *button = new PageNavButton(
            pages_nav, QString::fromUtf8(text));
        button->setObjectName(object_name);
        button->setAccessibleName(
            page == AgentDetailPage::conversation
                ? QStringLiteral("Conversation")
                : QString::fromUtf8(text));
        QObject::connect(button, &QPushButton::clicked, [this, page] {
            show_detail_page(page);
        });
        if (page == AgentDetailPage::presets) {
            button->hide();
        } else {
            pages_nav_layout->addWidget(button, 0);
        }
        page_nav_buttons_.push_back(button);
    }
    pages_nav_layout->addStretch(1);
    detail_layout->addWidget(pages_nav);

    // The conversation is the product, so it is the default page surface,
    // directly under the page navigation rather than below a stack of source
    // cards. Its duplicate "Conversation" heading is retained only as a hidden
    // object/implementation anchor: the nav item owns the user affordance.
    detail_layout->addWidget(make_label(
        detail, QStringLiteral("Conversation"),
        "lingtai_selected_agent_conversation_heading", 11, QFont::DemiBold));
    auto *conversation = new ConversationSurface(detail);
    conversation->setObjectName("lingtai_selected_agent_conversation");
    conversation->setAccessibleName(
        QStringLiteral("Selected Agent conversation"));
    conversation->setAccessibleDescription(QStringLiteral(
        "The current direct conversation with the selected Agent, shown "
        "read-only as plain text."));
    conversation->setMinimumHeight(180);
    conversation->setMinimumWidth(0);
    conversation->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    // The surface's own paintEvent owns the visible backdrop and bubbles; its
    // Base/Window palette roles stay transparent so the surface renders the
    // shell's palette background rather than a widget-level white base.
    detail_layout->addWidget(conversation, 1);

    // The outer Composer lane stays on the same base as the conversation and
    // owns status read-outs. Its first child is the one inset rounded controls
    // envelope; no attachment/input/Send control paints an independent frame.
    auto *composer = new PaletteSurface(detail, st::windowBg);
    composer_ = composer;
    composer->setObjectName("lingtai_composer");
    composer->setAccessibleName(QStringLiteral("Send a message"));
    auto *composer_layout = new QVBoxLayout(composer);
    composer_layout->setContentsMargins(0, 0, 0, 0);
    composer_layout->setSpacing(4);
    auto *composer_controls = new ComposerControls(composer);
    composer_controls->setObjectName("lingtai_composer_controls");
    composer_controls->setAccessibleName(QStringLiteral("Message controls"));
    composer_controls->setFixedHeight(52);
    auto *composer_action_row = new QHBoxLayout(composer_controls);
    composer_action_row->setContentsMargins(6, 4, 6, 4);
    composer_action_row->setSpacing(4);
    // A borderless copy of the shared single-line field style: the row's own
    // base `windowBg` surface is the only frame, and the field keeps the same
    // text/placeholder face as the standard control.
    static const auto borderless_composer_input = [] {
        auto result = st::defaultInputField;
        result.border = 0;
        result.borderActive = 0;
        // defaultInputField is a 55px floating-label control with a 28px top
        // inset. This Composer owns a fixed 40px single-line lane instead.
        result.textMargins = QMargins(0, 13, 0, 0);
        result.placeholderMargins = QMargins();
        result.placeholderScale = 0.0;
        result.placeholderShift = 0;
        return result;
    }();
    auto *composer_input = new Ui::InputField(
        composer_controls,
        borderless_composer_input,
        Ui::InputField::Mode::SingleLine,
        rpl::single(QStringLiteral("Message…")));
    composer_input->setObjectName("lingtai_composer_input");
    composer_input->setAccessibleName(QStringLiteral("Message"));
    composer_input->setFixedHeight(40);
    composer_input->setEnabled(false);
    auto *attachment_button = new ComposerAttachmentButton(composer_controls);
    attachment_button->setObjectName("lingtai_composer_attachment_button");
    attachment_button->setAccessibleName(QStringLiteral("Attach file"));
    attachment_button->setEnabled(false);
    composer_action_row->addWidget(attachment_button, 0, Qt::AlignVCenter);
    composer_action_row->addWidget(composer_input, 1, Qt::AlignVCenter);
    static const auto composer_send_style = [] {
        auto result = st::defaultActiveButton;
        result.height = 40;
        result.radius = 20;
        result.padding = QMargins(2, 2, 2, 2);
        return result;
    }();
    auto *send_button = new ComposerSendButton(
        composer_controls,
        composer_send_style);
    send_button->setObjectName("lingtai_composer_send_button");
    send_button->setAccessibleName(QStringLiteral("Send message"));
    send_button->setEnabled(false);
    send_button->setFixedSize(44, 44);
    send_button->setFullRadius(true);
    send_button->addClickHandler([this] {
        handle_send_message();
    });
    composer_action_row->addWidget(send_button, 0, Qt::AlignVCenter);
    composer_layout->addWidget(composer_controls);
    auto *slash_card = new SlashCommandCard(window_->body().get());
    auto *slash_popup = slash_card->list();
    QObject::connect(slash_popup, &QListWidget::itemClicked,
        [composer_input, slash_popup](QListWidgetItem *) {
            apply_slash_popup_choice(composer_input, slash_popup);
        });
    QObject::connect(slash_popup, &QListWidget::itemEntered,
        [slash_popup](QListWidgetItem *item) {
            slash_popup->setCurrentItem(item);
        });
    // The send status is owned by the lane itself, immediately below the
    // action row -- never a separate detail row.
    auto *composer_status = make_label(
        composer, QString(), "lingtai_composer_status", 10);
    composer_status->setAccessibleName(QStringLiteral("Send status"));
    composer_layout->addWidget(composer_status);
    // The conversation's own compact read-out shares the same lane, directly
    // under the send status: it never becomes a separate detail row.
    auto *conversation_state = make_label(
        composer, QString(), "lingtai_selected_agent_conversation_state", 10);
    conversation_state->setAccessibleName(
        QStringLiteral("Selected Agent conversation state"));
    composer_layout->addWidget(conversation_state);
    auto *composer_surface = new QHBoxLayout;
    composer_surface->setContentsMargins(16, 0, 16, 12);
    composer_surface->addWidget(composer);
    detail_layout->addLayout(composer_surface);
    composer_input->submits()
        | rpl::on_next([this, composer_input] {
            if (auto *popup = window_->findChild<QListWidget *>(
                    "lingtai_slash_command_popup");
                    popup && popup->isVisible() && popup->currentItem()) {
                apply_slash_popup_choice(composer_input, popup);
                return;
            }
            handle_send_message();
        }, submits_lifetime_);
    composer_input->changes()
        | rpl::on_next([this, composer_input] {
            const auto text = composer_input->getLastText();
            composer_input->setPlaceholder(rpl::single(
                text.isEmpty() ? QStringLiteral("Message…") : QString()));
            refresh_slash_command_popup(window_.get(), composer_input);
        }, submits_lifetime_);
    base::install_event_filter(
        composer_input->rawTextEdit(),
        composer_input->rawTextEdit(),
        [this, composer_input](not_null<QEvent *> event) {
            if (event->type() != QEvent::KeyPress) {
                return base::EventFilterResult::Continue;
            }
            auto *popup = window_->findChild<QListWidget *>(
                "lingtai_slash_command_popup");
            if (!popup || !popup->isVisible()) {
                return base::EventFilterResult::Continue;
            }
            const auto *key = static_cast<QKeyEvent *>(event.get());
            if (key->key() == Qt::Key_Up) {
                popup->setCurrentRow(std::max(0, popup->currentRow() - 1));
                return base::EventFilterResult::Cancel;
            }
            if (key->key() == Qt::Key_Down) {
                popup->setCurrentRow(
                    std::min(popup->count() - 1, popup->currentRow() + 1));
                return base::EventFilterResult::Cancel;
            }
            if (key->key() == Qt::Key_Escape) {
                popup->hide();
                return base::EventFilterResult::Cancel;
            }
            if (key->key() == Qt::Key_Tab) {
                apply_slash_popup_choice(composer_input, popup);
                return base::EventFilterResult::Cancel;
            }
            return base::EventFilterResult::Continue;
        });

    // The one retained bounded read-only selected-Agent source section is
    // presented through the same local structural framing: one semibold
    // heading, one read-only plain-text surface, one state line, and one thin
    // plain-shadow separator. It remains a distinct source and authority and
    // is never merged with the mailbox conversation; it moves behind the
    // compact secondary page host so it never stacks under the chat surface.
    auto *pages_host = new PaletteSurface(detail, st::windowBg);
    pages_host->setObjectName("lingtai_agent_pages_host");
    pages_host->setAccessibleName(
        QStringLiteral("Selected Agent secondary pages"));
    pages_host->setMinimumWidth(0);
    pages_host->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto *pages_host_layout = new QVBoxLayout(pages_host);
    pages_host_layout->setContentsMargins(0, 0, 0, 0);
    pages_host_layout->setSpacing(8);
    secondary_pages_.push_back(add_dashboard_section(
        pages_host, pages_host_layout, "preset_summary",
        QStringLiteral("Presets"),
        QStringLiteral("Selected Agent Presets"),
        QStringLiteral("The selected Agent's allowed presets, shown with the "
            "same catalog as project setup.")).owner);
    apply_preset_catalog_chrome(pages_host);
    pages_host->hide();
    detail_layout->addWidget(pages_host, 1);

    kanban_page_ = new KanbanPage(pages_host);
    kanban_page_->setMinimumWidth(0);
    kanban_page_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    pages_host_layout->addWidget(kanban_page_, 1);
    QObject::connect(kanban_page_, &KanbanPage::agent_selected,
        [this](const QString &key) {
            handle_kanban_agent_selected(
                std::filesystem::path(key.toStdString()));
        });
    QObject::connect(kanban_page_, &KanbanPage::presets_requested, [this] {
        show_detail_page(AgentDetailPage::presets);
    });
    QObject::connect(kanban_page_, &KanbanPage::back_requested, [this] {
        show_detail_page(AgentDetailPage::conversation);
    });
    QObject::connect(kanban_page_, &KanbanPage::reload_requested, [this] {
        if (selection_state_.active_project()) {
            agents_ = project_agents(*selection_state_.active_project());
        }
        render_kanban();
    });
    secondary_pages_.push_back(kanban_page_);

    auto *manifest_identity = make_label(
        detail, QString(), "lingtai_selected_agent_manifest_identity", 11);
    manifest_identity->setAccessibleName(QStringLiteral("Manifest identity"));
    detail_layout->addWidget(manifest_identity);
    auto *manifest_llm = make_label(
        detail, QString(), "lingtai_selected_agent_manifest_llm", 11);
    manifest_llm->setAccessibleName(QStringLiteral("Manifest live LLM"));
    detail_layout->addWidget(manifest_llm);
    auto *manifest_capabilities = make_label(
        detail, QString(), "lingtai_selected_agent_manifest_capabilities", 11);
    manifest_capabilities->setAccessibleName(
        QStringLiteral("Manifest capabilities"));
    detail_layout->addWidget(manifest_capabilities);
    auto *status_activity = make_label(
        detail, QString(), "lingtai_selected_agent_status_activity", 11);
    status_activity->setAccessibleName(QStringLiteral("Status activity"));
    detail_layout->addWidget(status_activity);
    auto *status_context = make_label(
        detail, QString(), "lingtai_selected_agent_status_context", 11);
    status_context->setAccessibleName(QStringLiteral("Status context"));
    detail_layout->addWidget(status_context);
    auto *detail_facts = make_label(
        detail, QString(), "lingtai_selected_agent_facts", 11);
    detail_facts->setAccessibleName(QStringLiteral("Selected Agent facts"));
    detail_layout->addWidget(detail_facts);
    detail_layout->addStretch();

    // The source-facts labels below the page host are read-only detail
    // surfaces, not chat content: they stay present as anchors but are never
    // revealed on the Presets page, so the chat-first surface stays clean.
    for (const auto *facts_name : {
            "lingtai_selected_agent_manifest_identity",
            "lingtai_selected_agent_manifest_llm",
            "lingtai_selected_agent_manifest_capabilities",
            "lingtai_selected_agent_status_activity",
            "lingtai_selected_agent_status_context",
            "lingtai_selected_agent_facts" }) {
        if (auto *label = window_->findChild<QLabel *>(facts_name)) {
            label->hide();
        }
    }

    // The chat is the default selected-Agent page; the page navigation and
    // secondary surfaces start in that exact state.
    show_detail_page(AgentDetailPage::conversation);

    #endif

    auto *detail = new AgentDetailView(runtime_options_, detail_scroll);
    detail_scroll->setWidget(detail);
    detail_view_ = detail;

    // Re-derive stable pointers/vectors until NativeShell delegates fully
    // to AgentDetailView in follow-up plan steps.
    detail_back_button_ = detail->findChild<QPushButton *>(
        "lingtai_agent_detail_back");
    chat_top_bar_ = detail->findChild<QWidget *>("lingtai_chat_top_bar");
    selected_agent_key_ = detail->findChild<QLabel *>(
        "lingtai_selected_agent_key");
    composer_ = detail->findChild<Ui::RpWidget *>("lingtai_composer");

    page_nav_buttons_.clear();
    secondary_pages_.clear();
    if (auto *nav_conversation = detail->findChild<QPushButton *>(
            "lingtai_agent_page_nav_conversation")) {
        page_nav_buttons_.push_back(nav_conversation);
    }
    if (auto *nav_presets = detail->findChild<QPushButton *>(
            "lingtai_agent_page_nav_presets")) {
        page_nav_buttons_.push_back(nav_presets);
    }

    if (auto *preset_section = detail->findChild<QWidget *>(
            "lingtai_selected_agent_preset_summary_section")) {
        secondary_pages_.push_back(preset_section);
    }

    kanban_page_ = detail->findChild<KanbanPage *>();
    if (kanban_page_) secondary_pages_.push_back(kanban_page_);

    // Delegate all user actions + page transitions back into the shell,
    // keeping business logic and data reads here while the view owns the
    // widget tree and page visibility.
    QObject::connect(detail_view_, &AgentDetailView::back_requested,
        [this] { handle_detail_back(); });
    QObject::connect(detail_view_, &AgentDetailView::start_requested,
        [this] { handle_start_agent(); });
    QObject::connect(detail_view_, &AgentDetailView::sleep_requested,
        [this] { handle_request_sleep(); });
    QObject::connect(detail_view_, &AgentDetailView::send_message_requested,
        [this](const QString &) { handle_send_message(); });
    QObject::connect(detail_view_, &AgentDetailView::kanban_agent_selected,
        [this](const fs::path &directory_key) {
            handle_kanban_agent_selected(directory_key);
        });
    QObject::connect(detail_view_, &AgentDetailView::page_changed,
        [this](AgentDetailPage previous, AgentDetailPage current) {
            current_detail_page_ = current;
            if (current == AgentDetailPage::kanban) {
                render_kanban();
            }
            if (current == AgentDetailPage::presets) {
                render_agent_preset_summary();
            }
            if (previous != current && selection_state_.active_project()
                && window_) {
                recompute_layout(window_->body()->width());
            }
        });

    show_detail_page(AgentDetailPage::conversation);

    // One simple view-scoped timer: it re-invokes the same stateless
    // snapshot reader every second so a same-selection append becomes
    // visible without reselection. It is behavior, not a watcher subsystem:
    // no background thread, debouncing, or persisted state.
    activity_timer_ = new QTimer(body);
    activity_timer_->setInterval(1000);
    QObject::connect(activity_timer_, &QTimer::timeout, [this] {
        render_conversation();
        render_agent_preset_summary();
        if (pending_sleep_observation_) {
            tick_agent_sleep_observation();
        } else if (pending_start_observation_) {
            tick_agent_start_observation();
        } else if (selection_state_.active_project()) {
            // No click-armed observation is pending. Rerun the same stateless
            // projection the shell already uses at every click/settle
            // boundary so manifest lifecycle state and heartbeat-derived
            // eligibility stay current without reselection.
            agents_ = project_agents(*selection_state_.active_project());
            render_roster();
        }
        // Kanban is a full disk snapshot (token ledgers, sqlite, daemon runs)
        // plus a widget-tree rebuild. The 1s conversation timer must not
        // refresh it: Reload and entering /kanban already do. Live refresh
        // here stalls scrolling on large agents.
    });
    if (!runtime_options_.ui_test_mode) {
        activity_timer_->start();
    }

    bootstrap_runner_ = std::make_unique<ProjectBootstrapRunner>();

    // New-folder setup is one workspace-sized route inside the main content
    // pane. It reuses the canonical headless preset discovery and spawn owner
    // below; the pages own only the human decisions reviewed before spawn.
    apply_project_setup_palette(setup_route_);
    setup_route_->setStyleSheet(QStringLiteral(
        "QWidget#lingtai_project_setup_wizard { background: transparent; } "
        "QPushButton { border-radius: 6px; padding: 0 16px; } "
        "QPushButton#lingtai_setup_preset_continue, "
        "QPushButton#lingtai_setup_edit_preset_save, "
        "QPushButton#lingtai_setup_agents_continue, "
        "QPushButton#lingtai_bootstrap_create_start { "
        "min-height: 34px; background: #16785C; color: white; border: none; font-weight: 600; }"));
    auto *wizard_layout = new QVBoxLayout(setup_route_);
    wizard_layout->setContentsMargins(0, 0, 0, 0);
    wizard_layout->setSpacing(0);

    // The native window title already says LingTai. Own the flow with one
    // horizontal Preset / Agents / Review sequence, not a second brand.
    auto *steps = new QWidget(setup_route_);
    steps->setObjectName("lingtai_setup_steps");
    steps->setFixedHeight(48);
    auto *steps_layout = new QHBoxLayout(steps);
    steps_layout->setContentsMargins(32, 8, 32, 0);
    steps_layout->setSpacing(12);
    steps_layout->addStretch();
    const auto make_step = [&](const QString &number, const QString &text,
            const char *name, const char *badge_name) {
        auto *step = new QWidget(steps);
        auto *layout = new QHBoxLayout(step);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(7);
        auto *badge = make_setup_label(step, number, badge_name, 11,
            QFont::DemiBold);
        badge->setAlignment(Qt::AlignCenter);
        badge->setFixedSize(22, 22);
        badge->setStyleSheet(QStringLiteral(
            "background: %1; color: white; border-radius: 11px;")
            .arg(setup_color_css(setup_tokens(step->palette()).selection_accent)));
        auto *label = make_setup_label(step, text, name, 12, QFont::DemiBold);
        label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        label->setStyleSheet(QStringLiteral("color: %1;")
            .arg(setup_color_css(setup_tokens(step->palette()).selection_accent)));
        layout->addWidget(badge);
        layout->addWidget(label);
        steps_layout->addWidget(step);
    };
    const auto add_connector = [&] {
        auto *connector = new QLabel(QStringLiteral("────────"), steps);
        connector->setFixedWidth(36);
        connector->setStyleSheet(QStringLiteral("color: palette(mid);"));
        connector->setAlignment(Qt::AlignCenter);
        steps_layout->addWidget(connector);
    };
    make_step(QStringLiteral("1"), QStringLiteral("Preset"),
        "lingtai_setup_step_preset", "lingtai_setup_step_badge_preset");
    add_connector();
    make_step(QStringLiteral("2"), QStringLiteral("Agents"),
        "lingtai_setup_step_agents", "lingtai_setup_step_badge_agents");
    add_connector();
    make_step(QStringLiteral("3"), QStringLiteral("Review"),
        "lingtai_setup_step_review", "lingtai_setup_step_badge_review");
    steps_layout->addStretch();
    auto *step_index = make_setup_label(steps, QStringLiteral("1 of 3"),
        "lingtai_setup_step_index", 12);
    step_index->setStyleSheet(QStringLiteral("color: #8a8f98;"));
    steps_layout->addWidget(step_index);
    wizard_layout->addWidget(steps);

    auto *right = new QWidget(setup_route_);
    right->setObjectName("lingtai_setup_body");
    auto *right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(32, 4, 32, 16);
    right_layout->setSpacing(0);
    auto *pages = new QStackedWidget(right);
    pages->setObjectName("lingtai_setup_pages");
    pages->setMinimumWidth(0);
    pages->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

    const auto make_heading = [](QWidget *page, QVBoxLayout *layout,
            const QString &title, const QString &subtitle, const char *name) {
        auto *heading = make_setup_label(page, title, name, 20, QFont::DemiBold);
        heading->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        heading->setStyleSheet(QStringLiteral("color: palette(window-text);"));
        layout->addWidget(heading);
        layout->addSpacing(6);
        auto *note = make_setup_label(page, subtitle, "lingtai_setup_page_note", 13);
        note->setWordWrap(true);
        note->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        note->setStyleSheet(QStringLiteral("color: palette(mid);"));
        layout->addWidget(note);
        layout->addSpacing(8);
    };

    auto *preset_page = new QWidget(pages);
    preset_page->setObjectName("lingtai_setup_preset_page");
    preset_page->setMinimumWidth(0);
    preset_page->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto *preset_layout = new QVBoxLayout(preset_page);
    preset_layout->setContentsMargins(0, 0, 0, 0);
    preset_layout->setSpacing(10);
    make_heading(preset_page, preset_layout,
        QStringLiteral("Choose how your orchestrator runs"),
        QStringLiteral("Select a saved preset or configure a template."),
        "lingtai_setup_preset_heading");
    auto *preset_search = new QLineEdit(preset_page);
    preset_search->setObjectName("lingtai_setup_preset_search");
    preset_search->setPlaceholderText(QStringLiteral("Search presets"));
    preset_search->setClearButtonEnabled(true);
    preset_search->setFixedHeight(34);
    preset_search->setMinimumWidth(kPresetSearchMinWidth);
    preset_search->setMaximumWidth(kPresetSearchMaxWidth);
    preset_search->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    preset_layout->addWidget(preset_search, 0, Qt::AlignLeft);

    auto *preset_catalog = new QTreeWidget(preset_page);
    preset_catalog->setObjectName("lingtai_setup_preset_catalog");
    preset_catalog->setAccessibleName(QStringLiteral("Presets"));
    preset_catalog->setColumnCount(3);
    preset_catalog->setHeaderLabels({ QStringLiteral("Preset"),
        QStringLiteral("Provider / model"), QStringLiteral("Capabilities") });
    configure_preset_table(preset_catalog);
    preset_catalog->setMinimumHeight(0);
    preset_catalog->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Expanding);
    add_preset_section(preset_catalog, QStringLiteral("Saved presets"));
    add_preset_section(preset_catalog, QStringLiteral("Preset templates"));
    preset_layout->addWidget(preset_catalog, 1);

    auto *codex_strip = new CodexCredentialsStrip(preset_page);
    preset_layout->addWidget(codex_strip, 0);

    // The existing spawn owner still consumes one canonical selected preset.
    // Keep that state in a hidden chooser; browsing belongs to the one catalog
    // table, never to a dropdown.
    auto *preset_chooser = new QComboBox(preset_page);
    preset_chooser->setObjectName("lingtai_bootstrap_preset_chooser");
    preset_chooser->setAccessibleName(QStringLiteral("Selected preset"));
    preset_chooser->hide();

    auto *preset_actions = new QHBoxLayout;
    auto *preset_back = new QPushButton(QStringLiteral("Back"), preset_page);
    preset_back->setObjectName("lingtai_setup_preset_back");
    preset_back->setCursor(Qt::PointingHandCursor);
    preset_back->setEnabled(true);
    auto *preset_footer_summary = make_setup_label(preset_page, QString(),
        "lingtai_setup_preset_footer_summary", 12);
    preset_footer_summary->setTextFormat(Qt::RichText);
    preset_footer_summary->setAlignment(Qt::AlignCenter);
    preset_footer_summary->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *preset_continue = new QPushButton(QStringLiteral("Use preset"), preset_page);
    preset_continue->setObjectName("lingtai_setup_preset_continue");
    preset_continue->setStyleSheet(QStringLiteral(
        "background: #16785C; color: white; border: none; font-weight: 600;"));
    preset_actions->addWidget(preset_back);
    preset_actions->addWidget(preset_footer_summary, 1);
    preset_actions->addWidget(preset_continue);
    preset_layout->addLayout(preset_actions, 0);
    apply_preset_catalog_chrome(setup_route_);
    pages->addWidget(preset_page);

    auto *edit_preset_page = new PresetEditorPage(pages);
    pages->addWidget(edit_preset_page);

    auto *agents_page = new AgentPresetsPage(pages);
    pages->addWidget(agents_page);

    auto *review_page = new AgentConfigPage(pages);
    auto *dialog_status = make_flat_label(review_page, QString(),
        "lingtai_bootstrap_dialog_status");
    dialog_status->setAccessibleName(QStringLiteral("Project setup status"));
    review_page->install_dialog_status(dialog_status);
    pages->addWidget(review_page);

    auto *credentials_page = new CredentialsPage(pages);
    pages->addWidget(credentials_page);

    auto *destination_input = new Ui::InputField(setup_route_,
        st::defaultInputField, Ui::InputField::Mode::SingleLine,
        rpl::single(QStringLiteral("Project folder")));
    destination_input->setObjectName("lingtai_bootstrap_destination_input");
    destination_input->setAccessibleName(QStringLiteral("Project folder"));
    destination_input->hide();
    auto *browse_button = new QPushButton(QStringLiteral("Browse…"),
        setup_route_);
    browse_button->setObjectName("lingtai_bootstrap_destination_browse");
    browse_button->setAccessibleName(QStringLiteral("Browse destination folder"));
    browse_button->hide();
    auto *cancel_button = new QPushButton(QStringLiteral("Cancel"),
        setup_route_);
    cancel_button->setObjectName("lingtai_bootstrap_cancel");
    cancel_button->hide();
    right_layout->addWidget(pages, 1);
    wizard_layout->addWidget(right, 1);

    const auto sync_catalog_selection = [preset_chooser](QTreeWidgetItem *item) {
        if (!item || is_preset_section(item)) return;
        preset_chooser->setCurrentIndex(item->data(0, Qt::UserRole).toInt());
    };
    QObject::connect(preset_search, &QLineEdit::textChanged,
        [preset_catalog](const QString &query) {
            const auto needle = query.trimmed();
            QTreeWidgetItem *section = nullptr;
            auto section_has_visible = false;
            const auto close_section = [&] {
                if (!section) return;
                const auto title_match = !needle.isEmpty()
                    && section->text(0).contains(needle, Qt::CaseInsensitive);
                section->setHidden(!needle.isEmpty()
                    && !section_has_visible && !title_match);
            };
            for (auto index = 0; index != preset_catalog->topLevelItemCount();
                    ++index) {
                auto *item = preset_catalog->topLevelItem(index);
                if (is_preset_section(item)) {
                    close_section();
                    section = item;
                    section_has_visible = false;
                    continue;
                }
                const auto haystack = item->text(0) + QStringLiteral(" ")
                    + item->text(1) + QStringLiteral(" ") + item->text(2)
                    + QStringLiteral(" ") + item->toolTip(0);
                const auto match = needle.isEmpty()
                    || haystack.contains(needle, Qt::CaseInsensitive);
                item->setHidden(!match);
                section_has_visible = section_has_visible || match;
            }
            close_section();
        });

    const auto update_review = [preset_chooser, preset_footer_summary,
            preset_continue, dialog = setup_route_] {
        const auto preset = preset_chooser->currentText().trimmed();
        const auto summary = preset_chooser->currentData(Qt::UserRole).toString();
        const auto provider = preset_chooser->currentData(Qt::UserRole + 2).toString();
        const auto model = preset_chooser->currentData(Qt::UserRole + 3).toString();
        const auto is_template = preset_chooser->currentData(Qt::UserRole + 4).toBool();
        const auto has_vision = preset_chooser->currentData(Qt::UserRole + 5).toBool();
        const auto has_tools = preset_chooser->currentData(Qt::UserRole + 6).toBool();
        const auto provider_model = provider.isEmpty() && model.isEmpty()
            ? summary
            : (provider.isEmpty() ? model : (model.isEmpty() ? provider
                : provider + QStringLiteral(" · ") + model));
        const auto tags = preset_capability_tags(has_vision, has_tools);
        const auto tokens = preset_catalog_tokens(dialog->palette());
        preset_footer_summary->setText(
            preset_footer_rich(preset, provider_model, tags,
                tokens.selection_accent, tokens.section_text));
        preset_footer_summary->setAccessibleName(
            preset_footer_plain(preset, provider_model, tags));
        preset_continue->setText(is_template
            ? QStringLiteral("Configure preset")
            : QStringLiteral("Use preset"));
        preset_continue->setEnabled(!preset.isEmpty());
    };
    const auto refresh_preset_selection = [update_review, sync_catalog_selection](
            QTreeWidgetItem *item) {
        sync_catalog_selection(item);
        update_review();
    };
    QObject::connect(preset_catalog, &QTreeWidget::currentItemChanged,
        [preset_catalog, refresh_preset_selection](
                QTreeWidgetItem *current, QTreeWidgetItem *previous) {
            if (!current) return;
            if (is_preset_section(current)) {
                const auto current_index =
                    preset_catalog->indexOfTopLevelItem(current);
                const auto previous_index = previous
                    ? preset_catalog->indexOfTopLevelItem(previous)
                    : current_index - 1;
                const auto step = current_index >= previous_index ? 1 : -1;
                if (auto *next = adjacent_preset_row(
                        preset_catalog, current_index, step)) {
                    preset_catalog->setCurrentItem(next);
                } else if (auto *back = adjacent_preset_row(
                        preset_catalog, current_index, -step)) {
                    preset_catalog->setCurrentItem(back);
                }
                return;
            }
            refresh_preset_selection(current);
        });
    const auto go_to_page = [pages, steps = steps, credentials_page](int index) {
        pages->setCurrentIndex(index);
        const auto credentials = pages->indexOf(credentials_page);
        const auto step = index <= 1 || index == credentials ? 0 : index - 1;
        update_setup_step_indicator(steps, step);
        steps->setVisible(index != 1 && index != credentials);
    };
    auto credentials_return = std::make_shared<int>(0);
    const auto open_credentials = [pages, credentials_page, credentials_return,
            go_to_page, edit_preset_page] {
        *credentials_return = pages->currentIndex();
        if (*credentials_return == pages->indexOf(credentials_page)) return;
        credentials_page->reload();
        credentials_page->set_back_label(*credentials_return == pages->indexOf(edit_preset_page)
            ? QStringLiteral("← Edit preset")
            : QStringLiteral("← Presets"));
        go_to_page(pages->indexOf(credentials_page));
    };
    const auto open_preset_editor = [edit_preset_page, preset_chooser, go_to_page,
            this] {
        const auto index = preset_chooser->currentIndex();
        if (index < 0) return;
        auto existing = QStringList();
        for (auto item = 0; item != preset_chooser->count(); ++item) {
            existing.push_back(preset_chooser->itemText(item));
        }
        edit_preset_page->load(PresetEditorLoadRequest{
            preset_chooser->itemData(index, Qt::UserRole + 7).toString(),
            preset_chooser->itemText(index),
            preset_chooser->itemData(index, Qt::UserRole).toString(),
            preset_chooser->itemData(index, Qt::UserRole + 1).toString(),
            preset_chooser->itemData(index, Qt::UserRole + 4).toBool(),
            existing,
        });
        if (!edit_preset_page->model().codex_auth_allows_editor()) {
            const auto family = credential_family(
                edit_preset_page->model().provider());
            set_bootstrap_status(family == QLatin1String("codex_pool")
                ? QStringLiteral(
                    "No eligible Codex pool account is available for this model.")
                : QStringLiteral(
                    "Codex login required — sign in from LingTai TUI first."));
            return;
        }
        go_to_page(1);
    };
    QObject::connect(preset_back, &QPushButton::clicked, [this] {
        handle_cancel_bootstrap();
    });
    QObject::connect(preset_chooser, &QComboBox::currentTextChanged,
        [update_review](const QString &) { update_review(); });
    QObject::connect(preset_continue, &QPushButton::clicked,
        [open_preset_editor] { open_preset_editor(); });
    QObject::connect(edit_preset_page, &PresetEditorPage::cancelled,
        [go_to_page] { go_to_page(0); });
    QObject::connect(edit_preset_page, &PresetEditorPage::saved,
        [preset_chooser, agents_page, go_to_page, update_review](
                const QString &saved_name) {
            const auto index = preset_chooser->currentIndex();
            if (index >= 0 && !saved_name.isEmpty()) {
                preset_chooser->setItemText(index, saved_name);
                preset_chooser->setItemData(index, false, Qt::UserRole + 4);
                preset_chooser->setItemData(index,
                    QDir(lingtai_global_dir()).filePath(
                        QStringLiteral("presets/saved/") + saved_name
                            + QStringLiteral(".json")),
                    Qt::UserRole + 7);
            }
            update_review();
            agents_page->load_from_chooser(preset_chooser, saved_name);
            go_to_page(2);
        });
    QObject::connect(agents_page, &AgentPresetsPage::back_requested,
        [go_to_page] { go_to_page(1); });
    QObject::connect(agents_page, &AgentPresetsPage::continue_requested,
        [preset_chooser, agents_page, review_page, go_to_page] {
            const auto name = agents_page->default_name();
            if (name.isEmpty()) return;
            const auto index = preset_chooser->findText(name);
            if (index >= 0) preset_chooser->setCurrentIndex(index);
            review_page->load(name, agents_page->allowed_count());
            go_to_page(3);
        });
    QObject::connect(review_page, &AgentConfigPage::back_requested,
        [go_to_page] { go_to_page(2); });
    QObject::connect(browse_button, &QPushButton::clicked, [this] {
        handle_browse_destination();
    });
    QObject::connect(cancel_button, &QPushButton::clicked, [this] {
        handle_cancel_bootstrap();
    });
    QObject::connect(review_page, &AgentConfigPage::create_requested, [this] {
        handle_create_and_start();
    });
    QObject::connect(codex_strip, &CodexCredentialsStrip::manage_requested,
        open_credentials);
    QObject::connect(edit_preset_page, &PresetEditorPage::credentials_requested,
        open_credentials);
    QObject::connect(credentials_page, &CredentialsPage::back_requested,
        [go_to_page, credentials_return, codex_strip, edit_preset_page] {
            go_to_page(*credentials_return);
            codex_strip->refresh();
            edit_preset_page->refresh_credentials();
        });
    QObject::connect(credentials_page, &CredentialsPage::accounts_changed,
        codex_strip, &CodexCredentialsStrip::refresh);
    QObject::connect(setup_route_, &ProjectSetupWizard::rejected, [this] {
        handle_cancel_bootstrap();
    });
    recompute_setup_layout(setup_route_);
    base::install_event_filter(
        setup_route_,
        setup_route_,
        [route = setup_route_](not_null<QEvent *> event) {
            if (event->type() == QEvent::Resize) {
                recompute_setup_layout(route);
            }
            return base::EventFilterResult::Continue;
        });
    setup_route_->hide();

    pages->setCurrentIndex(0);
    destination_input->submits()
        | rpl::on_next([go_to_page, update_review] {
            update_review();
            go_to_page(3);
        }, submits_lifetime_);

    // The one Telegram-derived mode recompute: Telegram's
    // `SessionController` re-derives OneColumn vs Normal on every chats
    // resize, so the body's own lifetime-owned size stream drives the same
    // single local recompute.
    window_->body()->sizeValue()
        | rpl::on_next([this](QSize size) {
            recompute_layout(size.width());
        }, layout_lifetime_);

    QObject::connect(
        QGuiApplication::styleHints(),
        &QStyleHints::colorSchemeChanged,
        window_.get(),
        [this] { refresh_system_palette(); });
    base::install_event_filter(
        window_.get(),
        qGuiApp,
        [this](not_null<QEvent *> event) {
            if (event->type() == QEvent::ApplicationPaletteChange) {
                refresh_system_palette();
            }
            return base::EventFilterResult::Continue;
        });

    auto *titlebar = [&]() -> Ui::Platform::TitleWidget * {
        for (auto *child : window_->findChildren<QWidget *>(
                QString(), Qt::FindDirectChildrenOnly)) {
            if (auto *candidate = dynamic_cast<Ui::Platform::TitleWidget *>(child)) {
                return candidate;
            }
        }
        return nullptr;
    }();
    if (titlebar) {
        const auto traffic_anchor = NativeTrafficLightAnchor(window_.get());
        auto *titlebar_brand = new QLabel(QStringLiteral("LINGTAI AI"), titlebar);
        titlebar_brand->setObjectName("lingtai_titlebar_brand");
        titlebar_brand->setAccessibleName(QStringLiteral("LINGTAI AI"));
        titlebar_brand->setAttribute(Qt::WA_TransparentForMouseEvents);
        titlebar_brand->setStyleSheet(QStringLiteral("background: transparent;"));
        auto brand_font = titlebar_brand->font();
        brand_font.setPointSize(11);
        brand_font.setWeight(QFont::DemiBold);
        titlebar_brand->setFont(brand_font);
        apply_titlebar_brand_palette(window_.get());
        titlebar_brand->adjustSize();
        titlebar_brand->setFixedHeight(titlebar->height());
        titlebar_brand->move(traffic_anchor.x(), 0);
        titlebar_brand->setProperty(
            "lingtai_native_traffic_light_anchor", traffic_anchor);
        titlebar_brand->show();
        titlebar_brand->raise();
    }

    refresh_route();
    render_roster();
    recompute_layout(window_->body()->width());
}

NativeShell::~NativeShell() = default;

void NativeShell::refresh_system_palette() {
    apply_system_palette();
    apply_titlebar_brand_palette(window_.get());
    apply_project_setup_palette(setup_route_);
    apply_preset_catalog_chrome(window_.get());
    if (auto *startup_heading = window_->findChild<QLabel *>(
            "lingtai_startup_heading")) {
        auto startup_palette = startup_heading->palette();
        startup_palette.setColor(QPalette::WindowText, st::windowBgActive->c);
        startup_heading->setPalette(startup_palette);
    }
    if (auto *popup = window_->findChild<QListWidget *>(
            "lingtai_slash_command_popup")) {
        apply_slash_popup_palette(popup);
    }
    if (detail_view_) detail_view_->refresh_chrome();
    render_conversation();
    window_->update();
    for (auto *widget : window_->findChildren<QWidget *>()) {
        widget->update();
    }
}

void NativeShell::show() {
    refresh_route();
    window_->show();
    ApplyNativeFullSizeTitlebar(window_.get());
    recompute_layout(window_->body()->width());
    // Qt 6.11 can finish recreating its NSWindow on the next main-loop turn.
    // Reapply to the actual post-show window unconditionally; lib_ui's poll
    // only does this when the NSWindow pointer itself changed.
    QTimer::singleShot(0, window_.get(), [this] {
        ApplyNativeFullSizeTitlebar(window_.get());
        recompute_layout(window_->body()->width());
    });
}

void NativeShell::show_offscreen() {
    refresh_route();
    window_->setAttribute(Qt::WA_DontShowOnScreen, true);
    window_->show();
    ApplyNativeFullSizeTitlebar(window_.get());
    recompute_layout(window_->body()->width());
}

void NativeShell::set_open_project_request_handler(
        OpenProjectRequestHandler handler) {
    open_project_request_handler_ = std::move(handler);
}

void NativeShell::set_open_project_in_new_window_request_handler(
        OpenProjectRequestHandler handler) {
    open_project_in_new_window_request_handler_ = std::move(handler);
}

void NativeShell::request_new_project_at(const fs::path &destination) {
    if (auto *input = find_ui_child<Ui::InputField>(
            *window_, "lingtai_bootstrap_destination_input")) {
        input->setText(QString::fromStdString(destination.string()));
    }
    request_new_project();
}

void NativeShell::set_agent_start_fallback_python(
        fs::path fallback_python) {
    agent_start_fallback_python_ = std::move(fallback_python);
}

void NativeShell::set_tui_executable(fs::path executable) {
    tui_executable_ = std::move(executable);
}

void NativeShell::set_bootstrap_actions_enabled(bool enabled) {
    if (auto *open_button = window_->findChild<QPushButton *>(
            "lingtai_open_project_button")) {
        open_button->setEnabled(enabled);
    }
    if (auto *open_new_window = window_->findChild<QPushButton *>(
            "lingtai_open_project_new_window_button")) {
        open_new_window->setEnabled(enabled);
    }
}

void NativeShell::set_bootstrap_status(const QString &text) {
    auto *status = find_ui_child<Ui::FlatLabel>(
        *window_, "lingtai_bootstrap_status");
    if (!status) return;
    status->setText(text);
    bootstrap_status_surface_->setVisible(!text.isEmpty());
}

void NativeShell::request_new_project() {
    if (bootstrap_pending_) return;
    if (tui_executable_.empty()) {
        set_bootstrap_status(QStringLiteral(
            "New Project is unavailable: no TUI executable is configured."));
        refresh_route();
        recompute_layout(window_->body()->width());
        return;
    }
    bootstrap_pending_ = true;
    set_bootstrap_actions_enabled(false);
    set_bootstrap_status(QStringLiteral("Discovering presets…"));
    refresh_route();
    recompute_layout(window_->body()->width());
    bootstrap_runner_->run_presets(tui_executable_, [this](
            PresetDiscoveryResult result) {
        handle_presets_finished(std::move(result));
    });
}

void NativeShell::handle_presets_finished(PresetDiscoveryResult result) {
    if (result.kind != PresetDiscoveryKind::succeeded
        || result.presets.empty()) {
        bootstrap_pending_ = false;
        set_bootstrap_actions_enabled(true);
        QString failure;
        if (result.kind == PresetDiscoveryKind::process_failed) {
            if (!result.error.empty()) {
                failure = QStringLiteral("Preset discovery failed: %1")
                    .arg(QString::fromStdString(result.error));
            } else {
                failure = QStringLiteral("Preset discovery failed.");
            }
        } else if (result.kind == PresetDiscoveryKind::empty) {
            failure = QStringLiteral(
                "No usable presets were found. Preset discovery returned an "
                "empty list.");
        } else {
            failure = QStringLiteral(
                "Preset discovery returned output that could not be used.");
        }
        set_bootstrap_status(failure);
        refresh_route();
        recompute_layout(window_->body()->width());
        return;
    }
    // The flow stays pending while the dialog is open so duplicate New
    // Project / Open Project activation is impossible throughout the whole
    // explicit bootstrap, not only during the two subprocess phases.
    show_setup_wizard(result.presets);
}

void NativeShell::show_setup_wizard(
        const std::vector<PresetEntry> &presets) {
    auto *chooser = window_->findChild<QComboBox *>(
        "lingtai_bootstrap_preset_chooser");
    if (!chooser) return;
    auto *catalog = window_->findChild<QTreeWidget *>(
        "lingtai_setup_preset_catalog");
    if (!catalog) return;
    chooser->clear();
    catalog->clear();

    const auto rows = build_preset_catalog_rows(presets);

    add_preset_section(catalog, QStringLiteral("Saved presets"));
    auto added_templates_header = false;
    for (const auto &row : rows) {
        const auto index = chooser->count();
        const auto name = QString::fromStdString(row.entry.name);
        chooser->addItem(name);
        chooser->setItemData(index, row.summary, Qt::UserRole);
        chooser->setItemData(index, QString::fromStdString(row.entry.source),
            Qt::UserRole + 1);
        chooser->setItemData(index, row.provider, Qt::UserRole + 2);
        chooser->setItemData(index, row.model, Qt::UserRole + 3);
        chooser->setItemData(index, row.is_template, Qt::UserRole + 4);
        chooser->setItemData(index, row.has_vision, Qt::UserRole + 5);
        chooser->setItemData(index, row.has_tools, Qt::UserRole + 6);
        chooser->setItemData(index,
            QString::fromStdString(row.entry.path), Qt::UserRole + 7);
        if (row.is_template && !added_templates_header) {
            add_preset_section(catalog, QStringLiteral("Preset templates"));
            added_templates_header = true;
        }
        add_preset_catalog_row(catalog, row, index);
    }
    if (!added_templates_header) {
        add_preset_section(catalog, QStringLiteral("Preset templates"));
    }

    chooser->setCurrentIndex(-1);
    for (auto index = 0; index != catalog->topLevelItemCount(); ++index) {
        auto *item = catalog->topLevelItem(index);
        if (!is_preset_section(item)) {
            catalog->setCurrentItem(item);
            break;
        }
    }
    if (auto *status = find_ui_child<Ui::FlatLabel>(
            *window_, "lingtai_bootstrap_dialog_status")) {
        status->setText(QString());
    }
    set_bootstrap_status(QString());
    if (auto *pages = window_->findChild<QStackedWidget *>("lingtai_setup_pages")) {
        pages->setCurrentIndex(0);
    }
    if (auto *codex_strip = window_->findChild<CodexCredentialsStrip *>(
            "lingtai_setup_codex_credentials_strip")) {
        codex_strip->refresh();
    }
    if (auto *steps = window_->findChild<QWidget *>("lingtai_setup_steps")) {
        update_setup_step_indicator(steps, 0);
        steps->show();
    }
    if (auto *summary = window_->findChild<QLabel *>(
            "lingtai_setup_preset_footer_summary")) {
        const auto name = chooser->currentText();
        const auto provider = chooser->currentData(Qt::UserRole + 2).toString();
        const auto model = chooser->currentData(Qt::UserRole + 3).toString();
        const auto has_vision = chooser->currentData(Qt::UserRole + 5).toBool();
        const auto has_tools = chooser->currentData(Qt::UserRole + 6).toBool();
        const auto provider_model = provider.isEmpty()
            ? model
            : (model.isEmpty() ? provider
                : provider + QStringLiteral(" · ") + model);
        const auto tags = preset_capability_tags(has_vision, has_tools);
        const auto tokens = preset_catalog_tokens(setup_route_->palette());
        summary->setText(
            preset_footer_rich(name, provider_model, tags,
                tokens.selection_accent, tokens.section_text));
        summary->setAccessibleName(
            preset_footer_plain(name, provider_model, tags));
    }
    setup_route_visible_ = true;
    setup_route_->show();
    setup_route_->setFocus();
    refresh_route();
    recompute_layout(window_->body()->width());
    recompute_setup_layout(setup_route_);
}

void NativeShell::hide_setup_wizard() {
    setup_route_visible_ = false;
    if (setup_route_) {
        setup_route_->hide();
        setup_route_->setMinimumSize(0, 0);
    }
    window_->setMinimumSize(
        QSize(kMinimumWindowWidth, kMinimumWindowHeight));
}

bool NativeShell::in_project_setup() const {
    // The bootstrap status banner is informational only; once the explicit
    // wizard or subprocess phases finish it must not suppress the project route.
    return setup_route_visible_ || bootstrap_pending_;
}

void NativeShell::handle_browse_destination() {
    const auto selected = QFileDialog::getExistingDirectory(
        window_.get(),
        QStringLiteral("Choose destination folder"),
        QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) return;
    if (auto *input = find_ui_child<Ui::InputField>(
            *window_, "lingtai_bootstrap_destination_input")) {
        input->setText(selected);
    }
}

void NativeShell::handle_create_and_start() {
    if (!setup_route_ || !setup_route_visible_
        || bootstrap_runner_->is_pending()) {
        return;
    }
    auto *input = find_ui_child<Ui::InputField>(
        *window_, "lingtai_bootstrap_destination_input");
    auto *chooser = window_->findChild<QComboBox *>(
        "lingtai_bootstrap_preset_chooser");
    auto *dialog_status = find_ui_child<Ui::FlatLabel>(
        *window_, "lingtai_bootstrap_dialog_status");
    if (!input || !chooser || !dialog_status) return;
    const auto destination = input->getLastText().trimmed();
    const auto preset = chooser->currentText().trimmed();
    if (destination.isEmpty() || preset.isEmpty()) {
        dialog_status->setText(QStringLiteral(
            "Choose a nonempty destination folder and a preset."));
        return;
    }
    if (tui_executable_.empty()) {
        dialog_status->setText(QStringLiteral(
            "No TUI executable is configured."));
        return;
    }
    dialog_status->setText(QString());
    hide_setup_wizard();
    refresh_route();
    recompute_layout(window_->body()->width());
    set_bootstrap_status(QStringLiteral(
        "Creating project and starting Agent…"));
    auto agent_name = std::string();
    auto language = std::string();
    if (auto *config = window_->findChild<AgentConfigPage *>(
            "lingtai_setup_review_page")) {
        agent_name = config->agent_name().toStdString();
        language = config->language().toStdString();
    }
    bootstrap_runner_->run_spawn(tui_executable_,
        fs::path(destination.toStdU16String()),
        preset.toStdString(),
        [this](SpawnOutcome outcome) {
            handle_spawn_finished(std::move(outcome));
        },
        agent_name,
        language);
}

void NativeShell::handle_cancel_bootstrap() {
    if (!setup_route_) return;
    // A user dismissal of the New Project flow -- Cancel, Escape, or
    // reject() -- is always a no-spawn cancellation. It must not run while a
    // discovery/spawn subprocess is pending.
    if (bootstrap_runner_ && bootstrap_runner_->is_pending()) return;
    bootstrap_pending_ = false;
    hide_setup_wizard();
    refresh_route();
    recompute_layout(window_->body()->width());
    set_bootstrap_status(QString());
    set_bootstrap_actions_enabled(true);
}

void NativeShell::handle_spawn_finished(SpawnOutcome outcome) {
    bootstrap_pending_ = false;
    hide_setup_wizard();
    refresh_route();
    recompute_layout(window_->body()->width());
    set_bootstrap_actions_enabled(true);
    if (outcome.kind != SpawnOutcomeKind::launched
        || outcome.project_dir.empty()) {
        QString failure;
        if (outcome.kind == SpawnOutcomeKind::process_failed) {
            if (!outcome.error.empty()) {
                failure = outcome.code.empty()
                    ? QStringLiteral("Project creation failed: %1").arg(
                        QString::fromStdString(outcome.error))
                    : QStringLiteral("Project creation failed (%1): %2").arg(
                        QString::fromStdString(outcome.code),
                        QString::fromStdString(outcome.error));
            } else {
                failure = QStringLiteral("Project creation failed.");
            }
        } else {
            failure = QStringLiteral(
                "Project creation returned output that could not be used.");
        }
        set_bootstrap_status(failure + QStringLiteral(
            " The destination may contain a partially initialized LingTai "
            "project."));
        return;
    }
    const auto opened = open_project(outcome.project_dir, std::nullopt);
    set_bootstrap_status(opened.disposition == ProjectOpenDisposition::opened
        ? QStringLiteral("Project created and Agent started.")
        : QStringLiteral(
            "Project was created but could not be opened here."));
}

ProjectOpenOutcome NativeShell::open_project(
        const fs::path &selected_directory,
        const std::optional<fs::path> &agent_relative_directory) {
    auto attached = attach_project(selected_directory);
    if (!attached) {
        switch (attached.failure) {
        case ProjectPathFailure::selection_not_found:
            return show_open_error(attached.failure,
                "The selected project does not exist.");
        case ProjectPathFailure::selection_not_directory:
            return show_open_error(attached.failure,
                "The selected project is not a directory.");
        default:
            return show_open_error(attached.failure,
                "The selected project could not be opened.");
        }
    }

    const auto metadata_path = attached.attachment->root() / ".lingtai";
    std::error_code status_error;
    const auto metadata_status = fs::symlink_status(metadata_path, status_error);
    if (status_error) {
        if (status_error == std::errc::no_such_file_or_directory) {
            return show_open_error(ProjectPathFailure::target_not_found,
                "The selected directory is not a LingTai project: .lingtai is missing.");
        }
        return show_open_error(ProjectPathFailure::filesystem_error,
            "The selected project's .lingtai directory could not be inspected.");
    }
    if (!fs::exists(metadata_status)) {
        return show_open_error(ProjectPathFailure::target_not_found,
            "The selected directory is not a LingTai project: .lingtai is missing.");
    }
    if (fs::is_symlink(metadata_status)) {
        const auto resolved = attached.attachment->resolve(".lingtai");
        return show_open_error(
            resolved.failure == ProjectPathFailure::outside_project
                ? ProjectPathFailure::outside_project
                : ProjectPathFailure::target_not_directory,
            resolved.failure == ProjectPathFailure::outside_project
                ? "The selected project's .lingtai path escapes the project root."
                : "The selected project's .lingtai path must be a real directory.");
    }
    if (!fs::is_directory(metadata_status)) {
        return show_open_error(ProjectPathFailure::target_not_directory,
            "The selected project's .lingtai path is not a directory.");
    }
    const auto contained_metadata = attached.attachment->resolve(".lingtai");
    if (!contained_metadata) {
        return show_open_error(contained_metadata.failure,
            "The selected project's .lingtai directory is not safely contained.");
    }

    auto agents = project_agents(*attached.attachment);
    const auto canonical_root = attached.attachment->root();
    const auto same_root = selection_state_.active_project()
        && selection_state_.active_project()->root() == canonical_root;
    auto selected_key = std::optional<fs::path>();
    if (agent_relative_directory) {
        const auto &relative = *agent_relative_directory;
        if (relative.parent_path() == fs::path(".lingtai")
            && selectable_item(agents, relative.filename())) {
            selected_key = relative.filename();
        }
    } else if (same_root
            && selection_state_.selected_agent_directory_key()
            && selectable_item(agents,
                *selection_state_.selected_agent_directory_key())) {
        selected_key = selection_state_.selected_agent_directory_key();
    }

    selection_state_.activate_project(std::move(*attached.attachment));
    selection_state_.clear_agent_selection();
    bump_lifecycle_generation();
    if (selected_key) {
        static_cast<void>(selection_state_.select_agent(*selected_key));
    }
    agents_ = std::move(agents);
    window_->findChild<QLabel *>("lingtai_project_root")
        ->setText(path_text(canonical_root));
    agent_roster_->set_project_display_name(QString::fromStdString(
        canonical_root.filename().string()));
    render_roster();
    auto *selection_error = window_->findChild<QLabel *>(
        "lingtai_agent_selection_error");
    selection_error->clear();
    selection_error->hide();
    auto *open_error = find_ui_child<Ui::FlatLabel>(
        *window_, "lingtai_project_open_error");
    open_error->setText(QString());
    open_error->setAccessibleName(QString());
    open_error_surface_->hide();
    reset_composer();
    reaction_store_.clear();
    injected_mail_journal_.reset();
    // A fresh open must never let a prior target's pending sleep or Start
    // observation surface under the newly opened project/selection.
    pending_sleep_observation_.reset();
    pending_start_observation_.reset();
    if (auto *start_status = window_->findChild<QLabel *>(
            "lingtai_selected_agent_start_status")) {
        start_status->clear();
    }
    refresh_route();
    show_detail_page(AgentDetailPage::conversation);
    recompute_layout(window_->body()->width());
    return {
        .disposition = ProjectOpenDisposition::opened,
        .failure = ProjectPathFailure::none,
    };
}

Ui::RpWindow &NativeShell::window() noexcept {
    return *window_;
}

const Ui::RpWindow &NativeShell::window() const noexcept {
    return *window_;
}

const WorkspaceSelectionState &NativeShell::selection_state() const noexcept {
    return selection_state_;
}

bool NativeShell::smoke_ready() const noexcept {
    const auto *content = window_->findChild<Ui::RpWidget *>(
        "lingtai_desktop_content");
    const auto *separator = window_->findChild<Ui::RpWidget *>(
        "lingtai_roster_separator");
    return window_->objectName() == "lingtai_desktop_window"
        && window_->body().get()->objectName() == "lingtai_desktop_body"
        && agent_roster_ && agent_roster_->objectName()
            == "lingtai_desktop_sidebar"
        && content && content->objectName() == "lingtai_desktop_content"
        && separator && separator->objectName() == "lingtai_roster_separator"
        && startup_route_ && startup_route_->isVisible()
        && window_->testAttribute(Qt::WA_DontShowOnScreen)
        && window_->isVisible();
}

void NativeShell::request_open_project() {
    if (bootstrap_pending_) return;
    if (open_project_request_handler_) {
        open_project_request_handler_();
    }
}

void NativeShell::request_open_project_in_new_window() {
    if (bootstrap_pending_) return;
    if (open_project_in_new_window_request_handler_) {
        open_project_in_new_window_request_handler_();
    }
}

void NativeShell::render_roster() {
    auto *selected_key = window_->findChild<QLabel *>(
        "lingtai_selected_agent_key");
    auto *presentation_name = window_->findChild<QLabel *>(
        "lingtai_selected_agent_presentation_name");
    auto *selected_avatar = static_cast<SelectedAgentAvatar *>(
        window_->findChild<QWidget *>("lingtai_selected_agent_avatar"));
    auto *manifest_identity = window_->findChild<QLabel *>(
        "lingtai_selected_agent_manifest_identity");
    auto *manifest_llm = window_->findChild<QLabel *>(
        "lingtai_selected_agent_manifest_llm");
    auto *manifest_capabilities = window_->findChild<QLabel *>(
        "lingtai_selected_agent_manifest_capabilities");
    auto *status_activity = window_->findChild<QLabel *>(
        "lingtai_selected_agent_status_activity");
    auto *status_context = window_->findChild<QLabel *>(
        "lingtai_selected_agent_status_context");
    auto *selected_facts = window_->findChild<QLabel *>(
        "lingtai_selected_agent_facts");
    if (!selected_key || !presentation_name || !selected_avatar
        || !manifest_identity
        || !manifest_llm || !manifest_capabilities || !status_activity
        || !status_context || !selected_facts) {
        return;
    }

    // The persistent left column owns the roster rows and their state label;
    // it rebuilds its row tree only when the visible model actually changed,
    // so an unchanged one-second projection refresh keeps scroll, focus, and
    // row identity intact.
    agent_roster_->set_rows(
        agents_, selection_state_.selected_agent_directory_key());

    const auto selected = selection_state_.selected_agent_directory_key();
    const AgentRow *detail_item = nullptr;
    for (const auto &item : agents_.items) {
        if (selected && *selected == item.directory_key
            && item.manifest_kind == AgentManifestKind::valid) {
            detail_item = &item;
            break;
        }
    }

    if (!detail_item) {
        selected_key->setText(QString());
        presentation_name->clear();
        presentation_name->setProperty("lingtai_full_text", QString());
        presentation_name->setAccessibleDescription(QString());
        selected_avatar->set_agent_name(QString());
        selected_avatar->hide();
        manifest_identity->clear();
        manifest_llm->clear();
        manifest_capabilities->clear();
        status_activity->clear();
        status_context->clear();
        selected_facts->setText(QStringLiteral(
            "Choose a valid manifest row to inspect its detail."));
        render_conversation();
        render_agent_preset_summary();
        render_agent_sleep_status();
        render_agent_start_status();
        if (auto *start_status = window_->findChild<QLabel *>(
                "lingtai_selected_agent_start_status")) {
            start_status->clear();
        }
        return;
    }
    const auto &identity = detail_item->identity;
    const auto key = path_text(detail_item->directory_key);
    const auto title = identity && identity->nickname
            ? QString::fromStdString(*identity->nickname)
        : identity && identity->true_name
            ? QString::fromStdString(*identity->true_name)
            : key;
    presentation_name->setText(title);
    // The full presentation title is retained on the label itself (a dynamic
    // property and the accessible description) so the responsive top-bar fit
    // can elide only the visible text without ever losing the identity.
    presentation_name->setProperty("lingtai_full_text", title);
    presentation_name->setAccessibleDescription(title);
    selected_avatar->set_agent_name(title);
    selected_avatar->show();
    const auto friendly_role = friendly_agent_role_text(detail_item->role);
    const auto state = friendly_agent_lifecycle_text(*detail_item);
    const auto role = friendly_role.isEmpty()
        ? role_text(detail_item->role)
        : friendly_role;
    selected_key->setText(
        QStringLiteral("%1 · %2").arg(role, state));
    if (identity) {
        manifest_identity->setText(QStringLiteral(
            "Manifest identity\naddress: %1\nagent ID: %2\nstate: %3")
            .arg(value_text(identity->address),
                value_text(identity->agent_id), value_text(identity->state)));
        manifest_llm->setText(QStringLiteral(
            "Manifest live LLM\nprovider: %1\nmodel: %2\nbase URL: %3\n"
            "API compatibility: %4\ncontext limit: %5")
            .arg(value_text(identity->llm.provider), value_text(identity->llm.model),
                value_text(identity->llm.base_url),
                value_text(identity->llm.api_compat),
                value_text(identity->llm.context_limit)));
        manifest_capabilities->setText(QStringLiteral(
            "Manifest capabilities\ndisplay names: %1")
            .arg(joined_names(identity->capabilities.display_names)));
    } else {
        manifest_identity->setText(QStringLiteral("Manifest identity unavailable"));
        manifest_llm->setText(QStringLiteral("Manifest live LLM unavailable"));
        manifest_capabilities->setText(
            QStringLiteral("Manifest capabilities unavailable"));
    }
    if (detail_item->status) {
        const auto &status = *detail_item->status;
        const auto active = status.active_turn
            ? &*status.active_turn : nullptr;
        status_activity->setText(QStringLiteral(
            "Status activity\nstate: %1\nrunning: %2\nPID: %3\n"
            "state changed at: %4\nlast progress at: %5\n"
            "no progress seconds: %6\nactive turn kind: %7\n"
            "active turn ID: %8\nactive turn started at: %9\n"
            "active turn elapsed seconds: %10")
            .arg(value_text(status.state),
                value_text(status.running), value_text(status.pid),
                value_text(status.state_changed_at),
                value_text(status.last_progress_at),
                value_text(status.no_progress_seconds),
                active ? value_text(active->kind) : QStringLiteral("unavailable"),
                active ? value_text(active->id) : QStringLiteral("unavailable"),
                active ? value_text(active->started_at) : QStringLiteral("unavailable"),
                active ? value_text(active->elapsed_seconds)
                       : QStringLiteral("unavailable")));
        if (status.context) {
            const auto &context = *status.context;
            status_context->setText(QStringLiteral(
                "Status context (source values)\nwindow size: %1\n"
                "system tokens: %2\ntools tokens: %3\nhistory tokens: %4\n"
                "total tokens: %5\nusage_percent (source usage_pct): %6\n"
                "fixed tokens: %7\ngrowing tokens: %8")
                .arg(QString::number(context.window_size),
                    value_text(context.system_tokens),
                    value_text(context.tools_tokens),
                    value_text(context.history_tokens),
                    value_text(context.total_tokens),
                    value_text(context.usage_percent),
                    value_text(context.fixed_tokens),
                    value_text(context.growing_tokens)));
        } else {
            status_context->setText(QStringLiteral(
                "Status context unavailable (no valid positive window projected)"));
        }
    } else {
        status_activity->setText(QStringLiteral(
            "Status activity unavailable from status source"));
        status_context->setText(QStringLiteral(
            "Status context unavailable (no valid positive window projected)"));
    }
    selected_facts->setText(QStringLiteral("manifest: %1\nrole: %2\npresence: %3")
        .arg(manifest_text(detail_item->manifest_kind),
            role_text(detail_item->role), presence_text(detail_item->presence)));
    render_conversation();
    render_agent_preset_summary();
    render_agent_sleep_status();
    render_agent_start_status();
    // Do not clear lingtai_selected_agent_start_status here. Ambient one-
    // second roster refreshes call this path and must leave terminal Start
    // wording ("Agent is online.", failures) intact; selection/project
    // boundaries clear the label themselves.
}

// Shows the current direct conversation for whatever the roster just made the
// selected Agent. It reads the human's own mailbox and infers nothing about
// delivery, replies, or unread state.
void NativeShell::render_conversation() {
    if (!detail_view_) return;

    const bool selection_present = selection_state_.active_project()
        && selection_state_.selected_agent_directory_key();

    auto main_agent_name = QString();
    for (const auto &item : agents_.items) {
        if (item.role != AgentRole::main) {
            continue;
        }
        if (item.identity && item.identity->nickname) {
            main_agent_name = QString::fromStdString(*item.identity->nickname);
        } else if (item.identity && item.identity->true_name) {
            main_agent_name = QString::fromStdString(*item.identity->true_name);
        } else {
            main_agent_name = path_text(item.directory_key);
        }
        break;
    }

    if (!selection_present) {
        DirectConversationHistory empty;
        detail_view_->render_conversation(
            QString(), empty, QString(),
            /*selection_present=*/false,
            /*conversation_route_available=*/false,
            {},
            main_agent_name);
        return;
    }

    const auto route = resolve_direct_conversation_route(
        *selection_state_.active_project(), agents_,
        selection_state_.selected_agent_directory_key());
    const bool route_available = route.has_value();
    if (!route_available) {
        DirectConversationHistory empty;
        detail_view_->render_conversation(
            QString(), empty, QString(),
            /*selection_present=*/true,
            /*conversation_route_available=*/false);
        return;
    }

    const auto history = read_direct_conversation(*route);
    injected_mail_journal_.poll(
        route->project_root, route->target_directory_key);
    sync_seen_from_injected(reaction_store_, injected_mail_journal_.ids());
    sync_receipts_from_history(reaction_store_, history.messages);
    const auto *presentation_name = window_->findChild<QLabel *>(
        "lingtai_selected_agent_presentation_name");
    // Sender identity always comes from the stored full title, never the
    // possibly elided visible text the responsive top-bar fit may have set.
    const auto full_title = presentation_name
        ? presentation_name->property("lingtai_full_text").toString()
        : QString();
    const auto them = !full_title.isEmpty()
        ? full_title
        : path_text(route->target_directory_key);

    const auto count = history.messages.size();
    auto compact = count == 1
        ? QStringLiteral("1 message")
        : QStringLiteral("%1 messages").arg(count);
    if (history.skipped > 0) {
        compact += QStringLiteral(" · %1 skipped").arg(history.skipped);
    }

    detail_view_->render_conversation(
        them, history, compact,
        /*selection_present=*/true,
        /*conversation_route_available=*/true,
        reaction_store_.all());
}

// Called only when the selected target actually changes (a fresh project open
// or a successful Agent selection), never on an ordinary conversation
// refresh, so a just-set receipt is never wiped by its own refresh.
void NativeShell::reset_composer() {
    if (auto *input = static_cast<Ui::InputField *>(
            window_->findChild<QObject *>("lingtai_composer_input"))) {
        input->clear();
        input->setPlaceholder(rpl::single(QStringLiteral("Message…")));
    }
    hide_slash_command_popup(window_.get());
    if (auto *status = window_->findChild<QLabel *>("lingtai_composer_status")) {
        status->clear();
    }
}

// Shows the selected Agent's own kernel-published resolved preset policy:
// only the minimal Provider, Model, Default and ordered Allowed refs from
// `system/manifest.resolved.json`. It is a distinct source and surface from
// the mailbox conversation above, refreshed on the same explicit
// open/selection paths plus the one-second timer. Every observation is shown
// exactly as read, so an absent/stale/unavailable current observation never
// keeps a prior target's projection visible.
void NativeShell::render_agent_preset_summary() {
    if (!detail_view_) return;

    if (!selection_state_.active_project()
        || !selection_state_.selected_agent_directory_key()) {
        detail_view_->render_preset_summary(std::nullopt);
        return;
    }

    const auto summary = read_agent_preset_summary(
        *selection_state_.active_project(),
        *selection_state_.selected_agent_directory_key());
    detail_view_->render_preset_summary(summary);
}

void NativeShell::render_kanban() {
    if (!detail_view_
        || current_detail_page_ != AgentDetailPage::kanban) {
        return;
    }
    if (!selection_state_.active_project()) {
        detail_view_->render_kanban({}, std::nullopt);
        return;
    }
    detail_view_->render_kanban(
        read_kanban_board(*selection_state_.active_project(), agents_),
        selection_state_.selected_agent_directory_key());
}

void NativeShell::handle_kanban_agent_selected(const fs::path &directory_key) {
    auto *error = window_->findChild<QLabel *>("lingtai_agent_selection_error");
    const auto *item = selectable_item(agents_, directory_key);
    if (!item) {
        if (error) {
            error->setText(QStringLiteral(
                "This roster item cannot be selected."));
            error->show();
        }
        return;
    }
    const auto result = selection_state_.select_agent(item->directory_key);
    if (result != AgentSelectionResult::selected
        || !selection_state_.active_project()) {
        if (error) {
            error->setText(QStringLiteral("Agent selection was rejected."));
            error->show();
        }
        return;
    }
    reset_composer();
    bump_lifecycle_generation();
    pending_sleep_observation_.reset();
    pending_start_observation_.reset();
    if (auto *start_status = window_->findChild<QLabel *>(
            "lingtai_selected_agent_start_status")) {
        start_status->clear();
    }
    render_roster();
    show_detail_page(AgentDetailPage::kanban);
    if (error) {
        error->clear();
        error->hide();
    }
}

// Classifies the raw composer text before ordinary trim/send handling. Every
// parsed slash command is cleared and dispatched locally here, so even an
// unknown or deliberately unavailable command can never reach DirectPublisher.
// Ordinary messages still resolve the route fresh from current C1/C3 truth
// rather than capturing it once, so a selection change between typing and
// clicking Send can never deliver to a stale target.
void NativeShell::handle_send_message() {
    auto *input = static_cast<Ui::InputField *>(
        window_->findChild<QObject *>("lingtai_composer_input"));
    auto *status = window_->findChild<QLabel *>("lingtai_composer_status");
    if (!input || !status) return;
    hide_slash_command_popup(window_.get());
    const auto raw_text = input->getLastText();
    if (const auto command = parse_slash_command(raw_text.toStdString())) {
        input->clear();
        if (command->name == "suspend" || command->name == "clear"
            || command->name == "refresh") {
            handle_lifecycle_command(command->name, command->args);
            return;
        }
        if (handle_prompt_command(command->name, command->args)) {
            return;
        }
        if (!command->args.empty()) {
            status->setText(QStringLiteral(
                "Command not available in this Desktop build."));
            return;
        }
        if (command->name == "presets") {
            status->clear();
            show_detail_page(AgentDetailPage::presets);
            return;
        }
        if (command->name == "setup") {
            status->clear();
            if (selection_state_.active_project()) {
                request_new_project_at(
                    selection_state_.active_project()->root());
            } else {
                request_new_project();
            }
            return;
        }
        if (command->name == "kanban") {
            status->clear();
            show_detail_page(AgentDetailPage::kanban);
            return;
        }
        if (command->name == "agents") {
            status->clear();
            if (detail_back_button_ && detail_back_button_->isVisible()) {
                handle_detail_back();
            } else {
                agent_roster_->focus_row(
                    selection_state_.selected_agent_directory_key());
            }
            return;
        }
        if (command->name == "sleep") {
            status->clear();
            handle_request_sleep();
            return;
        }
        if (command->name == "cpr") {
            status->clear();
            handle_start_agent();
            return;
        }
        if (command->name == "help") {
            status->setText(QStringLiteral(
                "Available commands: /agents, /presets, /setup, /kanban, "
                "/sleep, /cpr, /clear, /refresh, /suspend, /btw, /insights, "
                "/goal, /export, /molt, /help, /quit."));
            return;
        }
        if (command->name == "quit") {
            status->clear();
            window_->hide();
            QCoreApplication::quit();
            return;
        }
        status->setText(QStringLiteral(
            "Command not available in this Desktop build."));
        return;
    }

    const auto text = raw_text.trimmed();
    if (text.isEmpty()) return; // reject whitespace-only input without writing

    if (!selection_state_.active_project()
        || !selection_state_.selected_agent_directory_key()) {
        status->setText(QStringLiteral("Select an Agent to send a message."));
        return;
    }
    const auto route = resolve_direct_conversation_route(
        *selection_state_.active_project(), agents_,
        selection_state_.selected_agent_directory_key());
    if (!route) {
        status->setText(QStringLiteral(
            "No conversation is available for this selection."));
        return;
    }

    const auto outcome = send_direct_mail(*route, text.toStdString());
    if (outcome.result == DirectMailSendResult::queued) {
        input->clear();
        if (!outcome.message_id.empty()) {
            reaction_store_.set_receipt(
                outcome.message_id, ReceiptStage::received);
        }
        status->clear();
        render_conversation();
        if (detail_view_) {
            detail_view_->scroll_conversation_to_bottom();
        }
    } else {
        status->setText(QStringLiteral("Message was not queued."));
    }
}

void NativeShell::handle_agent_selection(const fs::path &directory_key) {
    auto *error = window_->findChild<QLabel *>("lingtai_agent_selection_error");
    const auto *item = selectable_item(agents_, directory_key);
    if (!item) {
        if (error) {
            error->setText(QStringLiteral(
                "This roster item cannot be selected."));
            error->show();
        }
        return;
    }
    const auto result = selection_state_.select_agent(item->directory_key);
    if (result != AgentSelectionResult::selected
        || !selection_state_.active_project()) {
        if (error) {
            error->setText(QStringLiteral("Agent selection was rejected."));
            error->show();
        }
        return;
    }
    reset_composer();
    bump_lifecycle_generation();
    reaction_store_.clear();
    injected_mail_journal_.reset();
    // A selection change must never let a prior target's pending sleep or
    // Start observation or terminal result surface under the newly selected
    // Agent.
    pending_sleep_observation_.reset();
    pending_start_observation_.reset();
    if (auto *start_status = window_->findChild<QLabel *>(
            "lingtai_selected_agent_start_status")) {
        start_status->clear();
    }
    render_roster();
    show_detail_page(AgentDetailPage::conversation);
    recompute_layout(window_->body()->width());
    // Telegram's `HistoryWidget::setInnerFocus()`: a selected Agent focuses
    // the visible, enabled composer.
    if (auto *composer = static_cast<Ui::InputField *>(
            window_->findChild<QObject *>("lingtai_composer_input"))) {
        if (composer->isVisible() && composer->isEnabled()) {
            composer->setFocus();
        }
    }
    if (error) {
        error->clear();
        error->hide();
    }
}

bool NativeShell::handle_prompt_command(
        const std::string &name, const std::string &args) {
    auto *status = window_->findChild<QLabel *>("lingtai_composer_status");
    if (!status) return false;
    const auto known = name == "btw" || name == "insights" || name == "goal"
        || name == "export" || name == "molt";
    if (!known) return false;
    if (name == "btw" && args.empty()) {
        status->setText(QStringLiteral("Usage: /btw <question>"));
        return true;
    }
    if (name == "export" && !args.empty() && args != "recipe") {
        status->setText(QStringLiteral(
            "[system] Usage: /export — or — /export recipe"));
        return true;
    }
    if (!selection_state_.active_project()
        || !selection_state_.selected_agent_directory_key()) {
        if (name == "goal") {
            status->setText(QStringLiteral(
                "No current agent is selected; cannot send a goal request."));
        } else if (name == "export") {
            status->setText(QStringLiteral(
                "[system] No orchestrator running — start an agent first."));
        } else {
            status->setText(QStringLiteral(
                "Command not available in this Desktop build."));
        }
        return true;
    }
    const auto &attachment = *selection_state_.active_project();
    const auto key = *selection_state_.selected_agent_directory_key();
    const auto *item = selectable_item(agents_, key);
    if (!item || item->presence != AgentPresenceKind::alive) {
        status->setText(QStringLiteral(
            "Agent is not running. Try /refresh first."));
        return true;
    }
    if (name == "btw") {
        static_cast<void>(write_agent_inquiry(attachment, key, "human", args));
        status->setText(QStringLiteral("Inquiry sent: %1")
            .arg(QString::fromStdString(args)));
        return true;
    }
    if (name == "insights") {
        static_cast<void>(write_insight_inquiry(attachment, key));
        status->setText(QStringLiteral("Requesting insight..."));
        return true;
    }
    if (name == "molt") {
        static_cast<void>(write_molt_prompt(attachment, key));
        status->setText(QStringLiteral("Molt command sent."));
        return true;
    }
    if (name == "export") {
        static_cast<void>(write_export_recipe_prompt(attachment, key));
        status->setText(QStringLiteral(
            "[system] Asked the orchestrator to start the recipe export flow."));
        return true;
    }
    const auto goal = write_agent_goal_request(attachment, key, args);
    if (!goal.ok) {
        status->setText(QStringLiteral(
            "Failed to send goal request notification: write failed"));
        return true;
    }
    status->setText(QStringLiteral(
        "Goal request notification sent (%1). The agent will read the goal "
        "manual and guide goal creation.")
        .arg(QString::fromStdString(goal.event_id)));
    return true;
}

// The one selected-Agent lifecycle owner for `/suspend`, `/clear`, and
// `/refresh [preset]`. Only the empty forms and a zero-or-one raw preset
// argument are accepted; extra or invalid arguments are rejected locally
// without launching. Dispatch always rides the already-injected
// `tui_executable_` with the exact separate argv through the one owned
// AgentCommandRunner; a duplicate lifecycle slash while pending is rejected
// with the exact truthful status, and ordinary chat send is never disabled.
void NativeShell::handle_lifecycle_command(
        const std::string &name, const std::string &args) {
    auto *status = window_->findChild<QLabel *>("lingtai_composer_status");
    if (!status) return;
    if (!selection_state_.active_project()
        || !selection_state_.selected_agent_directory_key()) {
        status->setText(QStringLiteral(
            "Command not available in this Desktop build."));
        return;
    }
    std::string optional_arg;
    const auto single_preset = !args.empty()
        && args.find(' ') == std::string::npos;
    const auto valid = ((name == "suspend" || name == "clear") && args.empty())
        || (name == "refresh" && (args.empty() || single_preset));
    if (!valid) {
        status->setText(QStringLiteral(
            "Command not available in this Desktop build."));
        return;
    }
    if (single_preset) {
        optional_arg = args;
    }
    const auto project_root =
        path_text(selection_state_.active_project()->root() / ".lingtai")
            .toStdString();
    const auto agent_key =
        selection_state_.selected_agent_directory_key()->string();
    if (!command_runner_.run(tui_executable_, project_root, agent_key, name,
            optional_arg, lifecycle_generation(),
            [this](AgentCommandResult result) {
                handle_lifecycle_finished(std::move(result));
            })) {
        status->setText(QStringLiteral("Agent command already pending."));
        return;
    }
    pending_lifecycle_action_ = name;
    status->setText(QStringLiteral("Agent command pending."));
}

// The one terminal lifecycle delivery. A completion may update the existing
// conversation status only when the generation, canonical project root, and
// selected Agent key captured at dispatch still match the current selection
// context, so an old completion -- including an away-and-back return to the
// same key -- can never surface under a later selection.
void NativeShell::handle_lifecycle_finished(AgentCommandResult result) {
    auto *status = window_->findChild<QLabel *>("lingtai_composer_status");
    if (!status) return;
    const auto matching_context = selection_state_.active_project()
        && selection_state_.selected_agent_directory_key()
        && result.bound_generation == lifecycle_generation()
        && result.bound_project_root
            == path_text(selection_state_.active_project()->root()
                / ".lingtai").toStdString()
        && result.bound_agent_key
            == selection_state_.selected_agent_directory_key()->string();
    if (!matching_context || pending_lifecycle_action_.empty()) {
        pending_lifecycle_action_.clear();
        return;
    }
    auto signaled = QString::fromStdString(pending_lifecycle_action_);
    signaled[0] = signaled[0].toUpper();
    status->setText(result.kind == AgentCommandResultKind::succeeded
        ? signaled + QStringLiteral(" signaled.")
        : QStringLiteral("Agent command failed."));
    pending_lifecycle_action_.clear();
}

std::string NativeShell::lifecycle_generation() const noexcept {
    return std::to_string(selection_generation_);
}

void NativeShell::bump_lifecycle_generation() noexcept {
    ++selection_generation_;
}

// Telegram's one mode recompute, fed by the body's own size stream: below
// the source-backed two-surface threshold (`260 + 380` usable column pixels
// after the 8px drag handle) exactly one full-width surface is shown -- the
// roster until an Agent is selected, then the detail with Back; at or above it
// roster + handle + detail all show and Back is hidden. The semantic separator
// object stays hidden in both modes so responsive recompute cannot restore an
// unwanted full-height pane edge. A selected Agent is the sole state that
// decides which narrow surface is active, so a wide->narrow resize with an
// active selection keeps the detail, exactly as Telegram keeps the active
// chat in OneColumn.
void NativeShell::recompute_layout(int body_width) {
    const auto project_active = selection_state_.active_project().has_value();
    const auto setup_active = in_project_setup();
    if (startup_route_) {
        const auto open_error_visible = open_error_active(*window_);
        startup_route_->setVisible(!project_active && !setup_active
            && !open_error_visible);
    }
    if (auto *brand = window_->findChild<QLabel *>("lingtai_titlebar_brand")) {
        auto *titlebar = brand->parentWidget();
        if (!project_active && !setup_active && titlebar) {
            brand->move((titlebar->width() - brand->width()) / 2, 0);
        } else {
            const auto traffic_anchor = NativeTrafficLightAnchor(window_.get());
            brand->setProperty("lingtai_native_traffic_light_anchor", traffic_anchor);
            brand->move(traffic_anchor.x(), 0);
        }
        brand->raise();
    }
    if (setup_active) {
        agent_roster_->setVisible(false);
        roster_resize_handle_->setVisible(false);
        separator_->setVisible(false);
        content_->setVisible(true);
        recompute_setup_layout(setup_route_);
        return;
    }
    if (!project_active) {
        agent_roster_->setVisible(false);
        roster_resize_handle_->setVisible(false);
        separator_->setVisible(false);
        const auto open_error_visible = open_error_active(*window_);
        content_->setVisible(open_error_visible);
        return;
    }
    const auto available = body_width - kRosterResizeHandleWidth
        - kRosterSeparatorWidth;
    if (available >= kTwoColumnAvailableThreshold) {
        auto roster_width = qRound(body_width * roster_width_ratio_);
        const auto roster_minimum =
            (roster_width_ratio_ < kWideRosterWidthRatio)
            ? kCollapsedRosterColumnWidth
            : kRosterColumnWidth;
        roster_width = std::clamp(roster_width,
            roster_minimum,
            body_width - kDetailColumnMinimumWidth
                - kRosterResizeHandleWidth - kRosterSeparatorWidth);
        agent_roster_->setVisible(true);
        agent_roster_->set_roster_width(roster_width);
        roster_resize_handle_->setVisible(true);
        separator_->setVisible(false);
        content_->setVisible(true);
        detail_back_button_->setVisible(false);
        const auto detail_width = body_width - roster_width
            - kRosterResizeHandleWidth - kRosterSeparatorWidth;
        if (detail_view_) {
            detail_view_->set_detail_width(detail_width);
        } else {
            update_composer_width(detail_width);
            update_top_bar_fit(detail_width);
            fit_kanban_page(detail_width);
        }
        return;
    }
    const auto detail_active =
        selection_state_.selected_agent_directory_key().has_value();
    agent_roster_->setVisible(!detail_active);
    agent_roster_->set_roster_width(detail_active
        ? kRosterColumnWidth
        : std::max(body_width, kRosterColumnWidth));
    if (roster_resize_handle_) roster_resize_handle_->setVisible(false);
    separator_->setVisible(false);
    content_->setVisible(detail_active);
    detail_back_button_->setVisible(detail_active);
    if (detail_view_) {
        detail_view_->set_detail_width(body_width);
    } else {
        update_composer_width(body_width);
        update_top_bar_fit(body_width);
        fit_kanban_page(body_width);
    }
}

// The one responsive chat-top-bar measure, re-entered on every recompute (and
// so on every real resize and selection change): the actual detail/header
// width is exactly what `recompute_layout` just derived -- in Normal mode the
// body minus the actual chosen roster width, 8px drag handle, and 1px
// separator; in OneColumn detail the full body width. The identity name keeps
// its full title stored on the presentation-name label (a `lingtai_full_text`
// dynamic property and the accessible description), so it can elide for the
// current width while the full identity never leaves accessibility. The
// complete row is first measured with the name unbounded and every secondary
// element visible; if it does not fit, the secondary key hides first, then
// both action status labels, while the one primary action, the icon-only
// Request sleep secondary, and Back stay reachable. The remaining width is
// then allocated to the name -- the actual detail width minus the row's
// natural non-name width, clamped to at least one pixel -- and its visible
// text becomes the right-elided full title. No timer/event framework or
// persisted state; primary controls, the icon-only secondary, fonts, and
// object names are never touched.
void NativeShell::update_top_bar_fit(int detail_width) {
    if (!chat_top_bar_ || !selected_agent_key_) return;
    auto *presentation_name = chat_top_bar_->findChild<QLabel *>(
        "lingtai_selected_agent_presentation_name");
    auto *start_status = chat_top_bar_->findChild<QLabel *>(
        "lingtai_selected_agent_start_status");
    auto *sleep_status = chat_top_bar_->findChild<QLabel *>(
        "lingtai_selected_agent_sleep_status");
    if (!presentation_name || !start_status || !sleep_status) return;
    const auto full = presentation_name->property(
        "lingtai_full_text").toString();
    if (full.isEmpty()) return;
    // Restore the full natural row: the name is unbounded with its full
    // title restored, and the secondary key plus both action captions show.
    presentation_name->setMaximumWidth(QWIDGETSIZE_MAX);
    presentation_name->setText(full);
    selected_agent_key_->setVisible(true);
    start_status->setVisible(true);
    sleep_status->setVisible(true);
    // Every nonempty Start/Sleep status must fit its own bounded label width
    // before any primary identity space is consumed; one that cannot is
    // hidden so a long read-out never clips inside its action row.
    const auto status_self_fits = [](QLabel *status) {
        if (status->text().isEmpty()) return true;
        return QFontMetrics(status->font()).horizontalAdvance(status->text())
            <= status->maximumWidth();
    };
    if (!status_self_fits(start_status)) start_status->setVisible(false);
    if (!status_self_fits(sleep_status)) sleep_status->setVisible(false);
    // Priority cascade: the secondary key hides first, then both action
    // captions, so the Start/Sleep rows, pills, and Back stay reachable.
    if (chat_top_bar_->sizeHint().width() > detail_width) {
        selected_agent_key_->setVisible(false);
        if (chat_top_bar_->sizeHint().width() > detail_width) {
            start_status->setVisible(false);
            sleep_status->setVisible(false);
        }
    }
    // Measure the non-name row with the visible name text blanked (and its
    // width clamped to zero), so the full presentation title never double
    // counts into the row's size hint and collapses the derived allocation.
    presentation_name->setMinimumWidth(0);
    presentation_name->setMaximumWidth(0);
    presentation_name->setText(QString());
    // Measure every visible top-level widget. The vertically stacked
    // identity layout has no widget and is skipped automatically; the avatar
    // and actions all count against the title allocation.
    auto *top_layout = chat_top_bar_->layout();
    const auto margins = top_layout->contentsMargins();
    auto non_name_width = margins.left() + margins.right();
    auto visible_non_identity_items = 0;
    for (auto i = 0; i != top_layout->count(); ++i) {
        auto *item = top_layout->itemAt(i);
        auto *widget = item ? item->widget() : nullptr;
        if (!widget || !widget->isVisible()) continue;
        non_name_width += item->sizeHint().width();
        ++visible_non_identity_items;
    }
    non_name_width += top_layout->spacing() * visible_non_identity_items;
    // Allocate every remaining pixel to the identity name: its maximum width
    // is the actual detail width minus the row's natural non-name width, so
    // the name never hides or overlaps; the visible text is the right-elided
    // full title and the full identity stays on the property/description.
    const auto available = std::max(1, detail_width - non_name_width);
    presentation_name->setMinimumWidth(available);
    presentation_name->setMaximumWidth(available);
    presentation_name->setText(QFontMetrics(presentation_name->font())
        .elidedText(full, Qt::ElideRight, available));
}

// The one full-width composer row, re-entered on every recompute (and so on
// every real resize and selection change): the row always stretches the full
// detail width, while its own layout keeps one centered adaptive lane capped
// at 1600px -- outer horizontal margins of at least 12px, growing to split any
// width beyond 1600px -- with the input/Send row and the two status lines
// staying in that same lane. Object names, the input/Send row, and the status
// wording are never touched.
void NativeShell::update_composer_width(int detail_width) {
    if (!composer_) return;
    const auto outer = std::max(12, (detail_width - 1600) / 2 + 12);
    composer_->setMinimumWidth(0);
    composer_->setMaximumWidth(QWIDGETSIZE_MAX);
    composer_->layout()->setContentsMargins(outer, 10, outer, 8);
    if (auto *input = static_cast<Ui::InputField *>(
            window_->findChild<QObject *>("lingtai_composer_input"))) {
        if (auto *popup = window_->findChild<QListWidget *>(
                "lingtai_slash_command_popup");
                popup && popup->isVisible()) {
            position_slash_command_popup(popup, input);
        }
    }
}

void NativeShell::fit_kanban_page(int detail_width) {
    auto *scroll = window_->findChild<QScrollArea *>(
        "lingtai_agent_detail_scroll");
    auto width = detail_width;
    auto height = 0;
    if (scroll && scroll->viewport()) {
        if (scroll->viewport()->width() > 0) {
            width = scroll->viewport()->width();
        }
        height = scroll->viewport()->height();
    }
    if (!kanban_page_) return;
    kanban_page_->setMinimumWidth(0);
    kanban_page_->setMinimumHeight(0);
    if (current_detail_page_ == AgentDetailPage::kanban) {
        kanban_page_->setMaximumWidth(std::max(1, width));
        kanban_page_->setMaximumHeight(std::max(1, height));
    } else {
        kanban_page_->setMaximumWidth(QWIDGETSIZE_MAX);
        kanban_page_->setMaximumHeight(QWIDGETSIZE_MAX);
    }
}

// Telegram's OneColumn history-back path: the narrow detail returns to the
// roster, drops the selection, and hands keyboard focus to a usable roster
// row. Guarded by Back's own visibility, so the wide two-column layout
// (where Back is hidden) can never be deselected through this path.
void NativeShell::handle_detail_back() {
    if (!detail_back_button_ || !detail_back_button_->isVisible()) return;
    selection_state_.clear_agent_selection();
    bump_lifecycle_generation();
    reset_composer();
    pending_sleep_observation_.reset();
    pending_start_observation_.reset();
    render_roster();
    show_detail_page(AgentDetailPage::conversation);
    recompute_layout(window_->body()->width());
    agent_roster_->focus_row(std::nullopt);
}

// Telegram's chat-first page switch: the conversation is the default
// selected-Agent surface, and exactly one of Conversation / Presets shows
// when its page is selected, so only one content surface dominates at a time.
// The secondary pages and the read-only source-facts labels are direct layout
// children (their object/accessibility anchors never move); switching only
// flips the page visibility, and the source-facts labels stay hidden.
void NativeShell::show_detail_page(AgentDetailPage page) {
    if (detail_view_) {
        current_detail_page_ = page;
        detail_view_->set_page(page);
        return;
    }
    auto *conversation_heading = window_->findChild<QLabel *>(
        "lingtai_selected_agent_conversation_heading");
    auto *conversation = window_->findChild<QTextEdit *>(
        "lingtai_selected_agent_conversation");
    auto *composer = window_->findChild<Ui::RpWidget *>("lingtai_composer");
    auto *composer_status = window_->findChild<QLabel *>(
        "lingtai_composer_status");
    auto *conversation_state = window_->findChild<QLabel *>(
        "lingtai_selected_agent_conversation_state");
    auto *pages_host = window_->findChild<Ui::RpWidget *>(
        "lingtai_agent_pages_host");
    auto *pages_nav = window_->findChild<Ui::RpWidget *>(
        "lingtai_agent_pages_nav");
    if (!conversation_heading || !conversation || !composer || !composer_status
        || !conversation_state || !pages_host) {
        return;
    }
    const auto previous_page = current_detail_page_;
    current_detail_page_ = page;
    const auto on_conversation = page == AgentDetailPage::conversation;
    conversation_heading->setVisible(false);
    conversation->setVisible(on_conversation);
    composer->setVisible(on_conversation);
    composer_status->setVisible(on_conversation);
    conversation_state->setVisible(on_conversation);
    pages_host->setVisible(!on_conversation);
    if (pages_nav) {
        pages_nav->setVisible(page == AgentDetailPage::presets);
    }
    if (chat_top_bar_) {
        chat_top_bar_->setVisible(page == AgentDetailPage::conversation);
    }
    for (auto index = std::size_t{0}; index != secondary_pages_.size();
            ++index) {
        secondary_pages_[index]->setVisible(
            page == static_cast<AgentDetailPage>(index + 1));
    }
    for (auto index = std::size_t{0}; index != page_nav_buttons_.size();
            ++index) {
        page_nav_buttons_[index]->setChecked(
            page == static_cast<AgentDetailPage>(index));
        if (static_cast<AgentDetailPage>(index) == AgentDetailPage::presets) {
            page_nav_buttons_[index]->hide();
        }
    }
    if (page == AgentDetailPage::kanban) {
        render_kanban();
        if (kanban_page_) kanban_page_->setFocus(Qt::OtherFocusReason);
    }
    if (page == AgentDetailPage::presets) {
        render_agent_preset_summary();
    }
    const auto width = content_ && content_->width() > 0
        ? content_->width()
        : (window_ ? window_->body()->width() : 0);
    fit_kanban_page(width);
    if (previous_page != page && window_ && window_->body()
            && selection_state_.active_project()) {
        recompute_layout(window_->body()->width());
    }
}
// whatever `agents_` currently holds. Also reached through `render_roster()`
// from a click or a timer tick, but only before either overwrites the
// button/status with its own more specific write/observation text, so this
// never clobbers in-progress or terminal wording for the current selection.
void NativeShell::render_agent_sleep_status() {
    auto *button = window_->findChild<QPushButton *>(
        "lingtai_selected_agent_request_sleep");
    auto *status = window_->findChild<QLabel *>(
        "lingtai_selected_agent_sleep_status");
    if (!button || !status) return;

    const auto *item = selection_state_.active_project()
            && selection_state_.selected_agent_directory_key()
        ? selectable_item(agents_,
              *selection_state_.selected_agent_directory_key())
        : nullptr;
    const auto eligible = item && agent_sleep_eligible(*item);
    button->setEnabled(eligible);
    if (auto *row = window_->findChild<QWidget *>(
            "lingtai_selected_agent_sleep_row")) {
        row->setVisible(false);
    }
    status->setText(eligible
        ? QString()
        : QStringLiteral("Select a live Agent that is not already asleep."));
}

// The human's explicit click is the local product action. Rerun the sole
// `project_agents` projection once at the click boundary and use only the
// fresh exact row for the exact current selection -- never a cached one --
// before writing anything.
void NativeShell::handle_request_sleep() {
    auto *status = window_->findChild<QLabel *>(
        "lingtai_selected_agent_sleep_status");
    auto *button = window_->findChild<QPushButton *>(
        "lingtai_selected_agent_request_sleep");
    if (!status || !button) return;
    if (pending_sleep_observation_) return; // one observation at a time

    if (!selection_state_.active_project()
        || !selection_state_.selected_agent_directory_key()) {
        return;
    }
    const auto &attachment = *selection_state_.active_project();
    const auto key = *selection_state_.selected_agent_directory_key();

    agents_ = project_agents(attachment);
    render_roster();

    const auto *item = selectable_item(agents_, key);
    if (!item || !agent_sleep_eligible(*item)) {
        status->setText(
            QStringLiteral("Select a live Agent that is not already asleep."));
        button->setEnabled(false);
        return;
    }

    const auto baseline = capture_agent_sleep_event_baseline(attachment, key);
    const auto result = request_agent_sleep(attachment, key);
    if (result != AgentSleepRequestResult::requested) {
        status->setText(QStringLiteral("Sleep request not written."));
        return;
    }

    status->setText(QStringLiteral("Sleep requested."));
    button->setEnabled(false); // disable duplicate click while observing
    pending_sleep_observation_ = SleepObservation{
        .project_root = attachment.root(),
        .directory_key = key,
        .baseline = baseline,
        .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3),
    };
}

// Piggybacks on the existing one-second view timer: no new timer, thread, or
// watcher. Idle unless a click armed a pending observation for the exact
// current selection.
void NativeShell::tick_agent_sleep_observation() {
    if (!pending_sleep_observation_) return;
    if (!selection_state_.active_project()
        || selection_state_.active_project()->root()
            != pending_sleep_observation_->project_root
        || !selection_state_.selected_agent_directory_key()
        || *selection_state_.selected_agent_directory_key()
            != pending_sleep_observation_->directory_key) {
        // The target changed since the click; a late result must never
        // appear under a different selection.
        pending_sleep_observation_.reset();
        return;
    }

    const auto &attachment = *selection_state_.active_project();
    const auto key = pending_sleep_observation_->directory_key;
    const auto applied = observe_agent_sleep_received(
        attachment, key, pending_sleep_observation_->baseline);
    const auto expired = std::chrono::steady_clock::now()
        >= pending_sleep_observation_->deadline;
    if (!applied && !expired) return; // keep waiting within the window

    pending_sleep_observation_.reset();
    agents_ = project_agents(attachment);
    render_roster();

    auto *status = window_->findChild<QLabel *>(
        "lingtai_selected_agent_sleep_status");
    auto *button = window_->findChild<QPushButton *>(
        "lingtai_selected_agent_request_sleep");
    if (!status || !button) return;
    const auto *item = selectable_item(agents_, key);
    const auto state = item && item->identity && item->identity->state
        ? QString::fromStdString(*item->identity->state)
        : QString();
    if (applied) {
        const auto woke = state == QStringLiteral("active")
            || state == QStringLiteral("idle");
        status->setText(woke
            ? QStringLiteral("Sleep applied; Agent subsequently woke. "
                  "Current state: %1.")
                  .arg(state)
            : state.isEmpty()
                ? QStringLiteral("Sleep request applied.")
                : QStringLiteral("Sleep request applied. Current state: %1.")
                    .arg(state));
    } else {
        status->setText(state.isEmpty()
            ? QStringLiteral("Sleep requested; application not yet observed.")
            : QStringLiteral("Sleep requested; application not yet "
                  "observed. Current state: %1.")
                  .arg(state));
    }
    button->setEnabled(item && agent_sleep_eligible(*item));
}

// Reflects only the eligibility of the exact current selection against
// whatever `agents_` currently holds, mirroring the button/enabled half of
// render_agent_sleep_status() -- but deliberately *not* the status-text
// half. This is called every second from the idle ambient timer branch
// (with no click or resolution involved) purely to keep the button's
// visible/enabled state honest against external drift (e.g. an ordinary-
// message wake with no reselection); it must never touch the status label,
// or it would erase a just-shown "Starting Agent...", "Agent is online.",
// or failure message on the very next tick after it was set. Callers that
// genuinely need a fresh status slate (selection change, project open, a
// click/tick resolution) clear the label themselves at that specific
// point, not through this function.
void NativeShell::render_agent_start_status() {
    auto *button = window_->findChild<QPushButton *>(
        "lingtai_selected_agent_start_agent");
    if (!button) return;

    const auto *item = selection_state_.active_project()
            && selection_state_.selected_agent_directory_key()
        ? selectable_item(agents_,
              *selection_state_.selected_agent_directory_key())
        : nullptr;
    const auto eligible = item && agent_start_eligible(*item);
    button->setVisible(false);
    button->setEnabled(eligible);
    if (auto *row = window_->findChild<QWidget *>(
            "lingtai_selected_agent_start_row")) {
        row->setVisible(false);
    }
}

// The human's explicit click is the local product action. Rerun the sole
// `project_agents` projection once at the click boundary and use only the
// fresh exact row for the exact current selection -- never a cached one --
// before starting anything. Request sleep is never separately disabled
// here: it already requires `alive` presence, which a start-eligible
// (stale/missing) row can never have at this exact instant.
void NativeShell::handle_start_agent() {
    auto *status = window_->findChild<QLabel *>(
        "lingtai_selected_agent_start_status");
    auto *button = window_->findChild<QPushButton *>(
        "lingtai_selected_agent_start_agent");
    if (!status || !button) return;
    if (pending_start_observation_) return; // one observation at a time

    if (!selection_state_.active_project()
        || !selection_state_.selected_agent_directory_key()) {
        return;
    }
    const auto &attachment = *selection_state_.active_project();
    const auto key = *selection_state_.selected_agent_directory_key();

    agents_ = project_agents(attachment);
    render_roster();

    const auto *item = selectable_item(agents_, key);
    if (!item || !agent_start_eligible(*item)) {
        if (item && item->presence == AgentPresenceKind::alive) {
            status->setText(QStringLiteral("Agent is already online."));
        }
        return; // render_roster() above already reflects fresh eligibility
    }

    const auto result =
        start_agent(attachment, key, agent_start_fallback_python_);
    if (result != AgentLaunchResult::started) {
        status->setText(QStringLiteral(
            "Could not start Agent. See %1/logs/agent.log.")
            .arg(path_text(attachment.root() / ".lingtai" / key)));
        return;
    }

    status->setText(QStringLiteral("Starting Agent..."));
    button->setEnabled(false); // disable duplicate click while observing
    pending_start_observation_ = StartObservation{
        .project_root = attachment.root(),
        .directory_key = key,
        .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10),
    };
}

// Piggybacks on the existing one-second view timer: no new timer, thread, or
// watcher, and no second heartbeat parser. Idle unless a click armed a
// pending observation for the exact current selection. Success is proven
// solely by the sole `project_agents` projection reporting this exact
// selection `alive`; `agent_start_eligible()`'s narrowed stale/missing gate
// is what makes that transition trustworthy without a separate timestamp
// baseline.
void NativeShell::tick_agent_start_observation() {
    if (!pending_start_observation_) return;
    if (!selection_state_.active_project()
        || selection_state_.active_project()->root()
            != pending_start_observation_->project_root
        || !selection_state_.selected_agent_directory_key()
        || *selection_state_.selected_agent_directory_key()
            != pending_start_observation_->directory_key) {
        // The target changed since the click; a late result must never
        // appear under a different selection.
        pending_start_observation_.reset();
        return;
    }

    const auto &attachment = *selection_state_.active_project();
    const auto key = pending_start_observation_->directory_key;
    agents_ = project_agents(attachment);
    const auto *item = selectable_item(agents_, key);
    const auto online = item && item->presence == AgentPresenceKind::alive;
    const auto expired = std::chrono::steady_clock::now()
        >= pending_start_observation_->deadline;
    if (!online && !expired) return; // keep waiting within the window

    pending_start_observation_.reset();
    render_roster();

    auto *status = window_->findChild<QLabel *>(
        "lingtai_selected_agent_start_status");
    if (!status) return;
    status->setText(online
        ? QStringLiteral("Agent is online.")
        : QStringLiteral("Agent did not come online. See %1/logs/agent.log.")
              .arg(path_text(attachment.root() / ".lingtai" / key)));
}

ProjectOpenOutcome NativeShell::show_open_error(
        ProjectPathFailure failure,
        std::string message) {
    auto *label = find_ui_child<Ui::FlatLabel>(
        *window_, "lingtai_project_open_error");
    const auto message_text = QString::fromStdString(message);
    label->setText(message_text);
    label->setAccessibleName(message_text);
    refresh_route();
    recompute_layout(window_->body()->width());
    open_error_surface_->show();
    return {
        .disposition = ProjectOpenDisposition::failed,
        .failure = failure,
    };
}

void NativeShell::refresh_route() {
    const auto project_active = selection_state_.active_project().has_value();
    const auto setup_active = in_project_setup();
    empty_route_->setVisible(!project_active && !setup_active);
    project_route_->setVisible(project_active && !setup_active);
    if (auto *title = window_->findChild<QLabel *>("lingtai_product_title")) {
        title->setVisible(!project_active && !setup_active);
    }
    if (auto *purpose = window_->findChild<QLabel *>(
            "lingtai_product_purpose")) {
        purpose->setVisible(!project_active && !setup_active);
    }
}

} // namespace lingtai::desktop
