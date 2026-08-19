#include "kanban_page.h"

#include "base/basic_types.h"
#include "setup_style.h"
#include "styles/palette.h"

#include <QtCore/QDateTime>
#include <QtCore/QEvent>
#include <QtCore/QTimeZone>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>
#include <QtGui/QKeyEvent>
#include <QtGui/QPaintEvent>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>
#include <QtGui/QShowEvent>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>

namespace lingtai::desktop {
namespace {

// Overlay scrollbars paint on the viewport. Keep facts (especially the
// right-aligned network values) clear of the thumb instead of using the
// page margin, which sits on the wrong side of the bar.
constexpr auto kKanbanScrollGutter = 36;

void apply_scroll_gutter(QLayout *layout) {
    if (!layout) return;
    const auto margins = layout->contentsMargins();
    layout->setContentsMargins(
        margins.left(), margins.top(), kKanbanScrollGutter, margins.bottom());
}

QString comma_number(std::int64_t value) {
    auto text = QString::number(value);
    auto insert_at = text.size() - 3;
    while (insert_at > 0 && text[0] != QLatin1Char('-')) {
        text.insert(insert_at, QLatin1Char(','));
        insert_at -= 3;
    }
    return text;
}

QString compact_number(std::int64_t value) {
    const auto abs = value < 0 ? -value : value;
    if (abs >= 1'000'000'000) {
        return QString::number(static_cast<double>(value) / 1'000'000'000.0, 'f', 2)
            + QStringLiteral("B");
    }
    if (abs >= 1'000'000) {
        return QString::number(static_cast<double>(value) / 1'000'000.0, 'f', 2)
            + QStringLiteral("M");
    }
    return comma_number(value);
}

QString path_text(const std::filesystem::path &path) {
    const auto bytes = path.u8string();
    return QString::fromUtf8(
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<qsizetype>(bytes.size()));
}

QString field_value(
        const std::vector<KanbanField> &fields, std::string_view key) {
    for (const auto &field : fields) {
        if (field.label == key) return QString::fromStdString(field.value);
    }
    return {};
}

QLabel *make_label(
        QWidget *parent,
        const QString &text,
        const char *object_name,
        int point_size,
        QFont::Weight weight = QFont::Normal) {
    auto *label = new QLabel(text, parent);
    label->setObjectName(object_name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    auto font = label->font();
    font.setPointSize(point_size);
    font.setWeight(weight);
    label->setFont(font);
    label->setMinimumWidth(0);
    return label;
}

void color_text(QLabel *label, const QColor &color) {
    if (!label) return;
    auto palette = label->palette();
    palette.setColor(QPalette::WindowText, color);
    palette.setColor(QPalette::Text, color);
    label->setPalette(palette);
    label->setStyleSheet(QStringLiteral("color: %1;").arg(setup_color_css(color)));
}

QString pretty_started(const QString &raw) {
    auto parsed = QDateTime::fromString(raw, Qt::ISODate);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(raw, Qt::ISODateWithMs);
    }
    if (!parsed.isValid()) return raw;
    return QStringLiteral("Started %1").arg(
        parsed.toLocalTime().toString(QStringLiteral("MMM d, yyyy 'at' h:mm")));
}

QString uptime_from_started(const QString &raw) {
    auto parsed = QDateTime::fromString(raw, Qt::ISODate);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(raw, Qt::ISODateWithMs);
    }
    if (!parsed.isValid()) return {};
    const auto seconds = parsed.secsTo(QDateTime::currentDateTimeUtc());
    if (seconds < 0) return {};
    const auto days = seconds / 86400;
    const auto hours = (seconds % 86400) / 3600;
    const auto minutes = (seconds % 3600) / 60;
    if (days > 0) {
        return QStringLiteral("%1d %2h %3m").arg(days).arg(hours).arg(minutes);
    }
    if (hours > 0) return QStringLiteral("%1h %2m").arg(hours).arg(minutes);
    return QStringLiteral("%1m").arg(minutes);
}

QColor state_badge_bg(const QString &state, const SetupTokens &tokens) {
    const auto upper = state.toUpper();
    if (upper == QStringLiteral("STUCK")) return QColor(QStringLiteral("#F59E0B"));
    if (upper == QStringLiteral("ACTIVE")) return tokens.selection_accent;
    if (upper == QStringLiteral("SUSPENDED")) return tokens.danger_text;
    if (upper == QStringLiteral("ASLEEP")) return QColor(QStringLiteral("#64748B"));
    return tokens.muted_text;
}

QString display_state(const QString &state) {
    if (state.isEmpty()) return QStringLiteral("Unknown");
    auto text = state.toLower();
    text[0] = text[0].toUpper();
    return text;
}

QString soul_flow_text(const KanbanAgent &agent) {
    const auto streaming = field_value(agent.llm_fields, "streaming").toLower();
    if (streaming == QStringLiteral("true") || streaming == QStringLiteral("1")) {
        return QStringLiteral("Enabled");
    }
    if (streaming == QStringLiteral("false") || streaming == QStringLiteral("0")) {
        return QStringLiteral("Disabled");
    }
    return streaming.isEmpty() ? QStringLiteral("Disabled") : streaming;
}

class AvatarDisc final : public QWidget {
public:
    AvatarDisc(QWidget *parent, int size)
    : QWidget(parent)
    , size_(size) {
        setFixedSize(size, size);
    }

    void set_name(const QString &name, const QColor &fill, const QColor &ink) {
        letter_ = name.isEmpty() ? QStringLiteral("?") : name.left(1).toUpper();
        fill_ = fill;
        ink_ = ink;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill_);
        painter.drawEllipse(rect());
        auto font = painter.font();
        font.setPixelSize(std::max(12, size_ / 2));
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(ink_);
        painter.drawText(rect(), Qt::AlignCenter, letter_);
    }

private:
    int size_ = 48;
    QString letter_;
    QColor fill_ = QColor(QStringLiteral("#E7F4EF"));
    QColor ink_ = QColor(QStringLiteral("#16785C"));
};

class SegmentedBar final : public QWidget {
public:
    explicit SegmentedBar(QWidget *parent)
    : QWidget(parent) {
        setFixedHeight(10);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void set_parts(std::int64_t system, std::int64_t tools, std::int64_t history,
            std::int64_t free) {
        system_ = std::max<std::int64_t>(0, system);
        tools_ = std::max<std::int64_t>(0, tools);
        history_ = std::max<std::int64_t>(0, history);
        free_ = std::max<std::int64_t>(0, free);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto total = static_cast<double>(system_ + tools_ + history_ + free_);
        auto x = 0.0;
        const auto draw = [&](std::int64_t value, const QColor &color) {
            if (value <= 0 || total <= 0) return;
            const auto width = std::max(2.0, (static_cast<double>(value) / total)
                * static_cast<double>(rect().width()));
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawRoundedRect(QRectF(x, 0, width, height()), 4, 4);
            x += width + 2;
        };
        draw(system_, QColor(QStringLiteral("#0F3D32")));
        draw(tools_, QColor(QStringLiteral("#16785C")));
        draw(history_, QColor(QStringLiteral("#8FBFB0")));
        draw(free_, QColor(QStringLiteral("#E6EDEA")));
    }

private:
    std::int64_t system_ = 0;
    std::int64_t tools_ = 0;
    std::int64_t history_ = 0;
    std::int64_t free_ = 0;
};

class MiniProgress final : public QWidget {
public:
    explicit MiniProgress(QWidget *parent)
    : QWidget(parent) {
        setFixedHeight(6);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void set_fraction(double fraction, const QColor &fill) {
        fraction_ = std::clamp(fraction, 0.0, 1.0);
        fill_ = fill;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#E6EDEA")));
        painter.drawRoundedRect(rect(), 3, 3);
        const auto width = static_cast<int>(rect().width() * fraction_);
        if (width > 0) {
            painter.setBrush(fill_);
            painter.drawRoundedRect(QRect(0, 0, std::max(width, 4), height()), 3, 3);
        }
    }

private:
    double fraction_ = 0.0;
    QColor fill_ = QColor(QStringLiteral("#16785C"));
};

QFrame *v_rule(QWidget *parent, const QColor &color) {
    auto *line = new QFrame(parent);
    line->setObjectName("lingtai_kanban_metric_rule");
    line->setFrameShape(QFrame::VLine);
    line->setFixedWidth(1);
    auto palette = line->palette();
    palette.setColor(QPalette::WindowText, color);
    line->setPalette(palette);
    return line;
}

QLabel *section_heading(QWidget *parent, const QString &text, const char *name) {
    auto *label = make_label(parent, text, name, 13, QFont::DemiBold);
    color_text(label, setup_tokens(parent->palette()).section_text);
    return label;
}

QWidget *make_fact_pair(
        QWidget *parent,
        const QString &label,
        const QString &value,
        const SetupTokens &tokens) {
    auto *cell = new QWidget(parent);
    cell->setMinimumWidth(0);
    cell->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *cell_layout = new QHBoxLayout(cell);
    cell_layout->setContentsMargins(0, 0, 0, 0);
    cell_layout->setSpacing(6);
    auto *k = make_label(cell, label, "lingtai_kanban_detail_k", 11);
    color_text(k, tokens.muted_text);
    k->setWordWrap(false);
    k->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    auto *v = make_label(cell, value, "lingtai_kanban_detail_v", 11);
    color_text(v, tokens.value_text);
    v->setWordWrap(false);
    v->setMinimumWidth(0);
    v->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    cell_layout->addWidget(k, 0);
    cell_layout->addWidget(v, 1);
    return cell;
}

class DetailFactRow final : public QWidget {
public:
    DetailFactRow(
            QWidget *parent,
            const QString &left_label,
            const QString &left_value,
            const QString &right_label,
            const QString &right_value,
            const SetupTokens &tokens)
    : QWidget(parent) {
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        grid_ = new QGridLayout(this);
        grid_->setContentsMargins(0, 2, 0, 2);
        grid_->setHorizontalSpacing(20);
        grid_->setVerticalSpacing(2);
        left_ = make_fact_pair(this, left_label, left_value, tokens);
        if (!right_label.isEmpty()) {
            right_ = make_fact_pair(this, right_label, right_value, tokens);
        }
        place(false);
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        if (width() < 16) return;
        const auto stacked = right_ && width() < 560;
        if (stacked == stacked_) return;
        place(stacked);
    }

private:
    void place(bool stacked) {
        stacked_ = stacked;
        grid_->removeWidget(left_);
        if (right_) grid_->removeWidget(right_);
        if (stacked_ && right_) {
            grid_->addWidget(left_, 0, 0);
            grid_->addWidget(right_, 1, 0);
            grid_->setColumnStretch(0, 1);
            grid_->setColumnStretch(1, 0);
        } else {
            grid_->addWidget(left_, 0, 0);
            if (right_) grid_->addWidget(right_, 0, 1);
            grid_->setColumnStretch(0, 1);
            grid_->setColumnStretch(1, right_ ? 1 : 0);
        }
    }

    QGridLayout *grid_ = nullptr;
    QWidget *left_ = nullptr;
    QWidget *right_ = nullptr;
    bool stacked_ = false;
};

QWidget *detail_fact_row(
        QWidget *parent,
        const QString &left_label,
        const QString &left_value,
        const QString &right_label,
        const QString &right_value,
        const SetupTokens &tokens) {
    return new DetailFactRow(
        parent, left_label, left_value, right_label, right_value, tokens);
}

QWidget *detail_single_row(
        QWidget *parent,
        const QString &label,
        const QString &value,
        const SetupTokens &tokens) {
    return detail_fact_row(parent, label, value, {}, {}, tokens);
}

QString percent_1(double value) {
    return QString::number(value, 'f', 1) + QStringLiteral("%");
}

QLabel *accent_heading(
        QWidget *parent,
        const QString &text,
        const char *name,
        const SetupTokens &tokens) {
    auto *label = section_heading(parent, text, name);
    color_text(label, tokens.selection_accent);
    return label;
}

QWidget *make_detail_section(
        QWidget *parent,
        const QString &title,
        const char *object_name,
        const SetupTokens &tokens) {
    auto *section = new QWidget(parent);
    section->setObjectName(object_name);
    section->setMinimumWidth(0);
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(accent_heading(section, title, "lingtai_kanban_detail_heading",
        tokens));
    return section;
}

QString cache_rate_text(std::int64_t cached, std::int64_t input) {
    if (input <= 0) return QStringLiteral("—");
    return percent_1(100.0 * static_cast<double>(cached)
        / static_cast<double>(input));
}

std::int64_t avg_per_call(std::int64_t tokens, std::int64_t calls) {
    if (calls <= 0) return 0;
    return (tokens + calls / 2) / calls;
}

QWidget *provider_usage_section(
        QWidget *parent,
        const QString &title,
        const char *object_name,
        const std::vector<KanbanProviderSpend> &rows,
        const SetupTokens &tokens) {
    auto *section = make_detail_section(parent, title, object_name, tokens);
    auto *layout = qobject_cast<QVBoxLayout *>(section->layout());
    std::int64_t grand_spend = 0;
    for (const auto &row : rows) grand_spend += row.totals.spend();
    if (rows.empty()) {
        auto *empty = make_label(section,
            QStringLiteral("No token usage recorded yet."),
            "lingtai_kanban_providers_empty", 12);
        color_text(empty, tokens.muted_text);
        layout->addWidget(empty);
        return section;
    }
    for (const auto &row : rows) {
        const auto spend = row.totals.spend();
        const auto pct = grand_spend > 0
            ? 100.0 * static_cast<double>(spend) / static_cast<double>(grand_spend)
            : 0.0;
        auto *header = new QWidget(section);
        header->setMinimumWidth(0);
        header->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *header_layout = new QHBoxLayout(header);
        header_layout->setContentsMargins(0, 4, 0, 2);
        header_layout->setSpacing(10);
        auto *provider_name = make_label(header,
            QString::fromStdString(row.name),
            "lingtai_kanban_provider_name", 13, QFont::DemiBold);
        color_text(provider_name, tokens.value_text);
        provider_name->setWordWrap(false);
        provider_name->setMinimumWidth(0);
        provider_name->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
        auto *bar = new MiniProgress(header);
        bar->setObjectName("lingtai_kanban_provider_share");
        bar->setFixedHeight(8);
        bar->setMinimumWidth(24);
        bar->set_fraction(pct / 100.0, tokens.selection_accent);
        auto *pct_label = make_label(header, percent_1(pct),
            "lingtai_kanban_provider_pct", 12);
        color_text(pct_label, tokens.muted_text);
        pct_label->setWordWrap(false);
        pct_label->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
        header_layout->addWidget(provider_name, 0);
        header_layout->addWidget(bar, 1);
        header_layout->addWidget(pct_label, 0);
        layout->addWidget(header);
        layout->addWidget(detail_fact_row(section,
            QStringLiteral("input"), comma_number(row.totals.input),
            QStringLiteral("output"), comma_number(row.totals.output), tokens));
        layout->addWidget(detail_fact_row(section,
            QStringLiteral("thinking"), comma_number(row.totals.thinking),
            QStringLiteral("cached"), comma_number(row.totals.cached), tokens));
        layout->addWidget(detail_fact_row(section,
            QStringLiteral("api_calls"), comma_number(row.totals.api_calls),
            QStringLiteral("miss"), comma_number(row.totals.miss()), tokens));
        if (row.totals.input > 0) {
            layout->addWidget(detail_single_row(section,
                QStringLiteral("cache hit"),
                cache_rate_text(row.totals.cached, row.totals.input),
                tokens));
        }
    }
    return section;
}

void add_session_api_section(
        QVBoxLayout *detail_layout,
        QWidget *parent,
        const QString &title,
        const char *object_name,
        const KanbanSessionStats &stats,
        const SetupTokens &tokens) {
    if (stats.tokens.api_calls <= 0) return;
    auto *section = make_detail_section(parent, title, object_name, tokens);
    auto *layout = qobject_cast<QVBoxLayout *>(section->layout());
    const auto token_spend = stats.tokens.spend();
    layout->addWidget(detail_single_row(section,
        QStringLiteral("api_calls"), comma_number(stats.tokens.api_calls), tokens));
    layout->addWidget(detail_single_row(section,
        QStringLiteral("tool_calls"), comma_number(stats.tool_calls), tokens));
    layout->addWidget(detail_single_row(section,
        QStringLiteral("tokens"), comma_number(token_spend), tokens));
    layout->addWidget(detail_single_row(section,
        QStringLiteral("input / output / thinking"),
        QStringLiteral("%1 / %2 / %3")
            .arg(comma_number(stats.tokens.input),
                comma_number(stats.tokens.output),
                comma_number(stats.tokens.thinking)),
        tokens));
    layout->addWidget(detail_single_row(section,
        QStringLiteral("cached / missed"),
        QStringLiteral("%1 / %2")
            .arg(comma_number(stats.tokens.cached),
                comma_number(stats.tokens.miss())),
        tokens));
    layout->addWidget(detail_single_row(section,
        QStringLiteral("cache hit rate"),
        cache_rate_text(stats.tokens.cached, stats.tokens.input),
        tokens));
    layout->addWidget(detail_single_row(section,
        QStringLiteral("tokens/api_call"),
        comma_number(avg_per_call(token_spend, stats.tokens.api_calls)),
        tokens));
    const auto tool_avg = stats.tokens.api_calls > 0
        ? static_cast<double>(stats.tool_calls)
            / static_cast<double>(stats.tokens.api_calls)
        : 0.0;
    layout->addWidget(detail_single_row(section,
        QStringLiteral("tool_calls/api_call"),
        QString::number(tool_avg, 'f', 2),
        tokens));
    if (stats.has_codex_transfer_mode) {
        layout->addWidget(detail_single_row(section,
            QStringLiteral("transfer full / incremental"),
            QStringLiteral("%1 / %2").arg(stats.codex_full).arg(stats.codex_incremental),
            tokens));
    }
    detail_layout->addWidget(section);
}

QString dash_if_empty(const std::string &value) {
    return value.empty() ? QStringLiteral("—") : QString::fromStdString(value);
}

QString format_kanban_timestamp(const std::string &ts) {
    auto parsed = QDateTime::fromString(
        QString::fromStdString(ts), Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(
            QString::fromStdString(ts), Qt::ISODate);
    }
    if (!parsed.isValid()) {
        const auto text = QString::fromStdString(ts);
        return text.size() > 16 ? text.left(16) : text;
    }
    const auto local = parsed.toLocalTime();
    auto offset = local.offsetFromUtc();
    auto sign = QLatin1Char('+');
    if (offset < 0) {
        sign = QLatin1Char('-');
        offset = -offset;
    }
    return local.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
        + QStringLiteral(" U") + sign
        + QString::number(offset / 3600) + QLatin1Char(':')
        + QStringLiteral("%1").arg(
            (offset % 3600) / 60, 2, 10, QLatin1Char('0'));
}

QLabel *mono_row(
        QWidget *parent,
        const QString &text,
        const char *object_name,
        const SetupTokens &tokens,
        bool muted = false) {
    auto *label = new QLabel(text, parent);
    label->setObjectName(object_name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(false);
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(11);
    label->setFont(font);
    color_text(label, muted ? tokens.muted_text : tokens.section_text);
    return label;
}

[[nodiscard]] bool is_reconstruct_entry(const KanbanLedgerEntry &entry) {
    const auto reason = QString::fromStdString(entry.codex_ws_delta_reason)
        .trimmed().toLower();
    if (reason == QStringLiteral("epoch_reset")
            || reason == QStringLiteral("no_baseline")
            || reason == QStringLiteral("reconstruct")
            || reason == QStringLiteral("reconstructed")
            || reason == QStringLiteral("context_rebuild")
            || reason == QStringLiteral("context_reconstructed")) {
        return true;
    }
    return QString::fromStdString(entry.codex_transfer_mode)
        .trimmed().compare(QStringLiteral("full"), Qt::CaseInsensitive) == 0;
}

void add_separator_label(
        std::map<int, std::vector<QString>> &out,
        int index,
        const QString &label) {
    auto &labels = out[index];
    for (const auto &existing : labels) {
        if (existing == label) return;
    }
    labels.push_back(label);
}

void mark_boundary_times(
        std::map<int, std::vector<QString>> &out,
        const std::vector<QDateTime> &parsed,
        const std::vector<char> &ok,
        const std::vector<std::int64_t> &times_ms,
        const QString &label) {
    for (const auto ms : times_ms) {
        const auto boundary = QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::utc());
        for (int i = 0; i + 1 < static_cast<int>(parsed.size()); ++i) {
            if (!ok[static_cast<std::size_t>(i)]
                    || !ok[static_cast<std::size_t>(i + 1)]) {
                continue;
            }
            if (parsed[static_cast<std::size_t>(i)] >= boundary
                    && parsed[static_cast<std::size_t>(i + 1)] < boundary) {
                add_separator_label(out, i, label);
                break;
            }
        }
    }
}

std::map<int, std::vector<QString>> ledger_separator_labels(
        const std::vector<KanbanLedgerEntry> &entries,
        const std::vector<std::int64_t> &molt_times_ms,
        const std::vector<std::int64_t> &refresh_times_ms) {
    std::map<int, std::vector<QString>> out;
    if (entries.size() < 2) return out;
    for (int i = 0; i + 1 < static_cast<int>(entries.size()); ++i) {
        if (is_reconstruct_entry(entries[static_cast<std::size_t>(i)])) {
            add_separator_label(out, i, QStringLiteral("context rebuilt"));
        }
    }
    std::vector<QDateTime> parsed(entries.size());
    std::vector<char> ok(entries.size(), 0);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto ts = QDateTime::fromString(
            QString::fromStdString(entries[i].ts), Qt::ISODateWithMs);
        if (!ts.isValid()) {
            ts = QDateTime::fromString(
                QString::fromStdString(entries[i].ts), Qt::ISODate);
        }
        if (!ts.isValid()) continue;
        parsed[i] = ts.toUTC();
        ok[i] = 1;
    }
    mark_boundary_times(out, parsed, ok, refresh_times_ms,
        QStringLiteral("context rebuilt"));
    mark_boundary_times(out, parsed, ok, molt_times_ms,
        QStringLiteral("molt"));
    return out;
}

QString main_call_line(const KanbanLedgerEntry &entry) {
    return QStringLiteral("%1  %2  %3  %4  %5  %6  %7  %8  %9  %10")
        .arg(format_kanban_timestamp(entry.ts), -24)
        .arg(dash_if_empty(entry.provider), -10)
        .arg(dash_if_empty(entry.model), -24)
        .arg(comma_number(entry.input), 10)
        .arg(comma_number(entry.output), 10)
        .arg(comma_number(entry.thinking), 10)
        .arg(comma_number(entry.cached), 10)
        .arg(comma_number(entry.miss()), 10)
        .arg(cache_rate_text(entry.cached, entry.input), 7)
        .arg(dash_if_empty(entry.endpoint));
}

QString daemon_call_line(const KanbanDaemonLedgerEntry &entry) {
    return QStringLiteral("%1  %2  %3  %4  %5  %6  %7  %8  %9  %10  %11  %12")
        .arg(format_kanban_timestamp(entry.ts), -24)
        .arg(dash_if_empty(entry.handle), -10)
        .arg(dash_if_empty(entry.state), -8)
        .arg(dash_if_empty(entry.backend), -10)
        .arg(dash_if_empty(entry.model), -24)
        .arg(comma_number(entry.input), 10)
        .arg(comma_number(entry.output), 10)
        .arg(comma_number(entry.thinking), 10)
        .arg(comma_number(entry.cached), 10)
        .arg(comma_number(entry.miss()), 10)
        .arg(cache_rate_text(entry.cached, entry.input), 7)
        .arg(dash_if_empty(entry.endpoint));
}

void add_separator_rows(
        QVBoxLayout *layout,
        QWidget *parent,
        const std::map<int, std::vector<QString>> &separators,
        int index,
        const SetupTokens &tokens) {
    const auto found = separators.find(index);
    if (found == separators.end()) return;
    for (const auto &label : found->second) {
        layout->addWidget(mono_row(parent,
            QStringLiteral("┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈ %1").arg(label),
            "lingtai_kanban_recent_separator", tokens, true));
    }
}

void add_token_totals(KanbanTokenTotals &into, const KanbanTokenTotals &from) {
    into.input += from.input;
    into.output += from.output;
    into.thinking += from.thinking;
    into.cached += from.cached;
    into.api_calls += from.api_calls;
}

QWidget *kv_cell(
        QWidget *parent,
        const QString &label,
        const QString &value,
        const SetupTokens &tokens,
        const char *object_name) {
    auto *cell = new QWidget(parent);
    auto *layout = new QVBoxLayout(cell);
    layout->setContentsMargins(0, 0, 12, 10);
    layout->setSpacing(2);
    auto *k = make_label(cell, label, "lingtai_kanban_kv_label", 10);
    color_text(k, tokens.muted_text);
    auto *v = make_label(cell, value.isEmpty() ? QStringLiteral("—") : value,
        object_name, 12);
    color_text(v, tokens.value_text);
    layout->addWidget(k);
    layout->addWidget(v);
    return cell;
}

QWidget *metric_cell(
        QWidget *parent,
        const QString &label,
        const QString &value,
        const QString &hint,
        const char *object_name,
        const SetupTokens &tokens,
        QWidget *extra = nullptr) {
    auto *cell = new QWidget(parent);
    cell->setObjectName(object_name);
    cell->setMinimumWidth(0);
    cell->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    auto *layout = new QVBoxLayout(cell);
    layout->setContentsMargins(12, 4, 12, 4);
    layout->setSpacing(4);
    auto *k = make_label(cell, label, "lingtai_kanban_metric_label", 10);
    color_text(k, tokens.muted_text);
    auto *v = make_label(cell, value, "lingtai_kanban_stat_value", 16, QFont::DemiBold);
    color_text(v, tokens.value_text);
    layout->addWidget(k);
    layout->addWidget(v);
    if (extra) layout->addWidget(extra);
    if (!hint.isEmpty()) {
        auto *h = make_label(cell, hint, "lingtai_kanban_metric_hint", 10);
        color_text(h, tokens.muted_text);
        layout->addWidget(h);
    }
    return cell;
}

QLabel *pill(
        QWidget *parent,
        const QString &text,
        const QColor &bg,
        const QColor &fg,
        const char *object_name) {
    auto *label = make_label(parent, text, object_name, 10, QFont::DemiBold);
    label->setAlignment(Qt::AlignCenter);
    label->setContentsMargins(8, 2, 8, 2);
    label->setStyleSheet(QStringLiteral(
        "background: %1; color: %2; border-radius: 9px; padding: 2px 8px;")
        .arg(setup_color_css(bg), setup_color_css(fg)));
    return label;
}

void clear_layout(QLayout *layout) {
    if (!layout) return;
    while (auto *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

void detach_items(QLayout *layout) {
    if (!layout) return;
    while (auto *item = layout->takeAt(0)) {
        delete item;
    }
}

void shrink_width(QWidget *widget) {
    if (!widget) return;
    widget->setMinimumWidth(0);
}

void place_grid(
        QGridLayout *grid, const std::vector<QWidget *> &cells, int columns) {
    if (!grid || columns < 1) return;
    detach_items(grid);
    for (auto index = 0; index != static_cast<int>(cells.size()); ++index) {
        grid->addWidget(cells[static_cast<std::size_t>(index)],
            index / columns, index % columns);
    }
}

QColor avatar_fill_for(const QString &name) {
    static const QColor fills[] = {
        QColor(QStringLiteral("#DCEEE6")),
        QColor(QStringLiteral("#E7F4EF")),
        QColor(QStringLiteral("#F4E7E7")),
        QColor(QStringLiteral("#E7EEF4")),
        QColor(QStringLiteral("#F4F0E7")),
    };
    auto hash = 0;
    for (const auto ch : name) hash = (hash * 33 + ch.unicode()) & 0x7fffffff;
    return fills[hash % 5];
}

} // namespace

void KanbanPage::choose_agent(const QString &directory_key) {
    emit agent_selected(directory_key);
}

KanbanPage::KanbanPage(QWidget *parent)
: QWidget(parent) {
    setObjectName("lingtai_kanban_page");
    setAccessibleName(QStringLiteral("Agent properties"));
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 12, 28, 20);
    root->setSpacing(12);

    auto *back = new QPushButton(QStringLiteral("←  Agents"), this);
    back->setObjectName("lingtai_kanban_back");
    back->setFlat(true);
    back->setCursor(Qt::PointingHandCursor);
    back->setAccessibleName(QStringLiteral("Agents"));
    QObject::connect(back, &QPushButton::clicked, this, &KanbanPage::back_requested);
    root->addWidget(back, 0, Qt::AlignLeft);

    hero_host_ = new QWidget(this);
    hero_host_->setObjectName("lingtai_kanban_hero");
    auto *hero_layout = new QVBoxLayout(hero_host_);
    hero_layout->setContentsMargins(0, 0, 0, 0);
    hero_layout->setSpacing(0);
    root->addWidget(hero_host_);

    stack_ = new QStackedWidget(this);
    stack_->setObjectName("lingtai_kanban_stack");
    stack_->setMinimumWidth(0);
    stack_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto *summary_scroll = new QScrollArea(stack_);
    summary_scroll->setObjectName("lingtai_kanban_summary_scroll");
    summary_scroll->setWidgetResizable(true);
    summary_scroll->setFrameShape(QFrame::NoFrame);
    summary_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    summary_scroll->setMinimumWidth(0);
    summary_scroll->setFocusPolicy(Qt::NoFocus);
    summary_scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    summary_body_ = new QWidget(summary_scroll);
    summary_body_->setObjectName("lingtai_kanban_summary");
    summary_body_->setMinimumWidth(0);
    summary_body_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *summary_layout = new QVBoxLayout(summary_body_);
    summary_layout->setContentsMargins(0, 0, kKanbanScrollGutter, 0);
    summary_layout->setSpacing(18);
    summary_scroll->setWidget(summary_body_);
    stack_->addWidget(summary_scroll);

    auto *detail_scroll = new QScrollArea(stack_);
    detail_scroll->setObjectName("lingtai_kanban_detail_scroll");
    detail_scroll->setWidgetResizable(true);
    detail_scroll->setFrameShape(QFrame::NoFrame);
    detail_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detail_scroll->setMinimumWidth(0);
    detail_scroll->setFocusPolicy(Qt::NoFocus);
    detail_scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    detail_body_ = new QWidget(detail_scroll);
    detail_body_->setObjectName("lingtai_kanban_detail");
    detail_body_->setMinimumWidth(0);
    detail_body_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    auto *detail_layout = new QVBoxLayout(detail_body_);
    detail_layout->setContentsMargins(0, 0, kKanbanScrollGutter, 0);
    detail_layout->setSpacing(14);
    detail_scroll->setWidget(detail_body_);
    stack_->addWidget(detail_scroll);

    root->addWidget(stack_, 1);
    apply_chrome();
}

void KanbanPage::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        apply_chrome();
    }
}

void KanbanPage::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    relayout();
}

void KanbanPage::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    relayout();
}

void KanbanPage::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), st::windowBg->c);
}

void KanbanPage::relayout() {
    const auto w = width();
    if (auto *root = layout()) {
        const auto pad = w < 520 ? 16 : 28;
        root->setContentsMargins(pad, 12, pad, 20);
    }
    if (summary_body_) apply_scroll_gutter(summary_body_->layout());
    if (detail_body_) apply_scroll_gutter(detail_body_->layout());

    if (hero_row_ && actions_ && hero_host_) {
        auto *hero_layout = qobject_cast<QHBoxLayout *>(hero_row_->layout());
        auto *host_layout = qobject_cast<QVBoxLayout *>(hero_host_->layout());
        if (hero_layout && host_layout) {
            const auto wrap_hero = w < 520;
            if (wrap_hero) {
                if (hero_layout->indexOf(actions_) >= 0) {
                    hero_layout->removeWidget(actions_);
                }
                if (host_layout->indexOf(actions_) < 0) {
                    host_layout->addWidget(actions_, 0, Qt::AlignLeft);
                }
            } else {
                if (host_layout->indexOf(actions_) >= 0) {
                    host_layout->removeWidget(actions_);
                }
                if (hero_layout->indexOf(actions_) < 0) {
                    hero_layout->addWidget(actions_, 0, Qt::AlignTop);
                }
            }
        }
    }

    if (metrics_grid_ && !metric_cells_.empty()) {
        const auto metric_cols = w >= 700 ? 5 : w >= 480 ? 3 : 2;
        detach_items(metrics_grid_);
        for (auto column = 0; column != 12; ++column) {
            metrics_grid_->setColumnStretch(column, 0);
        }
        if (metric_cols == 5) {
            auto column = 0;
            for (auto index = 0; index != static_cast<int>(metric_cells_.size());
                    ++index) {
                metrics_grid_->addWidget(metric_cells_[static_cast<std::size_t>(index)],
                    0, column++);
                metrics_grid_->setColumnStretch(column - 1, 1);
                if (index < static_cast<int>(metric_rules_.size())) {
                    metric_rules_[static_cast<std::size_t>(index)]->show();
                    metrics_grid_->addWidget(
                        metric_rules_[static_cast<std::size_t>(index)], 0, column++);
                    metrics_grid_->setColumnStretch(column - 1, 0);
                }
            }
        } else {
            for (auto *rule : metric_rules_) rule->hide();
            place_grid(metrics_grid_, metric_cells_, metric_cols);
            for (auto column = 0; column != metric_cols; ++column) {
                metrics_grid_->setColumnStretch(column, 1);
            }
        }
    }

    if (columns_grid_ && left_column_ && right_column_) {
        const auto stacked = w < 540;
        detach_items(columns_grid_);
        columns_grid_->setColumnStretch(0, 0);
        columns_grid_->setColumnStretch(1, 0);
        if (stacked) {
            columns_grid_->addWidget(left_column_, 0, 0);
            columns_grid_->addWidget(right_column_, 1, 0);
            columns_grid_->setColumnStretch(0, 1);
            columns_grid_->setColumnStretch(1, 0);
            columns_grid_->setHorizontalSpacing(0);
        } else {
            columns_grid_->addWidget(left_column_, 0, 0);
            columns_grid_->addWidget(right_column_, 0, 1);
            columns_grid_->setColumnStretch(0, 7);
            columns_grid_->setColumnStretch(1, 4);
            columns_grid_->setHorizontalSpacing(36);
        }
    }

    const auto stacked = w < 540;
    const auto facts_width = stacked ? w : std::max(1, w * 7 / 11);
    if (model_grid_ && !model_cells_.empty()) {
        place_grid(model_grid_, model_cells_, facts_width < 420 ? 1 : 2);
    }
    if (legend_grid_ && !legend_items_.empty()) {
        place_grid(legend_grid_, legend_items_, facts_width < 520 ? 2 : 4);
    }
    if (caps_grid_ && !cap_pills_.empty()) {
        const auto cap_cols = std::clamp(facts_width / 140, 2, 4);
        place_grid(caps_grid_, cap_pills_, cap_cols);
    }
}

void KanbanPage::apply_chrome() {
    if (applying_chrome_) return;
    applying_chrome_ = true;
    const auto bg = st::windowBg->c;
    auto palette = this->palette();
    palette.setColor(QPalette::Window, bg);
    palette.setColor(QPalette::Base, bg);
    palette.setColor(QPalette::Button, bg);
    palette.setColor(QPalette::WindowText, st::windowFg->c);
    palette.setColor(QPalette::Text, st::windowFg->c);
    palette.setColor(QPalette::ButtonText, st::windowFg->c);
    setPalette(palette);
    const auto tokens = setup_tokens(this->palette());
    setAutoFillBackground(true);
    if (hero_host_) hero_host_->setAutoFillBackground(false);
    if (stack_) stack_->setAutoFillBackground(false);
    if (summary_body_) summary_body_->setAutoFillBackground(false);
    if (detail_body_) detail_body_->setAutoFillBackground(false);
    for (auto *scroll : findChildren<QScrollArea *>()) {
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setAutoFillBackground(false);
        scroll->setStyleSheet(QString());
        if (auto *viewport = scroll->viewport()) {
            viewport->setStyleSheet(QString());
            viewport->setAutoFillBackground(true);
            auto vp_palette = viewport->palette();
            vp_palette.setColor(QPalette::Window, bg);
            vp_palette.setColor(QPalette::Base, bg);
            viewport->setPalette(vp_palette);
        }
    }
    if (auto *back = findChild<QPushButton *>("lingtai_kanban_back")) {
        back->setStyleSheet(QStringLiteral(
            "QPushButton { color: %1; border: none; background: transparent; "
            "font-size: 13px; padding: 0; }")
            .arg(setup_color_css(tokens.selection_accent)));
    }
    if (auto *detail_back = findChild<QPushButton *>("lingtai_kanban_detail_back")) {
        detail_back->setStyleSheet(QStringLiteral(
            "QPushButton { color: %1; border: none; background: transparent; "
            "font-size: 13px; padding: 0; }")
            .arg(setup_color_css(tokens.selection_accent)));
    }
    if (detail_button_) {
        apply_setup_primary_button(detail_button_);
        detail_button_->setText(detail_open_
            ? QStringLiteral("Back to summary")
            : QStringLiteral("Open context detail"));
    }
    if (auto *reload = findChild<QPushButton *>("lingtai_kanban_reload")) {
        reload->setStyleSheet(QStringLiteral(
            "QPushButton { min-height: 34px; padding: 0 16px; border-radius: 6px; "
            "background: %1; color: palette(text); border: 1px solid %2; }")
            .arg(setup_color_css(bg), setup_color_css(tokens.border)));
        reload->setMinimumHeight(32);
    }
    update();
    applying_chrome_ = false;
}

void KanbanPage::set_board(
        const KanbanBoard &board,
        const std::optional<std::filesystem::path> &selected_key) {
    auto scroll_value = [](QScrollArea *scroll) {
        return scroll && scroll->verticalScrollBar()
            ? scroll->verticalScrollBar()->value() : 0;
    };
    auto *summary_scroll = findChild<QScrollArea *>(
        "lingtai_kanban_summary_scroll");
    auto *detail_scroll = findChild<QScrollArea *>(
        "lingtai_kanban_detail_scroll");
    const auto summary_pos = scroll_value(summary_scroll);
    const auto detail_pos = scroll_value(detail_scroll);
    board_ = board;
    selected_key_ = selected_key;
    rebuild();
    apply_chrome();
    summary_scroll = findChild<QScrollArea *>("lingtai_kanban_summary_scroll");
    detail_scroll = findChild<QScrollArea *>("lingtai_kanban_detail_scroll");
    if (summary_scroll && summary_scroll->verticalScrollBar()) {
        summary_scroll->verticalScrollBar()->setValue(summary_pos);
    }
    if (detail_scroll && detail_scroll->verticalScrollBar()) {
        detail_scroll->verticalScrollBar()->setValue(detail_pos);
    }
}

const KanbanAgent *KanbanPage::selected_agent() const {
    if (selected_key_) {
        for (const auto &agent : board_.agents) {
            if (agent.directory_key == *selected_key_) return &agent;
        }
    }
    for (const auto &agent : board_.agents) {
        if (!agent.is_human) return &agent;
    }
    return board_.agents.empty() ? nullptr : &board_.agents.front();
}

void KanbanPage::show_summary() {
    detail_open_ = false;
    stack_->setCurrentIndex(0);
    if (hero_host_) hero_host_->show();
    if (detail_button_) {
        detail_button_->setText(QStringLiteral("Open context detail"));
        detail_button_->setToolTip(QStringLiteral("Ctrl+D"));
    }
    setFocus(Qt::OtherFocusReason);
}

void KanbanPage::show_detail() {
    detail_open_ = true;
    stack_->setCurrentIndex(1);
    if (hero_host_) hero_host_->hide();
    if (detail_button_) {
        detail_button_->setText(QStringLiteral("Back to summary"));
        detail_button_->setToolTip(QStringLiteral("Ctrl+D"));
    }
    setFocus(Qt::OtherFocusReason);
}

bool KanbanPage::consume_detail_shortcut(QKeyEvent *event) {
    if (!event) return false;
    const auto chord = event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier);
    if (event->key() == Qt::Key_D && chord) {
        if (detail_open_) show_summary();
        else show_detail();
        return true;
    }
    if (event->key() == Qt::Key_Escape && detail_open_) {
        show_summary();
        return true;
    }
    return false;
}

void KanbanPage::keyPressEvent(QKeyEvent *event) {
    if (consume_detail_shortcut(event)) {
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool KanbanPage::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::KeyPress && isVisible()) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (consume_detail_shortcut(key)) return true;
    }
    return QWidget::eventFilter(watched, event);
}

void KanbanPage::rebuild() {
    const auto tokens = setup_tokens(palette());
    const auto *agent = selected_agent();
    auto *host_layout = hero_host_
        ? qobject_cast<QVBoxLayout *>(hero_host_->layout()) : nullptr;
    auto *summary_layout = qobject_cast<QVBoxLayout *>(summary_body_->layout());
    auto *detail_layout = qobject_cast<QVBoxLayout *>(detail_body_->layout());
    clear_layout(host_layout);
    clear_layout(summary_layout);
    clear_layout(detail_layout);
    detail_button_ = nullptr;
    hero_row_ = nullptr;
    actions_ = nullptr;
    metrics_grid_ = nullptr;
    columns_grid_ = nullptr;
    model_grid_ = nullptr;
    legend_grid_ = nullptr;
    caps_grid_ = nullptr;
    left_column_ = nullptr;
    right_column_ = nullptr;
    metric_cells_.clear();
    metric_rules_.clear();
    model_cells_.clear();
    legend_items_.clear();
    cap_pills_.clear();
    if (!host_layout || !summary_layout || !detail_layout) return;
    if (!agent) {
        auto *empty = make_label(summary_body_,
            QStringLiteral("Select an Agent to inspect its properties."),
            "lingtai_kanban_empty", 13);
        color_text(empty, tokens.muted_text);
        summary_layout->addWidget(empty);
        summary_layout->addStretch(1);
        if (detail_open_) show_detail();
        else show_summary();
        relayout();
        for (auto *child : findChildren<QWidget *>()) {
            child->installEventFilter(this);
        }
        return;
    }

    const auto name = QString::fromStdString(agent->display_name);
    const auto state = QString::fromStdString(agent->state);
    const auto agent_id = field_value(agent->identity_fields, "id");
    const auto address = field_value(agent->identity_fields, "address");
    const auto language = field_value(agent->identity_fields, "language");
    const auto started = field_value(agent->identity_fields, "started_at");

    auto *hero = new QWidget(hero_host_);
    hero->setObjectName("lingtai_kanban_hero_row");
    hero_row_ = hero;
    auto *hero_layout = new QHBoxLayout(hero);
    hero_layout->setContentsMargins(0, 0, 0, 0);
    hero_layout->setSpacing(16);
    auto *avatar = new AvatarDisc(hero, 56);
    avatar->setObjectName("lingtai_kanban_avatar");
    avatar->set_name(name, QColor(QStringLiteral("#DCEEE6")), tokens.selection_accent);
    hero_layout->addWidget(avatar, 0, Qt::AlignTop);

    auto *identity = new QWidget(hero);
    identity->setObjectName("lingtai_kanban_section_identity");
    auto *identity_layout = new QVBoxLayout(identity);
    identity_layout->setContentsMargins(0, 2, 0, 0);
    identity_layout->setSpacing(4);
    auto *title_row = new QWidget(identity);
    auto *title_layout = new QHBoxLayout(title_row);
    title_layout->setContentsMargins(0, 0, 0, 0);
    title_layout->setSpacing(10);
    auto *title = make_label(title_row, name, "lingtai_kanban_name", 22, QFont::DemiBold);
    color_text(title, tokens.value_text);
    title_layout->addWidget(title, 0, Qt::AlignVCenter);
    title_layout->addWidget(pill(title_row, display_state(state),
        state_badge_bg(state, tokens), QColor(Qt::white),
        "lingtai_kanban_state"), 0, Qt::AlignVCenter);
    title_layout->addStretch(1);
    identity_layout->addWidget(title_row);
    if (!agent_id.isEmpty()) {
        auto *id = make_label(identity, agent_id, "lingtai_kanban_id", 11);
        color_text(id, tokens.muted_text);
        identity_layout->addWidget(id);
    }
    QStringList meta;
    if (!address.isEmpty()) meta << QStringLiteral("Address  %1").arg(address);
    if (!language.isEmpty()) meta << language;
    if (!started.isEmpty()) meta << pretty_started(started);
    auto *meta_label = make_label(identity, meta.join(QStringLiteral("     ")),
        "lingtai_kanban_meta", 11);
    color_text(meta_label, tokens.muted_text);
    identity_layout->addWidget(meta_label);
    hero_layout->addWidget(identity, 1);

    auto *actions = new QWidget(hero);
    actions->setObjectName("lingtai_kanban_actions");
    actions_ = actions;
    auto *actions_layout = new QHBoxLayout(actions);
    actions_layout->setContentsMargins(0, 0, 0, 0);
    actions_layout->setSpacing(8);
    auto *reload = new QPushButton(QStringLiteral("Reload"), actions);
    reload->setObjectName("lingtai_kanban_reload");
    reload->setCursor(Qt::PointingHandCursor);
    QObject::connect(reload, &QPushButton::clicked, this, &KanbanPage::reload_requested);
    detail_button_ = new QPushButton(
        detail_open_ ? QStringLiteral("Back to summary")
                     : QStringLiteral("Open context detail"),
        actions);
    detail_button_->setObjectName("lingtai_kanban_open_detail");
    detail_button_->setCursor(Qt::PointingHandCursor);
    QObject::connect(detail_button_, &QPushButton::clicked, [this] {
        if (detail_open_) show_summary();
        else show_detail();
    });
    actions_layout->addWidget(reload);
    actions_layout->addWidget(detail_button_);
    hero_layout->addWidget(actions, 0, Qt::AlignTop);
    host_layout->addWidget(hero);

    auto *metrics = new QWidget(summary_body_);
    metrics->setObjectName("lingtai_kanban_pulse");
    shrink_width(metrics);
    auto *metrics_layout = new QGridLayout(metrics);
    metrics_grid_ = metrics_layout;
    metrics_layout->setContentsMargins(0, 8, 0, 8);
    metrics_layout->setHorizontalSpacing(0);
    metrics_layout->setVerticalSpacing(12);
    const auto divider = tokens.border;
    auto *context_extra = static_cast<QWidget *>(nullptr);
    auto context_value = QStringLiteral("—");
    auto context_hint = QString();
    if (agent->context && agent->context->window_size > 0) {
        const auto used = agent->context->total_tokens.value_or(0);
        context_value = QStringLiteral("%1 / %2")
            .arg(comma_number(used), comma_number(agent->context->window_size));
        const auto pct = agent->context->usage_percent.value_or(
            agent->context->window_size
                ? 100.0 * static_cast<double>(used)
                    / static_cast<double>(agent->context->window_size)
                : 0.0);
        context_hint = QStringLiteral("%1%").arg(QString::number(pct, 'f', 1));
        auto *bar = new MiniProgress(metrics);
        bar->set_fraction(pct / 100.0, tokens.selection_accent);
        context_extra = bar;
    }
    auto uptime = uptime_from_started(started);
    if (uptime.isEmpty()) uptime = QStringLiteral("—");
    const auto add_metric = [&](QWidget *cell) {
        shrink_width(cell);
        metric_cells_.push_back(cell);
    };
    add_metric(metric_cell(metrics, QStringLiteral("Uptime"),
        uptime, {}, "lingtai_kanban_metric_uptime", tokens));
    metric_rules_.push_back(v_rule(metrics, divider));
    add_metric(metric_cell(metrics, QStringLiteral("Context"),
        context_value, context_hint, "lingtai_kanban_metric_context", tokens,
        context_extra));
    metric_rules_.push_back(v_rule(metrics, divider));
    add_metric(metric_cell(metrics, QStringLiteral("Total tokens"),
        compact_number(agent->tokens.spend()), {},
        "lingtai_kanban_stat_tokens", tokens));
    metric_rules_.push_back(v_rule(metrics, divider));
    add_metric(metric_cell(metrics, QStringLiteral("API calls"),
        comma_number(agent->tokens.api_calls), {},
        "lingtai_kanban_metric_api_calls", tokens));
    metric_rules_.push_back(v_rule(metrics, divider));
    const auto members = board_.agent_count + board_.human_count;
    add_metric(metric_cell(metrics, QStringLiteral("Network"),
        QStringLiteral("%1 members").arg(members),
        QStringLiteral("%1 agents  ·  %2 human%3")
            .arg(board_.agent_count)
            .arg(board_.human_count)
            .arg(board_.human_count == 1 ? QString() : QStringLiteral("s")),
        "lingtai_kanban_metric_network", tokens));
    summary_layout->addWidget(metrics);

    auto *rule = new QFrame(summary_body_);
    rule->setFrameShape(QFrame::HLine);
    rule->setStyleSheet(QStringLiteral("color: %1;").arg(setup_color_css(divider)));
    summary_layout->addWidget(rule);

    auto *columns = new QWidget(summary_body_);
    columns->setObjectName("lingtai_kanban_columns");
    shrink_width(columns);
    auto *columns_layout = new QGridLayout(columns);
    columns_grid_ = columns_layout;
    columns_layout->setContentsMargins(0, 0, 0, 0);
    columns_layout->setHorizontalSpacing(36);
    columns_layout->setVerticalSpacing(24);

    auto *left = new QWidget(columns);
    left->setObjectName("lingtai_kanban_left");
    left_column_ = left;
    shrink_width(left);
    auto *left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(18);

    left_layout->addWidget(section_heading(left, QStringLiteral("Model & runtime"),
        "lingtai_kanban_heading_model"));
    auto *grid = new QWidget(left);
    grid->setObjectName("lingtai_kanban_model_grid");
    auto *grid_layout = new QGridLayout(grid);
    model_grid_ = grid_layout;
    grid_layout->setContentsMargins(0, 0, 0, 0);
    grid_layout->setHorizontalSpacing(24);
    grid_layout->setVerticalSpacing(4);
    const auto pairs = std::vector<std::pair<QString, QString>>{
        {QStringLiteral("Model"), field_value(agent->llm_fields, "model")},
        {QStringLiteral("Provider"), field_value(agent->llm_fields, "provider")},
        {QStringLiteral("Base URL"), field_value(agent->llm_fields, "base_url")},
        {QStringLiteral("API compatibility"), field_value(agent->llm_fields, "api_compat")},
        {QStringLiteral("Soul flow"), soul_flow_text(*agent)},
        {QStringLiteral("Context limit"), field_value(agent->llm_fields, "context_limit")},
        {QStringLiteral("Max turns"), field_value(agent->runtime_fields, "max_turns")},
        {QStringLiteral("Max RPM"), field_value(agent->runtime_fields, "max_rpm")},
    };
    for (auto index = 0; index != static_cast<int>(pairs.size()); ++index) {
        auto *cell = kv_cell(grid, pairs[index].first, pairs[index].second, tokens,
            index == 0 ? "lingtai_kanban_section_llm" : "lingtai_kanban_kv_value");
        shrink_width(cell);
        model_cells_.push_back(cell);
        grid_layout->addWidget(cell, index / 2, index % 2);
    }
    left_layout->addWidget(grid);

    left_layout->addWidget(section_heading(left, QStringLiteral("Context usage"),
        "lingtai_kanban_heading_context"));
    auto *context_block = new QWidget(left);
    context_block->setObjectName("lingtai_kanban_section_context");
    auto *context_layout = new QVBoxLayout(context_block);
    context_layout->setContentsMargins(0, 0, 0, 0);
    context_layout->setSpacing(8);
    auto *segments = new SegmentedBar(context_block);
    auto system_tokens = agent->context ? agent->context->system_tokens.value_or(0) : 0;
    auto tools_tokens = agent->context ? agent->context->tools_tokens.value_or(0) : 0;
    auto history_tokens = agent->context ? agent->context->history_tokens.value_or(0) : 0;
    auto window = agent->context ? agent->context->window_size : 0;
    auto used = agent->context ? agent->context->total_tokens.value_or(
        system_tokens + tools_tokens + history_tokens) : 0;
    auto free_tokens = window > used ? window - used : 0;
    segments->set_parts(system_tokens, tools_tokens, history_tokens, free_tokens);
    context_layout->addWidget(segments);
    auto *legend = new QWidget(context_block);
    legend->setObjectName("lingtai_kanban_legend_row");
    auto *legend_layout = new QGridLayout(legend);
    legend_grid_ = legend_layout;
    legend_layout->setContentsMargins(0, 0, 0, 0);
    legend_layout->setHorizontalSpacing(16);
    legend_layout->setVerticalSpacing(6);
    const auto legend_item = [&](const QString &name, std::int64_t value,
            const QColor &swatch) {
        auto *item = new QWidget(legend);
        auto *row = new QHBoxLayout(item);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        auto *dot = new QLabel(item);
        dot->setFixedSize(8, 8);
        dot->setStyleSheet(QStringLiteral(
            "background: %1; border-radius: 2px;").arg(setup_color_css(swatch)));
        auto *text = make_label(item,
            QStringLiteral("%1  %2").arg(name, comma_number(value)),
            "lingtai_kanban_legend", 10);
        color_text(text, tokens.muted_text);
        row->addWidget(dot, 0, Qt::AlignVCenter);
        row->addWidget(text);
        legend_items_.push_back(item);
        legend_layout->addWidget(item, 0, static_cast<int>(legend_items_.size()) - 1);
    };
    legend_item(QStringLiteral("System"), system_tokens, QColor(QStringLiteral("#0F3D32")));
    legend_item(QStringLiteral("Tools"), tools_tokens, QColor(QStringLiteral("#16785C")));
    legend_item(QStringLiteral("History"), history_tokens, QColor(QStringLiteral("#8FBFB0")));
    legend_item(QStringLiteral("Free"), free_tokens, QColor(QStringLiteral("#C5D4CE")));
    context_layout->addWidget(legend);
    left_layout->addWidget(context_block);

    left_layout->addWidget(section_heading(left, QStringLiteral("Presets"),
        "lingtai_kanban_heading_presets"));
    auto *presets = new QWidget(left);
    presets->setObjectName("lingtai_kanban_section_presets");
    auto *presets_layout = new QVBoxLayout(presets);
    presets_layout->setContentsMargins(0, 0, 0, 0);
    presets_layout->setSpacing(8);
    auto *active_row = new QWidget(presets);
    auto *active_layout = new QVBoxLayout(active_row);
    active_layout->setContentsMargins(0, 0, 0, 0);
    active_layout->setSpacing(4);
    auto *active_head = new QWidget(active_row);
    auto *active_head_layout = new QHBoxLayout(active_head);
    active_head_layout->setContentsMargins(0, 0, 0, 0);
    auto *active_label = make_label(active_head, QStringLiteral("Active preset"),
        "lingtai_kanban_preset_active_label", 10);
    color_text(active_label, tokens.muted_text);
    active_head_layout->addWidget(active_label);
    active_head_layout->addStretch(1);
    if (agent->presets.active_ref) {
        active_head_layout->addWidget(pill(active_head, QStringLiteral("Active"),
            QColor(QStringLiteral("#E7F4EF")), tokens.selection_accent,
            "lingtai_kanban_preset_badge"));
    }
    active_layout->addWidget(active_head);
    const auto active_ref = agent->presets.active_ref
        ? QString::fromStdString(*agent->presets.active_ref)
        : QStringLiteral("—");
    auto *active_name = make_label(active_row, active_ref,
        "lingtai_kanban_preset_active", 12, QFont::DemiBold);
    color_text(active_name, tokens.value_text);
    active_name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    active_layout->addWidget(active_name);
    presets_layout->addWidget(active_row);
    if (!agent->presets.allowed.empty()) {
        auto *allowed_heading = make_label(presets, QStringLiteral("Allowed presets"),
            "lingtai_kanban_preset_allowed_label", 10);
        color_text(allowed_heading, tokens.muted_text);
        presets_layout->addWidget(allowed_heading);
        for (const auto &allowed : agent->presets.allowed) {
            auto *row = new QWidget(presets);
            auto *row_layout = new QHBoxLayout(row);
            row_layout->setContentsMargins(0, 2, 0, 2);
            const auto mark = allowed.is_active ? QStringLiteral("✓") : QStringLiteral("•");
            auto *marker = make_label(row, mark, "lingtai_kanban_preset_mark", 12);
            color_text(marker, allowed.is_active
                ? tokens.selection_accent : tokens.muted_text);
            auto *ref = make_label(row, QString::fromStdString(allowed.ref),
                "lingtai_kanban_preset_ref", 12);
            color_text(ref, tokens.value_text);
            row_layout->addWidget(marker);
            row_layout->addWidget(ref, 1);
            auto *saved = make_label(row, QStringLiteral("Saved"),
                "lingtai_kanban_preset_source", 10);
            color_text(saved, tokens.muted_text);
            row_layout->addWidget(saved);
            presets_layout->addWidget(row);
        }
    }
    auto *open_presets = new QPushButton(QStringLiteral("Open presets"), presets);
    open_presets->setObjectName("lingtai_kanban_open_presets");
    open_presets->setFlat(true);
    open_presets->setCursor(Qt::PointingHandCursor);
    open_presets->setStyleSheet(QStringLiteral(
        "QPushButton { color: %1; border: none; background: transparent; "
        "text-align: left; padding: 0; }")
        .arg(setup_color_css(tokens.selection_accent)));
    QObject::connect(open_presets, &QPushButton::clicked, this,
        &KanbanPage::presets_requested);
    presets_layout->addWidget(open_presets, 0, Qt::AlignLeft);
    left_layout->addWidget(presets);

    left_layout->addWidget(section_heading(left, QStringLiteral("Capabilities"),
        "lingtai_kanban_heading_capabilities"));
    auto *caps = new QWidget(left);
    caps->setObjectName("lingtai_kanban_section_capabilities");
    auto *caps_layout = new QGridLayout(caps);
    caps_grid_ = caps_layout;
    caps_layout->setContentsMargins(0, 0, 0, 0);
    caps_layout->setHorizontalSpacing(6);
    caps_layout->setVerticalSpacing(6);
    if (agent->capabilities.empty()) {
        auto *none = make_label(caps, QStringLiteral("none"),
            "lingtai_kanban_capability_empty", 11);
        color_text(none, tokens.muted_text);
        caps_layout->addWidget(none, 0, 0);
    } else {
        for (auto index = 0; index != static_cast<int>(agent->capabilities.size());
                ++index) {
            auto *chip = pill(caps, QString::fromStdString(
                    agent->capabilities[static_cast<std::size_t>(index)]),
                setup_is_dark(palette()) ? tokens.header : QColor(QStringLiteral("#F1F3F2")),
                setup_is_dark(palette()) ? tokens.section_text : QColor(QStringLiteral("#4D6259")),
                "lingtai_kanban_capability");
            cap_pills_.push_back(chip);
            caps_layout->addWidget(chip, index / 4, index % 4);
        }
    }
    left_layout->addWidget(caps);

    left_layout->addWidget(section_heading(left, QStringLiteral("Token usage"),
        "lingtai_kanban_heading_tokens"));
    auto *token_block = new QWidget(left);
    token_block->setObjectName("lingtai_kanban_section_tokens");
    auto *token_layout = new QVBoxLayout(token_block);
    token_layout->setContentsMargins(0, 0, 0, 0);
    token_layout->setSpacing(4);
    const auto token_row = [&](const QString &label, const QString &value) {
        auto *row = new QWidget(token_block);
        auto *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 2, 0, 2);
        auto *k = make_label(row, label, "lingtai_kanban_token_label", 12);
        color_text(k, tokens.muted_text);
        auto *v = make_label(row, value, "lingtai_kanban_token_value", 12);
        color_text(v, tokens.value_text);
        row_layout->addWidget(k);
        row_layout->addStretch(1);
        row_layout->addWidget(v);
        token_layout->addWidget(row);
    };
    auto cached = comma_number(agent->tokens.cached);
    if (agent->tokens.input > 0) {
        cached += QStringLiteral("  %1%")
            .arg(QString::number(
                100.0 * static_cast<double>(agent->tokens.cached)
                    / static_cast<double>(agent->tokens.input),
                'f', 1));
    }
    token_row(QStringLiteral("Input"), comma_number(agent->tokens.input));
    token_row(QStringLiteral("Output"), comma_number(agent->tokens.output));
    token_row(QStringLiteral("Thinking"), comma_number(agent->tokens.thinking));
    token_row(QStringLiteral("Cached"), cached);
    if (agent->tokens.api_calls > 0) {
        auto *note = make_label(token_block,
            QStringLiteral("Current session: %1 tokens / API call")
                .arg(comma_number(agent->tokens.spend() / agent->tokens.api_calls)),
            "lingtai_kanban_token_note", 10);
        color_text(note, tokens.muted_text);
        token_layout->addWidget(note);
    }
    left_layout->addWidget(token_block);

    if (!agent->admin_fields.empty()) {
        left_layout->addWidget(section_heading(left, QStringLiteral("Authority"),
            "lingtai_kanban_heading_admin"));
        auto *admin = new QWidget(left);
        admin->setObjectName("lingtai_kanban_section_admin");
        auto *admin_layout = new QVBoxLayout(admin);
        admin_layout->setContentsMargins(0, 0, 0, 0);
        for (const auto &field : agent->admin_fields) {
            auto *row = new QWidget(admin);
            auto *row_layout = new QHBoxLayout(row);
            row_layout->setContentsMargins(0, 3, 0, 3);
            auto *k = make_label(row, QString::fromStdString(field.label),
                "lingtai_kanban_admin_label", 12);
            color_text(k, tokens.muted_text);
            row_layout->addWidget(k);
            row_layout->addStretch(1);
            const auto enabled = QString::fromStdString(field.value);
            const auto on = enabled.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
                || enabled.compare(QStringLiteral("enabled"), Qt::CaseInsensitive) == 0;
            row_layout->addWidget(pill(row,
                on ? QStringLiteral("Enabled") : enabled,
                on ? QColor(QStringLiteral("#E7F4EF")) : QColor(QStringLiteral("#F4E7E7")),
                on ? tokens.selection_accent : tokens.danger_text,
                "lingtai_kanban_admin_value"));
            admin_layout->addWidget(row);
        }
        left_layout->addWidget(admin);
    }
    columns_layout->addWidget(left, 0, 0);

    auto *right = new QWidget(columns);
    right->setObjectName("lingtai_kanban_right");
    right_column_ = right;
    shrink_width(right);
    auto *right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(16);

    right_layout->addWidget(section_heading(right, QStringLiteral("Network"),
        "lingtai_kanban_heading_network"));
    auto *network = new QWidget(right);
    auto *network_layout = new QVBoxLayout(network);
    network_layout->setContentsMargins(0, 0, 0, 0);
    network_layout->setSpacing(6);
    const auto net_row = [&](const QString &label, const QString &value,
            const char *name, const QColor &value_color) {
        auto *row = new QWidget(network);
        auto *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 2, 0, 2);
        auto *k = make_label(row, label, "lingtai_kanban_net_label", 12);
        color_text(k, tokens.muted_text);
        auto *v = make_label(row, value, name, 12);
        color_text(v, value_color);
        row_layout->addWidget(k);
        row_layout->addStretch(1);
        row_layout->addWidget(v);
        network_layout->addWidget(row);
    };
    const auto created = QString::fromStdString(board_.network_created);
    net_row(QStringLiteral("Created"),
        created.isEmpty() ? QStringLiteral("—") : pretty_started(created),
        "lingtai_kanban_network_created", tokens.value_text);
    const auto activity = QString::fromStdString(board_.activity_status);
    auto activity_label = display_state(activity);
    if (activity == QStringLiteral("suspend")) activity_label = QStringLiteral("Suspended");
    const auto activity_color = activity == QStringLiteral("suspend")
            || activity == QStringLiteral("suspended")
        ? tokens.danger_text : tokens.value_text;
    net_row(QStringLiteral("Status"), activity_label,
        "lingtai_kanban_stat_activity", activity_color);
    net_row(QStringLiteral("Suspended"), QString::number(board_.suspended),
        "lingtai_kanban_network_suspended",
        board_.suspended > 0 ? tokens.danger_text : tokens.value_text);
    auto *daemon_row = new QWidget(network);
    daemon_row->setObjectName("lingtai_kanban_section_daemons");
    auto *daemon_layout = new QHBoxLayout(daemon_row);
    daemon_layout->setContentsMargins(0, 2, 0, 2);
    auto *daemon_k = make_label(daemon_row, QStringLiteral("Daemons"),
        "lingtai_kanban_net_label", 12);
    color_text(daemon_k, tokens.muted_text);
    auto *daemon_v = make_label(daemon_row,
        QStringLiteral("running: %1\ntotal: %2")
            .arg(agent->daemons.running).arg(agent->daemons.total),
        "lingtai_kanban_daemons_value", 12);
    color_text(daemon_v, tokens.value_text);
    daemon_layout->addWidget(daemon_k);
    daemon_layout->addStretch(1);
    daemon_layout->addWidget(daemon_v);
    network_layout->addWidget(daemon_row);
    right_layout->addWidget(network);

    right_layout->addWidget(section_heading(right, QStringLiteral("All-agent usage"),
        "lingtai_kanban_heading_network_tokens"));
    auto *all = new QWidget(right);
    auto *all_layout = new QVBoxLayout(all);
    all_layout->setContentsMargins(0, 0, 0, 0);
    const auto all_row = [&](const QString &label, std::int64_t value) {
        auto *row = new QWidget(all);
        auto *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 3, 0, 3);
        auto *k = make_label(row, label, "lingtai_kanban_all_label", 12);
        color_text(k, tokens.muted_text);
        auto *v = make_label(row, comma_number(value), "lingtai_kanban_all_value", 12);
        color_text(v, tokens.value_text);
        row_layout->addWidget(k);
        row_layout->addStretch(1);
        row_layout->addWidget(v);
        all_layout->addWidget(row);
    };
    all_row(QStringLiteral("Input"), board_.network_tokens.input);
    all_row(QStringLiteral("Output"), board_.network_tokens.output);
    all_row(QStringLiteral("Thinking"), board_.network_tokens.thinking);
    all_row(QStringLiteral("Cached"), board_.network_tokens.cached);
    all_row(QStringLiteral("API calls"), board_.network_tokens.api_calls);
    right_layout->addWidget(all);

    right_layout->addWidget(section_heading(right, QStringLiteral("Agent tree"),
        "lingtai_kanban_heading_tree"));
    auto *tree = new QWidget(right);
    tree->setObjectName("lingtai_kanban_section_tree");
    auto *tree_layout = new QVBoxLayout(tree);
    tree_layout->setContentsMargins(0, 0, 0, 0);
    tree_layout->setSpacing(6);
    if (board_.tree_lines.empty()) {
        auto *empty = make_label(tree, QStringLiteral("No spawn tree yet."),
            "lingtai_kanban_tree_empty", 11);
        color_text(empty, tokens.muted_text);
        tree_layout->addWidget(empty);
    }
    for (const auto &line : board_.tree_lines) {
        auto *row = new QPushButton(tree);
        row->setObjectName("lingtai_kanban_tree_row");
        row->setFlat(true);
        row->setCursor(Qt::PointingHandCursor);
        row->setStyleSheet(QStringLiteral(
            "QPushButton { text-align: left; border: none; background: transparent; "
            "padding: 2px 0; }"));
        const auto text = QString::fromStdString(line);
        auto *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(8);
        auto leaf = text;
        leaf.remove(QChar(0x2502));
        leaf.remove(QStringLiteral("└ "));
        leaf.remove(QStringLiteral("├ "));
        leaf = leaf.trimmed();
        auto *disc = new AvatarDisc(row, 22);
        disc->set_name(leaf, avatar_fill_for(leaf), tokens.selection_accent);
        auto *label = make_label(row, text, "lingtai_kanban_tree_label", 12);
        color_text(label, tokens.value_text);
        row_layout->addWidget(disc);
        row_layout->addWidget(label, 1);
        for (const auto &node : board_.agents) {
            if (QString::fromStdString(node.display_name) == leaf
                    || path_text(node.directory_key) == leaf) {
                const auto key = path_text(node.directory_key);
                QObject::connect(row, &QPushButton::clicked, this,
                    [this, key] { choose_agent(key); });
                break;
            }
        }
        tree_layout->addWidget(row);
    }
    right_layout->addWidget(tree);
    columns_layout->addWidget(right, 0, 1);
    summary_layout->addWidget(columns);
    summary_layout->addStretch(1);

    auto *detail_header = new QWidget(detail_body_);
    detail_header->setObjectName("lingtai_kanban_detail_header");
    auto *detail_header_layout = new QVBoxLayout(detail_header);
    detail_header_layout->setContentsMargins(0, 0, 0, 8);
    detail_header_layout->setSpacing(8);
    auto *detail_back = new QPushButton(
        QStringLiteral("←  Back to summary"), detail_header);
    detail_back->setObjectName("lingtai_kanban_detail_back");
    detail_back->setFlat(true);
    detail_back->setCursor(Qt::PointingHandCursor);
    detail_back->setAccessibleName(QStringLiteral("Back to summary"));
    detail_back->setToolTip(QStringLiteral("Esc"));
    QObject::connect(detail_back, &QPushButton::clicked, this,
        &KanbanPage::show_summary);
    detail_header_layout->addWidget(detail_back, 0, Qt::AlignLeft);
    auto *detail_title = make_label(detail_header,
        QStringLiteral("Agent Detail"),
        "lingtai_kanban_inspector_title", 22, QFont::DemiBold);
    color_text(detail_title, tokens.value_text);
    detail_header_layout->addWidget(detail_title);
    detail_layout->addWidget(detail_header);

    auto *info = make_detail_section(detail_body_,
        QStringLiteral("Agent information"),
        "lingtai_kanban_section_info", tokens);
    auto *info_layout = qobject_cast<QVBoxLayout *>(info->layout());
    const auto add_info = [&](const QString &label, const QString &value) {
        if (value.trimmed().isEmpty()) return;
        info_layout->addWidget(detail_single_row(info, label, value, tokens));
    };
    add_info(QStringLiteral("Name"), field_value(agent->identity_fields, "name"));
    add_info(QStringLiteral("Nickname"),
        field_value(agent->identity_fields, "nickname"));
    add_info(QStringLiteral("ID"), field_value(agent->identity_fields, "id"));
    add_info(QStringLiteral("Address"),
        field_value(agent->identity_fields, "address"));
    add_info(QStringLiteral("State"),
        field_value(agent->identity_fields, "state"));
    add_info(QStringLiteral("Agent path"), path_text(agent->directory_path));
    add_info(QStringLiteral("Network root"), path_text(board_.network_root));
    if (!board_.orchestrator_path.empty()
            && board_.orchestrator_path != agent->directory_path) {
        add_info(QStringLiteral("Orchestrator path"),
            path_text(board_.orchestrator_path));
    }
    detail_layout->addWidget(info);

    detail_layout->addWidget(provider_usage_section(detail_body_,
        QStringLiteral("Main-agent token usage by provider"),
        "lingtai_kanban_section_providers", agent->providers, tokens));
    auto daemon_title = QStringLiteral(
        "Daemon token usage by provider / backend");
    if (agent->daemon_runs_total > agent->daemon_runs_scanned) {
        daemon_title += QStringLiteral(" (latest %1/%2 runs)")
            .arg(agent->daemon_runs_scanned)
            .arg(agent->daemon_runs_total);
    }
    detail_layout->addWidget(provider_usage_section(detail_body_,
        daemon_title,
        "lingtai_kanban_section_daemon_providers",
        agent->daemon_providers, tokens));

    KanbanTokenTotals combined;
    auto combined_empty = agent->providers.empty()
        && agent->daemon_providers.empty();
    for (const auto &row : agent->providers) add_token_totals(combined, row.totals);
    for (const auto &row : agent->daemon_providers) {
        add_token_totals(combined, row.totals);
    }
    if (!combined_empty) {
        auto combined_title = QStringLiteral("Combined totals (main + daemons)");
        if (agent->daemon_runs_total > agent->daemon_runs_scanned) {
            combined_title += QStringLiteral(" (main + recent daemon window)");
        }
        auto *totals = make_detail_section(detail_body_,
            combined_title,
            "lingtai_kanban_section_totals", tokens);
        auto *totals_layout = qobject_cast<QVBoxLayout *>(totals->layout());
        totals_layout->addWidget(detail_single_row(totals,
            QStringLiteral("input + output + thinking"),
            comma_number(combined.spend()), tokens));
        totals_layout->addWidget(detail_single_row(totals,
            QStringLiteral("cached"), comma_number(combined.cached), tokens));
        totals_layout->addWidget(detail_single_row(totals,
            QStringLiteral("miss"), comma_number(combined.miss()), tokens));
        totals_layout->addWidget(detail_single_row(totals,
            QStringLiteral("api_calls"), comma_number(combined.api_calls), tokens));
        if (combined.input > 0) {
            totals_layout->addWidget(detail_single_row(totals,
                QStringLiteral("cache hit rate"),
                cache_rate_text(combined.cached, combined.input),
                tokens));
        }
        detail_layout->addWidget(totals);
    }

    add_session_api_section(detail_layout, detail_body_,
        QStringLiteral("Current session API"),
        "lingtai_kanban_section_session_current",
        agent->current_session, tokens);
    add_session_api_section(detail_layout, detail_body_,
        QStringLiteral("Last session API"),
        "lingtai_kanban_section_session_last",
        agent->last_session, tokens);

    if (agent->context_stats.entries > 0) {
        auto *stats = make_detail_section(detail_body_,
            QStringLiteral("Current context statistics"),
            "lingtai_kanban_section_context_stats", tokens);
        auto *stats_layout = qobject_cast<QVBoxLayout *>(stats->layout());
        const auto &cs = agent->context_stats;
        stats_layout->addWidget(detail_single_row(stats,
            QStringLiteral("entries"), QString::number(cs.entries), tokens));
        stats_layout->addWidget(detail_single_row(stats,
            QStringLiteral("messages"),
            QStringLiteral("system:%1  assistant:%2  user:%3")
                .arg(cs.system_messages).arg(cs.assistant_messages)
                .arg(cs.user_messages),
            tokens));
        stats_layout->addWidget(detail_single_row(stats,
            QStringLiteral("text input / output"),
            QStringLiteral("%1 / %2").arg(cs.text_inputs).arg(cs.text_outputs),
            tokens));
        stats_layout->addWidget(detail_single_row(stats,
            QStringLiteral("tool calls / results"),
            QStringLiteral("%1 / %2").arg(cs.tool_calls).arg(cs.tool_results),
            tokens));
        if (!cs.tool_counts.empty()) {
            auto *tools_heading = make_label(stats,
                QStringLiteral("tools in context"),
                "lingtai_kanban_detail_k", 11);
            color_text(tools_heading, tokens.muted_text);
            stats_layout->addWidget(tools_heading);
            for (const auto &tool : cs.tool_counts) {
                stats_layout->addWidget(detail_fact_row(stats,
                    QString::fromStdString(tool.name),
                    QStringLiteral("calls:%1  results:%2")
                        .arg(comma_number(tool.calls),
                            comma_number(tool.results)),
                    {}, {}, tokens));
            }
        }
        detail_layout->addWidget(stats);
    }

    if (!agent->mcp_names.empty()) {
        auto *mcp = make_detail_section(detail_body_,
            QStringLiteral("MCP servers"),
            "lingtai_kanban_section_mcp", tokens);
        auto *mcp_layout = qobject_cast<QVBoxLayout *>(mcp->layout());
        for (const auto &mcp_name : agent->mcp_names) {
            auto *name_label = make_label(mcp, QString::fromStdString(mcp_name),
                "lingtai_kanban_mcp_name", 12);
            color_text(name_label, tokens.value_text);
            mcp_layout->addWidget(name_label);
        }
        detail_layout->addWidget(mcp);
    }

    auto *daemons = make_detail_section(detail_body_,
        QStringLiteral("Daemons"),
        "lingtai_kanban_detail_daemons", tokens);
    auto *daemons_layout = qobject_cast<QVBoxLayout *>(daemons->layout());
    daemons_layout->addWidget(detail_fact_row(daemons,
        QStringLiteral("running"), QString::number(agent->daemons.running),
        QStringLiteral("total"), QString::number(agent->daemons.total),
        tokens));
    if (agent->daemon_runs_total > agent->daemon_runs_scanned) {
        auto *note = make_label(daemons,
            QStringLiteral(
                "running state checked in latest %1 runs; total is directory-wide")
                .arg(agent->daemon_runs_scanned),
            "lingtai_kanban_daemons_window_note", 11);
        color_text(note, tokens.muted_text);
        daemons_layout->addWidget(note);
    }
    detail_layout->addWidget(daemons);

    auto *recent_main = make_detail_section(detail_body_,
        QStringLiteral("Recent calls — agent (last 100)"),
        "lingtai_kanban_section_recent", tokens);
    auto *recent_main_layout = qobject_cast<QVBoxLayout *>(recent_main->layout());
    if (agent->recent.empty()) {
        auto *empty = make_label(recent_main,
            QStringLiteral("No agent calls recorded yet."),
            "lingtai_kanban_recent_empty", 12);
        color_text(empty, tokens.muted_text);
        recent_main_layout->addWidget(empty);
    } else {
        recent_main_layout->addWidget(mono_row(recent_main,
            QStringLiteral("%1  %2  %3  %4  %5  %6  %7  %8  %9  %10")
                .arg(QStringLiteral("time"), -24)
                .arg(QStringLiteral("provider"), -10)
                .arg(QStringLiteral("model"), -24)
                .arg(QStringLiteral("input"), 10)
                .arg(QStringLiteral("output"), 10)
                .arg(QStringLiteral("thinking"), 10)
                .arg(QStringLiteral("cached"), 10)
                .arg(QStringLiteral("miss"), 10)
                .arg(QStringLiteral("cache%"), 7)
                .arg(QStringLiteral("endpoint")),
            "lingtai_kanban_recent_header", tokens, true));
        const auto separators = ledger_separator_labels(
            agent->recent, agent->molt_times_ms, agent->refresh_times_ms);
        for (int i = 0; i < static_cast<int>(agent->recent.size()); ++i) {
            recent_main_layout->addWidget(mono_row(recent_main,
                main_call_line(agent->recent[static_cast<std::size_t>(i)]),
                "lingtai_kanban_recent_row", tokens));
            add_separator_rows(recent_main_layout, recent_main, separators, i,
                tokens);
        }
    }
    detail_layout->addWidget(recent_main);

    auto *recent_daemons = make_detail_section(detail_body_,
        QStringLiteral("Recent calls — daemons (last 100)"),
        "lingtai_kanban_section_recent_daemons", tokens);
    auto *recent_daemon_layout =
        qobject_cast<QVBoxLayout *>(recent_daemons->layout());
    if (agent->daemon_recent.empty()) {
        auto *empty = make_label(recent_daemons,
            QStringLiteral("No daemon calls recorded yet."),
            "lingtai_kanban_recent_daemons_empty", 12);
        color_text(empty, tokens.muted_text);
        recent_daemon_layout->addWidget(empty);
    } else {
        recent_daemon_layout->addWidget(mono_row(recent_daemons,
            QStringLiteral("%1  %2  %3  %4  %5  %6  %7  %8  %9  %10  %11  %12")
                .arg(QStringLiteral("time"), -24)
                .arg(QStringLiteral("daemon"), -10)
                .arg(QStringLiteral("state"), -8)
                .arg(QStringLiteral("backend"), -10)
                .arg(QStringLiteral("model"), -24)
                .arg(QStringLiteral("input"), 10)
                .arg(QStringLiteral("output"), 10)
                .arg(QStringLiteral("thinking"), 10)
                .arg(QStringLiteral("cached"), 10)
                .arg(QStringLiteral("miss"), 10)
                .arg(QStringLiteral("cache%"), 7)
                .arg(QStringLiteral("endpoint")),
            "lingtai_kanban_recent_daemons_header", tokens, true));
        for (const auto &entry : agent->daemon_recent) {
            recent_daemon_layout->addWidget(mono_row(recent_daemons,
                daemon_call_line(entry),
                "lingtai_kanban_recent_daemon_row", tokens));
        }
    }
    detail_layout->addWidget(recent_daemons);
    detail_layout->addStretch(1);

    if (detail_open_) show_detail();
    else show_summary();
    relayout();
    for (auto *child : findChildren<QWidget *>()) {
        child->installEventFilter(this);
    }
}

} // namespace lingtai::desktop
