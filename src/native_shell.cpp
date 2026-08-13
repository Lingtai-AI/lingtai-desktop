#include "native_shell.h"

#include "direct_conversation_history.h"
#include "direct_mail_publisher.h"

#include "ui/rp_widget.h"
#include "ui/widgets/rp_window.h"

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtGui/QFont>
#include <QtWidgets/QBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>

#include <algorithm>
#include <utility>

namespace lingtai::desktop {
namespace {

constexpr auto kSidebarWidth = 264;
constexpr auto kMinimumWindowWidth = 720;
constexpr auto kMinimumWindowHeight = 480;
constexpr auto kDefaultWindowWidth = 1100;
constexpr auto kDefaultWindowHeight = 720;

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

QString manifest_diagnostic_text(AgentManifestDiagnosticKind diagnostic) {
    using Kind = AgentManifestDiagnosticKind;
    switch (diagnostic) {
    case Kind::none: return QStringLiteral("none");
    case Kind::unsafe_symlink: return QStringLiteral("unsafe symlink");
    case Kind::unreadable: return QStringLiteral("unreadable");
    case Kind::invalid_json: return QStringLiteral("invalid JSON");
    case Kind::not_object: return QStringLiteral("JSON root is not an object");
    }
    return QStringLiteral("unreadable");
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

} // namespace

NativeShell::NativeShell()
: window_(std::make_unique<Ui::RpWindow>()) {
    window_->setObjectName("lingtai_desktop_window");
    window_->setTitle(QStringLiteral("LingTai Desktop"));
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

    auto *sidebar = new Ui::RpWidget(body);
    sidebar->setObjectName("lingtai_desktop_sidebar");
    sidebar->setAccessibleName(QStringLiteral("Workspace navigation"));
    sidebar->setFixedWidth(kSidebarWidth);
    shell_layout->addWidget(sidebar);

    auto *sidebar_layout = new QVBoxLayout(sidebar);
    sidebar_layout->setContentsMargins(28, 34, 28, 30);
    sidebar_layout->setSpacing(10);
    auto *brand = make_label(
        sidebar,
        QStringLiteral("LingTai"),
        "lingtai_sidebar_brand",
        20,
        QFont::DemiBold);
    auto *section = make_label(
        sidebar,
        QStringLiteral("Workspace"),
        "lingtai_sidebar_workspace_label",
        11,
        QFont::Medium);
    section->setAccessibleDescription(QStringLiteral(
        "Project and Agent navigation will appear here after a project opens."));
    sidebar_layout->addWidget(brand);
    sidebar_layout->addSpacing(26);
    sidebar_layout->addWidget(section);
    sidebar_layout->addStretch();

    auto *content = new Ui::RpWidget(body);
    content->setObjectName("lingtai_desktop_content");
    content->setAccessibleName(QStringLiteral("Workspace content"));
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    shell_layout->addWidget(content, 1);

    auto *content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(52, 44, 52, 44);
    content_layout->setSpacing(8);
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
    content_layout->addSpacing(40);

    open_error_surface_ = new Ui::RpWidget(content);
    open_error_surface_->setObjectName("lingtai_project_open_error_surface");
    auto *error_layout = new QVBoxLayout(open_error_surface_);
    error_layout->setContentsMargins(0, 0, 0, 0);
    auto *open_error = make_label(
        open_error_surface_,
        QString(),
        "lingtai_project_open_error",
        12,
        QFont::Medium);
    open_error->setAccessibleName(QStringLiteral("Project open error"));
    error_layout->addWidget(open_error);
    open_error_surface_->hide();
    content_layout->addWidget(open_error_surface_);

    empty_route_ = new Ui::RpWidget(content);
    empty_route_->setObjectName("lingtai_empty_workspace_route");
    empty_route_->setAccessibleName(QStringLiteral("No project open"));
    empty_route_->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Expanding);
    content_layout->addWidget(empty_route_, 1);

    auto *empty_layout = new QVBoxLayout(empty_route_);
    empty_layout->setContentsMargins(0, 0, 0, 0);
    empty_layout->setSpacing(12);
    auto *empty_title = make_label(
        empty_route_,
        QStringLiteral("No project open"),
        "lingtai_no_project_title",
        18,
        QFont::DemiBold);
    auto *empty_detail = make_label(
        empty_route_,
        QStringLiteral("Open a LingTai project to inspect its Agents."),
        "lingtai_no_project_detail",
        12);
    auto *open_button = new QPushButton(
        QStringLiteral("Open Project…"), empty_route_);
    open_button->setObjectName("lingtai_open_project_button");
    open_button->setAccessibleName(QStringLiteral("Open Project"));
    open_button->setAccessibleDescription(QStringLiteral(
        "Request a project location. No project is changed by this request."));
    open_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    QObject::connect(open_button, &QPushButton::clicked, [this] {
        request_open_project();
    });
    empty_layout->addStretch();
    empty_layout->addWidget(empty_title);
    empty_layout->addWidget(empty_detail);
    empty_layout->addSpacing(8);
    empty_layout->addWidget(open_button);
    empty_layout->addStretch(2);

    project_route_ = new Ui::RpWidget(content);
    project_route_->setObjectName("lingtai_project_route");
    project_route_->setAccessibleName(QStringLiteral("Project"));
    project_route_->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Expanding);
    project_route_->hide();
    content_layout->addWidget(project_route_, 1);

    auto *project_layout = new QVBoxLayout(project_route_);
    project_layout->setContentsMargins(0, 0, 0, 0);
    project_layout->setSpacing(12);
    project_layout->addWidget(make_label(
        project_route_,
        QStringLiteral("Project"),
        "lingtai_project_route_heading",
        18,
        QFont::DemiBold));
    project_layout->addWidget(make_label(
        project_route_, QString(), "lingtai_project_root", 11));
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
    directory_layout->setContentsMargins(0, 12, 0, 0);
    directory_layout->setSpacing(20);

    auto *roster = new Ui::RpWidget(directory);
    roster->setObjectName("lingtai_agent_roster");
    roster->setAccessibleName(QStringLiteral("Agent roster"));
    roster->setMinimumWidth(180);
    directory_layout->addWidget(roster, 1);
    auto *roster_layout = new QVBoxLayout(roster);
    roster_layout->setContentsMargins(0, 0, 0, 0);
    roster_layout->setSpacing(8);
    roster_layout->addWidget(make_label(
        roster, QStringLiteral("Agents"), "lingtai_agent_roster_heading", 14,
        QFont::DemiBold));
    auto *roster_state = make_label(
        roster, QString(), "lingtai_agent_roster_state", 10);
    roster_state->setAccessibleName(QStringLiteral("Agent roster status"));
    roster_layout->addWidget(roster_state);
    auto *roster_scroll = new QScrollArea(roster);
    roster_scroll->setObjectName("lingtai_agent_roster_scroll");
    roster_scroll->setAccessibleName(QStringLiteral("Agent roster rows"));
    roster_scroll->setWidgetResizable(true);
    roster_layout->addWidget(roster_scroll, 1);
    roster_rows_ = new Ui::RpWidget(roster_scroll);
    roster_rows_->setObjectName("lingtai_agent_roster_rows");
    roster_rows_->setAccessibleName(QStringLiteral("Agent roster rows"));
    auto *rows_layout = new QVBoxLayout(roster_rows_);
    rows_layout->setContentsMargins(0, 0, 0, 0);
    rows_layout->setSpacing(6);
    roster_scroll->setWidget(roster_rows_);

    // The detail column carries more evidence than any window is tall, so it
    // scrolls like the roster instead of overflowing and overpainting itself.
    auto *detail_scroll = new QScrollArea(directory);
    detail_scroll->setObjectName("lingtai_agent_detail_scroll");
    detail_scroll->setAccessibleName(QStringLiteral("Selected Agent detail"));
    detail_scroll->setWidgetResizable(true);
    directory_layout->addWidget(detail_scroll, 2);
    auto *detail = new Ui::RpWidget(detail_scroll);
    detail->setObjectName("lingtai_agent_detail");
    detail->setAccessibleName(QStringLiteral("Selected Agent detail"));
    detail_scroll->setWidget(detail);
    auto *detail_layout = new QVBoxLayout(detail);
    detail_layout->setContentsMargins(0, 0, 0, 0);
    detail_layout->setSpacing(8);
    detail_layout->addWidget(make_label(
        detail, QStringLiteral("Selected Agent"),
        "lingtai_agent_detail_heading", 14, QFont::DemiBold));
    auto *detail_key = make_label(
        detail, QString(), "lingtai_selected_agent_key", 12, QFont::Medium);
    detail_key->setAccessibleName(QStringLiteral("Selected Agent key"));
    detail_layout->addWidget(detail_key);
    auto *presentation_name = make_label(
        detail, QString(), "lingtai_selected_agent_presentation_name", 12,
        QFont::Medium);
    presentation_name->setAccessibleName(
        QStringLiteral("Selected Agent presentation name"));
    detail_layout->addWidget(presentation_name);
    // The conversation is the product, so it sits directly under the selected
    // Agent's name rather than below the source-facts labels.
    detail_layout->addWidget(make_label(
        detail, QStringLiteral("Conversation"),
        "lingtai_selected_agent_conversation_heading", 12, QFont::DemiBold));
    auto *conversation = new QPlainTextEdit(detail);
    conversation->setObjectName("lingtai_selected_agent_conversation");
    conversation->setAccessibleName(
        QStringLiteral("Selected Agent conversation"));
    conversation->setAccessibleDescription(QStringLiteral(
        "The current direct conversation with the selected Agent, shown "
        "read-only as plain text."));
    conversation->setReadOnly(true);
    conversation->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    conversation->setMinimumHeight(180);
    conversation->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    detail_layout->addWidget(conversation, 1);

    // The smallest visible composer, directly under the conversation it
    // sends into: one plain-text input and one explicit Send action.
    auto *composer = new Ui::RpWidget(detail);
    composer->setObjectName("lingtai_composer");
    composer->setAccessibleName(QStringLiteral("Send a message"));
    auto *composer_layout = new QHBoxLayout(composer);
    composer_layout->setContentsMargins(0, 0, 0, 0);
    composer_layout->setSpacing(8);
    auto *composer_input = new QLineEdit(composer);
    composer_input->setObjectName("lingtai_composer_input");
    composer_input->setAccessibleName(QStringLiteral("Message"));
    composer_input->setPlaceholderText(QStringLiteral("Message…"));
    composer_input->setEnabled(false);
    composer_layout->addWidget(composer_input, 1);
    auto *send_button = new QPushButton(
        QStringLiteral("Send"), composer);
    send_button->setObjectName("lingtai_composer_send_button");
    send_button->setAccessibleName(QStringLiteral("Send message"));
    send_button->setEnabled(false);
    QObject::connect(send_button, &QPushButton::clicked, [this] {
        handle_send_message();
    });
    composer_layout->addWidget(send_button);
    detail_layout->addWidget(composer);
    auto *composer_status = make_label(
        detail, QString(), "lingtai_composer_status", 10);
    composer_status->setAccessibleName(QStringLiteral("Send status"));
    detail_layout->addWidget(composer_status);

    auto *conversation_state = make_label(
        detail, QString(), "lingtai_selected_agent_conversation_state", 10);
    conversation_state->setAccessibleName(
        QStringLiteral("Selected Agent conversation state"));
    detail_layout->addWidget(conversation_state);
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

    refresh_route();
    render_roster();
}

NativeShell::~NativeShell() = default;

void NativeShell::show() {
    refresh_route();
    window_->show();
}

void NativeShell::show_offscreen() {
    refresh_route();
    window_->setAttribute(Qt::WA_DontShowOnScreen, true);
    window_->show();
}

void NativeShell::set_open_project_request_handler(
        OpenProjectRequestHandler handler) {
    open_project_request_handler_ = std::move(handler);
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
    if (selected_key) {
        static_cast<void>(selection_state_.select_agent(*selected_key));
    }
    agents_ = std::move(agents);
    window_->findChild<QLabel *>("lingtai_project_root")
        ->setText(path_text(canonical_root));
    render_roster();
    auto *selection_error = window_->findChild<QLabel *>(
        "lingtai_agent_selection_error");
    selection_error->clear();
    selection_error->hide();
    window_->findChild<QLabel *>("lingtai_project_open_error")->clear();
    open_error_surface_->hide();
    reset_composer();
    refresh_route();
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
    const auto *sidebar = window_->findChild<Ui::RpWidget *>(
        "lingtai_desktop_sidebar");
    const auto *content = window_->findChild<Ui::RpWidget *>(
        "lingtai_desktop_content");
    return window_->objectName() == "lingtai_desktop_window"
        && window_->body().get()->objectName() == "lingtai_desktop_body"
        && sidebar && sidebar->objectName() == "lingtai_desktop_sidebar"
        && content && content->objectName() == "lingtai_desktop_content"
        && empty_route_->isVisible()
        && window_->testAttribute(Qt::WA_DontShowOnScreen)
        && window_->isVisible();
}

void NativeShell::request_open_project() {
    if (open_project_request_handler_) {
        open_project_request_handler_();
    }
}

void NativeShell::render_roster() {
    auto *state = window_->findChild<QLabel *>("lingtai_agent_roster_state");
    auto *selected_key = window_->findChild<QLabel *>(
        "lingtai_selected_agent_key");
    auto *presentation_name = window_->findChild<QLabel *>(
        "lingtai_selected_agent_presentation_name");
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
    if (!state || !selected_key || !presentation_name || !manifest_identity
        || !manifest_llm || !manifest_capabilities || !status_activity
        || !status_context || !selected_facts) {
        return;
    }

    // roster_rows_ and its layout are stored pointers the constructor always
    // sets before render_roster() can run, so no null/no-layout branch is
    // real here; unlike the findChild lookups above, this is not speculative.
    auto *layout = roster_rows_->layout();
    while (auto *child = layout->takeAt(0)) {
        delete child->widget();
        delete child;
    }

    state->setText(agents_.scan != AgentScanState::complete
        ? QStringLiteral("Roster unavailable")
        : agents_.items.empty()
            ? QStringLiteral("No Agents found — scan complete")
            : QStringLiteral("%1 Agent(s) — scan complete")
                .arg(agents_.items.size()));

    const auto selected = selection_state_.selected_agent_directory_key();
    const AgentRow *detail_item = nullptr;
    for (auto index = std::size_t{0}; index != agents_.items.size(); ++index) {
        const auto &item = agents_.items[index];
        const auto facts = QStringLiteral("%1 — %2 — %3")
            .arg(manifest_text(item.manifest_kind), role_text(item.role),
                presence_text(item.presence));
        auto button_key = path_text(item.directory_key);
        button_key.replace(QLatin1Char('&'), QStringLiteral("&&"));
        auto *row = new QPushButton(
            QStringLiteral("%1\n%2\nmanifest diagnostic: %3")
                .arg(button_key, facts,
                    manifest_diagnostic_text(item.manifest_diagnostic)),
            roster_rows_);
        row->setObjectName(
            QStringLiteral("lingtai_agent_row_%1").arg(index));
        row->setAccessibleName(
            QStringLiteral("Agent %1").arg(path_text(item.directory_key)));
        row->setAccessibleDescription(facts);
        row->setProperty("directory_key", path_text(item.directory_key));
        row->setCheckable(true);
        row->setChecked(selected && *selected == item.directory_key);
        row->setEnabled(item.manifest_kind == AgentManifestKind::valid);
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        const auto key = item.directory_key;
        QObject::connect(row, &QPushButton::clicked, [this, key] {
            handle_agent_selection(key);
        });
        layout->addWidget(row);
        if (row->isChecked()) detail_item = &item;
    }
    if (auto *box = qobject_cast<QBoxLayout *>(layout)) box->addStretch();

    if (!detail_item) {
        selected_key->setText(QStringLiteral("No Agent selected"));
        presentation_name->clear();
        manifest_identity->clear();
        manifest_llm->clear();
        manifest_capabilities->clear();
        status_activity->clear();
        status_context->clear();
        selected_facts->setText(QStringLiteral(
            "Choose a valid manifest row to inspect its detail."));
        render_conversation();
        return;
    }
    selected_key->setText(path_text(detail_item->directory_key));
    const auto &identity = detail_item->identity;
    presentation_name->setText(identity && identity->nickname
            ? QString::fromStdString(*identity->nickname)
        : identity && identity->true_name
            ? QString::fromStdString(*identity->true_name)
            : path_text(detail_item->directory_key));
    if (identity) {
        manifest_identity->setText(QStringLiteral(
            "Manifest identity\ntrue name: %1\naddress: %2\nagent ID: %3\nstate: %4")
            .arg(value_text(identity->true_name), value_text(identity->address),
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
}

// Shows the current direct conversation for whatever the roster just made the
// selected Agent. It reads the human's own mailbox and infers nothing about
// delivery, replies, or unread state.
void NativeShell::render_conversation() {
    auto *surface = window_->findChild<QPlainTextEdit *>(
        "lingtai_selected_agent_conversation");
    auto *state = window_->findChild<QLabel *>(
        "lingtai_selected_agent_conversation_state");
    auto *composer_input = window_->findChild<QLineEdit *>(
        "lingtai_composer_input");
    auto *send_button = window_->findChild<QPushButton *>(
        "lingtai_composer_send_button");
    if (!surface || !state || !composer_input || !send_button) return;
    const auto show = [&](const QString &text, const QString &compact) {
        surface->setPlainText(text);
        state->setText(compact);
    };
    // Composer enablement only; never touches typed text or send status, so a
    // refresh right after a send does not erase the status it just set.
    const auto set_composer_eligible = [&](bool eligible) {
        composer_input->setEnabled(eligible);
        send_button->setEnabled(eligible);
    };

    if (!selection_state_.active_project()
        || !selection_state_.selected_agent_directory_key()) {
        show(QStringLiteral("Select an Agent to see your conversation."),
            QString());
        set_composer_eligible(false);
        return;
    }
    const auto route = resolve_direct_conversation_route(
        *selection_state_.active_project(), agents_,
        selection_state_.selected_agent_directory_key());
    if (!route) {
        show(QStringLiteral(
                "No conversation is available for this selection."),
            QString());
        set_composer_eligible(false);
        return;
    }
    set_composer_eligible(true);

    const auto history = read_direct_conversation(*route);
    const auto *presentation_name = window_->findChild<QLabel *>(
        "lingtai_selected_agent_presentation_name");
    const auto them = presentation_name && !presentation_name->text().isEmpty()
        ? presentation_name->text()
        : path_text(route->target_directory_key);
    auto blocks = QStringList();
    for (const auto &message : history.messages) {
        auto block = QStringLiteral("%1 · %2")
            .arg(message.outgoing ? QStringLiteral("You") : them,
                QString::fromStdString(message.timestamp));
        if (!message.subject.empty()) {
            block += QLatin1Char('\n')
                + QString::fromStdString(message.subject);
        }
        // Message text stays literal: the surface never interprets markup.
        block += QLatin1Char('\n') + QString::fromStdString(message.text);
        blocks.push_back(block);
    }
    const auto count = history.messages.size();
    auto compact = count == 1
        ? QStringLiteral("1 message")
        : QStringLiteral("%1 messages").arg(count);
    if (history.skipped > 0) {
        compact += QStringLiteral(" · %1 skipped").arg(history.skipped);
    }
    show(blocks.isEmpty() ? QStringLiteral("No messages yet.")
                          : blocks.join(QStringLiteral("\n\n")), compact);
}

// Called only when the selected target actually changes (a fresh project open
// or a successful Agent selection), never on an ordinary conversation
// refresh, so a just-set "Queued" status is never wiped by its own refresh.
void NativeShell::reset_composer() {
    if (auto *input = window_->findChild<QLineEdit *>("lingtai_composer_input")) {
        input->clear();
    }
    if (auto *status = window_->findChild<QLabel *>("lingtai_composer_status")) {
        status->clear();
    }
}

// Always resolves the route fresh from current C1/C3 truth rather than
// capturing it once, so a selection change between typing and clicking Send
// can never deliver to a stale target.
void NativeShell::handle_send_message() {
    auto *input = window_->findChild<QLineEdit *>("lingtai_composer_input");
    auto *status = window_->findChild<QLabel *>("lingtai_composer_status");
    if (!input || !status) return;
    const auto text = input->text().trimmed();
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

    const auto result = send_direct_mail(*route, text.toStdString());
    if (result == DirectMailSendResult::queued) {
        input->clear();
        status->setText(QStringLiteral("Queued"));
        render_conversation();
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
    render_roster();
    if (error) {
        error->clear();
        error->hide();
    }
}

ProjectOpenOutcome NativeShell::show_open_error(
        ProjectPathFailure failure,
        std::string message) {
    auto *label = window_->findChild<QLabel *>("lingtai_project_open_error");
    label->setText(QString::fromStdString(message));
    open_error_surface_->show();
    refresh_route();
    return {
        .disposition = ProjectOpenDisposition::failed,
        .failure = failure,
    };
}

void NativeShell::refresh_route() {
    // Both are stored pointers the constructor always sets before
    // refresh_route() can run, so neither branch here is ever reachable.
    empty_route_->setVisible(!selection_state_.active_project().has_value());
    project_route_->setVisible(selection_state_.active_project().has_value());
}

} // namespace lingtai::desktop
