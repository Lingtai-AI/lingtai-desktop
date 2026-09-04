#include "ui/conversation_surface.h"

#include "base/basic_types.h"
#include "styles/palette.h"
#include "styles/style_widgets.h"
#include "ui/style/style_core_scale.h"
#include "ui/style/style_core_palette.h"

#include "direct_conversation_history.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QImage>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPalette>
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
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>

namespace lingtai::desktop {
namespace {

constexpr auto kMessageOutgoingProperty = QTextFormat::UserProperty + 5;
constexpr auto kMessageTimestampProperty = QTextFormat::UserProperty + 2;
constexpr auto kMessageReactionsProperty = QTextFormat::UserProperty + 9;
constexpr auto kMessageBlockProperty = QTextFormat::UserProperty + 1;
constexpr auto kCodeBlockProperty = QTextFormat::UserProperty + 4;
constexpr auto kAttachmentBlockProperty = QTextFormat::UserProperty + 11;
constexpr auto kAttachmentMessageIdProperty = QTextFormat::UserProperty + 12;
constexpr auto kAttachmentIndexProperty = QTextFormat::UserProperty + 13;

DirectConversationAttachment projected_attachment(
        const std::filesystem::path &path,
        AttachmentMediaKind kind) {
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0) {
        throw std::runtime_error("attachment fixture must stat");
    }
    return {
        path,
        path.filename().string(),
        static_cast<std::uint64_t>(status.st_size),
        kind,
        static_cast<std::uint64_t>(status.st_dev),
        static_cast<std::uint64_t>(status.st_ino),
    };
}

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
        int timestamp_size) {
    // Sender stays slightly larger than body (identity emphasis); metadata is
    // the quietest rung. Weight, not size inversion, carries the hierarchy.
    if (!(author_size >= body_size && body_size > timestamp_size)) {
        throw std::runtime_error(
            std::string("the ") + direction
            + " message must read semibold sender >= body > metadata by size "
              "while sender weight preserves identity emphasis (sender "
            + std::to_string(author_size) + "px, body "
            + std::to_string(body_size) + "px, timestamp "
            + std::to_string(timestamp_size) + "px)");
    }
}

QColor expected_secondary_reading_color() {
    return st::windowBg->c.lightness() >= 128
        ? QColor(QStringLiteral("#8A8F98"))
        : st::msgServiceFg->c;
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

bool is_outgoing_frame(const QTextFrame &frame) {
    return frame.frameFormat().property(kMessageOutgoingProperty).toBool();
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
        .timestamp = "2026-08-07T18:48:52",
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
    for (auto *frame : surface.document()->rootFrame()->childFrames()) {
        const auto block = frame->begin().currentBlock();
        if (!block.isValid()) {
            continue;
        }
        if (is_outgoing_frame(*frame)) {
            outgoing = block;
        } else {
            incoming = block;
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
    // left minus the 42px avatar lane) matches the outgoing outer-right and
    // the incoming envelope's inner-right (its text right plus the 42px
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
    if (std::abs((wide_in.left - 42.0) - wide_out.right) > 2.0) {
        throw std::runtime_error(
            "at a very wide viewport the messages must share one centered "
            "reading column: the incoming avatar envelope's outer-left must "
            "align with the outgoing reading-column outer-right");
    }
    if (wide_in.right + 42.0 >= wide_out.left - 2.0) {
        throw std::runtime_error(
            "at a very wide viewport the incoming and outgoing messages must "
            "keep opposite anchors inside the shared reading column without "
            "crossing each other's content lanes");
    }
    if (wide_in.content_width < 520.0 || wide_in.content_width > 580.0
        || wide_out.content_width < 520.0
        || wide_out.content_width > 580.0) {
        throw std::runtime_error(
            "at a very wide viewport the message width must stop at an "
            "absolute readable cap around 560px, but it stretches "
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
    if (normal_in.content_ratio < 0.65 || normal_in.content_ratio > 0.75) {
        throw std::runtime_error(
            "at a normal viewport the incoming message width must keep its "
            "approximate 65-75% pane share (ratio="
            + std::to_string(normal_in.content_ratio) + ")");
    }
    if (normal_out.content_ratio < 0.55 || normal_out.content_ratio > 0.72) {
        throw std::runtime_error(
            "at a normal viewport the outgoing Human bubble must keep its "
            "approximate 55-72% pane share (ratio="
            + std::to_string(normal_out.content_ratio) + ")");
    }

    // Narrow: the message width becomes near-full rather than the fixed 72%.
    const auto narrow = measure_at_width(320);
    const auto narrow_in = narrow.first;
    const auto narrow_out = narrow.second;
    // The incoming message reserves its 42px avatar lane (34px circle + 8px
    // gap) inside the pane, so its text alone no longer stays >=90%; the whole
    // avatar envelope does. The outgoing bubble has no avatar lane.
    const auto narrow_in_envelope_ratio =
        (narrow_in.content_width + 42.0) / 320.0;
    if (narrow_in_envelope_ratio < 0.85
        || narrow_out.content_ratio < 0.85) {
        throw std::runtime_error(
            "at a narrow viewport the message width must become near-full "
            "(~85%+): the incoming avatar envelope (ratio="
            + std::to_string(narrow_in_envelope_ratio)
            + ") and the outgoing bubble (ratio="
            + std::to_string(narrow_out.content_ratio) + ")");
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
        .timestamp = "2026-08-07T18:48:52",
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
    // incoming and reserves its 42px avatar lane (34px circle + 8px gap) on
    // its outer edge, so its left anchor sits that lane inside the shared
    // centered lane's left edge.
    if (!(long_geometry.left < long_geometry.right
            && short_geometry.right < short_geometry.left)) {
        throw std::runtime_error(
            "at a very wide viewport the long and short messages must keep "
            "opposite anchors, incoming left and outgoing right");
    }
    if (std::abs((long_geometry.left - 42.0) - short_geometry.right) > 2.0) {
        throw std::runtime_error(
            "at a very wide viewport the long and short messages must share "
            "the centered reading column's stable outer anchors; the long "
            "incoming message's outer edge reserves its 42px avatar envelope");
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
        .timestamp = "2026-08-07T18:48:52",
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
        message_frames.push_back(frame);
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
        .timestamp = "2026-08-07T18:48:52",
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
        if (is_outgoing_frame(*frame)) {
            outgoing = frame;
        } else {
            incoming = frame;
        }
    }
    if (!incoming || !outgoing) {
        throw std::runtime_error(
            "the surface must render one incoming and one outgoing message "
            "frame for the typography contract");
    }

    const auto incoming_fragments = fragments_of(*incoming);
    const auto outgoing_fragments = fragments_of(*outgoing);

    // Incoming keeps its Agent-name header; outgoing is intentionally body-first.
    const auto &in_sender = require_fragment(
        incoming_fragments, them, true, "incoming sender");
    require_font(in_sender, 15, QFont::DemiBold, "incoming sender");

    // Incoming message metadata scales to 13px Normal. Per-email subjects are source
    // metadata, not conversation content, and must not enter either direction's
    // rendered message surface.
    const auto &in_metadata = require_fragment(
        incoming_fragments, QStringLiteral(" · "), false, "incoming metadata");
    require_font(in_metadata, 13, QFont::Normal, "incoming metadata");
    const auto rendered = surface.document()->toPlainText();
    if (rendered.contains(QStringLiteral("Slice done"))
        || rendered.contains(QStringLiteral("Re: Slice done"))) {
        throw std::runtime_error(
            "per-email subject/title metadata must not be rendered in the "
            "conversation surface");
    }

    // The reading-first message body scales to 14px Normal.
    const auto &in_body = require_fragment(
        incoming_fragments, QStringLiteral("PR published, not merged."), true,
        "incoming body");
    require_font(in_body, 14, QFont::Normal, "incoming body");
    const auto &out_body = require_fragment(
        outgoing_fragments, QStringLiteral("Thanks, reviewing tomorrow."), true,
        "outgoing body");
    require_font(out_body, 14, QFont::Normal, "outgoing body");

    const auto require_reading_line_height = [](const QTextFrame &frame,
            const char *direction) {
        for (auto it = frame.begin(); !it.atEnd(); ++it) {
            const auto block = it.currentBlock();
            if (!block.isValid()) continue;
            const auto format = block.blockFormat();
            if (format.lineHeightType() != QTextBlockFormat::ProportionalHeight
                || qRound(format.lineHeight()) != 132) {
                throw std::runtime_error(
                    std::string("the ") + direction
                    + " message blocks must use 132% proportional line height "
                      "after the reading font scales");
            }
        }
    };
    require_reading_line_height(*incoming, "incoming");
    require_reading_line_height(*outgoing, "outgoing");

    // The pinned visual hierarchy for both directions.
    require_hierarchy("incoming", in_sender.font.pixelSize(),
        in_body.font.pixelSize(), in_metadata.font.pixelSize());
}

// ---------------------------------------------------------------------------
// I3 RED contract (plan v2 §3.3 / §6.4): at the 1200px test viewport the
// plain empty state must join the same centered reading column as the message
// lane (symmetric 162px outer gutters), anchor at the perceptual ~1/3 of the
// usable viewport, render in the quiet secondary tone (12px Normal,
// st::msgServiceFg) and recompute all of that after a resize. The modern type
// ladder (sender 15px DemiBold / metadata 13px Normal / body 14px
// Normal) is asserted by verify_typography, while the existing opposite sender
// anchors and the directional width-dependent inner slack stay covered by the
// pre-existing responsive/content geometry tests. Fails on the exact base: the
// plain state is full-pane AlignCenter with zero margins at the document top
// in the default character format, resizeEvent reflows messages only, and the
// sender, metadata, and subject tiers are 15px / 13px / 13px Medium.
// ---------------------------------------------------------------------------

constexpr int kRedViewportWidth = 1200;
constexpr int kRedColumnMax = 1600;
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
    const auto expected_secondary = expected_secondary_reading_color();
    if (tone.font().pixelSize() != 12 || tone.font().weight() != QFont::Normal
        || tone.foreground().color() != expected_secondary) {
        throw std::runtime_error(
            std::string("the empty state must render in the quiet secondary "
                "tone (12px Normal, secondary reading color) ") + stage
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

void verify_reflow_after_hidden_resize() {
    ConversationSurface surface;
    surface.resize(1200, 400);
    surface.show();
    QCoreApplication::processEvents();
    surface.set_conversation(QStringLiteral("Alpha"), {{
        .id = "out-hidden-resize",
        .outgoing = true,
        .timestamp = "2026-08-18T12:00:00Z",
        .text = "A reasonably long human note that must reflow after /kanban.",
    }});
    surface.document()->documentLayout()->documentSize();
    QCoreApplication::processEvents();
    auto frames = surface.document()->rootFrame()->childFrames();
    if (frames.size() != 1) {
        throw std::runtime_error(
            "the hidden-resize fixture must render exactly one Human frame");
    }
    const auto wide_left = frames.front()->begin().currentBlock()
        .blockFormat().leftMargin();
    const auto wide_viewport = surface.viewport()->width();

    surface.hide();
    QCoreApplication::processEvents();
    surface.resize(720, 400);
    QCoreApplication::processEvents();
    surface.show();
    QCoreApplication::processEvents();
    surface.document()->documentLayout()->documentSize();
    QCoreApplication::processEvents();

    frames = surface.document()->rootFrame()->childFrames();
    if (frames.size() != 1) {
        throw std::runtime_error(
            "the hidden-resize fixture must keep one Human frame after show");
    }
    const auto left = frames.front()->begin().currentBlock()
        .blockFormat().leftMargin();
    const auto viewport_width = surface.viewport()->width();
    if (wide_viewport - viewport_width < 80) {
        throw std::runtime_error(
            "the hidden-resize fixture must actually shrink the viewport");
    }
    if (wide_left - left < 40) {
        throw std::runtime_error(
            "showing the conversation after a hidden resize must reflow Human "
            "bubbles to the current viewport, not keep the previous inner "
            "margin that overflows the right edge");
    }
    if (left + 12 > viewport_width) {
        throw std::runtime_error(
            "Human bubble inner margin must stay inside the current viewport");
    }
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
                || tone.foreground().color() != expected_secondary_reading_color()) {
                throw std::runtime_error(
                    "the empty-state prompt must be muted in the quiet "
                    "secondary reading tone (12px Normal)");
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
        .timestamp = "2026-08-07T18:48:52",
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
                return format.font().fixedPitch()
                    && (format.background().style() == Qt::NoBrush
                        || format.background().color().alpha() == 0);
            })) {
        throw std::runtime_error(
            "the fenced cpp code block must render in a fixed-pitch font with "
            "no per-glyph wash (paint owns the panel) in both directions");
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
        .timestamp = "2026-08-07T18:48:52",
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

    // Whole-frame selection ownership: incoming selection includes its sender;
    // outgoing selection begins with its body. Both still own every body block,
    // so copy/select act on the one message, not the whole document.
    const auto require_frame_selection = [](
            const QTextFrame &frame,
            const QString &sender,
            const char *direction) {
        auto cursor = QTextCursor(frame.document());
        cursor.setPosition(frame.firstPosition());
        cursor.setPosition(frame.lastPosition(), QTextCursor::KeepAnchor);
        const auto selected = cursor.selectedText();
        if ((!sender.isEmpty() && !selected.contains(sender))
            || !selected.contains(QStringLiteral("Plan"))
            || !selected.contains(QStringLiteral("bold item"))
            || !selected.contains(
                QStringLiteral("int main() { return 0; }"))) {
            throw std::runtime_error(
                std::string("the ") + direction
                + " message frame's whole selection must capture its optional "
                  "sender plus all body content (heading, list item, fenced code), "
                  "but it selects '" + selected.toStdString() + "'");
        }
    };
    require_frame_selection(
        incoming_frame, QStringLiteral("Telegram Bot"), "incoming");
    require_frame_selection(outgoing_frame, QString(), "outgoing");
}

// ---------------------------------------------------------------------------
// Directional bubble policy: incoming stays on the single window backdrop;
// Human keeps a distinct but deliberately pale low-saturation light bubble.
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
            .timestamp = "2026-08-07T18:48:52",
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
        if (is_outgoing_frame(*frame)) {
            outgoing = frame;
        } else {
            incoming = frame;
        }
    }
    if (!incoming || !outgoing) {
        throw std::runtime_error("no incoming/outgoing message frame rendered");
    }
    const auto image = surface.viewport()->grab().toImage();
    const auto h_offset = double(surface.horizontalScrollBar()->value());
    const auto v_offset = double(surface.verticalScrollBar()->value());
    const auto backdrop = st::windowBg->c;
    const auto incoming_fill = st::msgInBg->c;
    const auto outgoing_fill = QColor(QStringLiteral("#EEF7F3"));

    for (const auto &color : bubble_padding_colors(
            *incoming, image, h_offset, v_offset)) {
        if (incoming_fill != backdrop && color == incoming_fill) {
            throw std::runtime_error(
                "incoming must have NO bubble: padding is st::msgInBg, not "
                "the backdrop st::windowBg");
        }
        if (color != backdrop) {
            throw std::runtime_error(
                "incoming must stay on the backdrop st::windowBg");
        }
    }

    for (const auto &color : bubble_padding_colors(
            *outgoing, image, h_offset, v_offset)) {
        if (color != outgoing_fill) {
            throw std::runtime_error(
                "Human must keep its content-width bubble filled with the "
                "accepted pale low-saturation tint");
        }
    }

    // Incoming reserves the accepted smaller 34px avatar lane immediately to
    // the left of its text. Relationally, its left margin reaches at least 42px
    // (34 avatar + 8 gap) beyond the Human frame's outer right margin.
    const auto incoming_left = incoming->begin().currentBlock()
        .blockFormat().leftMargin();
    const auto outgoing_right = outgoing->begin().currentBlock()
        .blockFormat().rightMargin();
    if (incoming_left - outgoing_right < 42.0) {
        throw std::runtime_error(
            "incoming must reserve a 42px Agent avatar lane beside its text "
            "(34px avatar + 8px gap): the incoming header left margin must "
            "reach at least 42px further left than the Human outer margin, "
            "but it is "
            + std::to_string(incoming_left) + "px vs "
            + std::to_string(outgoing_right) + "px, so the reserved avatar "
              "extent is missing");
    }

    // The accepted 34x34 avatar circle sits top-aligned immediately left of the
    // incoming text bounds with an 8px gap, in viewport coordinates.
    const auto incoming_text = message_text_bounds(*incoming)
        .translated(-h_offset, -v_offset);
    const auto avatar_rect = QRectF(
        incoming_text.left() - 8.0 - 34.0,
        incoming_text.top(),
        34.0, 34.0);
    const auto avatar_fill = st::windowBgActive->c;
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
                "incoming must draw a 34px Agent avatar circle filled with "
                "st::windowBgActive immediately left of its text bounds, but "
                "the sampled avatar interior is not the circle fill (the "
                "avatar circle is missing)");
        }
    }
    for (const auto dy : avatar_offsets) {
        if (sample_avatar(ax, ay + dy) != avatar_fill) {
            throw std::runtime_error(
                "incoming must draw a 34px Agent avatar circle filled with "
                "st::windowBgActive immediately left of its text bounds, but "
                "the sampled avatar interior is not the circle fill (the "
                "avatar circle is missing)");
        }
    }
}


// Human messages are body-first and content-sized. Their bubble is anchored
// right, but the text within it is always a normal left-aligned reading block;
// timestamp ink lives below the bubble instead of floating beside its top edge.
struct OutgoingRender {
    QImage image;
    QRectF text;
    QString plain;
    QTextBlockFormat block;
    QTextCharFormat body;
    qreal effective_width = 0;
    qreal first_line_advance = 0;
    qreal document_margin = 0;
    int viewport_width = 0;
};

OutgoingRender render_outgoing_at(
        const std::string &timestamp,
        const std::string &text = "Human body only.",
        int width = 1200) {
    ConversationSurface surface;
    surface.resize(width, 340);
    surface.show();
    QCoreApplication::processEvents();
    surface.set_conversation(QStringLiteral("Telegram Bot"), {{
        .id = "out-time",
        .outgoing = true,
        .timestamp = timestamp,
        .text = text,
    }});
    surface.document()->documentLayout()->documentSize();
    QCoreApplication::processEvents();

    const auto frames = surface.document()->rootFrame()->childFrames();
    if (frames.size() != 1) {
        throw std::runtime_error(
            "the Human bubble fixture must render exactly one message frame");
    }
    const auto blocks = frame_blocks(*frames.front());
    if (blocks.empty()) {
        throw std::runtime_error("the Human bubble fixture has no body block");
    }
    const auto h_offset = double(surface.horizontalScrollBar()->value());
    const auto v_offset = double(surface.verticalScrollBar()->value());
    const auto format = blocks.front().blockFormat();
    const auto runs = format_runs(*surface.document(),
        QString::fromStdString(text).left(12));
    if (runs.empty()) {
        throw std::runtime_error("the Human bubble fixture has no body format");
    }
    auto frame_text = QString();
    for (const auto &block : blocks) {
        if (!frame_text.isEmpty()) {
            frame_text += QChar('\n');
        }
        frame_text += block.text();
    }
    return {
        surface.viewport()->grab().toImage(),
        message_text_bounds(*frames.front()).translated(-h_offset, -v_offset),
        frame_text.trimmed(),
        format,
        runs.front(),
        surface.viewport()->width() - format.leftMargin() - format.rightMargin(),
        blocks.front().layout()->lineAt(0).horizontalAdvance(),
        surface.document()->documentMargin(),
        surface.viewport()->width(),
    };
}

QRectF exact_color_bounds(const QImage &image, const QColor &color) {
    auto bounds = QRect();
    const auto pixel = color.rgba();
    for (auto y = 0; y < image.height(); ++y) {
        for (auto x = 0; x < image.width(); ++x) {
            if (image.pixel(x, y) == pixel) {
                const auto point = QRect(x, y, 1, 1);
                bounds = bounds.isNull() ? point : bounds.united(point);
            }
        }
    }
    const auto dpr = image.devicePixelRatio();
    if (bounds.isNull()) {
        return QRectF();
    }
    const auto physical = QRectF(bounds).adjusted(0, 0, 1, 1);
    return QRectF(physical.x() / dpr, physical.y() / dpr,
        physical.width() / dpr, physical.height() / dpr);
}

QRectF changed_bounds(const QImage &a, const QImage &b) {
    if (a.size() != b.size() || a.devicePixelRatio() != b.devicePixelRatio()) {
        throw std::runtime_error("paired Human renders must share one image geometry");
    }
    auto bounds = QRect();
    for (auto y = 0; y < a.height(); ++y) {
        for (auto x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y)) {
                const auto point = QRect(x, y, 1, 1);
                bounds = bounds.isNull() ? point : bounds.united(point);
            }
        }
    }
    const auto dpr = a.devicePixelRatio();
    if (bounds.isNull()) {
        return QRectF();
    }
    const auto physical = QRectF(bounds).adjusted(0, 0, 1, 1);
    return QRectF(physical.x() / dpr, physical.y() / dpr,
        physical.width() / dpr, physical.height() / dpr);
}

void verify_human_bubble_contract() {
    const auto first = render_outgoing_at("2026-08-07T19:00:00Z");
    const auto second = render_outgoing_at("2026-08-07T20:11:00Z");
    const auto short_message = render_outgoing_at(
        "2026-08-07T19:00:00Z", "OK");
    const auto long_message = render_outgoing_at(
        "2026-08-07T19:00:00Z",
        "This intentionally long Human message proves that the bubble stays "
        "content-sized and stops at a moderate share of the Conversation "
        "column instead of stretching across the available pane.");
    const auto very_wide_message = render_outgoing_at(
        "2026-08-07T19:00:00Z",
        "This intentionally long Human message proves that a very wide pane "
        "moves the shared message rail outward without stretching prose past "
        "the accepted readable bubble width.",
        1600);
    const auto real_short_mixed_message = render_outgoing_at(
        "2026-07-31T02:36:51Z",
        "你似乎在修复MCP不兼容的问题？",
        1600);
    // Exact adjacent Human records from ~/.lingtai/Personal_agent_minimax that
    // exposed the bug: both hit the same capped lane, but different word-wrap
    // slack must never move the bubble/timestamp right edge.
    const auto real_aug_1 = render_outgoing_at(
        "2026-08-01T10:00:58Z",
        "Automated LingTai 05:00 Chicago maintenance result: 已停止 "
        "lingtai-tui（PID 28358）；检查完成：LingTai TUI 已是当前版本 unknown，无需升级\n"
        "Send exactly one concise Telegram message to Ted (chat_id 6992160568) "
        "with this result. Do not rerun the upgrade. Do not send a duplicate "
        "internal-email reply; this mailbox item is an automation trigger.",
        800);
    const auto real_aug_2 = render_outgoing_at(
        "2026-08-02T10:00:42Z",
        "Automated LingTai 05:00 Chicago maintenance result: 检查完成：LingTai TUI "
        "已是当前版本 unknown，无需升级\nSend exactly one concise Telegram message "
        "to Ted (chat_id 6992160568) with this result. Do not rerun the upgrade. "
        "Do not send a duplicate internal-email reply; this mailbox item is an "
        "automation trigger.",
        800);
    const auto expected = QStringLiteral("Human body only.");
    if (first.plain != expected || second.plain != expected) {
        throw std::runtime_error(
            "Human bubble document must contain the body only: no sender/time "
            "header belongs inside the bubble");
    }

    const auto left_aligned = [](const OutgoingRender &render) {
        return bool(render.block.alignment() & Qt::AlignLeft)
            && render.block.leftMargin() > render.block.rightMargin();
    };
    if (!left_aligned(first) || !left_aligned(short_message)
        || !left_aligned(long_message)) {
        throw std::runtime_error(
            "Human body text must be left-aligned while asymmetric margins "
            "keep the content-sized bubble anchored on the right");
    }
    if (first.body.font().pixelSize() < 13
        || first.body.font().pixelSize() > 14
        || first.body.font().weight() != QFont::Normal
        || first.block.lineHeightType() != QTextBlockFormat::ProportionalHeight
        || first.block.lineHeight() < 125 || first.block.lineHeight() > 140) {
        throw std::runtime_error(
            "Human body must share the 13-14px Normal and ~1.32 reading "
            "typography used by Agent prose");
    }
    if (short_message.effective_width > 200) {
        throw std::runtime_error(
            "a short Human message must retain a narrow content-sized bubble");
    }
    if (long_message.effective_width < 500
        || long_message.effective_width > 560
        || very_wide_message.effective_width > 560) {
        throw std::runtime_error(
            "long Human prose must keep the accepted 500-560px readable bubble cap");
    }
    if (very_wide_message.block.rightMargin() > 64) {
        throw std::runtime_error(
            "a 1600px Conversation must keep its Human rail within a 64px edge gutter");
    }
    if (real_short_mixed_message.text.height() > 32.0) {
        throw std::runtime_error(
            "the real short mixed-language Human message must stay on one line");
    }
    // Telegram Desktop derives natural width from shaped layout advances and
    // bearings, not from the tighter visible-ink rectangle. The bubble must fit
    // the rendered line's horizontal advance plus its declared padding; an ink-
    // bounds probe can under-size the live line when its paint device differs.
    const auto layout_width_deficit = real_short_mixed_message.first_line_advance
        + 2 * 15 + 2 * real_short_mixed_message.document_margin
        - real_short_mixed_message.effective_width;
    if (layout_width_deficit > 0.5) {
        throw std::runtime_error(
            "the real mixed-language Human bubble must fit its QTextLine "
            "advance, declared padding, and the root document inset, but it "
            "is short by " + std::to_string(layout_width_deficit) + "px");
    }

    const auto expected_fill = QColor(QStringLiteral("#EEF7F3"));
    const auto fill = exact_color_bounds(first.image, expected_fill);
    const auto aug_1_fill = exact_color_bounds(real_aug_1.image, expected_fill);
    const auto aug_2_fill = exact_color_bounds(real_aug_2.image, expected_fill);
    if (aug_1_fill.isEmpty() || aug_2_fill.isEmpty()
        || std::abs(aug_1_fill.right() - aug_2_fill.right()) > 2.0) {
        throw std::runtime_error(
            "capped Human bubbles must share one right edge regardless of "
            "word-wrap slack; the real Personal_agent_minimax records drift");
    }
    const auto expected_bubble = first.text.adjusted(-15, -11, 15, 11);
    const auto close = [](qreal a, qreal b) { return std::abs(a - b) <= 2.0; };
    if (fill.isEmpty()
        || !close(fill.left(), expected_bubble.left())
        || fill.right() < expected_bubble.right() - 2.0
        || !close(fill.top(), expected_bubble.top())
        || !close(fill.bottom(), expected_bubble.bottom())) {
        throw std::runtime_error(
            "Human bubble must use the pale low-saturation fill with 15px "
            "leading/minimum trailing and 11px vertical padding");
    }
    if (first.image.pixelColor(
            qRound(expected_bubble.left()), qRound(expected_bubble.top()))
        == expected_fill) {
        throw std::runtime_error(
            "Human bubble needs a moderate rounded corner, not a square or "
            "extremely pill-like edge");
    }

    const auto changed = changed_bounds(first.image, second.image);
    if (changed.isEmpty()
        || changed.top() < expected_bubble.bottom() + 1
        || changed.left() < expected_bubble.left() - 1
        || changed.right() > fill.right() + 1
        || changed.height() > 16) {
        throw std::runtime_error(
            "Human timestamp must render as a compact muted line directly "
            "below and right-aligned with the bubble, never beside its top edge");
    }
}

void verify_outgoing_body_first_and_external_time() {
    verify_human_bubble_contract();
}


// ---------------------------------------------------------------------------
// Render-time lazy history RED contract: even though all 205 cached rows are
// already present, the surface must reveal them lazily in a render-time
// window instead of materializing every frame. Initially only the
// chronological tail 100 (messages 105..204) become direct child message
// frames in order with no 104, plus the `▲ 105 older — ctrl+u to load`
// banner. A Ctrl+U at the top reveals another 100 (messages 005..204), no
// 004, no duplicates, banner `▲ 5 older`, and the previously first-visible
// message keeps its viewport Y (no scroll jump). Fails on the exact base:
// rebuild_document writes every one of the 205 rows as a direct child frame
// (205 frames and no banner), so the initial frame-count 100 check fails
// first.
// ---------------------------------------------------------------------------
int history_index(const QTextFrame &frame) {
    QString text;
    for (auto it = frame.begin(); !it.atEnd(); ++it) {
        const auto block = it.currentBlock();
        if (block.isValid()) {
            text += block.text();
        }
    }
    const auto needle = QStringLiteral("message ");
    const auto pos = text.indexOf(needle);
    if (pos < 0) {
        return -1;
    }
    return text.mid(pos + needle.size(), 3).toInt();
}

std::vector<int> history_sequence(
        const QList<QTextFrame *> &frames) {
    std::vector<int> sequence;
    for (const auto *frame : frames) {
        const auto index = history_index(*frame);
        if (index >= 0) {
            sequence.push_back(index);
        }
    }
    return sequence;
}

double history_viewport_y(
        const QTextDocument &document,
        QTextFrame &frame,
        int v_offset) {
    return document.documentLayout()->frameBoundingRect(&frame).topLeft().y()
        - v_offset;
}

void require_history_sequence(
        const std::vector<int> &sequence,
        int first,
        int last,
        const char *stage) {
    const auto expected_size = std::size_t(last - first + 1);
    if (sequence.size() != expected_size) {
        throw std::runtime_error(
            std::string("the ") + stage
            + " history window must reveal exactly "
            + std::to_string(expected_size)
            + " direct child message frames in order (messages "
            + std::to_string(first) + ".." + std::to_string(last)
            + "), but it has " + std::to_string(sequence.size()));
    }
    for (auto i = std::size_t{0}; i != sequence.size(); ++i) {
        if (sequence[i] != first + int(i)) {
            throw std::runtime_error(
                std::string("the ") + stage
                + " history window must show messages "
                + std::to_string(first) + ".." + std::to_string(last)
                + " in order with no gap and no duplicates, but frame "
                + std::to_string(i) + " is message "
                + std::to_string(sequence[i]));
        }
    }
}

void require_history_banner(
        const QTextDocument &document,
        int count,
        const char *stage) {
    const auto banner = QStringLiteral("▲ %1 older — ctrl+u to load")
        .arg(count);
    if (!document.toPlainText().contains(banner)) {
        throw std::runtime_error(
            std::string("the ") + stage
            + " history window must render the banner '" + banner.toStdString()
            + "'");
    }
}

void verify_history_window_only() {
    std::vector<DirectConversationMessage> messages;
    messages.reserve(205);
    for (auto i = 0; i != 205; ++i) {
        char id[8];
        std::snprintf(id, sizeof id, "%03d", i);
        messages.push_back({
            .id = id,
            .outgoing = (i % 2) == 1,
            .timestamp = "2026-08-07T18:00:00Z",
            .subject = std::string(),
            .text = std::string("message ") + id,
        });
    }

    ConversationSurface surface;
    surface.resize(640, 480);
    surface.show();
    QCoreApplication::processEvents();
    surface.set_conversation(QStringLiteral("Telegram Bot"), messages);
    surface.document()->documentLayout()->documentSize();
    QCoreApplication::processEvents();

    // Pin the window to the top before inspecting the revealed frames.
    auto *scrollbar = surface.verticalScrollBar();
    scrollbar->setValue(scrollbar->minimum());
    QCoreApplication::processEvents();

    // RED first: the initial render-time window is exactly the tail 100.
    auto frames = surface.document()->rootFrame()->childFrames();
    auto sequence = history_sequence(frames);
    if (sequence.size() != 100) {
        throw std::runtime_error(
            "the render-time history window must initially reveal exactly 100 "
            "direct child message frames (messages 105..204), but the surface "
            "materialized " + std::to_string(sequence.size()));
    }
    require_history_sequence(sequence, 105, 204, "initial");
    require_history_banner(*surface.document(), 105, "initial");

    // Capture the old first-visible message's viewport Y so a reveal at the
    // top must preserve it (no scroll jump).
    const auto old_y = history_viewport_y(
        *surface.document(), *frames.front(), scrollbar->value());

    // Ctrl+U at the top reveals another 100 cached rows.
    QKeyEvent press(QEvent::KeyPress, Qt::Key_U, Qt::ControlModifier);
    QCoreApplication::sendEvent(&surface, &press);
    QCoreApplication::processEvents();

    frames = surface.document()->rootFrame()->childFrames();
    sequence = history_sequence(frames);
    if (sequence.size() != 200) {
        throw std::runtime_error(
            "after Ctrl+U the history window must reveal exactly 200 direct "
            "child message frames (messages 005..204), but the surface has "
            + std::to_string(sequence.size()));
    }
    require_history_sequence(sequence, 5, 204, "revealed");
    require_history_banner(*surface.document(), 5, "revealed");

    // The old first-visible message (105, now the 101st frame) keeps its
    // viewport Y after the reveal.
    const auto new_y = history_viewport_y(
        *surface.document(), *frames[100], scrollbar->value());
    if (std::abs(new_y - old_y) > 2.0) {
        throw std::runtime_error(
            "revealing older messages at the top must preserve the previously "
            "first-visible message's viewport Y, but it moved from "
            + std::to_string(old_y) + "px to " + std::to_string(new_y)
            + "px");
    }
}


// A send that starts a new calendar day adds the day-separator top margin.
// After that rebuild, the viewport must sit on the true document bottom so
// the extra separator height is not left below the fold.
void verify_new_day_append_scrolls_to_bottom() {
    ConversationSurface surface;
    surface.resize(420, 180);
    surface.show();
    QCoreApplication::processEvents();

    std::vector<DirectConversationMessage> messages;
    for (auto index = 0; index != 6; ++index) {
        messages.push_back({
            .id = "d1-" + std::to_string(index),
            .outgoing = true,
            .timestamp = "2026-08-07T18:0" + std::to_string(index) + ":00Z",
            .subject = std::string(),
            .text = "same-day padding message " + std::to_string(index)
                + " so the short viewport must scroll",
        });
    }
    surface.set_conversation(QStringLiteral("Bot"), messages);
    QCoreApplication::processEvents();
    auto *bar = surface.verticalScrollBar();
    if (bar->maximum() <= 0) {
        throw std::runtime_error(
            "the same-day history must overflow the short viewport");
    }
    bar->setValue(bar->maximum());

    messages.push_back({
        .id = "d2-send",
        .outgoing = true,
        .timestamp = "2026-08-08T01:00:00Z",
        .subject = std::string(),
        .text = "new-day-send",
    });
    surface.set_conversation(QStringLiteral("Bot"), messages);
    surface.scroll_to_bottom();
    QCoreApplication::processEvents();

    if (bar->value() != bar->maximum()) {
        throw std::runtime_error(
            "a new-day send must pin the conversation to the scrollbar "
            "bottom, but value=" + std::to_string(bar->value())
            + " maximum=" + std::to_string(bar->maximum()));
    }
    const auto frames = surface.document()->rootFrame()->childFrames();
    if (frames.isEmpty()) {
        throw std::runtime_error("new-day send left the conversation empty");
    }
    const auto last_bottom = surface.document()->documentLayout()
        ->frameBoundingRect(frames.back()).bottom();
    const auto visible_bottom = bar->value() + surface.viewport()->height();
    if (last_bottom > visible_bottom + 2.0) {
        throw std::runtime_error(
            "a new-day send left the last message below the fold by "
            + std::to_string(last_bottom - visible_bottom) + "px");
    }
}


// ---------------------------------------------------------------------------
// Time-separators-only RED contract: a chronological stream that crosses a
// midnight must expose only short wall-clock times and never the full raw ISO.
// The incoming messages render their short HH:MM (18:48 and 00:05), the
// outgoing frame carries its timestamp as the short time 19:05 on the
// message-timestamp frame property (QTextFormat::UserProperty + 2), and no
// full "2026-08-07T18:48:52" string survives in the exposed document. Between
// the two day boundaries the document inserts exactly one centered root block
// day separator (August 7, 2026 positioned before the first day-one message
// and August 8, 2026 before the first day-two message), the three message
// frames stay as the only direct child frames of the root in chronological
// order, and a same-day message adds no duplicate separator.
// ---------------------------------------------------------------------------
std::vector<QTextFrame *> day_separator_frames(
        const QList<QTextFrame *> &frames,
        const QString &label) {
    std::vector<QTextFrame *> matches;
    for (auto *frame : frames) {
        if (frame->frameFormat().property(QTextFormat::UserProperty + 6)
                .toString() == label) {
            matches.push_back(frame);
        }
    }
    return matches;
}

QTextFrame *frame_by_body(
        const QList<QTextFrame *> &frames,
        const QString &body) {
    for (auto *frame : frames) {
        for (auto it = frame->begin(); !it.atEnd(); ++it) {
            const auto block = it.currentBlock();
            if (block.isValid() && block.text().contains(body)) {
                return frame;
            }
        }
    }
    return nullptr;
}

void verify_time_separators_only() {
    const std::vector<DirectConversationMessage> messages = {
        {.id = "d1-in", .outgoing = false,
            .timestamp = "2026-08-07T18:48:52",
            .subject = std::string(), .text = "day-one-first"},
        {.id = "d1-out", .outgoing = true,
            .timestamp = "2026-08-07T19:05:01",
            .subject = std::string(), .text = "day-one-second"},
        {.id = "d2-in", .outgoing = false,
            .timestamp = "2026-08-08T00:05:02",
            .subject = std::string(), .text = "day-two-first"},
    };

    ConversationSurface surface;
    surface.resize(kRedViewportWidth, 480);
    surface.show();
    QCoreApplication::processEvents();
    surface.set_conversation(QStringLiteral("Telegram Bot"), messages);
    surface.document()->documentLayout()->documentSize();
    QCoreApplication::processEvents();
    const auto &document = *surface.document();

    const auto plain = document.toPlainText();

    // No full raw ISO may survive anywhere in the exposed document: every
    // stored timestamp must surface only as a short wall-clock time.
    for (const auto *raw : {"2026-08-07T18:48:52", "2026-08-07T19:05:01",
            "2026-08-08T00:05:02"}) {
        if (plain.contains(QString::fromUtf8(raw))) {
            throw std::runtime_error(
                std::string("the time-separators document must expose only "
                    "short wall-clock times, but the full raw ISO '") + raw
                + "' still appears in the rendered plain text");
        }
    }

    // The two incoming messages expose exactly the short wall-clock times
    // 18:48 and 00:05 (HH:MM, minutes only, no seconds).
    if (!plain.contains(QStringLiteral("18:48"))
        || !plain.contains(QStringLiteral("00:05"))) {
        throw std::runtime_error(
            "the time-separators document must expose the short incoming "
            "wall-clock times 18:48 and 00:05, but one or both are absent");
    }

    // The outgoing message's timestamp rides on its frame's message-timestamp
    // property as the short time 19:05, not the full raw ISO.
    const auto frames = document.rootFrame()->childFrames();
    QTextFrame *outgoing = nullptr;
    for (auto *frame : frames) {
        if (is_outgoing_frame(*frame)) {
            outgoing = frame;
        }
    }
    if (!outgoing) {
        throw std::runtime_error(
            "the time-separators document must render the outgoing message "
            "frame so its timestamp property can be checked");
    }
    const auto outgoing_time = outgoing->frameFormat()
        .property(QTextFormat::UserProperty + 2).toString();
    if (outgoing_time != QStringLiteral("19:05")) {
        throw std::runtime_error(
            "the outgoing frame's message-timestamp property must be the short "
            "time 19:05, but it is '" + outgoing_time.toStdString() + "'");
    }

    // Exactly one day divider per day boundary, carried on the first message
    // frame of that day.
    const auto sep_day_one = day_separator_frames(
        frames, QStringLiteral("August 7, 2026"));
    const auto sep_day_two = day_separator_frames(
        frames, QStringLiteral("August 8, 2026"));
    if (sep_day_one.size() != 1 || sep_day_two.size() != 1) {
        throw std::runtime_error(
            "the time-separators document must insert exactly one day divider "
            "per day boundary, but it has "
            + std::to_string(sep_day_one.size()) + " August 7, 2026 and "
            + std::to_string(sep_day_two.size()) + " August 8, 2026");
    }

    // Exactly three direct child message frames remain, in chronological
    // order, one per message with no extra frames.
    if (frames.size() != 3) {
        throw std::runtime_error(
            "the time-separators document must keep exactly three direct child "
            "message frames, but it has " + std::to_string(frames.size()));
    }
    const auto *day_one_in = frame_by_body(frames, QStringLiteral("day-one-first"));
    const auto *day_one_out = frame_by_body(frames, QStringLiteral("day-one-second"));
    const auto *day_two_in = frame_by_body(frames, QStringLiteral("day-two-first"));
    if (!day_one_in || !day_one_out || !day_two_in) {
        throw std::runtime_error(
            "all three message frames must render their bodies in the "
            "time-separators document");
    }
    if (sep_day_one.front() != day_one_in || sep_day_two.front() != day_two_in) {
        throw std::runtime_error(
            "each day divider must ride on the first message frame of that day");
    }
    if (!(day_one_in->firstPosition() < day_one_out->firstPosition()
            && day_one_out->firstPosition() < day_two_in->firstPosition())) {
        throw std::runtime_error(
            "the three message frames must stay in chronological order: "
            "day-one-first, then day-one-second, then day-two-first");
    }

}

// Corrected same-Agent grouping contract from Ted's file20: the group first is
// one flex row (top-aligned avatar beside a sender/time header and body column),
// every body shares the same x-axis, and a same-Agent row within five minutes is
// a headerless continuation with a small gap. A longer pause starts a new group.
void verify_same_agent_grouping_only() {
    ConversationSurface surface;
    surface.resize(1600, 760);
    surface.show();
    QCoreApplication::processEvents();

    const std::vector<DirectConversationMessage> messages = {
        {.id = "in-1", .outgoing = false,
            .timestamp = "2026-08-17T15:40:00Z",
            .subject = "private mail subject one",
            .text = "First Agent line.\nSecond Agent line.\nThird Agent line."},
        {.id = "in-2", .outgoing = false,
            .timestamp = "2026-08-17T15:42:00Z",
            .subject = "private mail subject two",
            .text = "Short-interval continuation."},
        {.id = "in-3", .outgoing = false,
            .timestamp = "2026-08-17T15:50:00Z",
            .subject = "private mail subject three",
            .text = "Long-pause Agent message."},
        {.id = "out-1", .outgoing = true,
            .timestamp = "2026-08-17T15:51:00Z",
            .subject = "private human reply subject",
            .text = "Human reply breaks the Agent group."},
        {.id = "in-4", .outgoing = false,
            .timestamp = "2026-08-17T15:52:00Z",
            .subject = "private mail subject four",
            .text = "Agent message after the Human reply."},
    };
    const auto them = QStringLiteral("Telegram Bot");
    surface.set_conversation(them, messages);
    surface.document()->documentLayout()->documentSize();
    QCoreApplication::processEvents();

    const auto rendered = surface.document()->toPlainText();
    if (rendered.contains(QStringLiteral("private mail subject"))
        || rendered.contains(QStringLiteral("private human reply subject"))) {
        throw std::runtime_error(
            "per-email subject/title metadata must never enter any message "
            "surface in a grouped conversation");
    }

    const auto frames = surface.document()->rootFrame()->childFrames();
    if (frames.size() != 5) {
        throw std::runtime_error(
            "the short-interval grouping fixture must keep five chronological "
            "message frames");
    }
    auto *first = frames[0];
    auto *continuation = frames[1];
    auto *long_pause = frames[2];
    auto *outgoing = frames[3];
    auto *after_human = frames[4];
    if (is_outgoing_frame(*first) || is_outgoing_frame(*continuation)
        || is_outgoing_frame(*long_pause) || !is_outgoing_frame(*outgoing)
        || is_outgoing_frame(*after_human)) {
        throw std::runtime_error(
            "the grouping fixture must remain incoming, incoming, incoming, "
            "outgoing, incoming in chronological order");
    }

    const auto frame_text = [](QTextFrame &frame) {
        auto cursor = QTextCursor(frame.document());
        cursor.setPosition(frame.firstPosition());
        cursor.setPosition(frame.lastPosition(), QTextCursor::KeepAnchor);
        return cursor.selectedText();
    };
    if (!frame_text(*first).contains(them)
        || !frame_text(*long_pause).contains(them)
        || !frame_text(*after_human).contains(them)) {
        throw std::runtime_error(
            "each Agent group first must render sender name directly above its "
            "body with muted time beside it");
    }
    if (frame_text(*continuation).contains(them)) {
        throw std::runtime_error(
            "a same-Agent message within five minutes must not repeat the "
            "avatar/name header");
    }
    for (const auto &fragment : fragments_of(*continuation)) {
        if (fragment.text.startsWith(QStringLiteral(" · "))) {
            throw std::runtime_error(
                "a headerless same-Agent continuation must not repeat the "
                "timestamp metadata either");
        }
    }

    const auto body_left = [](QTextFrame &frame, const QString &needle) {
        for (auto it = frame.begin(); !it.atEnd(); ++it) {
            const auto block = it.currentBlock();
            if (block.isValid() && block.text().contains(needle)) {
                return block.blockFormat().leftMargin();
            }
        }
        throw std::runtime_error("expected grouped body block is missing");
    };
    const auto first_left = body_left(*first, QStringLiteral("First Agent line"));
    const auto continuation_left = body_left(
        *continuation, QStringLiteral("Short-interval continuation"));
    const auto long_pause_left = body_left(
        *long_pause, QStringLiteral("Long-pause Agent message"));
    if (std::abs(first_left - continuation_left) > 0.5
        || std::abs(first_left - long_pause_left) > 0.5) {
        throw std::runtime_error(
            "all assistant body text must start on exactly the same x-axis, "
            "including headerless short-interval continuations");
    }

    const auto within_gap = message_bubble_rect(*continuation).top()
        - message_bubble_rect(*first).bottom();
    const auto time_break = message_bubble_rect(*long_pause).top()
        - message_bubble_rect(*continuation).bottom();
    const auto human_break = message_bubble_rect(*outgoing).top()
        - message_bubble_rect(*long_pause).bottom();
    constexpr auto kSmallGapMin = 4.0;
    constexpr auto kSmallGapMax = 10.0;
    constexpr auto kGroupDelta = 12.0;
    if (within_gap < kSmallGapMin || within_gap > kSmallGapMax) {
        throw std::runtime_error(
            "a short-interval continuation needs a small 4-10px vertical gap, "
            "but the rendered gap is " + std::to_string(within_gap) + "px");
    }
    if (time_break < within_gap + kGroupDelta
        || human_break < within_gap + kGroupDelta) {
        throw std::runtime_error(
            "a long pause or sender change must open a visibly larger group "
            "break than the short-interval continuation gap");
    }

    const auto image = surface.viewport()->grab().toImage();
    const auto h_offset = double(surface.horizontalScrollBar()->value());
    const auto v_offset = double(surface.verticalScrollBar()->value());
    const auto avatar_top_probe = [&](QTextFrame &frame) {
        const auto text = message_text_bounds(frame)
            .translated(-h_offset, -v_offset);
        const auto x = text.left() - 20.0;
        const auto y = text.top() + 10.0;
        const auto px = int(std::lround(x * image.devicePixelRatio()));
        const auto py = int(std::lround(y * image.devicePixelRatio()));
        if (px < 0 || py < 0 || px >= image.width() || py >= image.height()) {
            throw std::runtime_error("group avatar probe is outside the viewport");
        }
        return image.pixelColor(px, py);
    };
    const auto avatar_fill = st::windowBgActive->c;
    const auto backdrop = st::windowBg->c;
    if (avatar_top_probe(*first) != avatar_fill
        || avatar_top_probe(*long_pause) != avatar_fill
        || avatar_top_probe(*after_human) != avatar_fill) {
        throw std::runtime_error(
            "each group-first avatar must align to the top of its sender/body "
            "message column instead of centering against the whole body height");
    }
    if (avatar_top_probe(*continuation) != backdrop) {
        throw std::runtime_error(
            "a short-interval same-Agent continuation must leave the avatar "
            "lane empty");
    }
}

void verify_readable_semantic_body() {
    const auto require = [](bool condition, const char *message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    };
    ConversationSurface surface;
    surface.resize(1200, 760);
    surface.show();
    QCoreApplication::processEvents();

    const auto body = QStringLiteral(
        "# Setup\n\n"
        "普通正文 should stay regular and comfortable while a deliberately long English sentence proves that assistant prose never stretches across the entire wide window.\n\n"
        "Second paragraph keeps one controlled paragraph gap.\n"
        "- first compact list item\n"
        "2. second compact list item\n"
        "Use `inline code`, .secrets/telegram.json, and lingtai.mcp_servers.telegram.\n"
        "```sh\n"
        "lingtai-agent commands --help\n"
        "```");
    std::vector<DirectConversationMessage> messages = {{
        .id = "reading-1",
        .outgoing = false,
        .timestamp = "2026-08-17T12:00:00",
        .subject = "Body reading",
        .text = body.toStdString(),
    }};
    surface.set_conversation(QStringLiteral("codex"), messages);
    surface.document()->documentLayout()->documentSize();
    QCoreApplication::processEvents();

    const auto frames = surface.document()->rootFrame()->childFrames();
    require(frames.size() == 1,
        "the body-reading fixture must own exactly one message frame");
    const auto blocks = frame_blocks(*frames.front());
    const auto find_block = [&](const QString &needle) -> QTextBlock {
        for (const auto &block : blocks) {
            if (block.text().contains(needle)) {
                return block;
            }
        }
        throw std::runtime_error(
            "missing semantic body block: " + needle.toStdString());
    };
    const auto find_run = [&](const QString &prefix) -> QTextCharFormat {
        const auto runs = format_runs(*surface.document(), prefix);
        if (runs.empty()) {
            throw std::runtime_error(
                "missing semantic body run: " + prefix.toStdString());
        }
        return runs.front();
    };

    const auto prose_block = find_block(QStringLiteral("普通正文"));
    const auto prose = find_run(QStringLiteral("普通正文"));
    require(prose.font().pixelSize() >= 13 && prose.font().pixelSize() <= 14
            && prose.font().weight() == QFont::Normal
            && !prose.font().fixedPitch(),
        "ordinary Chinese/English prose must use the system sans body at "
        "13-14px Normal/400 without synthetic bold");
    require(prose.foreground().color() == QColor(QStringLiteral("#26282B")),
        "light-palette body prose must use the softer #26282B reading tone");
    require(prose_block.blockFormat().lineHeightType()
                == QTextBlockFormat::ProportionalHeight
            && prose_block.blockFormat().lineHeight() >= 125
            && prose_block.blockFormat().lineHeight() <= 140,
        "ordinary body prose must use a ~1.32 proportional line height");
    const auto effective_width = surface.viewport()->width()
        - int(prose_block.blockFormat().leftMargin())
        - int(prose_block.blockFormat().rightMargin());
    require(effective_width <= 570,
        "wide assistant prose must stay within a roughly 65-72-character "
        "reading lane (effective width <= 570px)");

    auto empty_blocks = 0;
    for (const auto &block : blocks) {
        if (block.text().isEmpty()) {
            ++empty_blocks;
        }
    }
    require(empty_blocks == 0,
        "blank Markdown paragraph delimiters must become controlled margins, "
        "not giant empty QTextBlocks");
    require(prose_block.blockFormat().bottomMargin() >= 8
            && prose_block.blockFormat().bottomMargin() <= 12,
        "paragraph spacing must be about 0.7em (8-12px at this body size)");

    const auto list = find_block(QStringLiteral("first compact list item"));
    const auto numbered = find_block(QStringLiteral("second compact list item"));
    for (const auto &block : { list, numbered }) {
        require(block.blockFormat().textIndent() < 0
                && block.blockFormat().leftMargin()
                    > prose_block.blockFormat().leftMargin()
                && block.blockFormat().bottomMargin() <= 4,
            "numbered and bulleted lists must use a shallow hanging indent "
            "with tight item spacing");
    }

    const auto inline_code = find_run(QStringLiteral("inline code"));
    const auto path = find_run(QStringLiteral(".secrets/telegram.json"));
    const auto identifier = find_run(
        QStringLiteral("lingtai.mcp_servers.telegram"));
    for (const auto &format : { inline_code, path, identifier }) {
        require(format.font().fixedPitch()
                && format.background().style() != Qt::NoBrush
                && format.background().color().alpha() > 0,
            "inline code, paths, and dotted IDs must use monospace ink on a "
            "subtle tinted background");
    }

    const auto heading = find_block(QStringLiteral("Setup"));
    const auto heading_run = find_run(QStringLiteral("Setup"));
    require(heading_run.font().pixelSize() > prose.font().pixelSize()
            && heading_run.font().weight() == QFont::DemiBold
            && heading.blockFormat().bottomMargin() <= 14,
        "Markdown headings must be slightly larger/semibold with controlled "
        "spacing");
    const auto code_block = find_block(
        QStringLiteral("lingtai-agent commands --help"));
    const auto code_run = find_run(
        QStringLiteral("lingtai-agent commands --help"));
    require(code_run.font().fixedPitch()
            && code_run.font().family() != prose.font().family()
            && code_block.blockFormat().background().style() != Qt::NoBrush
            && (code_run.background().style() == Qt::NoBrush
                || code_run.background().color().alpha() == 0),
        "fenced code must use a distinct monospace family on a semantic "
        "block surface (paint owns the visible panel; char wash stays clear)");

    const auto sender = find_run(QStringLiteral("codex"));
    const auto time = find_run(QStringLiteral(" · 12:00"));
    require(sender.font().pixelSize() >= 14 && sender.font().pixelSize() <= 15
            && sender.font().weight() == QFont::DemiBold,
        "the sender line must stay 14-15px semibold above the body size rung");
    require(time.font().pixelSize() >= 12 && time.font().pixelSize() <= 13
            && time.font().weight() == QFont::Normal
            && time.foreground().color() == QColor(QStringLiteral("#8A8F98")),
        "message time must stay 12-13px Normal in the muted #8A8F98 tone");
}

void verify_message_attachment_presentation() {
    const auto check = [](bool condition, const char *message) {
        if (!condition) throw std::runtime_error(message);
    };
    QTemporaryDir temporary;
    check(temporary.isValid(), "attachment presentation sandbox must exist");
    const auto root = std::filesystem::path(
        temporary.path().toStdString());
    const auto image_path = root / "market-map.png";
    QImage image(640, 360, QImage::Format_RGB32);
    image.fill(QColor(QStringLiteral("#3A8D73")));
    check(image.save(QString::fromStdString(image_path.string())),
        "valid image fixture must save");
    const auto file_path = root /
        "quarterly-risk-report-with-a-deliberately-very-long-filename.pdf";
    std::ofstream(file_path, std::ios::binary) << "Risk bounded.\n";
    const auto corrupt_path = root / "corrupt-preview.png";
    std::ofstream(corrupt_path, std::ios::binary) << "not an image";

    const auto messages = std::vector<DirectConversationMessage>{
        {
            .id = "incoming-attachments",
            .outgoing = false,
            .timestamp = "2026-08-24T10:00:00Z",
            .text = "Incoming text remains visible.",
            .attachments = {
                projected_attachment(image_path, AttachmentMediaKind::image),
                projected_attachment(file_path, AttachmentMediaKind::file),
            },
        },
        {
            .id = "outgoing-attachment",
            .outgoing = true,
            .timestamp = "2026-08-24T10:01:00Z",
            .text = "Outgoing text remains visible.",
            .attachments = {
                projected_attachment(corrupt_path, AttachmentMediaKind::image),
            },
        },
    };

    ConversationSurface surface;
    surface.resize(360, 620);
    surface.show();
    surface.set_conversation(QStringLiteral("Agent"), messages);
    QCoreApplication::processEvents();
    const auto plain = surface.document()->toPlainText();
    check(plain.contains(QStringLiteral("Incoming text remains visible."))
            && plain.contains(QStringLiteral("Outgoing text remains visible.")),
        "attachment rendering must not hide either message text");
    check(plain.contains(QStringLiteral("market-map.png"))
            && plain.contains(QStringLiteral("corrupt-preview.png"))
            && plain.indexOf(QStringLiteral("market-map.png"))
                < plain.indexOf(QStringLiteral("corrupt-preview.png")),
        "incoming/outgoing attachment cards must retain message chronology");
    check(plain.contains(QStringLiteral("PDF file"))
            && plain.contains(QStringLiteral("bytes"))
            && plain.contains(QStringLiteral("Open"))
            && plain.contains(QStringLiteral("Reveal in Finder")),
        "ordinary cards must expose stable type, human size, and both actions");

    auto image_count = 0;
    auto action_count = 0;
    auto long_name_tooltip = false;
    for (auto block = surface.document()->begin(); block.isValid();
            block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const auto format = fragment.charFormat();
            if (format.isImageFormat()) {
                const auto image_format = format.toImageFormat();
                ++image_count;
                check(image_format.width() <= 180
                        && image_format.height() <= 120,
                    "decoded thumbnail must remain inside its hard bounds");
            }
            if (format.isAnchor()
                && format.anchorHref().startsWith(
                    QStringLiteral("lingtai-attachment-action://"))) {
                ++action_count;
            }
            if (format.toolTip().contains(QStringLiteral(
                    "quarterly-risk-report-with-a-deliberately-very-long-filename.pdf"))) {
                long_name_tooltip = true;
            }
        }
    }
    check(image_count == 1,
        "only the safely decoded image may render inline; corrupt image falls back");
    check(action_count == 6,
        "every ordered attachment card must have one Open and one Reveal link");
    check(long_name_tooltip
            && surface.accessibleDescription().contains(QStringLiteral(
                "quarterly-risk-report-with-a-deliberately-very-long-filename.pdf")),
        "elided filename must retain full tooltip and accessibility metadata");

    const auto frames = surface.document()->rootFrame()->childFrames();
    check(frames.size() == 2,
        "attachments must remain inside their two owning message frames");
    const auto first = surface.document()->documentLayout()
        ->frameBoundingRect(frames[0]);
    const auto second = surface.document()->documentLayout()
        ->frameBoundingRect(frames[1]);
    check(first.right() <= surface.document()->size().width() + 1
            && second.right() <= surface.document()->size().width() + 1
            && first.bottom() <= second.top() + 1,
        "narrow attachment frames must reflow without viewport escape or overlap");

    struct Emission final {
        DirectConversationAttachmentRequest request;
        bool reveal = false;
    };
    auto emissions = std::vector<Emission>();
    QObject::connect(
        &surface,
        &ConversationSurface::attachment_action_requested,
        &surface,
        [&](const DirectConversationAttachmentRequest &request, bool reveal) {
            emissions.push_back({request, reveal});
        });

    auto open_point = QPoint();
    auto reveal_point = QPoint();
    auto body_point = QPoint();
    for (auto block = surface.document()->begin(); block.isValid();
            block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const auto format = fragment.charFormat();
            const auto point_in_fragment = [&](int offset) {
                auto cursor = QTextCursor(surface.document());
                cursor.setPosition(fragment.position()
                    + qBound(0, offset, fragment.length() - 1));
                return surface.cursorRect(cursor).center() + QPoint(2, 0);
            };
            if (body_point.isNull()
                && fragment.text().contains(QStringLiteral(
                    "Incoming text remains visible."))) {
                body_point = point_in_fragment(3);
            }
            if (!format.isAnchor()) continue;
            if (open_point.isNull()
                && fragment.text() == QStringLiteral("Open")) {
                open_point = point_in_fragment(2);
            } else if (reveal_point.isNull()
                && fragment.text() == QStringLiteral("Reveal in Finder")) {
                reveal_point = point_in_fragment(4);
            }
        }
    }
    check(!open_point.isNull() && !reveal_point.isNull()
            && !body_point.isNull(),
        "real pointer regression must locate body and action glyphs");

    const auto send_press = [&](const QPoint &point) {
        auto event = QMouseEvent(
            QEvent::MouseButtonPress,
            point,
            point,
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(surface.viewport(), &event);
    };
    const auto send_move = [&](const QPoint &point) {
        auto event = QMouseEvent(
            QEvent::MouseMove,
            point,
            point,
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(surface.viewport(), &event);
    };
    const auto send_release = [&](const QPoint &point) {
        auto event = QMouseEvent(
            QEvent::MouseButtonRelease,
            point,
            point,
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier);
        QApplication::sendEvent(surface.viewport(), &event);
    };
    const auto assert_first_request = [&](const Emission &emission) {
        const auto &expected = messages[0].attachments[0];
        check(emission.request.message_id == messages[0].id
                && emission.request.attachment_index == 0
                && emission.request.presented.local_path == expected.local_path
                && emission.request.presented.display_filename
                    == expected.display_filename
                && emission.request.presented.byte_size == expected.byte_size
                && emission.request.presented.media_kind == expected.media_kind
                && emission.request.presented.device_id == expected.device_id
                && emission.request.presented.inode_id == expected.inode_id,
            "real click must emit the exact displayed attachment request");
    };

    send_press(open_point);
    send_release(open_point);
    check(emissions.size() == 1 && !emissions[0].reveal,
        "a genuine same-anchor Open click must emit exactly once");
    assert_first_request(emissions[0]);

    send_press(reveal_point);
    send_release(reveal_point);
    check(emissions.size() == 2 && emissions[1].reveal,
        "a genuine same-anchor Reveal click must emit exactly once");
    assert_first_request(emissions[1]);

    send_press(open_point);
    send_release(reveal_point);
    check(emissions.size() == 2,
        "press/release href mismatch must not activate either action");

    auto clear_selection = QTextCursor(surface.document());
    clear_selection.setPosition(0);
    surface.setTextCursor(clear_selection);
    send_press(body_point);
    send_move(open_point);
    send_release(open_point);
    check(emissions.size() == 2,
        "selection drag ending over an attachment action must emit nothing");
    check(surface.textCursor().hasSelection()
            && !surface.textCursor().selectedText().isEmpty(),
        "selection drag ending over an action must preserve QTextEdit selection");
}

void verify_revision_noop_and_incremental_append() {
    const auto ensure = [](bool condition, const char *message) {
        if (!condition) throw std::runtime_error(message);
    };
    ConversationSurface surface;
    surface.setAttribute(Qt::WA_DontShowOnScreen);
    surface.resize(520, 220);
    surface.show();
    QCoreApplication::processEvents();

    auto messages = std::vector<DirectConversationMessage>();
    for (auto index = 0; index != 205; ++index) {
        messages.push_back({
            .id = "revision-" + std::to_string(index),
            .outgoing = index % 3 == 0,
            .timestamp = "2026-08-07T19:"
                + std::to_string(10 + index / 60) + ":00Z",
            .text = "body " + std::to_string(index)
                + "\nsecond line\nthird line",
            .mailbox_folder = index % 3 == 0 ? "sent" : "inbox",
        });
    }
    auto revision = ConversationPresentationRevision{
        .history = 10, .reactions = 20, .session_events = 30};
    surface.set_conversation(
        QStringLiteral("Agent"), messages, {}, {}, revision);
    auto *bar = surface.verticalScrollBar();
    ensure(bar->maximum() > 0,
        "revision fixture must genuinely overflow the viewport");
    bar->setValue(bar->minimum());
    QKeyEvent reveal(QEvent::KeyPress, Qt::Key_U, Qt::ControlModifier);
    QCoreApplication::sendEvent(&surface, &reveal);
    QCoreApplication::processEvents();
    ensure(surface.toPlainText().contains(
            QStringLiteral("▲ 5 older — ctrl+u to load")),
        "Ctrl+U must establish the expanded 200-row window");

    auto cursor = surface.textCursor();
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor, 2);
    surface.setTextCursor(cursor);
    surface.setFocus();
    QCoreApplication::processEvents();
    const auto focus_before = surface.hasFocus();
    const auto scroll_before = bar->value();
    const auto revision_before = surface.document()->revision();
    const auto frames_before = surface.document()->rootFrame()->childFrames();
    auto *first_frame = frames_before.front();
    const auto first_block = first_frame->begin().currentBlock();
    auto content_changes = 0;
    const auto connection = QObject::connect(surface.document(),
        &QTextDocument::contentsChange,
        [&content_changes](int, int, int) { ++content_changes; });

    auto impossible_same_revision_change = messages;
    impossible_same_revision_change.front().text =
        "must not be inspected on an accepted unchanged revision";
    surface.set_conversation(QStringLiteral("Agent"),
        impossible_same_revision_change, {}, {}, revision);
    ensure(content_changes == 0
            && surface.document()->revision() == revision_before
            && surface.document()->rootFrame()->childFrames().front()
                == first_frame
            && first_block.isValid()
            && surface.textCursor().hasSelection()
            && surface.hasFocus() == focus_before
            && bar->value() == scroll_before
            && surface.toPlainText().contains(QStringLiteral("body 5")),
        "same independent revisions must be an O(1) document no-op");

    bar->setValue(bar->maximum());
    messages.push_back({
        .id = "revision-new-day",
        .outgoing = false,
        .timestamp = "2026-08-08T00:01:00Z",
        .text = "new-day suffix",
        .mailbox_folder = "inbox",
    });
    auto append_revision = ConversationPresentationRevision{
        .history = 11,
        .append_from_history = 10,
        .append_from = 205,
        .reactions = 20,
        .session_events = 30,
    };
    surface.set_conversation(QStringLiteral("Agent"),
        messages, {}, {}, append_revision);
    QCoreApplication::processEvents();
    auto frames_after = surface.document()->rootFrame()->childFrames();
    ensure(frames_after.size() == frames_before.size() + 1
            && frames_after.front() == first_frame
            && first_block.isValid()
            && surface.toPlainText().count(QStringLiteral("new-day suffix")) == 1
            && surface.toPlainText().contains(
                QStringLiteral("▲ 5 older — ctrl+u to load"))
            && bar->value() == bar->maximum(),
        "proven append must retain old frames/window and follow true new bottom");

    bar->setValue(std::max(1, bar->maximum() / 3));
    const auto scrolled_value = bar->value();
    messages.push_back({
        .id = "revision-scrolled",
        .outgoing = true,
        .timestamp = "2026-08-08T00:02:00Z",
        .text = "scrolled suffix",
        .mailbox_folder = "sent",
    });
    const auto second_append = ConversationPresentationRevision{
        .history = 12,
        .append_from_history = 11,
        .append_from = 206,
        .reactions = 20,
        .session_events = 30,
    };
    surface.set_conversation(
        QStringLiteral("Agent"), messages, {}, {}, second_append);
    QCoreApplication::processEvents();
    ensure(surface.document()->rootFrame()->childFrames().front() == first_frame
            && bar->value() == scrolled_value
            && surface.toPlainText().count(
                QStringLiteral("scrolled suffix")) == 1,
        "append while scrolled up must retain the prior viewport and frames");

    auto replacement = messages;
    replacement[10].text = "replacement path";
    const auto before_replacement_changes = content_changes;
    surface.set_conversation(QStringLiteral("Agent"), replacement, {}, {},
        ConversationPresentationRevision{
            .history = 13, .reactions = 20, .session_events = 30});
    ensure(content_changes > before_replacement_changes
            && surface.toPlainText().contains(QStringLiteral("replacement path")),
        "a replacement/revision gap must fall back to a complete rebuild");
    QObject::disconnect(connection);
}

QString rect_fingerprint(const QRectF &rect) {
    return QStringLiteral("%1,%2,%3,%4")
        .arg(rect.x(), 0, 'f', 3)
        .arg(rect.y(), 0, 'f', 3)
        .arg(rect.width(), 0, 'f', 3)
        .arg(rect.height(), 0, 'f', 3);
}

// Serializes the geometry consumed by paintEvent, including both QTextLine
// rectangles, the glyph-tight naturalTextRect, frame origins/margins, the
// derived Human bubble, external timestamp, and current scroll translation.
QStringList presented_geometry(ConversationSurface &surface) {
    surface.document()->documentLayout()->documentSize();
    const auto frames = surface.document()->rootFrame()->childFrames();
    if (frames.isEmpty()) {
        throw std::runtime_error("presentation fixture has no message frame");
    }
    auto *frame = frames.back();
    const auto frame_format = frame->frameFormat();
    const auto frame_rect = surface.document()->documentLayout()
        ->frameBoundingRect(frame);
    auto result = QStringList();
    const auto h_offset = surface.horizontalScrollBar()->value();
    const auto v_offset = surface.verticalScrollBar()->value();
    auto text_bounds = QRectF();
    QTextBlock first_block;
    for (auto it = frame->begin(); !it.atEnd(); ++it) {
        const auto block = it.currentBlock();
        if (!block.isValid()) continue;
        if (!first_block.isValid()) first_block = block;
        const auto format = block.blockFormat();
        const auto *layout = block.layout();
        result.push_back(QStringLiteral(
            "block pos=%1 margins=%2,%3,%4,%5 lines=%6")
            .arg(rect_fingerprint(QRectF(layout->position(), QSizeF())))
            .arg(format.leftMargin(), 0, 'f', 3)
            .arg(format.topMargin(), 0, 'f', 3)
            .arg(format.rightMargin(), 0, 'f', 3)
            .arg(format.bottomMargin(), 0, 'f', 3)
            .arg(layout->lineCount()));
        for (auto index = 0; index != layout->lineCount(); ++index) {
            const auto line = layout->lineAt(index);
            const auto natural = line.naturalTextRect()
                .translated(layout->position())
                .translated(frame_rect.topLeft());
            text_bounds = text_bounds.isNull()
                ? natural : text_bounds.united(natural);
            result.push_back(QStringLiteral("line rect=%1 natural=%2 advance=%3")
                .arg(rect_fingerprint(line.rect()))
                .arg(rect_fingerprint(line.naturalTextRect()))
                .arg(line.horizontalAdvance(), 0, 'f', 3));
        }
    }
    if (!first_block.isValid() || first_block.layout()->lineCount() == 0) {
        throw std::runtime_error("appended message has no laid-out body line");
    }
    const auto lane_right = first_block.layout()->lineAt(0).rect()
        .translated(first_block.layout()->position())
        .translated(frame_rect.topLeft()).right();
    auto bubble = text_bounds.adjusted(-15, -11, 15, 11);
    bubble.setRight(lane_right + 15);
    const auto timestamp = frame_format
        .property(kMessageTimestampProperty).toString();
    auto timestamp_font = fragments_of(first_block).front().font;
    timestamp_font.setPixelSize(12);
    const auto metrics = QFontMetricsF(timestamp_font);
    const auto time_rect = QRectF(
        bubble.right() - metrics.horizontalAdvance(timestamp),
        bubble.bottom() + 4,
        metrics.horizontalAdvance(timestamp), metrics.height());
    result.push_back(QStringLiteral(
        "frame=%1 margins=%2,%3,%4,%5 outgoing=%6 reactions=%7")
        .arg(rect_fingerprint(frame_rect))
        .arg(frame_format.leftMargin(), 0, 'f', 3)
        .arg(frame_format.topMargin(), 0, 'f', 3)
        .arg(frame_format.rightMargin(), 0, 'f', 3)
        .arg(frame_format.bottomMargin(), 0, 'f', 3)
        .arg(frame_format.property(kMessageOutgoingProperty).toBool())
        .arg(frame_format.property(kMessageReactionsProperty)
            .toStringList().join('|')));
    result.push_back(QStringLiteral("bubble=%1 timestamp=%2 viewportBubble=%3")
        .arg(rect_fingerprint(bubble))
        .arg(rect_fingerprint(time_rect))
        .arg(rect_fingerprint(bubble.translated(-h_offset, -v_offset))));
    result.push_back(QStringLiteral(
        "viewport=%1x%2 scroll=%3,%4 doc=%5x%6 margin=%7")
        .arg(surface.viewport()->width())
        .arg(surface.viewport()->height())
        .arg(h_offset).arg(v_offset)
        .arg(surface.document()->size().width(), 0, 'f', 3)
        .arg(surface.document()->size().height(), 0, 'f', 3)
        .arg(surface.document()->documentMargin(), 0, 'f', 3));
    return result;
}

struct PresentedState {
    QStringList geometry;
    std::vector<QTextBlockFormat> block_formats;
    QImage viewport;
};

PresentedState capture_presented_state(ConversationSurface &surface) {
    auto block_formats = std::vector<QTextBlockFormat>();
    const auto frames = surface.document()->rootFrame()->childFrames();
    if (frames.isEmpty()) {
        throw std::runtime_error("presentation fixture has no message frame");
    }
    for (auto it = frames.back()->begin(); !it.atEnd(); ++it) {
        const auto block = it.currentBlock();
        if (block.isValid()) block_formats.push_back(block.blockFormat());
    }
    return {presented_geometry(surface), std::move(block_formats),
        surface.viewport()->grab().toImage()};
}

QString block_format_fingerprint(const QTextBlockFormat &format) {
    return QStringLiteral(
        "margins=%1,%2,%3,%4 message=%5 attachment=%6 "
        "attachmentMessageId=%7 attachmentIndex=%8 code=%9 "
        "alignment=%10 indent=%11 textIndent=%12")
        .arg(format.leftMargin(), 0, 'f', 3)
        .arg(format.topMargin(), 0, 'f', 3)
        .arg(format.rightMargin(), 0, 'f', 3)
        .arg(format.bottomMargin(), 0, 'f', 3)
        .arg(format.property(kMessageBlockProperty).toBool())
        .arg(format.property(kAttachmentBlockProperty).toBool())
        .arg(format.property(kAttachmentMessageIdProperty).toString())
        .arg(format.property(kAttachmentIndexProperty).toULongLong())
        .arg(format.property(kCodeBlockProperty).toBool())
        .arg(static_cast<int>(format.alignment()))
        .arg(format.indent())
        .arg(format.textIndent(), 0, 'f', 3);
}

void require_same_presentation(
        const PresentedState &incremental,
        const PresentedState &rebuilt,
        const char *phase) {
    if (incremental.block_formats != rebuilt.block_formats) {
        auto mismatch = std::size_t{0};
        const auto count = std::min(
            incremental.block_formats.size(), rebuilt.block_formats.size());
        while (mismatch != count
                && incremental.block_formats[mismatch]
                    == rebuilt.block_formats[mismatch]) {
            ++mismatch;
        }
        throw std::runtime_error(std::string(phase)
            + " incremental block format differs from clean rebuild at block "
            + std::to_string(mismatch) + ": incremental='"
            + (mismatch < incremental.block_formats.size()
                ? block_format_fingerprint(
                    incremental.block_formats[mismatch]).toStdString()
                : "<end>")
            + "' rebuilt='"
            + (mismatch < rebuilt.block_formats.size()
                ? block_format_fingerprint(
                    rebuilt.block_formats[mismatch]).toStdString()
                : "<end>") + "'");
    }
    if (incremental.geometry != rebuilt.geometry) {
        auto mismatch = 0;
        const auto count = std::min(
            incremental.geometry.size(), rebuilt.geometry.size());
        while (mismatch != count
                && incremental.geometry[mismatch] == rebuilt.geometry[mismatch]) {
            ++mismatch;
        }
        throw std::runtime_error(std::string(phase)
            + " incremental geometry differs from clean rebuild at item "
            + std::to_string(mismatch) + ": incremental='"
            + (mismatch < incremental.geometry.size()
                ? incremental.geometry[mismatch].toStdString() : "<end>")
            + "' rebuilt='"
            + (mismatch < rebuilt.geometry.size()
                ? rebuilt.geometry[mismatch].toStdString() : "<end>") + "'");
    }
    if (incremental.viewport != rebuilt.viewport) {
        auto changed = 0;
        auto bounds = QRect();
        for (auto y = 0; y != incremental.viewport.height(); ++y) {
            for (auto x = 0; x != incremental.viewport.width(); ++x) {
                if (incremental.viewport.pixel(x, y)
                        != rebuilt.viewport.pixel(x, y)) {
                    ++changed;
                    bounds = bounds.isNull() ? QRect(x, y, 1, 1)
                        : bounds.united(QRect(x, y, 1, 1));
                }
            }
        }
        throw std::runtime_error(std::string(phase)
            + " incremental viewport differs from clean rebuild: "
            + std::to_string(changed) + " pixels in "
            + rect_fingerprint(bounds).toStdString());
    }
}

void verify_incremental_append_matches_clean_presentation() {
    const auto revision = [](std::uint64_t history, std::uint64_t reactions,
            std::size_t append_from = std::numeric_limits<std::size_t>::max(),
            std::uint64_t parent = 0) {
        return ConversationPresentationRevision{
            .history = history,
            .append_from_history = parent,
            .append_from = append_from,
            .reactions = reactions,
            .session_events = 30,
        };
    };
    auto messages = std::vector<DirectConversationMessage>();
    for (auto index = 0; index != 18; ++index) {
        messages.push_back({
            .id = "presented-" + std::to_string(index),
            .outgoing = index % 3 == 0,
            .timestamp = "2026-08-24T18:" + std::to_string(10 + index) + ":00Z",
            .text = "Established conversation row " + std::to_string(index)
                + " with enough body text to exercise the real wrapping lane.",
            .mailbox_folder = index % 3 == 0 ? "sent" : "inbox",
        });
    }
    auto reactions = std::unordered_map<std::string, MessageReactions>();
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        throw std::runtime_error("append presentation sandbox must exist");
    }
    const auto attachment_path = std::filesystem::path(
        temporary.path().toStdString()) / "pure-append-evidence.txt";
    std::ofstream(attachment_path, std::ios::binary)
        << "Attachment survives incremental transplant.\n";
    const auto attachment = projected_attachment(
        attachment_path, AttachmentMediaKind::file);
    ConversationSurface incremental;
    incremental.setAttribute(Qt::WA_DontShowOnScreen);
    incremental.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    incremental.resize(640, 280);
    incremental.show();
    incremental.set_conversation(QStringLiteral("Agent"), messages,
        reactions, {}, revision(40, 50));
    incremental.scroll_to_bottom();
    QCoreApplication::processEvents();

    const auto first_append_from = messages.size();
    messages.push_back({
        .id = "first-real-send",
        .outgoing = true,
        .timestamp = "2026-08-24T19:00:00Z",
        .text = "OK, commit to main and push.",
        .attachments = {attachment},
        .mailbox_folder = "sent",
    });
    incremental.set_conversation(QStringLiteral("Agent"), messages,
        reactions, {}, revision(41, 50, first_append_from, 40));
    const auto first_incremental = capture_presented_state(incremental);

    ConversationSurface first_reference;
    first_reference.setAttribute(Qt::WA_DontShowOnScreen);
    first_reference.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    first_reference.resize(incremental.size());
    first_reference.show();
    first_reference.set_conversation(QStringLiteral("Agent"), messages,
        reactions, {}, revision(41, 50));
    first_reference.scroll_to_bottom();
    QCoreApplication::processEvents();
    const auto first_rebuilt = capture_presented_state(first_reference);
    require_same_presentation(first_incremental, first_rebuilt, "first append");
    if (first_incremental.block_formats.size() < 2) {
        throw std::runtime_error(
            "pure attachment append must render a tail block");
    }
    const auto &incremental_attachment = first_incremental.block_formats.back();
    if (!incremental_attachment
                .property(kAttachmentBlockProperty).toBool()
            || incremental_attachment
                .property(kAttachmentMessageIdProperty).toString()
                != QStringLiteral("first-real-send")
            || incremental_attachment
                .property(kAttachmentIndexProperty).toULongLong() != 0) {
        throw std::runtime_error(
            "pure attachment append must retain its tail block semantics");
    }

    MessageReactions received;
    received.list.push_back({
        .id = receipt_reaction_id(ReceiptStage::received),
        .count = 1,
        .mine = true,
        .source = ReactionSource::system_receipt,
    });
    reactions.emplace("first-real-send", received);
    incremental.set_conversation(QStringLiteral("Agent"), messages,
        reactions, {}, revision(41, 51));
    incremental.scroll_to_bottom();
    QCoreApplication::processEvents();

    const auto second_append_from = messages.size();
    messages.push_back({
        .id = "second-real-send",
        .outgoing = true,
        .timestamp = "2026-08-24T19:01:00Z",
        .text = "等一下\nCheck the second QTextBlock before continuing.",
        .mailbox_folder = "sent",
    });
    incremental.set_conversation(QStringLiteral("Agent"), messages,
        reactions, {}, revision(42, 51, second_append_from, 41));
    const auto second_incremental = capture_presented_state(incremental);

    ConversationSurface second_reference;
    second_reference.setAttribute(Qt::WA_DontShowOnScreen);
    second_reference.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    second_reference.resize(incremental.size());
    second_reference.show();
    second_reference.set_conversation(QStringLiteral("Agent"), messages,
        reactions, {}, revision(42, 51));
    second_reference.scroll_to_bottom();
    QCoreApplication::processEvents();
    const auto second_rebuilt = capture_presented_state(second_reference);
    require_same_presentation(second_incremental, second_rebuilt, "second append");
    if (second_incremental.block_formats.size() < 2) {
        throw std::runtime_error(
            "real newline append must render multiple QTextBlocks");
    }
}

void set_test_palette_color(const char *name, const char *hex) {
    const auto color = QColor(QString::fromLatin1(hex));
    const auto result = style::main_palette::setColor(
        QLatin1String(name),
        static_cast<uchar>(color.red()),
        static_cast<uchar>(color.green()),
        static_cast<uchar>(color.blue()),
        static_cast<uchar>(color.alpha()));
    if (result != style::palette::SetResult::Ok
        && result != style::palette::SetResult::Duplicate) {
        throw std::runtime_error("receipt palette fixture must apply");
    }
}

void apply_receipt_test_palette(bool dark) {
    style::main_palette::reset();
    if (!dark) {
        return;
    }
    set_test_palette_color("windowBg", "#17212B");
    set_test_palette_color("windowBgOver", "#232E3C");
    set_test_palette_color("windowBgRipple", "#24303D");
    set_test_palette_color("historyTextOutFg", "#E4ECF2");
    set_test_palette_color("msgServiceFg", "#708499");
}

double relative_luminance(const QColor &color) {
    const auto linear = [](double component) {
        component /= 255.0;
        return component <= 0.04045
            ? component / 12.92
            : std::pow((component + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.red())
        + 0.7152 * linear(color.green())
        + 0.0722 * linear(color.blue());
}

double contrast_ratio(const QColor &first, const QColor &second) {
    const auto first_luminance = relative_luminance(first);
    const auto second_luminance = relative_luminance(second);
    return (std::max(first_luminance, second_luminance) + 0.05)
        / (std::min(first_luminance, second_luminance) + 0.05);
}

int exact_color_pixel_count(
        const QImage &image,
        const QColor &color) {
    const auto expected = color.rgba();
    auto count = 0;
    for (auto y = 0; y != image.height(); ++y) {
        for (auto x = 0; x != image.width(); ++x) {
            count += image.pixel(x, y) == expected ? 1 : 0;
        }
    }
    return count;
}

int exact_color_pill_count(const QImage &image, const QColor &color) {
    const auto expected = color.rgba();
    auto rows = std::vector<bool>(std::size_t(image.height()), false);
    for (auto y = 0; y != image.height(); ++y) {
        for (auto x = 0; x != image.width(); ++x) {
            if (image.pixel(x, y) == expected) {
                rows[std::size_t(y)] = true;
                break;
            }
        }
    }
    auto pills = 0;
    for (auto top = 0; top != image.height();) {
        if (!rows[std::size_t(top)]) {
            ++top;
            continue;
        }
        auto bottom = top;
        while (bottom + 1 < image.height()
            && rows[std::size_t(bottom + 1)]) {
            ++bottom;
        }
        auto columns = std::vector<bool>(std::size_t(image.width()), false);
        for (auto y = top; y <= bottom; ++y) {
            for (auto x = 0; x != image.width(); ++x) {
                columns[std::size_t(x)] = columns[std::size_t(x)]
                    || image.pixel(x, y) == expected;
            }
        }
        for (auto x = 0; x != image.width();) {
            if (!columns[std::size_t(x)]) {
                ++x;
                continue;
            }
            ++pills;
            while (x < image.width() && columns[std::size_t(x)]) {
                ++x;
            }
        }
        top = bottom + 1;
    }
    return pills;
}

QImage render_receipt_chips(bool dark, bool include_peer_text = true) {
    apply_receipt_test_palette(dark);
    auto store = MessageReactionStore();
    store.set_receipt("received", ReceiptStage::received);
    store.set_receipt("seen", ReceiptStage::seen);
    store.set_receipt("replied", ReceiptStage::replied);
    if (include_peer_text) {
        store.upsert_peer_reaction(
            "replied", ReactionId{"acknowledge"}, "alpha");
    }

    const auto messages = std::vector<DirectConversationMessage>{
        {.id = "received", .outgoing = true,
            .timestamp = "2026-08-25T09:01:00Z",
            .text = "Human message: received receipt below."},
        {.id = "seen", .outgoing = true,
            .timestamp = "2026-08-25T09:02:00Z",
            .text = "Human message: seen receipt below."},
        {.id = "replied", .outgoing = true,
            .timestamp = "2026-08-25T09:03:00Z",
            .text = "Human message: replied receipt below."},
    };
    ConversationSurface surface;
    surface.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    surface.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    surface.resize(820, 620);
    surface.show();
    surface.set_conversation(
        QStringLiteral("Agent"), messages, store.all());
    QCoreApplication::processEvents();
    auto image = QImage(surface.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    surface.render(&image);
    return image;
}

void verify_unified_receipt_chip_fill(const QString &preview_path = {}) {
    const auto dark_fill = QColor(QStringLiteral("#45665A"));
    const auto old_dark_fill = QColor(QStringLiteral("#354F46"));
    const auto light_fill = QColor(QStringLiteral("#D7EBE3"));
    const auto dark_text = QColor(QStringLiteral("#E4ECF2"));
    const auto human_bubble = QColor(QStringLiteral("#2A4038"));

    const auto dark = render_receipt_chips(true);
    const auto dark_pills = exact_color_pill_count(dark, dark_fill);
    if (dark_pills != 4) {
        throw std::runtime_error(
            "received, seen, replied, and future peer-text chips must share "
            "the selected #45665A dark fill (found "
            + std::to_string(dark_pills) + " pills)");
    }
    const auto old_dark_pixels = exact_color_pixel_count(dark, old_dark_fill);
    if (old_dark_pixels >= 20) {
        throw std::runtime_error(
            "the superseded #354F46 dark receipt fill must not render as a "
            "solid chip area (found " + std::to_string(old_dark_pixels)
            + " exact pixels)");
    }
    if (dark_fill.lightnessF() <= human_bubble.lightnessF()) {
        throw std::runtime_error(
            "the dark receipt fill must stay lighter than the Human bubble");
    }
    if (contrast_ratio(dark_text, dark_fill) < 4.5) {
        throw std::runtime_error(
            "dark receipt text contrast must remain at least 4.5:1");
    }

    const auto light = render_receipt_chips(false);
    const auto light_pills = exact_color_pill_count(light, light_fill);
    if (light_pills != 4) {
        throw std::runtime_error(
            "light receipt chips must retain their shared #D7EBE3 fill");
    }
    if (!preview_path.isEmpty() && !dark.save(preview_path)) {
        throw std::runtime_error("selected receipt preview must save");
    }
}

void export_production_receipt_preview(const QString &preview_path) {
    if (preview_path.isEmpty()) {
        throw std::runtime_error("production receipt preview path is required");
    }
    const auto dark_fill = QColor(QStringLiteral("#45665A"));
    const auto image = render_receipt_chips(true, false);
    if (exact_color_pill_count(image, dark_fill) != 3) {
        throw std::runtime_error(
            "production receipt preview must contain exactly three chips");
    }
    if (!image.save(preview_path)) {
        throw std::runtime_error("production receipt preview must save");
    }
}

// Mirrors text_selection_wash_color()'s two theme targets. Kept as an
// independent constant here (not shared with the production TU) so this test
// distinguishes the actual chosen coverage from the brief's design range,
// rather than trivially re-asserting whatever the production file defines.
constexpr auto kLightSelectionAlpha = 97;
constexpr auto kDarkSelectionAlpha = 136;

// Mirrors semantic_selection_accent()'s normalization exactly (same
// constants), as an independent, directly callable pure function: the
// production version lives in conversation_surface.cpp's anonymous
// namespace and is not reachable from this translation unit, so equivalence
// is by construction (same formula, same constants) rather than by linkage.
// Taking the "native" color as a parameter (instead of always querying
// QApplication::palette() internally) is what makes the fallback branch
// unit-testable without depending on this machine's real OS accent color.
constexpr auto kSelectionAccentFallbackHue = 211;
constexpr auto kSelectionAccentHueBandLow = 195;
constexpr auto kSelectionAccentHueBandHigh = 235;
constexpr auto kSelectionAccentSaturation = 255;
constexpr auto kSelectionAccentValue = 255;

QColor semantic_selection_accent_from(const QColor &native) {
    auto hue = native.hue();
    if (hue < kSelectionAccentHueBandLow || hue > kSelectionAccentHueBandHigh) {
        hue = kSelectionAccentFallbackHue;
    }
    auto accent = QColor();
    accent.setHsv(hue, kSelectionAccentSaturation, kSelectionAccentValue);
    return accent;
}

QColor semantic_selection_accent() {
    return semantic_selection_accent_from(QApplication::palette()
        .color(QPalette::Active, QPalette::Highlight));
}

void apply_selection_test_palette(bool dark) {
    style::main_palette::reset();
    if (!dark) {
        return;
    }
    // The exact production night-palette values for the tokens this
    // selection contract reads (native_shell.cpp's apply_telegram_night_
    // palette()), not an ad hoc fixture subset: windowFg matters here
    // because it is what HighlightedText must track after refresh_chrome().
    set_test_palette_color("windowBg", "#17212B");
    set_test_palette_color("windowFg", "#F5F5F5");
    set_test_palette_color("windowBgActive", "#2B5278");
    set_test_palette_color("historyTextInFg", "#F5F5F5");
    set_test_palette_color("historyTextOutFg", "#E4ECF2");
    set_test_palette_color("msgServiceFg", "#708499");
}

QColor alpha_blended(const QColor &fg, const QColor &bg, int alpha) {
    const auto factor = alpha / 255.0;
    const auto blend = [&](int f, int b) {
        return int(std::lround(b + (f - b) * factor));
    };
    return QColor(
        blend(fg.red(), bg.red()),
        blend(fg.green(), bg.green()),
        blend(fg.blue(), bg.blue()));
}

bool colors_close(const QColor &a, const QColor &b, int tolerance) {
    return std::abs(a.red() - b.red()) <= tolerance
        && std::abs(a.green() - b.green()) <= tolerance
        && std::abs(a.blue() - b.blue()) <= tolerance;
}

// True when some pixel in `rect` is within `tolerance` of `color`. A region
// scan (not a single sampled point) because the rect covers real glyph ink
// as well as the painted wash, and which exact point lands on ink vs. wash
// background is font-metric-dependent.
bool region_contains_color_near(
        const QImage &image,
        const QRectF &rect,
        const QColor &color,
        int tolerance) {
    const auto dpr = image.devicePixelRatio();
    const auto left = std::max(0, int(std::floor(rect.left() * dpr)));
    const auto top = std::max(0, int(std::floor(rect.top() * dpr)));
    const auto right = std::min(
        image.width() - 1, int(std::ceil(rect.right() * dpr)));
    const auto bottom = std::min(
        image.height() - 1, int(std::ceil(rect.bottom() * dpr)));
    for (auto y = top; y <= bottom; ++y) {
        for (auto x = left; x <= right; ++x) {
            if (colors_close(image.pixelColor(x, y), color, tolerance)) {
                return true;
            }
        }
    }
    return false;
}

// paint_glyph_tight_selection widens the glyph rect by 1px each side and
// rounds its corners (radius 3) before filling it, so the outermost few
// pixels of the geometric glyph rect are an antialiased blend toward the
// surface background, not a solid fill. "Absence of color X" checks must
// look only at the solid interior, or a blend pixel can transiently land
// within tolerance of an unrelated target color by coincidence.
QRectF interior_of(const QRectF &rect, double margin = 3.0) {
    return rect.adjusted(margin, margin, -margin, -margin);
}

QTextCursor select_substring(ConversationSurface &surface, const QString &needle) {
    auto cursor = surface.document()->find(needle);
    if (!cursor.hasSelection()) {
        throw std::runtime_error(
            "selection fixture text '" + needle.toStdString()
            + "' was not found in the rendered document");
    }
    surface.setTextCursor(cursor);
    return cursor;
}

// Mirrors paint_glyph_tight_selection's own geometry (natural-glyph
// cursorToX span, translated into the owning frame — or the root frame for
// plain-state text — then into the viewport), so the test can predict
// exactly where the wash should, and should not, land.
QRectF selected_glyph_viewport_rect(
        ConversationSurface &surface, const QTextCursor &selection) {
    auto *document = surface.document();
    const auto sel_start = selection.selectionStart();
    const auto sel_end = selection.selectionEnd();
    const auto block = document->findBlock(sel_start);
    const auto *layout = block.layout();
    const auto local_start = sel_start - block.position();
    const auto local_end = sel_end - block.position();
    const auto line = layout->lineForTextPosition(local_start);
    const auto x1 = line.cursorToX(local_start);
    const auto x2 = line.cursorToX(local_end);
    auto *frame = document->rootFrame();
    for (auto *candidate : document->rootFrame()->childFrames()) {
        if (sel_start >= candidate->firstPosition()
                && sel_start <= candidate->lastPosition()) {
            frame = candidate;
            break;
        }
    }
    const auto frame_origin =
        document->documentLayout()->frameBoundingRect(frame).topLeft();
    auto rect = QRectF(
        std::min(x1, x2), line.y(), std::abs(x2 - x1), line.height())
        .translated(layout->position())
        .translated(frame_origin);
    const auto h_offset = double(surface.horizontalScrollBar()->value());
    const auto v_offset = double(surface.verticalScrollBar()->value());
    rect.translate(-h_offset, -v_offset);
    return rect;
}

void flush_deferred_events() {
    for (auto i = 0; i != 4; ++i) {
        QCoreApplication::processEvents();
    }
}

// The design targets are measurement ranges, not license to hard-code an
// unmeasured hex. This does NOT gate on this machine's raw OS Highlight
// color being blue — a human's accent-color preference must never be able
// to fail the build, and the product must render blue/azure regardless of
// it. Instead: (1) the coverage alphas land in the brief's windows; (2) the
// normalization itself is unit-tested against synthetic native inputs, both
// a blue one (hue passes through) and clearly-non-blue ones (falls back to
// the canonical azure hue) — deterministic on every machine; (3) the FINAL
// composited fill actually produced for each real conversation surface, in
// both themes, is measured for hue/saturation/contrast, using whatever this
// machine's live palette resolves to as input, but asserting only on the
// normalized output.
void verify_selection_accent_targets() {
    if (kLightSelectionAlpha < 89 || kLightSelectionAlpha > 102) {
        throw std::runtime_error(
            "light-mode selection coverage must land in the ~35-40% design "
            "target (alpha 89-102 of 255), got "
            + std::to_string(kLightSelectionAlpha));
    }
    if (kDarkSelectionAlpha < 128 || kDarkSelectionAlpha > 140) {
        throw std::runtime_error(
            "dark-mode selection coverage must land in the ~50-55% design "
            "target (alpha 128-140 of 255), got "
            + std::to_string(kDarkSelectionAlpha));
    }

    // Synthetic inputs: the normalization contract holds regardless of what
    // this machine's actual OS accent color is.
    const auto passthrough =
        semantic_selection_accent_from(QColor::fromHsv(214, 150, 120));
    if (passthrough.hue() != 214
            || passthrough.saturation() != kSelectionAccentSaturation
            || passthrough.value() != kSelectionAccentValue) {
        throw std::runtime_error(
            "a native Highlight already in the blue/azure band must keep its "
            "hue but always normalize saturation/value to the vivid "
            "baseline, got hue " + std::to_string(passthrough.hue())
            + " sat " + std::to_string(passthrough.saturation())
            + " val " + std::to_string(passthrough.value()));
    }
    for (const auto off_family : {Qt::red, Qt::green, Qt::yellow}) {
        const auto fallback = semantic_selection_accent_from(QColor(off_family));
        if (fallback.hue() != kSelectionAccentFallbackHue) {
            throw std::runtime_error(
                "a native Highlight outside the blue/azure band must fall "
                "back to the canonical azure hue "
                + std::to_string(kSelectionAccentFallbackHue) + ", got "
                + std::to_string(fallback.hue()));
        }
    }

    // The live-machine-dependent part: only used as *input* to the
    // normalization below, never asserted on directly.
    const auto native =
        QApplication::palette().color(QPalette::Active, QPalette::Highlight);
    const auto accent = semantic_selection_accent();
    std::cout << "selection accent: native QPalette::Highlight="
              << native.name().toStdString() << " (hue " << native.hue()
              << ") -> normalized accent=" << accent.name().toStdString()
              << " (hue " << accent.hue() << ")\n";

    const struct { const char *name; QColor light; QColor dark; } surfaces[] = {
        {"assistant/system canvas", QColor(QStringLiteral("#FFFFFF")),
            QColor(QStringLiteral("#17212B"))},
        {"human bubble", QColor(QStringLiteral("#EEF7F3")),
            QColor(QStringLiteral("#2A4038"))},
        {"fenced code surface", QColor(QStringLiteral("#E4E7EB")),
            QColor(QStringLiteral("#242F3D"))},
    };
    const auto light_ink = QColor(QStringLiteral("#000000"));
    const auto dark_ink = QColor(QStringLiteral("#F5F5F5"));
    for (const auto &surface : surfaces) {
        const auto light_fill =
            alpha_blended(accent, surface.light, kLightSelectionAlpha);
        const auto dark_fill =
            alpha_blended(accent, surface.dark, kDarkSelectionAlpha);
        std::cout << "  " << surface.name
                  << ": light fill=" << light_fill.name().toStdString()
                  << " hue=" << light_fill.hue()
                  << " sat=" << light_fill.saturation()
                  << " contrast=" << contrast_ratio(light_ink, light_fill)
                  << "  dark fill=" << dark_fill.name().toStdString()
                  << " hue=" << dark_fill.hue()
                  << " sat=" << dark_fill.saturation()
                  << " contrast=" << contrast_ratio(dark_ink, dark_fill) << "\n";
        for (const auto &fill : {light_fill, dark_fill}) {
            const auto fill_hue = fill.hue();
            if (fill_hue < kSelectionAccentHueBandLow
                    || fill_hue > kSelectionAccentHueBandHigh) {
                throw std::runtime_error(
                    std::string(surface.name)
                    + " composited selection fill must read as blue/azure "
                      "(hue in [195,235]), got hue "
                    + std::to_string(fill_hue) + " (#"
                    + fill.name().toStdString() + ")");
            }
            // A floor well under the ~31-93% actually measured on this
            // machine's real palette (see the printed diagnostics above),
            // but well above "grey": the coverage targets alone (35-40%/
            // 50-55% of a fully-saturated source) guarantee at least this
            // much post-blend saturation by construction.
            if (fill.saturationF() < 0.20) {
                throw std::runtime_error(
                    std::string(surface.name)
                    + " composited selection fill must be clearly "
                      "saturated blue/azure, not blue-grey, got "
                    + std::to_string(fill.saturationF() * 100.0)
                    + "% saturation (#" + fill.name().toStdString() + ")");
            }
        }
        if (contrast_ratio(light_ink, light_fill) < 4.5
                || contrast_ratio(dark_ink, dark_fill) < 4.5) {
            throw std::runtime_error(
                std::string(surface.name)
                + " composited selection fill must keep selected-text "
                  "contrast >= 4.5:1 in both themes");
        }
    }
}

// One pass: select `needle` inside `surface`, verify the composited
// selection wash is the new native blue/azure accent at the theme's target
// coverage (not the superseded windowBgActive teal wash), stays glyph-tight
// (a later word on the same row, `past_needle`, must show none of the
// wash), and keeps selected-text ink readable against the new fill.
void verify_selection_on_surface(
        ConversationSurface &surface,
        bool dark,
        const QColor &background,
        const QString &needle,
        const QString &past_needle,
        const char *surface_name) {
    const auto cursor = select_substring(surface, needle);
    QCoreApplication::processEvents();
    const auto image = surface.viewport()->grab().toImage();
    const auto glyph_rect = selected_glyph_viewport_rect(surface, cursor);

    const auto accent = semantic_selection_accent();
    const auto alpha = dark ? kDarkSelectionAlpha : kLightSelectionAlpha;
    const auto expected = alpha_blended(accent, background, alpha);
    if (!region_contains_color_near(image, glyph_rect, expected, 3)) {
        throw std::runtime_error(
            std::string(surface_name) + " selection fill in "
            + (dark ? "dark" : "light")
            + " must composite the normalized blue/azure accent at the "
              "target coverage over its surface, expected a pixel near "
            + expected.name().toStdString());
    }

    const auto old_base = dark
        ? QColor(QStringLiteral("#2B5278"))
        : QColor(QStringLiteral("#40A7E3"));
    const auto old_alpha = dark ? 72 : 48;
    const auto old_expected = alpha_blended(old_base, background, old_alpha);
    if (region_contains_color_near(
            image, interior_of(glyph_rect), old_expected, 3)) {
        throw std::runtime_error(
            std::string(surface_name) + " selection in "
            + (dark ? "dark" : "light")
            + " must not still composite as the superseded weak "
              "windowBgActive teal wash");
    }

    auto past_cursor = surface.document()->find(past_needle);
    if (!past_cursor.hasSelection()) {
        throw std::runtime_error(
            std::string("glyph-tight probe text was not found for ")
            + surface_name);
    }
    const auto past_rect = selected_glyph_viewport_rect(surface, past_cursor);
    if (region_contains_color_near(
            image, interior_of(past_rect), expected, 3)) {
        throw std::runtime_error(
            std::string(surface_name) + " selection wash in "
            + (dark ? "dark" : "light")
            + " must stay glyph-tight, but it also reached a later word on "
              "the same row past the selected glyphs");
    }

    // The FINAL composited fill actually used for THIS surface/theme (not
    // the raw OS Highlight, which region_contains_color_near above already
    // confirmed a real rendered pixel matches) must itself read as
    // blue/azure and clearly saturated, and never depends on this machine's
    // OS accent-color preference: expected is built from the normalized
    // semantic_selection_accent(), so this is deterministic on any machine.
    if (expected.hue() < kSelectionAccentHueBandLow
            || expected.hue() > kSelectionAccentHueBandHigh) {
        throw std::runtime_error(
            std::string(surface_name) + " composited selection fill in "
            + (dark ? "dark" : "light")
            + " must read as blue/azure (hue in [195,235]), got hue "
            + std::to_string(expected.hue()));
    }
    if (expected.saturationF() < 0.20) {
        throw std::runtime_error(
            std::string(surface_name) + " composited selection fill in "
            + (dark ? "dark" : "light")
            + " must be clearly saturated, not blue-grey, got "
            + std::to_string(expected.saturationF() * 100.0) + "% saturation");
    }

    // Read the ink Qt will actually paint (the widget's own HighlightedText
    // role), not st::windowFg directly: that is the value refresh_chrome()
    // is responsible for keeping current, and the two can diverge if it goes
    // stale after a live theme switch.
    const auto ink = surface.palette().color(QPalette::HighlightedText);
    const auto contrast = contrast_ratio(ink, expected);
    if (contrast < 4.5) {
        throw std::runtime_error(
            std::string(surface_name) + " selected-text ink in "
            + (dark ? "dark" : "light")
            + " must stay readable (>=4.5:1) against the new fill, got "
            + std::to_string(contrast));
    }
}

void verify_selection_wash_on_conversation_surfaces() {
    for (const auto dark : {true, false}) {
        apply_selection_test_palette(dark);
        ConversationSurface surface;
        surface.resize(1000, 700);
        surface.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        surface.show();

        const std::vector<DirectConversationMessage> messages = {
            {.id = "in-1", .outgoing = false,
                .timestamp = "2026-08-07T18:48:52",
                .text = "The assistant explains the plan clearly today."},
            {.id = "out-1", .outgoing = true,
                .timestamp = "2026-08-07T19:00:00Z",
                .text = "Understood, proceeding now with the change."},
            {.id = "code-1", .outgoing = false,
                .timestamp = "2026-08-07T19:05:00Z",
                .text = "Here is the function:\n```cpp\n"
                        "int compute(int x) { return x + 1; }\n```"},
        };
        surface.set_conversation(QStringLiteral("Telegram Bot"), messages);
        surface.document()->documentLayout()->documentSize();
        QCoreApplication::processEvents();

        const auto canvas = dark
            ? QColor(QStringLiteral("#17212B"))
            : QColor(QStringLiteral("#FFFFFF"));
        const auto human_bubble = dark
            ? QColor(QStringLiteral("#2A4038"))
            : QColor(QStringLiteral("#EEF7F3"));
        const auto code_surface = dark
            ? QColor(QStringLiteral("#242F3D"))
            : QColor(QStringLiteral("#E4E7EB"));

        verify_selection_on_surface(surface, dark, canvas,
            QStringLiteral("plan"), QStringLiteral("clearly"),
            "assistant bubble");
        verify_selection_on_surface(surface, dark, human_bubble,
            QStringLiteral("proceeding"), QStringLiteral("with"),
            "human bubble");
        verify_selection_on_surface(surface, dark, code_surface,
            QStringLiteral("compute"), QStringLiteral("return"),
            "fenced code surface");
    }
}

// "system/plain text" is the plain-state path (set_plain_state): no owning
// message QTextFrame, so paint_glyph_tight_selection's frame lookup falls
// back to the root frame — a genuinely different geometry branch than the
// three per-message surfaces above — and it is painted in secondary_format()
// service ink, the dimmest ink this surface uses.
void verify_selection_over_plain_state_text() {
    for (const auto dark : {true, false}) {
        apply_selection_test_palette(dark);
        ConversationSurface surface;
        surface.resize(900, 300);
        surface.show();
        surface.set_plain_state(QStringLiteral(
            "No agent is selected, choose one to continue exploring."));
        QCoreApplication::processEvents();

        const auto canvas = dark
            ? QColor(QStringLiteral("#17212B"))
            : QColor(QStringLiteral("#FFFFFF"));
        verify_selection_on_surface(surface, dark, canvas,
            QStringLiteral("selected"), QStringLiteral("choose"),
            "system/plain-state text");
    }
}

// A live theme refresh (refresh_chrome(), the same entry point native_shell
// calls on a system palette change) must not clear an in-progress drag
// selection, must not require the caller to resupply the conversation, and
// must repaint the wash/ink at the new theme's target the moment it runs.
void verify_selection_survives_live_theme_switch() {
    apply_selection_test_palette(false);
    ConversationSurface surface;
    surface.resize(900, 400);
    surface.show();
    const std::vector<DirectConversationMessage> messages = {
        {.id = "in-1", .outgoing = false,
            .timestamp = "2026-08-07T18:48:52",
            .text = "The assistant explains the plan clearly today."},
    };
    surface.set_conversation(QStringLiteral("Telegram Bot"), messages);
    QCoreApplication::processEvents();

    const auto initial_cursor =
        select_substring(surface, QStringLiteral("plan clearly"));
    const auto expected_text = initial_cursor.selectedText();
    QCoreApplication::processEvents();

    const auto verify_after_switch = [&](
            bool dark, const QColor &canvas, int alpha) {
        if (!surface.textCursor().hasSelection()
                || surface.textCursor().selectedText() != expected_text) {
            throw std::runtime_error(
                std::string("a live theme refresh to ")
                + (dark ? "dark" : "light")
                + " must not lose the active drag-select, but the "
                  "selection changed or was cleared (selectedText is now '"
                + surface.textCursor().selectedText().toStdString() + "')");
        }

        // Positive proof the deferred rebuild actually ran (not just that
        // nothing cleared the selection): body_format() bakes
        // body_reading_color() into the QTextCharFormat at insertion time,
        // so this only reads as the new theme's ink if rebuild_document()
        // reinserted the message after the switch.
        const auto body_runs = format_runs(
            *surface.document(), QStringLiteral("The assistant"));
        if (body_runs.size() != 1) {
            throw std::runtime_error(
                "expected exactly one body run for 'The assistant', found "
                + std::to_string(body_runs.size()));
        }
        const auto body_ink = body_runs.front().foreground().color();
        const auto expected_body_ink = dark
            ? st::historyTextInFg->c
            : QColor(QStringLiteral("#26282B"));
        if (body_ink != expected_body_ink) {
            throw std::runtime_error(
                std::string("after a live refresh to ") + (dark ? "dark" : "light")
                + " the message body must be reinserted with that theme's "
                  "ink (the document must actually rebuild), expected "
                + expected_body_ink.name().toStdString() + " got "
                + body_ink.name().toStdString());
        }

        // QPalette::setColor copies by value: this is the direct proof that
        // refresh_chrome() re-reads HighlightedText instead of leaving the
        // constructor-time (now possibly wrong-theme) ink in place.
        const auto ink = surface.palette().color(QPalette::HighlightedText);
        if (ink != st::windowFg->c) {
            throw std::runtime_error(
                std::string("after a live refresh to ") + (dark ? "dark" : "light")
                + " the widget's HighlightedText must track the current "
                  "st::windowFg, but it is stale (" + ink.name().toStdString()
                + " vs current " + st::windowFg->c.name().toStdString() + ")");
        }

        const auto image = surface.viewport()->grab().toImage();
        const auto glyph_rect = selected_glyph_viewport_rect(
            surface, surface.textCursor());
        const auto expected =
            alpha_blended(semantic_selection_accent(), canvas, alpha);
        if (!region_contains_color_near(image, glyph_rect, expected, 3)) {
            throw std::runtime_error(
                std::string("after a live refresh to ")
                + (dark ? "dark" : "light")
                + " the selection wash must repaint at that theme's target "
                  "coverage, but the expected composite was not found");
        }
        const auto contrast = contrast_ratio(ink, expected);
        if (contrast < 4.5) {
            throw std::runtime_error(
                std::string("after a live refresh to ")
                + (dark ? "dark" : "light")
                + " the selected-text ink must stay readable (>=4.5:1), got "
                + std::to_string(contrast));
        }
    };

    apply_selection_test_palette(true);
    surface.refresh_chrome();
    flush_deferred_events();
    verify_after_switch(
        true, QColor(QStringLiteral("#17212B")), kDarkSelectionAlpha);

    apply_selection_test_palette(false);
    surface.refresh_chrome();
    flush_deferred_events();
    verify_after_switch(
        false, QColor(QStringLiteral("#FFFFFF")), kLightSelectionAlpha);
}

// rebuild_document() only remaps the captured numeric selection offsets when
// the rebuild reproduces byte-identical plain text. A real content change
// (a new message arriving mid-selection — set_conversation()'s plain
// four-arg overload always calls rebuild_document(), not an incremental
// append) must not land the old offsets on unrelated new text.
void verify_selection_not_remapped_when_content_changes() {
    apply_selection_test_palette(false);
    ConversationSurface surface;
    surface.resize(900, 500);
    surface.show();

    std::vector<DirectConversationMessage> messages = {
        {.id = "in-1", .outgoing = false,
            .timestamp = "2026-08-07T18:48:52",
            .text = "The assistant explains the plan clearly today."},
    };
    surface.set_conversation(QStringLiteral("Telegram Bot"), messages);
    QCoreApplication::processEvents();

    const auto cursor = select_substring(surface, QStringLiteral("plan clearly"));
    if (!cursor.hasSelection()) {
        throw std::runtime_error(
            "selection fixture must be active before the content change");
    }
    QCoreApplication::processEvents();

    // A new message arrives mid-selection: a real product event (a
    // streaming reply), not a theme refresh — the document's plain text
    // genuinely changes.
    messages.push_back({.id = "in-2", .outgoing = false,
        .timestamp = "2026-08-07T18:49:10",
        .text = "A brand new unrelated follow-up line just arrived."});
    surface.set_conversation(QStringLiteral("Telegram Bot"), messages);
    QCoreApplication::processEvents();

    if (surface.textCursor().hasSelection()) {
        throw std::runtime_error(
            "when the rebuilt document's plain text actually changed, the "
            "old numeric selection offsets must not be remapped onto "
            "unrelated new text, but a selection is still active covering '"
            + surface.textCursor().selectedText().toStdString() + "'");
    }

    // Same-content rebuilds (the live-switch contract above) must still
    // work: a resize-triggered rebuild reproduces identical plain text, so
    // a fresh selection here must survive it exactly like a theme refresh.
    const auto resize_cursor =
        select_substring(surface, QStringLiteral("brand new"));
    const auto expected_text = resize_cursor.selectedText();
    QCoreApplication::processEvents();
    surface.resize(700, 500);
    QCoreApplication::processEvents();
    if (!surface.textCursor().hasSelection()
            || surface.textCursor().selectedText() != expected_text) {
        throw std::runtime_error(
            "a resize-triggered rebuild reproduces identical plain text and "
            "must still preserve an active selection, but it was lost or "
            "changed to '" + surface.textCursor().selectedText().toStdString()
            + "'");
    }
}

} // namespace

int run_typography_test(int argc, char **argv) {
    try {
        QApplication application(argc, argv);
        style::internal::init_palette(style::kScaleDefault);
        // Match the real native shell's font/widget style initialization so the
        // focused contract uses the configured family and scale, not only its
        // palette.
        style::internal::init_style_widgets(style::kScaleDefault);
        if (argc > 1
                && QString::fromLocal8Bit(argv[1])
                    == QStringLiteral("--human-bubble-only")) {
            verify_human_bubble_contract();
            std::cout << "conversation surface Human bubble: OK\n";
            return 0;
        }
        if (argc > 1
                && QString::fromLocal8Bit(argv[1])
                    == QStringLiteral("--body-reading-only")) {
            verify_readable_semantic_body();
            std::cout << "conversation surface readable semantic body: OK\n";
            return 0;
        }
        if (argc > 1
                && QString::fromLocal8Bit(argv[1])
                    == QStringLiteral("--message-type-only")) {
            ConversationSurface surface;
            surface.resize(640, 480);
            verify_typography(surface, QStringLiteral("Telegram Bot"));
            std::cout << "conversation surface message type: OK\n";
            return 0;
        }
        if (argc > 1
                && QString::fromLocal8Bit(argv[1])
                    == QStringLiteral("--message-grouping-only")) {
            verify_same_agent_grouping_only();
            std::cout << "conversation surface message grouping: OK\n";
            return 0;
        }
        if (argc > 1
                && QString::fromLocal8Bit(argv[1])
                    == QStringLiteral("--history-window-only")) {
            verify_history_window_only();
            std::cout << "conversation surface lazy history window: OK\n";
            return 0;
        }
        if (argc > 1
                && QString::fromLocal8Bit(argv[1])
                    == QStringLiteral("--time-separators-only")) {
            verify_time_separators_only();
            std::cout << "conversation surface time separators: OK\n";
            return 0;
        }
        if (argc > 1
                && QString::fromLocal8Bit(argv[1])
                    == QStringLiteral("--light-canvas-only")) {
            verify_directional_bubble_policy();
            std::cout << "conversation surface light canvas: OK\n";
            return 0;
        }
        if (argc > 1
                && QString::fromLocal8Bit(argv[1])
                    == QStringLiteral("--hidden-resize-only")) {
            verify_reflow_after_hidden_resize();
            std::cout << "conversation surface hidden resize reflow: OK\n";
            return 0;
        }
        if (argc > 1
                && QString::fromLocal8Bit(argv[1])
                    == QStringLiteral("--append-presentation-only")) {
            verify_incremental_append_matches_clean_presentation();
            std::cout << "conversation surface append presentation: OK\n";
            return 0;
        }
        if (argc > 1
                && QString::fromLocal8Bit(argv[1])
                    == QStringLiteral("--receipt-chip-only")) {
            verify_unified_receipt_chip_fill(
                argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString());
            std::cout << "conversation surface receipt chip fill: OK\n";
            return 0;
        }
        if (argc > 1
                && QString::fromLocal8Bit(argv[1])
                    == QStringLiteral("--receipt-production-preview-only")) {
            export_production_receipt_preview(
                argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString());
            std::cout << "conversation surface production receipts preview: OK\n";
            return 0;
        }
        if (argc > 1
                && QString::fromLocal8Bit(argv[1])
                    == QStringLiteral("--selection-highlight-only")) {
            verify_selection_accent_targets();
            verify_selection_wash_on_conversation_surfaces();
            verify_selection_over_plain_state_text();
            verify_selection_survives_live_theme_switch();
            verify_selection_not_remapped_when_content_changes();
            std::cout << "conversation surface selection highlight: OK\n";
            return 0;
        }
        ConversationSurface surface;
        surface.resize(640, 480);
        verify_typography(surface, QStringLiteral("Telegram Bot"));
        verify_reflow_after_hidden_resize();
        verify_responsive_width();
        verify_content_geometry();
        verify_turn_rhythm();
        verify_plain_state_resize_journey();
        verify_empty_state_contract();
        verify_markdown_safe_formatting();
        verify_per_message_containers();
        verify_directional_bubble_policy();
        verify_outgoing_body_first_and_external_time();
        verify_new_day_append_scrolls_to_bottom();
        verify_message_attachment_presentation();
        verify_revision_noop_and_incremental_append();
        verify_incremental_append_matches_clean_presentation();
        verify_unified_receipt_chip_fill();
        verify_selection_accent_targets();
        verify_selection_wash_on_conversation_surfaces();
        verify_selection_over_plain_state_text();
        verify_selection_survives_live_theme_switch();
        verify_selection_not_remapped_when_content_changes();
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
