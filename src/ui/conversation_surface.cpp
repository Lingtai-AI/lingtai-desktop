#include "ui/conversation_surface.h"

#include "base/basic_types.h"
#include "styles/palette.h"

#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QFont>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtGui/QPalette>
#include <QtGui/QResizeEvent>
#include <QtGui/QTextBlock>
#include <QtGui/QTextBlockFormat>
#include <QtGui/QTextCharFormat>
#include <QtGui/QTextCursor>
#include <QtGui/QTextFormat>
#include <QtGui/QTextLayout>
#include <QtWidgets/QScrollBar>

#include <algorithm>
#include <utility>

namespace lingtai::desktop {
namespace {

constexpr auto kDocumentMargin = 4;
constexpr auto kMessageEdgeMargin = 12;
constexpr auto kMessageTopMargin = 4;
constexpr auto kMessageBottomMargin = 12;
constexpr auto kMessageCapRatio = 0.72;
constexpr auto kMinMessageWidth = 160;
constexpr auto kBubbleHPadding = 10;
constexpr auto kBubbleVPadding = 4;
constexpr auto kBubbleRadius = 8;
constexpr auto kBackdropOverlayAlpha = 0x80;
constexpr auto kMessageBlockProperty = QTextFormat::UserProperty + 1;

QTextBlockFormat message_block_format(bool outgoing, int viewport_width) {
    auto format = QTextBlockFormat();
    format.setAlignment(outgoing ? Qt::AlignRight : Qt::AlignLeft);
    const auto cap = qMax(
        kMessageCapRatio * viewport_width,
        qreal(kMinMessageWidth));
    const auto opposite = qMax(
        qreal(kMessageEdgeMargin),
        viewport_width - cap - kMessageEdgeMargin);
    format.setLeftMargin(outgoing ? opposite : kMessageEdgeMargin);
    format.setRightMargin(outgoing ? kMessageEdgeMargin : opposite);
    format.setTopMargin(kMessageTopMargin);
    format.setBottomMargin(kMessageBottomMargin);
    format.setProperty(kMessageBlockProperty, true);
    return format;
}

QTextCharFormat secondary_format() {
    auto format = QTextCharFormat();
    format.setForeground(st::msgServiceFg);
    auto font = format.font();
    font.setPointSize(10);
    format.setFont(font);
    return format;
}

QTextCharFormat subject_format(bool outgoing) {
    auto format = QTextCharFormat();
    format.setForeground(outgoing ? st::historyTextOutFg
                                  : st::historyTextInFg);
    auto font = format.font();
    font.setPointSize(11);
    font.setWeight(QFont::Medium);
    format.setFont(font);
    return format;
}

QTextCharFormat body_format(bool outgoing) {
    auto format = QTextCharFormat();
    format.setForeground(outgoing ? st::historyTextOutFg
                                  : st::historyTextInFg);
    auto font = format.font();
    font.setPointSize(12);
    format.setFont(font);
    return format;
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
        QTextBlockFormat centered;
        centered.setAlignment(Qt::AlignCenter);
        auto cursor = QTextCursor(document());
        cursor.select(QTextCursor::Document);
        cursor.mergeBlockFormat(centered);
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
    auto cursor = QTextCursor(document);
    cursor.movePosition(QTextCursor::Start);

    // One message per QTextBlock; the header/subject/body lines are separated
    // inside the block so the standard layout honors the block alignment and
    // the margins bound each message near 72% of the viewport width.
    const auto separator = QString(QChar::LineSeparator);
    const auto header = secondary_format();
    const auto viewport_width = viewport()->width();
    auto first_block = true;
    for (const auto &message : messages) {
        const auto outgoing = message.outgoing;
        const auto block_format = message_block_format(outgoing, viewport_width);
        if (first_block) {
            cursor.setBlockFormat(block_format);
            first_block = false;
        } else {
            cursor.insertBlock(block_format);
        }
        cursor.insertText(
            QStringLiteral("%1 · %2")
                .arg(outgoing ? QStringLiteral("You") : them_,
                    QString::fromStdString(message.timestamp)),
            header);
        cursor.insertText(separator, header);
        if (!message.subject.empty()) {
            cursor.insertText(QString::fromStdString(message.subject),
                subject_format(outgoing));
            cursor.insertText(separator, subject_format(outgoing));
        }
        // Message text stays literal: the surface never interprets markup.
        cursor.insertText(QString::fromStdString(message.text),
            body_format(outgoing));
    }

    scrollbar->setValue(was_at_bottom
        ? scrollbar->maximum()
        : std::min(previous, scrollbar->maximum()));
}

void ConversationSurface::paintEvent(QPaintEvent *event) {
    auto *surface_viewport = viewport();
    QPainter painter(surface_viewport);
    painter.setClipRect(event->rect());
    painter.fillRect(event->rect(), st::windowBg);
    auto scroll_overlay = QColor(st::historyScrollBg->c);
    scroll_overlay.setAlpha(kBackdropOverlayAlpha);
    painter.fillRect(event->rect(), scroll_overlay);

    const auto h_offset = horizontalScrollBar()->value();
    const auto v_offset = verticalScrollBar()->value();
    auto *layout = document()->documentLayout();

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    for (auto block = document()->begin(); block.isValid();
         block = block.next()) {
        if (!block.blockFormat().property(kMessageBlockProperty).toBool()) {
            continue;
        }
        const auto rect = layout->blockBoundingRect(block)
            .translated(-h_offset, -v_offset);
        if (!rect.intersects(QRectF(event->rect()))) {
            continue;
        }
        auto widest = 0.0;
        const auto *block_layout = block.layout();
        const auto line_count = block_layout->lineCount();
        if (line_count == 0) {
            continue;
        }
        const auto first_line = block_layout->lineAt(0);
        const auto last_line = block_layout->lineAt(line_count - 1);
        for (auto i = 0; i != line_count; ++i) {
            widest = qMax(widest, block_layout->lineAt(i).naturalTextWidth());
        }
        if (widest <= 0.0) {
            continue;
        }
        // The bubble hugs the actual text lines only, translated by the block
        // rect top, so the block's own top/bottom margins stay outside the
        // bubble and adjacent bubbles never touch or overlap.
        const auto content_top = rect.top() + first_line.y();
        const auto content_bottom = rect.top() + last_line.y()
            + last_line.height();
        const auto outgoing = block.blockFormat().alignment()
            .testFlag(Qt::AlignRight);
        QRectF bubble;
        if (outgoing) {
            bubble = QRectF(
                rect.right() - widest - kBubbleHPadding,
                content_top - kBubbleVPadding,
                widest + 2 * kBubbleHPadding,
                (content_bottom - content_top) + 2 * kBubbleVPadding);
        } else {
            bubble = QRectF(
                rect.left() - kBubbleHPadding,
                content_top - kBubbleVPadding,
                widest + 2 * kBubbleHPadding,
                (content_bottom - content_top) + 2 * kBubbleVPadding);
        }
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
    // Quantize the viewport-bounded message width so a live resize only
    // reflows when the bound meaningfully changes, not on every pixel step.
    const auto cap = int(viewport()->width() * kMessageCapRatio / 8) * 8;
    if (cap == message_cap_width_) {
        return;
    }
    message_cap_width_ = cap;
    if (!last_messages_.empty()) {
        rebuild_document(last_messages_);
    }
}

} // namespace lingtai::desktop
