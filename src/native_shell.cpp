#include "native_shell.h"

#include "compatibility_probe.h"

#include "ui/rp_widget.h"
#include "ui/widgets/rp_window.h"

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtGui/QFont>
#include <QtWidgets/QBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>

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
    project_layout->addStretch();

    refresh_route();
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

    const auto report = probe_compatibility(
        *attached.attachment, agent_relative_directory, install_receipt_path);
    auto findings = QStringList();
    for (const auto &finding : report.findings) {
        findings.push_back(finding_text(finding.kind));
    }
    const auto title = report.disposition == CompatibilityDisposition::compatible
        ? QStringLiteral("Compatible")
        : report.disposition == CompatibilityDisposition::degraded
            ? QStringLiteral("Degraded — read-only")
            : QStringLiteral("Blocked");
    const auto commands = report.commands_allowed
        ? QStringLiteral("Commands available: Yes")
        : QStringLiteral("Commands available: No");
    const auto detail = findings.empty()
        ? QStringLiteral("No compatibility findings.")
        : findings.join(QLatin1Char('\n'));

    const auto canonical_root = attached.attachment->root();
    selection_state_.activate_project(std::move(*attached.attachment));
    window_->findChild<QLabel *>("lingtai_project_root")
        ->setText(path_text(canonical_root));
    window_->findChild<QLabel *>("lingtai_compatibility_title")->setText(title);
    window_->findChild<QLabel *>("lingtai_commands_availability")
        ->setText(commands);
    window_->findChild<QLabel *>("lingtai_compatibility_findings")
        ->setText(detail);
    window_->findChild<QLabel *>("lingtai_project_open_error")->clear();
    open_error_surface_->hide();
    refresh_route();
    return {
        .disposition = open_disposition(report.disposition),
        .commands_allowed = report.commands_allowed,
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
