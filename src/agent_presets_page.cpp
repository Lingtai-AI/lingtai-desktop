#include "agent_presets_page.h"

#include "setup_style.h"
#include "setup_toggle.h"

#include <QtCore/QEvent>
#include <QtGui/QFont>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QVBoxLayout>

namespace lingtai::desktop {
namespace {

constexpr auto kRowToggleSlot = 36;

QString value_css(const QWidget *widget) {
    return setup_color_css(setup_tokens(widget->palette()).value_text);
}

QString accent_css(const QWidget *widget) {
    return setup_color_css(setup_tokens(widget->palette()).selection_accent);
}

QString muted_css(const QWidget *widget) {
    return setup_color_css(setup_tokens(widget->palette()).muted_text);
}

SetupToggle *as_toggle(QWidget *widget) {
    return static_cast<SetupToggle *>(widget);
}

QLabel *make_label(QWidget *parent, const QString &text, const char *name,
        int point, QFont::Weight weight = QFont::Normal, const QString &color = {}) {
    auto *label = new QLabel(text, parent);
    label->setObjectName(name);
    auto font = label->font();
    font.setPointSize(point);
    font.setWeight(weight);
    label->setFont(font);
    if (!color.isEmpty()) {
        label->setStyleSheet(QStringLiteral("color: %1;").arg(color));
    }
    return label;
}

QStringList capability_tags(bool has_vision, bool has_tools) {
    QStringList tags;
    if (has_vision) tags << QStringLiteral("Vision");
    if (has_tools) tags << QStringLiteral("Tools");
    return tags;
}

QWidget *make_pills(QWidget *parent, const QStringList &tags) {
    auto *wrap = new QWidget(parent);
    auto *layout = new QHBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    if (tags.isEmpty()) {
        layout->addWidget(make_label(wrap, QStringLiteral("—"),
            "lingtai_setup_agents_row_caps", 11, QFont::Normal, muted_css(wrap)));
    } else {
        const auto tokens = setup_tokens(wrap->palette());
        for (const auto &tag : tags) {
            auto *pill = make_label(wrap, tag, "lingtai_setup_agents_row_caps",
                11, QFont::DemiBold);
            pill->setStyleSheet(setup_chip_css(tokens));
            layout->addWidget(pill, 0, Qt::AlignVCenter);
        }
    }
    layout->addStretch();
    return wrap;
}

} // namespace

AgentPresetsPage::AgentPresetsPage(QWidget *parent)
: QWidget(parent) {
    setObjectName("lingtai_setup_agents_page");
    setAutoFillBackground(false);
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    const auto tokens = setup_tokens(palette());
    root->addWidget(make_label(this, QStringLiteral("Configure agent presets"),
        "lingtai_setup_agents_heading", 22, QFont::DemiBold, value_css(this)));
    auto *subtitle = make_label(this,
        QStringLiteral("Choose the default preset and which alternatives the agent may use at runtime."),
        "lingtai_setup_agents_subtitle", 13, QFont::Normal, muted_css(this));
    subtitle->setWordWrap(true);
    root->addWidget(subtitle);

    auto *search = new QLineEdit(this);
    search->setObjectName("lingtai_setup_agents_search");
    search->setPlaceholderText(QStringLiteral("Search presets"));
    search->setClearButtonEnabled(true);
    search->setFixedHeight(34);
    search->setMinimumWidth(240);
    search->setMaximumWidth(520);
    apply_setup_line_edit(search, tokens);
    root->addWidget(search, 0, Qt::AlignLeft);

    auto *header = new QWidget(this);
    auto *header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(10 + kRowToggleSlot + 10, 0, 12, 0);
    header_layout->setSpacing(12);
    auto add_header = [&](const QString &text, int stretch) {
        auto *label = make_label(header, text, "lingtai_setup_agents_column",
            11, QFont::DemiBold, muted_css(header));
        header_layout->addWidget(label, stretch);
    };
    add_header(QStringLiteral("Preset"), 3);
    add_header(QStringLiteral("Provider / model"), 2);
    add_header(QStringLiteral("Capabilities"), 2);
    root->addWidget(header);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("lingtai_setup_agents_catalog");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setAutoFillBackground(false);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; border: 1px solid %1; "
        "border-radius: 8px; }").arg(setup_color_css(tokens.border)));
    list_ = new QWidget(scroll);
    list_->setAutoFillBackground(false);
    list_->setMinimumWidth(0);
    auto *list_layout = new QVBoxLayout(list_);
    list_layout->setContentsMargins(0, 0, 0, 0);
    list_layout->setSpacing(0);
    list_layout->addStretch();
    scroll->setWidget(list_);
    scroll->setMinimumHeight(280);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    root->addWidget(scroll, 1);

    empty_ = make_label(this,
        QStringLiteral("No saved presets yet — go back to the library and edit a template to create one."),
        "lingtai_setup_agents_empty", 13, QFont::Normal, muted_css(this));
    empty_->setWordWrap(true);
    empty_->hide();
    root->addWidget(empty_);

    message_ = make_label(this, QString(), "lingtai_setup_agents_message",
        12, QFont::Normal, QStringLiteral("#B42318"));
    message_->hide();
    root->addWidget(message_);

    auto *note = make_label(this,
        QStringLiteral("The default preset is used at startup. Enabled alternatives may be selected by the agent at runtime."),
        "lingtai_setup_agents_note", 12, QFont::Normal, muted_css(this));
    note->setWordWrap(true);
    root->addWidget(note);

    auto *actions = new QHBoxLayout;
    auto *back = new QPushButton(QStringLiteral("Back"), this);
    back->setObjectName("lingtai_setup_agents_back");
    back->setFixedHeight(34);
    apply_setup_secondary_button(back, tokens);
    continue_ = new QPushButton(QStringLiteral("Continue"), this);
    continue_->setObjectName("lingtai_setup_agents_continue");
    continue_->setFixedHeight(34);
    continue_->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: white; border: none; "
        "border-radius: 6px; padding: 0 16px; font-weight: 600; }").arg(accent_css(this)));
    actions->addWidget(back);
    actions->addStretch();
    actions->addWidget(continue_);
    root->addLayout(actions);

    connect(back, &QPushButton::clicked, this, &AgentPresetsPage::back_requested);
    connect(continue_, &QPushButton::clicked, this, [this] {
        if (!has_saved_presets()) {
            message_->setText(QStringLiteral(
                "No saved presets — edit a template first to create one."));
            message_->show();
            return;
        }
        message_->hide();
        emit continue_requested();
    });
    connect(search, &QLineEdit::textChanged, this, [this](const QString &query) {
        apply_filter(query);
    });
}

void AgentPresetsPage::load_from_chooser(
        QComboBox *chooser, const QString &preferred_default) {
    existing_mode_ = false;
    rows_.clear();
    default_index_ = -1;
    if (!chooser) {
        rebuild_rows();
        return;
    }
    for (auto index = 0; index != chooser->count(); ++index) {
        if (chooser->itemData(index, Qt::UserRole + 4).toBool()) continue;
        Row row;
        row.name = chooser->itemText(index);
        row.summary = chooser->itemData(index, Qt::UserRole).toString();
        row.provider = chooser->itemData(index, Qt::UserRole + 2).toString();
        row.model = chooser->itemData(index, Qt::UserRole + 3).toString();
        row.tags = capability_tags(
            chooser->itemData(index, Qt::UserRole + 5).toBool(),
            chooser->itemData(index, Qt::UserRole + 6).toBool());
        row.allowed = false;
        rows_.push_back(row);
    }
    for (auto index = 0; index != rows_.size(); ++index) {
        if (rows_[index].name == preferred_default) {
            default_index_ = index;
            break;
        }
    }
    if (default_index_ < 0 && !rows_.isEmpty()) default_index_ = 0;
    if (default_index_ >= 0) rows_[default_index_].allowed = true;
    rebuild_rows();
}

void AgentPresetsPage::load_existing(const QString &default_name,
        const QStringList &allowed_names, const QString &active_name) {
    existing_mode_ = true;
    rows_.clear();
    default_index_ = -1;
    auto references = allowed_names;
    if (!default_name.isEmpty() && !references.contains(default_name)) {
        references.push_back(default_name);
    }
    if (!active_name.isEmpty() && !references.contains(active_name)) {
        references.push_back(active_name);
    }
    for (const auto &reference : references) {
        Row row;
        row.name = reference;
        row.summary = QStringLiteral("Existing preset reference");
        row.allowed = allowed_names.contains(reference);
        row.active = reference == active_name;
        rows_.push_back(row);
        if (reference == default_name) default_index_ = rows_.size() - 1;
    }
    rebuild_rows();
}

QString AgentPresetsPage::default_name() const {
    if (default_index_ < 0 || default_index_ >= rows_.size()) return {};
    return rows_[default_index_].name;
}

QStringList AgentPresetsPage::allowed_names() const {
    auto names = QStringList();
    if (default_index_ >= 0 && default_index_ < rows_.size()) {
        names.push_back(rows_[default_index_].name);
    }
    for (auto index = 0; index != rows_.size(); ++index) {
        if (index == default_index_ || !rows_[index].allowed) continue;
        names.push_back(rows_[index].name);
    }
    return names;
}

int AgentPresetsPage::allowed_count() const {
    auto count = 0;
    for (const auto &row : rows_) {
        if (row.allowed) ++count;
    }
    return count;
}

bool AgentPresetsPage::has_saved_presets() const {
    return !rows_.isEmpty() && default_index_ >= 0;
}

void AgentPresetsPage::rebuild_rows() {
    auto *layout = qobject_cast<QVBoxLayout *>(list_->layout());
    while (layout->count() > 1) {
        auto *item = layout->takeAt(0);
        delete item->widget();
        delete item;
    }
    empty_->setVisible(rows_.isEmpty());
    continue_->setEnabled(!rows_.isEmpty());
    for (auto index = 0; index != rows_.size(); ++index) {
        auto &row = rows_[index];
        auto *widget = new QWidget(list_);
        widget->setFixedHeight(56);
        widget->setCursor(Qt::PointingHandCursor);
        auto *row_layout = new QHBoxLayout(widget);
        row_layout->setContentsMargins(10, 6, 12, 6);
        row_layout->setSpacing(10);

        auto *toggle_slot = new QWidget(widget);
        toggle_slot->setFixedWidth(kRowToggleSlot);
        auto *toggle_layout = new QHBoxLayout(toggle_slot);
        toggle_layout->setContentsMargins(0, 0, 0, 0);
        toggle_layout->setSpacing(0);
        auto *toggle = new SetupToggle(toggle_slot);
        toggle->setObjectName("lingtai_setup_agents_allowed");
        toggle_layout->addWidget(toggle, 0, Qt::AlignVCenter);
        toggle_layout->addStretch();
        row_layout->addWidget(toggle_slot, 0, Qt::AlignVCenter);

        auto *identity = new QWidget(widget);
        auto *identity_layout = new QVBoxLayout(identity);
        identity_layout->setContentsMargins(0, 0, 0, 0);
        identity_layout->setSpacing(1);
        identity_layout->addWidget(make_label(identity, row.name,
            "lingtai_setup_agents_row_name", 13, QFont::DemiBold,
            value_css(identity)));
        auto *summary = make_label(identity, row.summary,
            "lingtai_setup_agents_row_summary", 11, QFont::Normal,
            muted_css(identity));
        summary->setWordWrap(false);
        identity_layout->addWidget(summary);
        row_layout->addWidget(identity, 3);

        auto *provider = new QWidget(widget);
        auto *provider_layout = new QVBoxLayout(provider);
        provider_layout->setContentsMargins(0, 0, 0, 0);
        provider_layout->setSpacing(1);
        provider_layout->addWidget(make_label(provider,
            row.provider.isEmpty() ? QStringLiteral("—") : row.provider,
            "lingtai_setup_agents_row_provider", 12, QFont::DemiBold,
            value_css(provider)));
        provider_layout->addWidget(make_label(provider,
            row.model.isEmpty() ? QStringLiteral("—") : row.model,
            "lingtai_setup_agents_row_model", 11, QFont::Normal,
            muted_css(provider)));
        row_layout->addWidget(provider, 2);
        row_layout->addWidget(make_pills(widget, row.tags), 2);

        auto *default_label = make_label(widget, QString(),
            "lingtai_setup_agents_row_default", 12, QFont::DemiBold,
            accent_css(this));
        default_label->setFixedWidth(56);
        default_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row_layout->addWidget(default_label, 0, Qt::AlignRight | Qt::AlignVCenter);

        row.widget = widget;
        row.toggle = toggle;
        row.default_label = default_label;
        layout->insertWidget(layout->count() - 1, widget);

        connect(toggle, &QCheckBox::toggled, this, [this, index](bool on) {
            set_allowed(index, on, true);
        });
        widget->installEventFilter(this);
        refresh_row_chrome(index);
    }
}

bool AgentPresetsPage::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::MouseButtonRelease) {
        for (auto index = 0; index != rows_.size(); ++index) {
            if (rows_[index].widget != watched) continue;
            auto *mouse = static_cast<QMouseEvent *>(event);
            auto *child = rows_[index].widget->childAt(mouse->pos());
            while (child && child != rows_[index].widget) {
                if (child == rows_[index].toggle
                        || child->parentWidget() == rows_[index].toggle) {
                    return false;
                }
                child = child->parentWidget();
            }
            set_default_row(index);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AgentPresetsPage::refresh_row_chrome(int index, bool animate_toggle) {
    if (index < 0 || index >= rows_.size()) return;
    auto &row = rows_[index];
    const auto is_default = index == default_index_;
    if (auto *toggle = as_toggle(row.toggle)) {
        toggle->set_checked(row.allowed, animate_toggle);
    }
    const auto marker = is_default && row.active
        ? QStringLiteral("Default · Active")
        : (is_default ? QStringLiteral("Default")
            : (row.active ? QStringLiteral("Active") : QString()));
    row.default_label->setText(marker);
    row.default_label->setAccessibleName(marker);
    row.default_label->setFixedWidth(existing_mode_ ? 108 : 56);
    const auto highlight = setup_color_css(
        setup_tokens(row.widget->palette()).selected_row);
    row.widget->setStyleSheet(is_default
        ? QStringLiteral("background: %1;").arg(highlight)
        : QStringLiteral("background: transparent;"));
}

void AgentPresetsPage::set_default_row(int index) {
    if (index < 0 || index >= rows_.size()) return;
    const auto animate_toggle = !rows_[index].allowed;
    default_index_ = index;
    rows_[index].allowed = true;
    message_->hide();
    for (auto row = 0; row != rows_.size(); ++row) {
        refresh_row_chrome(row, row == index && animate_toggle);
    }
}

void AgentPresetsPage::set_allowed(int index, bool allowed, bool animate) {
    if (index < 0 || index >= rows_.size()) return;
    auto *toggle = as_toggle(rows_[index].toggle);
    if (!toggle) return;
    if (index == default_index_ && !allowed) {
        toggle->set_checked(true, animate);
        message_->setText(QStringLiteral(
            "The default preset must remain in the allowed list."));
        message_->show();
        return;
    }
    message_->hide();
    rows_[index].allowed = allowed;
    toggle->set_checked(allowed, animate);
}

void AgentPresetsPage::apply_filter(const QString &query) {
    const auto needle = query.trimmed();
    for (const auto &row : rows_) {
        if (!row.widget) continue;
        const auto haystack = row.name + QLatin1Char(' ') + row.summary
            + QLatin1Char(' ') + row.provider + QLatin1Char(' ') + row.model
            + QLatin1Char(' ') + row.tags.join(QLatin1Char(' '));
        row.widget->setVisible(needle.isEmpty()
            || haystack.contains(needle, Qt::CaseInsensitive));
    }
}

} // namespace lingtai::desktop
