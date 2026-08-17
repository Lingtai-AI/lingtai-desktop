#include "ui/conversation_surface.h"

#include "base/basic_types.h"
#include "styles/palette.h"
#include "ui/style/style_core_scale.h"

#include "direct_conversation_history.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QImage>
#include <QtGui/QPixmap>
#include <QtGui/QTextBlock>
#include <QtGui/QTextBlockFormat>
#include <QtGui/QTextCharFormat>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtGui/QTextFragment>
#include <QtWidgets/QScrollBar>
#include <QtGui/QTextFrame>
#include <QtGui/QTextLayout>
#include <QtWidgets/QApplication>

#include <algorithm>
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

std::vector<FragmentView> fragments_of(const QTextFrame &frame) {
    std::vector<FragmentView> fragments;
    for (auto it = frame.begin(); !it.atEnd(); ++it) {
        const auto block = it.currentBlock();
        if (!block.isValid()) {
            continue;
        }
        const auto block_fragments = fragments_of(block);
        fragments.insert(fragments.end(), block_fragments.begin(),
            block_fragments.end());
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
    if (!(body_size > author_size && author_size > timestamp_size
            && author_size > subject_size)) {
        throw std::runtime_error(
            std::string("the ") + direction
            + " message must read body > author > metadata (author "
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

    // Genuinely long realistic multi-clause content: under the content-driven
    // width contract these messages must naturally occupy and clamp to the
    // existing shared lane (the 640px wide cap, the ordinary 72% ratio at the
    // normal viewport, and the near-full narrow width) instead of shrinking to
    // a short line, so the shared-lane assertions below stay reachable.
    std::vector<DirectConversationMessage> messages;
    messages.push_back({
        .id = "in-1",
        .outgoing = false,
        .timestamp = "2026-08-07T18:48:52Z",
        .subject = "Direct-message projection is ready for review",
        .text = "The direct-message projection now routes the selected "
                "agent's conversation through the existing reader, and the "
                "message body is left literally untouched so the reviewer "
                "sees the exact stored text instead of a re-interpreted "
                "rendering, which matters because the test fixtures "
                "intentionally carry punctuation that a naive formatter "
                "would reshape.",
    });
    messages.push_back({
        .id = "out-1",
        .outgoing = true,
        .timestamp = "2026-08-07T19:00:00Z",
        .subject = "Re: Direct-message projection is ready for review",
        .text = "I pulled the branch and ran the focused conversation tests "
                "against the fixture corpus, and everything passes on a "
                "clean checkout, so please go ahead and leave any inline "
                "notes directly on the changed lines before we merge it "
                "into the mainline later this afternoon.",
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
    // pane. The shared column is proven by cross-direction symmetry across
    // the incoming avatar lane: the incoming envelope's outer-left (its text
    // left minus the 50px avatar lane) matches the outgoing outer-right and
    // the incoming envelope's inner-right (its text right plus the 50px
    // avatar lane) matches the outgoing inner-left.
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
    if (std::abs((wide_in.left - 50.0) - wide_out.right) > 2.0
        || std::abs((wide_in.right + 50.0) - wide_out.left) > 2.0) {
        throw std::runtime_error(
            "at a very wide viewport the messages must share one centered "
            "reading column: the incoming avatar envelope's outer/inner edges "
            "must align with the outgoing reading-column edges");
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

// Rebuilds the painted bubble for one whole message frame from the same
// detected rendered bounds paintEvent uses: the union of every laid-out line's
// natural text rect translated by its block layout position and the frame's
// document top-left, padded by the surface's bubble padding. Gaps between
// these rects are therefore the actual visible rhythm of the rendered stream,
// expressed relationally rather than as screenshot coordinates.
constexpr double kBubblePadding = 8.0;

QRectF message_text_bounds(QTextFrame &frame) {
    const auto frame_top_left = frame.document()->documentLayout()
        ->frameBoundingRect(&frame).topLeft();
    auto text_bounds = QRectF();
    for (auto it = frame.begin(); !it.atEnd(); ++it) {
        const auto block = it.currentBlock();
        if (!block.isValid()) {
            continue;
        }
        const auto *layout = block.layout();
        for (auto i = 0; i != layout->lineCount(); ++i) {
            const auto line = layout->lineAt(i);
            const auto line_bounds = line.naturalTextRect()
                .translated(layout->position())
                .translated(frame_top_left);
            text_bounds = text_bounds.isNull()
                ? line_bounds
                : text_bounds.united(line_bounds);
        }
    }
    return text_bounds;
}

QRectF message_bubble_rect(QTextFrame &frame) {
    return message_text_bounds(frame).adjusted(
        -kBubblePadding, -kBubblePadding, kBubblePadding, kBubblePadding);
}

void verify_content_geometry() {
    ConversationSurface surface;
    surface.resize(1600, 480);
    surface.show();
    QCoreApplication::processEvents();

    // One genuinely long incoming message and one genuinely short outgoing
    // message in the same wide state.
    std::vector<DirectConversationMessage> messages;
    messages.push_back({
        .id = "in-long",
        .outgoing = false,
        .timestamp = "2026-08-07T18:48:52Z",
        .subject = "Long report",
        .text = "This is a genuinely long incoming message whose body wraps "
                "well inside the accepted readable cap, so a content-driven "
                "renderer bounds it near the maximum and keeps it inside the "
                "shared centered reading column instead of stretching with "
                "the pane at a very wide window.",
    });
    messages.push_back({
        .id = "out-short",
        .outgoing = true,
        .timestamp = "2026-08-07T19:00:00Z",
        .subject = "Re: Long report",
        .text = "OK, go ahead.",
    });
    surface.set_conversation(QStringLiteral("Telegram Bot"), messages);
    surface.document()->documentLayout()->documentSize();
    QCoreApplication::processEvents();

    QTextBlock long_block;
    QTextBlock short_block;
    for (auto block = surface.document()->begin(); block.isValid();
         block = block.next()) {
        if (block.text().contains(
                QStringLiteral("This is a genuinely long incoming"))) {
            long_block = block;
        } else if (block.text().contains(QStringLiteral("OK, go ahead."))) {
            short_block = block;
        }
    }
    if (!long_block.isValid() || !short_block.isValid()) {
        throw std::runtime_error(
            "the surface must render one genuinely long and one genuinely "
            "short message block for the content-driven width contract");
    }

    const auto viewport_width = surface.viewport()->width();
    const auto long_geometry = message_geometry(long_block, viewport_width);
    const auto short_geometry = message_geometry(short_block, viewport_width);

    // Message widths are content-driven: in the same wide surface a genuinely
    // short message must render with a visibly narrower allocated width than
    // a longer message's, instead of every message owning the same fixed
    // 640px cap. The assertion is relational, not a screenshot width.
    if (!(short_geometry.content_width
            <= long_geometry.content_width * 0.75)) {
        throw std::runtime_error(
            "message widths must be content-driven: in the same wide surface "
            "a genuinely short message must render visibly narrower than a "
            "longer message, but both still own the same fixed allocated "
            "width (short "
            + std::to_string(short_geometry.content_width) + "px vs long "
            + std::to_string(long_geometry.content_width) + "px)");
    }

    // The shared centered lane's stable outer anchors and the opposite
    // incoming/outgoing directions stay intact for both messages under the
    // accepted R3 wide bounds. Only the outer lane equality is pinned: the two
    // messages share the centered lane's outer anchor pair, while their inner
    // edges differ with each message's content width. The long message is
    // incoming and reserves its 50px avatar lane (40px circle + 10px gap) on
    // its outer edge, so its left anchor sits that lane inside the shared
    // centered lane's left edge.
    if (!(long_geometry.left < long_geometry.right
            && short_geometry.right < short_geometry.left)) {
        throw std::runtime_error(
            "at a very wide viewport the long and short messages must keep "
            "opposite anchors, incoming left and outgoing right");
    }
    if (std::abs((long_geometry.left - 50.0) - short_geometry.right) > 2.0) {
        throw std::runtime_error(
            "at a very wide viewport the long and short messages must share "
            "the centered reading column's stable outer anchors, while their "
            "inner edges differ with each message's content width; the long "
            "incoming message's outer edge reserves its 50px avatar envelope");
    }
}

void verify_turn_rhythm() {
    ConversationSurface surface;
    surface.resize(1600, 480);
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
    messages.push_back({
        .id = "in-2",
        .outgoing = false,
        .timestamp = "2026-08-07T19:01:00Z",
        .subject = "Follow-up",
        .text = "Could you also update the CHANGELOG before we merge?",
    });
    messages.push_back({
        .id = "out-2",
        .outgoing = true,
        .timestamp = "2026-08-07T19:02:00Z",
        .subject = "Re: Follow-up",
        .text = "Sure, will do.",
    });
    surface.set_conversation(QStringLiteral("Telegram Bot"), messages);
    surface.document()->documentLayout()->documentSize();
    QCoreApplication::processEvents();

    std::vector<QTextFrame *> message_frames;
    for (auto *frame : surface.document()->rootFrame()->childFrames()) {
        const auto first_text = frame->begin().currentBlock().text();
        if (first_text.startsWith(QStringLiteral("Telegram Bot ·"))
            || first_text.startsWith(QStringLiteral("You ·"))) {
            message_frames.push_back(frame);
        }
    }
    if (message_frames.size() < 2) {
        throw std::runtime_error(
            "the surface must render at least two consecutive message frames "
            "for the turn-rhythm contract");
    }

    // Ordinary consecutive turns keep a deliberate nonzero vertical rhythm in
    // the rendered stream rather than appearing fused. It is expressed
    // relationally from the detected rendered bubble bounds: the visible gap
    // between consecutive bubbles must be at least one body-pixel-size of
    // separation, not a screenshot coordinate.
    constexpr double kBodyPixelSize = 14.0;
    for (auto i = std::size_t{1}; i != message_frames.size(); ++i) {
        const auto gap = message_bubble_rect(*message_frames[i]).top()
            - message_bubble_rect(*message_frames[i - 1]).bottom();
        if (gap < kBodyPixelSize) {
            throw std::runtime_error(
                "ordinary consecutive turns must keep a deliberate nonzero "
                "vertical rhythm in the rendered stream instead of appearing "
                "fused: the visible gap between consecutive bubbles is "
                + std::to_string(gap) + "px, below the body size "
                + std::to_string(kBodyPixelSize) + "px");
        }
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

    const QTextFrame *incoming = nullptr;
    const QTextFrame *outgoing = nullptr;
    for (const auto *frame : surface.document()->rootFrame()->childFrames()) {
        const auto first_text = frame->begin().currentBlock().text();
        if (first_text.startsWith(them + QStringLiteral(" ·"))) {
            incoming = frame;
        } else if (first_text.startsWith(QStringLiteral("You ·"))) {
            outgoing = frame;
        }
    }
    if (!incoming || !outgoing) {
        throw std::runtime_error(
            "the surface must render one incoming and one outgoing message "
            "frame for the typography contract");
    }

    const auto incoming_fragments = fragments_of(*incoming);
    const auto outgoing_fragments = fragments_of(*outgoing);

    // author/sender: 13px DemiBold, its own fragment for each direction.
    const auto &in_sender = require_fragment(
        incoming_fragments, them, true, "incoming sender");
    require_font(in_sender, 13, QFont::DemiBold, "incoming sender");
    const auto &out_sender = require_fragment(
        outgoing_fragments, QStringLiteral("You"), true, "outgoing sender");
    require_font(out_sender, 13, QFont::DemiBold, "outgoing sender");

    // metadata (timestamp) and subject: 12px Normal.
    const auto &in_metadata = require_fragment(
        incoming_fragments, QStringLiteral(" · "), false, "incoming metadata");
    require_font(in_metadata, 12, QFont::Normal, "incoming metadata");
    const auto &out_metadata = require_fragment(
        outgoing_fragments, QStringLiteral(" · "), false, "outgoing metadata");
    require_font(out_metadata, 12, QFont::Normal, "outgoing metadata");
    const auto &in_subject = require_fragment(
        incoming_fragments, QStringLiteral("Slice done"), false,
        "incoming subject");
    require_font(in_subject, 12, QFont::Normal, "incoming subject");
    const auto &out_subject = require_fragment(
        outgoing_fragments, QStringLiteral("Re: Slice done"), false,
        "outgoing subject");
    require_font(out_subject, 12, QFont::Normal, "outgoing subject");

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
        in_body.font.pixelSize(), in_metadata.font.pixelSize(),
        in_subject.font.pixelSize());
    require_hierarchy("outgoing", out_sender.font.pixelSize(),
        out_body.font.pixelSize(), out_metadata.font.pixelSize(),
        out_subject.font.pixelSize());
}

// ---------------------------------------------------------------------------
// I3 RED contract (plan v2 §3.3 / §6.4): at the 1200px test viewport the
// plain empty state must join the same centered reading column as the message
// lane (symmetric 162px outer gutters), anchor at the perceptual ~1/3 of the
// usable viewport, render in the quiet secondary tone (12px Normal,
// st::msgServiceFg) and recompute all of that after a resize. The modern type
// ladder (sender 13px DemiBold / metadata and subject 12px Normal / body 14px
// Normal) is asserted by verify_typography, while the existing opposite sender
// anchors and the directional width-dependent inner slack stay covered by the
// pre-existing responsive/content geometry tests. Fails on the exact base: the
// plain state is full-pane AlignCenter with zero margins at the document top
// in the default character format, resizeEvent reflows messages only, and the
// sender, metadata, and subject tiers are 15px / 13px / 13px Medium.
// ---------------------------------------------------------------------------

constexpr int kRedViewportWidth = 1200;
constexpr int kRedColumnMax = 900;
constexpr int kRedEdgeGutter = 12;

int plain_state_column_gutter(int viewport_width) {
    return std::max(kRedEdgeGutter,
        (viewport_width - kRedColumnMax) / 2 + kRedEdgeGutter);
}

void verify_plain_state_contract(
        ConversationSurface &surface,
        int viewport_width,
        const char *stage) {
    const auto block = surface.document()->begin();
    if (!block.isValid()) {
        throw std::runtime_error(
            std::string("the empty state must render one document block ")
            + stage);
    }

    // Symmetric reading-column margins: the same centered column the message
    // lane lives in, not the full-pane center (fails on the base: zero
    // margins).
    const auto expected_margin =
        double(plain_state_column_gutter(viewport_width));
    const auto format = block.blockFormat();
    if (std::abs(format.leftMargin() - expected_margin) > 1.0
        || std::abs(format.rightMargin() - expected_margin) > 1.0) {
        throw std::runtime_error(
            std::string("the empty state must join the centered reading column ")
            + stage + ": its block needs symmetric " + std::to_string(
                expected_margin) + "px left/right margins at viewport width "
            + std::to_string(viewport_width) + "px, but it carries "
            + std::to_string(format.leftMargin()) + "px / "
            + std::to_string(format.rightMargin()) + "px");
    }

    // Perceptual ~1/3 vertical anchor of the usable viewport (fails on the
    // base: the line sits at the document top).
    const auto viewport_height = surface.viewport()->height();
    const auto *layout = block.layout();
    if (layout->lineCount() < 1) {
        throw std::runtime_error(
            std::string("the empty-state block must lay out at least one line ")
            + stage);
    }
    const auto line_center = layout->lineAt(0).naturalTextRect()
        .translated(layout->position()).center().y();
    if (line_center < double(viewport_height) / 4.0
        || line_center > 2.0 * double(viewport_height) / 3.0) {
        throw std::runtime_error(
            std::string("the empty state must anchor at the perceptual 1/3 of "
                "the usable viewport ") + stage + ": the line's vertical center "
            + std::to_string(line_center) + "px must fall in [h/4, 2h/3] = ["
            + std::to_string(viewport_height / 4) + ", "
            + std::to_string(2 * viewport_height / 3) + "]px, but it stays at "
              "the document top");
    }

    // Quiet secondary tone: 12px Normal in st::msgServiceFg (fails on the
    // base: the default character format).
    QTextCharFormat tone;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        if (fragment.isValid()) {
            tone = fragment.charFormat();
            break;
        }
    }
    if (tone.font().pixelSize() != 12 || tone.font().weight() != QFont::Normal
        || tone.foreground().color() != st::msgServiceFg->c) {
        throw std::runtime_error(
            std::string("the empty state must render in the quiet secondary "
                "tone (12px Normal, st::msgServiceFg) ") + stage
            + ", but its character format is "
            + std::to_string(tone.font().pixelSize()) + "px weight "
            + std::to_string(tone.font().weight()));
    }
}

void verify_plain_state_resize_journey() {
    ConversationSurface surface;
    surface.resize(kRedViewportWidth, 480);
    surface.show();
    QCoreApplication::processEvents();
    surface.set_plain_state(QStringLiteral("No messages yet."));
    surface.document()->documentLayout()->documentSize();
    QCoreApplication::processEvents();
    verify_plain_state_contract(surface, kRedViewportWidth, "before resize");

    // Resize must recompute the column margins and the 1/3 anchor (fails on
    // the base: resizeEvent reflows messages only, leaving the full-pane
    // center stale).
    surface.resize(1400, 600);
    QCoreApplication::processEvents();
    surface.document()->documentLayout()->documentSize();
    QCoreApplication::processEvents();
    verify_plain_state_contract(surface, 1400, "after resize");
}

// ---------------------------------------------------------------------------
// Empty-state RED contract (map unit "empty"): the no-message conversation
// (zero rows) must render the content surface's centered empty state: a small
// Agent avatar, the title "No messages yet", and the muted prompt "Send a
// message or start the Agent.", all inside the same centered reading column
// messages use. Fails on the exact base: set_conversation with zero rows
// rebuilds an empty document, so the title, prompt, and avatar are all
// absent. The existing plain-state journey stays: other plain/error states
// remain truthful.
// ---------------------------------------------------------------------------
void verify_empty_state_contract() {
    ConversationSurface surface;
    surface.resize(kRedViewportWidth, 480);
    surface.show();
    QCoreApplication::processEvents();
    surface.set_conversation(QStringLiteral("Telegram Bot"), {});
    surface.document()->documentLayout()->documentSize();
    QCoreApplication::processEvents();

    QTextBlock title_block;
    QTextBlock prompt_block;
    auto saw_avatar = false;
    for (auto block = surface.document()->begin(); block.isValid();
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid()) {
                continue;
            }
            if (fragment.text() == QStringLiteral("No messages yet")) {
                title_block = block;
            } else if (fragment.text() == QStringLiteral(
                    "Send a message or start the Agent.")) {
                prompt_block = block;
            }
            if (fragment.charFormat().isImageFormat()) {
                saw_avatar = true;
            }
        }
    }
    if (!title_block.isValid() || !prompt_block.isValid()) {
        throw std::runtime_error(
            "the no-message state must render the centered title 'No messages "
            "yet' and the muted prompt 'Send a message or start the Agent.' "
            "on the content surface");
    }
    if (!saw_avatar) {
        throw std::runtime_error(
            "the no-message state must render a centered small Agent avatar");
    }

    // The empty state stays on the content surface: the same centered reading
    // column messages use, centered rather than full-pane.
    const auto gutter = double(plain_state_column_gutter(kRedViewportWidth));
    const auto title_format = title_block.blockFormat();
    if (std::abs(title_format.leftMargin() - gutter) > 1.0
        || std::abs(title_format.rightMargin() - gutter) > 1.0
        || !title_format.alignment().testFlag(Qt::AlignCenter)) {
        throw std::runtime_error(
            "the empty state must stay centered inside the reading column "
            "with symmetric " + std::to_string(int(gutter))
            + "px gutters, but the title block carries "
            + std::to_string(title_format.leftMargin()) + "px / "
            + std::to_string(title_format.rightMargin()) + "px");
    }

    // The prompt is the muted secondary tone (12px Normal, st::msgServiceFg).
    for (auto it = prompt_block.begin(); !it.atEnd(); ++it) {
        const auto piece = it.fragment();
        if (piece.isValid() && piece.text() == QStringLiteral(
                "Send a message or start the Agent.")) {
            const auto tone = piece.charFormat();
            if (tone.font().pixelSize() != 12
                || tone.font().weight() != QFont::Normal
                || tone.foreground().color() != st::msgServiceFg->c) {
                throw std::runtime_error(
                    "the empty-state prompt must be muted in the quiet "
                    "secondary tone (12px Normal, st::msgServiceFg)");
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Safe-Markdown RED contract: the message bodies are stored raw (Markdown
// control characters and literal HTML), but the surface must render them
// through the accepted safe formatter contract: control markers are absent
// from the displayed plain text, heading/bold/link/code runs are real
// distinct QTextCharFormat runs in the existing QTextDocument, raw HTML is
// never interpreted as HTML, and no image or other resource enters the
// document. Fails on the exact base: rebuild_document inserts the whole body
// as one literal 14px Normal run, so '# Plan', '- **bold** item',
// '[docs](https://example.com)', the backticks, '> note', and '<b>unsafe</b>'
// all survive verbatim with no distinct formatted runs at all.
//
// The scan is whole-document and direction-independent: it never assumes a
// body fragment lives in its message's header block, because the accepted
// future design lets one message own several QTextBlocks inside a QTextFrame.
// Both directions receive the identical raw body, so each element surfacing
// at least twice (one distinct run per message) is an occurrence-count proof
// that both the incoming and outgoing bodies are safe-formatted.
// ---------------------------------------------------------------------------
std::vector<QTextCharFormat> format_runs(
        const QTextDocument &document,
        const QString &prefix) {
    std::vector<QTextCharFormat> runs;
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (fragment.isValid() && fragment.text().startsWith(prefix)) {
                runs.push_back(fragment.charFormat());
            }
        }
    }
    return runs;
}

std::vector<QTextCharFormat> require_formatted(
        const QTextDocument &document,
        const QString &text,
        const char *role) {
    auto runs = format_runs(document, text);
    if (runs.size() < 2) {
        throw std::runtime_error(
            std::string("expected the ") + role + " text '" + text.toStdString()
            + "' to render as distinct QTextCharFormat runs for both the "
              "incoming and outgoing directions, but the whole-document scan "
              "found " + std::to_string(runs.size()) + " run(s)");
    }
    return runs;
}

void verify_markdown_safe_formatting() {
    ConversationSurface surface;
    surface.resize(kRedViewportWidth, 480);
    surface.show();
    QCoreApplication::processEvents();

    // One incoming and one outgoing message with the identical stored raw
    // body exercising every contract element: a heading, a bold list item, a
    // link, inline code, a fenced cpp block, a blockquote, and literal HTML.
    const auto raw_body = QStringLiteral(
        "# Plan\n"
        "- **bold** item\n"
        "[docs](https://example.com)\n"
        "`inline code`\n"
        "```cpp\n"
        "int main() { return 0; }\n"
        "```\n"
        "> note\n"
        "<b>unsafe</b>");
    std::vector<DirectConversationMessage> messages;
    messages.push_back({
        .id = "in-1",
        .outgoing = false,
        .timestamp = "2026-08-07T18:48:52Z",
        .subject = "Slice done",
        .text = raw_body.toStdString(),
    });
    messages.push_back({
        .id = "out-1",
        .outgoing = true,
        .timestamp = "2026-08-07T19:00:00Z",
        .subject = "Re: Slice done",
        .text = raw_body.toStdString(),
    });
    surface.set_conversation(QStringLiteral("Telegram Bot"), messages);
    surface.document()->documentLayout()->documentSize();
    QCoreApplication::processEvents();
    const auto &document = *surface.document();

    // Markdown control markers are absent from the displayed plain text: the
    // heading, list/bold, link, fence/backtick, and blockquote syntax all
    // disappear even though the stored bodies still carry them.
    const auto plain = document.toPlainText();
    for (const auto *marker : { "# Plan", "- **bold** item",
            "[docs](https://example.com)", "> note" }) {
        if (plain.contains(QString::fromUtf8(marker))) {
            throw std::runtime_error(
                std::string("the Markdown control marker '") + marker
                + "' must be absent from the displayed plain text, but the "
                  "body is rendered literally");
        }
    }
    if (plain.contains(QChar('`'))) {
        throw std::runtime_error(
            "inline and fenced code backticks must be absent from the "
            "displayed plain text, but the body is rendered literally");
    }

    // Heading: every 'Plan' run (one per direction) is visually distinct from
    // the 14px plain body (a larger size or an emphasized weight).
    const auto heading = require_formatted(
        document, QStringLiteral("Plan"), "markdown heading");
    if (!std::all_of(heading.begin(), heading.end(),
            [](const QTextCharFormat &format) {
                return format.font().pixelSize() > 14
                    || format.font().weight() >= QFont::DemiBold;
            })) {
        throw std::runtime_error(
            "the '# Plan' heading must render as its own visually distinct "
            "run (larger or emphasized) in both directions, not as 14px "
            "plain body text");
    }

    // Bold: every 'bold' run (one per direction) is genuinely bold.
    const auto bold = require_formatted(
        document, QStringLiteral("bold"), "markdown bold");
    if (!std::all_of(bold.begin(), bold.end(),
            [](const QTextCharFormat &format) {
                return format.font().weight() >= QFont::Bold;
            })) {
        throw std::runtime_error(
            "the '- **bold** item' list item must render 'bold' as a "
            "genuinely bold run in both directions, not literal '**' markers");
    }

    // Link: every 'docs' run (one per direction) is an anchor.
    const auto link = require_formatted(
        document, QStringLiteral("docs"), "markdown link");
    if (!std::all_of(link.begin(), link.end(),
            [](const QTextCharFormat &format) {
                return format.isAnchor();
            })) {
        throw std::runtime_error(
            "the '[docs](https://example.com)' link must render 'docs' as an "
            "anchor run in both directions, not the literal bracket syntax");
    }

    // Code: both the inline backtick run and the fenced cpp block render in a
    // fixed-pitch font with the backticks gone, once per direction.
    const auto inline_code = require_formatted(
        document, QStringLiteral("inline code"), "inline code");
    if (!std::all_of(inline_code.begin(), inline_code.end(),
            [](const QTextCharFormat &format) {
                return format.font().fixedPitch();
            })) {
        throw std::runtime_error(
            "inline backtick code must render in a fixed-pitch font in both "
            "directions with the backticks absent");
    }
    const auto code_block = require_formatted(
        document, QStringLiteral("int main() { return 0; }"),
        "fenced cpp code");
    if (!std::all_of(code_block.begin(), code_block.end(),
            [](const QTextCharFormat &format) {
                return format.font().fixedPitch();
            })) {
        throw std::runtime_error(
            "the fenced cpp code block must render in a fixed-pitch font in "
            "both directions with the fence markers absent");
    }

    // Blockquote: '> note' appears as plain quote text without the '>'
    // control marker, once per direction.
    require_formatted(document, QStringLiteral("note"), "blockquote");

    // Raw HTML is never interpreted as HTML: '<b>unsafe</b>' must not turn
    // 'unsafe' into a real bold run, and no image/resource may enter the
    // document from the bodies.
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid()) {
                continue;
            }
            const auto format = fragment.charFormat();
            if (format.isImageFormat()) {
                throw std::runtime_error(
                    "no image may be introduced into the conversation "
                    "document by the safe formatter");
            }
            if (fragment.text().contains(QStringLiteral("unsafe"))
                && format.font().weight() >= QFont::Bold) {
                throw std::runtime_error(
                    "the raw HTML '<b>unsafe</b>' must never be interpreted "
                    "as a real bold HTML run");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Per-message-container RED contract: each message (one incoming and one
// outgoing, with bodies that need several QTextBlocks for a standard Markdown
// presentation) must own exactly one direct child QTextFrame of the root
// frame, in order, with its sender header and every body block inside that
// frame. A whole-frame selection must capture the sender plus all body
// content, so copy/selection act on the one message rather than the whole
// document. Fails on the exact base: rebuild_document writes each message as
// one root block and keeps the body inside it with U+2028 line separators, so
// the root frame has zero child frames and there are no per-message blocks to
// select.
// ---------------------------------------------------------------------------
std::vector<QTextBlock> frame_blocks(const QTextFrame &frame) {
    std::vector<QTextBlock> blocks;
    for (auto it = frame.begin(); !it.atEnd(); ++it) {
        const auto block = it.currentBlock();
        if (block.isValid()) {
            blocks.push_back(block);
        }
    }
    return blocks;
}

void verify_per_message_containers() {
    ConversationSurface surface;
    surface.resize(kRedViewportWidth, 480);
    surface.show();
    QCoreApplication::processEvents();

    // Exactly two messages, one per direction, each with a heading, a list
    // item, and a fenced code block so its Markdown structure needs several
    // distinct QTextBlocks inside its message container.
    const auto raw_body = QStringLiteral(
        "# Plan\n"
        "- **bold** item\n"
        "```cpp\n"
        "int main() { return 0; }\n"
        "```");
    std::vector<DirectConversationMessage> messages;
    messages.push_back({
        .id = "in-1",
        .outgoing = false,
        .timestamp = "2026-08-07T18:48:52Z",
        .subject = "Slice done",
        .text = raw_body.toStdString(),
    });
    messages.push_back({
        .id = "out-1",
        .outgoing = true,
        .timestamp = "2026-08-07T19:00:00Z",
        .subject = "Re: Slice done",
        .text = raw_body.toStdString(),
    });
    surface.set_conversation(QStringLiteral("Telegram Bot"), messages);
    surface.document()->documentLayout()->documentSize();
    QCoreApplication::processEvents();

    // One direct child frame per message, in chronological order.
    const auto &frames = surface.document()->rootFrame()->childFrames();
    if (frames.size() != 2) {
        throw std::runtime_error(
            "each message must own exactly one direct child QTextFrame of the "
            "root frame (incoming then outgoing), but the document has "
            + std::to_string(frames.size()) + " child frame(s)");
    }
    const auto &incoming_frame = *frames[0];
    const auto &outgoing_frame = *frames[1];

    // Each message frame must hold the multiple body blocks a standard
    // Markdown presentation needs: at least a heading, a list item, and a
    // fenced code block, besides its sender header.
    const auto incoming_blocks = frame_blocks(incoming_frame);
    const auto outgoing_blocks = frame_blocks(outgoing_frame);
    if (incoming_blocks.size() < 3 || outgoing_blocks.size() < 3) {
        throw std::runtime_error(
            "each message frame must contain at least three direct QTextBlocks "
            "for its internal Markdown structure (heading, list item, fenced "
            "code), but the incoming frame has "
            + std::to_string(incoming_blocks.size()) + " and the outgoing "
              "frame has " + std::to_string(outgoing_blocks.size()));
    }

    // Whole-frame selection ownership: selecting from the frame's first to its
    // last cursor position must capture the sender plus every body block, so
    // copy/select act on the one message, not the whole document.
    const auto require_frame_selection = [](
            const QTextFrame &frame,
            const QString &sender,
            const char *direction) {
        auto cursor = QTextCursor(frame.document());
        cursor.setPosition(frame.firstPosition());
        cursor.setPosition(frame.lastPosition(), QTextCursor::KeepAnchor);
        const auto selected = cursor.selectedText();
        if (!selected.contains(sender)
            || !selected.contains(QStringLiteral("Plan"))
            || !selected.contains(QStringLiteral("bold item"))
            || !selected.contains(
                QStringLiteral("int main() { return 0; }"))) {
            throw std::runtime_error(
                std::string("the ") + direction
                + " message frame's whole selection must capture its sender "
                  "plus all body content (heading, list item, fenced code), "
                  "but it selects '" + selected.toStdString() + "'");
        }
    };
    require_frame_selection(
        incoming_frame, QStringLiteral("Telegram Bot"), "incoming");
    require_frame_selection(outgoing_frame, QStringLiteral("You"), "outgoing");
}

// ---------------------------------------------------------------------------
// Directional bubble-policy RED contract: incoming renders with NO bubble (the
// backdrop st::windowBgOver), outgoing keeps the st::msgOutBg bubble. Fails on
// the base: paintEvent fills a bubble for both lanes.
// ---------------------------------------------------------------------------
std::vector<QColor> bubble_padding_colors(
        QTextFrame &frame,
        const QImage &image,
        double h_offset,
        double v_offset) {
    const auto text = message_text_bounds(frame)
        .translated(-h_offset, -v_offset);
    const auto sample = [&](double x, double y) {
        const auto px = int(std::lround(x * image.devicePixelRatio()));
        const auto py = int(std::lround(y * image.devicePixelRatio()));
        if (px < 0 || py < 0 || px >= image.width() || py >= image.height()) {
            throw std::runtime_error("a padding sample is outside the viewport");
        }
        return image.pixelColor(px, py);
    };
    const auto cy = text.center().y();
    return {
        sample(text.left() - 6.0, cy - 4.0),
        sample(text.left() - 6.0, cy),
        sample(text.left() - 6.0, cy + 4.0),
        sample(text.right() + 6.0, cy - 4.0),
        sample(text.right() + 6.0, cy),
        sample(text.right() + 6.0, cy + 4.0),
    };
}

void verify_directional_bubble_policy() {
    ConversationSurface surface;
    surface.resize(1600, 480);
    surface.show();
    QCoreApplication::processEvents();

    const std::vector<DirectConversationMessage> messages = {
        {.id = "in-1", .outgoing = false,
            .timestamp = "2026-08-07T18:48:52Z",
            .text = "Incoming reads like an Agent note."},
        {.id = "out-1", .outgoing = true,
            .timestamp = "2026-08-07T19:00:00Z",
            .text = "OK, go ahead."},
    };
    surface.set_conversation(QStringLiteral("Telegram Bot"), messages);
    surface.document()->documentLayout()->documentSize();
    QCoreApplication::processEvents();

    QTextFrame *incoming = nullptr;
    QTextFrame *outgoing = nullptr;
    for (auto *frame : surface.document()->rootFrame()->childFrames()) {
        const auto first_text = frame->begin().currentBlock().text();
        if (first_text.startsWith(QStringLiteral("Telegram Bot ·"))) {
            incoming = frame;
        } else if (first_text.startsWith(QStringLiteral("You ·"))) {
            outgoing = frame;
        }
    }
    if (!incoming || !outgoing) {
        throw std::runtime_error("no incoming/outgoing message frame rendered");
    }
    const auto image = surface.viewport()->grab().toImage();
    const auto h_offset = double(surface.horizontalScrollBar()->value());
    const auto v_offset = double(surface.verticalScrollBar()->value());
    const auto backdrop = st::windowBgOver->c;
    const auto incoming_fill = st::msgInBg->c;
    const auto outgoing_fill = st::msgOutBg->c;

    for (const auto &color : bubble_padding_colors(
            *incoming, image, h_offset, v_offset)) {
        if (color == incoming_fill) {
            throw std::runtime_error(
                "incoming must have NO bubble: padding is st::msgInBg, not "
                "the backdrop st::windowBgOver");
        }
        if (color != backdrop) {
            throw std::runtime_error(
                "incoming must stay on the backdrop st::windowBgOver");
        }
    }

    for (const auto &color : bubble_padding_colors(
            *outgoing, image, h_offset, v_offset)) {
        if (color != outgoing_fill) {
            throw std::runtime_error(
                "outgoing must keep its content-width bubble filled with "
                "st::msgOutBg");
        }
    }

    // Incoming must reserve a 40px avatar lane immediately to the left of its
    // text bounds. Relationally, the incoming header's left margin must reach
    // at least 50px (40 avatar + 10 gap) further left than the outgoing
    // header's right margin, which today sits at the same symmetric gutter.
    const auto incoming_left = incoming->begin().currentBlock()
        .blockFormat().leftMargin();
    const auto outgoing_right = outgoing->begin().currentBlock()
        .blockFormat().rightMargin();
    if (incoming_left - outgoing_right < 50.0) {
        throw std::runtime_error(
            "incoming must reserve a 50px Agent avatar lane beside its text "
            "(40px avatar + 10px gap): the incoming header left margin must "
            "reach at least 50px further left than the outgoing header right "
            "margin, but it is "
            + std::to_string(incoming_left) + "px vs "
            + std::to_string(outgoing_right) + "px, so the reserved avatar "
              "extent is missing");
    }

    // The expected 40x40 avatar circle sits immediately left of the incoming
    // text bounds with a 10px gap, in viewport coordinates.
    const auto incoming_text = message_text_bounds(*incoming)
        .translated(-h_offset, -v_offset);
    const auto avatar_rect = QRectF(
        incoming_text.left() - 10.0 - 40.0,
        incoming_text.center().y() - 20.0,
        40.0, 40.0);
    const auto avatar_fill = st::dialogsNameFg->c;
    const auto sample_avatar = [&](double x, double y) {
        const auto px = int(std::lround(x * image.devicePixelRatio()));
        const auto py = int(std::lround(y * image.devicePixelRatio()));
        if (px < 0 || py < 0 || px >= image.width() || py >= image.height()) {
            throw std::runtime_error(
                "an avatar sample is outside the viewport");
        }
        return image.pixelColor(px, py);
    };
    const auto ax = avatar_rect.center().x();
    const auto ay = avatar_rect.center().y();
    const double avatar_offsets[] = {-10.0, 10.0};
    for (const auto dx : avatar_offsets) {
        if (sample_avatar(ax + dx, ay) != avatar_fill) {
            throw std::runtime_error(
                "incoming must draw a 40px Agent avatar circle filled with "
                "st::dialogsNameFg immediately left of its text bounds, but "
                "the sampled avatar interior is not the circle fill (the "
                "avatar circle is missing)");
        }
    }
    for (const auto dy : avatar_offsets) {
        if (sample_avatar(ax, ay + dy) != avatar_fill) {
            throw std::runtime_error(
                "incoming must draw a 40px Agent avatar circle filled with "
                "st::dialogsNameFg immediately left of its text bounds, but "
                "the sampled avatar interior is not the circle fill (the "
                "avatar circle is missing)");
        }
    }
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
        verify_content_geometry();
        verify_turn_rhythm();
        verify_plain_state_resize_journey();
        verify_empty_state_contract();
        verify_markdown_safe_formatting();
        verify_per_message_containers();
        verify_directional_bubble_policy();
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
