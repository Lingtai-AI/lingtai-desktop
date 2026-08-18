#include "preset_editor_page.h"

#include <QtCore/QFileInfo>
#include <QtCore/QSignalBlocker>
#include <QtCore/QVector>
#include <QtGui/QFont>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QVBoxLayout>
#include <functional>

namespace lingtai::desktop {
namespace {

const auto kJade = QStringLiteral("#16785C");
const auto kBorder = QStringLiteral("#DCE2DF");
const auto kMuted = QStringLiteral("#6B7280");

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

QLineEdit *make_field(QWidget *parent, const char *name) {
    auto *field = new QLineEdit(parent);
    field->setObjectName(name);
    field->setFixedHeight(34);
    field->setStyleSheet(QStringLiteral(
        "QLineEdit { border: 1px solid %1; border-radius: 8px; padding: 0 10px; "
        "background: #FFFFFF; }").arg(kBorder));
    return field;
}

QWidget *make_field_block(QWidget *parent, const QString &caption,
        QWidget *field, int stretch = 0) {
    auto *block = new QWidget(parent);
    auto *layout = new QVBoxLayout(block);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    auto *label = make_label(block, caption, "lingtai_setup_edit_preset_field_label",
        12, QFont::DemiBold);
    layout->addWidget(label);
    layout->addWidget(field);
    if (stretch > 0) {
        field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    return block;
}

QFrame *make_rule(QWidget *parent) {
    auto *rule = new QFrame(parent);
    rule->setFrameShape(QFrame::HLine);
    rule->setStyleSheet(QStringLiteral("color: %1;").arg(kBorder));
    return rule;
}

struct CapabilityCopy {
    const char *name;
    const char *title;
    const char *description;
};

constexpr CapabilityCopy kCapabilities[] = {
    {"email", "Email", "Internal LingTai mail."},
    {"psyche", "Psyche", "Evolving identity and context."},
    {"soul", "Soul", "Periodic self-reflection."},
    {"system", "System", "Lifecycle control."},
    {"knowledge", "Knowledge", "Private durable memory."},
    {"skills", "Skills", "Browse and use the skill catalog."},
    {"shell", "Shell", "Run shell commands with guardrails."},
    {"avatar", "Avatar", "Spawn independent sub-agents."},
    {"daemon", "Daemon", "Spawn short-lived parallel workers."},
    {"mcp", "MCP", "Connect external tools."},
    {"file", "File", "Read, write, edit, glob, and grep files."},
    {"web_search", "Web search", "Search current information."},
    {"vision", "Vision", "Understand images and documents."},
};

} // namespace

class ChoiceStrip final : public QWidget {
public:
    explicit ChoiceStrip(QWidget *parent, const char *object_name)
    : QWidget(parent)
    , group_(new QButtonGroup(this)) {
        setObjectName(object_name);
        group_->setExclusive(true);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        layout_ = layout;
        connect(group_, &QButtonGroup::idClicked, this, [this](int) {
            if (changed_) changed_(value());
        });
    }

    void set_changed(std::function<void(const QString &)> changed) {
        changed_ = std::move(changed);
    }

    void set_options(const QStringList &labels, const QStringList &values) {
        while (auto *item = layout_->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        buttons_.clear();
        values_ = values;
        for (auto index = 0; index != labels.size(); ++index) {
            auto *button = new QPushButton(labels[index], this);
            button->setCheckable(true);
            button->setFixedHeight(32);
            button->setCursor(Qt::PointingHandCursor);
            button->setStyleSheet(QStringLiteral(
                "QPushButton { border: 1px solid %1; border-radius: 6px; "
                "padding: 0 12px; background: #FFFFFF; color: #111827; }"
                "QPushButton:checked { background: %2; color: white; border: none; "
                "font-weight: 600; }")
                .arg(kBorder, kJade));
            group_->addButton(button, index);
            layout_->addWidget(button);
            buttons_.push_back(button);
        }
        layout_->addStretch();
    }

    void set_value(const QString &value) {
        for (auto index = 0; index != values_.size(); ++index) {
            if (values_[index] == value) {
                if (auto *button = group_->button(index)) button->setChecked(true);
                return;
            }
        }
        if (auto *checked = group_->checkedButton()) {
            group_->setExclusive(false);
            checked->setChecked(false);
            group_->setExclusive(true);
        }
    }

    [[nodiscard]] QString value() const {
        const auto id = group_->checkedId();
        if (id < 0 || id >= values_.size()) return {};
        return values_[id];
    }

private:
    QButtonGroup *group_ = nullptr;
    QHBoxLayout *layout_ = nullptr;
    QStringList values_;
    QVector<QPushButton *> buttons_;
    std::function<void(const QString &)> changed_;
};

PresetEditorPage::PresetEditorPage(QWidget *parent)
: QWidget(parent) {
    setObjectName("lingtai_setup_edit_preset_page");
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("lingtai_setup_edit_preset_scroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body = new QWidget(scroll);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 8, 16);
    layout->setSpacing(14);

    auto *back = new QPushButton(QStringLiteral("← Presets"), body);
    back->setObjectName("lingtai_setup_edit_preset_back");
    back->setFlat(true);
    back->setCursor(Qt::PointingHandCursor);
    back->setStyleSheet(QStringLiteral(
        "QPushButton { color: %1; border: none; text-align: left; padding: 0; "
        "font-weight: 600; }").arg(kJade));
    back->setFixedHeight(24);
    layout->addWidget(back, 0, Qt::AlignLeft);
    connect(back, &QPushButton::clicked, this, &PresetEditorPage::cancelled);

    layout->addWidget(make_label(body, QStringLiteral("Edit preset"),
        "lingtai_setup_edit_preset_heading", 22, QFont::DemiBold));
    auto *subtitle = make_label(body,
        QStringLiteral("Configure identity, model, connection, and capabilities."),
        "lingtai_setup_edit_preset_subtitle", 13, QFont::Normal,
        kMuted);
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);

    layout->addWidget(make_label(body, QStringLiteral("Identity"),
        "lingtai_setup_edit_preset_section_identity", 16, QFont::DemiBold));
    layout->addWidget(make_label(body,
        QStringLiteral("Define the preset's name, summary, and tier."),
        "lingtai_setup_edit_preset_identity_note", 12, QFont::Normal,
        kMuted));
    name_ = make_field(body, "lingtai_setup_edit_preset_name");
    summary_ = make_field(body, "lingtai_setup_edit_preset_summary");
    auto *identity_row = new QHBoxLayout;
    identity_row->setSpacing(12);
    identity_row->addWidget(make_field_block(body, QStringLiteral("Name"), name_), 1);
    identity_row->addWidget(make_field_block(body, QStringLiteral("Summary"), summary_), 2);
    layout->addLayout(identity_row);

    tier_ = new ChoiceStrip(body, "lingtai_setup_edit_preset_tier");
    tier_->set_options(
        {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"),
            QStringLiteral("4"), QStringLiteral("5")},
        {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"),
            QStringLiteral("4"), QStringLiteral("5")});
    layout->addWidget(make_field_block(body, QStringLiteral("Tier"), tier_));
    auto *extra_row = new QHBoxLayout;
    extra_row->setSpacing(12);
    gains_ = make_field(body, "lingtai_setup_edit_preset_gains");
    gains_->setPlaceholderText(QStringLiteral("Optional"));
    losses_ = make_field(body, "lingtai_setup_edit_preset_losses");
    losses_->setPlaceholderText(QStringLiteral("Optional"));
    extra_row->addWidget(make_field_block(body, QStringLiteral("Gains (optional)"), gains_), 1);
    extra_row->addWidget(make_field_block(body, QStringLiteral("Losses (optional)"), losses_), 1);
    layout->addLayout(extra_row);

    layout->addWidget(make_rule(body));
    layout->addWidget(make_label(body, QStringLiteral("Model & connection"),
        "lingtai_setup_edit_preset_section_llm", 16, QFont::DemiBold));
    layout->addWidget(make_label(body,
        QStringLiteral("Choose the model, tune performance, and configure connection."),
        "lingtai_setup_edit_preset_llm_note", 12, QFont::Normal,
        kMuted));

    const auto combo_style = QStringLiteral(
        "QComboBox { border: 1px solid %1; border-radius: 8px; padding: 0 10px; "
        "min-height: 34px; background: #FFFFFF; }").arg(kBorder);
    provider_ = new QComboBox(body);
    provider_->setObjectName("lingtai_setup_edit_preset_provider");
    provider_->setStyleSheet(combo_style);
    model_combo_ = new QComboBox(body);
    model_combo_->setObjectName("lingtai_setup_edit_preset_model");
    model_combo_->setEditable(true);
    model_combo_->setStyleSheet(combo_style);
    model_edit_ = make_field(body, "lingtai_setup_edit_preset_model_edit");
    auto *provider_row = new QHBoxLayout;
    provider_row->setSpacing(12);
    provider_row->addWidget(make_field_block(body, QStringLiteral("Provider"), provider_), 1);
    auto *model_stack = new QWidget(body);
    auto *model_stack_layout = new QVBoxLayout(model_stack);
    model_stack_layout->setContentsMargins(0, 0, 0, 0);
    model_stack_layout->setSpacing(0);
    model_stack_layout->addWidget(model_combo_);
    model_stack_layout->addWidget(model_edit_);
    provider_row->addWidget(make_field_block(body, QStringLiteral("Model"), model_stack), 1);
    layout->addLayout(provider_row);

    service_tier_ = new ChoiceStrip(body, "lingtai_setup_edit_preset_service_tier");
    service_tier_->set_options(
        {QStringLiteral("Normal"), QStringLiteral("Fast")},
        {QStringLiteral("normal"), QStringLiteral("fast")});
    service_tier_row_ = make_field_block(body, QStringLiteral("Service tier"), service_tier_);
    layout->addWidget(service_tier_row_);

    thinking_ = new ChoiceStrip(body, "lingtai_setup_edit_preset_thinking");
    thinking_row_ = make_field_block(body, QStringLiteral("Reasoning effort"), thinking_);
    layout->addWidget(thinking_row_);

    api_compat_ = new ChoiceStrip(body, "lingtai_setup_edit_preset_api_compat");
    api_compat_->set_options(
        {QStringLiteral("Off"), QStringLiteral("OpenAI"), QStringLiteral("Anthropic")},
        {QString(), QStringLiteral("openai"), QStringLiteral("anthropic")});
    layout->addWidget(make_field_block(body, QStringLiteral("OpenAI-compatible API"),
        api_compat_));

    wire_api_ = new ChoiceStrip(body, "lingtai_setup_edit_preset_wire_api");
    wire_api_->set_options(
        {QStringLiteral("auto"), QStringLiteral("chat_completions"),
            QStringLiteral("responses")},
        {QStringLiteral("auto"), QStringLiteral("chat_completions"),
            QStringLiteral("responses")});
    wire_api_row_ = make_field_block(body, QStringLiteral("API type"), wire_api_);
    layout->addWidget(wire_api_row_);

    transport_ = new ChoiceStrip(body, "lingtai_setup_edit_preset_responses_transport");
    transport_->set_options(
        {QStringLiteral("http"), QStringLiteral("websocket")},
        {QStringLiteral("http"), QStringLiteral("websocket")});
    transport_row_ = make_field_block(body, QStringLiteral("Responses transport"),
        transport_);
    layout->addWidget(transport_row_);

    regions_ = new ChoiceStrip(body, "lingtai_setup_edit_preset_regions");
    region_row_ = make_field_block(body, QStringLiteral("Endpoint"), regions_);
    layout->addWidget(region_row_);
    base_url_ = make_field(body, "lingtai_setup_edit_preset_base_url");
    layout->addWidget(make_field_block(body, QStringLiteral("Base URL"), base_url_));

    credential_status_ = make_label(body, QString(),
        "lingtai_setup_edit_preset_credential", 13);
    manage_ = new QPushButton(QStringLiteral("Manage"), body);
    manage_->setObjectName("lingtai_setup_edit_preset_manage");
    manage_->setFixedHeight(30);
    manage_->setStyleSheet(QStringLiteral(
        "QPushButton { border: 1px solid %1; border-radius: 6px; padding: 0 12px; "
        "background: #FFFFFF; }").arg(kBorder));
    auto *credential_inner = new QWidget(body);
    auto *credential_layout = new QHBoxLayout(credential_inner);
    credential_layout->setContentsMargins(0, 0, 0, 0);
    credential_layout->addWidget(credential_status_, 1);
    credential_layout->addWidget(manage_, 0, Qt::AlignRight);
    credential_row_ = make_field_block(body, QStringLiteral("Credential"), credential_inner);
    layout->addWidget(credential_row_);

    api_key_ = make_field(body, "lingtai_setup_edit_preset_api_key");
    api_key_->setEchoMode(QLineEdit::Password);
    api_key_row_ = make_field_block(body, QStringLiteral("API key"), api_key_);
    layout->addWidget(api_key_row_);

    layout->addWidget(make_rule(body));
    layout->addWidget(make_label(body, QStringLiteral("Capabilities"),
        "lingtai_setup_edit_preset_section_capabilities", 16, QFont::DemiBold));
    layout->addWidget(make_label(body,
        QStringLiteral("Every capability the kernel can grant is always included."),
        "lingtai_setup_edit_preset_capabilities_note", 12, QFont::Normal,
        kMuted));
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(10);
    const auto count = static_cast<int>(sizeof(kCapabilities) / sizeof(kCapabilities[0]));
    const auto mid = (count + 1) / 2;
    for (auto index = 0; index != count; ++index) {
        const auto &cap = kCapabilities[index];
        auto *item = new QWidget(body);
        auto *item_layout = new QHBoxLayout(item);
        item_layout->setContentsMargins(0, 0, 0, 0);
        item_layout->setSpacing(10);
        auto *toggle = new QCheckBox(item);
        toggle->setObjectName(QStringLiteral("lingtai_setup_edit_preset_cap_%1")
            .arg(QLatin1String(cap.name)));
        toggle->setChecked(true);
        toggle->setEnabled(false);
        toggle->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        toggle->setFocusPolicy(Qt::NoFocus);
        toggle->setStyleSheet(QStringLiteral(
            "QCheckBox::indicator { width: 36px; height: 20px; }"
            "QCheckBox::indicator:checked { image: none; border-radius: 10px; "
            "background: %1; }").arg(kJade));
        auto *copy = new QWidget(item);
        auto *copy_layout = new QVBoxLayout(copy);
        copy_layout->setContentsMargins(0, 0, 0, 0);
        copy_layout->setSpacing(1);
        copy_layout->addWidget(make_label(copy, QLatin1String(cap.title),
            "lingtai_setup_edit_preset_cap_title", 13, QFont::DemiBold));
        auto *desc = make_label(copy, QLatin1String(cap.description),
            "lingtai_setup_edit_preset_cap_desc", 11, QFont::Normal,
            kMuted);
        desc->setWordWrap(true);
        copy_layout->addWidget(desc);
        item_layout->addWidget(toggle, 0, Qt::AlignTop);
        item_layout->addWidget(copy, 1);
        const auto column = index < mid ? 0 : 1;
        const auto row = index < mid ? index : index - mid;
        grid->addWidget(item, row, column);
    }
    layout->addLayout(grid);
    auto *guidance = make_label(body,
        QStringLiteral("Advanced capability settings can be edited in init.json."),
        "lingtai_setup_edit_preset_capabilities_guidance", 11, QFont::Normal,
        kMuted);
    guidance->setWordWrap(true);
    layout->addWidget(guidance);
    layout->addStretch();
    scroll->setWidget(body);
    root->addWidget(scroll, 1);

    auto *footer = new QWidget(this);
    footer->setObjectName("lingtai_setup_edit_preset_footer");
    footer->setStyleSheet(QStringLiteral(
        "QWidget#lingtai_setup_edit_preset_footer { background: #FFFFFF; "
        "border-top: 1px solid %1; }").arg(kBorder));
    auto *footer_layout = new QVBoxLayout(footer);
    footer_layout->setContentsMargins(0, 10, 0, 0);
    footer_layout->setSpacing(8);
    error_ = make_label(footer, QString(), "lingtai_setup_edit_preset_error",
        12, QFont::Normal, QStringLiteral("#B42318"));
    error_->setWordWrap(true);
    error_->hide();
    footer_layout->addWidget(error_);
    auto *actions = new QHBoxLayout;
    auto *cancel = new QPushButton(QStringLiteral("Cancel"), footer);
    cancel->setObjectName("lingtai_setup_edit_preset_cancel");
    cancel->setFixedHeight(34);
    cancel->setStyleSheet(QStringLiteral(
        "QPushButton { background: #FFFFFF; border: 1px solid %1; border-radius: 6px; "
        "padding: 0 16px; }").arg(kBorder));
    save_ = new QPushButton(QStringLiteral("Save preset"), footer);
    save_->setObjectName("lingtai_setup_edit_preset_save");
    save_->setFixedHeight(34);
    save_->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: white; border: none; border-radius: 6px; "
        "padding: 0 16px; font-weight: 600; }").arg(kJade));
    actions->addWidget(cancel);
    actions->addStretch();
    actions->addWidget(save_);
    footer_layout->addLayout(actions);
    root->addWidget(footer);

    connect(cancel, &QPushButton::clicked, this, &PresetEditorPage::cancelled);
    connect(save_, &QPushButton::clicked, this, &PresetEditorPage::on_save);
    connect(manage_, &QPushButton::clicked, this, &PresetEditorPage::on_manage_credential);
    connect(provider_, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        if (rebuilding_) return;
        model_.set_provider(text);
        rebuild_from_model();
    });
    connect(model_combo_, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        if (rebuilding_) return;
        model_.set_model(text.trimmed());
        sync_conditional_rows();
    });
    connect(model_edit_, &QLineEdit::editingFinished, this, [this] {
        if (rebuilding_) return;
        model_.set_model(model_edit_->text().trimmed());
    });
    connect(base_url_, &QLineEdit::editingFinished, this, [this] {
        if (rebuilding_) return;
        model_.set_base_url(base_url_->text().trimmed());
        rebuild_from_model();
    });
    connect(api_key_, &QLineEdit::editingFinished, this, [this] {
        if (rebuilding_) return;
        if (!api_key_->text().isEmpty() || api_key_->isModified()) {
            model_.set_api_key(api_key_->text());
        }
    });
    tier_->set_changed([this](const QString &value) {
        if (!rebuilding_) model_.set_tier(value);
    });
    service_tier_->set_changed([this](const QString &value) {
        if (!rebuilding_) model_.set_service_tier(value);
    });
    thinking_->set_changed([this](const QString &value) {
        if (!rebuilding_) model_.set_thinking(value);
    });
    api_compat_->set_changed([this](const QString &value) {
        if (rebuilding_) return;
        model_.set_api_compat(value);
        rebuild_from_model();
    });
    wire_api_->set_changed([this](const QString &value) {
        if (rebuilding_) return;
        model_.set_wire_api(value);
        rebuild_from_model();
    });
    transport_->set_changed([this](const QString &value) {
        if (!rebuilding_) model_.set_responses_transport(value);
    });
    regions_->set_changed([this](const QString &value) {
        if (rebuilding_) return;
        model_.set_base_url(value);
        rebuild_from_model();
    });
}

void PresetEditorPage::load(const PresetEditorLoadRequest &request) {
    existing_names_ = request.existing_names;
    model_.load(request);
    error_->hide();
    error_->clear();
    rebuild_from_model();
}

void PresetEditorPage::pull_text_fields() {
    model_.set_name(name_->text().trimmed());
    model_.set_summary(summary_->text().trimmed());
    model_.set_extra(QStringLiteral("gains"), gains_->text());
    model_.set_extra(QStringLiteral("loses"), losses_->text());
    if (model_combo_->isVisible()) {
        model_.set_model(model_combo_->currentText().trimmed());
    } else {
        model_.set_model(model_edit_->text().trimmed());
    }
    model_.set_base_url(base_url_->text().trimmed());
    if (api_key_->isModified()) {
        model_.set_api_key(api_key_->text());
    }
}

void PresetEditorPage::rebuild_from_model() {
    rebuilding_ = true;
    const QSignalBlocker block_provider(provider_);
    const QSignalBlocker block_model(model_combo_);
    name_->setText(model_.name());
    summary_->setText(model_.summary());
    tier_->set_value(model_.tier());
    gains_->setText(model_.extra(QStringLiteral("gains")));
    losses_->setText(model_.extra(QStringLiteral("loses")));

    provider_->clear();
    provider_->addItems(model_.provider_options());
    provider_->setCurrentText(model_.provider());

    const auto picker = model_.model_has_picker();
    model_combo_->setVisible(picker);
    model_edit_->setVisible(!picker);
    if (picker) {
        model_combo_->clear();
        model_combo_->addItems(model_.model_options());
        model_combo_->setCurrentText(model_.model());
    } else {
        model_edit_->setText(model_.model());
    }

    service_tier_->set_value(model_.service_tier());
    const auto thinking_options = model_.thinking_options();
    auto thinking_labels = QStringList();
    for (const auto &option : thinking_options) {
        if (option == QLatin1String("xhigh")) {
            thinking_labels.push_back(QStringLiteral("X-high"));
        } else if (option.isEmpty()) {
            thinking_labels.push_back(option);
        } else {
            auto label = option;
            label[0] = label[0].toUpper();
            thinking_labels.push_back(label);
        }
    }
    thinking_->set_options(thinking_labels, thinking_options);
    thinking_->set_value(model_.thinking());
    api_compat_->set_value(model_.api_compat());
    wire_api_->set_value(model_.wire_api());
    transport_->set_value(model_.responses_transport());

    const auto regions = model_.region_options();
    auto region_labels = QStringList();
    auto region_values = QStringList();
    for (const auto &region : regions) {
        region_labels.push_back(region.label);
        region_values.push_back(region.url);
    }
    regions_->set_options(region_labels, region_values);
    const auto selected = model_.selected_region_index();
    if (selected >= 0 && selected < regions.size()) {
        regions_->set_value(regions[selected].url);
    }
    base_url_->setText(model_.base_url());
    if (model_.has_saved_api_key() && !model_.api_key_set()) {
        api_key_->setPlaceholderText(QStringLiteral("Saved — paste to replace"));
        api_key_->clear();
        api_key_->setModified(false);
    } else {
        api_key_->setPlaceholderText(QStringLiteral("(not set — paste here)"));
        if (model_.api_key_set()) api_key_->setText(model_.api_key());
    }

    if (model_.is_codex_provider()) {
        const auto valid = model_.codex_bound_valid();
        const auto label = model_.codex_bound_label();
        credential_status_->setText(valid
            ? QStringLiteral("Connected as %1").arg(label)
            : QStringLiteral("%1 — not logged in").arg(label));
        credential_status_->setStyleSheet(valid
            ? QStringLiteral("color: %1;").arg(kJade)
            : QStringLiteral("color: #B42318;"));
    } else if (model_.is_codex_pool_provider() || model_.is_claude_cli_provider()) {
        credential_status_->setText(
            QStringLiteral("Credentials are managed outside this API-key field."));
        credential_status_->setStyleSheet(QStringLiteral("color: %1;")
            .arg(kMuted));
    }
    rebuilding_ = false;
    sync_conditional_rows();
}

void PresetEditorPage::sync_conditional_rows() {
    service_tier_row_->setVisible(model_.service_tier_visible());
    thinking_row_->setVisible(model_.thinking_visible());
    wire_api_row_->setVisible(model_.wire_api_visible());
    transport_row_->setVisible(model_.responses_transport_visible());
    region_row_->setVisible(!model_.region_options().isEmpty());
    const auto oauth = model_.is_codex_provider()
        || model_.is_codex_pool_provider()
        || model_.is_claude_cli_provider();
    credential_row_->setVisible(oauth);
    manage_->setVisible(model_.is_codex_provider());
    api_key_row_->setVisible(model_.uses_api_key_field());
}

void PresetEditorPage::on_manage_credential() {
    if (!model_.is_codex_provider()) return;
    auto accounts = model_.codex_accounts();
    if (accounts.isEmpty()) {
        error_->setText(QStringLiteral(
            "Codex login required — sign in from LingTai TUI first."));
        error_->show();
        return;
    }
    QMenu menu(this);
    for (const auto &account : accounts) {
        auto label = account.email.isEmpty() ? account.label : account.email;
        if (label.isEmpty()) {
            label = account.legacy ? QStringLiteral("default account")
                : QFileInfo(account.path).completeBaseName();
        }
        if (!account.valid) label += QStringLiteral(" (invalid)");
        auto *action = menu.addAction(label);
        action->setData(account.ref);
    }
    const auto *chosen = menu.exec(manage_->mapToGlobal(QPoint(0, manage_->height())));
    if (!chosen) return;
    model_.set_codex_auth_ref(chosen->data().toString());
    rebuild_from_model();
}

void PresetEditorPage::on_save() {
    pull_text_fields();
    auto names = existing_names_;
    names.append(saved_preset_names(lingtai_global_dir()));
    names.removeDuplicates();
    auto result = model_.commit(names);
    if (!model_.save_commit(result) || !result.ok) {
        error_->setText(result.error.isEmpty()
            ? QStringLiteral("Could not save this preset.")
            : result.error);
        error_->show();
        return;
    }
    error_->hide();
    emit saved(result.name);
}

} // namespace lingtai::desktop
