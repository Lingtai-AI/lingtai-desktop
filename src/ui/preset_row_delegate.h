#pragma once

#include "base/basic_types.h"

#include "styles/palette.h"

#include <QtGui/QPainter>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QTreeWidget>

namespace lingtai::desktop {

constexpr auto kPresetSummaryRole = Qt::UserRole + 10;
constexpr auto kPresetSectionRole = Qt::UserRole + 11;
constexpr auto kPresetCapabilitiesRole = Qt::UserRole + 12;
constexpr auto kPresetRowHeight = 52;
constexpr auto kPresetSectionHeight = 28;
constexpr auto kPresetSelectionRail = 3;

struct PresetCatalogTokens {
    QColor surface;
    QColor header;
    QColor section_band;
    QColor section_text;
    QColor divider;
    QColor selected_row;
    QColor selection_accent;
    QColor border;
    QColor tag_fill;
    QColor value_text;
};

inline bool preset_catalog_is_dark(const QPalette &palette) {
    if (st::windowBg->c.lightness() < 128) return true;
    return palette.color(QPalette::Window).lightness() < 128;
}

inline PresetCatalogTokens preset_catalog_tokens(const QPalette &palette) {
    if (preset_catalog_is_dark(palette)) {
        return {
            QColor(QStringLiteral("#181B1A")),
            QColor(QStringLiteral("#202422")),
            QColor(QStringLiteral("#222A26")),
            QColor(QStringLiteral("#B8CBC2")),
            QColor(255, 255, 255, 20),
            QColor(QStringLiteral("#213A31")),
            QColor(QStringLiteral("#78C9A7")),
            QColor(255, 255, 255, 20),
            QColor(QStringLiteral("#222A26")),
            QColor(QStringLiteral("#E8EEEA")),
        };
    }
    return {
        QColor(QStringLiteral("#FFFFFF")),
        QColor(QStringLiteral("#F1F3F2")),
        QColor(QStringLiteral("#EDF3F0")),
        QColor(QStringLiteral("#4D6259")),
        QColor(0, 0, 0, 20),
        QColor(QStringLiteral("#E7F4EF")),
        QColor(QStringLiteral("#16785C")),
        QColor(QStringLiteral("#DCE2DF")),
        QColor(0, 0, 0, 8),
        QColor(QStringLiteral("#1F2933")),
    };
}

inline bool is_preset_section_index(const QModelIndex &index) {
    return index.isValid()
        && index.siblingAtColumn(0).data(kPresetSectionRole).toBool();
}

inline QRect preset_section_band_rect(
        const QStyleOptionViewItem &option, const QModelIndex &index) {
    auto band = option.rect;
    const auto *view = qobject_cast<const QAbstractItemView *>(option.widget);
    if (!view || !index.model()) {
        return band;
    }
    const auto last = index.model()->columnCount(index.parent()) - 1;
    return view->visualRect(index.siblingAtColumn(0)).united(
        view->visualRect(index.siblingAtColumn(last)));
}

class PresetRowDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(
            QPainter *painter,
            const QStyleOptionViewItem &option,
            const QModelIndex &index) const override {
        auto opt = option;
        initStyleOption(&opt, index);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (is_preset_section_index(index)) {
            if (index.column() != 0) {
                painter->restore();
                return;
            }
            const auto tokens = preset_catalog_tokens(opt.palette);
            const auto band = preset_section_band_rect(opt, index);
            painter->setClipRect(band);
            painter->fillRect(band, tokens.section_band);
            painter->setPen(tokens.divider);
            painter->drawLine(band.topLeft(), band.topRight());
            painter->drawLine(band.bottomLeft(), band.bottomRight());
            auto section_font = opt.font;
            section_font.setPointSize(10);
            section_font.setWeight(QFont::DemiBold);
            section_font.setLetterSpacing(QFont::PercentageSpacing, 118);
            painter->setFont(section_font);
            painter->setPen(tokens.section_text);
            painter->drawText(
                band.adjusted(12, 0, -12, 0),
                Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                index.data(Qt::DisplayRole).toString().toUpper());
            painter->restore();
            return;
        }
        painter->setClipRect(opt.rect);
        const auto tokens = preset_catalog_tokens(opt.palette);
        const auto selected = opt.state.testFlag(QStyle::State_Selected);
        if (selected) {
            painter->fillRect(opt.rect, tokens.selected_row);
            if (index.column() == 0) {
                painter->fillRect(
                    QRect(opt.rect.left(), opt.rect.top(),
                        kPresetSelectionRail, opt.rect.height()),
                    tokens.selection_accent);
            }
        }
        const auto text_color = tokens.value_text;
        const auto muted = tokens.section_text;
        const auto inner = opt.rect.adjusted(12, 5, -10, -5);
        if (index.column() == 0) {
            const auto name = index.data(Qt::DisplayRole).toString();
            const auto summary = index.data(kPresetSummaryRole).toString();
            auto name_font = opt.font;
            name_font.setPointSize(13);
            name_font.setWeight(QFont::DemiBold);
            painter->setFont(name_font);
            painter->setPen(text_color);
            const auto name_height = summary.isEmpty()
                ? inner.height()
                : inner.height() / 2;
            const auto name_rect = QRect(
                inner.left(), inner.top(), inner.width(), name_height);
            painter->drawText(
                name_rect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                painter->fontMetrics().elidedText(
                    name, Qt::ElideRight, name_rect.width()));
            if (!summary.isEmpty()) {
                auto summary_font = opt.font;
                summary_font.setPointSize(10);
                summary_font.setWeight(QFont::Normal);
                painter->setFont(summary_font);
                painter->setPen(muted);
                const auto summary_rect = QRect(
                    inner.left(), inner.top() + name_height,
                    inner.width(), inner.height() - name_height);
                painter->drawText(
                    summary_rect,
                    Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                    painter->fontMetrics().elidedText(
                        summary, Qt::ElideRight, summary_rect.width()));
            }
        } else if (index.column() == 2) {
            auto tags = index.siblingAtColumn(0).data(kPresetCapabilitiesRole)
                .toStringList();
            if (tags.isEmpty()) {
                const auto capability = index.data(Qt::DisplayRole).toString();
                if (!capability.isEmpty()) {
                    tags = capability.split(QStringLiteral(", "),
                        Qt::SkipEmptyParts);
                }
            }
            auto tag_font = opt.font;
            tag_font.setPointSize(10);
            tag_font.setWeight(QFont::Medium);
            painter->setFont(tag_font);
            auto x = inner.left();
            for (const auto &tag : tags) {
                const auto chip_width =
                    painter->fontMetrics().horizontalAdvance(tag) + 14;
                const auto chip = QRect(
                    x, inner.center().y() - 9, chip_width, 18);
                painter->setPen(QPen(tokens.border, 1));
                painter->setBrush(tokens.tag_fill);
                painter->drawRoundedRect(chip.adjusted(0, 0, -1, -1), 6, 6);
                painter->setPen(tokens.selection_accent);
                painter->drawText(chip, Qt::AlignCenter, tag);
                x += chip_width + 6;
            }
        } else {
            auto provider_font = opt.font;
            provider_font.setPointSize(12);
            provider_font.setWeight(QFont::Normal);
            painter->setFont(provider_font);
            painter->setPen(muted);
            painter->drawText(
                inner, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                painter->fontMetrics().elidedText(
                    index.data(Qt::DisplayRole).toString(),
                    Qt::ElideRight, inner.width()));
        }
        if (index.column() == 0) {
            const auto line = preset_section_band_rect(opt, index);
            painter->setPen(tokens.divider);
            painter->drawLine(line.bottomLeft(), line.bottomRight());
        }
        painter->restore();
    }

    QSize sizeHint(
            const QStyleOptionViewItem &,
            const QModelIndex &index) const override {
        if (is_preset_section_index(index)) {
            return {120, kPresetSectionHeight};
        }
        return {120, kPresetRowHeight};
    }
};

inline bool is_preset_section(const QTreeWidgetItem *item) {
    return item && item->data(0, kPresetSectionRole).toBool();
}

inline void configure_preset_table(QTreeWidget *table) {
    table->setRootIsDecorated(false);
    table->setUniformRowHeights(false);
    table->setIndentation(0);
    table->setItemsExpandable(false);
    table->setAnimated(false);
    table->setItemDelegate(new PresetRowDelegate(table));
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    table->setAutoFillBackground(true);
    table->viewport()->setAutoFillBackground(true);
    table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    table->header()->setVisible(true);
    table->header()->setStretchLastSection(true);
    table->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
}

inline QTreeWidgetItem *add_preset_section(QTreeWidget *table, const QString &title) {
    auto *item = new QTreeWidgetItem(table);
    item->setText(0, title);
    item->setFlags(Qt::ItemIsEnabled);
    for (auto column = 0; column != table->columnCount(); ++column) {
        item->setData(column, kPresetSectionRole, true);
    }
    item->setFirstColumnSpanned(true);
    return item;
}

inline QTreeWidgetItem *adjacent_preset_row(
        QTreeWidget *table, int from, int step) {
    if (!table || step == 0) {
        return nullptr;
    }
    for (auto index = from + step;
            index >= 0 && index < table->topLevelItemCount();
            index += step) {
        auto *item = table->topLevelItem(index);
        if (!is_preset_section(item) && !item->isHidden()) {
            return item;
        }
    }
    return nullptr;
}

} // namespace lingtai::desktop
