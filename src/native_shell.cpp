#include "native_shell.h"

#include "ui/rp_widget.h"
#include "ui/widgets/rp_window.h"

#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QPalette>
#include <QtWidgets/QBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>

#include <utility>

namespace lingtai::desktop {
namespace {

constexpr auto kSidebarWidth = 264;
constexpr auto kMinimumWindowWidth = 720;
constexpr auto kMinimumWindowHeight = 480;
constexpr auto kDefaultWindowWidth = 1100;
constexpr auto kDefaultWindowHeight = 720;

void set_background(QWidget &widget, const QColor &color) {
    auto palette = widget.palette();
    palette.setColor(QPalette::Window, color);
    widget.setAutoFillBackground(true);
    widget.setPalette(palette);
}

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
    set_background(*body, QColor(QStringLiteral("#F7F8FA")));

    auto *shell_layout = new QHBoxLayout(body);
    shell_layout->setContentsMargins(0, 0, 0, 0);
    shell_layout->setSpacing(0);

    auto *sidebar = new Ui::RpWidget(body);
    sidebar->setObjectName("lingtai_desktop_sidebar");
    sidebar->setAccessibleName(QStringLiteral("Workspace navigation"));
    sidebar->setFixedWidth(kSidebarWidth);
    set_background(*sidebar, QColor(QStringLiteral("#E9EDF3")));
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
    set_background(*content, QColor(QStringLiteral("#FFFFFF")));
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

    auto *empty_route = new Ui::RpWidget(content);
    empty_route->setObjectName("lingtai_empty_workspace_route");
    empty_route->setAccessibleName(QStringLiteral("No project open"));
    empty_route->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    content_layout->addWidget(empty_route, 1);

    auto *empty_layout = new QVBoxLayout(empty_route);
    empty_layout->setContentsMargins(0, 0, 0, 0);
    empty_layout->setSpacing(12);
    auto *empty_title = make_label(
        empty_route,
        QStringLiteral("No project open"),
        "lingtai_no_project_title",
        18,
        QFont::DemiBold);
    auto *empty_detail = make_label(
        empty_route,
        QStringLiteral("Open a LingTai project to inspect its Agents."),
        "lingtai_no_project_detail",
        12);
    auto *open_button = new QPushButton(
        QStringLiteral("Open Project\u2026"), empty_route);
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
    const auto *body = window_->body().get();
    const auto *sidebar = window_->findChild<Ui::RpWidget *>(
        "lingtai_desktop_sidebar");
    const auto *content = window_->findChild<Ui::RpWidget *>(
        "lingtai_desktop_content");
    const auto *empty_route = window_->findChild<Ui::RpWidget *>(
        "lingtai_empty_workspace_route");
    return {
        .window_class = "Ui::RpWindow",
        .window_object = window_->objectName().toStdString(),
        .body_object = body->objectName().toStdString(),
        .sidebar_object = sidebar ? sidebar->objectName().toStdString() : "",
        .content_object = content ? content->objectName().toStdString() : "",
        .shown = window_->isVisible(),
        .empty_route_visible = empty_route && empty_route->isVisible(),
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

void NativeShell::refresh_route() {
    auto *empty_route = window_->findChild<Ui::RpWidget *>(
        "lingtai_empty_workspace_route");
    empty_route->setVisible(!selection_state_.active_project().has_value());
}

} // namespace lingtai::desktop
