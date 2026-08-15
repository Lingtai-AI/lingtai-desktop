#include "ui/conversation_surface.h"

#include "base/basic_types.h"
#include "styles/palette.h"
#include "ui/style/style_core_scale.h"

#include "direct_conversation_history.h"

#include <QtGui/QFont>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCharFormat>
#include <QtGui/QTextDocument>
#include <QtGui/QTextFragment>
#include <QtWidgets/QApplication>

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace lingtai::desktop {
namespace {

struct FragmentView {
    QString text;
    QFont font;
};

std::vector<FragmentView> fragments_of(const QTextBlock &block) {
    std::vector<FragmentView> fragments;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        if (fragment.isValid()) {
            fragments.push_back({fragment.text(),
                fragment.charFormat().font()});
        }
    }
    return fragments;
}

const FragmentView &require_fragment(
        const std::vector<FragmentView> &fragments,
        const QString &text,
        bool exact,
        const char *role) {
    for (const auto &fragment : fragments) {
        if ((exact && fragment.text == text)
            || (!exact && fragment.text.startsWith(text))) {
            return fragment;
        }
    }
    throw std::runtime_error(
        std::string("expected the ") + role + " text '" + text.toStdString()
        + "' to be rendered as its own distinct QTextFragment with its own "
          "font, but no separate fragment with that text exists in the "
          "conversation document");
}

void require_font(
        const FragmentView &fragment,
        int pixel_size,
        int weight,
        const char *role) {
    if (fragment.font.pixelSize() != pixel_size
        || fragment.font.weight() != weight) {
        throw std::runtime_error(
            std::string("the ") + role + " fragment '"
            + fragment.text.toStdString() + "' must render at "
            + std::to_string(pixel_size) + "px weight "
            + std::to_string(weight) + ", but it renders at "
            + std::to_string(fragment.font.pixelSize()) + "px weight "
            + std::to_string(fragment.font.weight()));
    }
}

void require_hierarchy(
        const char *direction,
        int author_size,
        int body_size,
        int timestamp_size,
        int subject_size) {
    if (!(author_size >= body_size && body_size > timestamp_size
            && body_size > subject_size)) {
        throw std::runtime_error(
            std::string("the ") + direction
            + " message must read author >= body > metadata (author "
            + std::to_string(author_size) + "px, body "
            + std::to_string(body_size) + "px, timestamp "
            + std::to_string(timestamp_size) + "px, subject "
            + std::to_string(subject_size) + "px)");
    }
}

void verify_typography(ConversationSurface &surface, const QString &them) {
    std::vector<DirectConversationMessage> messages;
    messages.push_back({
        .id = "in-1",
        .outgoing = false,
        .timestamp = "2026-08-07T18:48:52Z",
        .subject = "Slice done",
        .text = "PR published, not merged.",
    });
    messages.push_back({
        .id = "out-1",
        .outgoing = true,
        .timestamp = "2026-08-07T19:00:00Z",
        .subject = "Re: Slice done",
        .text = "Thanks, reviewing tomorrow.",
    });
    surface.set_conversation(them, messages);

    auto incoming = QTextBlock();
    auto outgoing = QTextBlock();
    for (auto block = surface.document()->begin(); block.isValid();
         block = block.next()) {
        if (block.text().startsWith(them + QStringLiteral(" ·"))) {
            incoming = block;
        } else if (block.text().startsWith(QStringLiteral("You ·"))) {
            outgoing = block;
        }
    }
    if (!incoming.isValid() || !outgoing.isValid()) {
        throw std::runtime_error(
            "the surface must render one incoming and one outgoing message "
            "block for the typography contract");
    }

    const auto incoming_fragments = fragments_of(incoming);
    const auto outgoing_fragments = fragments_of(outgoing);

    // author/sender: 15px semibold, its own fragment for each direction.
    const auto &in_sender = require_fragment(
        incoming_fragments, them, true, "incoming sender");
    require_font(in_sender, 15, QFont::DemiBold, "incoming sender");
    const auto &out_sender = require_fragment(
        outgoing_fragments, QStringLiteral("You"), true, "outgoing sender");
    require_font(out_sender, 15, QFont::DemiBold, "outgoing sender");

    // timestamp and subject metadata: 13px.
    const auto &in_timestamp = require_fragment(
        incoming_fragments, QStringLiteral(" · "), false, "incoming timestamp");
    require_font(in_timestamp, 13, QFont::Normal, "incoming timestamp");
    const auto &out_timestamp = require_fragment(
        outgoing_fragments, QStringLiteral(" · "), false, "outgoing timestamp");
    require_font(out_timestamp, 13, QFont::Normal, "outgoing timestamp");
    const auto &in_subject = require_fragment(
        incoming_fragments, QStringLiteral("Slice done"), false,
        "incoming subject");
    require_font(in_subject, 13, QFont::Medium, "incoming subject");
    const auto &out_subject = require_fragment(
        outgoing_fragments, QStringLiteral("Re: Slice done"), false,
        "outgoing subject");
    require_font(out_subject, 13, QFont::Medium, "outgoing subject");

    // message body: 14px normal.
    const auto &in_body = require_fragment(
        incoming_fragments, QStringLiteral("PR published, not merged."), true,
        "incoming body");
    require_font(in_body, 14, QFont::Normal, "incoming body");
    const auto &out_body = require_fragment(
        outgoing_fragments, QStringLiteral("Thanks, reviewing tomorrow."), true,
        "outgoing body");
    require_font(out_body, 14, QFont::Normal, "outgoing body");

    // The pinned visual hierarchy for both directions.
    require_hierarchy("incoming", in_sender.font.pixelSize(),
        in_body.font.pixelSize(), in_timestamp.font.pixelSize(),
        in_subject.font.pixelSize());
    require_hierarchy("outgoing", out_sender.font.pixelSize(),
        out_body.font.pixelSize(), out_timestamp.font.pixelSize(),
        out_subject.font.pixelSize());
}

} // namespace

int run_typography_test(int argc, char **argv) {
    try {
        QApplication application(argc, argv);
        style::internal::init_palette(style::kScaleDefault);
        ConversationSurface surface;
        surface.resize(640, 480);
        verify_typography(surface, QStringLiteral("Telegram Bot"));
        std::cout << "conversation surface typography: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "conversation surface typography: " << error.what()
                  << '\n';
        return 1;
    }
}

} // namespace lingtai::desktop

int main(int argc, char **argv) {
    return lingtai::desktop::run_typography_test(argc, argv);
}
