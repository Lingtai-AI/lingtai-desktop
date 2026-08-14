#pragma once

#include "direct_conversation_history.h"

#include <QtWidgets/QPlainTextEdit>

#include <cstddef>
#include <string>
#include <vector>

namespace lingtai::desktop {

// One LingTai-owned, read-only, text-selectable conversation surface for the
// selected Agent. It stays a plain QPlainTextEdit so the inherited selection,
// copy, and accessibility behavior is preserved; the visible conversation is
// rebuilt programmatically with QTextCursor/QTextBlockFormat/QTextCharFormat
// blocks (incoming left / outgoing right, with the shared palette's distinct
// bubble backgrounds) from the existing direct-conversation rows.
class ConversationSurface final : public QPlainTextEdit {
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

private:
    void rebuild_document(
        const std::vector<DirectConversationMessage> &messages);
    [[nodiscard]] bool same_content(
        const std::vector<DirectConversationMessage> &messages) const;

    QString them_;
    std::vector<DirectConversationMessage> last_messages_;
    QString last_plain_state_;
};

} // namespace lingtai::desktop
