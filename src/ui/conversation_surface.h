#pragma once

#include "direct_conversation_history.h"

#include <QtWidgets/QTextEdit>

#include <cstddef>
#include <string>
#include <vector>

class QPaintEvent;
class QResizeEvent;

namespace lingtai::desktop {

// One LingTai-owned, read-only, text-selectable conversation surface for the
// selected Agent. It stays a plain QTextEdit so the inherited selection, copy,
// and accessibility behavior is preserved; the visible conversation is rebuilt
// programmatically with QTextCursor/QTextBlockFormat/QTextCharFormat blocks
// (one message per block, incoming left / outgoing right, with the shared
// palette's distinct rounded bubble backgrounds painted behind the text) from
// the existing direct-conversation rows.
class ConversationSurface final : public QTextEdit {
    Q_OBJECT
public:
    explicit ConversationSurface(QWidget *parent = nullptr);

    // Replaces the document from the existing direct rows in their accepted
    // chronological order. Changed contents only: an identical refresh is a
    // no-op that preserves scroll, selection, and focus. The sender/direction
    // label shown for incoming rows is the caller-chosen presentation name.
    void set_conversation(
        const QString &them,
        const std::vector<DirectConversationMessage> &messages);

    // One plain centered state for the selection/no-route/empty cases.
    void set_plain_state(const QString &text);

protected:
    // Fills the viewport with the chat backdrop, paints the rounded message
    // bubbles behind the text, then lets the document layout draw the text.
    void paintEvent(QPaintEvent *event) override;
    // Recomputes the content-driven message widths and reflows the document
    // when the quantized layout width meaningfully changes.
    void resizeEvent(QResizeEvent *event) override;

private:
    void rebuild_document(
        const std::vector<DirectConversationMessage> &messages);
    [[nodiscard]] bool same_content(
        const std::vector<DirectConversationMessage> &messages) const;

    QString them_;
    std::vector<DirectConversationMessage> last_messages_;
    QString last_plain_state_;
    int last_layout_width_ = 0;
};

} // namespace lingtai::desktop
