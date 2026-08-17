#include "native_shell.h"

#include "agent_preset_summary.h"
#include "agent_sleep.h"
#include "direct_conversation_history.h"
#include "direct_mail_publisher.h"
#include "slash_command.h"

#include "base/event_filter.h"
#include "base/integration.h"

#include "styles/palette.h"
#include "styles/style_widgets.h"
#include "ui/conversation_surface.h"
#include "ui/effects/animations.h"
#include "ui/integration.h"
#include "ui/rp_widget.h"
#include "ui/style/style_core_palette.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/rp_window.h"
#include "ui/widgets/shadow.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QPoint>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtGui/QFont>
#include <QtGui/QFontMetrics>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPalette>
#include <QtGui/QStyleHints>
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
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QWidget>

#include <rpl/range.h>

#include <algorithm>
#include <array>
#include <utility>

namespace lingtai::desktop {
namespace {

constexpr auto kMinimumWindowWidth = 380;
constexpr auto kMinimumWindowHeight = 480;
constexpr auto kDefaultWindowWidth = 1100;
constexpr auto kDefaultWindowHeight = 720;

// Telegram's source-backed wide two-surface minima, from the pinned
// `computeColumnLayout` / `window.style`: the preferred list column is 260px
// and the detail column remains at least 380px. Unlike the old hard lower
// bound, a direct drag may collapse the roster to one 40px avatar plus the
// row and Sidebar framing (96px total), matching Telegram's narrow row paint.
// The default runtime ratio stays in the 22%-30% wide band; only an explicit
// drag below that band enters the collapsed range.
constexpr auto kRosterColumnWidth = 260;
constexpr auto kCollapsedRosterColumnWidth = 96;
constexpr auto kDetailColumnMinimumWidth = 380;
constexpr auto kRosterSeparatorWidth = 1;
constexpr auto kWideRosterWidthRatio = 0.22;
constexpr auto kMaximumRosterWidthRatio = 0.30;
constexpr auto kRosterResizeHandleWidth = 8;
constexpr auto kTwoColumnAvailableThreshold =
    kRosterColumnWidth + kDetailColumnMinimumWidth;

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

// The first-project surfaces use the same mature lib_ui label language as the
// accepted composer/dashboard: a FlatLabel whose accessible text is its own
// readable text, with the exact current object name and accessibility label.
Ui::FlatLabel *make_flat_label(
        QWidget *parent,
        const QString &text,
        const char *object_name) {
    auto *label = new Ui::FlatLabel(parent, text);
    label->setObjectName(object_name);
    label->setAccessibleName(text);
    return label;
}

// The vendored lib_ui controls (InputField, RoundButton, FlatLabel) carry no
// Q_OBJECT macro, so Qt's templated findChild<Ui::X *> cannot name them. These
// classes are polymorphic, so resolving by object name through a QObject
// lookup plus dynamic_cast stays safe and type-checked.
template <typename Widget>
Widget *find_ui_child(QObject &root, const char *object_name) {
    return dynamic_cast<Widget *>(root.findChild<QObject *>(object_name));
}

// One LingTai-owned full-surface widget whose background is painted from the
// shared lib_ui palette (never a raw white Qt surface): the right chat/content
// pane fills `windowBg`, the same token the chat surface and top bar use.
class PaletteSurface final : public Ui::RpWidget {
public:
    explicit PaletteSurface(QWidget *parent, style::color fill)
    : Ui::RpWidget(parent)
    , fill_(std::move(fill)) {
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), fill_);
    }

private:
    style::color fill_;
};

// One LingTai-owned semantic drag handle for the roster column: a fixed 8px
// strip between the roster and its one-pixel shadow that reports only the
// pointer's current global x while the primary button is held, so the shell
// can re-derive the runtime-only roster width ratio. It paints nothing and is
// deliberately distinct from the passive `Ui::PlainShadow` that follows it.
class RosterResizeHandle final : public QWidget {
public:
    using GlobalXCallback = std::function<void(int global_x)>;

    explicit RosterResizeHandle(QWidget *parent, GlobalXCallback callback)
    : QWidget(parent)
    , callback_(std::move(callback)) {
        setFixedWidth(kRosterResizeHandleWidth);
        setCursor(Qt::SplitHCursor);
        auto policy = sizePolicy();
        policy.setVerticalPolicy(QSizePolicy::Expanding);
        setSizePolicy(policy);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            dragging_ = true;
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (dragging_) {
            callback_(event->globalPosition().toPoint().x());
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            dragging_ = false;
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    GlobalXCallback callback_;
    bool dragging_ = false;
};

// The one compact selected-Agent page navigation control: a plain text tab
// that is never a filled rectangular slab. Every state paints only its caption
// glyphs on the transparent shell backdrop, and the selected page adds just
// one short `dialogsBgActive` accent underline along its bottom edge.
class PageNavButton final : public QPushButton {
public:
    explicit PageNavButton(QWidget *parent, const QString &text)
    : QPushButton(text, parent) {
        setCheckable(true);
        setFixedHeight(24);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        auto font = this->font();
        font.setPointSize(11);
        font.setWeight(QFont::Normal);
        painter.setFont(font);
        painter.setPen(isChecked() ? st::dialogsTextFgService : st::windowSubTextFg);
        painter.drawText(rect(), Qt::AlignCenter, text());
        if (isChecked()) {
            const auto underline_width = qMin(width(), 24);
            painter.fillRect(
                (width() - underline_width) / 2, height() - 2,
                underline_width, 2, st::dialogsBgActive);
        }
    }
};

// One compact palette-owned action button shared by the three detail-top-bar
// actions (Back, Start Agent, Request sleep): it paints its resting/hover text
// and backgrounds from the same shared lib_ui light-button tokens the dialog
// actions use, and the disabled state from the existing disabled text token,
// so the three controls never fall back to the raw platform button style.
class PaletteActionButton final : public QPushButton {
public:
    explicit PaletteActionButton(QWidget *parent, const QString &text)
    : QPushButton(text, parent) {
        setFixedHeight(26);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto radius = qMin(width(), height()) / 2;
        painter.setPen(Qt::NoPen);
        painter.setBrush(!isEnabled()
            ? st::defaultLightButton.textBg->c
            : (isDown() || underMouse())
                ? st::defaultLightButton.textBgOver->c
                : st::defaultLightButton.textBg->c);
        painter.drawRoundedRect(rect(), radius, radius);
        auto font = this->font();
        font.setPointSize(11);
        painter.setFont(font);
        painter.setPen(!isEnabled()
            ? st::windowSubTextFg->c
            : (isDown() || underMouse())
                ? st::defaultLightButton.textFgOver->c
                : st::defaultLightButton.textFg->c);
        painter.drawText(rect(), Qt::AlignCenter, text());
    }
};

// One compact palette-owned icon-only lifecycle secondary: the same light-pill
// language as `PaletteActionButton`, but painted with only a small crescent
// glyph and never a caption, so its accessible name stays the only label a
// screen reader hears and the header keeps exactly one captioned action.
class PaletteIconButton final : public QPushButton {
public:
    explicit PaletteIconButton(QWidget *parent)
    : QPushButton(parent) {
        setFixedSize(26, 26);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto radius = qMin(width(), height()) / 2;
        const auto pill = !isEnabled()
            ? st::defaultLightButton.textBg->c
            : (isDown() || underMouse())
                ? st::defaultLightButton.textBgOver->c
                : st::defaultLightButton.textBg->c;
        painter.setPen(Qt::NoPen);
        painter.setBrush(pill);
        painter.drawRoundedRect(rect(), radius, radius);
        const auto ink = !isEnabled()
            ? st::windowSubTextFg->c
            : (isDown() || underMouse())
                ? st::defaultLightButton.textFgOver->c
                : st::defaultLightButton.textFg->c;
        // A small crescent moon: one full disc with an overlapping pill-colored
        // disc carving the crescent, so the glyph never needs an icon font.
        const auto size = 13.0;
        const auto center = QPointF(width() / 2.0, height() / 2.0);
        painter.setBrush(ink);
        painter.drawEllipse(center, size / 2.0, size / 2.0);
        painter.setBrush(pill);
        painter.drawEllipse(
            center + QPointF(size * 0.55, -size * 0.25),
            size * 0.52, size * 0.52);
    }
};

// The selected-Agent header reuses the Sidebar's initial-circle avatar
// language on a neutral header surface. The owning title is also exposed as
// its accessibility description, so the glyph never becomes an opaque icon.
class SelectedAgentAvatar final : public QWidget {
public:
    explicit SelectedAgentAvatar(QWidget *parent)
    : QWidget(parent) {
        setFixedSize(38, 38);
        setAccessibleName(QStringLiteral("Selected Agent avatar"));
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void set_agent_name(QString name) {
        if (name_ == name) return;
        name_ = std::move(name);
        setAccessibleDescription(name_);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (name_.isEmpty()) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(st::dialogsNameFg);
        painter.drawEllipse(QRectF(rect()).adjusted(1, 1, -1, -1));
        auto font = this->font();
        font.setPointSize(13);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(st::windowBg);
        painter.drawText(
            rect(), Qt::AlignCenter, name_.left(1).toUpper());
    }

private:
    QString name_;
};

// One shared structural owner for the one retained read-only selected-Agent
// source section (Presets). Each section directly owns its own semibold
// heading, read-only plain-text surface, and state line, with the same inner
// margins and spacing, so the retained source uses one consistent local
// framing instead of a hand-built heading/surface/state sequence. Sections
// never frame themselves with a plain-shadow block: they separate by the
// surrounding layout spacing alone.
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

bool system_prefers_dark_palette() {
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Dark) return true;
    if (scheme == Qt::ColorScheme::Light) return false;
    const auto palette = QGuiApplication::palette();
    return palette.color(QPalette::WindowText).lightness()
        > palette.color(QPalette::Window).lightness();
}

void apply_telegram_night_palette() {
    const auto set = [](const char *name, const char *hex) {
        const auto color = QColor(QString::fromLatin1(hex));
        const auto result = style::main_palette::setColor(
            QLatin1String(name),
            static_cast<uchar>(color.red()),
            static_cast<uchar>(color.green()),
            static_cast<uchar>(color.blue()),
            static_cast<uchar>(color.alpha()));
        Q_ASSERT(result == style::palette::SetResult::Ok
            || result == style::palette::SetResult::Duplicate);
        (void)result;
    };

    set("windowBg", "#17212b");
    set("windowFg", "#f5f5f5");
    set("windowBgOver", "#232e3c");
    set("windowBgRipple", "#24303d");
    set("windowFgOver", "#f5f5f5");
    set("windowSubTextFg", "#708499");
    set("windowSubTextFgOver", "#7f91a4");
    set("windowBoldFg", "#e9e8e8");
    set("windowBoldFgOver", "#e9e8e8");
    set("windowBgActive", "#2b5278");
    set("windowFgActive", "#ffffff");
    set("windowActiveTextFg", "#6ab3f3");
    set("activeButtonBg", "#2b5278");
    set("activeButtonBgOver", "#356487");
    set("activeButtonBgRipple", "#3b6d91");
    set("activeButtonFg", "#ffffff");
    set("activeButtonFgOver", "#ffffff");
    set("activeLineFg", "#6ab3f3");
    set("lightButtonBg", "#17212b");
    set("lightButtonBgOver", "#232e3c");
    set("lightButtonBgRipple", "#24303d");
    set("lightButtonFg", "#6ab3f3");
    set("lightButtonFgOver", "#6ab3f3");
    set("placeholderFg", "#708499");
    set("placeholderFgActive", "#7f91a4");
    set("inputBorderFg", "#24303d");
    set("dialogsBg", "#17212b");
    set("dialogsNameFg", "#f5f5f5");
    set("dialogsNameFgOver", "#f5f5f5");
    set("dialogsNameFgActive", "#ffffff");
    set("dialogsTextFg", "#7f91a4");
    set("dialogsTextFgOver", "#7f91a4");
    set("dialogsTextFgActive", "#e4ecf2");
    set("dialogsBgOver", "#202b36");
    set("dialogsBgActive", "#2b5278");
    set("historyTextInFg", "#f5f5f5");
    set("historyTextOutFg", "#e4ecf2");
    set("msgInBg", "#182533");
    set("msgOutBg", "#2b5278");
    set("msgServiceFg", "#708499");
}

void apply_system_palette() {
    style::main_palette::reset();
    if (system_prefers_dark_palette()) {
        apply_telegram_night_palette();
    }
}

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
        apply_system_palette();
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

    // One semantic drag handle between the roster and its shadow: a fixed 8px
    // strip that reports the pointer's real global x while the primary button
    // is held, so the shell re-derives the runtime-only roster width ratio. It
    // is distinct from the passive one-pixel shadow that immediately follows
    // it.
    auto *resize_handle = new RosterResizeHandle(body, [this, body](int global_x) {
        const auto body_width = body->width();
        const auto usable = body_width - kRosterResizeHandleWidth
            - kRosterSeparatorWidth;
        if (usable < kTwoColumnAvailableThreshold) return;
        const auto local_x = body->mapFromGlobal(QPoint(global_x, 0)).x();
        const auto chosen_px = std::clamp(local_x,
            kCollapsedRosterColumnWidth,
            body_width - kDetailColumnMinimumWidth
                - kRosterResizeHandleWidth - kRosterSeparatorWidth);
        roster_width_ratio_ = std::clamp(
            double(chosen_px) / double(body_width),
            double(kCollapsedRosterColumnWidth) / double(body_width),
            kMaximumRosterWidthRatio);
        recompute_layout(body_width);
    });
    roster_resize_handle_ = resize_handle;
    resize_handle->setObjectName("lingtai_roster_resize_handle");
    resize_handle->setAccessibleName(QStringLiteral("Resize Agent list"));
    resize_handle->setAccessibleDescription(QStringLiteral(
        "Drag to resize the Agent list"));
    shell_layout->addWidget(resize_handle);

    // One thin lib_ui shadow separates the persistent list column from the
    // selected-content pane, matching the pinned shell's between-column
    // `_sideShadow` geometry.
    auto *separator = new Ui::PlainShadow(body);
    separator_ = separator;
    separator->setObjectName("lingtai_roster_separator");
    separator->setAccessibleName(QStringLiteral("Project list divider"));
    shell_layout->addWidget(separator);

    auto *content = new PaletteSurface(body, st::windowBg);
    content_ = content;
    content->setObjectName("lingtai_desktop_content");
    content->setAccessibleName(QStringLiteral("Workspace content"));
    auto palette = content->palette();
    palette.setColor(QPalette::Window, st::windowBg->c);
    content->setPalette(palette);
    content->setAutoFillBackground(true);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    shell_layout->addWidget(content, 1);

    auto *content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);
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

    open_error_surface_ = new Ui::RpWidget(content);
    open_error_surface_->setObjectName("lingtai_project_open_error_surface");
    auto *error_layout = new QVBoxLayout(open_error_surface_);
    error_layout->setContentsMargins(0, 0, 0, 0);
    auto *open_error = make_flat_label(
        open_error_surface_,
        QString(),
        "lingtai_project_open_error");
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
    auto *bootstrap_status = make_flat_label(
        bootstrap_status_surface_,
        QString(),
        "lingtai_bootstrap_status");
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
    empty_layout->setContentsMargins(24, 24, 24, 24);
    empty_layout->setSpacing(12);
    auto *empty_title = make_flat_label(
        empty_route_,
        QStringLiteral("No project open"),
        "lingtai_no_project_title");
    auto *empty_detail = make_flat_label(
        empty_route_,
        QStringLiteral("Open a LingTai project to inspect its Agents."),
        "lingtai_no_project_detail");
    // The branding rhythm between the no-project title/purpose and this empty
    // route belongs to the empty-route-only layout, so a selected project's
    // route never inherits a shared spacer above the active workspace.
    empty_layout->addSpacing(40);
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
    project_layout->setSpacing(0);
    // The old full-width "Project" heading stays as a hidden semantic anchor:
    // a selected project's right pane is now one chat-first surface whose
    // identity lives in the top bar, not a dashboard heading.
    auto *project_heading = make_label(
        project_route_,
        QStringLiteral("Project"),
        "lingtai_project_route_heading",
        18,
        QFont::DemiBold);
    project_heading->hide();
    project_layout->addWidget(project_heading);
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
    directory_layout->setContentsMargins(0, 0, 0, 0);

    // The detail column carries more evidence than any window is tall, so it
    // scrolls like the roster instead of overflowing and overpainting itself.
    auto *detail_scroll = new QScrollArea(directory);
    detail_scroll->setObjectName("lingtai_agent_detail_scroll");
    detail_scroll->setAccessibleName(QStringLiteral("Selected Agent detail"));
    detail_scroll->setWidgetResizable(true);
    detail_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    directory_layout->addWidget(detail_scroll, 1);
    auto *detail = new Ui::RpWidget(detail_scroll);
    detail->setObjectName("lingtai_agent_detail");
    detail->setAccessibleName(QStringLiteral("Selected Agent detail"));
    detail_scroll->setWidget(detail);
    auto *detail_layout = new QVBoxLayout(detail);
    detail_layout->setContentsMargins(0, 0, 0, 0);
    detail_layout->setSpacing(4);
    // The old "Selected Agent" header heading stays as a hidden semantic
    // anchor: the chat top bar below now owns the selected-Agent identity.
    auto *detail_heading = make_label(
        detail, QStringLiteral("Selected Agent"),
        "lingtai_agent_detail_heading", 14, QFont::DemiBold);
    detail_heading->hide();
    detail_layout->addWidget(detail_heading);

    // One Telegram-like chat top bar: selected Agent identity and presence on
    // the left, and the compact Start/Sleep controls plus the narrow-mode
    // Back control on the right.
    auto *top_bar = new QWidget(detail);
    top_bar->setObjectName("lingtai_chat_top_bar");
    top_bar->setAccessibleName(QStringLiteral("Selected Agent"));
    top_bar->setFixedHeight(54);
    auto *top_bar_layout = new QHBoxLayout(top_bar);
    top_bar_layout->setContentsMargins(12, 8, 12, 8);
    top_bar_layout->setSpacing(8);
    auto *selected_avatar = new SelectedAgentAvatar(top_bar);
    selected_avatar->setObjectName("lingtai_selected_agent_avatar");
    selected_avatar->hide();
    top_bar_layout->addWidget(selected_avatar);
    auto *identity_column = new QVBoxLayout;
    identity_column->setContentsMargins(0, 0, 0, 0);
    identity_column->setSpacing(2);
    auto *presentation_name = make_label(
        top_bar, QString(), "lingtai_selected_agent_presentation_name", 14,
        QFont::DemiBold);
    presentation_name->setAccessibleName(
        QStringLiteral("Selected Agent presentation name"));
    identity_column->addWidget(presentation_name);
    auto *detail_key = make_label(
        top_bar, QString(), "lingtai_selected_agent_key", 10);
    detail_key->setAccessibleName(
        QStringLiteral("Selected Agent status and role"));
    detail_key->setWordWrap(false);
    detail_key->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    // The status line stays visually subordinate to the title: a smaller
    // point size above and a distinct muted ink drawn from the same
    // secondary-text token the page nav and disabled actions already use,
    // never the prominent title ink.
    auto key_palette = detail_key->palette();
    key_palette.setColor(QPalette::WindowText, st::windowSubTextFg->c);
    detail_key->setPalette(key_palette);
    identity_column->addWidget(detail_key);
    top_bar_layout->addLayout(identity_column, 1);
    // One compact palette-owned Back control in the chat top bar, visible only
    // in Telegram's narrow OneColumn detail view; it returns to the roster
    // through the same narrow-mode path Telegram's history-back uses.
    detail_back_button_ = new PaletteActionButton(
        top_bar, QStringLiteral("Back"));
    detail_back_button_->setObjectName("lingtai_agent_detail_back");
    detail_back_button_->setAccessibleName(QStringLiteral("Back to Agent list"));
    QObject::connect(detail_back_button_, &QPushButton::clicked, [this] {
        handle_detail_back();
    });
    top_bar_layout->addWidget(detail_back_button_);

    // The one Step-6 action on the exact selected Agent: an explicit,
    // nonblocking start for a selected non-human Agent whose current
    // projection is not heartbeat-live. Hidden entirely (not merely
    // disabled) for a live Agent, matching the product contract's "no
    // Start action" rather than Request sleep's always-visible/disabled
    // shape. The status label below shows only truthful, evidence-backed
    // claims -- spawn acceptance is never "online" on its own.
    auto *start_row = new Ui::RpWidget(top_bar);
    start_row->setObjectName("lingtai_selected_agent_start_row");
    start_row->setAccessibleName(QStringLiteral("Start Agent"));
    auto *start_row_layout = new QVBoxLayout(start_row);
    start_row_layout->setContentsMargins(0, 0, 0, 0);
    start_row_layout->setSpacing(0);
    auto *start_button = new PaletteActionButton(
        start_row, QStringLiteral("Start Agent"));
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
    auto *start_status = make_label(
        start_row, QString(), "lingtai_selected_agent_start_status", 10);
    start_status->setAccessibleName(QStringLiteral("Start Agent status"));
    start_status->setMaximumWidth(160);
    start_status->setMaximumHeight(12);
    start_status->setWordWrap(false);
    start_row_layout->addWidget(start_status);
    // Reserve the action region's height from the row's own layout so the
    // chat surface below never jumps when the Start button is hidden for a
    // heartbeat-live Agent; visibility/enablement still track eligibility
    // exactly, only the button is ever absent.
    start_row->setMinimumHeight(start_row->sizeHint().height());
    start_button->setVisible(false);
    top_bar_layout->addWidget(start_row);

    // The one Step-5 action on the exact selected Agent: reproduces only the
    // canonical empty `.sleep` marker write plus a best-effort target-side
    // observation, as a subtle compact icon-only secondary -- never a second
    // full-caption action button -- whose accessible name preserves the
    // "Request sleep" identity. Disabled while ineligible or while a
    // just-clicked observation is still pending; the status label below shows
    // only truthful, evidence-backed claims, never a lifecycle status inferred
    // from the write or a timeout alone.
    auto *sleep_row = new Ui::RpWidget(top_bar);
    sleep_row->setObjectName("lingtai_selected_agent_sleep_row");
    sleep_row->setAccessibleName(QStringLiteral("Request sleep"));
    auto *sleep_row_layout = new QVBoxLayout(sleep_row);
    sleep_row_layout->setContentsMargins(0, 0, 0, 0);
    sleep_row_layout->setSpacing(0);
    auto *sleep_button = new PaletteIconButton(sleep_row);
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
    auto *sleep_status = make_label(
        sleep_row, QString(), "lingtai_selected_agent_sleep_status", 10);
    sleep_status->setAccessibleName(QStringLiteral("Sleep request status"));
    sleep_status->setMaximumWidth(160);
    sleep_status->setMaximumHeight(12);
    sleep_status->setWordWrap(false);
    sleep_row_layout->addWidget(sleep_status);
    top_bar_layout->addWidget(sleep_row);
    detail_layout->addWidget(top_bar);
    // Retained once for the whole shell lifetime so the responsive fit measure
    // in `recompute_layout` can evaluate the full natural row against the
    // actual detail width without re-deriving these two anchors.
    chat_top_bar_ = top_bar;
    selected_agent_key_ = detail_key;

    // One compact secondary page navigation: the chat is the default selected
    // Agent surface, and Presets owns one page so only one content surface
    // shows at a time.
    auto *pages_nav = new PaletteSurface(detail, st::windowBg);
    pages_nav->setObjectName("lingtai_agent_pages_nav");
    pages_nav->setAccessibleName(QStringLiteral("Selected Agent pages"));
    auto *pages_nav_layout = new QHBoxLayout(pages_nav);
    pages_nav_layout->setContentsMargins(0, 0, 0, 0);
    pages_nav_layout->setSpacing(4);
    const auto nav_specs = std::array<std::pair<const char *, const char *>, 2>{{
        std::pair{"lingtai_agent_page_nav_conversation", "Conversation"},
        std::pair{"lingtai_agent_page_nav_presets", "Presets"},
    }};
    for (auto index = std::size_t{0}; index != nav_specs.size(); ++index) {
        const auto &[object_name, text] = nav_specs[index];
        auto *button = new PageNavButton(
            pages_nav, QString::fromUtf8(text));
        button->setObjectName(object_name);
        button->setAccessibleName(QString::fromUtf8(text));
        const auto page = static_cast<AgentDetailPage>(index);
        QObject::connect(button, &QPushButton::clicked, [this, page] {
            show_detail_page(page);
        });
        // Content-driven leading navigation: the two buttons keep their own
        // size, and the one trailing stretch absorbs the remaining width so
        // the nav is never two equal full-pane slabs.
        pages_nav_layout->addWidget(button, 0);
        page_nav_buttons_.push_back(button);
    }
    // The flexible trailing room that makes the page nav content-driven.
    pages_nav_layout->addStretch(1);
    detail_layout->addWidget(pages_nav);

    // The conversation is the product, so it is the default page surface,
    // directly under the page navigation rather than below a stack of source
    // cards. Its duplicate "Conversation" heading is retained only as a hidden
    // object/implementation anchor: the nav item owns the user affordance.
    detail_layout->addWidget(make_label(
        detail, QStringLiteral("Conversation"),
        "lingtai_selected_agent_conversation_heading", 11, QFont::DemiBold));
    auto *conversation = new ConversationSurface(detail);
    conversation->setObjectName("lingtai_selected_agent_conversation");
    conversation->setAccessibleName(
        QStringLiteral("Selected Agent conversation"));
    conversation->setAccessibleDescription(QStringLiteral(
        "The current direct conversation with the selected Agent, shown "
        "read-only as plain text."));
    conversation->setMinimumHeight(180);
    conversation->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // The surface's own paintEvent owns the visible backdrop and bubbles; its
    // Base/Window palette roles stay transparent so the surface renders the
    // shell's palette background rather than a widget-level white base.
    detail_layout->addWidget(conversation, 1);

    // The one floating composer surface, directly under the conversation it
    // sends into: the surface's base `windowBg` content token sits on the same
    // base main surface as the content pane -- never a full-width dark band --
    // and it stays physically inset from both detail edges by a wrapper lane,
    // while its own layout owns the compact attachment/input/Send action row
    // and both status read-outs.
    auto *composer = new PaletteSurface(detail, st::windowBg);
    composer_ = composer;
    composer->setObjectName("lingtai_composer");
    composer->setAccessibleName(QStringLiteral("Send a message"));
    auto *composer_layout = new QVBoxLayout(composer);
    composer_layout->setContentsMargins(12, 10, 12, 8);
    composer_layout->setSpacing(4);
    // The one compact aligned action row: one attachment icon, one vendored
    // single-line input, and one explicit icon Send action, all owned by the
    // composer lane and the input/Send both submitting through the same path.
    auto *composer_action_row = new QHBoxLayout;
    composer_action_row->setSpacing(8);
    // A borderless copy of the shared single-line field style: the row's own
    // base `windowBg` surface is the only frame, and the field keeps the same
    // text/placeholder face as the standard control.
    static const auto borderless_composer_input = [] {
        auto result = st::defaultInputField;
        result.border = 0;
        result.borderActive = 0;
        return result;
    }();
    auto *composer_input = new Ui::InputField(
        composer,
        borderless_composer_input,
        Ui::InputField::Mode::SingleLine,
        rpl::single(QStringLiteral("Message…")));
    composer_input->setObjectName("lingtai_composer_input");
    composer_input->setAccessibleName(QStringLiteral("Message"));
    composer_input->setMinHeight(36);
    composer_input->setEnabled(false);
    auto *attachment_button = new QPushButton(QStringLiteral("+"), composer);
    attachment_button->setObjectName("lingtai_composer_attachment_button");
    attachment_button->setAccessibleName(QStringLiteral("Attach file"));
    attachment_button->setEnabled(false);
    attachment_button->setFixedWidth(36);
    attachment_button->setFixedHeight(36);
    composer_action_row->addWidget(attachment_button);
    composer_action_row->addWidget(composer_input, 1);
    auto *send_button = new Ui::RoundButton(
        composer,
        rpl::single(QStringLiteral("↑")),
        st::defaultActiveButton);
    send_button->setObjectName("lingtai_composer_send_button");
    send_button->setAccessibleName(QStringLiteral("Send message"));
    send_button->setEnabled(false);
    send_button->setFixedWidth(40);
    send_button->addClickHandler([this] {
        handle_send_message();
    });
    composer_action_row->addWidget(send_button);
    composer_layout->addLayout(composer_action_row);
    // The send status is owned by the lane itself, immediately below the
    // action row -- never a separate detail row.
    auto *composer_status = make_label(
        composer, QString(), "lingtai_composer_status", 10);
    composer_status->setAccessibleName(QStringLiteral("Send status"));
    composer_layout->addWidget(composer_status);
    // The conversation's own compact read-out shares the same lane, directly
    // under the send status: it never becomes a separate detail row.
    auto *conversation_state = make_label(
        composer, QString(), "lingtai_selected_agent_conversation_state", 10);
    conversation_state->setAccessibleName(
        QStringLiteral("Selected Agent conversation state"));
    composer_layout->addWidget(conversation_state);
    auto *composer_surface = new QHBoxLayout;
    composer_surface->setContentsMargins(24, 0, 24, 0);
    composer_surface->addWidget(composer);
    detail_layout->addLayout(composer_surface);
    composer_input->submits()
        | rpl::on_next([this] {
            handle_send_message();
        }, submits_lifetime_);

    // The one retained bounded read-only selected-Agent source section is
    // presented through the same local structural framing: one semibold
    // heading, one read-only plain-text surface, one state line, and one thin
    // plain-shadow separator. It remains a distinct source and authority and
    // is never merged with the mailbox conversation; it moves behind the
    // compact secondary page host so it never stacks under the chat surface.
    auto *pages_host = new Ui::RpWidget(detail);
    pages_host->setObjectName("lingtai_agent_pages_host");
    pages_host->setAccessibleName(
        QStringLiteral("Selected Agent secondary pages"));
    auto *pages_host_layout = new QVBoxLayout(pages_host);
    pages_host_layout->setContentsMargins(0, 0, 0, 0);
    pages_host_layout->setSpacing(8);
    secondary_pages_.push_back(add_dashboard_section(
        pages_host, pages_host_layout, "preset_summary",
        QStringLiteral("Presets"),
        QStringLiteral("Selected Agent Presets summary"),
        QStringLiteral("The selected Agent's own kernel-resolved preset policy "
            "and active effective configuration, shown read-only as plain "
            "text.")).owner);
    pages_host->hide();
    detail_layout->addWidget(pages_host);

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

    // The source-facts labels below the page host are read-only detail
    // surfaces, not chat content: they stay present as anchors but are never
    // revealed on the Presets page, so the chat-first surface stays clean.
    for (const auto *facts_name : {
            "lingtai_selected_agent_manifest_identity",
            "lingtai_selected_agent_manifest_llm",
            "lingtai_selected_agent_manifest_capabilities",
            "lingtai_selected_agent_status_activity",
            "lingtai_selected_agent_status_context",
            "lingtai_selected_agent_facts" }) {
        if (auto *label = window_->findChild<QLabel *>(facts_name)) {
            label->hide();
        }
    }

    // The chat is the default selected-Agent page; the page navigation and
    // secondary surfaces start in that exact state.
    show_detail_page(AgentDetailPage::conversation);

    // One simple view-scoped timer: it re-invokes the same stateless
    // snapshot reader every second so a same-selection append becomes
    // visible without reselection. It is behavior, not a watcher subsystem:
    // no background thread, debouncing, or persisted state.
    activity_timer_ = new QTimer(body);
    activity_timer_->setInterval(1000);
    QObject::connect(activity_timer_, &QTimer::timeout, [this] {
        render_conversation();
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
    auto *dialog_note = make_flat_label(
        bootstrap_dialog_,
        QStringLiteral(
            "Creates a new LingTai project, names its first Agent from the "
            "destination folder by default, and starts it."),
        "lingtai_bootstrap_dialog_note");
    dialog_note->setAccessibleName(QStringLiteral(
        "New project note"));
    dialog_layout->addWidget(dialog_note);

    auto *destination_row = new QHBoxLayout;
    destination_row->setSpacing(8);
    auto *destination_input = new Ui::InputField(
        bootstrap_dialog_,
        st::defaultInputField,
        Ui::InputField::Mode::SingleLine,
        rpl::single(QStringLiteral("Destination folder")));
    destination_input->setObjectName("lingtai_bootstrap_destination_input");
    destination_input->setAccessibleName(QStringLiteral("Destination folder"));
    destination_row->addWidget(destination_input, 1);
    auto *browse_button = new Ui::RoundButton(
        bootstrap_dialog_,
        rpl::single(QStringLiteral("Browse…")),
        st::defaultLightButton);
    browse_button->setObjectName("lingtai_bootstrap_destination_browse");
    browse_button->setAccessibleName(
        QStringLiteral("Browse destination folder"));
    browse_button->addClickHandler([this] {
        handle_browse_destination();
    });
    destination_row->addWidget(browse_button);
    dialog_layout->addLayout(destination_row);

    auto *preset_label = make_flat_label(
        bootstrap_dialog_,
        QStringLiteral("Preset"),
        "lingtai_bootstrap_preset_label");
    dialog_layout->addWidget(preset_label);
    auto *preset_chooser = new QComboBox(bootstrap_dialog_);
    preset_chooser->setObjectName("lingtai_bootstrap_preset_chooser");
    preset_chooser->setAccessibleName(QStringLiteral("Preset"));
    preset_chooser->setAccessibleDescription(QStringLiteral(
        "Choose the preset the new project's first Agent is created from."));
    dialog_layout->addWidget(preset_chooser);

    auto *dialog_status = make_flat_label(
        bootstrap_dialog_,
        QString(),
        "lingtai_bootstrap_dialog_status");
    dialog_status->setAccessibleName(QStringLiteral("New project dialog status"));
    dialog_layout->addWidget(dialog_status);

    // One thin plain-shadow divider gives the form/actions one visible stable
    // separation, the same structural cue the dashboard sections use.
    auto *dialog_divider = new Ui::PlainShadow(bootstrap_dialog_);
    dialog_divider->setObjectName("lingtai_bootstrap_dialog_divider");
    dialog_divider->setAccessibleName(QStringLiteral("New project form divider"));
    dialog_divider->setFixedHeight(st::lineWidth);
    dialog_layout->addWidget(dialog_divider);

    auto *dialog_actions = new QHBoxLayout;
    dialog_actions->setSpacing(8);
    auto *cancel_button = new Ui::RoundButton(
        bootstrap_dialog_,
        rpl::single(QStringLiteral("Cancel")),
        st::defaultLightButton);
    cancel_button->setObjectName("lingtai_bootstrap_cancel");
    cancel_button->setAccessibleName(QStringLiteral("Cancel"));
    cancel_button->addClickHandler([this] {
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
    auto *create_button = new Ui::RoundButton(
        bootstrap_dialog_,
        rpl::single(QStringLiteral("Create & Start")),
        st::defaultActiveButton);
    create_button->setObjectName("lingtai_bootstrap_create_start");
    create_button->setAccessibleName(QStringLiteral("Create and Start"));
    create_button->setAccessibleDescription(QStringLiteral(
        "Creates the new project, names its first Agent from the destination "
        "folder, and starts that Agent."));
    create_button->addClickHandler([this] {
        handle_create_and_start();
    });
    dialog_actions->addStretch();
    dialog_actions->addWidget(cancel_button);
    dialog_actions->addWidget(create_button);
    dialog_layout->addLayout(dialog_actions);
    // The QPushButton default-enter path does not survive the switch to
    // RoundButton, so the destination field's own Return submission keeps the
    // current useful default activation wired to the same Create & Start
    // handler. Wider keyboard navigation stays Commit30's work.
    destination_input->submits()
        | rpl::on_next([this] {
            handle_create_and_start();
        }, submits_lifetime_);
    bootstrap_dialog_->hide();

    // The one Telegram-derived mode recompute: Telegram's
    // `SessionController` re-derives OneColumn vs Normal on every chats
    // resize, so the body's own lifetime-owned size stream drives the same
    // single local recompute.
    window_->body()->sizeValue()
        | rpl::on_next([this](QSize size) {
            recompute_layout(size.width());
        }, layout_lifetime_);

    QObject::connect(
        QGuiApplication::styleHints(),
        &QStyleHints::colorSchemeChanged,
        window_.get(),
        [this] { refresh_system_palette(); });
    base::install_event_filter(
        window_.get(),
        qGuiApp,
        [this](not_null<QEvent *> event) {
            if (event->type() == QEvent::ApplicationPaletteChange) {
                refresh_system_palette();
            }
            return base::EventFilterResult::Continue;
        });

    refresh_route();
    render_roster();
    recompute_layout(window_->body()->width());
}

NativeShell::~NativeShell() = default;

void NativeShell::refresh_system_palette() {
    apply_system_palette();
    render_conversation();
    window_->update();
    for (auto *widget : window_->findChildren<QWidget *>()) {
        widget->update();
    }
}

void NativeShell::show() {
    refresh_route();
    window_->show();
    recompute_layout(window_->body()->width());
}

void NativeShell::show_offscreen() {
    refresh_route();
    window_->setAttribute(Qt::WA_DontShowOnScreen, true);
    window_->show();
    recompute_layout(window_->body()->width());
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
    auto *status = find_ui_child<Ui::FlatLabel>(
        *window_, "lingtai_bootstrap_status");
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
    if (auto *status = find_ui_child<Ui::FlatLabel>(
            *window_, "lingtai_bootstrap_dialog_status")) {
        status->setText(QString());
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
    if (auto *input = find_ui_child<Ui::InputField>(
            *window_, "lingtai_bootstrap_destination_input")) {
        input->setText(selected);
    }
}

void NativeShell::handle_create_and_start() {
    if (!bootstrap_dialog_ || !bootstrap_dialog_->isVisible()
        || bootstrap_runner_->is_pending()) {
        return;
    }
    auto *input = find_ui_child<Ui::InputField>(
        *window_, "lingtai_bootstrap_destination_input");
    auto *chooser = window_->findChild<QComboBox *>(
        "lingtai_bootstrap_preset_chooser");
    auto *dialog_status = find_ui_child<Ui::FlatLabel>(
        *window_, "lingtai_bootstrap_dialog_status");
    if (!input || !chooser || !dialog_status) return;
    const auto destination = input->getLastText().trimmed();
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
    dialog_status->setText(QString());
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
    bump_lifecycle_generation();
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
    find_ui_child<Ui::FlatLabel>(*window_, "lingtai_project_open_error")
        ->setText(QString());
    open_error_surface_->hide();
    reset_composer();
    // A fresh open must never let a prior target's pending sleep or Start
    // observation surface under the newly opened project/selection.
    pending_sleep_observation_.reset();
    pending_start_observation_.reset();
    refresh_route();
    show_detail_page(AgentDetailPage::conversation);
    recompute_layout(window_->body()->width());
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
    auto *selected_avatar = static_cast<SelectedAgentAvatar *>(
        window_->findChild<QWidget *>("lingtai_selected_agent_avatar"));
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
    if (!selected_key || !presentation_name || !selected_avatar
        || !manifest_identity
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
        presentation_name->setProperty("lingtai_full_text", QString());
        presentation_name->setAccessibleDescription(QString());
        selected_avatar->set_agent_name(QString());
        selected_avatar->hide();
        manifest_identity->clear();
        manifest_llm->clear();
        manifest_capabilities->clear();
        status_activity->clear();
        status_context->clear();
        selected_facts->setText(QStringLiteral(
            "Choose a valid manifest row to inspect its detail."));
        render_conversation();
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
    // The full presentation title is retained on the label itself (a dynamic
    // property and the accessible description) so the responsive top-bar fit
    // can elide only the visible text without ever losing the identity.
    presentation_name->setProperty("lingtai_full_text", title);
    presentation_name->setAccessibleDescription(title);
    selected_avatar->set_agent_name(title);
    selected_avatar->show();
    const auto friendly_role = friendly_agent_role_text(detail_item->role);
    const auto friendly_presence = friendly_agent_presence_text(
        detail_item->presence);
    const auto role = friendly_role.isEmpty()
        ? role_text(detail_item->role)
        : friendly_role;
    const auto presence = friendly_presence.isEmpty()
        ? presence_text(detail_item->presence)
        : friendly_presence;
    selected_key->setText(
        QStringLiteral("%1 · %2").arg(presence, role));
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
    // Sender identity always comes from the stored full title, never the
    // possibly elided visible text the responsive top-bar fit may have set.
    const auto full_title = presentation_name
        ? presentation_name->property("lingtai_full_text").toString()
        : QString();
    const auto them = !full_title.isEmpty()
        ? full_title
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

// Shows the selected Agent's own kernel-published resolved preset policy:
// only the minimal Provider, Model, Default and ordered Allowed refs from
// `system/manifest.resolved.json`. It is a distinct source and surface from
// the mailbox conversation above, refreshed on the same explicit
// open/selection paths plus the one-second timer. Every observation is shown
// exactly as read, so an absent/stale/unavailable current observation never
// keeps a prior target's projection visible.
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
    lines << QStringLiteral("Provider: %1")
        .arg(value_text(summary.effective.provider));
    lines << QStringLiteral("Model: %1").arg(value_text(summary.effective.model));
    lines << QStringLiteral("Default: %1").arg(value_text(summary.default_ref));
    lines << QStringLiteral("Allowed:");
    for (const auto &ref : summary.allowed) {
        lines << QStringLiteral("  • %1").arg(QString::fromStdString(ref.ref));
    }

    show(lines.join(QStringLiteral("\n")),
        summary.source == AgentPresetSummarySource::stale
            ? QStringLiteral("Stale") : QStringLiteral("Resolved"));
}

// Classifies the raw composer text before ordinary trim/send handling. Every
// parsed slash command is cleared and dispatched locally here, so even an
// unknown or deliberately unavailable command can never reach DirectPublisher.
// Ordinary messages still resolve the route fresh from current C1/C3 truth
// rather than capturing it once, so a selection change between typing and
// clicking Send can never deliver to a stale target.
void NativeShell::handle_send_message() {
    auto *input = static_cast<Ui::InputField *>(
        window_->findChild<QObject *>("lingtai_composer_input"));
    auto *status = window_->findChild<QLabel *>("lingtai_composer_status");
    if (!input || !status) return;
    const auto raw_text = input->getLastText();
    if (const auto command = parse_slash_command(raw_text.toStdString())) {
        input->clear();
        if (command->name == "suspend" || command->name == "clear"
            || command->name == "refresh") {
            handle_lifecycle_command(command->name, command->args);
            return;
        }
        if (!command->args.empty()) {
            status->setText(QStringLiteral(
                "Command not available in this Desktop build."));
            return;
        }
        if (command->name == "presets") {
            status->clear();
            show_detail_page(AgentDetailPage::presets);
            return;
        }
        if (command->name == "agents") {
            status->clear();
            if (detail_back_button_ && detail_back_button_->isVisible()) {
                handle_detail_back();
            } else {
                agent_roster_->focus_row(
                    selection_state_.selected_agent_directory_key());
            }
            return;
        }
        if (command->name == "sleep") {
            status->clear();
            handle_request_sleep();
            return;
        }
        if (command->name == "cpr") {
            status->clear();
            handle_start_agent();
            return;
        }
        if (command->name == "help") {
            status->setText(QStringLiteral(
                "Available commands: /agents, /presets, /sleep, /cpr, /clear, "
                "/refresh, /suspend, /help, /quit."));
            return;
        }
        if (command->name == "quit") {
            status->clear();
            window_->hide();
            QCoreApplication::quit();
            return;
        }
        status->setText(QStringLiteral(
            "Command not available in this Desktop build."));
        return;
    }

    const auto text = raw_text.trimmed();
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
    bump_lifecycle_generation();
    // A selection change must never let a prior target's pending sleep or
    // Start observation or terminal result surface under the newly selected
    // Agent.
    pending_sleep_observation_.reset();
    pending_start_observation_.reset();
    render_roster();
    show_detail_page(AgentDetailPage::conversation);
    recompute_layout(window_->body()->width());
    // Telegram's `HistoryWidget::setInnerFocus()`: a selected Agent focuses
    // the visible, enabled composer.
    if (auto *composer = static_cast<Ui::InputField *>(
            window_->findChild<QObject *>("lingtai_composer_input"))) {
        if (composer->isVisible() && composer->isEnabled()) {
            composer->setFocus();
        }
    }
    if (error) {
        error->clear();
        error->hide();
    }
}

// The one selected-Agent lifecycle owner for `/suspend`, `/clear`, and
// `/refresh [preset]`. Only the empty forms and a zero-or-one raw preset
// argument are accepted; extra or invalid arguments are rejected locally
// without launching. Dispatch always rides the already-injected
// `tui_executable_` with the exact separate argv through the one owned
// AgentCommandRunner; a duplicate lifecycle slash while pending is rejected
// with the exact truthful status, and ordinary chat send is never disabled.
void NativeShell::handle_lifecycle_command(
        const std::string &name, const std::string &args) {
    auto *status = window_->findChild<QLabel *>("lingtai_composer_status");
    if (!status) return;
    if (!selection_state_.active_project()
        || !selection_state_.selected_agent_directory_key()) {
        status->setText(QStringLiteral(
            "Command not available in this Desktop build."));
        return;
    }
    std::string optional_arg;
    const auto single_preset = !args.empty()
        && args.find(' ') == std::string::npos;
    const auto valid = ((name == "suspend" || name == "clear") && args.empty())
        || (name == "refresh" && (args.empty() || single_preset));
    if (!valid) {
        status->setText(QStringLiteral(
            "Command not available in this Desktop build."));
        return;
    }
    if (single_preset) {
        optional_arg = args;
    }
    const auto project_root =
        path_text(selection_state_.active_project()->root() / ".lingtai")
            .toStdString();
    const auto agent_key =
        selection_state_.selected_agent_directory_key()->string();
    if (!command_runner_.run(tui_executable_, project_root, agent_key, name,
            optional_arg, lifecycle_generation(),
            [this](AgentCommandResult result) {
                handle_lifecycle_finished(std::move(result));
            })) {
        status->setText(QStringLiteral("Agent command already pending."));
        return;
    }
    pending_lifecycle_action_ = name;
    status->setText(QStringLiteral("Agent command pending."));
}

// The one terminal lifecycle delivery. A completion may update the existing
// conversation status only when the generation, canonical project root, and
// selected Agent key captured at dispatch still match the current selection
// context, so an old completion -- including an away-and-back return to the
// same key -- can never surface under a later selection.
void NativeShell::handle_lifecycle_finished(AgentCommandResult result) {
    auto *status = window_->findChild<QLabel *>("lingtai_composer_status");
    if (!status) return;
    const auto matching_context = selection_state_.active_project()
        && selection_state_.selected_agent_directory_key()
        && result.bound_generation == lifecycle_generation()
        && result.bound_project_root
            == path_text(selection_state_.active_project()->root()
                / ".lingtai").toStdString()
        && result.bound_agent_key
            == selection_state_.selected_agent_directory_key()->string();
    if (!matching_context || pending_lifecycle_action_.empty()) {
        pending_lifecycle_action_.clear();
        return;
    }
    auto signaled = QString::fromStdString(pending_lifecycle_action_);
    signaled[0] = signaled[0].toUpper();
    status->setText(result.kind == AgentCommandResultKind::succeeded
        ? signaled + QStringLiteral(" signaled.")
        : QStringLiteral("Agent command failed."));
    pending_lifecycle_action_.clear();
}

std::string NativeShell::lifecycle_generation() const noexcept {
    return std::to_string(selection_generation_);
}

void NativeShell::bump_lifecycle_generation() noexcept {
    ++selection_generation_;
}

// Telegram's one mode recompute, fed by the body's own size stream: below
// the source-backed two-surface threshold (`260 + 380` usable column pixels
// after the one-pixel separator and the 8px drag handle) exactly one
// full-width surface is shown -- the roster until an Agent is selected, then
// the detail with Back; at or above it roster + handle + separator + detail
// all show and Back is hidden. A selected Agent is the sole state that
// decides which narrow surface is active, so a wide->narrow resize with an
// active selection keeps the detail, exactly as Telegram keeps the active
// chat in OneColumn.
void NativeShell::recompute_layout(int body_width) {
    const auto available = body_width - kRosterResizeHandleWidth
        - kRosterSeparatorWidth;
    if (available >= kTwoColumnAvailableThreshold) {
        auto roster_width = qRound(body_width * roster_width_ratio_);
        const auto roster_minimum =
            (roster_width_ratio_ < kWideRosterWidthRatio)
            ? kCollapsedRosterColumnWidth
            : kRosterColumnWidth;
        roster_width = std::clamp(roster_width,
            roster_minimum,
            body_width - kDetailColumnMinimumWidth
                - kRosterResizeHandleWidth - kRosterSeparatorWidth);
        agent_roster_->setVisible(true);
        agent_roster_->set_roster_width(roster_width);
        roster_resize_handle_->setVisible(true);
        separator_->setVisible(true);
        content_->setVisible(true);
        detail_back_button_->setVisible(false);
        const auto detail_width = body_width - roster_width
            - kRosterResizeHandleWidth - kRosterSeparatorWidth;
        update_composer_width(detail_width);
        update_top_bar_fit(detail_width);
        return;
    }
    const auto detail_active =
        selection_state_.selected_agent_directory_key().has_value();
    agent_roster_->setVisible(!detail_active);
    agent_roster_->set_roster_width(detail_active
        ? kRosterColumnWidth
        : std::max(body_width, kRosterColumnWidth));
    if (roster_resize_handle_) roster_resize_handle_->setVisible(false);
    separator_->setVisible(false);
    content_->setVisible(detail_active);
    detail_back_button_->setVisible(detail_active);
    update_composer_width(body_width);
    update_top_bar_fit(body_width);
}

// The one responsive chat-top-bar measure, re-entered on every recompute (and
// so on every real resize and selection change): the actual detail/header
// width is exactly what `recompute_layout` just derived -- in Normal mode the
// body minus the actual chosen roster width, 8px drag handle, and 1px
// separator; in OneColumn detail the full body width. The identity name keeps
// its full title stored on the presentation-name label (a `lingtai_full_text`
// dynamic property and the accessible description), so it can elide for the
// current width while the full identity never leaves accessibility. The
// complete row is first measured with the name unbounded and every secondary
// element visible; if it does not fit, the secondary key hides first, then
// both action status labels, while the one primary action, the icon-only
// Request sleep secondary, and Back stay reachable. The remaining width is
// then allocated to the name -- the actual detail width minus the row's
// natural non-name width, clamped to at least one pixel -- and its visible
// text becomes the right-elided full title. No timer/event framework or
// persisted state; primary controls, the icon-only secondary, fonts, and
// object names are never touched.
void NativeShell::update_top_bar_fit(int detail_width) {
    if (!chat_top_bar_ || !selected_agent_key_) return;
    auto *presentation_name = chat_top_bar_->findChild<QLabel *>(
        "lingtai_selected_agent_presentation_name");
    auto *start_status = chat_top_bar_->findChild<QLabel *>(
        "lingtai_selected_agent_start_status");
    auto *sleep_status = chat_top_bar_->findChild<QLabel *>(
        "lingtai_selected_agent_sleep_status");
    if (!presentation_name || !start_status || !sleep_status) return;
    const auto full = presentation_name->property(
        "lingtai_full_text").toString();
    if (full.isEmpty()) return;
    // Restore the full natural row: the name is unbounded with its full
    // title restored, and the secondary key plus both action captions show.
    presentation_name->setMaximumWidth(QWIDGETSIZE_MAX);
    presentation_name->setText(full);
    selected_agent_key_->setVisible(true);
    start_status->setVisible(true);
    sleep_status->setVisible(true);
    // Every nonempty Start/Sleep status must fit its own bounded label width
    // before any primary identity space is consumed; one that cannot is
    // hidden so a long read-out never clips inside its action row.
    const auto status_self_fits = [](QLabel *status) {
        if (status->text().isEmpty()) return true;
        return QFontMetrics(status->font()).horizontalAdvance(status->text())
            <= status->maximumWidth();
    };
    if (!status_self_fits(start_status)) start_status->setVisible(false);
    if (!status_self_fits(sleep_status)) sleep_status->setVisible(false);
    // Priority cascade: the secondary key hides first, then both action
    // captions, so the Start/Sleep rows, pills, and Back stay reachable.
    if (chat_top_bar_->sizeHint().width() > detail_width) {
        selected_agent_key_->setVisible(false);
        if (chat_top_bar_->sizeHint().width() > detail_width) {
            start_status->setVisible(false);
            sleep_status->setVisible(false);
        }
    }
    // Measure the non-name row with the visible name text blanked (and its
    // width clamped to zero), so the full presentation title never double
    // counts into the row's size hint and collapses the derived allocation.
    presentation_name->setMinimumWidth(0);
    presentation_name->setMaximumWidth(0);
    presentation_name->setText(QString());
    // Measure every visible top-level widget. The vertically stacked
    // identity layout has no widget and is skipped automatically; the avatar
    // and actions all count against the title allocation.
    auto *top_layout = chat_top_bar_->layout();
    const auto margins = top_layout->contentsMargins();
    auto non_name_width = margins.left() + margins.right();
    auto visible_non_identity_items = 0;
    for (auto i = 0; i != top_layout->count(); ++i) {
        auto *item = top_layout->itemAt(i);
        auto *widget = item ? item->widget() : nullptr;
        if (!widget || !widget->isVisible()) continue;
        non_name_width += item->sizeHint().width();
        ++visible_non_identity_items;
    }
    non_name_width += top_layout->spacing() * visible_non_identity_items;
    // Allocate every remaining pixel to the identity name: its maximum width
    // is the actual detail width minus the row's natural non-name width, so
    // the name never hides or overlaps; the visible text is the right-elided
    // full title and the full identity stays on the property/description.
    const auto available = std::max(1, detail_width - non_name_width);
    presentation_name->setMinimumWidth(available);
    presentation_name->setMaximumWidth(available);
    presentation_name->setText(QFontMetrics(presentation_name->font())
        .elidedText(full, Qt::ElideRight, available));
}

// The one full-width composer row, re-entered on every recompute (and so on
// every real resize and selection change): the row always stretches the full
// detail width, while its own layout keeps one centered adaptive lane capped
// at 900px -- outer horizontal margins of at least 12px, growing to split any
// width beyond 900px -- with the input/Send row and the two status lines
// staying in that same lane. Object names, the input/Send row, and the status
// wording are never touched.
void NativeShell::update_composer_width(int detail_width) {
    if (!composer_) return;
    const auto outer = std::max(12, (detail_width - 900) / 2 + 12);
    composer_->setMinimumWidth(0);
    composer_->setMaximumWidth(QWIDGETSIZE_MAX);
    composer_->layout()->setContentsMargins(outer, 10, outer, 8);
}

// Telegram's OneColumn history-back path: the narrow detail returns to the
// roster, drops the selection, and hands keyboard focus to a usable roster
// row. Guarded by Back's own visibility, so the wide two-column layout
// (where Back is hidden) can never be deselected through this path.
void NativeShell::handle_detail_back() {
    if (!detail_back_button_ || !detail_back_button_->isVisible()) return;
    selection_state_.clear_agent_selection();
    bump_lifecycle_generation();
    reset_composer();
    pending_sleep_observation_.reset();
    pending_start_observation_.reset();
    render_roster();
    show_detail_page(AgentDetailPage::conversation);
    recompute_layout(window_->body()->width());
    agent_roster_->focus_row(std::nullopt);
}

// Telegram's chat-first page switch: the conversation is the default
// selected-Agent surface, and exactly one of Conversation / Presets shows
// when its page is selected, so only one content surface dominates at a time.
// The secondary pages and the read-only source-facts labels are direct layout
// children (their object/accessibility anchors never move); switching only
// flips the page visibility, and the source-facts labels stay hidden.
void NativeShell::show_detail_page(AgentDetailPage page) {
    auto *conversation_heading = window_->findChild<QLabel *>(
        "lingtai_selected_agent_conversation_heading");
    auto *conversation = window_->findChild<QTextEdit *>(
        "lingtai_selected_agent_conversation");
    auto *composer = window_->findChild<Ui::RpWidget *>("lingtai_composer");
    auto *composer_status = window_->findChild<QLabel *>(
        "lingtai_composer_status");
    auto *conversation_state = window_->findChild<QLabel *>(
        "lingtai_selected_agent_conversation_state");
    auto *pages_host = window_->findChild<Ui::RpWidget *>(
        "lingtai_agent_pages_host");
    if (!conversation_heading || !conversation || !composer || !composer_status
        || !conversation_state || !pages_host) {
        return;
    }
    const auto on_conversation = page == AgentDetailPage::conversation;
    // The one visible Conversation affordance is the nav item; the duplicate
    // heading is retained only as a hidden object/implementation anchor and is
    // never revealed beside the selected nav control.
    conversation_heading->setVisible(false);
    conversation->setVisible(on_conversation);
    composer->setVisible(on_conversation);
    composer_status->setVisible(on_conversation);
    conversation_state->setVisible(on_conversation);
    pages_host->setVisible(!on_conversation);
    for (auto index = std::size_t{0}; index != secondary_pages_.size();
            ++index) {
        secondary_pages_[index]->setVisible(
            page == static_cast<AgentDetailPage>(index + 1));
    }
    for (auto index = std::size_t{0}; index != page_nav_buttons_.size();
            ++index) {
        page_nav_buttons_[index]->setChecked(
            page == static_cast<AgentDetailPage>(index));
    }
}
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
        if (item && item->presence == AgentPresenceKind::alive) {
            status->setText(QStringLiteral("Agent is already online."));
        }
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
    auto *label = find_ui_child<Ui::FlatLabel>(
        *window_, "lingtai_project_open_error");
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
    const auto project_active = selection_state_.active_project().has_value();
    empty_route_->setVisible(!project_active);
    project_route_->setVisible(project_active);
    // The welcome branding only belongs to the no-project state: with a
    // project open the right pane is one chat-first surface.
    if (auto *title = window_->findChild<QLabel *>("lingtai_product_title")) {
        title->setVisible(!project_active);
    }
    if (auto *purpose = window_->findChild<QLabel *>(
            "lingtai_product_purpose")) {
        purpose->setVisible(!project_active);
    }
}

} // namespace lingtai::desktop
