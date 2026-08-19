#pragma once

#include "base/basic_types.h"

#include "styles/palette.h"
#include "styles/style_widgets.h"

#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

namespace lingtai::desktop {

void paint_slash_glyph(
        QPainter &painter,
        const QRectF &box,
        const QString &name,
        const QColor &ink,
        const QColor &well);

constexpr auto kSlashCardShadow = 14;
constexpr auto kSlashRowHeight = 52;

class SlashOfferDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(
            QPainter *painter,
            const QStyleOptionViewItem &option,
            const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const auto selected = option.state.testFlag(QStyle::State_Selected)
            || option.state.testFlag(QStyle::State_MouseOver);
        const auto name = index.data(Qt::UserRole).toString();
        const auto blurb = index.data(Qt::UserRole + 1).toString();
        const auto accent = st::defaultActiveButton.textBg->c;
        const auto row = option.rect.adjusted(6, 2, -6, -2);
        if (selected) {
            auto wash = accent;
            wash.setAlpha(28);
            painter->setPen(Qt::NoPen);
            painter->setBrush(wash);
            painter->drawRoundedRect(row, 10, 10);
            painter->setBrush(accent);
            painter->drawRoundedRect(
                QRect(row.left() + 3, row.top() + 10, 3, row.height() - 20),
                1.5, 1.5);
        }
        const auto well = QRect(row.left() + 12, row.center().y() - 14, 28, 28);
        auto well_fill = accent;
        well_fill.setAlpha(selected ? 36 : 22);
        painter->setPen(Qt::NoPen);
        painter->setBrush(well_fill);
        painter->drawRoundedRect(well, 8, 8);
        paint_slash_glyph(*painter, well, name, accent, well_fill);

        auto name_font = option.font;
        name_font.setPointSize(13);
        name_font.setWeight(QFont::DemiBold);
        auto blurb_font = option.font;
        blurb_font.setPointSize(11);
        blurb_font.setWeight(QFont::Normal);
        const auto text_left = well.right() + 12;
        const auto text_width = row.right() - 14 - text_left;
        painter->setFont(name_font);
        painter->setPen(st::windowFg->c);
        painter->drawText(
            QRect(text_left, row.top() + 7, text_width, 18),
            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
            QStringLiteral("/%1").arg(name));
        painter->setFont(blurb_font);
        painter->setPen(st::windowSubTextFg->c);
        painter->drawText(
            QRect(text_left, row.top() + 25, text_width, 16),
            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
            blurb);
        painter->restore();
    }

    QSize sizeHint(
            const QStyleOptionViewItem &,
            const QModelIndex &) const override {
        return {320, 52};
    }
};

class SlashCommandCard final : public QWidget {
public:
    explicit SlashCommandCard(QWidget *parent)
    : QWidget(parent) {
        setObjectName("lingtai_slash_command_card");
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(
            kSlashCardShadow, 10, kSlashCardShadow, kSlashCardShadow + 4);
        layout->setSpacing(0);
        header_ = new QLabel(QStringLiteral("Commands"), this);
        header_->setObjectName("lingtai_slash_command_header");
        header_->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto header_font = header_->font();
        header_font.setPointSize(10);
        header_font.setWeight(QFont::DemiBold);
        header_font.setCapitalization(QFont::AllUppercase);
        header_->setFont(header_font);
        header_->setContentsMargins(18, 10, 18, 6);
        layout->addWidget(header_);
        list_ = new QListWidget(this);
        list_->setObjectName("lingtai_slash_command_popup");
        list_->setAccessibleName(QStringLiteral("Slash commands"));
        list_->setFocusPolicy(Qt::NoFocus);
        list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        list_->setMouseTracking(true);
        list_->setFrameShape(QFrame::NoFrame);
        list_->setItemDelegate(new SlashOfferDelegate(list_));
        list_->setSpacing(0);
        list_->setUniformItemSizes(true);
        list_->setAutoFillBackground(false);
        list_->viewport()->setAutoFillBackground(false);
        list_->viewport()->setMouseTracking(true);
        layout->addWidget(list_, 1);
        hint_ = new QLabel(
            QStringLiteral("↑↓ Navigate    ↵ Select    Esc Dismiss"), this);
        hint_->setObjectName("lingtai_slash_command_hint");
        hint_->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto hint_font = hint_->font();
        hint_font.setPointSize(10);
        hint_->setFont(hint_font);
        hint_->setContentsMargins(18, 4, 18, 10);
        layout->addWidget(hint_);
        apply_palette();
        hide();
    }

    [[nodiscard]] QListWidget *list() const { return list_; }

    void apply_palette() {
        auto palette = this->palette();
        palette.setColor(QPalette::Window, st::windowBg->c);
        palette.setColor(QPalette::Base, Qt::transparent);
        palette.setColor(QPalette::Text, st::windowFg->c);
        palette.setColor(QPalette::Highlight, Qt::transparent);
        palette.setColor(QPalette::HighlightedText, st::windowFg->c);
        setPalette(palette);
        list_->setPalette(palette);
        list_->setStyleSheet(QStringLiteral(
            "QListWidget { background: transparent; border: none; outline: none; }"
            "QListWidget::item { border: none; padding: 0; }"));
        auto header_palette = header_->palette();
        header_palette.setColor(QPalette::WindowText, st::windowSubTextFg->c);
        header_->setPalette(header_palette);
        auto hint_palette = hint_->palette();
        hint_palette.setColor(QPalette::WindowText, st::windowSubTextFg->c);
        hint_->setPalette(hint_palette);
        update();
        list_->viewport()->update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto card = rect().adjusted(
            kSlashCardShadow, 6, -kSlashCardShadow, -(kSlashCardShadow - 2));
        auto shadow = st::windowFg->c;
        for (auto i = 5; i >= 1; --i) {
            shadow.setAlpha(6 * (6 - i));
            painter.setPen(Qt::NoPen);
            painter.setBrush(shadow);
            painter.drawRoundedRect(card.adjusted(-i, i - 1, i, i + 1), 16, 16);
        }
        painter.setBrush(st::windowBg->c);
        painter.setPen(QPen(QColor(st::defaultActiveButton.textBg->c.red(),
            st::defaultActiveButton.textBg->c.green(),
            st::defaultActiveButton.textBg->c.blue(), 40), 1));
        painter.drawRoundedRect(card, 14, 14);
    }

private:
    QListWidget *list_ = nullptr;
    QLabel *header_ = nullptr;
    QLabel *hint_ = nullptr;
};

} // namespace lingtai::desktop
