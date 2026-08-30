#include "agent_config_page.h"

#include "preset_editor_model.h"
#include "setup_style.h"
#include "setup_toggle.h"

#include <QtCore/QDir>
#include <QtCore/QSignalBlocker>
#include <QtGui/QFont>
#include <QtWidgets/QAbstractSpinBox>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyleFactory>
#include <QtWidgets/QVBoxLayout>

namespace lingtai::desktop {
namespace {

QString value_css(const QWidget *widget) {
    return setup_color_css(setup_tokens(widget->palette()).value_text);
}

QString accent_css(const QWidget *widget) {
    return setup_color_css(setup_tokens(widget->palette()).selection_accent);
}

QString muted_css(const QWidget *widget) {
    return setup_color_css(setup_tokens(widget->palette()).muted_text);
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

QLineEdit *make_field(QWidget *parent, const char *name) {
    auto *field = new QLineEdit(parent);
    field->setObjectName(name);
    field->setFixedHeight(34);
    field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    apply_setup_line_edit(field, setup_tokens(parent->palette()));
    return field;
}

QSpinBox *make_spin(QWidget *parent, const char *name, int min, int max) {
    auto *field = new QSpinBox(parent);
    field->setObjectName(name);
    field->setRange(min, max);
    field->setButtonSymbols(QAbstractSpinBox::NoButtons);
    field->setFrame(false);
    field->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    const auto tokens = setup_tokens(parent->palette());
    field->setStyleSheet(QStringLiteral(
        "QSpinBox { border: none; background: transparent; padding: 0 10px; "
        "color: %1; }").arg(setup_color_css(tokens.value_text)));
    return field;
}

QWidget *make_stepper_spin(QWidget *parent, QSpinBox *field) {
    auto *wrap = new QWidget(parent);
    wrap->setObjectName("lingtai_setup_review_spin_wrap");
    wrap->setFixedHeight(34);
    wrap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    wrap->setStyleSheet(setup_spin_wrap_css(setup_tokens(parent->palette())));
    field->setParent(wrap);

    auto make_step = [&](const QString &label) {
        const auto tokens = setup_tokens(wrap->palette());
        auto *button = new QPushButton(label, wrap);
        button->setFlat(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedSize(22, 12);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; border: none; border-radius: 4px; "
            "color: %2; font-size: 10px; font-weight: 700; padding: 0; }"
            "QPushButton:hover { background: %3; }"
            "QPushButton:pressed { background: %3; }"
            "QPushButton:disabled { color: %4; background: %1; }")
            .arg(setup_color_css(tokens.header),
                setup_color_css(tokens.selection_accent),
                setup_color_css(tokens.selected_row),
                setup_color_css(tokens.muted_text)));
        return button;
    };
    auto *up = make_step(QStringLiteral("+"));
    auto *down = make_step(QStringLiteral("−"));
    auto *stepper = new QWidget(wrap);
    auto *step_layout = new QVBoxLayout(stepper);
    step_layout->setContentsMargins(0, 3, 6, 3);
    step_layout->setSpacing(2);
    step_layout->addWidget(up);
    step_layout->addWidget(down);

    auto *layout = new QHBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(field, 1);
    layout->addWidget(stepper, 0, Qt::AlignVCenter);
    QObject::connect(up, &QPushButton::clicked, field, &QSpinBox::stepUp);
    QObject::connect(down, &QPushButton::clicked, field, &QSpinBox::stepDown);
    QObject::connect(field, &QSpinBox::valueChanged, wrap, [field, up, down] {
        up->setEnabled(field->value() < field->maximum());
        down->setEnabled(field->value() > field->minimum());
    });
    return wrap;
}

QWidget *make_field_block(QWidget *parent, const QString &caption,
        QWidget *field, const QString &help = {}) {
    auto *block = new QWidget(parent);
    auto *layout = new QVBoxLayout(block);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(make_label(block, caption,
        "lingtai_setup_review_field_label", 12, QFont::DemiBold,
        value_css(block)));
    layout->addWidget(field);
    if (!help.isEmpty()) {
        auto *note = make_label(block, help, "lingtai_setup_review_field_help",
            11, QFont::Normal, muted_css(block));
        note->setWordWrap(true);
        layout->addWidget(note);
    }
    return block;
}

QFrame *make_rule(QWidget *parent) {
    auto *rule = new QFrame(parent);
    rule->setFrameShape(QFrame::HLine);
    rule->setStyleSheet(QStringLiteral("color: %1;")
        .arg(setup_color_css(setup_tokens(parent->palette()).border)));
    return rule;
}

QString prompt_path(const QString &kind, const QString &lang) {
    return QDir(lingtai_global_dir()).filePath(kind + QLatin1Char('/') + lang
        + QLatin1Char('/') + (kind == QLatin1String("covenant")
            ? QStringLiteral("covenant.md")
            : QStringLiteral("soul-flow.md")));
}

SetupToggle *make_switch(QWidget *parent, const char *name) {
    auto *toggle = new SetupToggle(parent);
    toggle->setObjectName(name);
    return toggle;
}

} // namespace

AgentConfigPage::AgentConfigPage(QWidget *parent)
: QWidget(parent) {
    setObjectName("lingtai_setup_review_page");
    setAutoFillBackground(false);
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("lingtai_setup_review_scroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setFrameShadow(QFrame::Plain);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setAutoFillBackground(false);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setAttribute(Qt::WA_StyledBackground, false);
    scroll->viewport()->setAttribute(Qt::WA_StyledBackground, false);
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollArea > QWidget { background: transparent; }"));
    scroll->viewport()->setStyleSheet(QStringLiteral(
        "background: transparent; border: none;"));
    auto *body = new QWidget(scroll);
    body->setAutoFillBackground(false);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 8, 16);
    layout->setSpacing(14);

    const auto tokens = setup_tokens(palette());
    layout->addWidget(make_label(body, QStringLiteral("Configure your agent"),
        "lingtai_setup_review_heading", 22, QFont::DemiBold, value_css(body)));
    auto *subtitle = make_label(body,
        QStringLiteral("Name the agent and review how it will run."),
        "lingtai_setup_review_subtitle", 13, QFont::Normal, muted_css(body));
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);

    layout->addWidget(make_label(body, QStringLiteral("Identity"),
        "lingtai_setup_review_section_identity", 16, QFont::DemiBold,
        value_css(body)));
    name_ = make_field(body, "lingtai_setup_review_agent_name");
    folder_ = make_field(body, "lingtai_setup_review_folder_name");
    language_ = new QComboBox(body);
    language_->setObjectName("lingtai_setup_review_language");
    language_->setFixedHeight(34);
    language_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    language_->setAttribute(Qt::WA_MacShowFocusRect, false);
    if (auto *fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        language_->setStyle(fusion);
    }
    language_->setStyleSheet(setup_combo_css(tokens));
    language_->addItem(QStringLiteral("English (en)"), QStringLiteral("en"));
    language_->addItem(QStringLiteral("现代汉语 (zh)"), QStringLiteral("zh"));
    language_->addItem(QStringLiteral("文言 (wen)"), QStringLiteral("wen"));
    auto *identity = new QWidget(body);
    auto *identity_grid = new QGridLayout(identity);
    identity_grid->setContentsMargins(0, 0, 0, 0);
    identity_grid->setHorizontalSpacing(12);
    identity_grid->setVerticalSpacing(6);
    identity_grid->setColumnStretch(0, 1);
    identity_grid->setColumnStretch(1, 1);
    identity_grid->setColumnStretch(2, 1);
    auto add_identity_caption = [&](int column, const QString &text) {
        identity_grid->addWidget(make_label(identity, text,
            "lingtai_setup_review_field_label", 12, QFont::DemiBold,
            value_css(identity)),
            0, column, Qt::AlignLeft | Qt::AlignBottom);
    };
    add_identity_caption(0, QStringLiteral("Agent name"));
    add_identity_caption(1, QStringLiteral("Folder name"));
    add_identity_caption(2, QStringLiteral("Language"));
    identity_grid->addWidget(name_, 1, 0);
    identity_grid->addWidget(folder_, 1, 1);
    identity_grid->addWidget(language_, 1, 2);
    auto *folder_help = make_label(identity,
        QStringLiteral("Used for the agent's local workspace."),
        "lingtai_setup_review_field_help", 11, QFont::Normal, muted_css(identity));
    folder_help->setWordWrap(true);
    identity_grid->addWidget(folder_help, 2, 1, Qt::AlignTop);
    layout->addWidget(identity);

    layout->addWidget(make_rule(body));
    layout->addWidget(make_label(body, QStringLiteral("Runtime"),
        "lingtai_setup_review_section_runtime", 16, QFont::DemiBold,
        value_css(body)));
    context_limit_ = make_spin(body, "lingtai_setup_review_context_limit",
        0, 10000000);
    context_limit_->setSingleStep(10000);
    soul_cadence_ = make_spin(body, "lingtai_setup_review_soul_cadence",
        0, 86400);
    soul_cadence_->setSpecialValueText(QStringLiteral("Not set"));
    soul_cadence_->setValue(0);
    max_rpm_ = make_spin(body, "lingtai_setup_review_max_rpm", 0, 100000);
    max_aed_ = make_spin(body, "lingtai_setup_review_max_aed", 1, 100);
    auto *runtime = new QWidget(body);
    auto *runtime_grid = new QGridLayout(runtime);
    runtime_grid->setContentsMargins(0, 0, 0, 0);
    runtime_grid->setHorizontalSpacing(12);
    runtime_grid->setVerticalSpacing(10);
    runtime_grid->setColumnStretch(0, 1);
    runtime_grid->setColumnStretch(1, 1);
    runtime_grid->addWidget(make_field_block(runtime, QStringLiteral("Context limit"),
        make_stepper_spin(runtime, context_limit_),
        QStringLiteral("Maximum shared context window.")), 0, 0);
    soul_cadence_block_ = make_field_block(runtime, QStringLiteral("Soul cadence"),
        make_stepper_spin(runtime, soul_cadence_),
        QStringLiteral("Seconds between autonomous reflections."));
    runtime_grid->addWidget(soul_cadence_block_, 0, 1);
    runtime_grid->addWidget(make_field_block(runtime, QStringLiteral("Max RPM"),
        make_stepper_spin(runtime, max_rpm_),
        QStringLiteral("Shared request limit for agents using the same provider; 0 means no limit.")),
        1, 0);
    runtime_grid->addWidget(make_field_block(runtime, QStringLiteral("AED max attempts"),
        make_stepper_spin(runtime, max_aed_),
        QStringLiteral("Recovery attempts before fallback or sleep.")), 1, 1);
    layout->addWidget(runtime);

    layout->addWidget(make_rule(body));
    layout->addWidget(make_label(body, QStringLiteral("Authority"),
        "lingtai_setup_review_section_authority", 16, QFont::DemiBold,
        value_css(body)));
    const auto add_authority = [&](const QString &title, const QString &help,
            const char *name, QCheckBox **slot) {
        auto *row = new QWidget(body);
        auto *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 6, 0, 6);
        auto *copy = new QWidget(row);
        auto *copy_layout = new QVBoxLayout(copy);
        copy_layout->setContentsMargins(0, 0, 0, 0);
        copy_layout->setSpacing(2);
        copy_layout->addWidget(make_label(copy, title,
            "lingtai_setup_review_authority_title", 13, QFont::DemiBold,
            value_css(copy)));
        auto *note = make_label(copy, help, "lingtai_setup_review_authority_help",
            11, QFont::Normal, muted_css(copy));
        note->setWordWrap(true);
        copy_layout->addWidget(note);
        *slot = make_switch(row, name);
        row_layout->addWidget(copy, 1);
        row_layout->addWidget(*slot, 0, Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(row);
        return note;
    };
    add_authority(QStringLiteral("Karma"),
        QStringLiteral("May manage other agents."),
        "lingtai_setup_review_karma", &karma_);
    add_authority(QStringLiteral("Nirvana"),
        QStringLiteral("May permanently destroy agents."),
        "lingtai_setup_review_nirvana", &nirvana_);
    soul_flow_help_ = add_authority(QStringLiteral("Soul flow"),
        QStringLiteral("Autonomous reflection on recent work and prior selves."),
        "lingtai_setup_review_soul_flow", &soul_flow_);
    soul_flow_help_->setObjectName("lingtai_setup_review_soul_flow_help");

    layout->addWidget(make_rule(body));
    layout->addWidget(make_label(body, QStringLiteral("Prompts"),
        "lingtai_setup_review_section_prompts", 16, QFont::DemiBold,
        value_css(body)));
    const auto add_path_row = [&](const QString &caption, const char *field_name,
            const char *button_name, QLineEdit **slot) {
        auto *row = new QWidget(body);
        auto *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(8);
        *slot = make_field(row, field_name);
        auto *choose = new QPushButton(QStringLiteral("Choose…"), row);
        choose->setObjectName(button_name);
        choose->setFixedHeight(34);
        apply_setup_secondary_button(choose, tokens);
        row_layout->addWidget(*slot, 1);
        row_layout->addWidget(choose);
        layout->addWidget(make_field_block(body, caption, row));
        connect(choose, &QPushButton::clicked, this, [this, slot] {
            const auto selected = QFileDialog::getOpenFileName(this,
                QStringLiteral("Choose prompt file"),
                (*slot)->text(),
                QStringLiteral("Markdown (*.md);;All files (*)"));
            if (selected.isEmpty()) return;
            (*slot)->setText(selected);
            if (*slot == covenant_) covenant_dirty_ = true;
            if (*slot == soul_path_) soul_path_dirty_ = true;
        });
    };
    add_path_row(QStringLiteral("Covenant path"),
        "lingtai_setup_review_covenant",
        "lingtai_setup_review_covenant_choose", &covenant_);
    add_path_row(QStringLiteral("Soul flow path"),
        "lingtai_setup_review_soul_path",
        "lingtai_setup_review_soul_path_choose", &soul_path_);
    comment_ = new QPlainTextEdit(body);
    comment_->setObjectName("lingtai_setup_review_comment");
    comment_->setPlaceholderText(QStringLiteral("Optional instructions for this agent."));
    comment_->setFixedHeight(96);
    apply_setup_plain_text(comment_, tokens);
    layout->addWidget(make_field_block(body, QStringLiteral("Comment"), comment_));
    auto *prompt_note = make_label(body,
        QStringLiteral("Prompt files can be changed later in agent settings."),
        "lingtai_setup_review_prompt_note", 11, QFont::Normal, muted_css(body));
    prompt_note->setWordWrap(true);
    layout->addWidget(prompt_note);
    layout->addStretch();
    scroll->setWidget(body);
    root->addWidget(scroll, 1);

    footer_host_ = new QVBoxLayout;
    footer_host_->setContentsMargins(0, 10, 0, 0);
    footer_host_->setSpacing(8);
    status_ = make_label(this, QString(), "lingtai_setup_review_status",
        12, QFont::Normal, muted_css(this));
    status_->setWordWrap(true);
    footer_host_->addWidget(status_);
    auto *actions = new QHBoxLayout;
    auto *back = new QPushButton(QStringLiteral("Back"), this);
    back->setObjectName("lingtai_setup_review_back");
    back->setFixedHeight(34);
    apply_setup_secondary_button(back, tokens);
    commit_ = new QPushButton(QStringLiteral("Create orchestrator"), this);
    commit_->setObjectName("lingtai_bootstrap_create_start");
    commit_->setAccessibleName(QStringLiteral("Create orchestrator"));
    commit_->setFixedHeight(34);
    commit_->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: white; border: none; "
        "border-radius: 6px; padding: 0 16px; font-weight: 600; }").arg(accent_css(this)));
    actions->addWidget(back);
    actions->addStretch();
    actions->addWidget(commit_);
    footer_host_->addLayout(actions);
    root->addLayout(footer_host_);

    connect(back, &QPushButton::clicked, this, &AgentConfigPage::back_requested);
    connect(commit_, &QPushButton::clicked, this, &AgentConfigPage::create_requested);
    connect(name_, &QLineEdit::textEdited, this, [this](const QString &text) {
        if (!folder_dirty_) folder_->setText(text);
    });
    connect(folder_, &QLineEdit::textEdited, this, [this] {
        folder_dirty_ = true;
    });
    connect(language_, &QComboBox::currentIndexChanged, this, [this] {
        update_prompt_paths();
    });
    connect(covenant_, &QLineEdit::textEdited, this, [this] {
        covenant_dirty_ = true;
    });
    connect(soul_path_, &QLineEdit::textEdited, this, [this] {
        soul_path_dirty_ = true;
    });
    connect(soul_flow_, &QCheckBox::toggled, this, [this](bool on) {
        const auto enabled = on && soul_flow_->isEnabled();
        soul_cadence_->setEnabled(enabled);
        if (soul_cadence_block_) soul_cadence_block_->setEnabled(enabled);
    });
    context_limit_->setValue(500000);
    max_rpm_->setValue(60);
    max_aed_->setValue(5);
    karma_->setChecked(true);
    nirvana_->setChecked(false);
    soul_flow_->setChecked(false);
    soul_cadence_->setEnabled(false);
    if (soul_cadence_block_) soul_cadence_block_->setEnabled(false);
    apply_chrome();
}

void AgentConfigPage::apply_chrome() {
    const auto tokens = setup_tokens(palette());
    const auto value = setup_color_css(tokens.value_text);
    const auto muted = setup_color_css(tokens.muted_text);

    for (auto *field : findChildren<QLineEdit *>()) {
        if (field->objectName().startsWith(QStringLiteral("lingtai_setup_review_"))) {
            apply_setup_line_edit(field, tokens);
        }
    }
    if (language_) language_->setStyleSheet(setup_combo_css(tokens));
    apply_setup_plain_text(comment_, tokens);
    for (auto *spin : findChildren<QSpinBox *>()) {
        spin->setStyleSheet(QStringLiteral(
            "QSpinBox { border: none; background: transparent; padding: 0 10px; "
            "color: %1; }").arg(value));
    }
    for (auto *wrap : findChildren<QWidget *>(
            QStringLiteral("lingtai_setup_review_spin_wrap"))) {
        wrap->setStyleSheet(setup_spin_wrap_css(tokens));
        for (auto *button : wrap->findChildren<QPushButton *>()) {
            button->setStyleSheet(QStringLiteral(
                "QPushButton { background: %1; border: none; border-radius: 4px; "
                "color: %2; font-size: 10px; font-weight: 700; padding: 0; }"
                "QPushButton:hover { background: %3; }"
                "QPushButton:pressed { background: %3; }"
                "QPushButton:disabled { color: %4; background: %1; }")
                .arg(setup_color_css(tokens.header),
                    setup_color_css(tokens.selection_accent),
                    setup_color_css(tokens.selected_row), muted));
        }
    }
    for (auto *rule : findChildren<QFrame *>()) {
        if (rule->frameShape() == QFrame::HLine) {
            rule->setStyleSheet(QStringLiteral("color: %1;")
                .arg(setup_color_css(tokens.border)));
        }
    }
    for (auto *label : findChildren<QLabel *>()) {
        const auto name = label->objectName();
        const auto secondary = name.contains(QStringLiteral("subtitle"))
            || name.contains(QStringLiteral("help"))
            || name.contains(QStringLiteral("note"))
            || name.endsWith(QStringLiteral("_status"));
        label->setStyleSheet(QStringLiteral("color: %1;")
            .arg(secondary ? muted : value));
    }
    for (const auto *name : {
            "lingtai_setup_review_covenant_choose",
            "lingtai_setup_review_soul_path_choose",
            "lingtai_setup_review_back"}) {
        apply_setup_secondary_button(findChild<QPushButton *>(name), tokens);
    }
    apply_setup_primary_button(commit_);
}

void AgentConfigPage::load(const QString &default_preset, int allowed_count) {
    set_existing_mode(false);
    folder_dirty_ = false;
    covenant_dirty_ = false;
    soul_path_dirty_ = false;
    name_->setText(default_preset);
    folder_->setText(default_preset);
    {
        const QSignalBlocker block(language_);
        language_->setCurrentIndex(0);
    }
    context_limit_->setValue(500000);
    soul_cadence_->setValue(0);
    max_rpm_->setValue(60);
    max_aed_->setValue(5);
    karma_->setChecked(true);
    nirvana_->setChecked(false);
    soul_flow_->setChecked(false);
    soul_cadence_->setEnabled(false);
    if (soul_cadence_block_) soul_cadence_block_->setEnabled(false);
    comment_->clear();
    update_prompt_paths();
    update_status(default_preset, allowed_count);
}

void AgentConfigPage::load_existing(const AgentSetupDraft &draft,
        const QString &folder_name, const QString &default_preset,
        int allowed_count) {
    set_existing_mode(true);
    folder_dirty_ = true;
    covenant_dirty_ = true;
    soul_path_dirty_ = true;
    name_->setText(QString::fromStdString(draft.agent_name));
    folder_->setText(folder_name);
    const auto language = QString::fromStdString(draft.language);
    auto language_index = language_->findData(language);
    if (language_index < 0 && !language.isEmpty()) {
        language_->addItem(language, language);
        language_index = language_->count() - 1;
    }
    {
        const QSignalBlocker block(language_);
        language_->setCurrentIndex(language_index >= 0 ? language_index : 0);
    }
    context_limit_->setValue(static_cast<int>(draft.context_limit));
    soul_cadence_->setValue(draft.soul_delay
        ? static_cast<int>(*draft.soul_delay) : 0);
    max_rpm_->setValue(static_cast<int>(draft.max_rpm));
    max_aed_->setValue(static_cast<int>(draft.max_aed_attempts));
    karma_->setChecked(draft.karma);
    nirvana_->setChecked(draft.nirvana);
    soul_flow_->setChecked(draft.soul_flow_enabled);
    soul_cadence_->setEnabled(draft.soul_flow_enabled);
    if (soul_cadence_block_) {
        soul_cadence_block_->setEnabled(draft.soul_flow_enabled);
    }
    covenant_->setText(QString::fromStdString(draft.covenant_file));
    // Existing setup owns comment_file, not free-form comment content.
    comment_->setPlainText(QString::fromStdString(draft.comment_file));
    soul_path_->clear();
    soul_path_->setReadOnly(true);
    update_status(default_preset, allowed_count);
}

AgentSetupDraft AgentConfigPage::apply_to_draft(AgentSetupDraft draft) const {
    draft.language = language().toStdString();
    draft.context_limit = context_limit_->value();
    draft.soul_delay = soul_cadence_->value() > 0
        ? std::optional<double>(soul_cadence_->value()) : std::nullopt;
    draft.max_rpm = max_rpm_->value();
    draft.max_aed_attempts = max_aed_->value();
    draft.karma = karma_->isChecked();
    draft.nirvana = nirvana_->isChecked();
    draft.soul_flow_enabled = soul_flow_->isChecked();
    draft.covenant_file = covenant_->text().trimmed().toStdString();
    const auto comment = comment_->toPlainText();
    draft.comment_file = (existing_mode_ ? comment.trimmed() : comment)
        .toStdString();
    return draft;
}

void AgentConfigPage::set_existing_mode(bool existing) {
    existing_mode_ = existing;
    if (!existing) {
        while (language_->count() > 3) language_->removeItem(3);
    }
    name_->setReadOnly(existing);
    folder_->setReadOnly(existing);
    name_->setAccessibleDescription(existing
        ? QStringLiteral("Existing Agent name; cannot be changed during setup.")
        : QString());
    folder_->setAccessibleDescription(existing
        ? QStringLiteral("Existing Agent folder; cannot be changed during setup.")
        : QString());
    soul_path_->setReadOnly(existing);
    soul_flow_->setEnabled(existing);
    if (soul_flow_help_) {
        soul_flow_help_->setText(existing
            ? QStringLiteral(
                "Autonomous reflection on recent work and prior selves.")
            : QStringLiteral(
                "Available after creation from /setup; New Project does not "
                "change shared runtime Soul flow."));
    }
    const auto cadence_enabled = existing && soul_flow_->isChecked();
    soul_cadence_->setEnabled(cadence_enabled);
    if (soul_cadence_block_) {
        soul_cadence_block_->setEnabled(cadence_enabled);
    }
    if (commit_) {
        const auto text = existing
            ? QStringLiteral("Save setup")
            : QStringLiteral("Create orchestrator");
        commit_->setText(text);
        commit_->setAccessibleName(text);
    }
}

QString AgentConfigPage::agent_name() const {
    return name_ ? name_->text().trimmed() : QString();
}

QString AgentConfigPage::folder_name() const {
    return folder_ ? folder_->text().trimmed() : QString();
}

QString AgentConfigPage::language() const {
    if (!language_) return QStringLiteral("en");
    const auto code = language_->currentData().toString();
    return code.isEmpty() ? QStringLiteral("en") : code;
}

QString AgentConfigPage::create_status() const {
    return status_ ? status_->text() : QString();
}

void AgentConfigPage::install_dialog_status(QWidget *status) {
    if (!status || !footer_host_) return;
    footer_host_->insertWidget(0, status);
}

void AgentConfigPage::update_prompt_paths() {
    const auto lang = language();
    if (!covenant_dirty_) covenant_->setText(prompt_path(QStringLiteral("covenant"), lang));
    if (!soul_path_dirty_) soul_path_->setText(prompt_path(QStringLiteral("soul"), lang));
}

void AgentConfigPage::update_status(const QString &default_preset, int allowed_count) {
    status_->setText(QStringLiteral("Default preset: %1 · Runtime alternatives: %2 enabled")
        .arg(default_preset.isEmpty() ? QStringLiteral("—") : default_preset)
        .arg(allowed_count));
}

} // namespace lingtai::desktop
