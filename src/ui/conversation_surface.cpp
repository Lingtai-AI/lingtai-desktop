#include "ui/conversation_surface.h"

#include "base/basic_types.h"
#include "styles/palette.h"

#include <QtGui/QFont>
#include <QtGui/QFontMetricsF>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtGui/QPalette>
#include <QtGui/QResizeEvent>
#include <QtGui/QTextBlock>
#include <QtGui/QTextBlockFormat>
#include <QtGui/QTextCharFormat>
#include <QtGui/QTextCursor>
#include <QtGui/QTextFormat>
#include <QtGui/QTextFrame>
#include <QtGui/QTextLayout>
#include <QtWidgets/QScrollBar>

#include <algorithm>
#include <utility>

namespace lingtai::desktop {
namespace {

constexpr auto kDocumentMargin = 8;
constexpr auto kMessageEdgeMargin = 12;
constexpr auto kMessageTopMargin = 4;
// The combined 4px/30px vertical margins leave at least the test-pinned 14px
// rendered gap between consecutive bubbles: the 34px text-to-text distance
// minus the 8px vertical bubble padding drawn on each of the two bubbles.
constexpr auto kMessageBottomMargin = 30;
constexpr auto kMessageRatio = 0.72;
constexpr auto kMinMessageWidth = 160;
constexpr auto kMessageAbsoluteCap = 640;
constexpr auto kReadingColumnMax = 900;
constexpr auto kNarrowViewportWidth = 480;
constexpr auto kBubbleHPadding = 11;
constexpr auto kBubbleVPadding = 8;
constexpr auto kBubbleRadius = 8;
constexpr auto kMessageBlockProperty = QTextFormat::UserProperty + 1;

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

QTextBlockFormat message_block_format(
        bool outgoing,
        int viewport_width,
        int width) {
    auto format = QTextBlockFormat();
    format.setAlignment(outgoing ? Qt::AlignRight : Qt::AlignLeft);
    // Derive one outer gutter (the centered reading column's offset plus the
    // fixed edge gutter) and one inner remainder, then cross-assign them so
    // incoming stays left-anchored and outgoing right-anchored inside the same
    // column rather than centering each message individually. Each message
    // keeps the shared outer anchor while its inner edge follows its own
    // content-driven width.
    const auto column = qMin(viewport_width, kReadingColumnMax);
    const auto outer = (viewport_width - column) / 2 + kMessageEdgeMargin;
    const auto inner = qMax(outer, viewport_width - width - outer);
    format.setLeftMargin(outgoing ? inner : outer);
    format.setRightMargin(outgoing ? outer : inner);
    format.setTopMargin(kMessageTopMargin);
    format.setBottomMargin(kMessageBottomMargin);
    format.setProperty(kMessageBlockProperty, true);
    return format;
}

QTextCharFormat sender_format(bool outgoing) {
    auto format = QTextCharFormat();
    format.setForeground(outgoing ? st::historyTextOutFg
                                  : st::historyTextInFg);
    auto font = format.font();
    font.setPixelSize(13);
    font.setWeight(QFont::DemiBold);
    format.setFont(font);
    return format;
}

QTextCharFormat secondary_format() {
    auto format = QTextCharFormat();
    format.setForeground(st::msgServiceFg);
    auto font = format.font();
    font.setPixelSize(12);
    font.setWeight(QFont::Normal);
    format.setFont(font);
    return format;
}

QTextCharFormat subject_format(bool outgoing) {
    auto format = QTextCharFormat();
    format.setForeground(outgoing ? st::historyTextOutFg
                                  : st::historyTextInFg);
    auto font = format.font();
    font.setPixelSize(12);
    font.setWeight(QFont::Normal);
    format.setFont(font);
    return format;
}

QTextCharFormat body_format(bool outgoing) {
    auto format = QTextCharFormat();
    format.setForeground(outgoing ? st::historyTextOutFg
                                  : st::historyTextInFg);
    auto font = format.font();
    font.setPixelSize(14);
    font.setWeight(QFont::Normal);
    format.setFont(font);
    return format;
}

// A message's own natural content width: the widest single rendered line of
// its visible text (the sender · timestamp header, the optional subject, and
// every body line), measured with the exact same fonts rebuild_document
// applies. The caller adds the existing horizontal bubble padding, so a short
// message owns a visibly narrower lane than a longer one in the same state
// instead of every message stretching to the shared lane maximum.
int message_content_width(
        const DirectConversationMessage &message,
        const QString &them) {
    const auto outgoing = message.outgoing;
    auto widest = 0.0;
    const auto header = QFontMetricsF(sender_format(outgoing).font())
            .horizontalAdvance(outgoing ? QStringLiteral("You") : them)
        + QFontMetricsF(secondary_format().font()).horizontalAdvance(
            QStringLiteral(" · %1")
                .arg(QString::fromStdString(message.timestamp)));
    widest = qMax(widest, header);

    if (!message.subject.empty()) {
        const auto subject_metrics = QFontMetricsF(
            subject_format(outgoing).font());
        widest = qMax(widest,
            subject_metrics.horizontalAdvance(
                QString::fromStdString(message.subject)));
    }

    // The body is measured line by line: the same paragraph-delimiter
    // normalization rebuild_document applies, so only the widest single line
    // contributes instead of an unwrapped paragraph.
    const auto body_metrics = QFontMetricsF(body_format(outgoing).font());
    const auto body_lines = QString::fromStdString(message.text)
        .replace(QStringLiteral("\r\n"), QString(QChar::LineSeparator))
        .replace(QChar::LineFeed, QString(QChar::LineSeparator))
        .replace(QChar::CarriageReturn, QString(QChar::LineSeparator))
        .replace(QChar::ParagraphSeparator, QString(QChar::LineSeparator))
        .split(QChar::LineSeparator);
    for (const auto &line : body_lines) {
        widest = qMax(widest, body_metrics.horizontalAdvance(line));
    }
    return int(widest + 0.5);
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

} // namespace

ConversationSurface::ConversationSurface(QWidget *parent)
: QTextEdit(parent) {
    setReadOnly(true);
    setUndoRedoEnabled(false);
    // The chat backdrop and bubbles are painted in paintEvent, so the Qt
    // viewport must not paint its own solid background on top of them.
    auto transparent_palette = palette();
    transparent_palette.setBrush(QPalette::Base, QBrush(Qt::transparent));
    transparent_palette.setBrush(QPalette::Window, QBrush(Qt::transparent));
    setPalette(transparent_palette);
    viewport()->setAutoFillBackground(false);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent, false);
    document()->setDocumentMargin(kDocumentMargin);
}

void ConversationSurface::set_plain_state(const QString &text) {
    if (text == last_plain_state_) {
        return;
    }
    last_plain_state_ = text;
    last_messages_.clear();
    setPlainText(text);
    if (!text.isEmpty()) {
        apply_plain_state_formatting(
            document(), viewport()->width(), viewport()->height());
    } else {
        clear_plain_state_anchor(document());
    }
}

bool ConversationSurface::same_content(
        const std::vector<DirectConversationMessage> &messages) const {
    if (last_messages_.size() != messages.size()) {
        return false;
    }
    for (auto index = std::size_t{0}; index != messages.size(); ++index) {
        const auto &before = last_messages_[index];
        const auto &after = messages[index];
        if (before.id != after.id || before.outgoing != after.outgoing
            || before.timestamp != after.timestamp
            || before.subject != after.subject
            || before.text != after.text) {
            return false;
        }
    }
    return true;
}

void ConversationSurface::set_conversation(
        const QString &them,
        const std::vector<DirectConversationMessage> &messages) {
    if (them == them_ && same_content(messages)) {
        return;
    }
    them_ = them;
    last_messages_ = messages;
    last_plain_state_.clear();
    rebuild_document(messages);
}

void ConversationSurface::rebuild_document(
        const std::vector<DirectConversationMessage> &messages) {
    // Capture the human's exact scroll state before rebuilding the document,
    // so a changed refresh follows the new bottom only when the human was
    // already there and otherwise preserves the prior non-bottom position.
    auto *scrollbar = verticalScrollBar();
    const auto previous = scrollbar->value();
    const auto was_at_bottom = previous >= scrollbar->maximum();

    auto *document = this->document();
    document->clear();
    clear_plain_state_anchor(document);
    auto cursor = QTextCursor(document);
    cursor.movePosition(QTextCursor::Start);

    // One message per QTextBlock; the header/subject/body lines are separated
    // inside the block so the standard layout honors the block alignment and
    // the margins bound each message to the shared reading-column width.
    const auto separator = QString(QChar::LineSeparator);
    const auto viewport_width = viewport()->width();
    const auto lane_max = message_block_width(viewport_width);
    auto first_block = true;
    for (const auto &message : messages) {
        const auto outgoing = message.outgoing;
        // Each message's width is content-driven: its own widest visible line
        // plus the existing horizontal bubble padding, clamped between the
        // modest 160px lower bound and the shared responsive lane maximum.
        const auto width = qBound(
            qMin(kMinMessageWidth, lane_max),
            message_content_width(message, them_) + 2 * kBubbleHPadding,
            lane_max);
        const auto block_format = message_block_format(
            outgoing, viewport_width, width);
        if (first_block) {
            cursor.setBlockFormat(block_format);
            first_block = false;
        } else {
            cursor.insertBlock(block_format);
        }
        cursor.insertText(
            outgoing ? QStringLiteral("You") : them_,
            sender_format(outgoing));
        cursor.insertText(
            QStringLiteral(" · %1")
                .arg(QString::fromStdString(message.timestamp)),
            secondary_format());
        cursor.insertText(separator, secondary_format());
        if (!message.subject.empty()) {
            cursor.insertText(QString::fromStdString(message.subject),
                subject_format(outgoing));
            cursor.insertText(separator, subject_format(outgoing));
        }
        // Message text stays literal: the surface never interprets markup.
        // Paragraph delimiters are normalized so a decoded LF, CRLF/CR or
        // U+2029 renders as a U+2028 line separator inside this one block,
        // never as extra QTextBlocks/bubbles; stored source text is untouched.
        auto body = QString::fromStdString(message.text);
        body.replace(QStringLiteral("\r\n"), QString(QChar::LineSeparator));
        body.replace(QChar::LineFeed, QString(QChar::LineSeparator));
        body.replace(QChar::CarriageReturn, QString(QChar::LineSeparator));
        body.replace(QChar::ParagraphSeparator, QString(QChar::LineSeparator));
        cursor.insertText(body, body_format(outgoing));
    }

    scrollbar->setValue(was_at_bottom
        ? scrollbar->maximum()
        : std::min(previous, scrollbar->maximum()));
}

void ConversationSurface::paintEvent(QPaintEvent *event) {
    auto *surface_viewport = viewport();
    QPainter painter(surface_viewport);
    painter.setClipRect(event->rect());
    // The chat backdrop is the existing light palette token (windowBgOver),
    // the same warm-neutral field the shell list surface paints, so the
    // non-bubble area reads light rather than a forced-alpha composite.
    painter.fillRect(event->rect(), st::windowBgOver);

    const auto h_offset = horizontalScrollBar()->value();
    const auto v_offset = verticalScrollBar()->value();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    for (auto block = document()->begin(); block.isValid();
         block = block.next()) {
        if (!block.blockFormat().property(kMessageBlockProperty).toBool()) {
            continue;
        }
        const auto *block_layout = block.layout();
        auto text_bounds = QRectF();
        for (auto i = 0; i != block_layout->lineCount(); ++i) {
            const auto line = block_layout->lineAt(i);
            const auto line_bounds = line.naturalTextRect()
                .translated(block_layout->position());
            text_bounds = text_bounds.isNull()
                ? line_bounds
                : text_bounds.united(line_bounds);
        }
        if (text_bounds.isNull()) {
            continue;
        }
        text_bounds.translate(-h_offset, -v_offset);
        const auto bubble = text_bounds.adjusted(
            -kBubbleHPadding,
            -kBubbleVPadding,
            kBubbleHPadding,
            kBubbleVPadding);
        if (!bubble.intersects(QRectF(event->rect()))) {
            continue;
        }
        const auto outgoing = block.blockFormat().alignment()
            .testFlag(Qt::AlignRight);
        painter.setBrush(outgoing ? st::msgOutBg : st::msgInBg);
        painter.drawRoundedRect(bubble, kBubbleRadius, kBubbleRadius);
    }
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.end();

    // Let Qt draw the text with its native scroll translation, selection,
    // copy, and accessibility behavior; the widget's transparent Base keeps
    // the custom backdrop and bubbles visible underneath.
    QTextEdit::paintEvent(event);
}

void ConversationSurface::resizeEvent(QResizeEvent *event) {
    QTextEdit::resizeEvent(event);
    if (!last_plain_state_.isEmpty()) {
        apply_plain_state_formatting(
            document(), viewport()->width(), viewport()->height());
    }
    // Quantize the viewport/layout width so a live resize only reflows when
    // the bound meaningfully changes. This follows the full layout width, not
    // just the capped message width: on a very wide pane the centered reading
    // column's outer gutters keep moving after the message cap stops.
    const auto width = int(viewport()->width() / 8) * 8;
    if (width == last_layout_width_) {
        return;
    }
    last_layout_width_ = width;
    if (!last_messages_.empty()) {
        rebuild_document(last_messages_);
    }
}

} // namespace lingtai::desktop
