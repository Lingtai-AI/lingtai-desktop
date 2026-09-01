#include "agent_detail_view.h"
#include "attachment_thumbnail.h"
#include "composer_spellcheck.h"

#include "conversation_session.h"
#include "kanban_page.h"

#include "slash_command.h"

#include "base/event_filter.h"

#include "ui/palette_action_button.h"
#include "ui/palette_icon_button.h"
#include "ui/palette_surface.h"
#include "ui/conversation_surface.h"
#include "ui/preset_row_delegate.h"
#include "ui/selected_agent_avatar.h"
#include "ui/slash_command_card.h"

#include "preset_catalog_presentation.h"

#include "ui/widgets/fields/input_field.h"

#include <algorithm>
#include <QtCore/QMargins>
#include <QtCore/QPoint>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QFont>
#include <QtGui/QFontMetrics>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPen>
#include <QtGui/QPalette>
#include <QtGui/QResizeEvent>
#include <QtGui/QTextBlock>
#include <QtGui/QTextDocument>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

#include <rpl/rpl.h>

#include "ui/widgets/shadow.h"

#include <styles/palette.h>
#include <styles/style_widgets.h>

namespace lingtai::desktop {
namespace {

constexpr auto kChatTopBarHeight = 64;
constexpr auto kHeaderStatusDotSize = 6;

// Sidebar-matching status disc for the conversation header secondary line.
class HeaderStatusDot final : public QWidget {
public:
    explicit HeaderStatusDot(QWidget *parent)
    : QWidget(parent) {
        setFixedSize(kHeaderStatusDotSize, kHeaderStatusDotSize);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        const auto color = property("lingtai_status_color").value<QColor>();
        if (!color.isValid()) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QRectF(rect()));
    }
};

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
    // Keep consistent height-for-width behavior for the detail column.
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

[[nodiscard]] QString human_file_size(std::uint64_t bytes) {
    static constexpr std::array<const char *, 4> units = {
        "B", "KB", "MB", "GB"};
    auto value = static_cast<double>(bytes);
    auto unit = std::size_t{0};
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    const auto precision = unit == 0 || value >= 10.0 ? 0 : 1;
    return QStringLiteral("%1 %2")
        .arg(value, 0, 'f', precision)
        .arg(QString::fromLatin1(units[unit]));
}

[[nodiscard]] QString attachment_display_name(const std::filesystem::path &path) {
    const auto filename = path.filename().string();
    return QString::fromStdString(filename.empty() ? path.string() : filename);
}

[[nodiscard]] QString selection_rejection_text(
        const RejectedAttachment &rejection) {
    const auto name = attachment_display_name(rejection.input_path);
    switch (rejection.reason) {
    case AttachmentRejectionReason::missing:
        return QStringLiteral("%1 is no longer available.").arg(name);
    case AttachmentRejectionReason::not_regular:
        return QStringLiteral("%1 must be a regular file.").arg(name);
    case AttachmentRejectionReason::unreadable:
        return QStringLiteral("%1 can’t be read. Check its permissions.").arg(name);
    case AttachmentRejectionReason::per_file_limit:
        return QStringLiteral("%1 exceeds the 25 MB file limit.").arg(name);
    case AttachmentRejectionReason::total_limit:
        return QStringLiteral("%1 would exceed the 100 MB attachment limit.").arg(name);
    case AttachmentRejectionReason::duplicate:
        return QStringLiteral("%1 is already attached.").arg(name);
    case AttachmentRejectionReason::local_failure:
        return QStringLiteral("%1 couldn’t be inspected.").arg(name);
    }
    return QStringLiteral("%1 couldn’t be attached.").arg(name);
}

// Composer growth: one centered line, two lines with the first nudged up, and
// a themed scrollbar once content exceeds two visible lines.
constexpr auto kComposerOneLineHeight = 40;
constexpr auto kComposerVisibleLines = 2;
constexpr auto kComposerTwoLinePad = 4;

[[nodiscard]] QString css_rgba(const QColor &color) {
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

void apply_composer_scrollbar_style(QTextEdit *edit) {
    if (!edit) return;
    // Match Telegram's defaultScrollArea handle colors (scrollBarBg / scrollBg)
    // with a slim overlay-style bar so the composer tracks the rest of the app.
    edit->setStyleSheet(QStringLiteral(
        "QTextEdit { background: transparent; }"
        "QScrollBar:vertical {"
        "  background: %1;"
        "  width: 8px;"
        "  margin: 4px 1px 4px 0;"
        "  border-radius: 4px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: %2;"
        "  min-height: 20px;"
        "  border-radius: 4px;"
        "}"
        "QScrollBar::handle:vertical:hover { background: %3; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "  height: 0; width: 0; border: none; background: transparent;"
        "}"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        "  background: transparent;"
        "}")
        .arg(css_rgba(st::scrollBg->c),
            css_rgba(st::scrollBarBg->c),
            css_rgba(st::scrollBarBgOver->c)));
}

[[nodiscard]] int composer_visual_line_count(Ui::InputField &field) {
    const auto document = field.document();
    static_cast<void>(document->documentLayout()->documentSize());
    auto layout_lines = 0;
    for (auto block = document->begin(); block.isValid(); block = block.next()) {
        if (const auto *layout = block.layout()) {
            layout_lines += std::max(1, layout->lineCount());
        } else {
            ++layout_lines;
        }
    }
    // Prefer the richer of laid-out wrap count and explicit newline count so a
    // just-inserted Shift+Enter is visible before the next layout pass.
    const auto newline_lines = static_cast<int>(
        field.getLastText().count(QChar('\n')) + 1);
    return std::max({1, layout_lines, newline_lines});
}

[[nodiscard]] int composer_two_line_height(const QFontMetrics &metrics) {
    return kComposerVisibleLines * metrics.lineSpacing()
        + 2 * kComposerTwoLinePad;
}

void sync_composer_multiline_geometry(Ui::InputField &field) {
    const auto metrics = QFontMetrics(field.font());
    const auto line = std::max(1, metrics.lineSpacing());
    const auto lines = composer_visual_line_count(field);
    const auto two_line_height = std::max(
        kComposerOneLineHeight + line,
        composer_two_line_height(metrics));
    const auto edit = field.rawTextEdit();

    if (lines <= 1) {
        const auto pad = std::max(0, (kComposerOneLineHeight - line) / 2);
        field.setAdditionalMargins(QMargins(0, pad, 0, pad));
        field.setMinHeight(kComposerOneLineHeight);
        field.setMaxHeight(kComposerOneLineHeight);
        edit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        edit->verticalScrollBar()->setValue(0);
        return;
    }

    // Two or more lines: drop the one-line centering pad so the first line
    // moves up and two rows fit inside the capsule without clipping. Lock
    // min=max so InputField's content-height autoupdate cannot shrink the
    // capsule on two lines and then grow it again on the third.
    field.setAdditionalMargins(QMargins(
        0, kComposerTwoLinePad, 0, kComposerTwoLinePad));
    field.setMinHeight(two_line_height);
    field.setMaxHeight(two_line_height);
    if (field.height() != two_line_height) {
        field.resize(field.width(), two_line_height);
    }
    if (lines > kComposerVisibleLines) {
        edit->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        edit->verticalScrollBar()->setValue(
            edit->verticalScrollBar()->maximum());
    } else {
        edit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        edit->verticalScrollBar()->setValue(0);
    }
}

class ComposerControlsBorder final : public QWidget {
public:
    explicit ComposerControlsBorder(QWidget *parent)
        : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto outline = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        // Cap the corner radius so a growing multi-line field stays a rounded
        // capsule instead of becoming a full stadium.
        const auto radius = std::min(26.0, outline.height() / 2.0);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(st::shadowFg->c, 1.0));
        painter.drawRoundedRect(outline, radius, radius);
    }
};

class ComposerControls final : public Ui::RpWidget {
public:
    explicit ComposerControls(QWidget *parent)
        : Ui::RpWidget(parent)
        , border_(new ComposerControlsBorder(this)) {
        // Stroke must stay above the Send circle so multiline growth never
        // punches a hole through the capsule outline.
        border_->raise();
    }

    void raise_border() {
        if (border_) {
            border_->raise();
        }
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        Ui::RpWidget::resizeEvent(event);
        border_->setGeometry(rect());
        border_->raise();
    }

    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto outline = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const auto radius = std::min(26.0, outline.height() / 2.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(st::windowBg->c);
        painter.drawRoundedRect(outline, radius, radius);
    }

private:
    ComposerControlsBorder *border_ = nullptr;
};

// A flat vector paperclip avoids platform-framed icons while keeping the
// expected 40px lane geometry.
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
        painter.translate(
            button_center - clip_center + QPointF(0.25, -0.25));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(st::windowSubTextFg->c, 1.8, Qt::SolidLine,
            Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(clip);
    }
};

// Keep the Telegram-like ripple surface, but paint the Send arrow as vector
// ink so its visible bounds share the circle center.
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

// Compact selected-Agent page navigation tab.
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
            // Headless/offscreen font metrics can crash on some Qt builds
            // where font database warmup is incomplete. The exact pixel width
            // is not critical for our widget-level page-switch contract, so
            // prefer a conservative fixed width in those platforms.
            const auto platform = QGuiApplication::platformName().toLower();
            if (platform.contains(QStringLiteral("offscreen"))
                    || platform.contains(QStringLiteral("minimal"))) {
                setMinimumWidth(170);
            } else {
                setMinimumWidth(QFontMetrics(font).horizontalAdvance(text) + 12);
            }
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

struct DashboardSection {
    Ui::RpWidget *owner = nullptr;
    QLabel *heading = nullptr;
    QTreeWidget *surface = nullptr;
    QLabel *state = nullptr;
};

constexpr auto kDashboardSectionSurfaceHeight = 300;

DashboardSection add_dashboard_section(
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
    surface->setHeaderLabels(
        {QStringLiteral("Preset"),
            QStringLiteral("Provider · Model"),
            QStringLiteral("Capabilities")});
    configure_preset_table(surface);
    surface->setMinimumWidth(0);
    surface->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    surface->setMinimumHeight(kDashboardSectionSurfaceHeight);
    layout->addWidget(surface, 1);

    auto *state = make_label(
        owner, QString(),
        (base + QStringLiteral("_state")).toUtf8().constData(), 10);
    state->setAccessibleName(surface_accessible_name
        + QStringLiteral(" state"));
    layout->addWidget(state);

    auto *separator = new Ui::PlainShadow(owner);
    separator->setObjectName(
        (base + QStringLiteral("_separator")).toUtf8().constData());
    separator->setFixedHeight(1);
    layout->addWidget(separator);

    detail_layout->addWidget(owner, 1);
    return {owner, heading, surface, state};
}

void apply_one_preset_catalog_chrome(
    QTreeWidget *table,
    const QPalette &root_palette) {
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
        .arg(tokens.surface.name(QColor::HexRgb).toUpper(), border,
            tokens.selected_row.name(QColor::HexRgb).toUpper(),
            tokens.header.name(QColor::HexRgb).toUpper(),
            tokens.section_text.name(QColor::HexRgb).toUpper()));
}

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

void add_preset_catalog_row(
    QTreeWidget *table,
    const PresetCatalogRow &row,
    int index) {
    const auto name = QString::fromStdString(row.entry.name);
    const auto tags = preset_capability_tags(row.has_vision, row.has_tools);
    auto *item = new QTreeWidgetItem(table);
    item->setData(0, Qt::UserRole, index);
    item->setData(0, kPresetSummaryRole, row.summary);
    item->setData(0, kPresetCapabilitiesRole, tags);
    item->setData(0, Qt::UserRole + 8,
        QString::fromStdString(row.entry.path));
    item->setText(0, name);
    item->setText(1, row.provider_model);
    item->setText(2, tags.join(QStringLiteral(", ")));
    item->setToolTip(0, row.summary);
    item->setToolTip(1, row.provider_model);
}

void hide_slash_command_popup(QWidget *root) {
    if (!root) return;
    auto *popup = root->findChild<QListWidget *>(
        QStringLiteral("lingtai_slash_command_popup"));
    if (!popup) return;
    if (auto *card = popup->parentWidget()) {
        card->hide();
    } else {
        popup->hide();
    }
}

void apply_slash_popup_choice(Ui::InputField *input, QListWidget *popup) {
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

void position_slash_command_popup(QListWidget *popup, Ui::InputField *input) {
    auto *card = popup ? popup->parentWidget() : nullptr;
    if (!popup || !input || !card || !card->parentWidget()) return;
    const auto origin = input->mapTo(card->parentWidget(), QPoint(0, 0));
    const auto width = std::clamp(input->width() + 2 * kSlashCardShadow,
        340, 460);
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

void refresh_slash_command_popup(QWidget *root, Ui::InputField *input) {
    if (!root || !input) return;
    auto *popup = root->findChild<QListWidget *>(
        QStringLiteral("lingtai_slash_command_popup"));
    if (!popup) return;

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
        item->setData(Qt::UserRole + 1,
            QString::fromUtf8(offer.description));
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

} // namespace

void fit_selected_agent_chat_top_bar(QWidget *top_bar, int detail_width) {
    if (!top_bar || detail_width <= 0) return;
    auto *presentation_name = top_bar->findChild<QLabel *>(
        "lingtai_selected_agent_presentation_name");
    auto *status_key = top_bar->findChild<QLabel *>(
        "lingtai_selected_agent_key");
    auto *status_row = top_bar->findChild<QWidget *>(
        "lingtai_selected_agent_status_row");
    auto *identity = top_bar->findChild<QWidget *>(
        "lingtai_selected_agent_identity");
    auto *start_status = top_bar->findChild<QLabel *>(
        "lingtai_selected_agent_start_status");
    auto *sleep_status = top_bar->findChild<QLabel *>(
        "lingtai_selected_agent_sleep_status");
    if (!presentation_name || !status_key || !status_row || !identity
            || !start_status || !sleep_status) {
        return;
    }
    const auto full_title = presentation_name->property(
        "lingtai_full_text").toString();
    if (full_title.isEmpty()) return;
    auto full_status = status_key->property("lingtai_full_text").toString();
    if (full_status.isEmpty()) {
        full_status = status_key->text();
        if (!full_status.isEmpty()) {
            status_key->setProperty("lingtai_full_text", full_status);
        }
    }

    // Status stays under the name at every width; only action captions may
    // hide when they cannot fit their own bounded label.
    status_row->setVisible(true);
    status_key->setVisible(true);
    start_status->setVisible(true);
    sleep_status->setVisible(true);
    const auto caption_fits = [](QLabel *status) {
        if (status->text().isEmpty()) return true;
        return QFontMetrics(status->font()).horizontalAdvance(status->text())
            <= status->maximumWidth();
    };
    if (!caption_fits(start_status)) start_status->setVisible(false);
    if (!caption_fits(sleep_status)) sleep_status->setVisible(false);

    auto *top_layout = top_bar->layout();
    if (!top_layout) return;

    // Blank identity text while measuring sibling chrome so the name/status
    // strings never inflate the top-bar size hint.
    identity->setMinimumWidth(0);
    identity->setMaximumWidth(QWIDGETSIZE_MAX);
    presentation_name->setText(QString());
    status_key->setText(QString());

    const auto margins = top_layout->contentsMargins();
    auto non_identity_width = margins.left() + margins.right();
    auto visible_count = 0;
    for (auto i = 0; i != top_layout->count(); ++i) {
        auto *item = top_layout->itemAt(i);
        auto *widget = item ? item->widget() : nullptr;
        if (!widget || !widget->isVisible()) continue;
        ++visible_count;
        if (widget == identity) continue;
        non_identity_width += widget->sizeHint().width();
    }
    if (visible_count > 1) {
        non_identity_width += top_layout->spacing() * (visible_count - 1);
    }

    const auto available = std::max(1, detail_width - non_identity_width);
    identity->setMaximumWidth(available);

    auto *status_dot = top_bar->findChild<QWidget *>(
        "lingtai_selected_agent_status_dot");
    const auto dot_reserve = status_dot && status_dot->isVisible()
        ? std::max(status_dot->width(), status_dot->sizeHint().width())
            + (status_row->layout() ? status_row->layout()->spacing() : 6)
        : 0;
    const auto status_text_width = std::max(1, available - dot_reserve);
    presentation_name->setText(QFontMetrics(presentation_name->font())
        .elidedText(full_title, Qt::ElideRight, available));
    status_key->setText(QFontMetrics(status_key->font())
        .elidedText(full_status, Qt::ElideRight, status_text_width));
}

AgentDetailView::AgentDetailView(
    RuntimeOptions runtime_options,
    QScrollArea *outer_scroll,
    QWidget *parent)
    : Ui::RpWidget(parent ? parent : outer_scroll)
    , runtime_options_(runtime_options)
    , outer_scroll_(outer_scroll) {
    // Keep the same semantic identity anchors as existing tests expect.
    setObjectName("lingtai_agent_detail");
    setAccessibleName(QStringLiteral("Selected Agent detail"));
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto *detail_layout = new QVBoxLayout(this);
    detail_layout->setContentsMargins(0, 0, 0, 0);
    detail_layout->setSpacing(4);

    if (runtime_options_.deterministic_ui) {
        // Minimal widget tree for headless/widget-level page switching
        // tests: avoid constructing composer/slash, preset catalog, and
        // kanban, which pull in additional font/selection machinery.

        // Conversation anchor heading (tests expect this to remain hidden).
        conversation_heading_ = make_label(
            this, QStringLiteral("Conversation"),
            "lingtai_selected_agent_conversation_heading", 11,
            QFont::DemiBold);
        detail_layout->addWidget(conversation_heading_);
        conversation_heading_->hide();

        // Secondary page navigation (only one tab visible at a time).
        auto *pages_nav = new PaletteSurface(this, st::windowBg);
        pages_nav->setObjectName("lingtai_agent_pages_nav");
        pages_nav->setAccessibleName(
            QStringLiteral("Selected Agent pages"));

        auto *pages_nav_layout = new QHBoxLayout(pages_nav);
        pages_nav_layout->setContentsMargins(12, 8, 12, 4);
        pages_nav_layout->setSpacing(4);

        auto *nav_conversation = new QPushButton(QStringLiteral("←  Conversation"), pages_nav);
        nav_conversation->setObjectName(
            "lingtai_agent_page_nav_conversation");
        nav_conversation->setCheckable(true);
        nav_conversation->setFixedHeight(28);

        auto *nav_presets = new QPushButton(QStringLiteral("Presets"), pages_nav);
        nav_presets->setObjectName("lingtai_agent_page_nav_presets");
        nav_presets->setCheckable(true);
        nav_presets->setFixedHeight(28);
        nav_presets->hide();

        pages_nav_ = pages_nav;
        page_nav_buttons_ = {nav_conversation, nav_presets};

        pages_nav_layout->addWidget(nav_conversation, 0);
        pages_nav_layout->addStretch(1);
        detail_layout->addWidget(pages_nav);
        pages_nav->hide();

        // Conversation surface placeholder.
        auto *conversation = new ConversationSurface(this);
        conversation->setObjectName("lingtai_selected_agent_conversation");
        conversation_surface_ = conversation;
        connect(conversation, &ConversationSurface::verbose_level_changed,
            this, [this](ConversationVerboseLevel level) {
                refresh_conversation_state_hint();
                emit conversation_verbose_changed(level);
            });
        connect(conversation, &ConversationSurface::attachment_action_requested,
            this, &AgentDetailView::attachment_action_requested);
        detail_layout->addWidget(conversation, 1);

        return;
    }

    // The old "Selected Agent" header heading stays as a hidden semantic
    // anchor: the chat top bar below now owns the selected-Agent identity.
    auto *detail_heading = make_label(
        this, QStringLiteral("Selected Agent"),
        "lingtai_agent_detail_heading", 14, QFont::DemiBold);
    detail_heading->hide();
    detail_layout->addWidget(detail_heading);

    // One Telegram-like chat top bar: selected Agent identity and the same
    // Role · Status secondary line the Sidebar paints under each name.
    auto *top_bar = new PaletteSurface(this, st::windowBg);
    top_bar->setObjectName("lingtai_chat_top_bar");
    top_bar->setAccessibleName(QStringLiteral("Selected Agent"));
    top_bar->setMinimumWidth(0);
    top_bar->setFixedHeight(kChatTopBarHeight);

    auto *top_bar_layout = new QHBoxLayout(top_bar);
    top_bar_layout->setContentsMargins(12, 8, 12, 8);
    top_bar_layout->setSpacing(8);

    auto *selected_avatar = new SelectedAgentAvatar(top_bar);
    selected_avatar->setObjectName("lingtai_selected_agent_avatar");
    selected_avatar->hide();
    top_bar_layout->addWidget(selected_avatar);

    // One identity column widget owns both title and status so resize fit
    // allocates width to the column and elides each line — never hides the
    // Sidebar-matching status row as if it were a horizontal competitor.
    auto *identity = new QWidget(top_bar);
    identity->setObjectName("lingtai_selected_agent_identity");
    identity->setMinimumWidth(0);
    identity->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *identity_column = new QVBoxLayout(identity);
    identity_column->setContentsMargins(0, 0, 0, 0);
    identity_column->setSpacing(2);

    auto *presentation_name = make_label(
        identity, QString(), "lingtai_selected_agent_presentation_name", 16,
        QFont::DemiBold);
    presentation_name->setAccessibleName(
        QStringLiteral("Selected Agent presentation name"));
    presentation_name->setWordWrap(false);
    presentation_name->setMinimumWidth(0);
    presentation_name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    identity_column->addWidget(presentation_name);

    auto *status_row = new QWidget(identity);
    status_row->setObjectName("lingtai_selected_agent_status_row");
    status_row->setAccessibleName(QStringLiteral("Selected Agent status"));
    status_row->setMinimumWidth(0);
    status_row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto *status_row_layout = new QHBoxLayout(status_row);
    status_row_layout->setContentsMargins(0, 0, 0, 0);
    status_row_layout->setSpacing(6);
    auto *status_dot = new HeaderStatusDot(status_row);
    status_dot->setObjectName("lingtai_selected_agent_status_dot");
    status_dot->setAccessibleName(QStringLiteral("Selected Agent status dot"));
    status_row_layout->addWidget(status_dot, 0, Qt::AlignVCenter);

    auto *detail_key = make_label(
        status_row, QString(), "lingtai_selected_agent_key", 12);
    detail_key->setAccessibleName(
        QStringLiteral("Selected Agent status and role"));
    detail_key->setWordWrap(false);
    detail_key->setMinimumWidth(0);
    detail_key->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto key_palette = detail_key->palette();
    key_palette.setColor(QPalette::WindowText, st::windowSubTextFg->c);
    detail_key->setPalette(key_palette);
    status_row_layout->addWidget(detail_key, 1);
    identity_column->addWidget(status_row);
    top_bar_layout->addWidget(identity, 1);

    // Narrow-mode palette-owned Back control.
    detail_back_button_ = new PaletteActionButton(
        top_bar, QStringLiteral("Back"));
    detail_back_button_->setObjectName("lingtai_agent_detail_back");
    detail_back_button_->setAccessibleName(QStringLiteral("Back to Agent list"));
    top_bar_layout->addWidget(detail_back_button_);

    // Start row (hidden by default; enabled/visible is decided in later
    // render/update steps).
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

    start_row_layout->addWidget(start_button);

    auto *start_status = make_label(
        start_row, QString(), "lingtai_selected_agent_start_status", 10);
    start_status->setAccessibleName(QStringLiteral("Start Agent status"));
    start_status->setMaximumWidth(160);
    start_status->setMaximumHeight(12);
    start_status->setWordWrap(false);
    start_row_layout->addWidget(start_status);

    start_row->setMinimumHeight(start_row->sizeHint().height());
    start_button->setVisible(false);
    start_row->hide();
    top_bar_layout->addWidget(start_row);

    // Sleep row.
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

    conversation_detail_button_ = new PaletteActionButton(
        top_bar, QStringLiteral("Details"));
    conversation_detail_button_->setObjectName("lingtai_conversation_detail_toggle");
    conversation_detail_button_->setAccessibleName(
        QStringLiteral("Conversation LLM details"));
    conversation_detail_button_->setAccessibleDescription(QStringLiteral(
        "Cycles LLM detail levels: off, compact thinking and tools, then "
        "extended tool bodies."));
    conversation_detail_button_->hide();
    conversation_detail_button_->setMinimumWidth(
        conversation_detail_button_->sizeHint().width());
    top_bar_layout->addWidget(
        conversation_detail_button_, 0, Qt::AlignVCenter | Qt::AlignRight);
    QObject::connect(
        conversation_detail_button_,
        &QPushButton::clicked,
        this,
        [this] {
            if (conversation_surface_) {
                conversation_surface_->cycle_verbose_level();
            }
        });

    detail_layout->addWidget(top_bar);

    // Retained once for the whole view lifecycle: responsive fit uses them.
    chat_top_bar_ = top_bar;
    selected_agent_key_ = detail_key;

    // Page navigation: only Conversation is visible as a tab.
    auto *pages_nav = new PaletteSurface(this, st::windowBg);
    pages_nav->setObjectName("lingtai_agent_pages_nav");
    pages_nav->setAccessibleName(QStringLiteral("Selected Agent pages"));

    auto *pages_nav_layout = new QHBoxLayout(pages_nav);
    pages_nav_layout->setContentsMargins(12, 8, 12, 4);
    pages_nav_layout->setSpacing(4);

    const auto nav_specs = std::array<std::pair<const char *, const char *>, 2>{{
        std::pair{"lingtai_agent_page_nav_conversation", "←  Conversation"},
        std::pair{"lingtai_agent_page_nav_presets", "Presets"},
    }};

    pages_nav_ = pages_nav;
    for (auto index = std::size_t{0}; index != nav_specs.size(); ++index) {
        const auto &[object_name, text] = nav_specs[index];
        const auto page = static_cast<AgentDetailPage>(index);
        auto *button = new PageNavButton(pages_nav, QString::fromUtf8(text));
        button->setObjectName(object_name);
        button->setAccessibleName(
            page == AgentDetailPage::conversation
                ? QStringLiteral("Conversation")
                : QString::fromUtf8(text));
        if (page == AgentDetailPage::presets) {
            button->hide();
        } else {
            pages_nav_layout->addWidget(button, 0);
        }
        page_nav_buttons_.push_back(button);
    }
    pages_nav_layout->addStretch(1);
    detail_layout->addWidget(pages_nav);
    // Initial page is Conversation, so the nav ("← Conversation" / "Presets")
    // must be hidden until the user enters secondary pages (Presets/Kanban).
    pages_nav->hide();

    // Conversation page anchor heading (hidden by show_detail_page logic).
    conversation_heading_ = make_label(
        this, QStringLiteral("Conversation"),
        "lingtai_selected_agent_conversation_heading", 11, QFont::DemiBold);
    detail_layout->addWidget(conversation_heading_);
    conversation_heading_->hide();

    auto *conversation = new ConversationSurface(this);
    conversation->setObjectName("lingtai_selected_agent_conversation");
    conversation->setAccessibleName(
        QStringLiteral("Selected Agent conversation"));
    conversation->setAccessibleDescription(QStringLiteral(
        "The current direct conversation with the selected Agent, shown "
        "read-only as plain text."));
    conversation->setMinimumHeight(180);
    conversation->setMinimumWidth(0);
    conversation->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    conversation_surface_ = conversation;
    connect(conversation, &ConversationSurface::verbose_level_changed,
        this, [this](ConversationVerboseLevel level) {
            refresh_conversation_state_hint();
            emit conversation_verbose_changed(level);
        });
    connect(conversation, &ConversationSurface::attachment_action_requested,
        this, &AgentDetailView::attachment_action_requested);
    detail_layout->addWidget(conversation, 1);

    // Composer lane.
    auto *composer = new PaletteSurface(this, st::windowBg);
    composer_ = composer;
    composer->setObjectName("lingtai_composer");
    composer->setAccessibleName(QStringLiteral("Send a message"));

    auto *composer_layout = new QVBoxLayout(composer);
    composer_layout->setContentsMargins(0, 0, 0, 0);
    composer_layout->setSpacing(4);

    composer_attachment_tray_ = new QWidget(composer);
    composer_attachment_tray_->setObjectName(
        "lingtai_composer_attachment_tray");
    composer_attachment_tray_->setAccessibleName(
        QStringLiteral("Pending attachments"));
    composer_attachment_tray_->setAccessibleDescription(QStringLiteral(
        "Files that will be sent with the next ordinary message."));
    composer_attachment_tray_->setMinimumWidth(0);
    composer_attachment_layout_ = new QGridLayout(composer_attachment_tray_);
    composer_attachment_layout_->setContentsMargins(0, 0, 0, 2);
    composer_attachment_layout_->setHorizontalSpacing(6);
    composer_attachment_layout_->setVerticalSpacing(6);
    composer_attachment_layout_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    composer_attachment_tray_->hide();
    composer_layout->addWidget(composer_attachment_tray_);

    auto *composer_controls = new ComposerControls(composer);
    composer_controls->setObjectName("lingtai_composer_controls");
    composer_controls->setAccessibleName(QStringLiteral("Message controls"));
    composer_controls->setMinimumHeight(52);

    auto *composer_action_row = new QHBoxLayout(composer_controls);
    composer_action_row->setContentsMargins(6, 4, 6, 4);
    composer_action_row->setSpacing(4);

    static const auto borderless_composer_input = [] {
        auto result = st::defaultInputField;
        result.border = 0;
        result.borderActive = 0;
        // Vertical padding is owned by sync_composer_multiline_geometry():
        // equal pads center one line; smaller pads let two lines fit cleanly.
        result.textMargins = QMargins(0, 0, 0, 0);
        result.placeholderMargins = QMargins();
        result.placeholderScale = 0.0;
        result.placeholderShift = 0;
        return result;
    }();

    // MultiLine keeps Enter-to-send while Shift+Enter inserts a newline.
    // Geometry: 1 line centered, 2 lines grown, 3+ capped with a scrollbar.
    auto *composer_input = new Ui::InputField(
        composer_controls,
        borderless_composer_input,
        Ui::InputField::Mode::MultiLine,
        rpl::single(QStringLiteral("Message…")));
    composer_input_ = composer_input;
    composer_input->setObjectName("lingtai_composer_input");
    composer_input->setAccessibleName(QStringLiteral("Message"));
    composer_input->setSubmitSettings(Ui::InputField::SubmitSettings::Enter);
    composer_input->setMinHeight(kComposerOneLineHeight);
    composer_input->setMaxHeight(kComposerOneLineHeight);
    composer_input->setEnabled(false);
    composer_spell_service_ = make_platform_composer_spell_service();
    add_composer_spell_context_menu_hook(composer_input, [this] {
        return composer_spell_service_;
    });
    apply_composer_scrollbar_style(composer_input->rawTextEdit());
    sync_composer_multiline_geometry(*composer_input);

    auto *attachment_button = new ComposerAttachmentButton(composer_controls);
    composer_attachment_button_ = attachment_button;
    attachment_button->setObjectName("lingtai_composer_attachment_button");
    attachment_button->setAccessibleName(QStringLiteral("Attach file"));
    attachment_button->setAccessibleDescription(QStringLiteral(
        "Choose one or more local files to send with the next message."));
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
    composer_send_button_ = send_button;
    send_button->setObjectName("lingtai_composer_send_button");
    send_button->setAccessibleName(QStringLiteral("Send message"));
    send_button->setEnabled(false);
    send_button->setFixedSize(44, 44);
    send_button->setFullRadius(true);
    composer_action_row->addWidget(send_button, 0, Qt::AlignVCenter);

    const auto sync_composer_controls_height =
        [composer_controls, composer_input, send_button, attachment_button] {
            const auto margins = composer_controls->layout()->contentsMargins();
            const auto content = std::max({
                composer_input->height(),
                send_button->height(),
                attachment_button->height(),
            });
            const auto target = content
                + margins.top() + margins.bottom();
            composer_controls->setFixedHeight(std::max(52, target));
            composer_controls->raise_border();
        };
    const auto sync_composer_multiline =
        [composer_input, sync_composer_controls_height] {
            sync_composer_multiline_geometry(*composer_input);
            sync_composer_controls_height();
        };
    sync_composer_multiline();
    composer_input->heightChanges()
        | rpl::on_next([sync_composer_controls_height] {
            // Height already decided by multiline geometry; only grow the
            // capsule chrome around the field.
            sync_composer_controls_height();
        }, composer_lifetime_);
    composer_input->changes()
        | rpl::on_next([sync_composer_multiline] {
            sync_composer_multiline();
            // Layout may settle after the change signal; defer one tick so
            // wrapped visual line counts catch up with width.
            QTimer::singleShot(0, sync_composer_multiline);
        }, composer_lifetime_);

    composer_layout->addWidget(composer_controls);

    // Slash popup card: behavior is wired in follow-up steps.
    auto *slash_card = new SlashCommandCard(
        outer_scroll_ ? static_cast<QWidget *>(outer_scroll_) : this);
    (void)slash_card;
    auto *slash_popup = slash_card->list();
    (void)slash_popup;

    composer_status_ = make_label(
        composer, QString(), "lingtai_composer_status", 10);
    composer_status_->setAccessibleName(QStringLiteral("Composer notice"));
    composer_layout->addWidget(composer_status_);
    composer_notice_timer_ = new QTimer(this);
    composer_notice_timer_->setObjectName("lingtai_composer_notice_timer");
    composer_notice_timer_->setSingleShot(true);
    QObject::connect(composer_notice_timer_, &QTimer::timeout,
        this, [this] { clear_composer_notice(); });

    conversation_state_ = make_label(
        composer, QString(), "lingtai_selected_agent_conversation_state", 10);
    conversation_state_->setAccessibleName(
        QStringLiteral("Selected Agent conversation state"));
    composer_layout->addWidget(conversation_state_);

    auto *composer_surface = new QHBoxLayout;
    composer_surface->setContentsMargins(16, 0, 16, 12);
    composer_surface->addWidget(composer);
    detail_layout->addLayout(composer_surface);

    // Secondary pages host (presets + kanban).
    auto *pages_host = new PaletteSurface(this, st::windowBg);
    pages_host_ = pages_host;
    pages_host->setObjectName("lingtai_agent_pages_host");
    pages_host->setAccessibleName(
        QStringLiteral("Selected Agent secondary pages"));
    pages_host->setMinimumWidth(0);
    pages_host->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

    auto *pages_host_layout = new QVBoxLayout(pages_host);
    pages_host_layout->setContentsMargins(0, 0, 0, 0);
    pages_host_layout->setSpacing(8);

    auto presets_section = add_dashboard_section(
        pages_host, pages_host_layout, "preset_summary",
        QStringLiteral("Presets"),
        QStringLiteral("Selected Agent Presets"),
        QStringLiteral("The selected Agent's allowed presets, shown with the "
            "same catalog as project setup."));
    preset_summary_state_ = presets_section.state;
    secondary_pages_.push_back(presets_section.owner);

    apply_one_preset_catalog_chrome(presets_section.surface,
        pages_host->palette());

    pages_host->hide();
    detail_layout->addWidget(pages_host, 1);

    kanban_page_ = new KanbanPage(pages_host);
    kanban_page_holder_ = kanban_page_;
    kanban_page_->setMinimumWidth(0);
    kanban_page_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    pages_host_layout->addWidget(kanban_page_, 1);
    secondary_pages_.push_back(kanban_page_);

    // Source-facts labels below the page host.
    auto *manifest_identity = make_label(
        this, QString(), "lingtai_selected_agent_manifest_identity", 11);
    manifest_identity->setAccessibleName(QStringLiteral("Manifest identity"));
    detail_layout->addWidget(manifest_identity);

    auto *manifest_llm = make_label(
        this, QString(), "lingtai_selected_agent_manifest_llm", 11);
    manifest_llm->setAccessibleName(QStringLiteral("Manifest live LLM"));
    detail_layout->addWidget(manifest_llm);

    auto *manifest_capabilities = make_label(
        this, QString(), "lingtai_selected_agent_manifest_capabilities", 11);
    manifest_capabilities->setAccessibleName(
        QStringLiteral("Manifest capabilities"));
    detail_layout->addWidget(manifest_capabilities);

    auto *status_activity = make_label(
        this, QString(), "lingtai_selected_agent_status_activity", 11);
    status_activity->setAccessibleName(QStringLiteral("Status activity"));
    detail_layout->addWidget(status_activity);

    auto *status_context = make_label(
        this, QString(), "lingtai_selected_agent_status_context", 11);
    status_context->setAccessibleName(QStringLiteral("Status context"));
    detail_layout->addWidget(status_context);

    auto *detail_facts = make_label(
        this, QString(), "lingtai_selected_agent_facts", 11);
    detail_facts->setAccessibleName(QStringLiteral("Selected Agent facts"));
    detail_layout->addWidget(detail_facts);
    detail_layout->addStretch();

    // Keep the source-facts surfaces hidden by default. Presets page
    // visibility changes are handled in later refactor steps.
    for (const auto *facts_name : {
            "lingtai_selected_agent_manifest_identity",
            "lingtai_selected_agent_manifest_llm",
            "lingtai_selected_agent_manifest_capabilities",
            "lingtai_selected_agent_status_activity",
            "lingtai_selected_agent_status_context",
            "lingtai_selected_agent_facts" }) {
        if (auto *label = findChild<QLabel *>(facts_name)) {
            label->hide();
        }
    }

    // Composer + slash UI wiring.
    //
    // Side-effects (sending, navigation business logic) are handled by the
    // later NativeShell<->AgentDetailView delegation steps.
    if (detail_back_button_) {
        QObject::connect(detail_back_button_, &QPushButton::clicked,
            this, [this] { emit back_requested(); });
    }

    if (auto *start_button = findChild<QPushButton *>(
            "lingtai_selected_agent_start_agent")) {
        QObject::connect(start_button, &QPushButton::clicked,
            this, [this] { emit start_requested(); });
    }

    if (auto *sleep_button = findChild<QPushButton *>(
            "lingtai_selected_agent_request_sleep")) {
        QObject::connect(sleep_button, &QPushButton::clicked,
            this, [this] { emit sleep_requested(); });
    }

    for (std::size_t index = 0; index != page_nav_buttons_.size(); ++index) {
        if (auto *button = page_nav_buttons_[index]) {
            const auto page = static_cast<AgentDetailPage>(index);
            QObject::connect(button, &QPushButton::clicked, this,
                [this, page] { set_page(page); });
        }
    }

    if (kanban_page_) {
        QObject::connect(kanban_page_, &KanbanPage::agent_selected,
            this, [this](const QString &key) {
                emit kanban_agent_selected(
                    std::filesystem::path(key.toStdString()));
            });
        QObject::connect(kanban_page_, &KanbanPage::presets_requested,
            this, [this] { set_page(AgentDetailPage::presets); });
        QObject::connect(kanban_page_, &KanbanPage::back_requested,
            this, [this] {
                set_page(AgentDetailPage::conversation);
            });
    }

    if (composer_send_button_ && composer_input_) {
        composer_send_button_->addClickHandler([this] {
            hide_slash_command_popup(
                outer_scroll_ ? static_cast<QWidget *>(outer_scroll_) : this);
            if (!composer_input_) return;
            emit send_message_requested(composer_input_->getLastText());
        });
    }

    if (composer_attachment_button_) {
        QObject::connect(composer_attachment_button_, &QPushButton::clicked,
            this, [this] { emit attachment_selection_requested(); });
    }

    if (composer_input_) {
        composer_input_->submits()
            | rpl::on_next([this] {
                auto *root = outer_scroll_
                    ? static_cast<QWidget *>(outer_scroll_)
                    : static_cast<QWidget *>(this);
                auto *popup = root->findChild<QListWidget *>(
                    QStringLiteral("lingtai_slash_command_popup"));
                if (popup && popup->isVisible() && popup->currentItem()) {
                    apply_slash_popup_choice(composer_input_, popup);
                    return;
                }
                hide_slash_command_popup(root);
                if (!composer_input_) return;
                emit send_message_requested(composer_input_->getLastText());
            }, composer_lifetime_);

        composer_input_->changes()
            | rpl::on_next([this] {
                const auto text = composer_input_->getLastText();
                composer_input_->setPlaceholder(rpl::single(
                    text.isEmpty() ? QStringLiteral("Message…") : QString()));
                refresh_slash_command_popup(
                    outer_scroll_ ? static_cast<QWidget *>(outer_scroll_)
                        : this,
                    composer_input_);
                refresh_composer_enablement(composer_eligible_);
            }, composer_lifetime_);

        // With placeholderScale 0 the field would keep "Message…" while
        // focused; hide it for an empty ready-to-type cursor and restore it
        // only when focus leaves an empty field.
        composer_input_->focusedChanges()
            | rpl::on_next([this](bool focused) {
                if (!composer_input_) return;
                composer_input_->setPlaceholderHidden(focused);
            }, composer_lifetime_);

        base::install_event_filter(
            composer_input_->rawTextEdit(),
            composer_input_->rawTextEdit(),
            [this](not_null<QEvent *> event) {
                if (!composer_input_) return base::EventFilterResult::Continue;
                if (event->type() != QEvent::KeyPress) {
                    return base::EventFilterResult::Continue;
                }
                const auto *key = static_cast<QKeyEvent *>(event.get());

                auto *root = outer_scroll_
                    ? static_cast<QWidget *>(outer_scroll_)
                    : static_cast<QWidget *>(this);
                auto *popup = root->findChild<QListWidget *>(
                    QStringLiteral("lingtai_slash_command_popup"));
                if (!popup || !popup->isVisible()) {
                    return base::EventFilterResult::Continue;
                }
                if (key->key() == Qt::Key_Up) {
                    if (popup->count() <= 0) {
                        return base::EventFilterResult::Continue;
                    }
                    popup->setCurrentRow(
                        std::max(0, popup->currentRow() - 1));
                    return base::EventFilterResult::Cancel;
                }
                if (key->key() == Qt::Key_Down) {
                    if (popup->count() <= 0) {
                        return base::EventFilterResult::Continue;
                    }
                    popup->setCurrentRow(
                        std::min(popup->count() - 1,
                            popup->currentRow() + 1));
                    return base::EventFilterResult::Cancel;
                }
                if (key->key() == Qt::Key_Escape) {
                    hide_slash_command_popup(root);
                    return base::EventFilterResult::Cancel;
                }
                if (key->key() == Qt::Key_Tab) {
                    apply_slash_popup_choice(composer_input_, popup);
                    return base::EventFilterResult::Cancel;
                }
                return base::EventFilterResult::Continue;
            });
    }

    refresh_chrome();
}

void AgentDetailView::set_page(AgentDetailPage page) {
    if (page_ == page) return;
    const auto previous = page_;
    page_ = page;

    if (conversation_heading_) {
        // Kept only as a hidden object/implementation anchor for tests.
        conversation_heading_->setVisible(false);
    }

    const auto on_conversation = page == AgentDetailPage::conversation;
    if (conversation_surface_) {
        conversation_surface_->setVisible(on_conversation);
    }
    if (composer_) composer_->setVisible(on_conversation);
    if (composer_status_) composer_status_->setVisible(on_conversation);
    if (conversation_state_) conversation_state_->setVisible(on_conversation);

    if (pages_host_) pages_host_->setVisible(!on_conversation);
    if (pages_nav_) pages_nav_->setVisible(page == AgentDetailPage::presets);

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
        const auto nav_page = static_cast<AgentDetailPage>(index);
        page_nav_buttons_[index]->setChecked(page == nav_page);
        if (nav_page == AgentDetailPage::presets) {
            page_nav_buttons_[index]->hide();
        }
    }

    if (page == AgentDetailPage::kanban) {
        if (kanban_page_) kanban_page_->setFocus(Qt::OtherFocusReason);
    }

    emit page_changed(previous, page_);
}

void AgentDetailView::set_detail_width(int detail_width) {
    // Responsive top bar + composer lane + Kanban max sizing live here so
    // the shell can stay deterministic and UI tests can instantiate this
    // widget in isolation.
    if (composer_) {
        composer_detail_width_ = detail_width;
        const auto outer = std::max(12, (detail_width - 1600) / 2 + 12);
        composer_->setMinimumWidth(0);
        composer_->setMaximumWidth(QWIDGETSIZE_MAX);
        composer_->layout()->setContentsMargins(outer, 10, outer, 8);
        reflow_attachment_cards(std::max(1, detail_width - 2 * outer));
        if (composer_input_) {
            QListWidget *popup = nullptr;
            if (outer_scroll_) {
                popup = outer_scroll_->findChild<QListWidget *>(
                    QStringLiteral("lingtai_slash_command_popup"));
            }
            if (!popup) {
                popup = findChild<QListWidget *>(
                    QStringLiteral("lingtai_slash_command_popup"));
            }
            if (popup && popup->isVisible()) {
                position_slash_command_popup(popup, composer_input_);
            }
        }
    }

    if (chat_top_bar_) {
        fit_selected_agent_chat_top_bar(chat_top_bar_, detail_width);
    }

    if (kanban_page_) {
        auto width = detail_width;
        auto height = 0;
        if (outer_scroll_ && outer_scroll_->viewport()) {
            if (outer_scroll_->viewport()->width() > 0) {
                width = outer_scroll_->viewport()->width();
            }
            height = outer_scroll_->viewport()->height();
        }

        kanban_page_->setMinimumWidth(0);
        kanban_page_->setMinimumHeight(0);
        if (page_ == AgentDetailPage::kanban) {
            kanban_page_->setMaximumWidth(std::max(1, width));
            kanban_page_->setMaximumHeight(std::max(1, height));
        } else {
            kanban_page_->setMaximumWidth(QWIDGETSIZE_MAX);
            kanban_page_->setMaximumHeight(QWIDGETSIZE_MAX);
        }
    }
}

void AgentDetailView::render_conversation(
    const QString &them,
    const DirectConversationHistory &history,
    const QString &compact_state,
    bool selection_present,
    bool conversation_route_available,
    const std::unordered_map<std::string, MessageReactions> &reactions,
    const QString &main_agent_name,
    const std::vector<ConversationSessionEntry> &session_events,
    bool session_log_present,
    std::optional<ConversationPresentationRevision> revision) {
    const auto composer_eligible = selection_present
        && conversation_route_available;
    refresh_composer_enablement(composer_eligible);
    if (!conversation_surface_ || !conversation_state_) return;

    if (!selection_present) {
        conversation_surface_->set_select_agent_prompt(main_agent_name);
        conversation_state_->setText(QString());
        last_compact_state_.clear();
        session_log_present_ = false;
        conversation_route_available_ = false;
        refresh_conversation_detail_button();
        return;
    }

    if (!conversation_route_available) {
        conversation_surface_->set_plain_state(
            QStringLiteral("No conversation is available for this selection."));
        conversation_state_->setText(QString());
        last_compact_state_.clear();
        session_log_present_ = false;
        conversation_route_available_ = false;
        refresh_conversation_detail_button();
        return;
    }

    last_compact_state_ = compact_state;
    session_log_present_ = session_log_present;
    conversation_route_available_ = true;
    if (history.messages.empty()) {
        conversation_surface_->set_plain_state(
            QStringLiteral("No messages yet."));
    } else {
        if (revision) {
            conversation_surface_->set_conversation(
                them, history.messages, reactions, session_events, *revision);
        } else {
            conversation_surface_->set_conversation(
                them, history.messages, reactions, session_events);
        }
    }
    refresh_conversation_state_hint();
}

ConversationVerboseLevel AgentDetailView::conversation_verbose_level() const {
    if (!conversation_surface_) {
        return ConversationVerboseLevel::off;
    }
    return conversation_surface_->verbose_level();
}

void AgentDetailView::apply_conversation_session_events(
        const std::vector<ConversationSessionEntry> &session_events) {
    if (!conversation_surface_) {
        return;
    }
    conversation_surface_->apply_session_events(session_events);
}

void AgentDetailView::set_conversation_detail_loading(bool loading) {
    if (!conversation_detail_button_) {
        return;
    }
    conversation_detail_button_->setEnabled(!loading);
    if (loading) {
        conversation_detail_button_->setText(QStringLiteral("Loading…"));
        conversation_detail_button_->show();
        return;
    }
    refresh_conversation_detail_button();
}

void AgentDetailView::refresh_conversation_detail_button() {
    if (!conversation_detail_button_ || !conversation_surface_) {
        return;
    }
    if (!conversation_route_available_) {
        conversation_detail_button_->hide();
        return;
    }
    conversation_detail_button_->show();
    switch (conversation_surface_->verbose_level()) {
    case ConversationVerboseLevel::off:
        conversation_detail_button_->setText(QStringLiteral("Details"));
        break;
    case ConversationVerboseLevel::thinking:
        conversation_detail_button_->setText(QStringLiteral("Thinking"));
        break;
    case ConversationVerboseLevel::extended:
        conversation_detail_button_->setText(QStringLiteral("Extended"));
        break;
    }
    conversation_detail_button_->update();
}

void AgentDetailView::refresh_conversation_state_hint() {
    if (!conversation_state_) {
        return;
    }
    conversation_state_->setText(last_compact_state_);
    refresh_conversation_detail_button();
}

void AgentDetailView::scroll_conversation_to_bottom() {
    if (conversation_surface_) {
        conversation_surface_->scroll_to_bottom();
    }
}

void AgentDetailView::refresh_chrome() {
    const auto bg = st::windowBg->c;
    // QScrollArea viewports paint QPalette::Base. After a dark→light switch
    // the detail viewport can keep the old application Base while surfaces
    // that paint st::windowBg directly (conversation, composer pill) update,
    // leaving dark bands under the transparent header and composer margins.
    if (outer_scroll_) {
        outer_scroll_->setFrameShape(QFrame::NoFrame);
        outer_scroll_->setAutoFillBackground(false);
        if (auto *viewport = outer_scroll_->viewport()) {
            auto viewport_palette = viewport->palette();
            viewport_palette.setColor(QPalette::Window, bg);
            viewport_palette.setColor(QPalette::Base, bg);
            viewport->setPalette(viewport_palette);
            viewport->setAutoFillBackground(true);
        }
    }
    auto self_palette = palette();
    self_palette.setColor(QPalette::Window, bg);
    self_palette.setColor(QPalette::Base, bg);
    setPalette(self_palette);

    if (auto *presentation_name = findChild<QLabel *>(
            "lingtai_selected_agent_presentation_name")) {
        auto name_palette = presentation_name->palette();
        name_palette.setColor(QPalette::WindowText, st::windowFg->c);
        presentation_name->setPalette(name_palette);
    }
    if (selected_agent_key_) {
        auto key_palette = selected_agent_key_->palette();
        key_palette.setColor(QPalette::WindowText, st::windowSubTextFg->c);
        selected_agent_key_->setPalette(key_palette);
    }
    if (chat_top_bar_) chat_top_bar_->update();
    if (composer_) composer_->update();
    if (auto *controls = findChild<QWidget *>("lingtai_composer_controls")) {
        controls->update();
    }
    if (pages_host_) pages_host_->update();
    if (pages_nav_) pages_nav_->update();

    // Preset catalog chrome is stable except for theme/palette changes.
    if (pages_host_) {
        if (auto *catalog = findChild<QTreeWidget *>(
                QStringLiteral("lingtai_selected_agent_preset_summary"))) {
            apply_one_preset_catalog_chrome(catalog, pages_host_->palette());
        }
    }
    if (kanban_page_) {
        kanban_page_->apply_chrome();
    }
    if (conversation_surface_) {
        conversation_surface_->refresh_chrome();
    }
    refresh_conversation_detail_button();
}

void AgentDetailView::render_preset_summary(
    const std::optional<AgentPresetSummary> &summary) {
    auto *catalog = findChild<QTreeWidget *>(
        QStringLiteral("lingtai_selected_agent_preset_summary"));
    auto *state = preset_summary_state_;
    if (!state) {
        state = findChild<QLabel *>(
            QStringLiteral("lingtai_selected_agent_preset_summary_state"));
    }
    if (!catalog || !state) return;

    if (!summary.has_value()) {
        catalog->clear();
        catalog->setProperty("lingtai_preset_signature", QString());
        state->setText(QString());
        return;
    }
    const auto &value = *summary;

    auto set_state = [&](const QString &compact) {
        if (state->text() != compact) state->setText(compact);
    };

    QString compact;
    auto refs = std::vector<std::string>();
    switch (value.source) {
    case AgentPresetSummarySource::not_yet_published:
        compact = QStringLiteral("Not yet published");
        break;
    case AgentPresetSummarySource::unavailable:
        compact = QStringLiteral("Unavailable");
        break;
    case AgentPresetSummarySource::resolved:
        compact = QStringLiteral("Resolved");
        for (const auto &ref : value.allowed) refs.push_back(ref.ref);
        break;
    case AgentPresetSummarySource::stale:
        compact = QStringLiteral("Stale");
        for (const auto &ref : value.allowed) refs.push_back(ref.ref);
        break;
    }

    auto signature = compact + QLatin1Char('\n');
    for (const auto &ref : refs) {
        signature += QString::fromStdString(ref) + QLatin1Char('\n');
    }
    if (value.active_ref) {
        signature += QStringLiteral("active:")
            + QString::fromStdString(*value.active_ref);
    }

    if (catalog->property("lingtai_preset_signature").toString() == signature) {
        set_state(compact);
        return;
    }

    catalog->setProperty("lingtai_preset_signature", signature);
    catalog->clear();
    set_state(compact);

    const auto rows = build_preset_catalog_rows_from_refs(refs);
    QTreeWidgetItem *active_item = nullptr;
    for (auto index = 0; index != static_cast<int>(rows.size()); ++index) {
        add_preset_catalog_row(catalog, rows[static_cast<std::size_t>(index)],
            index);
        auto *item = catalog->topLevelItem(catalog->topLevelItemCount() - 1);
        if (value.active_ref
                && rows[static_cast<std::size_t>(index)].entry.path
                    == *value.active_ref) {
            active_item = item;
        }
    }

    if (active_item) {
        catalog->setCurrentItem(active_item);
    } else if (catalog->topLevelItemCount() > 0) {
        catalog->setCurrentItem(catalog->topLevelItem(0));
    }
}

void AgentDetailView::render_kanban(
    const KanbanBoard &board,
    const std::optional<std::filesystem::path> &selected_agent_key) {
    if (!kanban_page_) return;
    auto outer_pos = 0;
    if (outer_scroll_ && outer_scroll_->verticalScrollBar()) {
        outer_pos = outer_scroll_->verticalScrollBar()->value();
    }
    kanban_page_->set_board(board, selected_agent_key);
    if (outer_scroll_ && outer_scroll_->verticalScrollBar()) {
        outer_scroll_->verticalScrollBar()->setValue(outer_pos);
    }
}

void AgentDetailView::refresh_composer_enablement(bool composer_eligible) {
    composer_eligible_ = composer_eligible;
    if (composer_input_) composer_input_->setEnabled(composer_eligible);
    if (composer_attachment_button_) {
        composer_attachment_button_->setEnabled(composer_eligible);
    }
    if (composer_send_button_) {
        const auto has_text = composer_input_
            && !composer_input_->getLastText().trimmed().isEmpty();
        composer_send_button_->setEnabled(
            composer_eligible && (has_text || has_pending_attachments()));
    }
}

void AgentDetailView::merge_pending_attachments(
        const std::vector<std::filesystem::path> &selected_paths) {
    if (selected_paths.empty()) return;
    auto combined = std::vector<std::filesystem::path>();
    combined.reserve(pending_attachments_.size() + selected_paths.size());
    for (const auto &attachment : pending_attachments_) {
        combined.push_back(attachment.source_path);
    }
    combined.insert(combined.end(), selected_paths.begin(), selected_paths.end());

    auto result = preflight_attachments(combined);
    pending_attachments_ = std::move(result.accepted);
    attachment_errors_.assign(pending_attachments_.size(), QString());
    rebuild_attachment_cards();
    refresh_composer_enablement(composer_eligible_);
    if (pending_attachments_.empty() && result.rejected.empty()
            && !combined.empty()) {
        show_composer_notice(
            QStringLiteral("The selected files couldn’t be inspected."),
            ComposerNoticeKind::error);
    } else if (!result.rejected.empty()) {
        show_composer_notice(
            selection_rejection_text(result.rejected.front()),
            ComposerNoticeKind::warning);
    } else {
        show_composer_notice(
            pending_attachments_.size() == 1
                ? QStringLiteral("1 attachment ready.")
                : QStringLiteral("%1 attachments ready.")
                    .arg(pending_attachments_.size()),
            ComposerNoticeKind::success);
    }
}

void AgentDetailView::remove_pending_attachment(std::size_t index) {
    if (index >= pending_attachments_.size()) return;
    pending_attachments_.erase(pending_attachments_.begin()
        + static_cast<std::ptrdiff_t>(index));
    if (index < attachment_errors_.size()) {
        attachment_errors_.erase(attachment_errors_.begin()
            + static_cast<std::ptrdiff_t>(index));
    }
    rebuild_attachment_cards();
    refresh_composer_enablement(composer_eligible_);
}

void AgentDetailView::clear_pending_attachments() {
    pending_attachments_.clear();
    attachment_errors_.clear();
    rebuild_attachment_cards();
    refresh_composer_enablement(composer_eligible_);
}

void AgentDetailView::clear_attachment_errors() {
    attachment_errors_.assign(pending_attachments_.size(), QString());
    rebuild_attachment_cards();
}

void AgentDetailView::mark_attachment_error(
        std::size_t index, const QString &message) {
    if (index >= pending_attachments_.size()) return;
    attachment_errors_.resize(pending_attachments_.size());
    attachment_errors_[index] = message;
    rebuild_attachment_cards();
}

void AgentDetailView::show_composer_notice(
        const QString &message, ComposerNoticeKind kind) {
    if (!composer_status_) return;
    auto palette = composer_status_->palette();
    palette.setColor(QPalette::WindowText,
        kind == ComposerNoticeKind::error
            ? st::attentionButtonFg->c
            : kind == ComposerNoticeKind::success
                ? st::dialogsNameFg->c
                : st::windowSubTextFg->c);
    composer_status_->setPalette(palette);
    composer_status_->setProperty("lingtai_notice_kind",
        kind == ComposerNoticeKind::error ? "error"
            : kind == ComposerNoticeKind::warning ? "warning" : "success");
    composer_status_->setText(message);
    composer_status_->setAccessibleDescription(message);
    if (composer_notice_timer_) composer_notice_timer_->start(4500);
}

void AgentDetailView::clear_composer_notice() {
    if (composer_notice_timer_) composer_notice_timer_->stop();
    if (!composer_status_) return;
    composer_status_->clear();
    composer_status_->setAccessibleDescription(QString());
    composer_status_->setProperty("lingtai_notice_kind", QVariant());
}

void AgentDetailView::set_composer_spell_service(
        std::shared_ptr<ComposerSpellService> service) {
    composer_spell_service_ = std::move(service);
}

void AgentDetailView::rebuild_attachment_cards() {
    if (!composer_attachment_layout_ || !composer_attachment_tray_) return;
    while (auto *item = composer_attachment_layout_->takeAt(0)) {
        if (auto *widget = item->widget()) delete widget;
        delete item;
    }

    for (auto index = std::size_t{0}; index != pending_attachments_.size();
            ++index) {
        const auto &attachment = pending_attachments_[index];
        auto *card = new QWidget(composer_attachment_tray_);
        card->setObjectName(QStringLiteral("lingtai_composer_attachment_card_%1")
            .arg(index));
        card->setAccessibleName(QStringLiteral("Pending attachment %1")
            .arg(QString::fromStdString(attachment.display_filename)));
        card->setAccessibleDescription(QStringLiteral("%1, %2")
            .arg(QString::fromStdString(attachment.source_path.string()),
                human_file_size(attachment.byte_size)));
        card->setToolTip(QString::fromStdString(attachment.source_path.string()));
        card->setFixedWidth(340);
        card->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        auto card_border = index < attachment_errors_.size()
                && !attachment_errors_[index].isEmpty()
            ? st::attentionButtonFg->c : st::windowSubTextFg->c;
        if (index >= attachment_errors_.size()
                || attachment_errors_[index].isEmpty()) {
            card_border.setAlpha(104);
        }
        card->setStyleSheet(QStringLiteral(
            "QWidget#%1 { background-color: %2; border: 1px solid %3; "
            "border-radius: 8px; } ")
            .arg(card->objectName(),
                st::windowBgOver->c.name(QColor::HexArgb),
                card_border.name(QColor::HexArgb)));

        auto *layout = new QHBoxLayout(card);
        layout->setContentsMargins(8, 7, 6, 7);
        layout->setSpacing(8);
        auto *preview = new QLabel(card);
        preview->setObjectName(QStringLiteral(
            "lingtai_composer_attachment_preview_%1").arg(index));
        preview->setAccessibleName(QStringLiteral("Attachment preview"));
        preview->setFixedSize(52, 40);
        preview->setAlignment(Qt::AlignCenter);
        auto preview_border = st::windowSubTextFg->c;
        preview_border.setAlpha(120);
        preview->setStyleSheet(QStringLiteral(
            "QLabel#%1 { background-color: %2; border: 1px solid %3; "
            "border-radius: 6px; color: %4; font-weight: 600; }")
            .arg(preview->objectName(),
                st::windowBg->c.name(QColor::HexArgb),
                preview_border.name(QColor::HexArgb),
                st::windowFg->c.name(QColor::HexArgb)));
        const auto thumbnail = load_attachment_thumbnail(attachment, QSize(52, 40));
        if (!thumbnail.isNull()) {
            preview->setPixmap(thumbnail);
            preview->setProperty("lingtai_preview_kind", "thumbnail");
        } else {
            auto extension = QString::fromStdString(
                attachment.source_path.extension().string())
                .remove(QLatin1Char('.')).toUpper();
            preview->setText(extension.isEmpty() ? QStringLiteral("FILE")
                                                 : extension.left(5));
            preview->setProperty("lingtai_preview_kind", "file");
        }
        layout->addWidget(preview, 0, Qt::AlignVCenter);

        auto *labels = new QWidget(card);
        labels->setMinimumWidth(0);
        auto *labels_layout = new QVBoxLayout(labels);
        labels_layout->setContentsMargins(0, 0, 0, 0);
        labels_layout->setSpacing(1);
        auto *name = new QLabel(
            QString::fromStdString(attachment.display_filename), labels);
        name->setObjectName(QStringLiteral(
            "lingtai_composer_attachment_name_%1").arg(index));
        name->setToolTip(QString::fromStdString(attachment.source_path.string()));
        name->setTextFormat(Qt::PlainText);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        name->setMinimumWidth(0);
        auto name_font = name->font();
        name_font.setWeight(QFont::DemiBold);
        name->setFont(name_font);
        name->setText(QFontMetrics(name->font()).elidedText(
            name->text(), Qt::ElideMiddle, 210));
        labels_layout->addWidget(name);
        auto *detail = new QLabel(human_file_size(attachment.byte_size), labels);
        detail->setObjectName(QStringLiteral(
            "lingtai_composer_attachment_size_%1").arg(index));
        auto detail_palette = detail->palette();
        detail_palette.setColor(QPalette::WindowText, st::windowSubTextFg->c);
        detail->setPalette(detail_palette);
        labels_layout->addWidget(detail);
        if (index < attachment_errors_.size()
                && !attachment_errors_[index].isEmpty()) {
            auto *error = new QLabel(attachment_errors_[index], labels);
            error->setObjectName(QStringLiteral(
                "lingtai_composer_attachment_error_%1").arg(index));
            error->setWordWrap(true);
            auto error_palette = error->palette();
            error_palette.setColor(
                QPalette::WindowText, st::attentionButtonFg->c);
            error->setPalette(error_palette);
            labels_layout->addWidget(error);
        }
        layout->addWidget(labels, 1);

        auto *remove = new QPushButton(QStringLiteral("×"), card);
        remove->setObjectName(QStringLiteral(
            "lingtai_composer_attachment_remove_%1").arg(index));
        remove->setAccessibleName(QStringLiteral("Remove %1")
            .arg(QString::fromStdString(attachment.display_filename)));
        remove->setAccessibleDescription(QStringLiteral(
            "Removes this file from the pending message."));
        remove->setToolTip(remove->accessibleName());
        remove->setFixedSize(28, 28);
        remove->setFlat(true);
        remove->setCursor(Qt::PointingHandCursor);
        remove->setFocusPolicy(Qt::StrongFocus);
        QObject::connect(remove, &QPushButton::clicked,
            this, [this, index] {
                QTimer::singleShot(0, this,
                    [this, index] { remove_pending_attachment(index); });
            });
        layout->addWidget(remove, 0, Qt::AlignTop);
        composer_attachment_layout_->addWidget(card, 0, static_cast<int>(index));
    }
    composer_attachment_tray_->setVisible(!pending_attachments_.empty());
    reflow_attachment_cards(std::max(1, composer_detail_width_));
}

void AgentDetailView::reflow_attachment_cards(int available_width) {
    if (!composer_attachment_layout_) return;
    auto cards = std::vector<QWidget *>();
    while (auto *item = composer_attachment_layout_->takeAt(0)) {
        if (auto *widget = item->widget()) cards.push_back(widget);
        delete item;
    }
    constexpr auto kCardWidth = 340;
    constexpr auto kCardGap = 6;
    const auto columns = std::max(
        1, (available_width + kCardGap) / (kCardWidth + kCardGap));
    for (auto index = std::size_t{0}; index != cards.size(); ++index) {
        composer_attachment_layout_->addWidget(cards[index],
            static_cast<int>(index) / columns,
            static_cast<int>(index) % columns);
    }
}

} // namespace lingtai::desktop
