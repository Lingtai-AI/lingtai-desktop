#include "ui/conversation_surface.h"

#include "base/basic_types.h"
#include "styles/palette.h"
#include "ui/style/style_core_scale.h"

#include "direct_conversation_history.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QFont>
#include <QtGui/QTextBlock>
#include <QtGui/QTextBlockFormat>
#include <QtGui/QTextCharFormat>
#include <QtGui/QTextDocument>
#include <QtGui/QTextFragment>
#include <QtWidgets/QApplication>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
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

struct MessageGeometry {
    double left;
    double right;
    double content_width;
    double content_ratio;
};

MessageGeometry message_geometry(
        const QTextBlock &block, int viewport_width) {
    const auto format = block.blockFormat();
    const auto left = format.leftMargin();
    const auto right = format.rightMargin();
    const auto content_width = double(viewport_width) - left - right;
    return {left, right, content_width, content_width / double(viewport_width)};
}

std::pair<MessageGeometry, MessageGeometry> measure_at_width(int width) {
    ConversationSurface surface;
    surface.resize(width, 480);
    surface.show();
    QCoreApplication::processEvents();

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
    surface.set_conversation(QStringLiteral("Telegram Bot"), messages);

    auto incoming = QTextBlock();
    auto outgoing = QTextBlock();
    for (auto block = surface.document()->begin(); block.isValid();
         block = block.next()) {
        if (block.text().startsWith(QStringLiteral("Telegram Bot ·"))) {
            incoming = block;
        } else if (block.text().startsWith(QStringLiteral("You ·"))) {
            outgoing = block;
        }
    }
    if (!incoming.isValid() || !outgoing.isValid()) {
        throw std::runtime_error(
            "the surface must render one incoming and one outgoing message "
            "block for the responsive width contract");
    }
    const auto viewport_width = surface.viewport()->width();
    return {
        message_geometry(incoming, viewport_width),
        message_geometry(outgoing, viewport_width),
    };
}

void verify_responsive_width() {
    // Very wide: both messages must share one centered reading column while
    // keeping opposite anchors inside it, and the message width is capped
    // near the design's ~636px readable target instead of stretching with the
    // pane. The shared column is proven by cross-direction symmetry: the
    // incoming outer-left matches the outgoing outer-right and the incoming
    // inner-right matches the outgoing inner-left.
    const auto wide = measure_at_width(1600);
    const auto wide_in = wide.first;
    const auto wide_out = wide.second;
    if (!(wide_in.left < wide_in.right
            && wide_out.right < wide_out.left)) {
        throw std::runtime_error(
            "at a very wide viewport the messages must keep opposite anchors "
            "inside the shared reading column, incoming left and outgoing "
            "right");
    }
    if (std::abs(wide_in.left - wide_out.right) > 2.0
        || std::abs(wide_in.right - wide_out.left) > 2.0) {
        throw std::runtime_error(
            "at a very wide viewport the messages must share one centered "
            "reading column: incoming outer-left must match outgoing "
            "outer-right and incoming inner-right must match outgoing "
            "inner-left");
    }
    if (wide_in.content_width < 580.0 || wide_in.content_width > 700.0
        || wide_out.content_width < 580.0
        || wide_out.content_width > 700.0) {
        throw std::runtime_error(
            "at a very wide viewport the message width must stop at an "
            "absolute readable cap around 636px, but it stretches "
            "with the pane");
    }

    // Normal: incoming/outgoing stay opposite-aligned and keep the
    // approximate 65-75% pane share.
    const auto normal = measure_at_width(640);
    const auto normal_in = normal.first;
    const auto normal_out = normal.second;
    if (!(normal_in.left < normal_in.right
            && normal_out.right < normal_out.left)) {
        throw std::runtime_error(
            "at a normal viewport the messages must stay opposite-aligned, "
            "incoming left and outgoing right");
    }
    if (normal_in.content_ratio < 0.65 || normal_in.content_ratio > 0.75
        || normal_out.content_ratio < 0.65
        || normal_out.content_ratio > 0.75) {
        throw std::runtime_error(
            "at a normal viewport the message width must keep its "
            "approximate 65-75% pane share");
    }

    // Narrow: the message width becomes near-full rather than the fixed 72%.
    const auto narrow = measure_at_width(320);
    const auto narrow_in = narrow.first;
    const auto narrow_out = narrow.second;
    if (narrow_in.content_ratio < 0.90
        || narrow_out.content_ratio < 0.90) {
        throw std::runtime_error(
            "at a narrow viewport the message width must become near-full "
            "(~90%+) instead of the current 72%");
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
        verify_responsive_width();
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
