#include "ui/conversation_surface.h"

#include "base/basic_types.h"
#include "styles/palette.h"

#include <QtGui/QFont>
#include <QtGui/QTextBlockFormat>
#include <QtGui/QTextCharFormat>
#include <QtGui/QTextCursor>
#include <QtWidgets/QScrollBar>

#include <algorithm>
#include <utility>

namespace lingtai::desktop {
namespace {

constexpr auto kMessageEdgeMargin = 6;
constexpr auto kMessageOppositeMargin = 48;
constexpr auto kMessageTopMargin = 4;
constexpr auto kMessageBottomMargin = 4;

QTextBlockFormat message_block_format(bool outgoing) {
    auto format = QTextBlockFormat();
    format.setAlignment(outgoing ? Qt::AlignRight : Qt::AlignLeft);
    format.setBackground(outgoing ? st::msgOutBg : st::msgInBg);
    format.setLeftMargin(
        outgoing ? kMessageOppositeMargin : kMessageEdgeMargin);
    format.setRightMargin(
        outgoing ? kMessageEdgeMargin : kMessageOppositeMargin);
    format.setTopMargin(kMessageTopMargin);
    format.setBottomMargin(kMessageBottomMargin);
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
: QPlainTextEdit(parent) {
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setUndoRedoEnabled(false);
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

    const auto append_block = [&](const QTextBlockFormat &block_format,
            const QTextCharFormat &char_format, const QString &text) {
        if (!cursor.atStart()) {
            cursor.insertBlock(block_format, char_format);
        } else {
            cursor.setBlockFormat(block_format);
            cursor.setCharFormat(char_format);
        }
        cursor.insertText(text, char_format);
    };

    const auto header = secondary_format();
    for (const auto &message : messages) {
        const auto outgoing = message.outgoing;
        const auto block_format = message_block_format(outgoing);
        append_block(
            block_format,
            header,
            QStringLiteral("%1 · %2")
                .arg(outgoing ? QStringLiteral("You") : them_,
                    QString::fromStdString(message.timestamp)));
        if (!message.subject.empty()) {
            append_block(block_format, subject_format(outgoing),
                QString::fromStdString(message.subject));
        }
        // Message text stays literal: the surface never interprets markup.
        append_block(block_format, body_format(outgoing),
            QString::fromStdString(message.text));
    }

    scrollbar->setValue(was_at_bottom
        ? scrollbar->maximum()
        : std::min(previous, scrollbar->maximum()));
}

} // namespace lingtai::desktop
