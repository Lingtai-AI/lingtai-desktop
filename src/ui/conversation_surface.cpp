#include "ui/conversation_surface.h"

#include "base/basic_types.h"
#include "styles/palette.h"

#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QFontMetricsF>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtGui/QPalette>
#include <QtGui/QPixmap>
#include <QtGui/QResizeEvent>
#include <QtGui/QTextBlock>
#include <QtGui/QTextBlockFormat>
#include <QtGui/QTextCharFormat>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtGui/QTextFormat>
#include <QtGui/QTextFrame>
#include <QtGui/QTextImageFormat>
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
constexpr auto kEmptyAvatarDiameter = 32;
constexpr auto kEmptyAvatarGap = 10;
constexpr auto kEmptyTitleGap = 6;
constexpr auto kEmptyStateHeadroom = 28;
constexpr auto kEmptyStateTitleSize = 15;
constexpr auto kEmptyStateAvatarLetterSize = 14;

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
    font.setPixelSize(15);
    font.setWeight(QFont::DemiBold);
    format.setFont(font);
    return format;
}

QTextCharFormat code_text_format(const QTextCharFormat &base) {
    auto format = base;
    auto font = format.font();
    font.setFixedPitch(true);
    font.setStyleHint(QFont::Monospace);
    format.setFont(font);
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
        const QTextBlockFormat &continuation) {
    const auto bold = emphasized_text_format(base);
    const auto heading = heading_text_format(base);
    const auto code = code_text_format(base);
    const auto quote = quote_text_format(base);

    // The inline pass over one line's plain remainder: **bold**, `code`, and
    // [label](url) become real formatted runs, everything else stays literal.
    const auto insert_inline = [&](
            const QString &line,
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
                cursor.insertText(line.mid(index), plain);
                break;
            }
            if (at > index) {
                cursor.insertText(line.mid(index, at - index), plain);
            }
            if (kind == 2) {
                const auto close = line.indexOf(QChar('`'), at + 1);
                if (close < 0) {
                    cursor.insertText(line.mid(at), plain);
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
                    cursor.insertText(line.mid(at), plain);
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
                    cursor.insertText(line.mid(at), plain);
                    break;
                }
                cursor.insertText(line.mid(at + 2, close - at - 2), bold);
                index = close + 2;
            }
        }
    };

    auto in_code_fence = false;
    for (const auto &line : body.split(QChar::LineSeparator)) {
        if (in_code_fence) {
            if (line.startsWith(QStringLiteral("```"))) {
                in_code_fence = false;
                continue;
            }
        } else if (line.startsWith(QStringLiteral("```"))) {
            in_code_fence = true;
            continue;
        }
        cursor.insertBlock(continuation);
        if (in_code_fence) {
            cursor.insertText(line, code);
        } else if (line.startsWith(QChar('#'))) {
            cursor.insertText(line.mid(1).trimmed(), heading);
        } else if (line.startsWith(QStringLiteral("- "))) {
            cursor.insertText(QStringLiteral("- "), base);
            insert_inline(line.mid(2), base);
        } else if (line.startsWith(QStringLiteral("> "))) {
            insert_inline(line.mid(2), quote);
        } else {
            insert_inline(line, base);
        }
    }
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

QTextCharFormat empty_state_title_format() {
    auto format = QTextCharFormat();
    format.setForeground(st::msgServiceFg);
    auto font = format.font();
    font.setPixelSize(kEmptyStateTitleSize);
    font.setWeight(QFont::Normal);
    format.setFont(font);
    return format;
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
    painter.setBrush(st::msgServiceFg);
    painter.drawEllipse(0, 0, diameter, diameter);
    auto font = QFont();
    font.setPixelSize(kEmptyStateAvatarLetterSize);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(st::windowBg);
    painter.drawText(QRect(0, 0, diameter, diameter), Qt::AlignCenter, initial);
    painter.end();
    return pixmap;
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
    empty_state_active_ = false;
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
    empty_state_active_ = messages.empty();
    if (messages.empty()) {
        rebuild_empty_state();
    } else {
        rebuild_document(messages);
    }
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

    // One transparent borderless sibling QTextFrame per message under the root
    // frame, each owning its header and body blocks: the frame's first block
    // is the header carrying the message block format, and every body logical
    // line becomes its own real unmarked QTextBlock cloned from that format,
    // so the standard layout honors the block alignment and the margins bind
    // each message to the shared reading-column width.
    const auto separator = QString(QChar::LineSeparator);
    const auto viewport_width = viewport()->width();
    const auto lane_max = message_block_width(viewport_width);
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
        // A fresh document-end cursor places every message as its own direct
        // sibling QTextFrame under the root frame, never nested. The frame is
        // transparent with no border, padding, or margin, so only the first
        // block's own alignment and margins shape the message lane.
        auto cursor = QTextCursor(document);
        cursor.movePosition(QTextCursor::End);
        auto frame_format = QTextFrameFormat();
        frame_format.setBorder(0);
        frame_format.setPadding(0);
        frame_format.setMargin(0);
        frame_format.setBackground(Qt::transparent);
        auto *frame = cursor.insertFrame(frame_format);
        cursor = frame->firstCursorPosition();
        cursor.setBlockFormat(block_format);
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
        insert_markdown_body(
            cursor, body, body_format(outgoing), continuation);
    }

    scrollbar->setValue(was_at_bottom
        ? scrollbar->maximum()
        : std::min(previous, scrollbar->maximum()));
}

void ConversationSurface::rebuild_empty_state() {
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
    if (empty_state_active_) {
        rebuild_empty_state();
    } else if (!last_messages_.empty()) {
        rebuild_document(last_messages_);
    }
}

} // namespace lingtai::desktop
