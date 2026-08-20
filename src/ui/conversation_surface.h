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
// the existing direct-conversation rows. Drag-select uses a pale glyph-tight
// wash; hovering a message paints a Slack-like full-width row tint and left
// accent without an action toolbar.
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

    // Design empty state when no Agent is selected: illustration, title,
    // helper copy, and an optional quieter main-agent hint.
    void set_select_agent_prompt(const QString &main_agent_name);

    // Pin the viewport to the laid-out document bottom. Used after a send and
    // after a rebuild that started already at the bottom, so extra height such
    // as a new-day separator is included instead of a stale scrollbar maximum.
    void scroll_to_bottom();

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
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void reflow_to_viewport();
    void rebuild_document();
    void rebuild_empty_state();
    void rebuild_select_agent_prompt();
    void reveal_older();
    void scroll_to_bottom_now();
    void update_hovered_message(const QPoint &viewport_pos);
    void clear_hovered_message();
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
    QString select_agent_main_name_;
    QString hovered_message_id_;
    bool empty_state_active_ = false;
    bool select_agent_prompt_active_ = false;
    int last_layout_width_ = 0;
    // The number of oldest cached rows still hidden above the visible window;
    // also the count shown by the leading banner when it is nonzero.
    int history_offset_ = 0;
    // A scrollbar-driven viewport resize can arrive while QTextDocument is
    // being rebuilt. Ignore that nested reflow instead of recursively clearing
    // and appending into the same document.
    bool rebuild_in_progress_ = false;
    // Cancels a deferred bottom pin when the human scrolls away or a rebuild
    // restores a non-bottom position before the queued pass runs.
    int scroll_pin_generation_ = 0;
};

} // namespace lingtai::desktop
