#include "ui/conversation_surface.h"

#include "base/basic_types.h"
#include "attachment_thumbnail.h"
#include "conversation_session.h"
#include "styles/palette.h"

#include <QtCore/QEvent>
#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QVariant>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>
#include <QtGui/QFontMetricsF>
#include <QtGui/QImage>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtGui/QPalette>
#include <QtGui/QPen>
#include <QtGui/QPixmap>
#include <QtGui/QResizeEvent>
#include <QtGui/QShowEvent>
#include <QtGui/QTextBlock>
#include <QtGui/QTextBlockFormat>
#include <QtGui/QTextCharFormat>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtGui/QTextDocumentFragment>
#include <QtGui/QTextFormat>
#include <QtGui/QTextFrame>
#include <QtGui/QTextImageFormat>
#include <QtGui/QTextLayout>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QScrollBar>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace lingtai::desktop {
namespace {

constexpr auto kDocumentMargin = 8;
constexpr auto kMessageEdgeMargin = 12;
constexpr auto kMessageTopMargin = 4;
// One readable rhythm inside a same-Agent group and a larger break between
// groups. The current frame's bottom margin owns the gap to its next sibling.
constexpr auto kWithinGroupBottomMargin = 0;
constexpr auto kBetweenGroupBottomMargin = 22;
constexpr qint64 kSameAgentGroupMaxSeconds = 5 * 60;
constexpr auto kMessageRatio = 0.72;
constexpr auto kHumanMessageRatio = 0.60;
constexpr auto kMinMessageWidth = 160;
constexpr auto kHumanMinMessageWidth = 64;
constexpr auto kMessageAbsoluteCap = 560;
constexpr auto kHumanMessageAbsoluteCap = 540;
constexpr auto kReadingColumnMax = 1600;
constexpr auto kNarrowViewportWidth = 480;
constexpr auto kBubbleHPadding = 11;
constexpr auto kHumanBubbleHPadding = 15;
constexpr auto kHumanBubbleVPadding = 11;
constexpr auto kHumanBubbleRadius = 12;
constexpr auto kReactionChipHPadding = 7;
constexpr auto kReactionChipVPadding = 3;
constexpr auto kReactionChipGap = 4;
constexpr auto kReactionRowTopGap = 6;
constexpr auto kReactionRowBottomInset = 6;
constexpr auto kReactionRowSideInset = 8;
// The Human timestamp sits below the bubble with a small quiet gap; the frame
// reserves enough bottom space for the 12px metadata line and the break to
// the next turn (~22% tighter than the prior 36px rhythm).
constexpr auto kTimestampGap = 4;
constexpr auto kHumanMessageBottomMargin = 28;
constexpr auto kMessageAvatarDiameter = 34;
constexpr auto kMessageAvatarGap = 8;
constexpr auto kMessageBlockProperty = QTextFormat::UserProperty + 1;
// The outgoing message timestamp rides on the sibling frame so paintEvent can
// place it outside the body-only bubble without a visible header block.
constexpr auto kMessageTimestampProperty = QTextFormat::UserProperty + 2;
// Incoming frames paint an avatar only when they start a visible same-Agent
// group; continuation frames retain the shared text lane but leave it empty.
constexpr auto kMessageAvatarProperty = QTextFormat::UserProperty + 3;
constexpr auto kEmptyAvatarDiameter = 32;
constexpr auto kEmptyAvatarGap = 10;
constexpr auto kEmptyTitleGap = 6;
constexpr auto kEmptyStateHeadroom = 28;
constexpr auto kEmptyStateTitleSize = 15;
constexpr auto kSelectAgentIllustrationSize = 96;
constexpr auto kSelectAgentIllustrationGap = 14;
constexpr auto kSelectAgentTitleSize = 17;
// The leading lazy-history banner's vertical breathing room around the single
// centered muted line. It is deliberately small so the banner reads as a quiet
// inline note above the first revealed message rather than a tall separator.
constexpr auto kBannerTopMargin = 6;
constexpr auto kBannerBottomMargin = 8;
// The day separator's vertical breathing room around the centered label and its
// horizontal rules, kept generous so the divider reads between message groups.
constexpr auto kDayTopMargin = 18;
constexpr auto kDayBottomMargin = 14;
// The day divider label rides on the first message frame of each calendar day;
// paintEvent draws the centered rules in the gap above that frame.
constexpr auto kDaySeparatorProperty = QTextFormat::UserProperty + 6;
constexpr auto kParagraphBottomMargin = 10;
// Body reading uses 14px with a tight proportional line box (~2pt less
// inter-line space than the prior 146% pair at this size).
constexpr auto kBodyLineHeightPercent = 132;
constexpr auto kListBottomMargin = 3;
// Pull the first body block up under the Agent name/time header. 31 closes
// ~25% more of the name→body gap so the header and body read as one message,
// without colliding with the 15px sender line; inter-message frame margins
// still own message spacing.
constexpr auto kHeaderToBodyOverlap = 31;
constexpr auto kCodeBlockProperty = QTextFormat::UserProperty + 4;
// Direction is frame metadata, not body alignment: Human body text stays left
// aligned while the frame's margins and painter keep the bubble on the right.
constexpr auto kMessageOutgoingProperty = QTextFormat::UserProperty + 5;
constexpr auto kMessageIdProperty = QTextFormat::UserProperty + 8;
constexpr auto kMessageReactionsProperty = QTextFormat::UserProperty + 9;
// Marks LLM-detail frames so paintEvent never draws a Human/Agent bubble
// around them (they share the root sibling list with message frames).
constexpr auto kVerboseFrameProperty = QTextFormat::UserProperty + 10;
constexpr auto kAttachmentBlockProperty = QTextFormat::UserProperty + 11;
constexpr auto kAttachmentMessageIdProperty = QTextFormat::UserProperty + 12;
constexpr auto kAttachmentIndexProperty = QTextFormat::UserProperty + 13;

// The symmetric gutter of the centered reading column for a viewport: fixed
// 12px edge gutters until the column max, then a shared share of the excess.
int reading_column_margins(int viewport_width) {
    return qMax(12, (viewport_width - kReadingColumnMax) / 2 + 12);
}

// The shared lane maximum for a given viewport: the widest any one message may
// be. At the explicit narrow breakpoint the lane is near-full (the viewport
// minus the two fixed edge gutters) instead of the ordinary ratio; otherwise
// the lane lives inside the centered reading column at the ordinary 72% ratio,
// with the existing 160px lower bound and the absolute readable cap, never
// wider than the column.
int message_block_width(int viewport_width) {
    if (viewport_width < kNarrowViewportWidth) {
        return qMax(0, viewport_width - 2 * kMessageEdgeMargin);
    }
    const auto column = qMin(viewport_width, kReadingColumnMax);
    return qBound(
        kMinMessageWidth,
        int(column * kMessageRatio),
        kMessageAbsoluteCap);
}

// Human bubbles use the same wider centered rail but retain the accepted
// readable prose cap; widening the rail moves its right anchor outward instead
// of stretching long Human text. On narrow panes the common near-full lane
// remains the usable fallback.
int human_message_block_width(int viewport_width) {
    if (viewport_width < kNarrowViewportWidth) {
        return qMax(0, viewport_width - 2 * kMessageEdgeMargin);
    }
    const auto column = qMin(viewport_width, kReadingColumnMax);
    return qMin(
        kHumanMessageAbsoluteCap,
        qMax(kHumanMinMessageWidth, int(column * kHumanMessageRatio)));
}

QTextBlockFormat message_block_format(
        bool outgoing,
        int viewport_width,
        int width) {
    auto format = QTextBlockFormat();
    // Reading direction and lane position are independent. Both message bodies
    // read left-to-right; asymmetric margins below anchor the Human lane right.
    format.setAlignment(Qt::AlignLeft);
    // Derive one outer gutter (the centered reading column's offset plus the
    // fixed edge gutter) and one inner remainder, then cross-assign them so
    // incoming stays left-anchored and outgoing right-anchored inside the same
    // column rather than centering each message individually. Each message
    // keeps the shared outer anchor while its inner edge follows its own
    // content-driven width.
    const auto column = qMin(viewport_width, kReadingColumnMax);
    const auto outer = (viewport_width - column) / 2 + kMessageEdgeMargin;
    const auto avatar_lane = kMessageAvatarDiameter + kMessageAvatarGap;
    const auto text_outer = outgoing ? outer : outer + avatar_lane;
    const auto inner = qMax(outer, viewport_width - width - text_outer);
    format.setLeftMargin(outgoing ? inner : text_outer);
    format.setRightMargin(outgoing ? outer : inner);
    format.setTopMargin(kMessageTopMargin);
    format.setBottomMargin(0);
    format.setLineHeight(
        kBodyLineHeightPercent, QTextBlockFormat::ProportionalHeight);
    format.setProperty(kMessageBlockProperty, true);
    return format;
}

QTextCharFormat sender_format(bool outgoing) {
    auto format = QTextCharFormat();
    format.setForeground(outgoing ? st::historyTextOutFg
                                  : st::historyTextInFg);
    auto font = format.font();
    font.setPixelSize(15);
    font.setWeight(QFont::DemiBold);
    format.setFont(font);
    return format;
}

[[nodiscard]] bool conversation_canvas_is_light() {
    return st::windowBg->c.lightness() >= 128;
}

QColor body_reading_color(bool outgoing) {
    if (conversation_canvas_is_light()) {
        return QColor(QStringLiteral("#26282B"));
    }
    return outgoing ? st::historyTextOutFg->c : st::historyTextInFg->c;
}

QColor secondary_reading_color() {
    return conversation_canvas_is_light()
        ? QColor(QStringLiteral("#8A8F98"))
        : st::msgServiceFg->c;
}

// Soft mint Human bubble for both themes. Light and dark are equal companions
// over windowBg — never Telegram's solid Send/active msgOutBg.
QColor human_bubble_color() {
    return conversation_canvas_is_light()
        ? QColor(QStringLiteral("#EEF7F3"))
        : QColor(QStringLiteral("#2A4038"));
}

QColor attachment_card_color() {
    return conversation_canvas_is_light()
        ? QColor(QStringLiteral("#F7F8FA"))
        : QColor(QStringLiteral("#202B36"));
}

QColor attachment_card_border_color() {
    return conversation_canvas_is_light()
        ? QColor(QStringLiteral("#D8DDE5"))
        : QColor(QStringLiteral("#405063"));
}

QString human_file_size(std::uint64_t bytes) {
    constexpr auto kib = std::uint64_t{1024};
    constexpr auto mib = kib * 1024;
    constexpr auto gib = mib * 1024;
    if (bytes >= gib) {
        return QStringLiteral("%1 GB").arg(double(bytes) / double(gib), 0, 'f', 1);
    }
    if (bytes >= mib) {
        return QStringLiteral("%1 MB").arg(double(bytes) / double(mib), 0, 'f', 1);
    }
    if (bytes >= kib) {
        return QStringLiteral("%1 KB").arg(double(bytes) / double(kib), 0, 'f', 1);
    }
    return bytes == 1
        ? QStringLiteral("1 byte")
        : QStringLiteral("%1 bytes").arg(bytes);
}

QString attachment_type_label(const DirectConversationAttachment &attachment) {
    if (attachment.media_kind == AttachmentMediaKind::image) {
        return QStringLiteral("Image");
    }
    const auto suffix = QString::fromStdString(
        std::filesystem::path(attachment.display_filename).extension().string())
        .mid(1).toUpper();
    return suffix.isEmpty() ? QStringLiteral("File")
                            : QStringLiteral("%1 file").arg(suffix);
}

QTextCharFormat attachment_name_format(bool outgoing, const QString &tooltip) {
    auto format = QTextCharFormat();
    format.setForeground(body_reading_color(outgoing));
    auto font = format.font();
    font.setPixelSize(14);
    font.setWeight(QFont::DemiBold);
    format.setFont(font);
    format.setToolTip(tooltip);
    return format;
}

QTextCharFormat attachment_action_format(
        bool outgoing,
        const QString &message_id,
        std::size_t index,
        bool reveal) {
    auto format = QTextCharFormat();
    format.setForeground(st::windowActiveTextFg->c);
    auto font = format.font();
    font.setPixelSize(12);
    format.setFont(font);
    format.setFontUnderline(true);
    format.setAnchor(true);
    auto url = QUrl(QStringLiteral("lingtai-attachment-action://action"));
    auto query = QUrlQuery();
    query.addQueryItem(QStringLiteral("message"), message_id);
    query.addQueryItem(QStringLiteral("index"), QString::number(index));
    query.addQueryItem(QStringLiteral("reveal"), reveal ? QStringLiteral("1")
                                                        : QStringLiteral("0"));
    url.setQuery(query);
    format.setAnchorHref(url.toString(QUrl::FullyEncoded));
    format.setToolTip(reveal ? QStringLiteral("Reveal this attachment in Finder")
                             : QStringLiteral("Open this attachment"));
    return format;
}

struct ParsedAttachmentAction final {
    QString message_id;
    std::size_t attachment_index = 0;
    bool reveal = false;
};

[[nodiscard]] std::optional<ParsedAttachmentAction>
parse_attachment_action_href(const QString &href) {
    const auto url = QUrl(href);
    if (url.scheme() != QStringLiteral("lingtai-attachment-action")
        || url.host() != QStringLiteral("action")
        || !url.path().isEmpty()
        || !url.userInfo().isEmpty()
        || url.port() != -1) {
        return std::nullopt;
    }
    const auto query = QUrlQuery(url);
    if (query.queryItems().size() != 3) return std::nullopt;
    const auto message_id = query.queryItemValue(
        QStringLiteral("message"), QUrl::FullyDecoded);
    auto index_ok = false;
    const auto index = query.queryItemValue(QStringLiteral("index"))
        .toULongLong(&index_ok);
    const auto reveal = query.queryItemValue(QStringLiteral("reveal"));
    if (message_id.isEmpty() || !index_ok
        || index > std::numeric_limits<std::size_t>::max()
        || (reveal != QStringLiteral("0")
            && reveal != QStringLiteral("1"))) {
        return std::nullopt;
    }
    return ParsedAttachmentAction{
        message_id,
        static_cast<std::size_t>(index),
        reveal == QStringLiteral("1"),
    };
}

QStringList reaction_chip_labels(const MessageReactions &bag) {
    auto labels = QStringList();
    for (const auto &entry : bag.list) {
        auto label = QString::fromStdString(reaction_glyph(entry.id));
        if (entry.count > 1) {
            label += QStringLiteral(" %1").arg(entry.count);
        }
        labels.push_back(label);
    }
    return labels;
}

int reaction_row_height(const QFontMetricsF &metrics) {
    return int(std::ceil(metrics.height())) + 2 * kReactionChipVPadding;
}

QColor reaction_chip_fill(bool outgoing) {
    if (!outgoing) {
        return st::windowBgRipple->c;
    }
    // Stronger mint than the bubble so chips read on top in both themes.
    return conversation_canvas_is_light()
        ? QColor(QStringLiteral("#D7EBE3"))
        : QColor(QStringLiteral("#45665A"));
}

QTextCharFormat secondary_format() {
    auto format = QTextCharFormat();
    format.setForeground(secondary_reading_color());
    auto font = format.font();
    font.setPixelSize(12);
    font.setWeight(QFont::Normal);
    format.setFont(font);
    return format;
}

QTextCharFormat verbose_thinking_format() {
    auto format = QTextCharFormat();
    // Same theme-aware body ink as Agent prose (light #26282B / dark
    // historyTextInFg), not a raw palette peek that can diverge on theme swap.
    format.setForeground(body_reading_color(false));
    auto font = format.font();
    font.setPixelSize(12);
    font.setWeight(QFont::Normal);
    font.setItalic(true);
    format.setFont(font);
    return format;
}

QTextCharFormat verbose_tool_format() {
    auto format = QTextCharFormat();
    // Link/online blue: light #168acd, night palette #6ab3f3.
    format.setForeground(st::windowActiveTextFg->c);
    auto font = format.font();
    font.setPixelSize(12);
    font.setWeight(QFont::Normal);
    format.setFont(font);
    return format;
}

QTextCharFormat verbose_footer_format() {
    // Muted secondary: light #8A8F98, dark msgServiceFg (#708499).
    return secondary_format();
}

QTextBlockFormat verbose_block_format(int viewport_width) {
    auto format = QTextBlockFormat();
    format.setAlignment(Qt::AlignLeft);
    const auto gutter = reading_column_margins(viewport_width);
    format.setLeftMargin(gutter + kMessageAvatarDiameter + kMessageAvatarGap);
    format.setRightMargin(gutter);
    format.setTopMargin(1);
    format.setBottomMargin(1);
    return format;
}

// Cursor that can insert a new root-level sibling frame. Document End and
// root->lastCursorPosition() often sit *inside* the last child frame, so
// insertBlock/insertFrame would nest and Human bubbles paint beside verbose
// text on the same row.
[[nodiscard]] QTextCursor root_append_cursor(QTextDocument *document) {
    auto *root = document->rootFrame();
    QTextCursor cursor(document);
    cursor.movePosition(QTextCursor::End);
    // Walk out of any nested frames until the cursor is in the root.
    for (int guard = 0; guard < 8; ++guard) {
        auto *frame = cursor.currentFrame();
        if (!frame || frame == root) {
            break;
        }
        const auto after = frame->lastPosition() + 1;
        if (after > cursor.position()) {
            cursor.setPosition(after);
            continue;
        }
        // Already at the frame end character — step into the parent.
        auto *parent = frame->parentFrame();
        if (!parent) {
            break;
        }
        cursor = QTextCursor(parent);
        cursor.setPosition(frame->lastPosition() + 1);
    }
    if (cursor.currentFrame() != root) {
        cursor = root->lastCursorPosition();
    }
    return cursor;
}

[[nodiscard]] QTextFrameFormat verbose_frame_format() {
    auto format = QTextFrameFormat();
    format.setBorder(0);
    format.setPadding(0);
    format.setMargin(0);
    format.setBottomMargin(2);
    format.setBackground(Qt::transparent);
    format.setProperty(kVerboseFrameProperty, true);
    return format;
}

// Insert one verbose line as its own root sibling frame so it never shares a
// QTextFrame with a Human/Agent message (which caused side-by-side overlap).
void insert_verbose_line(
        QTextDocument *document,
        int viewport_width,
        const QTextCharFormat &format,
        const QString &text,
        int bottom_margin = 1) {
    if (text.isEmpty()) {
        return;
    }
    auto cursor = root_append_cursor(document);
    auto frame_format = verbose_frame_format();
    frame_format.setBottomMargin(bottom_margin);
    auto *frame = cursor.insertFrame(frame_format);
    cursor = frame->firstCursorPosition();
    auto block = verbose_block_format(viewport_width);
    cursor.setBlockFormat(block);
    cursor.insertText(text, format);
}

[[nodiscard]] int timestamp_compare(
        const std::string &left, const std::string &right) {
    const auto parsed_left = QDateTime::fromString(
        QString::fromStdString(left), Qt::ISODate);
    const auto parsed_right = QDateTime::fromString(
        QString::fromStdString(right), Qt::ISODate);
    if (parsed_left.isValid() && parsed_right.isValid()) {
        if (parsed_left == parsed_right) {
            return 0;
        }
        return parsed_left < parsed_right ? -1 : 1;
    }
    if (left == right) {
        return 0;
    }
    return left < right ? -1 : 1;
}

QString format_verbose_tool_timestamp(const std::string &raw) {
    if (raw.empty()) {
        return {};
    }
    const auto parsed = QDateTime::fromString(
        QString::fromStdString(raw), Qt::ISODate);
    if (!parsed.isValid()) {
        return {};
    }
    return parsed.toLocalTime().toString(QStringLiteral("HH:mm"));
}

constexpr int kMaxVerboseRenderLines = 36;

struct VerboseRenderState final {
    std::size_t event_index = 0;
    std::optional<SessionTokenUsage> pending_usage;
    std::string pending_group;
    const ConversationSessionEntry *prev_visible = nullptr;
    int rendered_lines = 0;
    bool render_capped = false;
};

void flush_verbose_token_footer(
        QTextCursor &cursor,
        int viewport_width,
        VerboseRenderState &state) {
    if (!state.pending_usage.has_value()) {
        return;
    }
    const auto footer = format_token_usage_footer(*state.pending_usage);
    state.pending_usage.reset();
    state.pending_group.clear();
    if (footer.empty()) {
        return;
    }
    insert_verbose_line(
        cursor.document(),
        viewport_width,
        verbose_footer_format(),
        QStringLiteral("• %1").arg(QString::fromStdString(footer)),
        6);
}

void append_verbose_separator(
        QTextCursor &cursor, int viewport_width) {
    insert_verbose_line(
        cursor.document(),
        viewport_width,
        secondary_format(),
        QStringLiteral("  %1").arg(QString(24, QChar(0x2508))),
        4);
}

void insert_verbose_text(
        QTextCursor &cursor,
        int viewport_width,
        const QTextCharFormat &format,
        const QString &text) {
    auto normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QString(QChar::LineSeparator));
    normalized.replace(QChar::LineFeed, QString(QChar::LineSeparator));
    normalized.replace(QChar::CarriageReturn, QString(QChar::LineSeparator));
    const auto lines = normalized.split(QChar::LineSeparator);
    for (int index = 0; index != lines.size(); ++index) {
        if (lines[index].isEmpty()) {
            continue;
        }
        insert_verbose_line(
            cursor.document(), viewport_width, format, lines[index]);
    }
}

void append_verbose_event_line(
        QTextCursor &cursor,
        int viewport_width,
        const ConversationSessionEntry &entry,
        ConversationVerboseLevel level,
        VerboseRenderState &state) {
    if (entry.type == "llm_response") {
        if (entry.token_usage.has_value()
                && level != ConversationVerboseLevel::off) {
            if (!state.pending_group.empty()
                    && state.pending_group != entry.api_call_id) {
                flush_verbose_token_footer(cursor, viewport_width, state);
            }
            state.pending_usage = entry.token_usage;
            state.pending_group = entry.api_call_id;
        }
        return;
    }
    if (!conversation_verbose_event_visible(entry, level)) {
        return;
    }
    if (state.rendered_lines >= kMaxVerboseRenderLines) {
        state.render_capped = true;
        return;
    }
    if (state.pending_usage.has_value()) {
        const auto grouped = entry.type == "thinking"
            || entry.type == "diary"
            || entry.type == "text_input"
            || entry.type == "text_output"
            || entry.type == "tool_call"
            || entry.type == "tool_result";
        if (!grouped || entry.api_call_id != state.pending_group) {
            flush_verbose_token_footer(cursor, viewport_width, state);
        }
    }
    if (conversation_api_group_separator_before(state.prev_visible, entry)
            && level == ConversationVerboseLevel::extended) {
        append_verbose_separator(cursor, viewport_width);
    }

    const auto body = conversation_verbose_event_body(entry, level);
    const auto is_tool = entry.type == "tool_call" || entry.type == "tool_result";
    auto line = QString::fromStdString(body);
    if (is_tool) {
        line = QStringLiteral("[%1] %2")
            .arg(QString::fromStdString(entry.type))
            .arg(line);
        const auto stamp = format_verbose_tool_timestamp(entry.timestamp);
        if (!stamp.isEmpty()) {
            line = stamp + QStringLiteral(" ") + line;
        }
    }

    if (entry.type == "tool_call" && !entry.reasoning.empty()) {
        insert_verbose_text(
            cursor,
            viewport_width,
            verbose_thinking_format(),
            QStringLiteral("[Reasoning] %1")
                .arg(QString::fromStdString(entry.reasoning)));
    }
    insert_verbose_text(
        cursor,
        viewport_width,
        is_tool ? verbose_tool_format() : verbose_thinking_format(),
        QStringLiteral("• %1").arg(line));

    ++state.rendered_lines;
    state.prev_visible = &entry;
}

void append_verbose_events_until(
        QTextCursor &cursor,
        int viewport_width,
        const std::vector<ConversationSessionEntry> &events,
        ConversationVerboseLevel level,
        VerboseRenderState &state,
        const std::optional<std::string> &until_timestamp) {
    if (level == ConversationVerboseLevel::off) {
        return;
    }
    while (state.event_index < events.size()) {
        if (state.render_capped) {
            break;
        }
        const auto &entry = events[state.event_index];
        if (until_timestamp.has_value()
                && timestamp_compare(entry.timestamp, *until_timestamp) > 0) {
            break;
        }
        append_verbose_event_line(cursor, viewport_width, entry, level, state);
        ++state.event_index;
    }
}

// Message timestamps are the legible secondary rung of the reading surface.
// Empty-state prompts and the lazy-history banner keep the quieter 12px
// secondary format above, while per-message time scales with the body.
QTextCharFormat message_metadata_format() {
    auto format = QTextCharFormat();
    format.setForeground(secondary_reading_color());
    auto font = format.font();
    font.setPixelSize(13);
    font.setWeight(QFont::Normal);
    format.setFont(font);
    return format;
}

// The renderer-only presentation of one message's stored timestamp: the raw
// ISO string is parsed with Qt's ISO parser and shown in the local wall clock
// as HH:mm for the message header/time and a calendar day for the day divider.
// An invalid or empty raw value yields empty strings so the raw input is never
// shown verbatim.
struct PresentationTime {
    QString time;
    QString day;
    QDate date;
};

bool within_same_agent_interval(
        const std::string &before,
        const std::string &after) {
    const auto first = QDateTime::fromString(
        QString::fromStdString(before), Qt::ISODate);
    const auto second = QDateTime::fromString(
        QString::fromStdString(after), Qt::ISODate);
    if (!first.isValid() || !second.isValid()) {
        return false;
    }
    const auto seconds = first.secsTo(second);
    return seconds >= 0 && seconds <= kSameAgentGroupMaxSeconds;
}

PresentationTime present_timestamp(const std::string &raw) {
    if (raw.empty()) {
        return {};
    }
    const auto parsed = QDateTime::fromString(
        QString::fromStdString(raw), Qt::ISODate);
    if (!parsed.isValid()) {
        return {};
    }
    const auto local = parsed.toLocalTime();
    return {
        local.toString(QStringLiteral("HH:mm")),
        local.toString(QStringLiteral("yyyy/MM/dd")),
        local.date(),
    };
}

// The centered day divider label: Today / Yesterday for the local calendar,
// otherwise a long-form month-day-year like Slack and other chat apps.
QString format_day_label(const QDate &date) {
    if (!date.isValid()) {
        return {};
    }
    const auto today = QDate::currentDate();
    if (date == today) {
        return QStringLiteral("Today");
    }
    if (date == today.addDays(-1)) {
        return QStringLiteral("Yesterday");
    }
    return QLocale(QLocale::English, QLocale::UnitedStates)
        .toString(date, QStringLiteral("MMMM d, yyyy"));
}

QFont day_separator_font() {
    auto font = QApplication::font();
    font.setPixelSize(13);
    font.setWeight(QFont::DemiBold);
    return font;
}

void paint_day_separator_in_gap(
        QPainter &painter,
        qreal gap_top,
        qreal gap_bottom,
        const QString &label,
        int viewport_width,
        int v_offset,
        const QRectF &clip) {
    if (label.isEmpty() || gap_bottom <= gap_top) {
        return;
    }
    const auto gutter = reading_column_margins(viewport_width);
    const auto column_left = qreal(gutter);
    const auto column_right = qreal(viewport_width - gutter);
    const auto center_y = (gap_top + gap_bottom) / 2.0 - v_offset;
    const auto font = day_separator_font();
    const QFontMetricsF metrics(font);
    const auto text_width = metrics.horizontalAdvance(label);
    const auto text_height = metrics.height();
    const auto pad_h = 10.0;
    const auto center_x = (column_left + column_right) / 2.0;
    const auto text_left = center_x - text_width / 2.0;
    const auto text_right = center_x + text_width / 2.0;
    const auto text_bg = QRectF(
        text_left - pad_h,
        center_y - text_height / 2.0 - 2.0,
        text_width + 2.0 * pad_h,
        text_height + 4.0);
    const auto paint_rect = QRectF(
        column_left, center_y - text_height / 2.0 - 4.0,
        column_right - column_left, text_height + 8.0);
    if (!paint_rect.intersects(clip)) {
        return;
    }

    const auto line_color = secondary_reading_color();
    const auto bg = st::windowBg->c;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    QPen pen(line_color);
    pen.setWidthF(1.0);
    painter.setPen(pen);
    painter.drawLine(
        QPointF(column_left, center_y),
        QPointF(text_left - pad_h, center_y));
    painter.drawLine(
        QPointF(text_right + pad_h, center_y),
        QPointF(column_right, center_y));
    painter.fillRect(text_bg, bg);
    painter.setPen(line_color);
    painter.setFont(font);
    painter.drawText(text_bg, Qt::AlignCenter, label);
    painter.restore();
}

void paint_day_separators(
        QPainter &painter,
        const QPaintEvent *event,
        const QTextDocument *document,
        const QAbstractTextDocumentLayout *document_layout,
        int viewport_width,
        int v_offset) {
    const auto clip = QRectF(event->rect());
    auto previous_bottom = 0.0;
    for (auto it = document->rootFrame()->begin(); !it.atEnd(); ++it) {
        if (it.currentFrame()) {
            continue;
        }
        const auto block = it.currentBlock();
        if (!block.isValid()) {
            continue;
        }
        previous_bottom = qMax(
            previous_bottom,
            document_layout->blockBoundingRect(block).bottom());
    }
    for (auto *frame : document->rootFrame()->childFrames()) {
        const auto label = frame->frameFormat()
            .property(kDaySeparatorProperty).toString();
        const auto frame_rect = document_layout->frameBoundingRect(frame);
        if (!label.isEmpty()) {
            paint_day_separator_in_gap(
                painter,
                previous_bottom,
                frame_rect.top(),
                label,
                viewport_width,
                v_offset,
                clip);
        }
        previous_bottom = frame_rect.bottom();
    }
}

QTextCharFormat body_format(bool outgoing) {
    auto format = QTextCharFormat();
    format.setForeground(body_reading_color(outgoing));
    auto font = QApplication::font();
    font.setPixelSize(14);
    font.setWeight(QFont::Normal);
    font.setStyleHint(QFont::SansSerif);
    format.setFont(font);
    return format;
}

// The accepted safe-markdown character formats, each derived from the message
// body's base format so the direction colors stay intact while the run becomes
// visually distinct from the plain 14px body.
QTextCharFormat emphasized_text_format(const QTextCharFormat &base) {
    auto format = base;
    auto font = format.font();
    font.setWeight(QFont::Bold);
    format.setFont(font);
    return format;
}

QTextCharFormat heading_text_format(const QTextCharFormat &base) {
    auto format = base;
    auto font = format.font();
    font.setPixelSize(17);
    font.setWeight(QFont::DemiBold);
    format.setFont(font);
    return format;
}

QColor code_surface_color() {
    // Distinct from windowBg / Human bubble so fenced and inline code read as
    // chrome, not as the surrounding message canvas. Light and dark both use
    // opaque elevated fills (not a faint white wash) so the panel stays clear
    // on night `#17212b` the same way `#E4E7EB` does on light.
    return st::windowBg->c.lightness() >= 128
        ? QColor(QStringLiteral("#E4E7EB"))
        : QColor(QStringLiteral("#242F3D"));
}

QColor code_surface_border_color() {
    return st::windowBg->c.lightness() >= 128
        ? QColor(QStringLiteral("#C5CAD3"))
        : QColor(QStringLiteral("#3D4A5C"));
}

QFont code_font(const QFont &base) {
    // Style hints alone often keep the body family (e.g. Open Sans) on macOS;
    // pin the system fixed font so fenced/inline code is visibly monospace.
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const auto size = base.pixelSize() > 0 ? base.pixelSize() : 14;
    font.setPixelSize(std::max(12, size - 1));
    font.setWeight(QFont::Normal);
    font.setFixedPitch(true);
    font.setStyleHint(QFont::Monospace, QFont::PreferDefault);
    return font;
}

QTextCharFormat code_text_format(const QTextCharFormat &base) {
    auto format = base;
    format.setFont(code_font(base.font()));
    format.setBackground(code_surface_color());
    return format;
}

// Fenced lines rely on paintEvent for the rounded panel; keep the char run
// transparent so glyph ink does not fight a mismatched per-glyph wash.
QTextCharFormat fenced_code_text_format(const QTextCharFormat &base) {
    auto format = base;
    format.setFont(code_font(base.font()));
    format.setBackground(Qt::transparent);
    return format;
}

QTextCharFormat quote_text_format(const QTextCharFormat &base) {
    auto format = base;
    auto font = format.font();
    font.setItalic(true);
    format.setFont(font);
    return format;
}

// One line scanner for the whole message body: the stored raw text is rendered
// through the accepted safe-markdown contract. Control markers disappear from
// the displayed plain text, formatted runs become real QTextCharFormat runs in
// the existing QTextDocument, raw HTML is never interpreted, and no resource,
// image, or URL load ever enters the document. Each non-delimiter body line is
// inserted into its own real, unmarked QTextBlock cloned from the caller's
// continuation block format; ``` fence delimiters only toggle the fence state
// and create no block. Everything outside the accepted marker set, including
// literal HTML, is inserted verbatim with insertText.
void insert_markdown_body(
        QTextCursor &cursor,
        const QString &body,
        const QTextCharFormat &base,
        const QTextBlockFormat &continuation,
        bool first_line_in_current_block = false) {
    const auto bold = emphasized_text_format(base);
    const auto heading = heading_text_format(base);
    const auto code = code_text_format(base);
    const auto fenced_code = fenced_code_text_format(base);
    const auto quote = quote_text_format(base);
    const QRegularExpression technical(
        QStringLiteral(R"((\.[A-Za-z0-9_-]+(?:[/.][A-Za-z0-9_.-]+)+|[A-Za-z_][A-Za-z0-9_-]*(?:\.[A-Za-z0-9_-]+){2,}|--[A-Za-z0-9-]+))"));

    const auto insert_plain = [&](const QString &text,
                                  const QTextCharFormat &plain) {
        auto offset = 0;
        auto matches = technical.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            if (match.capturedStart() > offset) {
                cursor.insertText(text.mid(
                    offset, match.capturedStart() - offset), plain);
            }
            cursor.insertText(match.captured(), code);
            offset = match.capturedEnd();
        }
        cursor.insertText(text.mid(offset), plain);
    };

    const auto insert_inline = [&](const QString &line,
                                   const QTextCharFormat &plain) {
        auto index = 0;
        while (index < line.size()) {
            const auto bold_open = line.indexOf(QStringLiteral("**"), index);
            const auto code_open = line.indexOf(QChar('`'), index);
            const auto link_open = line.indexOf(QChar('['), index);
            auto at = -1;
            auto kind = 0;
            const auto consider = [&](int candidate, int candidate_kind) {
                if (candidate >= 0 && (at < 0 || candidate < at)) {
                    at = candidate;
                    kind = candidate_kind;
                }
            };
            consider(bold_open, 1);
            consider(code_open, 2);
            consider(link_open, 3);
            if (at < 0) {
                insert_plain(line.mid(index), plain);
                break;
            }
            if (at > index) {
                insert_plain(line.mid(index, at - index), plain);
            }
            if (kind == 2) {
                const auto close = line.indexOf(QChar('`'), at + 1);
                if (close < 0) {
                    insert_plain(line.mid(at), plain);
                    break;
                }
                cursor.insertText(line.mid(at + 1, close - at - 1), code);
                index = close + 1;
            } else if (kind == 3) {
                const auto close = line.indexOf(QStringLiteral("]("), at + 1);
                const auto end = close >= 0
                    ? line.indexOf(QChar(')'), close + 2)
                    : -1;
                if (close < 0 || end < 0) {
                    insert_plain(line.mid(at), plain);
                    break;
                }
                auto link_format = base;
                auto link_font = link_format.font();
                link_font.setUnderline(true);
                link_format.setFont(link_font);
                link_format.setForeground(QColor(0x1a, 0x73, 0xe8));
                link_format.setAnchor(true);
                link_format.setAnchorHref(
                    line.mid(close + 2, end - close - 2));
                cursor.insertText(line.mid(at + 1, close - at - 1), link_format);
                index = end + 1;
            } else {
                const auto close = line.indexOf(QStringLiteral("**"), at + 2);
                if (close < 0) {
                    insert_plain(line.mid(at), plain);
                    break;
                }
                cursor.insertText(line.mid(at + 2, close - at - 2), bold);
                index = close + 2;
            }
        }
    };

    auto in_code_fence = false;
    auto paragraph_break = false;
    const QRegularExpression heading_line(QStringLiteral(R"(^(#{1,6})\s+(.+)$)"));
    const QRegularExpression unordered(QStringLiteral(R"(^[-*]\s+(.+)$)"));
    const QRegularExpression ordered(QStringLiteral(R"(^(\d+[.)])\s+(.+)$)"));
    for (const auto &line : body.split(QChar::LineSeparator)) {
        if (line.startsWith(QStringLiteral("```"))) {
            in_code_fence = !in_code_fence;
            continue;
        }
        if (line.trimmed().isEmpty()) {
            if (cursor.block().isValid() && !cursor.block().text().isEmpty()) {
                auto previous = cursor.block().blockFormat();
                previous.setBottomMargin(kParagraphBottomMargin);
                cursor.setBlockFormat(previous);
            }
            paragraph_break = true;
            continue;
        }

        if (first_line_in_current_block) {
            first_line_in_current_block = false;
        } else {
            cursor.insertBlock(continuation);
        }
        auto block = continuation;
        block.setLineHeight(
            kBodyLineHeightPercent, QTextBlockFormat::ProportionalHeight);
        block.setTopMargin(0);
        block.setBottomMargin(0);
        if (paragraph_break) {
            paragraph_break = false;
        }

        const auto heading_match = heading_line.match(line);
        const auto unordered_match = unordered.match(line);
        const auto ordered_match = ordered.match(line);
        if (in_code_fence) {
            block.setAlignment(Qt::AlignLeft);
            block.setLeftMargin(block.leftMargin() + 10);
            block.setRightMargin(block.rightMargin() + 10);
            block.setTopMargin(5);
            block.setBottomMargin(5);
            // Keep a nonempty semantic brush on the block for document/a11y
            // inspection, but let paintEvent own the visible rounded surface.
            block.setBackground(QColor(0, 0, 0, 1));
            block.setProperty(kCodeBlockProperty, true);
            cursor.setBlockFormat(block);
            cursor.insertText(line, fenced_code);
        } else if (heading_match.hasMatch()) {
            block.setAlignment(Qt::AlignLeft);
            block.setTopMargin(6);
            block.setBottomMargin(6);
            cursor.setBlockFormat(block);
            cursor.insertText(heading_match.captured(2), heading);
        } else if (unordered_match.hasMatch() || ordered_match.hasMatch()) {
            block.setAlignment(Qt::AlignLeft);
            block.setLeftMargin(block.leftMargin() + 16);
            block.setTextIndent(-14);
            block.setBottomMargin(kListBottomMargin);
            block.setLineHeight(
                kBodyLineHeightPercent, QTextBlockFormat::ProportionalHeight);
            cursor.setBlockFormat(block);
            if (ordered_match.hasMatch()) {
                cursor.insertText(ordered_match.captured(1) + QChar(' '), base);
                insert_inline(ordered_match.captured(2), base);
            } else {
                cursor.insertText(QStringLiteral("• "), base);
                insert_inline(unordered_match.captured(1), base);
            }
        } else if (line.startsWith(QStringLiteral("> "))) {
            cursor.setBlockFormat(block);
            insert_inline(line.mid(2), quote);
        } else {
            cursor.setBlockFormat(block);
            insert_inline(line, base);
        }
    }
}

// The plain-state vertical anchor lives on the document root frame's top
// margin rather than on the first block: Qt's first-block layout does not
// honor a first block's own topMargin and places the line at the frame's top
// edge. Drop that margin whenever a real conversation is rebuilt or the plain
// state clears, so empty-state spacing can never leak into message layout.
void clear_plain_state_anchor(QTextDocument *document) {
    auto frame_format = document->rootFrame()->frameFormat();
    frame_format.setTopMargin(0);
    document->rootFrame()->setFrameFormat(frame_format);
}

// Apply the plain-state lane formatting to a whole document: symmetric
// reading-column gutters on the sides, horizontal center, and a root-frame
// top margin that anchors the first line a third of the way down the
// viewport, with the placeholder tone carried by the same secondary format
// messages use.
void apply_plain_state_formatting(
        QTextDocument *document,
        int viewport_width,
        int viewport_height) {
    const auto gutter = reading_column_margins(viewport_width);
    const auto line_height = QFontMetricsF(secondary_format().font()).height();
    QTextBlockFormat format;
    format.setAlignment(Qt::AlignCenter);
    format.setLeftMargin(gutter);
    format.setRightMargin(gutter);
    format.setTopMargin(0);
    auto cursor = QTextCursor(document);
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(format);
    cursor.select(QTextCursor::Document);
    cursor.mergeCharFormat(secondary_format());

    auto frame_format = document->rootFrame()->frameFormat();
    frame_format.setTopMargin(
        qMax(0.0, (viewport_height - line_height) / 3.0));
    document->rootFrame()->setFrameFormat(frame_format);
}

QTextCharFormat empty_state_title_format() {
    auto format = QTextCharFormat();
    format.setForeground(st::msgServiceFg);
    auto font = format.font();
    font.setPixelSize(kEmptyStateTitleSize);
    font.setWeight(QFont::Normal);
    format.setFont(font);
    return format;
}

QTextCharFormat select_agent_title_format() {
    auto format = QTextCharFormat();
    format.setForeground(st::dialogsNameFg);
    auto font = format.font();
    font.setPixelSize(kSelectAgentTitleSize);
    font.setWeight(QFont::DemiBold);
    format.setFont(font);
    return format;
}

// Product avatar for the no-selection empty state. The asset ships on a solid
// black studio canvas; near-black pixels are cleared so the chat backdrop shows
// through on both light and dark themes.
QPixmap select_agent_illustration(int size) {
    static const auto source = []() -> QPixmap {
        auto image = QImage(QStringLiteral(
            ":/lingtai/conversation/select-agent-avatar.png"));
        if (image.isNull()) {
            return {};
        }
        image = image.convertToFormat(QImage::Format_ARGB32);
        for (auto y = 0; y != image.height(); ++y) {
            auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (auto x = 0; x != image.width(); ++x) {
                const auto pixel = line[x];
                if (qRed(pixel) < 18 && qGreen(pixel) < 18
                        && qBlue(pixel) < 18) {
                    line[x] = qRgba(0, 0, 0, 0);
                }
            }
        }
        return QPixmap::fromImage(std::move(image));
    }();
    if (source.isNull() || size <= 0) {
        return {};
    }
    return source.scaled(
        size,
        size,
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation);
}

// The small empty-state Agent avatar: a quiet muted circle carrying the
// Agent's own initial, painted with the same shared palette tokens the roster
// rows use so the empty conversation reads as the same partner identity.
QPixmap empty_state_avatar(const QString &initial, int diameter) {
    auto pixmap = QPixmap(diameter, diameter);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(st::windowBgActive->c);
    painter.drawEllipse(0, 0, diameter, diameter);
    auto font = QFont();
    font.setPixelSize(std::max(14, diameter / 2));
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(QColor(Qt::white));
    painter.drawText(QRect(0, 0, diameter, diameter), Qt::AlignCenter, initial);
    painter.end();
    return pixmap;
}

// Blue/azure accent wash for drag-select. The base hue is Qt's live native
// Highlight role (the macOS selection color), read fresh on every paint, not
// windowBgActive or any other conversation-surface token — a translucent
// windowBgActive wash reads as a weak tint of the same teal/green family as
// the bubbles it sits over, especially in dark mode. Coverage after
// compositing is ~38% in light, ~53% in dark: the darker canvas needs more
// of the accent to show through before a selection reads as unmistakable.
[[nodiscard]] QColor text_selection_wash_color() {
    auto wash = QApplication::palette().color(QPalette::Active, QPalette::Highlight);
    wash.setAlpha(conversation_canvas_is_light() ? 97 : 136);
    return wash;
}

[[nodiscard]] QTextFrame *frame_owning_block(
        QTextDocument *document,
        const QTextBlock &block) {
    if (!document || !block.isValid()) {
        return nullptr;
    }
    const auto position = block.position();
    for (auto *frame : document->rootFrame()->childFrames()) {
        if (position >= frame->firstPosition()
                && position <= frame->lastPosition()) {
            return frame;
        }
    }
    return document->rootFrame();
}

// Paint selection behind the glyphs only (cursorToX → natural text span), not
// across the full message lane width Qt would fill with QPalette::Highlight.
void paint_glyph_tight_selection(
        QPainter &painter,
        QTextDocument *document,
        const QTextCursor &cursor,
        int h_offset,
        int v_offset,
        const QRect &clip) {
    if (!document || !cursor.hasSelection()) {
        return;
    }
    const auto sel_start = cursor.selectionStart();
    const auto sel_end = cursor.selectionEnd();
    if (sel_start >= sel_end) {
        return;
    }
    auto *document_layout = document->documentLayout();
    if (!document_layout) {
        return;
    }
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(text_selection_wash_color());
    for (auto block = document->findBlock(sel_start);
            block.isValid() && block.position() < sel_end;
            block = block.next()) {
        auto *block_layout = block.layout();
        if (!block_layout || block_layout->lineCount() <= 0) {
            continue;
        }
        auto *frame = frame_owning_block(document, block);
        if (!frame) {
            continue;
        }
        const auto frame_origin =
            document_layout->frameBoundingRect(frame).topLeft();
        // Exclude the block separator so a whole-line select stops at the last
        // glyph instead of extending through the trailing paragraph break.
        const auto block_pos = block.position();
        const auto local_start = std::max(0, sel_start - block_pos);
        const auto local_end = std::min(
            std::max(0, block.length() - 1),
            sel_end - block_pos);
        if (local_start >= local_end) {
            continue;
        }
        for (auto i = 0; i != block_layout->lineCount(); ++i) {
            const auto line = block_layout->lineAt(i);
            const auto line_start = line.textStart();
            const auto line_end = line_start + line.textLength();
            const auto from = std::max(local_start, line_start);
            const auto to = std::min(local_end, line_end);
            if (from >= to) {
                continue;
            }
            const auto x1 = line.cursorToX(from);
            const auto x2 = line.cursorToX(to);
            auto glyph = QRectF(
                std::min(x1, x2),
                line.y(),
                std::abs(x2 - x1),
                line.height())
                .translated(block_layout->position())
                .translated(frame_origin);
            glyph.translate(-h_offset, -v_offset);
            if (!glyph.intersects(QRectF(clip))) {
                continue;
            }
            painter.drawRoundedRect(glyph.adjusted(-1, 0, 1, 0), 3, 3);
        }
    }
    painter.restore();
}

[[nodiscard]] QColor message_hover_wash_color() {
    // Same mint wash in both themes; keep it translucent so Agent rows stay on
    // the canvas and neither theme floods the pane with opaque bubble chrome.
    auto wash = human_bubble_color();
    wash.setAlpha(150);
    return wash;
}

// Visible message content bounds in document coordinates (union of laid-out
// glyph runs), matching paintEvent's text_bounds construction.
[[nodiscard]] QRectF message_text_bounds_doc(
        QTextFrame *frame,
        QAbstractTextDocumentLayout *document_layout) {
    if (!frame || !document_layout) {
        return {};
    }
    const auto frame_origin =
        document_layout->frameBoundingRect(frame).topLeft();
    auto text_bounds = QRectF();
    for (auto block = frame->begin(); !block.atEnd(); ++block) {
        const auto current_block = block.currentBlock();
        if (!current_block.isValid()) {
            continue;
        }
        const auto *block_layout = current_block.layout();
        for (auto i = 0; i != block_layout->lineCount(); ++i) {
            const auto line = block_layout->lineAt(i);
            const auto line_bounds = line.naturalTextRect()
                .translated(block_layout->position())
                .translated(frame_origin);
            text_bounds = text_bounds.isNull()
                ? line_bounds
                : text_bounds.united(line_bounds);
        }
    }
    return text_bounds;
}

// Vertical span for the hover wash. Human uses the painted bubble top/bottom
// (not the taller frame rect that also covers day/timestamp margins); Agent
// uses the text bounds expanded by Human bubble vertical padding.
[[nodiscard]] QRectF message_hover_vertical_span(
        QTextFrame *frame,
        QAbstractTextDocumentLayout *document_layout) {
    const auto text_bounds = message_text_bounds_doc(frame, document_layout);
    if (text_bounds.isNull()) {
        return {};
    }
    const auto outgoing = frame->frameFormat()
        .property(kMessageOutgoingProperty)
        .toBool();
    if (!outgoing) {
        // Match Human bubble vertical padding so Agent hover has the same
        // breathing room above and below the text block.
        return text_bounds.adjusted(
            0, -kHumanBubbleVPadding, 0, kHumanBubbleVPadding);
    }
    // Same geometry as the Human bubble painter: text ± bubble padding, right
    // edge from the first line's lane rect, optional reaction chips inside.
    const auto frame_origin =
        document_layout->frameBoundingRect(frame).topLeft();
    QTextBlock first_valid_block;
    for (auto block = frame->begin(); !block.atEnd(); ++block) {
        const auto current_block = block.currentBlock();
        if (current_block.isValid() && current_block.layout()
                && current_block.layout()->lineCount() > 0) {
            first_valid_block = current_block;
            break;
        }
    }
    auto bubble = text_bounds.adjusted(
        -kHumanBubbleHPadding,
        -kHumanBubbleVPadding,
        kHumanBubbleHPadding,
        kHumanBubbleVPadding);
    if (first_valid_block.isValid()) {
        const auto *first_layout = first_valid_block.layout();
        const auto lane_right = first_layout->lineAt(0).rect()
            .translated(first_layout->position())
            .translated(frame_origin)
            .right();
        bubble.setRight(lane_right + kHumanBubbleHPadding);
    }
    const auto reaction_labels = frame->frameFormat()
        .property(kMessageReactionsProperty)
        .toStringList();
    if (!reaction_labels.isEmpty()) {
        auto chip_font = secondary_format().font();
        chip_font.setPixelSize(13);
        const auto chip_row = reaction_row_height(QFontMetricsF(chip_font));
        bubble.setBottom(
            bubble.bottom() + kReactionRowTopGap + chip_row
            + kReactionRowBottomInset);
    }
    return bubble;
}

void paint_message_hover_row(
        QPainter &painter,
        QTextDocument *document,
        const QString &message_id,
        int viewport_width,
        int h_offset,
        int v_offset,
        const QRect &clip) {
    if (!document || message_id.isEmpty()) {
        return;
    }
    auto *document_layout = document->documentLayout();
    if (!document_layout) {
        return;
    }
    for (auto *frame : document->rootFrame()->childFrames()) {
        if (frame->frameFormat().property(kMessageIdProperty).toString()
                != message_id) {
            continue;
        }
        auto span = message_hover_vertical_span(frame, document_layout);
        if (span.isNull()) {
            return;
        }
        span.translate(-h_offset, -v_offset);
        // Full-width row wash. Vertical span already includes Human-bubble
        // padding (Agent) or the painted bubble bounds (Human).
        const auto row = QRectF(
            0,
            span.top(),
            viewport_width,
            span.height());
        if (!row.intersects(QRectF(clip))) {
            return;
        }
        painter.save();
        painter.setPen(Qt::NoPen);
        painter.setBrush(message_hover_wash_color());
        painter.drawRect(row);
        painter.restore();
        return;
    }
}

} // namespace

ConversationSurface::ConversationSurface(QWidget *parent)
: QTextEdit(parent) {
    setReadOnly(true);
    setUndoRedoEnabled(false);
    setMinimumWidth(0);
    // The chat backdrop and bubbles are painted in paintEvent, so the Qt
    // viewport must not paint its own solid background on top of them.
    auto transparent_palette = palette();
    transparent_palette.setBrush(QPalette::Base, QBrush(Qt::transparent));
    transparent_palette.setBrush(QPalette::Window, QBrush(Qt::transparent));
    // Selection geometry is painted by paintEvent (glyph-tight). Keep Highlight
    // transparent so Qt does not fill the full message lane underneath.
    // HighlightedText is copied by value here, so it goes stale across a live
    // theme switch; refresh_chrome() re-applies both roles from the current
    // ink so a later dark<->light change does not leave selected text on the
    // wrong contrast.
    transparent_palette.setColor(QPalette::Highlight, Qt::transparent);
    transparent_palette.setColor(QPalette::HighlightedText, st::windowFg->c);
    setPalette(transparent_palette);
    viewport()->setAutoFillBackground(false);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent, false);
    viewport()->setMouseTracking(true);
    viewport()->installEventFilter(this);
    setMouseTracking(true);
    document()->setDocumentMargin(kDocumentMargin);
}

void ConversationSurface::set_plain_state(const QString &text) {
    if (text == last_plain_state_
            && !select_agent_prompt_active_
            && !empty_state_active_) {
        return;
    }
    disarm_attachment_action();
    empty_state_active_ = false;
    select_agent_prompt_active_ = false;
    select_agent_main_name_.clear();
    them_.clear();
    last_plain_state_ = text;
    last_messages_.clear();
    last_reactions_.clear();
    presentation_revision_valid_ = false;
    accessible_attachment_names_.clear();
    setAccessibleDescription(QStringLiteral(
        "The current direct conversation, read-only."));
    // A plain (selection/no-route/empty) state has no lazy history, so the
    // render-time window resets to the full initial tail.
    history_offset_ = 0;
    clear_hovered_message();
    setPlainText(text);
    if (!text.isEmpty()) {
        apply_plain_state_formatting(
            document(), viewport()->width(), viewport()->height());
    } else {
        clear_plain_state_anchor(document());
    }
    last_layout_width_ = int(viewport()->width() / 8) * 8;
}

void ConversationSurface::set_select_agent_prompt(const QString &main_agent_name) {
    auto text = QStringLiteral(
        "Select an agent\n"
        "Choose an agent from the sidebar to view its conversation.");
    if (!main_agent_name.trimmed().isEmpty()) {
        text += QStringLiteral("\nYour main agent is %1.")
            .arg(main_agent_name.trimmed());
    }
    // Keep the no-selection prompt on the plain-state path so a later
    // set_conversation() rebuild is identical to the pre-illustration flow.
    // The design illustration is painted in paintEvent while this prompt is
    // active (see select_agent_prompt_active_).
    if (select_agent_prompt_active_
            && select_agent_main_name_ == main_agent_name
            && last_plain_state_ == text) {
        return;
    }
    disarm_attachment_action();
    select_agent_main_name_ = main_agent_name;
    setAccessibleDescription(QStringLiteral(
        "Select an Agent to view a direct conversation."));
    select_agent_prompt_active_ = true;
    empty_state_active_ = false;
    them_.clear();
    last_messages_.clear();
    last_reactions_.clear();
    presentation_revision_valid_ = false;
    accessible_attachment_names_.clear();
    history_offset_ = 0;
    last_plain_state_ = text;
    setPlainText(text);
    // Leave room above the title for the painted illustration.
    apply_plain_state_formatting(
        document(), viewport()->width(), viewport()->height());
    auto frame_format = document()->rootFrame()->frameFormat();
    frame_format.setTopMargin(std::max(
        frame_format.topMargin(),
        qreal(kSelectAgentIllustrationSize + kSelectAgentIllustrationGap
            + kEmptyStateHeadroom)));
    document()->rootFrame()->setFrameFormat(frame_format);
    auto cursor = QTextCursor(document());
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    cursor.mergeCharFormat(select_agent_title_format());
    last_layout_width_ = int(viewport()->width() / 8) * 8;
    update();
}

bool ConversationSurface::same_core_content(
        const std::vector<DirectConversationMessage> &messages,
        const std::unordered_map<std::string, MessageReactions> &reactions)
        const {
    if (last_messages_.size() != messages.size()
        || last_reactions_.size() != reactions.size()) {
        return false;
    }
    for (auto index = std::size_t{0}; index != messages.size(); ++index) {
        const auto &before = last_messages_[index];
        const auto &after = messages[index];
        if (before.id != after.id || before.outgoing != after.outgoing
            || before.timestamp != after.timestamp
            || before.text != after.text
            || before.attachments.size() != after.attachments.size()) {
            return false;
        }
        for (auto attachment_index = std::size_t{0};
                attachment_index != before.attachments.size();
                ++attachment_index) {
            const auto &left = before.attachments[attachment_index];
            const auto &right = after.attachments[attachment_index];
            if (left.display_filename != right.display_filename
                || left.byte_size != right.byte_size
                || left.media_kind != right.media_kind
                || left.device_id != right.device_id
                || left.inode_id != right.inode_id) {
                return false;
            }
        }
    }
    for (const auto &[key, after] : reactions) {
        const auto found = last_reactions_.find(key);
        if (found == last_reactions_.end()
            || found->second.list.size() != after.list.size()) {
            return false;
        }
        for (auto index = std::size_t{0}; index != after.list.size(); ++index) {
            const auto &left = found->second.list[index];
            const auto &right = after.list[index];
            if (left.id != right.id || left.count != right.count
                || left.source != right.source
                || left.reactor != right.reactor) {
                return false;
            }
        }
    }
    return true;
}

bool ConversationSurface::same_session_events(
        const std::vector<ConversationSessionEntry> &session_events) const {
    if (last_session_events_.size() != session_events.size()) {
        return false;
    }
    for (auto index = std::size_t{0}; index != session_events.size(); ++index) {
        const auto &before = last_session_events_[index];
        const auto &after = session_events[index];
        if (before.timestamp != after.timestamp
            || before.type != after.type
            || before.body != after.body
            || before.api_call_id != after.api_call_id
            || before.reasoning != after.reasoning
            || before.token_usage.has_value() != after.token_usage.has_value()) {
            return false;
        }
        if (before.token_usage.has_value()
            && (before.token_usage->input != after.token_usage->input
                || before.token_usage->output != after.token_usage->output
                || before.token_usage->cached != after.token_usage->cached
                || before.token_usage->api_duration_ms
                    != after.token_usage->api_duration_ms
                || before.token_usage->estimated != after.token_usage->estimated)) {
            return false;
        }
    }
    return true;
}

bool ConversationSurface::same_content(
        const std::vector<DirectConversationMessage> &messages,
        const std::unordered_map<std::string, MessageReactions> &reactions,
        const std::vector<ConversationSessionEntry> &session_events)
        const {
    return same_core_content(messages, reactions)
        && same_session_events(session_events);
}

void ConversationSurface::set_conversation(
        const QString &them,
        const std::vector<DirectConversationMessage> &messages,
        const std::unordered_map<std::string, MessageReactions> &reactions,
        const std::vector<ConversationSessionEntry> &session_events) {
    if (them == them_ && same_content(messages, reactions, session_events)
            && verbose_level_ == last_rendered_verbose_level_) {
        return;
    }
    disarm_attachment_action();
    const auto core_unchanged = them == them_
        && same_core_content(messages, reactions);
    const auto same_conversation_nonshrinking = !last_messages_.empty()
        && them == them_
        && messages.size() >= last_messages_.size();
    them_ = them;
    last_messages_ = messages;
    last_reactions_ = reactions;
    last_session_events_ = session_events;
    last_plain_state_.clear();
    select_agent_prompt_active_ = false;
    select_agent_main_name_.clear();
    empty_state_active_ = messages.empty();
    auto attachment_names = QStringList();
    for (const auto &message : messages) {
        for (const auto &attachment : message.attachments) {
            attachment_names.push_back(
                QString::fromStdString(attachment.display_filename));
        }
    }
    setAccessibleDescription(attachment_names.isEmpty()
        ? QStringLiteral("The current direct conversation, read-only.")
        : QStringLiteral("The current direct conversation, read-only. "
              "Attachments in message order: %1")
              .arg(attachment_names.join(QStringLiteral(", "))));
    accessible_attachment_names_ = attachment_names;
    if (!core_unchanged && !same_conversation_nonshrinking) {
        // Reset the render-time window to the initial chronological tail
        // when the conversation changes or rows disappear. An append keeps
        // every older slice the human already revealed without adding a
        // second history cursor or truncating the cached model.
        const auto page = history_page_size();
        history_offset_ = int(messages.size() > std::size_t(page)
            ? messages.size() - std::size_t(page)
            : std::size_t{0});
    }
    clear_hovered_message();
    if (messages.empty()) {
        rebuild_empty_state();
    } else {
        rebuild_document();
    }
    last_rendered_verbose_level_ = verbose_level_;
    last_layout_width_ = int(viewport()->width() / 8) * 8;
}

void ConversationSurface::set_conversation(
        const QString &them,
        const std::vector<DirectConversationMessage> &messages,
        const std::unordered_map<std::string, MessageReactions> &reactions,
        const std::vector<ConversationSessionEntry> &session_events,
        const ConversationPresentationRevision &revision) {
    if (presentation_revision_valid_
            && them == them_
            && revision == presentation_revision_
            && verbose_level_ == last_rendered_verbose_level_) {
        return;
    }
    const auto pure_append = presentation_revision_valid_
        && them == them_
        && verbose_level_ == ConversationVerboseLevel::off
        && last_rendered_verbose_level_ == ConversationVerboseLevel::off
        && revision.append_from_history == presentation_revision_.history
        && revision.append_from == last_messages_.size()
        && revision.reactions == presentation_revision_.reactions
        && revision.session_events == presentation_revision_.session_events
        && session_events.empty()
        && messages.size() > revision.append_from;
    if (pure_append && append_conversation_suffix(
            them, messages, reactions, revision.append_from)) {
        presentation_revision_ = revision;
        presentation_revision_valid_ = true;
        return;
    }

    // Any revision gap or incompatible presentation input uses the established
    // complete rebuild. The deep comparison here is reached only for a real
    // independent change, never for an ambient unchanged tick.
    presentation_revision_valid_ = false;
    set_conversation(them, messages, reactions, session_events);
    presentation_revision_ = revision;
    presentation_revision_valid_ = true;
}

bool ConversationSurface::append_conversation_suffix(
        const QString &them,
        const std::vector<DirectConversationMessage> &messages,
        const std::unordered_map<std::string, MessageReactions> &reactions,
        std::size_t append_from) {
    if (append_from == 0 || append_from != last_messages_.size()
            || append_from >= messages.size() || rebuild_in_progress_) {
        return false;
    }

    // Build only one boundary seed plus the proven new suffix with the same
    // renderer, then transplant each suffix frame into the retained document.
    // The seed gives the first appended row exact day/group continuity without
    // copying or rescanning the complete authoritative history.
    auto suffix_messages = std::vector<DirectConversationMessage>();
    suffix_messages.reserve(messages.size() - append_from + 1);
    suffix_messages.push_back(messages[append_from - 1]);
    suffix_messages.insert(suffix_messages.end(),
        messages.begin() + static_cast<std::ptrdiff_t>(append_from),
        messages.end());
    auto suffix_reactions = std::unordered_map<std::string, MessageReactions>();
    for (const auto &message : suffix_messages) {
        const auto found = reactions.find(message.id);
        if (found != reactions.end()) {
            suffix_reactions.emplace(found->first, found->second);
        }
    }

    ConversationSurface suffix_surface;
    suffix_surface.setAttribute(Qt::WA_DontShowOnScreen);
    suffix_surface.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    suffix_surface.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    suffix_surface.resize(
        viewport()->width() + 2 * suffix_surface.frameWidth(),
        std::max(64, viewport()->height()));
    suffix_surface.show();
    suffix_surface.set_conversation(
        them, suffix_messages, suffix_reactions, {});
    const auto source_frames =
        suffix_surface.document()->rootFrame()->childFrames();
    if (source_frames.size() != static_cast<qsizetype>(suffix_messages.size())) {
        return false;
    }

    auto *scrollbar = verticalScrollBar();
    const auto previous_scroll = scrollbar->value();
    const auto was_at_bottom = !wheel_gesture_active_
        && previous_scroll >= scrollbar->maximum();
    const auto old_frames = document()->rootFrame()->childFrames();
    if (old_frames.isEmpty()) return false;

    // Install image resources before the copied fragments reference them.
    for (auto index = append_from; index != messages.size(); ++index) {
        for (auto attachment_index = std::size_t{0};
                attachment_index != messages[index].attachments.size();
                ++attachment_index) {
            const auto resource = QUrl(QStringLiteral(
                "lingtai-attachment-thumbnail://%1/%2")
                .arg(QString::fromStdString(messages[index].id))
                .arg(attachment_index));
            const auto value = suffix_surface.document()->resource(
                QTextDocument::ImageResource, resource);
            if (value.isValid()) {
                document()->addResource(
                    QTextDocument::ImageResource, resource, value);
            }
        }
    }

    rebuild_in_progress_ = true;
    auto prior_format = old_frames.back()->frameFormat();
    prior_format.setBottomMargin(
        source_frames.front()->frameFormat().bottomMargin());
    old_frames.back()->setFrameFormat(prior_format);

    for (auto source_index = qsizetype{1};
            source_index != source_frames.size(); ++source_index) {
        auto *source = source_frames[source_index];
        auto target_cursor = root_append_cursor(document());
        auto *target = target_cursor.insertFrame(source->frameFormat());
        const auto source_first_format =
            source->begin().currentBlock().blockFormat();
        auto source_cursor = source->firstCursorPosition();
        source_cursor.setPosition(
            source->lastPosition(), QTextCursor::KeepAnchor);
        auto target_content = target->firstCursorPosition();
        const auto merged_block = target_content.block();
        target_content.insertFragment(QTextDocumentFragment(source_cursor));
        // insertFragment merges its first source block into the target frame's
        // existing empty block but keeps that block's default format. Restore
        // the real first format after the merge, or a one-block Human suffix
        // loses its right-anchoring margins until a complete rebuild.
        QTextCursor(merged_block).setBlockFormat(source_first_format);
    }
    last_messages_.insert(last_messages_.end(),
        messages.begin() + static_cast<std::ptrdiff_t>(append_from),
        messages.end());
    for (auto index = append_from; index != messages.size(); ++index) {
        for (const auto &attachment : messages[index].attachments) {
            accessible_attachment_names_.push_back(
                QString::fromStdString(attachment.display_filename));
        }
    }
    setAccessibleDescription(accessible_attachment_names_.isEmpty()
        ? QStringLiteral("The current direct conversation, read-only.")
        : QStringLiteral("The current direct conversation, read-only. "
              "Attachments in message order: %1")
              .arg(accessible_attachment_names_.join(QStringLiteral(", "))));
    empty_state_active_ = false;
    last_plain_state_.clear();
    clear_hovered_message();
    document()->documentLayout()->documentSize();
    rebuild_in_progress_ = false;
    if (was_at_bottom) {
        scroll_to_bottom();
    } else {
        ++scroll_pin_generation_;
        scrollbar->setValue(std::min(previous_scroll, scrollbar->maximum()));
    }
    last_rendered_verbose_level_ = verbose_level_;
    last_layout_width_ = int(viewport()->width() / 8) * 8;
    return true;
}

void ConversationSurface::apply_session_events(
        const std::vector<ConversationSessionEntry> &session_events) {
    if (same_session_events(session_events)
            && verbose_level_ == last_rendered_verbose_level_) {
        return;
    }
    disarm_attachment_action();
    last_session_events_ = session_events;
    if (last_messages_.empty()) {
        last_rendered_verbose_level_ = verbose_level_;
        return;
    }
    if (verbose_level_ != ConversationVerboseLevel::off) {
        const auto page = history_page_size();
        const auto min_offset = int(
            last_messages_.size() > std::size_t(page)
                ? last_messages_.size() - std::size_t(page)
                : std::size_t{0});
        if (history_offset_ < min_offset) {
            history_offset_ = min_offset;
        }
    }
    // Coalesce only when a rebuild is already running (activity timer + async
    // session load). Otherwise rebuild immediately so the UI and widget tests
    // see frames without waiting on a debounce timer.
    schedule_rebuild_document();
    last_rendered_verbose_level_ = verbose_level_;
    last_layout_width_ = int(viewport()->width() / 8) * 8;
}

ConversationVerboseLevel ConversationSurface::cycle_verbose_level() {
    disarm_attachment_action();
    verbose_level_ = cycle_conversation_verbose_level(verbose_level_);
    emit verbose_level_changed(verbose_level_);
    return verbose_level_;
}

void ConversationSurface::refresh_chrome() {
    disarm_attachment_action();
    // QPalette::setColor copies by value, so the constructor-time
    // HighlightedText (and the plain-state/empty-state path, which never
    // rebuilds the document) would otherwise keep painting selected text in
    // the ink of whichever theme was active at construction. Re-read it here
    // on every refresh, before the possibly-deferred rebuild below.
    auto refreshed_palette = palette();
    refreshed_palette.setColor(QPalette::Highlight, Qt::transparent);
    refreshed_palette.setColor(QPalette::HighlightedText, st::windowFg->c);
    setPalette(refreshed_palette);
    if (last_messages_.empty() || rebuild_in_progress_) {
        update();
        return;
    }
    last_layout_width_ = -1;
    // Theme refreshes can arrive in a burst; coalesce into one deferred rebuild
    // instead of synchronously laying out every message + verbose frame on the
    // UI thread (that froze LingTai at 100% CPU).
    if (rebuild_scheduled_) {
        return;
    }
    rebuild_scheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        rebuild_scheduled_ = false;
        if (last_messages_.empty() || rebuild_in_progress_) {
            update();
            return;
        }
        rebuild_document();
    });
}

int ConversationSurface::history_page_size() const noexcept {
    return verbose_level_ == ConversationVerboseLevel::off
        ? kHistoryPageSize
        : kVerboseHistoryPageSize;
}

void ConversationSurface::schedule_rebuild_document() {
    if (rebuild_in_progress_) {
        rebuild_scheduled_ = true;
        return;
    }
    if (rebuild_scheduled_) {
        return;
    }
    // Prefer an immediate rebuild; only defer when coalescing a burst.
    rebuild_document();
}

void ConversationSurface::rebuild_document() {
    disarm_attachment_action();
    if (rebuild_in_progress_) {
        rebuild_scheduled_ = true;
        return;
    }
    rebuild_in_progress_ = true;

    // Capture the human's exact scroll state before rebuilding the document,
    // so a changed refresh follows the new bottom only when the human was
    // already there and otherwise preserves the prior non-bottom position.
    auto *scrollbar = verticalScrollBar();
    const auto previous = scrollbar->value();
    const auto was_at_bottom = !wheel_gesture_active_
        && previous >= scrollbar->maximum();

    // document->clear() below drops every QTextCursor onto position 0, which
    // would silently end an active drag-select on a theme refresh (the same
    // messages are about to be reinserted, so the old offsets still apply).
    // Restore the selection by content offset once the rebuild finishes.
    const auto selection_before = textCursor();
    const auto had_selection = selection_before.hasSelection();
    const auto selection_start = selection_before.selectionStart();
    const auto selection_end = selection_before.selectionEnd();

    auto *document = this->document();
    document->clear();
    clear_plain_state_anchor(document);

    // The render-time history window: only the cached rows from history_offset_
    // to the end become real message frames, so an unchanged cache is never
    // paged in beyond the window a reveal already requested.
    const auto visible_begin = std::size_t(history_offset_);
    const auto visible_end = last_messages_.size();
    const auto viewport_width = viewport()->width();

    // When older rows are hidden, one centered muted banner on the root frame
    // leads the stream. It is plain root blocks, never a message QTextFrame, so
    // the banner does not count among the direct child message frames and keeps
    // the per-message container contract intact.
    if (history_offset_ > 0) {
        auto banner_cursor = QTextCursor(document);
        banner_cursor.movePosition(QTextCursor::Start);
        auto banner_format = QTextBlockFormat();
        banner_format.setAlignment(Qt::AlignCenter);
        const auto gutter = reading_column_margins(viewport_width);
        banner_format.setLeftMargin(gutter);
        banner_format.setRightMargin(gutter);
        banner_format.setTopMargin(kBannerTopMargin);
        banner_format.setBottomMargin(kBannerBottomMargin);
        banner_cursor.setBlockFormat(banner_format);
        banner_cursor.insertText(
            QStringLiteral("\u25B2 %1 older \u2014 ctrl+u to load")
                .arg(history_offset_),
            secondary_format());
    }

    // One transparent borderless sibling QTextFrame per message under the root
    // frame, each owning its header and body blocks: the frame's first block
    // is the header carrying the message block format, and every body logical
    // line becomes its own real unmarked QTextBlock cloned from that format,
    // so the standard layout honors the block alignment and the margins bind
    // each message to the shared reading-column width.
    const auto separator = QString(QChar::LineSeparator);
    const auto agent_lane_max = message_block_width(viewport_width);
    const auto human_lane_max = human_message_block_width(viewport_width);
    // The formatted day tracks only this visible lazy suffix, so a separator
    // appears before the first message of each changed nonempty day within the
    // window and never leaks across reveals.
    // Telegram Desktop's sizing is two-stage on one real text object: first lay
    // the actual message out at its allowed lane, then read that same layout's
    // real line widths and shrink the lane once. Keep the real frames here for
    // that second stage; no detached measurement document is involved.
    struct PendingMessageWidth final {
        QTextFrame *frame = nullptr;
        bool outgoing = false;
        int lane_min = 0;
        int lane_max = 0;
        int horizontal_padding = 0;
        qreal provisional_left = 0;
        qreal provisional_right = 0;
    };
    std::vector<PendingMessageWidth> pending_widths;

    VerboseRenderState verbose_state;
    if (verbose_level_ != ConversationVerboseLevel::off
            && !last_session_events_.empty()
            && visible_begin < last_messages_.size()) {
        const auto &first_visible = last_messages_[visible_begin].timestamp;
        while (verbose_state.event_index < last_session_events_.size()
                && timestamp_compare(
                    last_session_events_[verbose_state.event_index].timestamp,
                    first_visible) < 0) {
            ++verbose_state.event_index;
        }
    }
    QString previous_day;
    for (auto index = visible_begin; index != visible_end; ++index) {
        const auto &message = last_messages_[index];
        if (verbose_level_ != ConversationVerboseLevel::off
                && !last_session_events_.empty()) {
            auto verbose_cursor = QTextCursor(document);
            append_verbose_events_until(
                verbose_cursor,
                viewport_width,
                last_session_events_,
                verbose_level_,
                verbose_state,
                message.timestamp);
            // Keep pending token footers with their API group — do not flush
            // before every mail row (that duplicated footers and forced extra
            // layout work on each message).
        }
        const auto outgoing = message.outgoing;
        // The renderer-only presentation (HH:mm time, yyyy/MM/dd day) is
        // computed once per visible message and shared by the incoming header,
        // the outgoing frame time, and the day separator.
        const auto present = present_timestamp(message.timestamp);
        const auto day_changed = !present.day.isEmpty()
            && present.day != previous_day;
        // A visible Agent group starts at the lazy-window boundary, after a
        // Human row/day boundary, or when the preceding Agent message is more
        // than five minutes away. Only a proven short chronological interval
        // creates a headerless continuation.
        const auto continues_previous = !outgoing
            && index > visible_begin
            && !last_messages_[index - 1].outgoing
            && !day_changed
            && within_same_agent_interval(
                last_messages_[index - 1].timestamp, message.timestamp);
        const auto incoming_group_first = !outgoing && !continues_previous;
        auto group_continues = !outgoing
            && index + 1 < visible_end
            && !last_messages_[index + 1].outgoing
            && within_same_agent_interval(
                message.timestamp, last_messages_[index + 1].timestamp);
        if (group_continues) {
            const auto next_day = present_timestamp(
                last_messages_[index + 1].timestamp).day;
            group_continues = present.day.isEmpty() || next_day.isEmpty()
                || next_day == present.day;
        }

        // Each message's width is content-driven. Agent prose retains the
        // established 72% lane and minimum; Human bubbles use a smaller 60%
        // column cap, a genuinely compact minimum, and their own 15px padding.
        const auto lane_max = outgoing ? human_lane_max : agent_lane_max;
        const auto lane_min = outgoing
            ? kHumanMinMessageWidth
            : kMinMessageWidth;
        const auto horizontal_padding = outgoing
            ? kHumanBubbleHPadding
            : kBubbleHPadding;
        // Stage one uses the whole allowed lane. This is the same provisional
        // width Telegram passes into its real text object's countSize(): it lets
        // the real frame decide line breaks before any content-sized shrink.
        const auto block_format = message_block_format(
            outgoing, viewport_width, lane_max);
        // Append after every prior child frame at the root — never nest inside
        // the previous message by using document End while still in that frame.
        auto cursor = root_append_cursor(document);
        auto frame_format = QTextFrameFormat();
        frame_format.setBorder(0);
        frame_format.setPadding(0);
        frame_format.setMargin(0);
        // The current frame owns its gap to the next sibling: consecutive
        // short-interval same-Agent rows keep a small rendered rhythm, while a
        // long-pause/direction/day/end boundary gets the visibly larger break.
        frame_format.setBottomMargin(outgoing
            ? kHumanMessageBottomMargin
            : (group_continues
                ? kWithinGroupBottomMargin
                : kBetweenGroupBottomMargin));
        frame_format.setBackground(Qt::transparent);
        frame_format.setProperty(kMessageOutgoingProperty, outgoing);
        frame_format.setProperty(
            kMessageIdProperty, QString::fromStdString(message.id));
        const auto reaction_found = last_reactions_.find(message.id);
        const auto reaction_labels = reaction_found == last_reactions_.end()
            ? QStringList()
            : reaction_chip_labels(reaction_found->second);
        if (!reaction_labels.isEmpty()) {
            frame_format.setProperty(
                kMessageReactionsProperty, reaction_labels);
            // Telegram-like in-bubble chips need room below the body text.
            const auto chip_font = secondary_format().font();
            const auto chip_row = reaction_row_height(QFontMetricsF(chip_font));
            frame_format.setBottomMargin(
                (outgoing
                    ? kHumanMessageBottomMargin
                    : (group_continues
                        ? kWithinGroupBottomMargin
                        : kBetweenGroupBottomMargin))
                + chip_row + kReactionRowTopGap);
        }
        frame_format.setProperty(
            kMessageAvatarProperty, incoming_group_first);
        // The Human timestamp is not a visible header: it rides on the frame so
        // paintEvent can draw a compact muted line below-right of the body-only
        // bubble, or omit it when the presented HH:mm is empty.
        if (outgoing) {
            frame_format.setProperty(kMessageTimestampProperty, present.time);
        }
        // The first message frame of each calendar day carries the divider
        // label on its frame metadata and extra top margin so paintEvent can
        // draw the centered rules in the gap above it, between message groups.
        if (day_changed) {
            frame_format.setTopMargin(kDayTopMargin + kDayBottomMargin);
            frame_format.setProperty(
                kDaySeparatorProperty, format_day_label(present.date));
            previous_day = present.day;
        }
        auto *frame = cursor.insertFrame(frame_format);
        cursor = frame->firstCursorPosition();
        // The header block carries the whole-message lane format. A negative
        // bottom margin closes the Agent-name → body gap by kHeaderToBodyOverlap
        // without changing sender line metrics or inter-message frame gaps.
        auto header_format = block_format;
        header_format.setBottomMargin(-kHeaderToBodyOverlap);
        cursor.setBlockFormat(header_format);
        if (!outgoing && incoming_group_first) {
            cursor.insertText(them_, sender_format(outgoing));
            if (!present.time.isEmpty()) {
                cursor.insertText(
                    QStringLiteral(" · %1").arg(present.time),
                    message_metadata_format());
            }
            cursor.insertText(separator, secondary_format());
        }
        // Stored email subject/title metadata is deliberately absent from every
        // message surface; the conversation begins with sender metadata (Agent)
        // or the body itself (Human).
        // The stored raw body is rendered through the accepted safe-markdown
        // contract: only the narrow marker set becomes character formatting,
        // everything else (including raw HTML) stays literal text. Each body
        // line becomes its own real unmarked QTextBlock cloned from the
        // message block format with zero top/bottom margins and no message
        // property, so only the message header keeps the property. Paragraph
        // delimiters are normalized first so a decoded LF, CRLF/CR or U+2029
        // reads as a line break; stored source text is untouched.
        auto body = QString::fromStdString(message.text);
        body.replace(QStringLiteral("\r\n"), QString(QChar::LineSeparator));
        body.replace(QChar::LineFeed, QString(QChar::LineSeparator));
        body.replace(QChar::CarriageReturn, QString(QChar::LineSeparator));
        body.replace(QChar::ParagraphSeparator, QString(QChar::LineSeparator));
        auto continuation = block_format;
        continuation.setTopMargin(0);
        continuation.setBottomMargin(0);
        continuation.clearProperty(kMessageBlockProperty);
        // An outgoing body-only message keeps its first real body line in the
        // frame's existing initial block (preserving that block's lane and top
        // margin) instead of opening a fresh continuation block. Incoming group
        // firsts retain a sender/time header; headerless continuations put their
        // body directly in the initial block at the identical left axis.
        const auto first_in_initial = outgoing
            || (!outgoing && !incoming_group_first);
        insert_markdown_body(
            cursor, body, body_format(outgoing), continuation,
            first_in_initial);

        // Attachments are ordinary blocks inside the owning message frame, so
        // their order, wrapping, lane alignment, scrolling, and chronology are
        // inherited from the message rather than maintained by an overlay.
        for (auto attachment_index = std::size_t{0};
                attachment_index != message.attachments.size();
                ++attachment_index) {
            const auto &attachment = message.attachments[attachment_index];
            auto attachment_block = continuation;
            attachment_block.setTopMargin(8);
            attachment_block.setBottomMargin(3);
            attachment_block.setLeftMargin(
                attachment_block.leftMargin() + 8);
            attachment_block.setRightMargin(
                attachment_block.rightMargin() + 8);
            attachment_block.setProperty(kAttachmentBlockProperty, true);
            attachment_block.setProperty(
                kAttachmentMessageIdProperty,
                QString::fromStdString(message.id));
            attachment_block.setProperty(
                kAttachmentIndexProperty,
                static_cast<qulonglong>(attachment_index));
            cursor.insertBlock(attachment_block);

            const auto tooltip = QString::fromStdString(
                attachment.display_filename);
            const auto preview = load_attachment_thumbnail(
                attachment, QSize(180, 120));
            if (!preview.isNull()) {
                const auto resource = QUrl(QStringLiteral(
                    "lingtai-attachment-thumbnail://%1/%2")
                    .arg(QString::fromStdString(message.id))
                    .arg(attachment_index));
                document->addResource(
                    QTextDocument::ImageResource,
                    resource,
                    preview.toImage());
                auto image = QTextImageFormat();
                image.setName(resource.toString());
                image.setWidth(preview.width());
                image.setHeight(preview.height());
                image.setToolTip(tooltip);
                image.setProperty(QTextFormat::ImageAltText,
                    QStringLiteral("Image attachment %1").arg(tooltip));
                cursor.insertImage(image);
                cursor.insertText(separator, secondary_format());
            } else {
                auto icon = secondary_format();
                auto icon_font = icon.font();
                icon_font.setWeight(QFont::DemiBold);
                icon.setFont(icon_font);
                cursor.insertText(
                    attachment.media_kind == AttachmentMediaKind::image
                        ? QStringLiteral("IMG  ")
                        : QStringLiteral("FILE  "),
                    icon);
            }
            // Leave stable room for the icon and the block's card insets. A
            // conservative single-line budget prevents punctuation break
            // opportunities in a long filename from turning elision into a
            // second line at the narrow breakpoint.
            const auto max_name_width = qMax(72, lane_max - 180);
            const auto full_name = QString::fromStdString(
                attachment.display_filename);
            auto name_font = attachment_name_format(outgoing, tooltip).font();
            const auto shown_name = QFontMetrics(name_font).elidedText(
                full_name, Qt::ElideMiddle, max_name_width);
            cursor.insertText(
                shown_name,
                attachment_name_format(outgoing, tooltip));
            cursor.insertText(separator, secondary_format());
            cursor.insertText(
                QStringLiteral("%1 · %2   ")
                    .arg(human_file_size(attachment.byte_size),
                         attachment_type_label(attachment)),
                secondary_format());
            cursor.insertText(
                QStringLiteral("Open"),
                attachment_action_format(
                    outgoing,
                    QString::fromStdString(message.id),
                    attachment_index,
                    false));
            cursor.insertText(QStringLiteral("   "), secondary_format());
            cursor.insertText(
                QStringLiteral("Reveal in Finder"),
                attachment_action_format(
                    outgoing,
                    QString::fromStdString(message.id),
                    attachment_index,
                    true));
        }
        pending_widths.push_back({
            frame,
            outgoing,
            lane_min,
            lane_max,
            horizontal_padding,
            block_format.leftMargin(),
            block_format.rightMargin(),
        });
    }

    if (verbose_level_ != ConversationVerboseLevel::off
            && !last_session_events_.empty()) {
        auto verbose_cursor = QTextCursor(document);
        append_verbose_events_until(
            verbose_cursor,
            viewport_width,
            last_session_events_,
            verbose_level_,
            verbose_state,
            std::nullopt);
        flush_verbose_token_footer(
            verbose_cursor, viewport_width, verbose_state);
    }
    if (verbose_state.render_capped) {
        insert_verbose_line(
            document,
            viewport_width,
            secondary_format(),
            QStringLiteral("▲ older LLM details hidden — ctrl+u for history"),
            8);
    }

    // Stage two reads the real QTextLayouts just created above, equivalent to
    // Telegram's countSize()/textRealWidth() pass. Shrink every message once to
    // its widest real line plus declared padding, preserving per-block list
    // indents and other semantic formatting as the outer lane margins move.
    document->documentLayout()->documentSize();
    for (const auto &pending : pending_widths) {
        auto real_width = 0.0;
        for (auto it = pending.frame->begin(); !it.atEnd(); ++it) {
            const auto block = it.currentBlock();
            if (!block.isValid()) {
                continue;
            }
            const auto *layout = block.layout();
            for (auto line_index = 0;
                    line_index != layout->lineCount(); ++line_index) {
                real_width = std::max(
                    real_width,
                    layout->lineAt(line_index).horizontalAdvance());
            }
        }
        // Block margins are measured in the viewport, while QTextDocument also
        // subtracts its root margin from both sides of the actual line lane. Add
        // that existing inset back so declared bubble padding is not consumed by
        // the document chrome during the second layout pass.
        const auto document_inset = int(std::ceil(
            2 * document->documentMargin()));
        const auto final_width = qBound(
            qMin(pending.lane_min, pending.lane_max),
            int(std::ceil(real_width)) + 2 * pending.horizontal_padding
                + document_inset,
            pending.lane_max);
        const auto final_lane = message_block_format(
            pending.outgoing, viewport_width, final_width);
        const auto left_delta = final_lane.leftMargin()
            - pending.provisional_left;
        const auto right_delta = final_lane.rightMargin()
            - pending.provisional_right;
        for (auto it = pending.frame->begin(); !it.atEnd(); ++it) {
            const auto block = it.currentBlock();
            if (!block.isValid()) {
                continue;
            }
            auto format = block.blockFormat();
            format.setLeftMargin(format.leftMargin() + left_delta);
            format.setRightMargin(format.rightMargin() + right_delta);
            auto block_cursor = QTextCursor(block);
            block_cursor.setBlockFormat(format);
        }
    }
    document->documentLayout()->documentSize();

    // The rebuild above reinserted the identical cached messages, so the
    // captured offsets still address the same content; clamp defensively in
    // case the visible window changed (e.g. a reveal_older() in the same
    // pass) and restore rather than leaving the drag-select silently gone.
    if (had_selection) {
        const auto length = document->characterCount();
        auto restored = textCursor();
        restored.setPosition(std::clamp(selection_start, 0, length - 1));
        restored.setPosition(
            std::clamp(selection_end, 0, length - 1), QTextCursor::KeepAnchor);
        setTextCursor(restored);
    }

    if (was_at_bottom) {
        scroll_to_bottom();
    } else {
        // Cancel any deferred bottom pin from a prior follow-bottom rebuild so
        // it cannot yank the viewport after this non-bottom restore.
        ++scroll_pin_generation_;
        scrollbar->setValue(std::min(previous, scrollbar->maximum()));
    }

    rebuild_in_progress_ = false;
    if (rebuild_scheduled_) {
        rebuild_scheduled_ = false;
        QTimer::singleShot(0, this, [this] {
            if (!last_messages_.empty() && !rebuild_in_progress_) {
                rebuild_document();
            }
        });
    }
}

void ConversationSurface::rebuild_select_agent_prompt() {
    disarm_attachment_action();
    if (!select_agent_prompt_active_ || last_plain_state_.isEmpty()) {
        return;
    }
    apply_plain_state_formatting(
        document(), viewport()->width(), viewport()->height());
    // apply_plain_state_formatting paints the whole document in secondary
    // Normal — re-assert the title and illustration headroom every rebuild,
    // including height-only / same-quantum width reflows that otherwise leave
    // "Select an agent" looking like body copy.
    auto frame_format = document()->rootFrame()->frameFormat();
    frame_format.setTopMargin(std::max(
        frame_format.topMargin(),
        qreal(kSelectAgentIllustrationSize + kSelectAgentIllustrationGap
            + kEmptyStateHeadroom)));
    document()->rootFrame()->setFrameFormat(frame_format);
    auto cursor = QTextCursor(document());
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    cursor.mergeCharFormat(select_agent_title_format());
}

void ConversationSurface::rebuild_empty_state() {
    disarm_attachment_action();
    auto *document = this->document();
    document->clear();
    clear_plain_state_anchor(document);
    auto cursor = QTextCursor(document);
    cursor.movePosition(QTextCursor::Start);

    // The empty no-message state is one centered group inside the same reading
    // column messages use: symmetric gutters bound every block, and AlignCenter
    // holds the small Agent avatar, the title, and the muted prompt together
    // rather than the full pane.
    const auto gutter = reading_column_margins(viewport()->width());
    auto centered = QTextBlockFormat();
    centered.setAlignment(Qt::AlignCenter);
    centered.setLeftMargin(gutter);
    centered.setRightMargin(gutter);

    auto avatar_format = centered;
    avatar_format.setBottomMargin(kEmptyAvatarGap);
    cursor.setBlockFormat(avatar_format);
    auto image_format = QTextImageFormat();
    image_format.setName(QStringLiteral("empty-state-agent-avatar"));
    image_format.setWidth(kEmptyAvatarDiameter);
    image_format.setHeight(kEmptyAvatarDiameter);
    cursor.insertImage(image_format);

    auto title_format = centered;
    title_format.setBottomMargin(kEmptyTitleGap);
    cursor.insertBlock(title_format);
    cursor.insertText(
        QStringLiteral("No messages yet"), empty_state_title_format());

    cursor.insertBlock(centered);
    cursor.insertText(
        QStringLiteral("Send a message or start the Agent."),
        secondary_format());

    auto frame_format = document->rootFrame()->frameFormat();
    frame_format.setTopMargin(kEmptyStateHeadroom);
    document->rootFrame()->setFrameFormat(frame_format);

    const auto initial = them_.trimmed().isEmpty()
        ? QStringLiteral("A")
        : them_.trimmed().left(1).toUpper();
    document->addResource(
        QTextDocument::ImageResource,
        QUrl(QStringLiteral("empty-state-agent-avatar")),
        empty_state_avatar(initial, kEmptyAvatarDiameter));
}

void ConversationSurface::reveal_older() {
    // Page one more older slice of the cached history into the render-time
    // window, clamped so the offset never goes below zero (the full stream).
    history_offset_ = std::max(0, history_offset_ - history_page_size());

    // Rebuild with the wider window, then restore the previously first-visible
    // message's viewport Y. Revealing rows at the top grows the document by
    // exactly the new scroll maximum's increase, so shifting the current
    // scroll position by that delta keeps the old anchor frame on screen.
    auto *scrollbar = verticalScrollBar();
    const auto old_maximum = scrollbar->maximum();
    const auto previous = scrollbar->value();
    rebuild_document();
    const auto delta = scrollbar->maximum() - old_maximum;
    scrollbar->setValue(std::min(previous + delta, scrollbar->maximum()));
}

void ConversationSurface::scroll_to_bottom_now() {
    auto *bar = verticalScrollBar();
    const auto laid_out = document()->documentLayout()->documentSize().height();
    const auto layout_max = std::max(0,
        int(std::ceil(laid_out - viewport()->height())));
    if (layout_max > bar->maximum()) {
        bar->setMaximum(layout_max);
    }
    bar->setValue(bar->maximum());
}

void ConversationSurface::scroll_to_bottom() {
    if (wheel_gesture_active_) {
        ++scroll_pin_generation_;
        return;
    }
    scroll_to_bottom_now();
    // QTextEdit may still sync its slider from a queued documentSizeChanged
    // after a new-day frame top margin lands. Pin again once that maximum is
    // real so the viewport sits on the true document bottom — but only if the
    // human has not scrolled away from the pin in the meantime.
    const auto pinned_at = verticalScrollBar()->value();
    const auto generation = ++scroll_pin_generation_;
    QTimer::singleShot(0, this, [this, generation, pinned_at] {
        if (generation != scroll_pin_generation_) return;
        if (wheel_gesture_active_) return;
        if (verticalScrollBar()->value() != pinned_at) return;
        scroll_to_bottom_now();
    });
}

void ConversationSurface::paintEvent(QPaintEvent *event) {
    auto *surface_viewport = viewport();
    QPainter painter(surface_viewport);
    painter.setClipRect(event->rect());
    // The chat backdrop shares the shell's single-canvas `windowBg` base;
    // message bubbles and transient interaction states keep their own tokens.
    painter.fillRect(event->rect(), st::windowBg);

    const auto h_offset = horizontalScrollBar()->value();
    const auto v_offset = verticalScrollBar()->value();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    // Slack-like row hover: full-width wash matching the Human bubble tint.
    paint_message_hover_row(
        painter,
        document(),
        hovered_message_id_,
        surface_viewport->width(),
        h_offset,
        v_offset,
        event->rect());

    if (select_agent_prompt_active_) {
        const auto illustration = select_agent_illustration(
            kSelectAgentIllustrationSize);
        if (!illustration.isNull()) {
            const auto x = (surface_viewport->width()
                - illustration.width()) / 2;
            const auto y = std::max(16,
                int(document()->rootFrame()->frameFormat().topMargin())
                    - illustration.height()
                    - kSelectAgentIllustrationGap);
            painter.drawPixmap(x, y, illustration);
        }
    }

    const auto *document_layout = document()->documentLayout();
    const auto message_frames = document()->rootFrame()->childFrames();
    for (auto *frame : message_frames) {
        // Verbose LLM-detail frames are root siblings too; never paint a
        // message bubble from their text bounds or they sit beside Human rows.
        if (frame->frameFormat().property(kVerboseFrameProperty).toBool()) {
            continue;
        }
        if (!frame->frameFormat().hasProperty(kMessageIdProperty)) {
            continue;
        }
        // Each sibling message frame carries its own advancing document
        // origin, while the blocks' QTextLayout positions stay relative to
        // that frame. Translate every line by both the block layout position
        // and the frame's document top-left so each bubble sits around its
        // native Qt text instead of the first frame's local coordinates.
        const auto frame_origin = document_layout
            ->frameBoundingRect(frame)
            .topLeft();
        QTextBlock first_valid_block;
        auto text_bounds = QRectF();
        // Contiguous fenced runs only — never unite across prose, or the panel
        // swallows intervening body text and fails to hug each fence.
        std::vector<QRectF> code_runs;
        std::vector<QRectF> attachment_cards;
        auto current_code_run = QRectF();
        auto in_code_run = false;
        for (auto block = frame->begin(); !block.atEnd();
             ++block) {
            const auto current_block = block.currentBlock();
            if (!current_block.isValid()) {
                continue;
            }
            const auto *block_layout = current_block.layout();
            auto block_bounds = QRectF();
            // Prefer the full line rect over naturalTextRect so short fence
            // lines still share one panel width with longer neighbors.
            for (auto i = 0; i != block_layout->lineCount(); ++i) {
                const auto line = block_layout->lineAt(i);
                const auto line_bounds = line.rect()
                    .translated(block_layout->position())
                    .translated(frame_origin);
                block_bounds = block_bounds.isNull()
                    ? line_bounds
                    : block_bounds.united(line_bounds);
            }
            if (block_bounds.isNull()) {
                continue;
            }
            if (current_block.blockFormat()
                    .property(kCodeBlockProperty).toBool()) {
                current_code_run = current_code_run.isNull()
                    ? block_bounds
                    : current_code_run.united(block_bounds);
                in_code_run = true;
            } else if (in_code_run) {
                code_runs.push_back(current_code_run);
                current_code_run = QRectF();
                in_code_run = false;
            }
            if (current_block.blockFormat()
                    .property(kAttachmentBlockProperty).toBool()) {
                attachment_cards.push_back(block_bounds.adjusted(-7, -5, 7, 5));
            }
            if (!first_valid_block.isValid()) {
                first_valid_block = current_block;
            }
            // Human-bubble geometry still needs glyph-tight bounds so the
            // bubble does not grow with unused wrap slack.
            auto glyph_bounds = QRectF();
            for (auto i = 0; i != block_layout->lineCount(); ++i) {
                const auto line = block_layout->lineAt(i);
                const auto line_bounds = line.naturalTextRect()
                    .translated(block_layout->position())
                    .translated(frame_origin);
                glyph_bounds = glyph_bounds.isNull()
                    ? line_bounds
                    : glyph_bounds.united(line_bounds);
            }
            const auto &for_text = glyph_bounds.isNull()
                ? block_bounds
                : glyph_bounds;
            text_bounds = text_bounds.isNull()
                ? for_text
                : text_bounds.united(for_text);
        }
        if (in_code_run) {
            code_runs.push_back(current_code_run);
        }
        if (text_bounds.isNull() || !first_valid_block.isValid()) {
            continue;
        }
        text_bounds.translate(-h_offset, -v_offset);
        for (auto &run : code_runs) {
            run.translate(-h_offset, -v_offset);
        }
        for (auto &card : attachment_cards) {
            card.translate(-h_offset, -v_offset);
        }
        const auto outgoing = frame->frameFormat()
            .property(kMessageOutgoingProperty)
            .toBool();
        if (outgoing) {
            // `naturalTextRect()` stops at the longest painted glyph run, so
            // word-wrap slack made otherwise identical capped Human lanes end
            // at different x positions. The QTextLine rect owns the actual
            // right-anchored lane width; use that edge for both bubble and time.
            const auto *first_layout = first_valid_block.layout();
            const auto lane_right = first_layout->lineAt(0).rect()
                .translated(first_layout->position())
                .translated(frame_origin)
                .right() - h_offset;
            auto bubble = text_bounds.adjusted(
                -kHumanBubbleHPadding,
                -kHumanBubbleVPadding,
                kHumanBubbleHPadding,
                kHumanBubbleVPadding);
            bubble.setRight(lane_right + kHumanBubbleHPadding);
            const auto reaction_labels = frame->frameFormat()
                .property(kMessageReactionsProperty)
                .toStringList();
            auto chip_font = secondary_format().font();
            chip_font.setPixelSize(13);
            const auto chip_metrics = QFontMetricsF(chip_font);
            const auto chip_row = reaction_labels.isEmpty()
                ? 0
                : reaction_row_height(chip_metrics);
            if (chip_row > 0) {
                bubble.setBottom(
                    bubble.bottom() + kReactionRowTopGap + chip_row
                    + kReactionRowBottomInset);
            }
            // The painter is already clipped to event->rect(). Do not skip the
            // whole message merely because the bubble itself misses a partial
            // repaint: the below-bubble timestamp may still intersect it.
            painter.setBrush(human_bubble_color());
            painter.drawRoundedRect(
                bubble, kHumanBubbleRadius, kHumanBubbleRadius);
            if (chip_row > 0) {
                auto chip_x = bubble.left() + kReactionRowSideInset;
                const auto chip_y = bubble.bottom() - kReactionRowBottomInset
                    - chip_row;
                for (const auto &label : reaction_labels) {
                    const auto text_width = chip_metrics.horizontalAdvance(label);
                    const auto chip_width = text_width + 2 * kReactionChipHPadding;
                    const auto chip_rect = QRectF(
                        chip_x, chip_y, chip_width, chip_row);
                    painter.setBrush(reaction_chip_fill(true));
                    painter.drawRoundedRect(chip_rect, chip_row / 2.0, chip_row / 2.0);
                    painter.setPen(body_reading_color(true));
                    painter.setFont(chip_font);
                    painter.drawText(
                        chip_rect, Qt::AlignCenter, label);
                    painter.setPen(Qt::NoPen);
                    chip_x += chip_width + kReactionChipGap;
                }
            }
            // The stored HH:mm is one compact muted 12px line below the bubble,
            // sharing its right edge instead of floating beside the top.
            const auto timestamp = frame->frameFormat()
                .property(kMessageTimestampProperty)
                .toString();
            if (!timestamp.isEmpty()) {
                const auto metadata = secondary_format();
                const auto metrics = QFontMetricsF(metadata.font());
                const auto text_width = metrics.horizontalAdvance(timestamp);
                const auto text_height = metrics.height();
                const auto time_rect = QRectF(
                    bubble.right() - text_width,
                    bubble.bottom() + kTimestampGap,
                    text_width,
                    text_height);
                if (time_rect.intersects(QRectF(event->rect()))) {
                    painter.save();
                    painter.setPen(secondary_reading_color());
                    painter.setFont(metadata.font());
                    painter.drawText(
                        time_rect,
                        Qt::AlignRight | Qt::AlignVCenter,
                        timestamp);
                    painter.restore();
                }
            }
        } else {
            const auto reaction_labels = frame->frameFormat()
                .property(kMessageReactionsProperty)
                .toStringList();
            if (!reaction_labels.isEmpty()) {
                auto chip_font = secondary_format().font();
                chip_font.setPixelSize(13);
                const auto chip_metrics = QFontMetricsF(chip_font);
                const auto chip_row = reaction_row_height(chip_metrics);
                auto chip_x = text_bounds.left();
                const auto chip_y = text_bounds.bottom() + kReactionRowTopGap;
                for (const auto &label : reaction_labels) {
                    const auto text_width = chip_metrics.horizontalAdvance(label);
                    const auto chip_width = text_width + 2 * kReactionChipHPadding;
                    const auto chip_rect = QRectF(
                        chip_x, chip_y, chip_width, chip_row);
                    if (chip_rect.intersects(QRectF(event->rect()))) {
                        painter.setBrush(reaction_chip_fill(false));
                        painter.drawRoundedRect(
                            chip_rect, chip_row / 2.0, chip_row / 2.0);
                        painter.setPen(body_reading_color(false));
                        painter.setFont(chip_font);
                        painter.drawText(chip_rect, Qt::AlignCenter, label);
                        painter.setPen(Qt::NoPen);
                    }
                    chip_x += chip_width + kReactionChipGap;
                }
            }
            if (frame->frameFormat()
                .property(kMessageAvatarProperty).toBool()) {
                const auto avatar = QRectF(
                    text_bounds.left() - kMessageAvatarGap
                        - kMessageAvatarDiameter,
                    text_bounds.top(),
                    kMessageAvatarDiameter,
                    kMessageAvatarDiameter);
                if (avatar.intersects(QRectF(event->rect()))) {
                    painter.setBrush(st::windowBgActive->c);
                    painter.drawEllipse(avatar);
                    auto font = QFont();
                    font.setPixelSize(18);
                    font.setWeight(QFont::DemiBold);
                    painter.setFont(font);
                    painter.setPen(QColor(Qt::white));
                    painter.drawText(avatar, Qt::AlignCenter,
                        them_.trimmed().left(1).toUpper());
                    painter.setPen(Qt::NoPen);
                }
            }
        }
        for (const auto &card : attachment_cards) {
            if (!card.intersects(QRectF(event->rect()))) {
                continue;
            }
            painter.save();
            painter.setPen(QPen(attachment_card_border_color(), 1.0));
            painter.setBrush(attachment_card_color());
            painter.drawRoundedRect(card, 9, 9);
            painter.restore();
        }
        // Only fenced code gets a separate rounded surface per contiguous run;
        // ordinary paragraphs remain on the single Conversation canvas.
        for (const auto &code_bounds : code_runs) {
            const auto code_surface = code_bounds.adjusted(-8, -6, 8, 6);
            if (!code_surface.intersects(QRectF(event->rect()))) {
                continue;
            }
            painter.save();
            painter.setPen(QPen(code_surface_border_color(), 1.0));
            painter.setBrush(code_surface_color());
            painter.drawRoundedRect(code_surface, 8, 8);
            painter.restore();
        }
    }
    // Glyph-tight selection wash sits above bubbles and below Qt's text draw,
    // so selected ink stays readable without a full-lane Highlight fill.
    paint_glyph_tight_selection(
        painter,
        document(),
        textCursor(),
        h_offset,
        v_offset,
        event->rect());
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.end();

    // Let Qt draw the text with its native scroll translation, selection,
    // copy, and accessibility behavior; the widget's transparent Base keeps
    // the custom backdrop and bubbles visible underneath.
    QTextEdit::paintEvent(event);

    QPainter overlay(surface_viewport);
    overlay.setClipRect(event->rect());
    paint_day_separators(
        overlay,
        event,
        document(),
        document_layout,
        surface_viewport->width(),
        v_offset);
    overlay.end();
}

void ConversationSurface::reflow_to_viewport() {
    const auto viewport_width = viewport()->width();
    if (viewport_width < 16) {
        last_layout_width_ = -1;
        return;
    }
    // Quantize the viewport/layout width so a live resize only reflows when
    // the bound meaningfully changes. This follows the full layout width, not
    // just the capped message width: on a very wide pane the centered reading
    // column's outer gutters keep moving after the message cap stops.
    const auto width = int(viewport_width / 8) * 8;
    if (select_agent_prompt_active_) {
        // Always rebuild: apply_plain_state_formatting wipes char formats, so
        // an early return on an unchanged width quantum would leave the title
        // stuck in secondary Normal instead of DemiBold.
        last_layout_width_ = width;
        if (!rebuild_in_progress_) {
            rebuild_select_agent_prompt();
        }
        return;
    }
    if (!last_plain_state_.isEmpty()) {
        apply_plain_state_formatting(
            document(), viewport_width, viewport()->height());
    }
    if (width == last_layout_width_) {
        return;
    }
    last_layout_width_ = width;
    if (rebuild_in_progress_) {
        return;
    }
    if (empty_state_active_) {
        rebuild_empty_state();
    } else if (!last_messages_.empty()) {
        // A resize reflows the already-materialized render-time window; it
        // never pages older rows in, so the current history offset is kept.
        rebuild_document();
    }
}

void ConversationSurface::resizeEvent(QResizeEvent *event) {
    QTextEdit::resizeEvent(event);
    reflow_to_viewport();
}

void ConversationSurface::showEvent(QShowEvent *event) {
    QTextEdit::showEvent(event);
    if (last_messages_.empty() && last_plain_state_.isEmpty()
            && !empty_state_active_ && !select_agent_prompt_active_) {
        return;
    }
    last_layout_width_ = -1;
    reflow_to_viewport();
}

void ConversationSurface::keyPressEvent(QKeyEvent *event) {
    // Ctrl+O / Cmd+O cycles verbose LLM detail (TUI ctrl+o parity).
    if ((event->modifiers() & Qt::ControlModifier) != 0
            && event->key() == Qt::Key_O) {
        cycle_verbose_level();
        event->accept();
        return;
    }
    if ((event->modifiers() & Qt::MetaModifier) != 0
            && event->key() == Qt::Key_O) {
        cycle_verbose_level();
        event->accept();
        return;
    }
    // Ctrl+U reveals the next older page, but only while the window is pinned
    // to the very top and older rows remain hidden. Everywhere else the
    // inherited text-edit key handling (scrolling, selection, copy) stays.
    if (event->modifiers() == Qt::ControlModifier
        && event->key() == Qt::Key_U
        && history_offset_ > 0
        && verticalScrollBar()->value() <= verticalScrollBar()->minimum()) {
        reveal_older();
        event->accept();
        return;
    }
    QTextEdit::keyPressEvent(event);
}

bool ConversationSurface::eventFilter(QObject *watched, QEvent *event) {
    if (watched == viewport()) {
        switch (event->type()) {
        case QEvent::Wheel: {
            // Observe intent before QTextEdit handles the event. A zero/tiny
            // pixel delta can leave QScrollBar's integer value unchanged, but
            // it must still cancel a queued automatic bottom pin.
            ++scroll_pin_generation_;
            const auto phase = static_cast<QWheelEvent *>(event)->phase();
            switch (phase) {
            case Qt::ScrollBegin:
            case Qt::ScrollUpdate:
            case Qt::ScrollMomentum:
                wheel_gesture_active_ = true;
                break;
            case Qt::ScrollEnd:
                wheel_gesture_active_ = false;
                break;
            case Qt::NoScrollPhase:
                break;
            }
            // Never consume the wheel: native QTextEdit scrolling, kinetic
            // behavior, selection, copy, and accessibility remain in charge.
            break;
        }
        case QEvent::MouseButtonPress: {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            disarm_attachment_action();
            if (mouse->button() == Qt::LeftButton) {
                const auto href = anchorAt(mouse->pos());
                if (parse_attachment_action_href(href)) {
                    armed_attachment_href_ = href;
                    armed_attachment_press_pos_ = mouse->pos();
                }
            }
            // Never consume the press: QTextEdit must retain its native
            // caret, drag-selection, copy, and focus behavior.
            break;
        }
        case QEvent::MouseMove: {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            update_hovered_message(mouse->pos());
            viewport()->setCursor(parse_attachment_action_href(
                    anchorAt(mouse->pos()))
                ? Qt::PointingHandCursor
                : Qt::IBeamCursor);
            if (!armed_attachment_href_.isEmpty()
                && ((mouse->pos() - armed_attachment_press_pos_)
                        .manhattanLength() >= QApplication::startDragDistance()
                    || anchorAt(mouse->pos()) != armed_attachment_href_)) {
                disarm_attachment_action();
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() != Qt::LeftButton) {
                disarm_attachment_action();
                break;
            }
            const auto released_href = anchorAt(mouse->pos());
            const auto action = released_href == armed_attachment_href_
                    && (mouse->pos() - armed_attachment_press_pos_)
                            .manhattanLength()
                        < QApplication::startDragDistance()
                ? parse_attachment_action_href(released_href)
                : std::nullopt;
            disarm_attachment_action();
            if (!action) break;
            const auto message = std::ranges::find_if(
                last_messages_,
                [&](const auto &candidate) {
                    return QString::fromStdString(candidate.id)
                        == action->message_id;
                });
            if (message == last_messages_.end()
                || action->attachment_index >= message->attachments.size()) {
                return true;
            }
            emit attachment_action_requested({
                message->id,
                action->attachment_index,
                message->attachments[action->attachment_index],
            }, action->reveal);
            return true;
        }
        case QEvent::Leave:
            clear_hovered_message();
            disarm_attachment_action();
            viewport()->unsetCursor();
            break;
        default:
            break;
        }
    }
    return QTextEdit::eventFilter(watched, event);
}

void ConversationSurface::disarm_attachment_action() {
    armed_attachment_href_.clear();
    armed_attachment_press_pos_ = {};
}

void ConversationSurface::clear_hovered_message() {
    if (hovered_message_id_.isEmpty()) {
        return;
    }
    hovered_message_id_.clear();
    viewport()->update();
}

void ConversationSurface::update_hovered_message(const QPoint &viewport_pos) {
    if (last_messages_.empty()
            || empty_state_active_
            || select_agent_prompt_active_) {
        clear_hovered_message();
        return;
    }
    auto *document_layout = document()->documentLayout();
    if (!document_layout) {
        clear_hovered_message();
        return;
    }
    const auto doc_point = QPointF(
        viewport_pos.x() + horizontalScrollBar()->value(),
        viewport_pos.y() + verticalScrollBar()->value());
    auto found = QString();
    for (auto *frame : document()->rootFrame()->childFrames()) {
        const auto id = frame->frameFormat()
            .property(kMessageIdProperty).toString();
        if (id.isEmpty()) {
            continue;
        }
        auto hit = document_layout->frameBoundingRect(frame);
        hit.adjust(0, -kHumanBubbleVPadding, 0, kHumanBubbleVPadding);
        if (hit.contains(doc_point)) {
            found = id;
            break;
        }
    }
    if (found == hovered_message_id_) {
        return;
    }
    hovered_message_id_ = found;
    viewport()->update();
}

} // namespace lingtai::desktop
