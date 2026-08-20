#pragma once

#include "direct_conversation_history.h"
#include "message_reactions.h"

#include <QtWidgets/QTextEdit>

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

class QKeyEvent;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;

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
    // Session reaction bags are keyed by message id and painted as Telegram-
    // like in-bubble chips (receipts and peer reactions).
    void set_conversation(
        const QString &them,
        const std::vector<DirectConversationMessage> &messages,
        const std::unordered_map<std::string, MessageReactions> &reactions = {});

    // One plain centered state for the selection/no-route/empty cases.
    void set_plain_state(const QString &text);

protected:
    // Fills the viewport with the chat backdrop, paints the rounded message
    // bubbles behind the text, then lets the document layout draw the text.
    void paintEvent(QPaintEvent *event) override;
    // Recomputes the content-driven message widths and reflows the document
    // when the quantized layout width meaningfully changes.
    void resizeEvent(QResizeEvent *event) override;
    // Hidden pages skip layout; becoming visible again must reflow even when
    // Qt does not send a resize because sizeHint kept the stale width.
    void showEvent(QShowEvent *event) override;
    // Ctrl+U at the top reveals the next older page of the cached history.
    void keyPressEvent(QKeyEvent *event) override;

private:
    void reflow_to_viewport();
    void rebuild_document();
    void rebuild_empty_state();
    void reveal_older();
    [[nodiscard]] bool same_content(
        const std::vector<DirectConversationMessage> &messages,
        const std::unordered_map<std::string, MessageReactions> &reactions)
        const;

    // The render-time history window reveals the cached rows in fixed pages:
    // initially only the chronological tail is materialized and each reveal
    // brings in one more page of older rows.
    static constexpr int kHistoryPageSize = 100;

    QString them_;
    std::vector<DirectConversationMessage> last_messages_;
    std::unordered_map<std::string, MessageReactions> last_reactions_;
    QString last_plain_state_;
    bool empty_state_active_ = false;
    int last_layout_width_ = 0;
    // The number of oldest cached rows still hidden above the visible window;
    // also the count shown by the leading banner when it is nonzero.
    int history_offset_ = 0;
    // A scrollbar-driven viewport resize can arrive while QTextDocument is
    // being rebuilt. Ignore that nested reflow instead of recursively clearing
    // and appending into the same document.
    bool rebuild_in_progress_ = false;
};

} // namespace lingtai::desktop
