#include "native_shell.h"

#include "compatibility_probe.h"

#include "ui/rp_widget.h"
#include "ui/widgets/rp_window.h"

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtGui/QFont>
#include <QtWidgets/QBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>

#include <algorithm>
#include <type_traits>
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

QString finding_text(CompatibilityFindingKind kind) {
    using Kind = CompatibilityFindingKind;
    switch (kind) {
    case Kind::probe_failed:
        return QStringLiteral("Compatibility checks could not be completed.");
    case Kind::no_agent_requested:
        return QStringLiteral(
            "Select an Agent to complete compatibility checks.");
    case Kind::agent_unavailable:
        return QStringLiteral("The selected Agent directory is unavailable.");
    case Kind::receipt_missing:
        return QStringLiteral(
            "The LingTai install receipt is missing; inspection remains read-only.");
    case Kind::receipt_unreadable:
        return QStringLiteral(
            "The LingTai install receipt cannot be read; inspection remains read-only.");
    case Kind::receipt_malformed:
        return QStringLiteral(
            "The LingTai install receipt is malformed; inspection remains read-only.");
    case Kind::receipt_schema_unsupported:
        return QStringLiteral("The LingTai install receipt schema is unsupported.");
    case Kind::receipt_version_unsupported:
        return QStringLiteral("The LingTai install receipt version is unsupported.");
    case Kind::resolved_missing:
        return QStringLiteral("The resolved Agent manifest is missing.");
    case Kind::resolved_unreadable:
        return QStringLiteral("The resolved Agent manifest cannot be read.");
    case Kind::resolved_malformed:
        return QStringLiteral("The resolved Agent manifest is malformed.");
    case Kind::resolved_schema_unsupported:
        return QStringLiteral("The resolved Agent manifest schema is unsupported.");
    case Kind::resolved_version_unsupported:
        return QStringLiteral("The resolved Agent manifest version is unsupported.");
    case Kind::resolved_source_unsupported:
        return QStringLiteral("The resolved Agent manifest has an unsupported source.");
    case Kind::resolved_manifest_invalid:
        return QStringLiteral("The resolved Agent manifest payload is invalid.");
    case Kind::resolved_stale:
        return QStringLiteral("The resolved Agent manifest is stale.");
    case Kind::resolved_freshness_unavailable:
        return QStringLiteral("Resolved Agent manifest freshness cannot be established.");
    case Kind::init_missing:
        return QStringLiteral("The Agent init.json file is missing.");
    case Kind::init_unreadable:
        return QStringLiteral("The Agent init.json file cannot be read.");
    case Kind::init_malformed:
        return QStringLiteral("The Agent init.json file is malformed.");
    case Kind::init_not_object:
        return QStringLiteral("The Agent init.json root is not an object.");
    case Kind::init_manifest_missing:
        return QStringLiteral("The Agent init.json manifest is missing.");
    case Kind::init_manifest_not_object:
        return QStringLiteral("The Agent init.json manifest is not an object.");
    case Kind::capabilities_shape_unknown:
        return QStringLiteral("The Agent capabilities structure is unsupported.");
    case Kind::legacy_bash_alias:
        return QStringLiteral("The Agent uses the legacy bash capability alias.");
    case Kind::capability_conflict:
        return QStringLiteral("The Agent has conflicting bash and shell capabilities.");
    }
    return QStringLiteral("An unknown compatibility finding was reported.");
}

ProjectOpenDisposition open_disposition(CompatibilityDisposition disposition) {
    switch (disposition) {
    case CompatibilityDisposition::compatible:
        return ProjectOpenDisposition::compatible;
    case CompatibilityDisposition::degraded:
        return ProjectOpenDisposition::degraded;
    case CompatibilityDisposition::blocked:
        return ProjectOpenDisposition::blocked;
    }
    return ProjectOpenDisposition::blocked;
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

QString manifest_diagnostic_text(AgentManifestDiagnosticKind diagnostic) {
    using Kind = AgentManifestDiagnosticKind;
    switch (diagnostic) {
    case Kind::none: return QStringLiteral("read this scan");
    case Kind::unsafe_symlink: return QStringLiteral("unsafe symlink");
    case Kind::not_regular: return QStringLiteral("not a regular file");
    case Kind::unreadable: return QStringLiteral("unreadable");
    case Kind::io_error: return QStringLiteral("I/O error");
    case Kind::invalid_json: return QStringLiteral("invalid JSON");
    case Kind::not_object: return QStringLiteral("JSON root is not an object");
    case Kind::too_large: return QStringLiteral("source exceeds size limit");
    }
    return QStringLiteral("unavailable");
}

QString heartbeat_diagnostic_text(const AgentRosterPresenceItem *presence) {
    if (!presence) return QStringLiteral("projection unavailable");
    using Kind = AgentHeartbeatDiagnosticKind;
    switch (presence->heartbeat_source.diagnostic) {
    case Kind::none:
        return presence->heartbeat_source.observation
                == AgentHeartbeatObservationState::not_applicable
            ? QStringLiteral("not applicable")
            : presence->heartbeat_source.observation
                    == AgentHeartbeatObservationState::absent
                ? QStringLiteral("absent")
                : QStringLiteral("read this scan");
    case Kind::invalid_number: return QStringLiteral("invalid decimal");
    case Kind::nonfinite: return QStringLiteral("non-finite number");
    case Kind::future: return QStringLiteral("future timestamp");
    case Kind::unsafe_symlink: return QStringLiteral("unsafe symlink");
    case Kind::not_regular: return QStringLiteral("not a regular file");
    case Kind::unreadable: return QStringLiteral("unreadable");
    case Kind::io_error: return QStringLiteral("I/O error");
    case Kind::too_large: return QStringLiteral("source exceeds size limit");
    case Kind::container_unavailable: return QStringLiteral("Agent unavailable");
    case Kind::container_changed: return QStringLiteral("Agent changed during scan");
    case Kind::source_changed: return QStringLiteral("source changed during scan");
    }
    return QStringLiteral("unavailable");
}

QString scan_text(AgentManifestScanState state) {
    using State = AgentManifestScanState;
    switch (state) {
    case State::complete: return QStringLiteral("complete");
    case State::root_missing: return QStringLiteral("root missing");
    case State::root_not_directory: return QStringLiteral("root is not a directory");
    case State::root_unsafe_symlink: return QStringLiteral("root is an unsafe symlink");
    case State::root_unreadable: return QStringLiteral("root is unreadable");
    case State::root_io_error: return QStringLiteral("root I/O error");
    }
    return QStringLiteral("root I/O error");
}

QString scan_diagnostic_text(AgentManifestScanDiagnosticKind kind) {
    using Kind = AgentManifestScanDiagnosticKind;
    switch (kind) {
    case Kind::child_unsafe_symlink: return QStringLiteral("child_unsafe_symlink");
    case Kind::child_unreadable: return QStringLiteral("child_unreadable");
    case Kind::child_io_error: return QStringLiteral("child_io_error");
    }
    return QStringLiteral("child_io_error");
}

const AgentManifestDiscoveryItem *selectable_item(
        const AgentRosterSnapshot &roster, const fs::path &key) {
    const auto found = std::ranges::find_if(roster.discovery.items,
        [&](const auto &item) {
            return item.directory_key == key
                && item.manifest_kind == AgentManifestKind::valid;
        });
    return found == roster.discovery.items.end() ? nullptr : &*found;
}

} // namespace

NativeShell::NativeShell()
: selection_state_(8)
, window_(std::make_unique<Ui::RpWindow>()) {
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
        QStringLiteral("Open Project\u2026"), empty_route_);
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
    project_route_->setObjectName("lingtai_project_compatibility_route");
    project_route_->setAccessibleName(QStringLiteral("Project compatibility"));
    project_route_->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Expanding);
    project_route_->hide();
    content_layout->addWidget(project_route_, 1);

    auto *project_layout = new QVBoxLayout(project_route_);
    project_layout->setContentsMargins(0, 0, 0, 0);
    project_layout->setSpacing(12);
    project_layout->addWidget(make_label(
        project_route_,
        QStringLiteral("Project compatibility"),
        "lingtai_project_route_heading",
        18,
        QFont::DemiBold));
    project_layout->addWidget(make_label(
        project_route_, QString(), "lingtai_project_root", 11));
    project_layout->addWidget(make_label(
        project_route_, QString(), "lingtai_compatibility_title", 16,
        QFont::DemiBold));
    project_layout->addWidget(make_label(
        project_route_, QString(), "lingtai_commands_availability", 12,
        QFont::Medium));
    project_layout->addWidget(make_label(
        project_route_, QString(), "lingtai_compatibility_findings", 12));
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

    auto *detail = new Ui::RpWidget(directory);
    detail->setObjectName("lingtai_agent_detail");
    detail->setAccessibleName(QStringLiteral("Selected Agent detail"));
    directory_layout->addWidget(detail, 2);
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
    auto *detail_facts = make_label(
        detail, QString(), "lingtai_selected_agent_facts", 11);
    detail_facts->setAccessibleName(QStringLiteral("Selected Agent facts"));
    detail_layout->addWidget(detail_facts);
    auto *detail_diagnostic = make_label(
        detail, QString(), "lingtai_selected_agent_diagnostic", 10);
    detail_diagnostic->setAccessibleName(
        QStringLiteral("Selected Agent source diagnostics"));
    detail_layout->addWidget(detail_diagnostic);
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
        const fs::path &install_receipt_path,
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

    auto roster = project_agent_roster(*attached.attachment);
    const auto canonical_root = attached.attachment->root();
    const auto same_root = selection_state_.active_project()
        && selection_state_.active_project()->root() == canonical_root;
    auto selected_key = std::optional<fs::path>();
    if (agent_relative_directory) {
        const auto &relative = *agent_relative_directory;
        if (relative.parent_path() == fs::path(".lingtai")
            && selectable_item(roster, relative.filename())) {
            selected_key = relative.filename();
        }
    } else if (same_root
            && selection_state_.selected_agent_directory_key()
            && selectable_item(roster,
                *selection_state_.selected_agent_directory_key())) {
        selected_key = selection_state_.selected_agent_directory_key();
    }

    selection_state_.activate_project(std::move(*attached.attachment));
    selection_state_.clear_agent_selection();
    if (selected_key) {
        static_cast<void>(selection_state_.select_agent(*selected_key));
    }
    roster_ = std::move(roster);
    install_receipt_path_ = install_receipt_path;
    const auto selected_relative = selected_key
        ? std::optional<fs::path>(fs::path(".lingtai") / *selected_key)
        : std::nullopt;
    const auto report = probe_compatibility(
        *selection_state_.active_project(), selected_relative,
        *install_receipt_path_);
    window_->findChild<QLabel *>("lingtai_project_root")
        ->setText(path_text(canonical_root));
    render_compatibility(report);
    render_roster();
    auto *selection_error = window_->findChild<QLabel *>(
        "lingtai_agent_selection_error");
    selection_error->clear();
    selection_error->hide();
    window_->findChild<QLabel *>("lingtai_project_open_error")->clear();
    open_error_surface_->hide();
    refresh_route();
    return {
        .disposition = open_disposition(report.disposition),
        .commands_allowed = report.commands_allowed,
        .failure = ProjectPathFailure::none,
    };
}

AgentSelectionOutcome NativeShell::select_agent(
        const fs::path &directory_key) {
    return handle_agent_selection(directory_key);
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

std::size_t NativeShell::open_project_request_count() const noexcept {
    return open_project_request_count_;
}

NativeShellSnapshot NativeShell::snapshot() const {
    static_assert(std::is_same_v<decltype(*window_), Ui::RpWindow &>);
    const auto *body = window_->body().get();
    const auto *sidebar = window_->findChild<Ui::RpWidget *>(
        "lingtai_desktop_sidebar");
    const auto *content = window_->findChild<Ui::RpWidget *>(
        "lingtai_desktop_content");
    return {
        .window_class = "Ui::RpWindow",
        .window_object = window_->objectName().toStdString(),
        .body_object = body->objectName().toStdString(),
        .sidebar_object = sidebar ? sidebar->objectName().toStdString() : "",
        .content_object = content ? content->objectName().toStdString() : "",
        .shown = window_->isVisible(),
        .empty_route_visible = empty_route_ && empty_route_->isVisible(),
        .positioned_offscreen = window_->testAttribute(
            Qt::WA_DontShowOnScreen),
    };
}

void NativeShell::request_open_project() {
    ++open_project_request_count_;
    if (open_project_request_handler_) {
        open_project_request_handler_();
    }
}

void NativeShell::render_compatibility(const CompatibilityReport &report) {
    auto findings = QStringList();
    for (const auto &finding : report.findings) {
        findings.push_back(finding_text(finding.kind));
    }
    const auto title = report.disposition == CompatibilityDisposition::compatible
        ? QStringLiteral("Compatible")
        : report.disposition == CompatibilityDisposition::degraded
            ? QStringLiteral("Degraded — read-only")
            : QStringLiteral("Blocked");
    window_->findChild<QLabel *>("lingtai_compatibility_title")
        ->setText(title);
    window_->findChild<QLabel *>("lingtai_commands_availability")->setText(
        report.commands_allowed
            ? QStringLiteral("Commands available: Yes")
            : QStringLiteral("Commands available: No"));
    window_->findChild<QLabel *>("lingtai_compatibility_findings")->setText(
        findings.empty() ? QStringLiteral("No compatibility findings.")
                         : findings.join(QLatin1Char('\n')));
}

void NativeShell::render_roster() {
    auto *state = window_->findChild<QLabel *>("lingtai_agent_roster_state");
    auto *selected_key = window_->findChild<QLabel *>(
        "lingtai_selected_agent_key");
    auto *selected_facts = window_->findChild<QLabel *>(
        "lingtai_selected_agent_facts");
    auto *selected_diagnostic = window_->findChild<QLabel *>(
        "lingtai_selected_agent_diagnostic");
    if (!state || !selected_key || !selected_facts || !selected_diagnostic
        || !roster_rows_ || !roster_rows_->layout()) {
        return;
    }

    auto *layout = roster_rows_->layout();
    while (auto *child = layout->takeAt(0)) {
        delete child->widget();
        delete child;
    }

    const auto scan = scan_text(roster_.discovery.scan.state);
    auto status = QString();
    if (!roster_.projection_complete) {
        status = QStringLiteral("Roster incomplete — scan: %1").arg(scan);
    } else if (roster_.discovery.scan.state != AgentManifestScanState::complete) {
        status = QStringLiteral("Roster unavailable — scan: %1").arg(scan);
    } else if (roster_.discovery.items.empty()) {
        status = QStringLiteral("No Agents found — scan complete");
    } else {
        status = QStringLiteral("%1 Agent(s) — projection complete")
            .arg(roster_.discovery.items.size());
    }
    for (const auto &diagnostic : roster_.discovery.scan_diagnostics) {
        status += QStringLiteral(
            "\nchild scan diagnostic: %1; key: %2; path: %3")
            .arg(scan_diagnostic_text(diagnostic.kind),
                path_text(diagnostic.path.filename()),
                path_text(diagnostic.path));
    }
    state->setText(status);

    const auto selected = selection_state_.selected_agent_directory_key();
    const auto parallel = roster_.projection_complete
        && roster_.presence_by_item.size() == roster_.discovery.items.size();
    const AgentManifestDiscoveryItem *detail_item = nullptr;
    const AgentRosterPresenceItem *detail_presence = nullptr;
    for (auto index = std::size_t{0};
         index != roster_.discovery.items.size(); ++index) {
        const auto &item = roster_.discovery.items[index];
        const auto *presence = parallel ? &roster_.presence_by_item[index] : nullptr;
        const auto facts = QStringLiteral("%1 — %2 — %3")
            .arg(manifest_text(item.manifest_kind), role_text(item.role),
                presence_text(presence ? presence->presence
                                       : AgentPresenceKind::unknown));
        auto button_key = path_text(item.directory_key);
        button_key.replace(QLatin1Char('&'), QStringLiteral("&&"));
        auto *row = new QPushButton(
            QStringLiteral("%1\n%2\nmanifest: %3; heartbeat: %4")
                .arg(button_key, facts,
                    manifest_diagnostic_text(item.manifest_source.diagnostic),
                    heartbeat_diagnostic_text(presence)),
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
            static_cast<void>(handle_agent_selection(key));
        });
        layout->addWidget(row);
        if (row->isChecked()) {
            detail_item = &item;
            detail_presence = presence;
        }
    }
    if (auto *box = qobject_cast<QBoxLayout *>(layout)) box->addStretch();

    if (!detail_item) {
        selected_key->setText(QStringLiteral("No Agent selected"));
        selected_facts->setText(QStringLiteral(
            "Choose a valid manifest row to inspect compatibility."));
        selected_diagnostic->clear();
        return;
    }
    selected_key->setText(path_text(detail_item->directory_key));
    selected_facts->setText(QStringLiteral("manifest: %1\nrole: %2\npresence: %3")
        .arg(manifest_text(detail_item->manifest_kind),
            role_text(detail_item->role),
            presence_text(detail_presence ? detail_presence->presence
                                          : AgentPresenceKind::unknown)));
    selected_diagnostic->setText(QStringLiteral(
        "manifest source: %1\nheartbeat source: %2")
        .arg(manifest_diagnostic_text(
                detail_item->manifest_source.diagnostic),
            heartbeat_diagnostic_text(detail_presence)));
}

AgentSelectionOutcome NativeShell::handle_agent_selection(
        const fs::path &directory_key) {
    auto *error = window_->findChild<QLabel *>("lingtai_agent_selection_error");
    const auto *item = selectable_item(roster_, directory_key);
    if (!item) {
        if (error) {
            error->setText(QStringLiteral(
                "This roster item cannot be selected for compatibility."));
            error->show();
        }
        return {
            .disposition = AgentSelectionDisposition::not_selectable,
            .state_result = AgentSelectionResult::invalid_directory_key,
            .commands_allowed = false,
        };
    }
    const auto result = selection_state_.select_agent(item->directory_key);
    if (result != AgentSelectionResult::selected
        || !selection_state_.active_project() || !install_receipt_path_) {
        if (error) {
            error->setText(QStringLiteral("Agent selection was rejected."));
            error->show();
        }
        return {
            .disposition = AgentSelectionDisposition::rejected,
            .state_result = result,
            .commands_allowed = false,
        };
    }
    const auto report = probe_compatibility(
        *selection_state_.active_project(),
        fs::path(".lingtai") / item->directory_key,
        *install_receipt_path_);
    render_compatibility(report);
    render_roster();
    if (error) {
        error->clear();
        error->hide();
    }
    return {
        .disposition = AgentSelectionDisposition::selected,
        .state_result = result,
        .commands_allowed = report.commands_allowed,
    };
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
        .commands_allowed = false,
        .failure = failure,
    };
}

void NativeShell::refresh_route() {
    if (empty_route_) {
        empty_route_->setVisible(
            !selection_state_.active_project().has_value());
    }
    if (project_route_) {
        project_route_->setVisible(
            selection_state_.active_project().has_value());
    }
}

} // namespace lingtai::desktop
