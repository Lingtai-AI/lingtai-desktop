#include "native_shell.h"

#include "agent_activity.h"
#include "agent_preset_summary.h"
#include "agent_sleep.h"
#include "agent_task_card.h"
#include "direct_conversation_history.h"
#include "direct_mail_publisher.h"

#include "base/integration.h"

#include "styles/palette.h"
#include "styles/style_widgets.h"
#include "ui/conversation_surface.h"
#include "ui/effects/animations.h"
#include "ui/integration.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/rp_window.h"
#include "ui/widgets/shadow.h"

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtCore/QDir>
#include <QtGui/QFont>
#include <QtWidgets/QBoxLayout>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSizePolicy>

#include <rpl/range.h>

#include <algorithm>
#include <utility>

namespace lingtai::desktop {
namespace {

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

// One shared structural owner for the three read-only selected-Agent source
// sections (Agent activity, Task Card, Presets). Each section directly owns
// its own semibold heading, read-only plain-text surface, state line, and one
// thin plain-shadow separator, with the same inner margins and spacing, so the
// three distinct sources share one consistent local framing instead of three
// hand-built heading/surface/state sequences.
struct DashboardSection {
    Ui::RpWidget *owner = nullptr;
    QLabel *heading = nullptr;
    QPlainTextEdit *surface = nullptr;
    QLabel *state = nullptr;
};

constexpr auto kDashboardSectionSurfaceHeight = 140;

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
    auto *layout = new QVBoxLayout(owner);
    layout->setContentsMargins(0, 8, 0, 8);
    layout->setSpacing(6);
    auto *heading = make_label(
        owner, heading_text,
        (base + QStringLiteral("_heading")).toUtf8().constData(), 12,
        QFont::DemiBold);
    layout->addWidget(heading);
    auto *surface = new QPlainTextEdit(owner);
    surface->setObjectName(base);
    surface->setAccessibleName(surface_accessible_name);
    surface->setAccessibleDescription(surface_accessible_description);
    surface->setReadOnly(true);
    surface->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    surface->setMinimumHeight(kDashboardSectionSurfaceHeight);
    layout->addWidget(surface);
    auto *state = make_label(
        owner, QString(), (base + QStringLiteral("_state")).toUtf8().constData(),
        10);
    state->setAccessibleName(surface_accessible_name + QStringLiteral(" state"));
    layout->addWidget(state);
    auto *separator = new Ui::PlainShadow(owner);
    separator->setObjectName((base + QStringLiteral("_separator")).toUtf8().constData());
    separator->setAccessibleName(heading_text + QStringLiteral(" divider"));
    separator->setFixedHeight(st::lineWidth);
    layout->addWidget(separator);
    detail_layout->addWidget(owner);
    return {owner, heading, surface, state};
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

// Desktop's own product gate, not kernel-core enforcement: a valid manifest,
// a main/agent role (never human/unknown), the canonical strict `< 5.0 s`
// heartbeat predicate, and a known current manifest state -- from
// `.agent.json.state`, not the best-effort `.status.json` -- other than
// `asleep`/`suspended`. No `.status.json.running`, PID, active turn,
// `admin.karma`, or compatibility probe is ever consulted.
bool agent_sleep_eligible(const AgentRow &item) {
    if (item.manifest_kind != AgentManifestKind::valid) return false;
    if (item.role != AgentRole::main && item.role != AgentRole::agent) {
        return false;
    }
    if (item.presence != AgentPresenceKind::alive) return false;
    if (!item.identity || !item.identity->state) return false;
    return *item.identity->state != "asleep"
        && *item.identity->state != "suspended";
}

// Desktop's own product gate for showing Start Agent at all: a valid
// manifest, a main/agent role (never human/unknown), and exactly a stale or
// missing heartbeat -- the two presence kinds a genuine new heartbeat write
// can honestly transition out of. `invalid` (for example a future-dated
// heartbeat) and `unavailable` are deliberately excluded: either could
// later read as `alive` from wall-clock movement alone or a transient read
// failure, with no real new write, which would let the post-click success
// check in `tick_agent_start_observation()` claim a false "Agent is
// online." without ever consulting a heartbeat timestamp baseline.
bool agent_start_eligible(const AgentRow &item) {
    if (item.manifest_kind != AgentManifestKind::valid) return false;
    if (item.role != AgentRole::main && item.role != AgentRole::agent) {
        return false;
    }
    return item.presence == AgentPresenceKind::stale
        || item.presence == AgentPresenceKind::missing;
}

// Desktop never installs the base::Integration the vendored InputField's Qt
// signal producer needs: it delivers QTextDocument::contentsChange through
// base::Integration::Instance(), and with no instance installed the first
// composer clear() trips the base assertion and crashes. One process-lifetime
// minimum adapter suffices, mirroring how Core::BaseIntegration wraps the
// base contract: direct event-loop delivery plus no-op logging.
class DesktopBaseIntegration final : public base::Integration {
public:
    DesktopBaseIntegration()
    : base::Integration(0, nullptr) {
        base::Integration::Set(this);
    }

    void enterFromEventLoop(FnMut<void()> &&method) override {
        std::move(method)();
    }

    bool logSkipDebug() override {
        return true;
    }

    void logMessageDebug(const QString &message) override {
    }

    void logMessage(const QString &message) override {
    }

};

// lib_ui's own Integration is equally required: the vendored InputField calls
// Ui::Integration::Instance() from its contents-changed handler, and without
// an installed instance that assertion fires on the first composer clear().
// The base class implements every non-pure virtual with a safe default, so
// this adapter only supplies the eight pure slots with no-op semantics.
class DesktopUiIntegration final : public Ui::Integration {
public:
    DesktopUiIntegration() {
        Ui::Integration::Set(this);
    }

    void postponeCall(FnMut<void()> &&callable) override {
        std::move(callable)();
    }

    void registerLeaveSubscription(not_null<QWidget *> widget) override {
    }

    void unregisterLeaveSubscription(not_null<QWidget *> widget) override {
    }

    [[nodiscard]] QString emojiCacheFolder() override {
        return QString();
    }

    [[nodiscard]] QString openglCheckFilePath() override {
        return QString();
    }

    [[nodiscard]] QString angleBackendFilePath() override {
        return QString();
    }

    void touchCounterIncrement() override {
    }

    [[nodiscard]] int touchCounterNow() override {
        return 0;
    }

};

std::unique_ptr<Ui::RpWindow> make_native_window() {
    // Install the adapters before any vendored widget is constructed, unless
    // a hosting environment already installed them.
    static const auto integration_installed = [] {
        if (!base::Integration::Exists()) {
            static DesktopBaseIntegration base_integration;
        }
        if (!Ui::Integration::Exists()) {
            static DesktopUiIntegration ui_integration;
        }
        // The vendored InputField's placeholder animation asserts unless the
        // process-global animations manager exists, so own exactly one.
        static Ui::Animations::Manager animations_manager;
        return true;
    }();
    (void)integration_installed;

    // The roster paints generated palette tokens; that palette must be ready
    // before any window is built. The vendored widget-style module is started
    // only after the window exists: its generated custom-title style would
    // otherwise give the mac window helper a nonzero title height and change
    // the explicit window minimum, while the composer controls (which need
    // those styles) are constructed only after this function returns.
    static const auto palette_started = [] {
        style::internal::init_palette(style::kScaleDefault);
        return true;
    }();
    (void)palette_started;
    auto result = std::make_unique<Ui::RpWindow>();
    static const auto widget_styles_started = [] {
        style::internal::init_style_widgets(style::kScaleDefault);
        return true;
    }();
    (void)widget_styles_started;
    // The vendored mac title widget keeps a pointer to the style it is given,
    // so this zero-height copy must be process-lifetime: it keeps the custom
    // title hidden (frameMargins().top() == 0) and the explicit window minimum
    // exactly (720, 480) while the widget styles stay initialized for the
    // vendored composer controls.
    static const auto desktop_title_style = [] {
        auto style_copy = st::defaultWindowTitle;
        style_copy.height = 0;
        return style_copy;
    }();
    result->setTitleStyle(desktop_title_style);
    return result;
}

} // namespace

NativeShell::NativeShell()
: window_(make_native_window()) {
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

    // The persistent left 260px project/Agent list column: project identity
    // header, compact Open/New Project actions, and the scrollable Agent
    // rows. The shell wires the owner's row clicks and its action buttons.
    agent_roster_ = new AgentRoster(body);
    agent_roster_->set_row_click_handler([this](const fs::path &key) {
        handle_agent_selection(key);
    });
    if (auto *open_button = agent_roster_->findChild<QPushButton *>(
            "lingtai_open_project_button")) {
        QObject::connect(open_button, &QPushButton::clicked, [this] {
            request_open_project();
        });
    }
    if (auto *new_button = agent_roster_->findChild<QPushButton *>(
            "lingtai_new_project_button")) {
        QObject::connect(new_button, &QPushButton::clicked, [this] {
            request_new_project();
        });
    }
    shell_layout->addWidget(agent_roster_);

    // One thin lib_ui shadow separates the persistent list column from the
    // selected-content pane, matching the pinned shell's between-column
    // `_sideShadow` geometry.
    auto *separator = new Ui::PlainShadow(body);
    separator->setObjectName("lingtai_roster_separator");
    separator->setAccessibleName(QStringLiteral("Project list divider"));
    shell_layout->addWidget(separator);

    auto *content = new Ui::RpWidget(body);
    content->setObjectName("lingtai_desktop_content");
    content->setAccessibleName(QStringLiteral("Workspace content"));
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    shell_layout->addWidget(content, 1);

    auto *content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(24, 24, 24, 24);
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

    // The one truthful New Project status surface, above both routes so a
    // pending phase, a failure, or a created-and-started success stays
    // visible regardless of which route is showing.
    bootstrap_status_surface_ = new Ui::RpWidget(content);
    bootstrap_status_surface_->setObjectName(
        "lingtai_bootstrap_status_surface");
    auto *bootstrap_status_layout = new QVBoxLayout(bootstrap_status_surface_);
    bootstrap_status_layout->setContentsMargins(0, 0, 0, 0);
    auto *bootstrap_status = make_label(
        bootstrap_status_surface_,
        QString(),
        "lingtai_bootstrap_status",
        12,
        QFont::Medium);
    bootstrap_status->setAccessibleName(QStringLiteral("New project status"));
    bootstrap_status_layout->addWidget(bootstrap_status);
    bootstrap_status_surface_->hide();
    content_layout->addWidget(bootstrap_status_surface_);

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
    empty_layout->addStretch();
    empty_layout->addWidget(empty_title);
    empty_layout->addWidget(empty_detail);
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

    // The detail column carries more evidence than any window is tall, so it
    // scrolls like the roster instead of overflowing and overpainting itself.
    auto *detail_scroll = new QScrollArea(directory);
    detail_scroll->setObjectName("lingtai_agent_detail_scroll");
    detail_scroll->setAccessibleName(QStringLiteral("Selected Agent detail"));
    detail_scroll->setWidgetResizable(true);
    directory_layout->addWidget(detail_scroll, 1);
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
    auto *presentation_name = make_label(
        detail, QString(), "lingtai_selected_agent_presentation_name", 12,
        QFont::Medium);
    presentation_name->setAccessibleName(
        QStringLiteral("Selected Agent presentation name"));
    detail_layout->addWidget(presentation_name);
    auto *detail_key = make_label(
        detail, QString(), "lingtai_selected_agent_key", 12, QFont::Medium);
    detail_key->setAccessibleName(QStringLiteral("Selected Agent key"));
    detail_layout->addWidget(detail_key);
    // The conversation is the product, so it sits directly under the selected
    // Agent's name rather than below the source-facts labels.
    detail_layout->addWidget(make_label(
        detail, QStringLiteral("Conversation"),
        "lingtai_selected_agent_conversation_heading", 12, QFont::DemiBold));
    auto *conversation = new ConversationSurface(detail);
    conversation->setObjectName("lingtai_selected_agent_conversation");
    conversation->setAccessibleName(
        QStringLiteral("Selected Agent conversation"));
    conversation->setAccessibleDescription(QStringLiteral(
        "The current direct conversation with the selected Agent, shown "
        "read-only as plain text."));
    conversation->setMinimumHeight(180);
    conversation->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    detail_layout->addWidget(conversation, 1);

    // The smallest visible composer, directly under the conversation it
    // sends into: one vendored single-line input and one explicit Send
    // action, both submitting through the same send path.
    auto *composer = new Ui::RpWidget(detail);
    composer->setObjectName("lingtai_composer");
    composer->setAccessibleName(QStringLiteral("Send a message"));
    auto *composer_layout = new QHBoxLayout(composer);
    composer_layout->setContentsMargins(0, 0, 0, 0);
    composer_layout->setSpacing(8);
    auto *composer_input = new Ui::InputField(
        composer,
        st::defaultInputField,
        Ui::InputField::Mode::SingleLine,
        rpl::single(QStringLiteral("Message…")));
    composer_input->setObjectName("lingtai_composer_input");
    composer_input->setAccessibleName(QStringLiteral("Message"));
    composer_input->setMinHeight(36);
    composer_input->setEnabled(false);
    composer_layout->addWidget(composer_input, 1);
    auto *send_button = new Ui::RoundButton(
        composer,
        rpl::single(QStringLiteral("Send")),
        st::defaultActiveButton);
    send_button->setObjectName("lingtai_composer_send_button");
    send_button->setAccessibleName(QStringLiteral("Send message"));
    send_button->setEnabled(false);
    send_button->addClickHandler([this] {
        handle_send_message();
    });
    composer_layout->addWidget(send_button);
    detail_layout->addWidget(composer);
    auto *composer_status = make_label(
        detail, QString(), "lingtai_composer_status", 10);
    composer_status->setAccessibleName(QStringLiteral("Send status"));
    detail_layout->addWidget(composer_status);
    composer_input->submits()
        | rpl::on_next([this] {
            handle_send_message();
        }, submits_lifetime_);

    auto *conversation_state = make_label(
        detail, QString(), "lingtai_selected_agent_conversation_state", 10);
    conversation_state->setAccessibleName(
        QStringLiteral("Selected Agent conversation state"));
    detail_layout->addWidget(conversation_state);

    // Each of the three bounded read-only selected-Agent source sections
    // below is presented through the same local structural framing: one
    // semibold heading, one read-only plain-text surface, one state line,
    // and one thin plain-shadow separator, with the same inner margins and
    // spacing. They remain three distinct sources and authorities and are
    // never merged with each other or with the mailbox conversation.
    add_dashboard_section(
        detail, detail_layout, "activity", QStringLiteral("Agent activity"),
        QStringLiteral("Selected Agent activity"),
        QStringLiteral("A bounded read-only snapshot of the selected Agent's "
            "own visible activity, shown as plain text."));
    add_dashboard_section(
        detail, detail_layout, "task_card", QStringLiteral("Task Card"),
        QStringLiteral("Selected Agent Task Card"),
        QStringLiteral("The selected Agent's current self-published Task Card "
            "body, when active, shown read-only as plain text."));
    add_dashboard_section(
        detail, detail_layout, "preset_summary", QStringLiteral("Presets"),
        QStringLiteral("Selected Agent Presets summary"),
        QStringLiteral("The selected Agent's own kernel-resolved preset policy "
            "and active effective configuration, shown read-only as plain "
            "text."));

    // The one Step-6 action on the exact selected Agent: an explicit,
    // nonblocking start for a selected non-human Agent whose current
    // projection is not heartbeat-live. Hidden entirely (not merely
    // disabled) for a live Agent, matching the product contract's "no
    // Start action" rather than Request sleep's always-visible/disabled
    // shape. The status label below shows only truthful, evidence-backed
    // claims -- spawn acceptance is never "online" on its own.
    auto *start_row = new Ui::RpWidget(detail);
    start_row->setObjectName("lingtai_selected_agent_start_row");
    start_row->setAccessibleName(QStringLiteral("Start Agent"));
    auto *start_row_layout = new QHBoxLayout(start_row);
    start_row_layout->setContentsMargins(0, 0, 0, 0);
    start_row_layout->setSpacing(8);
    auto *start_button = new QPushButton(
        QStringLiteral("Start Agent"), start_row);
    start_button->setObjectName("lingtai_selected_agent_start_agent");
    start_button->setAccessibleName(QStringLiteral("Start Agent"));
    start_button->setAccessibleDescription(QStringLiteral(
        "Starts the selected Agent's own configured runtime as a detached "
        "local process. It does not provision, install, or repair a "
        "runtime, and never auto-starts any Agent on its own."));
    QObject::connect(start_button, &QPushButton::clicked, [this] {
        handle_start_agent();
    });
    start_row_layout->addWidget(start_button);
    // Reserve the action region's height from the row's own layout so the
    // detail column below never jumps when the Start button is hidden for a
    // heartbeat-live Agent; visibility/enablement still track eligibility
    // exactly, only the button is ever absent.
    start_row->setMinimumHeight(start_row->sizeHint().height());
    start_button->setVisible(false);
    detail_layout->addWidget(start_row);
    auto *start_status = make_label(
        detail, QString(), "lingtai_selected_agent_start_status", 10);
    start_status->setAccessibleName(QStringLiteral("Start Agent status"));
    detail_layout->addWidget(start_status);

    // The one Step-5 action on the exact selected Agent: reproduces only the
    // canonical empty `.sleep` marker write plus a best-effort target-side
    // observation. Disabled while ineligible or while a just-clicked
    // observation is still pending; the status label below shows only
    // truthful, evidence-backed claims, never a lifecycle status inferred
    // from the write or a timeout alone.
    auto *sleep_row = new Ui::RpWidget(detail);
    sleep_row->setObjectName("lingtai_selected_agent_sleep_row");
    sleep_row->setAccessibleName(QStringLiteral("Request sleep"));
    auto *sleep_row_layout = new QHBoxLayout(sleep_row);
    sleep_row_layout->setContentsMargins(0, 0, 0, 0);
    sleep_row_layout->setSpacing(8);
    auto *sleep_button = new QPushButton(
        QStringLiteral("Request sleep"), sleep_row);
    sleep_button->setObjectName("lingtai_selected_agent_request_sleep");
    sleep_button->setAccessibleName(QStringLiteral("Request sleep"));
    sleep_button->setAccessibleDescription(QStringLiteral(
        "Writes one empty local sleep-request marker for the selected "
        "Agent. It does not queue, cancel, suspend, or restart anything."));
    sleep_button->setEnabled(false);
    QObject::connect(sleep_button, &QPushButton::clicked, [this] {
        handle_request_sleep();
    });
    sleep_row_layout->addWidget(sleep_button);
    detail_layout->addWidget(sleep_row);
    auto *sleep_status = make_label(
        detail, QString(), "lingtai_selected_agent_sleep_status", 10);
    sleep_status->setAccessibleName(QStringLiteral("Sleep request status"));
    detail_layout->addWidget(sleep_status);

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

    // One simple view-scoped timer: it re-invokes the same stateless
    // snapshot reader every second so a same-selection append becomes
    // visible without reselection. It is behavior, not a watcher subsystem:
    // no background thread, debouncing, or persisted state.
    activity_timer_ = new QTimer(body);
    activity_timer_->setInterval(1000);
    QObject::connect(activity_timer_, &QTimer::timeout, [this] {
        render_conversation();
        render_agent_activity();
        render_agent_task_card();
        render_agent_preset_summary();
        if (pending_sleep_observation_) {
            tick_agent_sleep_observation();
        } else if (pending_start_observation_) {
            tick_agent_start_observation();
        } else if (selection_state_.active_project()
                && selection_state_.selected_agent_directory_key()) {
            // No click-armed observation is pending, so eligibility can only
            // go stale here: the target's own state can change (e.g. an
            // ordinary-message wake, or a Start observation from a since-
            // abandoned pending click for a different selection landing in
            // the background) with no click or reselection to trigger a
            // re-check. Reruns the same stateless projection this shell
            // already reruns at every click/settle boundary.
            agents_ = project_agents(*selection_state_.active_project());
            render_agent_sleep_status();
            render_agent_start_status();
        }
    });
    activity_timer_->start();

    bootstrap_runner_ = std::make_unique<ProjectBootstrapRunner>();

    // The one small Desktop-owned New Project dialog: a destination field
    // with Browse, a preset chooser populated from discovery, the explicit
    // Create & Start and Cancel actions, and a truthful note. Built once and
    // hidden until a successful nonempty preset discovery.
    bootstrap_dialog_ = new QDialog(window_.get());
    bootstrap_dialog_->setObjectName("lingtai_new_project_dialog");
    bootstrap_dialog_->setWindowTitle(QStringLiteral("New LingTai Project"));
    bootstrap_dialog_->setAccessibleName(QStringLiteral("New LingTai Project"));
    auto *dialog_layout = new QVBoxLayout(bootstrap_dialog_);
    dialog_layout->setContentsMargins(24, 24, 24, 24);
    dialog_layout->setSpacing(12);
    auto *dialog_note = make_label(
        bootstrap_dialog_,
        QStringLiteral(
            "Creates a new LingTai project, names its first Agent from the "
            "destination folder by default, and starts it."),
        "lingtai_bootstrap_dialog_note",
        12);
    dialog_note->setAccessibleName(QStringLiteral(
        "New project note"));
    dialog_layout->addWidget(dialog_note);

    auto *destination_row = new QHBoxLayout;
    destination_row->setSpacing(8);
    auto *destination_input = new QLineEdit(bootstrap_dialog_);
    destination_input->setObjectName("lingtai_bootstrap_destination_input");
    destination_input->setAccessibleName(QStringLiteral("Destination folder"));
    destination_input->setPlaceholderText(QStringLiteral("Destination folder"));
    destination_row->addWidget(destination_input, 1);
    auto *browse_button = new QPushButton(
        QStringLiteral("Browse…"), bootstrap_dialog_);
    browse_button->setObjectName("lingtai_bootstrap_destination_browse");
    browse_button->setAccessibleName(QStringLiteral("Browse destination folder"));
    QObject::connect(browse_button, &QPushButton::clicked, [this] {
        handle_browse_destination();
    });
    destination_row->addWidget(browse_button);
    dialog_layout->addLayout(destination_row);

    auto *preset_label = make_label(
        bootstrap_dialog_,
        QStringLiteral("Preset"),
        "lingtai_bootstrap_preset_label",
        12,
        QFont::Medium);
    dialog_layout->addWidget(preset_label);
    auto *preset_chooser = new QComboBox(bootstrap_dialog_);
    preset_chooser->setObjectName("lingtai_bootstrap_preset_chooser");
    preset_chooser->setAccessibleName(QStringLiteral("Preset"));
    preset_chooser->setAccessibleDescription(QStringLiteral(
        "Choose the preset the new project's first Agent is created from."));
    dialog_layout->addWidget(preset_chooser);

    auto *dialog_status = make_label(
        bootstrap_dialog_,
        QString(),
        "lingtai_bootstrap_dialog_status",
        11,
        QFont::Medium);
    dialog_status->setAccessibleName(QStringLiteral("New project dialog status"));
    dialog_status->setWordWrap(true);
    dialog_layout->addWidget(dialog_status);

    auto *dialog_actions = new QHBoxLayout;
    dialog_actions->setSpacing(8);
    auto *cancel_button = new QPushButton(
        QStringLiteral("Cancel"), bootstrap_dialog_);
    cancel_button->setObjectName("lingtai_bootstrap_cancel");
    cancel_button->setAccessibleName(QStringLiteral("Cancel"));
    QObject::connect(cancel_button, &QPushButton::clicked, [this] {
        handle_cancel_bootstrap();
    });
    // The standard QDialog dismissal paths (window close control, Escape) call
    // `reject()`, which hides the dialog and emits `rejected` -- it never
    // reaches the explicit Cancel button. Route the real rejected path through
    // the same no-spawn cancellation. Programmatic hides at spawn or finish
    // use `hide()`, which does not emit `rejected`, so they cannot misfire
    // here; `handle_cancel_bootstrap` additionally guards on no pending
    // subprocess.
    QObject::connect(bootstrap_dialog_, &QDialog::rejected, [this] {
        handle_cancel_bootstrap();
    });
    auto *create_button = new QPushButton(
        QStringLiteral("Create & Start"), bootstrap_dialog_);
    create_button->setObjectName("lingtai_bootstrap_create_start");
    create_button->setAccessibleName(QStringLiteral("Create and Start"));
    create_button->setAccessibleDescription(QStringLiteral(
        "Creates the new project, names its first Agent from the destination "
        "folder, and starts that Agent."));
    create_button->setDefault(true);
    QObject::connect(create_button, &QPushButton::clicked, [this] {
        handle_create_and_start();
    });
    dialog_actions->addStretch();
    dialog_actions->addWidget(cancel_button);
    dialog_actions->addWidget(create_button);
    dialog_layout->addLayout(dialog_actions);
    bootstrap_dialog_->hide();

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

void NativeShell::set_agent_start_fallback_python(
        fs::path fallback_python) {
    agent_start_fallback_python_ = std::move(fallback_python);
}

void NativeShell::set_tui_executable(fs::path executable) {
    tui_executable_ = std::move(executable);
}

void NativeShell::set_bootstrap_actions_enabled(bool enabled) {
    if (auto *new_button = window_->findChild<QPushButton *>(
            "lingtai_new_project_button")) {
        new_button->setEnabled(enabled);
    }
    if (auto *open_button = window_->findChild<QPushButton *>(
            "lingtai_open_project_button")) {
        open_button->setEnabled(enabled);
    }
}

void NativeShell::set_bootstrap_status(const QString &text) {
    auto *status = window_->findChild<QLabel *>("lingtai_bootstrap_status");
    if (!status) return;
    status->setText(text);
    bootstrap_status_surface_->setVisible(!text.isEmpty());
}

void NativeShell::request_new_project() {
    if (bootstrap_pending_) return;
    if (tui_executable_.empty()) {
        set_bootstrap_status(QStringLiteral(
            "New Project is unavailable: no TUI executable is configured."));
        return;
    }
    bootstrap_pending_ = true;
    set_bootstrap_actions_enabled(false);
    set_bootstrap_status(QStringLiteral("Discovering presets…"));
    bootstrap_runner_->run_presets(tui_executable_, [this](
            PresetDiscoveryResult result) {
        handle_presets_finished(std::move(result));
    });
}

void NativeShell::handle_presets_finished(PresetDiscoveryResult result) {
    if (result.kind != PresetDiscoveryKind::succeeded
        || result.presets.empty()) {
        bootstrap_pending_ = false;
        set_bootstrap_actions_enabled(true);
        QString failure;
        if (result.kind == PresetDiscoveryKind::process_failed) {
            if (!result.error.empty()) {
                failure = QStringLiteral("Preset discovery failed: %1")
                    .arg(QString::fromStdString(result.error));
            } else {
                failure = QStringLiteral("Preset discovery failed.");
            }
        } else if (result.kind == PresetDiscoveryKind::empty) {
            failure = QStringLiteral(
                "No usable presets were found. Preset discovery returned an "
                "empty list.");
        } else {
            failure = QStringLiteral(
                "Preset discovery returned output that could not be used.");
        }
        set_bootstrap_status(failure);
        return;
    }
    // The flow stays pending while the dialog is open so duplicate New
    // Project / Open Project activation is impossible throughout the whole
    // explicit bootstrap, not only during the two subprocess phases.
    show_bootstrap_dialog(result.presets);
}

void NativeShell::show_bootstrap_dialog(
        const std::vector<PresetEntry> &presets) {
    auto *chooser = window_->findChild<QComboBox *>(
        "lingtai_bootstrap_preset_chooser");
    if (!chooser) return;
    chooser->clear();
    for (const auto &preset : presets) {
        auto help = QStringList();
        if (!preset.description.empty()) {
            help << QString::fromStdString(preset.description);
        }
        if (!preset.tier.empty()) {
            help << QStringLiteral("tier: %1").arg(
                QString::fromStdString(preset.tier));
        }
        if (!preset.source.empty()) {
            help << QStringLiteral("source: %1").arg(
                QString::fromStdString(preset.source));
        }
        chooser->addItem(QString::fromStdString(preset.name));
        if (!help.isEmpty()) {
            chooser->setItemData(chooser->count() - 1,
                help.join(QStringLiteral(" · ")), Qt::ToolTipRole);
        }
    }
    if (auto *status = window_->findChild<QLabel *>(
            "lingtai_bootstrap_dialog_status")) {
        status->clear();
    }
    set_bootstrap_status(QString());
    bootstrap_dialog_->show();
    bootstrap_dialog_->raise();
}

void NativeShell::handle_browse_destination() {
    const auto selected = QFileDialog::getExistingDirectory(
        bootstrap_dialog_,
        QStringLiteral("Choose destination folder"),
        QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) return;
    if (auto *input = window_->findChild<QLineEdit *>(
            "lingtai_bootstrap_destination_input")) {
        input->setText(selected);
    }
}

void NativeShell::handle_create_and_start() {
    if (!bootstrap_dialog_ || !bootstrap_dialog_->isVisible()
        || bootstrap_runner_->is_pending()) {
        return;
    }
    auto *input = window_->findChild<QLineEdit *>(
        "lingtai_bootstrap_destination_input");
    auto *chooser = window_->findChild<QComboBox *>(
        "lingtai_bootstrap_preset_chooser");
    auto *dialog_status = window_->findChild<QLabel *>(
        "lingtai_bootstrap_dialog_status");
    if (!input || !chooser || !dialog_status) return;
    const auto destination = input->text().trimmed();
    const auto preset = chooser->currentText().trimmed();
    if (destination.isEmpty() || preset.isEmpty()) {
        dialog_status->setText(QStringLiteral(
            "Choose a nonempty destination folder and a preset."));
        return;
    }
    if (tui_executable_.empty()) {
        dialog_status->setText(QStringLiteral(
            "No TUI executable is configured."));
        return;
    }
    dialog_status->clear();
    bootstrap_dialog_->hide();
    set_bootstrap_status(QStringLiteral(
        "Creating project and starting Agent…"));
    bootstrap_runner_->run_spawn(tui_executable_,
        fs::path(destination.toStdU16String()),
        preset.toStdString(),
        [this](SpawnOutcome outcome) {
            handle_spawn_finished(std::move(outcome));
        });
}

void NativeShell::handle_cancel_bootstrap() {
    if (!bootstrap_dialog_) return;
    // A user dismissal of the New Project dialog -- the explicit Cancel
    // button, the standard window close control, or Escape -- is always a
    // no-spawn cancellation. It must not run while a discovery/spawn
    // subprocess is pending, and `reject()` hides the dialog before emitting
    // `rejected`, so the old isVisible() guard would have blocked this real
    // path. Programmatic hides at spawn/finish never emit `rejected`.
    if (bootstrap_runner_ && bootstrap_runner_->is_pending()) return;
    bootstrap_pending_ = false;
    bootstrap_dialog_->hide();
    set_bootstrap_status(QString());
    set_bootstrap_actions_enabled(true);
}

void NativeShell::handle_spawn_finished(SpawnOutcome outcome) {
    bootstrap_pending_ = false;
    bootstrap_dialog_->hide();
    set_bootstrap_actions_enabled(true);
    if (outcome.kind != SpawnOutcomeKind::launched
        || outcome.project_dir.empty()) {
        QString failure;
        if (outcome.kind == SpawnOutcomeKind::process_failed) {
            if (!outcome.error.empty()) {
                failure = outcome.code.empty()
                    ? QStringLiteral("Project creation failed: %1").arg(
                        QString::fromStdString(outcome.error))
                    : QStringLiteral("Project creation failed (%1): %2").arg(
                        QString::fromStdString(outcome.code),
                        QString::fromStdString(outcome.error));
            } else {
                failure = QStringLiteral("Project creation failed.");
            }
        } else {
            failure = QStringLiteral(
                "Project creation returned output that could not be used.");
        }
        set_bootstrap_status(failure + QStringLiteral(
            " The destination may contain a partially initialized LingTai "
            "project."));
        return;
    }
    const auto opened = open_project(outcome.project_dir, std::nullopt);
    set_bootstrap_status(opened.disposition == ProjectOpenDisposition::opened
        ? QStringLiteral("Project created and Agent started.")
        : QStringLiteral(
            "Project was created but could not be opened here."));
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
    // A fresh open must never let a prior target's preserved Task Card
    // projection surface under the newly opened project/selection.
    task_card_last_valid_.reset();
    render_roster();
    auto *selection_error = window_->findChild<QLabel *>(
        "lingtai_agent_selection_error");
    selection_error->clear();
    selection_error->hide();
    window_->findChild<QLabel *>("lingtai_project_open_error")->clear();
    open_error_surface_->hide();
    reset_composer();
    // A fresh open must never let a prior target's pending sleep or Start
    // observation surface under the newly opened project/selection.
    pending_sleep_observation_.reset();
    pending_start_observation_.reset();
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
    const auto *content = window_->findChild<Ui::RpWidget *>(
        "lingtai_desktop_content");
    const auto *separator = window_->findChild<Ui::RpWidget *>(
        "lingtai_roster_separator");
    return window_->objectName() == "lingtai_desktop_window"
        && window_->body().get()->objectName() == "lingtai_desktop_body"
        && agent_roster_ && agent_roster_->objectName()
            == "lingtai_desktop_sidebar"
        && content && content->objectName() == "lingtai_desktop_content"
        && separator && separator->objectName() == "lingtai_roster_separator"
        && empty_route_->isVisible()
        && window_->testAttribute(Qt::WA_DontShowOnScreen)
        && window_->isVisible();
}

void NativeShell::request_open_project() {
    if (bootstrap_pending_) return;
    if (open_project_request_handler_) {
        open_project_request_handler_();
    }
}

void NativeShell::render_roster() {
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
    if (!selected_key || !presentation_name || !manifest_identity
        || !manifest_llm || !manifest_capabilities || !status_activity
        || !status_context || !selected_facts) {
        return;
    }

    // The persistent left column owns the roster rows and their state label;
    // it rebuilds its row tree only when the visible model actually changed,
    // so an unchanged one-second projection refresh keeps scroll, focus, and
    // row identity intact.
    agent_roster_->set_rows(
        agents_, selection_state_.selected_agent_directory_key());

    const auto selected = selection_state_.selected_agent_directory_key();
    const AgentRow *detail_item = nullptr;
    for (const auto &item : agents_.items) {
        if (selected && *selected == item.directory_key
            && item.manifest_kind == AgentManifestKind::valid) {
            detail_item = &item;
            break;
        }
    }

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
        render_agent_activity();
        render_agent_task_card();
        render_agent_preset_summary();
        render_agent_sleep_status();
        render_agent_start_status();
        if (auto *start_status = window_->findChild<QLabel *>(
                "lingtai_selected_agent_start_status")) {
            start_status->clear();
        }
        return;
    }
    const auto &identity = detail_item->identity;
    const auto key = path_text(detail_item->directory_key);
    const auto title = identity && identity->nickname
            ? QString::fromStdString(*identity->nickname)
        : identity && identity->true_name
            ? QString::fromStdString(*identity->true_name)
            : key;
    presentation_name->setText(title);
    const auto role_presence = QStringLiteral("role: %1 · presence: %2")
        .arg(role_text(detail_item->role),
            presence_text(detail_item->presence));
    selected_key->setText(title == key
        ? role_presence
        : key + QStringLiteral(" · ") + role_presence);
    if (identity) {
        manifest_identity->setText(QStringLiteral(
            "Manifest identity\naddress: %1\nagent ID: %2\nstate: %3")
            .arg(value_text(identity->address),
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
    render_agent_activity();
    render_agent_task_card();
    render_agent_preset_summary();
    render_agent_sleep_status();
    render_agent_start_status();
    if (auto *start_status = window_->findChild<QLabel *>(
            "lingtai_selected_agent_start_status")) {
        start_status->clear();
    }
}

// Shows the current direct conversation for whatever the roster just made the
// selected Agent. It reads the human's own mailbox and infers nothing about
// delivery, replies, or unread state.
void NativeShell::render_conversation() {
    auto *surface = window_->findChild<ConversationSurface *>(
        "lingtai_selected_agent_conversation");
    auto *state = window_->findChild<QLabel *>(
        "lingtai_selected_agent_conversation_state");
    auto *composer_input = static_cast<Ui::InputField *>(
        window_->findChild<QObject *>("lingtai_composer_input"));
    auto *send_button = static_cast<Ui::RoundButton *>(
        window_->findChild<QObject *>("lingtai_composer_send_button"));
    if (!surface || !state || !composer_input || !send_button) return;
    // Composer enablement only; never touches typed text or send status, so a
    // refresh right after a send does not erase the status it just set.
    const auto set_composer_eligible = [&](bool eligible) {
        composer_input->setEnabled(eligible);
        send_button->setEnabled(eligible);
    };

    if (!selection_state_.active_project()
        || !selection_state_.selected_agent_directory_key()) {
        surface->set_plain_state(QStringLiteral(
            "Select an Agent to see your conversation."));
        state->setText(QString());
        set_composer_eligible(false);
        return;
    }
    const auto route = resolve_direct_conversation_route(
        *selection_state_.active_project(), agents_,
        selection_state_.selected_agent_directory_key());
    if (!route) {
        surface->set_plain_state(QStringLiteral(
                "No conversation is available for this selection."));
        state->setText(QString());
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
    // The owner rebuilds only on real change and owns the exact was-at-bottom
    // capture plus scroll restoration; composer code stays untouched.
    if (history.messages.empty()) {
        surface->set_plain_state(QStringLiteral("No messages yet."));
    } else {
        surface->set_conversation(them, history.messages);
    }
    const auto count = history.messages.size();
    auto compact = count == 1
        ? QStringLiteral("1 message")
        : QStringLiteral("%1 messages").arg(count);
    if (history.skipped > 0) {
        compact += QStringLiteral(" · %1 skipped").arg(history.skipped);
    }
    state->setText(compact);
}

// Called only when the selected target actually changes (a fresh project open
// or a successful Agent selection), never on an ordinary conversation
// refresh, so a just-set "Queued" status is never wiped by its own refresh.
void NativeShell::reset_composer() {
    if (auto *input = static_cast<Ui::InputField *>(
            window_->findChild<QObject *>("lingtai_composer_input"))) {
        input->clear();
    }
    if (auto *status = window_->findChild<QLabel *>("lingtai_composer_status")) {
        status->clear();
    }
}

// Shows a bounded read-only snapshot of the selected Agent's own visible
// activity: public diary text plus reduced tool call/result rows. This is a
// distinct source and surface from the mailbox conversation above, refreshed
// on the same explicit open/selection paths plus the one-second timer.
void NativeShell::render_agent_activity() {
    auto *surface = window_->findChild<QPlainTextEdit *>(
        "lingtai_selected_agent_activity");
    auto *state = window_->findChild<QLabel *>(
        "lingtai_selected_agent_activity_state");
    if (!surface || !state) return;
    const auto show = [&](const QString &text, const QString &compact) {
        surface->setPlainText(text);
        state->setText(compact);
    };

    if (!selection_state_.active_project()
        || !selection_state_.selected_agent_directory_key()) {
        show(QStringLiteral("Select an Agent to see its activity."),
            QString());
        return;
    }
    const auto snapshot = read_agent_activity(
        *selection_state_.active_project(),
        *selection_state_.selected_agent_directory_key());
    if (!snapshot.available) {
        show(QStringLiteral("No activity is available for this selection."),
            QString());
        return;
    }

    auto blocks = QStringList();
    for (const auto &row : snapshot.rows) {
        auto block = QString();
        if (row.is_tool) {
            block = QStringLiteral("Tool · %1")
                .arg(QString::fromStdString(row.tool_name));
            if (!row.tool_action.empty()) {
                block += QStringLiteral(" (%1)")
                    .arg(QString::fromStdString(row.tool_action));
            }
            block += row.tool_status == AgentActivityToolStatus::success
                ? QStringLiteral(" — success")
                : row.tool_status == AgentActivityToolStatus::error
                    ? QStringLiteral(" — error")
                    : QStringLiteral(" — unknown");
            if (row.tool_elapsed_ms) {
                block += QStringLiteral(" (%1 ms)").arg(*row.tool_elapsed_ms);
            }
        } else {
            block = QStringLiteral("Agent · %1")
                .arg(QString::fromStdString(row.text));
        }
        if (!row.timestamp_text.empty()) {
            block += QStringLiteral("\n%1")
                .arg(QString::fromStdString(row.timestamp_text));
        }
        blocks.push_back(block);
    }
    auto compact = QStringLiteral("%1 row(s)").arg(snapshot.rows.size());
    if (snapshot.skipped > 0) {
        compact += QStringLiteral(" · some activity records were skipped");
    }
    show(blocks.isEmpty() ? QStringLiteral("No activity yet.")
                          : blocks.join(QStringLiteral("\n\n")), compact);
}

// Shows the selected Agent's current self-published Task Card: only
// `taskcard/status` and, when exactly `active`, `taskcard/taskcard.md`. It
// is a distinct source and surface from both the mailbox conversation and
// Agent Activity, refreshed on the same explicit open/selection paths plus
// the one-second timer. A transient unavailable observation for the same
// selected target preserves the last valid active/inactive projection
// rather than clearing or erroring it; only an exact inactive status or a
// project/selection change clears a preserved active body.
void NativeShell::render_agent_task_card() {
    auto *surface = window_->findChild<QPlainTextEdit *>(
        "lingtai_selected_agent_task_card");
    auto *state = window_->findChild<QLabel *>(
        "lingtai_selected_agent_task_card_state");
    if (!surface || !state) return;
    const auto show = [&](const QString &text, const QString &compact) {
        if (surface->toPlainText() != text) surface->setPlainText(text);
        if (state->text() != compact) state->setText(compact);
    };

    if (!selection_state_.active_project()
        || !selection_state_.selected_agent_directory_key()) {
        task_card_last_valid_.reset();
        show(QStringLiteral("Select an Agent to see its Task Card."),
            QString());
        return;
    }

    const auto snapshot = read_agent_task_card(
        *selection_state_.active_project(),
        *selection_state_.selected_agent_directory_key());
    if (snapshot.state != AgentTaskCardState::unavailable) {
        task_card_last_valid_ = snapshot;
    }
    const auto &projected = snapshot.state != AgentTaskCardState::unavailable
        ? snapshot
        : task_card_last_valid_.value_or(AgentTaskCardSnapshot{});

    if (projected.state == AgentTaskCardState::active) {
        show(QString::fromStdString(projected.body), QStringLiteral("Active"));
    } else if (projected.state == AgentTaskCardState::inactive) {
        show(QStringLiteral("No active Task Card."), QStringLiteral("Inactive"));
    } else {
        show(QStringLiteral("Task Card unavailable."), QString());
    }
}

// Shows the selected Agent's own kernel-published resolved preset policy
// and active effective configuration: only `system/manifest.resolved.json`.
// It is a distinct source and surface from the mailbox conversation, Agent
// Activity, and Task Card above, refreshed on the same explicit open/
// selection paths plus the one-second timer. Unlike Task Card, there is no
// last-valid preservation: every observation is shown exactly as read, so
// an absent/stale/unavailable current observation never keeps a prior
// target's projection visible.
void NativeShell::render_agent_preset_summary() {
    auto *surface = window_->findChild<QPlainTextEdit *>(
        "lingtai_selected_agent_preset_summary");
    auto *state = window_->findChild<QLabel *>(
        "lingtai_selected_agent_preset_summary_state");
    if (!surface || !state) return;
    const auto show = [&](const QString &text, const QString &compact) {
        if (surface->toPlainText() != text) surface->setPlainText(text);
        if (state->text() != compact) state->setText(compact);
    };

    if (!selection_state_.active_project()
        || !selection_state_.selected_agent_directory_key()) {
        show(QStringLiteral("Select an Agent to see its Presets."), QString());
        return;
    }

    const auto summary = read_agent_preset_summary(
        *selection_state_.active_project(),
        *selection_state_.selected_agent_directory_key());

    switch (summary.source) {
    case AgentPresetSummarySource::not_yet_published:
        show(QStringLiteral(
                "No resolved preset summary has been published yet."),
            QStringLiteral("Not yet published"));
        return;
    case AgentPresetSummarySource::unavailable:
        show(QStringLiteral("Preset summary is unavailable."),
            QStringLiteral("Unavailable"));
        return;
    case AgentPresetSummarySource::resolved:
    case AgentPresetSummarySource::stale:
        break;
    }

    auto lines = QStringList();
    lines << QStringLiteral("Active:  %1").arg(value_text(summary.active_ref));
    lines << QStringLiteral("Default: %1").arg(value_text(summary.default_ref));
    lines << QStringLiteral("Allowed:");
    for (const auto &ref : summary.allowed) {
        auto badges = QStringList();
        if (ref.is_active) badges << QStringLiteral("Active");
        if (ref.is_default) badges << QStringLiteral("Default");
        lines << (badges.isEmpty()
            ? QStringLiteral("  • %1").arg(QString::fromStdString(ref.ref))
            : QStringLiteral("  • [%1] %2")
                  .arg(badges.join(QStringLiteral(", ")),
                      QString::fromStdString(ref.ref)));
    }
    lines << QString();
    lines << QStringLiteral("Active effective");
    lines << QStringLiteral("  Provider: %1")
        .arg(value_text(summary.effective.provider));
    lines << QStringLiteral("  Model: %1").arg(value_text(summary.effective.model));
    lines << QStringLiteral("  Context limit: %1")
        .arg(value_text(summary.effective.context_limit));
    lines << QStringLiteral("  Capabilities: %1")
        .arg(joined_names(summary.effective.capability_names));
    lines << QString();
    lines << QStringLiteral("Source: kernel · generated %1")
        .arg(value_text(summary.generated_at));

    show(lines.join(QStringLiteral("\n")),
        summary.source == AgentPresetSummarySource::stale
            ? QStringLiteral("Stale") : QStringLiteral("Resolved"));
}

// Always resolves the route fresh from current C1/C3 truth rather than
// capturing it once, so a selection change between typing and clicking Send
// can never deliver to a stale target.
void NativeShell::handle_send_message() {
    auto *input = static_cast<Ui::InputField *>(
        window_->findChild<QObject *>("lingtai_composer_input"));
    auto *status = window_->findChild<QLabel *>("lingtai_composer_status");
    if (!input || !status) return;
    const auto text = input->getLastText().trimmed();
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
    // A selection change must never let a prior target's pending sleep or
    // Start observation, terminal result, or preserved Task Card
    // projection surface under the newly selected Agent.
    pending_sleep_observation_.reset();
    pending_start_observation_.reset();
    task_card_last_valid_.reset();
    render_roster();
    if (error) {
        error->clear();
        error->hide();
    }
}

// Reflects only the eligibility of the exact current selection against
// whatever `agents_` currently holds. Also reached through `render_roster()`
// from a click or a timer tick, but only before either overwrites the
// button/status with its own more specific write/observation text, so this
// never clobbers in-progress or terminal wording for the current selection.
void NativeShell::render_agent_sleep_status() {
    auto *button = window_->findChild<QPushButton *>(
        "lingtai_selected_agent_request_sleep");
    auto *status = window_->findChild<QLabel *>(
        "lingtai_selected_agent_sleep_status");
    if (!button || !status) return;

    const auto *item = selection_state_.active_project()
            && selection_state_.selected_agent_directory_key()
        ? selectable_item(agents_,
              *selection_state_.selected_agent_directory_key())
        : nullptr;
    const auto eligible = item && agent_sleep_eligible(*item);
    button->setEnabled(eligible);
    status->setText(eligible
        ? QString()
        : QStringLiteral("Select a live Agent that is not already asleep."));
}

// The human's explicit click is the local product action. Rerun the sole
// `project_agents` projection once at the click boundary and use only the
// fresh exact row for the exact current selection -- never a cached one --
// before writing anything.
void NativeShell::handle_request_sleep() {
    auto *status = window_->findChild<QLabel *>(
        "lingtai_selected_agent_sleep_status");
    auto *button = window_->findChild<QPushButton *>(
        "lingtai_selected_agent_request_sleep");
    if (!status || !button) return;
    if (pending_sleep_observation_) return; // one observation at a time

    if (!selection_state_.active_project()
        || !selection_state_.selected_agent_directory_key()) {
        return;
    }
    const auto &attachment = *selection_state_.active_project();
    const auto key = *selection_state_.selected_agent_directory_key();

    agents_ = project_agents(attachment);
    render_roster();

    const auto *item = selectable_item(agents_, key);
    if (!item || !agent_sleep_eligible(*item)) {
        status->setText(
            QStringLiteral("Select a live Agent that is not already asleep."));
        button->setEnabled(false);
        return;
    }

    const auto baseline = capture_agent_sleep_event_baseline(attachment, key);
    const auto result = request_agent_sleep(attachment, key);
    if (result != AgentSleepRequestResult::requested) {
        status->setText(QStringLiteral("Sleep request not written."));
        return;
    }

    status->setText(QStringLiteral("Sleep requested."));
    button->setEnabled(false); // disable duplicate click while observing
    pending_sleep_observation_ = SleepObservation{
        .project_root = attachment.root(),
        .directory_key = key,
        .baseline = baseline,
        .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3),
    };
}

// Piggybacks on the existing one-second view timer: no new timer, thread, or
// watcher. Idle unless a click armed a pending observation for the exact
// current selection.
void NativeShell::tick_agent_sleep_observation() {
    if (!pending_sleep_observation_) return;
    if (!selection_state_.active_project()
        || selection_state_.active_project()->root()
            != pending_sleep_observation_->project_root
        || !selection_state_.selected_agent_directory_key()
        || *selection_state_.selected_agent_directory_key()
            != pending_sleep_observation_->directory_key) {
        // The target changed since the click; a late result must never
        // appear under a different selection.
        pending_sleep_observation_.reset();
        return;
    }

    const auto &attachment = *selection_state_.active_project();
    const auto key = pending_sleep_observation_->directory_key;
    const auto applied = observe_agent_sleep_received(
        attachment, key, pending_sleep_observation_->baseline);
    const auto expired = std::chrono::steady_clock::now()
        >= pending_sleep_observation_->deadline;
    if (!applied && !expired) return; // keep waiting within the window

    pending_sleep_observation_.reset();
    agents_ = project_agents(attachment);
    render_roster();

    auto *status = window_->findChild<QLabel *>(
        "lingtai_selected_agent_sleep_status");
    auto *button = window_->findChild<QPushButton *>(
        "lingtai_selected_agent_request_sleep");
    if (!status || !button) return;
    const auto *item = selectable_item(agents_, key);
    const auto state = item && item->identity && item->identity->state
        ? QString::fromStdString(*item->identity->state)
        : QString();
    if (applied) {
        const auto woke = state == QStringLiteral("active")
            || state == QStringLiteral("idle");
        status->setText(woke
            ? QStringLiteral("Sleep applied; Agent subsequently woke. "
                  "Current state: %1.")
                  .arg(state)
            : state.isEmpty()
                ? QStringLiteral("Sleep request applied.")
                : QStringLiteral("Sleep request applied. Current state: %1.")
                    .arg(state));
    } else {
        status->setText(state.isEmpty()
            ? QStringLiteral("Sleep requested; application not yet observed.")
            : QStringLiteral("Sleep requested; application not yet "
                  "observed. Current state: %1.")
                  .arg(state));
    }
    button->setEnabled(item && agent_sleep_eligible(*item));
}

// Reflects only the eligibility of the exact current selection against
// whatever `agents_` currently holds, mirroring the button/enabled half of
// render_agent_sleep_status() -- but deliberately *not* the status-text
// half. This is called every second from the idle ambient timer branch
// (with no click or resolution involved) purely to keep the button's
// visible/enabled state honest against external drift (e.g. an ordinary-
// message wake with no reselection); it must never touch the status label,
// or it would erase a just-shown "Starting Agent...", "Agent is online.",
// or failure message on the very next tick after it was set. Callers that
// genuinely need a fresh status slate (selection change, project open, a
// click/tick resolution) clear the label themselves at that specific
// point, not through this function.
void NativeShell::render_agent_start_status() {
    auto *button = window_->findChild<QPushButton *>(
        "lingtai_selected_agent_start_agent");
    if (!button) return;

    const auto *item = selection_state_.active_project()
            && selection_state_.selected_agent_directory_key()
        ? selectable_item(agents_,
              *selection_state_.selected_agent_directory_key())
        : nullptr;
    const auto eligible = item && agent_start_eligible(*item);
    button->setVisible(eligible);
    button->setEnabled(eligible);
}

// The human's explicit click is the local product action. Rerun the sole
// `project_agents` projection once at the click boundary and use only the
// fresh exact row for the exact current selection -- never a cached one --
// before starting anything. Request sleep is never separately disabled
// here: it already requires `alive` presence, which a start-eligible
// (stale/missing) row can never have at this exact instant.
void NativeShell::handle_start_agent() {
    auto *status = window_->findChild<QLabel *>(
        "lingtai_selected_agent_start_status");
    auto *button = window_->findChild<QPushButton *>(
        "lingtai_selected_agent_start_agent");
    if (!status || !button) return;
    if (pending_start_observation_) return; // one observation at a time

    if (!selection_state_.active_project()
        || !selection_state_.selected_agent_directory_key()) {
        return;
    }
    const auto &attachment = *selection_state_.active_project();
    const auto key = *selection_state_.selected_agent_directory_key();

    agents_ = project_agents(attachment);
    render_roster();

    const auto *item = selectable_item(agents_, key);
    if (!item || !agent_start_eligible(*item)) {
        return; // render_roster() above already reflects fresh eligibility
    }

    const auto result =
        start_agent(attachment, key, agent_start_fallback_python_);
    if (result != AgentLaunchResult::started) {
        status->setText(QStringLiteral(
            "Could not start Agent. See %1/logs/agent.log.")
            .arg(path_text(attachment.root() / ".lingtai" / key)));
        return;
    }

    status->setText(QStringLiteral("Starting Agent..."));
    button->setEnabled(false); // disable duplicate click while observing
    pending_start_observation_ = StartObservation{
        .project_root = attachment.root(),
        .directory_key = key,
        .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10),
    };
}

// Piggybacks on the existing one-second view timer: no new timer, thread, or
// watcher, and no second heartbeat parser. Idle unless a click armed a
// pending observation for the exact current selection. Success is proven
// solely by the sole `project_agents` projection reporting this exact
// selection `alive`; `agent_start_eligible()`'s narrowed stale/missing gate
// is what makes that transition trustworthy without a separate timestamp
// baseline.
void NativeShell::tick_agent_start_observation() {
    if (!pending_start_observation_) return;
    if (!selection_state_.active_project()
        || selection_state_.active_project()->root()
            != pending_start_observation_->project_root
        || !selection_state_.selected_agent_directory_key()
        || *selection_state_.selected_agent_directory_key()
            != pending_start_observation_->directory_key) {
        // The target changed since the click; a late result must never
        // appear under a different selection.
        pending_start_observation_.reset();
        return;
    }

    const auto &attachment = *selection_state_.active_project();
    const auto key = pending_start_observation_->directory_key;
    agents_ = project_agents(attachment);
    const auto *item = selectable_item(agents_, key);
    const auto online = item && item->presence == AgentPresenceKind::alive;
    const auto expired = std::chrono::steady_clock::now()
        >= pending_start_observation_->deadline;
    if (!online && !expired) return; // keep waiting within the window

    pending_start_observation_.reset();
    render_roster();

    auto *status = window_->findChild<QLabel *>(
        "lingtai_selected_agent_start_status");
    if (!status) return;
    status->setText(online
        ? QStringLiteral("Agent is online.")
        : QStringLiteral("Agent did not come online. See %1/logs/agent.log.")
              .arg(path_text(attachment.root() / ".lingtai" / key)));
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
