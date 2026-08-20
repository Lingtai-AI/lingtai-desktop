#include "ui/conversation_surface.h"

#include "base/basic_types.h"
#include "styles/palette.h"

#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QFontMetricsF>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtGui/QPalette>
#include <QtGui/QPixmap>
#include <QtGui/QResizeEvent>
#include <QtGui/QShowEvent>
#include <QtGui/QTextBlock>
#include <QtGui/QTextBlockFormat>
#include <QtGui/QTextCharFormat>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtGui/QTextFormat>
#include <QtGui/QTextFrame>
#include <QtGui/QTextImageFormat>
#include <QtGui/QTextLayout>
#include <QtWidgets/QApplication>
#include <QtWidgets/QScrollBar>

#include <algorithm>
#include <cmath>
#include <utility>

namespace lingtai::desktop {
namespace {

constexpr auto kDocumentMargin = 8;
constexpr auto kMessageEdgeMargin = 12;
constexpr auto kMessageTopMargin = 4;
// One readable rhythm inside a same-Agent group and a larger break between
// groups. The current frame's bottom margin owns the gap to its next sibling.
constexpr auto kWithinGroupBottomMargin = 0;
constexpr auto kBetweenGroupBottomMargin = 28;
constexpr qint64 kSameAgentGroupMaxSeconds = 5 * 60;
constexpr auto kMessageRatio = 0.72;
constexpr auto kHumanMessageRatio = 0.60;
constexpr auto kMinMessageWidth = 160;
constexpr auto kHumanMinMessageWidth = 64;
constexpr auto kMessageAbsoluteCap = 560;
constexpr auto kHumanMessageAbsoluteCap = 540;
constexpr auto kReadingColumnMax = 1600;
constexpr auto kNarrowViewportWidth = 480;
constexpr auto kBubbleHPadding = 11;
constexpr auto kHumanBubbleHPadding = 15;
constexpr auto kHumanBubbleVPadding = 11;
constexpr auto kHumanBubbleRadius = 12;
constexpr auto kReactionChipHPadding = 7;
constexpr auto kReactionChipVPadding = 3;
constexpr auto kReactionChipGap = 4;
constexpr auto kReactionRowTopGap = 6;
constexpr auto kReactionRowBottomInset = 6;
constexpr auto kReactionRowSideInset = 8;
// The Human timestamp sits below the bubble with a small quiet gap; the frame
// reserves enough bottom space for the 12px metadata line and this padding.
constexpr auto kTimestampGap = 4;
constexpr auto kHumanMessageBottomMargin = 36;
constexpr auto kMessageAvatarDiameter = 34;
constexpr auto kMessageAvatarGap = 8;
constexpr auto kMessageBlockProperty = QTextFormat::UserProperty + 1;
// The outgoing message timestamp rides on the sibling frame so paintEvent can
// place it outside the body-only bubble without a visible header block.
constexpr auto kMessageTimestampProperty = QTextFormat::UserProperty + 2;
// Incoming frames paint an avatar only when they start a visible same-Agent
// group; continuation frames retain the shared text lane but leave it empty.
constexpr auto kMessageAvatarProperty = QTextFormat::UserProperty + 3;
constexpr auto kEmptyAvatarDiameter = 32;
constexpr auto kEmptyAvatarGap = 10;
constexpr auto kEmptyTitleGap = 6;
constexpr auto kEmptyStateHeadroom = 28;
constexpr auto kEmptyStateTitleSize = 15;
constexpr auto kSelectAgentIllustrationSize = 72;
constexpr auto kSelectAgentIllustrationGap = 14;
constexpr auto kSelectAgentTitleSize = 17;
// The leading lazy-history banner's vertical breathing room around the single
// centered muted line. It is deliberately small so the banner reads as a quiet
// inline note above the first revealed message rather than a tall separator.
constexpr auto kBannerTopMargin = 6;
constexpr auto kBannerBottomMargin = 8;
// The day separator's vertical breathing room around the centered label and its
// horizontal rules, kept generous so the divider reads between message groups.
constexpr auto kDayTopMargin = 18;
constexpr auto kDayBottomMargin = 14;
// The day divider label rides on the first message frame of each calendar day;
// paintEvent draws the centered rules in the gap above that frame.
constexpr auto kDaySeparatorProperty = QTextFormat::UserProperty + 6;
constexpr auto kParagraphBottomMargin = 13;
constexpr auto kListBottomMargin = 3;
constexpr auto kCodeBlockProperty = QTextFormat::UserProperty + 4;
// Direction is frame metadata, not body alignment: Human body text stays left
// aligned while the frame's margins and painter keep the bubble on the right.
constexpr auto kMessageOutgoingProperty = QTextFormat::UserProperty + 5;
constexpr auto kMessageIdProperty = QTextFormat::UserProperty + 8;
constexpr auto kMessageReactionsProperty = QTextFormat::UserProperty + 9;

// The symmetric gutter of the centered reading column for a viewport: fixed
// 12px edge gutters until the column max, then a shared share of the excess.
int reading_column_margins(int viewport_width) {
    return qMax(12, (viewport_width - kReadingColumnMax) / 2 + 12);
}

// The shared lane maximum for a given viewport: the widest any one message may
// be. At the explicit narrow breakpoint the lane is near-full (the viewport
// minus the two fixed edge gutters) instead of the ordinary ratio; otherwise
// the lane lives inside the centered reading column at the ordinary 72% ratio,
// with the existing 160px lower bound and the absolute readable cap, never
// wider than the column.
int message_block_width(int viewport_width) {
    if (viewport_width < kNarrowViewportWidth) {
        return qMax(0, viewport_width - 2 * kMessageEdgeMargin);
    }
    const auto column = qMin(viewport_width, kReadingColumnMax);
    return qBound(
        kMinMessageWidth,
        int(column * kMessageRatio),
        kMessageAbsoluteCap);
}

// Human bubbles use the same wider centered rail but retain the accepted
// readable prose cap; widening the rail moves its right anchor outward instead
// of stretching long Human text. On narrow panes the common near-full lane
// remains the usable fallback.
int human_message_block_width(int viewport_width) {
    if (viewport_width < kNarrowViewportWidth) {
        return qMax(0, viewport_width - 2 * kMessageEdgeMargin);
    }
    const auto column = qMin(viewport_width, kReadingColumnMax);
    return qMin(
        kHumanMessageAbsoluteCap,
        qMax(kHumanMinMessageWidth, int(column * kHumanMessageRatio)));
}

QTextBlockFormat message_block_format(
        bool outgoing,
        int viewport_width,
        int width) {
    auto format = QTextBlockFormat();
    // Reading direction and lane position are independent. Both message bodies
    // read left-to-right; asymmetric margins below anchor the Human lane right.
    format.setAlignment(Qt::AlignLeft);
    // Derive one outer gutter (the centered reading column's offset plus the
    // fixed edge gutter) and one inner remainder, then cross-assign them so
    // incoming stays left-anchored and outgoing right-anchored inside the same
    // column rather than centering each message individually. Each message
    // keeps the shared outer anchor while its inner edge follows its own
    // content-driven width.
    const auto column = qMin(viewport_width, kReadingColumnMax);
    const auto outer = (viewport_width - column) / 2 + kMessageEdgeMargin;
    const auto avatar_lane = kMessageAvatarDiameter + kMessageAvatarGap;
    const auto text_outer = outgoing ? outer : outer + avatar_lane;
    const auto inner = qMax(outer, viewport_width - width - text_outer);
    format.setLeftMargin(outgoing ? inner : text_outer);
    format.setRightMargin(outgoing ? outer : inner);
    format.setTopMargin(kMessageTopMargin);
    format.setBottomMargin(0);
    format.setLineHeight(160, QTextBlockFormat::ProportionalHeight);
    format.setProperty(kMessageBlockProperty, true);
    return format;
}

QTextCharFormat sender_format(bool outgoing) {
    auto format = QTextCharFormat();
    format.setForeground(outgoing ? st::historyTextOutFg
                                  : st::historyTextInFg);
    auto font = format.font();
    font.setPixelSize(15);
    font.setWeight(QFont::DemiBold);
    format.setFont(font);
    return format;
}

QColor body_reading_color(bool outgoing) {
    if (st::windowBg->c.lightness() >= 128) {
        return QColor(QStringLiteral("#26282B"));
    }
    return outgoing ? st::historyTextOutFg->c : st::historyTextInFg->c;
}

QColor secondary_reading_color() {
    return st::windowBg->c.lightness() >= 128
        ? QColor(QStringLiteral("#8A8F98"))
        : st::msgServiceFg->c;
}

QColor human_bubble_color() {
    return st::windowBg->c.lightness() >= 128
        ? QColor(QStringLiteral("#EEF7F3"))
        : st::msgOutBg->c;
}

QStringList reaction_chip_labels(const MessageReactions &bag) {
    auto labels = QStringList();
    for (const auto &entry : bag.list) {
        auto label = QString::fromStdString(reaction_glyph(entry.id));
        if (entry.count > 1) {
            label += QStringLiteral(" %1").arg(entry.count);
        }
        labels.push_back(label);
    }
    return labels;
}

int reaction_row_height(const QFontMetricsF &metrics) {
    return int(std::ceil(metrics.height())) + 2 * kReactionChipVPadding;
}

QColor reaction_chip_fill(bool outgoing) {
    if (outgoing) {
        return st::windowBg->c.lightness() >= 128
            ? QColor(QStringLiteral("#D7EBE3"))
            : st::windowBgRipple->c;
    }
    return st::windowBgRipple->c;
}

QTextCharFormat secondary_format() {
    auto format = QTextCharFormat();
    format.setForeground(secondary_reading_color());
    auto font = format.font();
    font.setPixelSize(12);
    font.setWeight(QFont::Normal);
    format.setFont(font);
    return format;
}

// Message timestamps are the legible secondary rung of the reading surface.
// Empty-state prompts and the lazy-history banner keep the quieter 12px
// secondary format above, while per-message time scales with the body.
QTextCharFormat message_metadata_format() {
    auto format = QTextCharFormat();
    format.setForeground(secondary_reading_color());
    auto font = format.font();
    font.setPixelSize(13);
    font.setWeight(QFont::Normal);
    format.setFont(font);
    return format;
}

// The renderer-only presentation of one message's stored timestamp: the raw
// ISO string is parsed with Qt's ISO parser and shown in the local wall clock
// as HH:mm for the message header/time and a calendar day for the day divider.
// An invalid or empty raw value yields empty strings so the raw input is never
// shown verbatim.
struct PresentationTime {
    QString time;
    QString day;
    QDate date;
};

bool within_same_agent_interval(
        const std::string &before,
        const std::string &after) {
    const auto first = QDateTime::fromString(
        QString::fromStdString(before), Qt::ISODate);
    const auto second = QDateTime::fromString(
        QString::fromStdString(after), Qt::ISODate);
    if (!first.isValid() || !second.isValid()) {
        return false;
    }
    const auto seconds = first.secsTo(second);
    return seconds >= 0 && seconds <= kSameAgentGroupMaxSeconds;
}

PresentationTime present_timestamp(const std::string &raw) {
    if (raw.empty()) {
        return {};
    }
    const auto parsed = QDateTime::fromString(
        QString::fromStdString(raw), Qt::ISODate);
    if (!parsed.isValid()) {
        return {};
    }
    const auto local = parsed.toLocalTime();
    return {
        local.toString(QStringLiteral("HH:mm")),
        local.toString(QStringLiteral("yyyy/MM/dd")),
        local.date(),
    };
}

// The centered day divider label: Today / Yesterday for the local calendar,
// otherwise a long-form month-day-year like Slack and other chat apps.
QString format_day_label(const QDate &date) {
    if (!date.isValid()) {
        return {};
    }
    const auto today = QDate::currentDate();
    if (date == today) {
        return QStringLiteral("Today");
    }
    if (date == today.addDays(-1)) {
        return QStringLiteral("Yesterday");
    }
    return QLocale(QLocale::English, QLocale::UnitedStates)
        .toString(date, QStringLiteral("MMMM d, yyyy"));
}

QFont day_separator_font() {
    auto font = QApplication::font();
    font.setPixelSize(13);
    font.setWeight(QFont::DemiBold);
    return font;
}

void paint_day_separator_in_gap(
        QPainter &painter,
        qreal gap_top,
        qreal gap_bottom,
        const QString &label,
        int viewport_width,
        int v_offset,
        const QRectF &clip) {
    if (label.isEmpty() || gap_bottom <= gap_top) {
        return;
    }
    const auto gutter = reading_column_margins(viewport_width);
    const auto column_left = qreal(gutter);
    const auto column_right = qreal(viewport_width - gutter);
    const auto center_y = (gap_top + gap_bottom) / 2.0 - v_offset;
    const auto font = day_separator_font();
    const QFontMetricsF metrics(font);
    const auto text_width = metrics.horizontalAdvance(label);
    const auto text_height = metrics.height();
    const auto pad_h = 10.0;
    const auto center_x = (column_left + column_right) / 2.0;
    const auto text_left = center_x - text_width / 2.0;
    const auto text_right = center_x + text_width / 2.0;
    const auto text_bg = QRectF(
        text_left - pad_h,
        center_y - text_height / 2.0 - 2.0,
        text_width + 2.0 * pad_h,
        text_height + 4.0);
    const auto paint_rect = QRectF(
        column_left, center_y - text_height / 2.0 - 4.0,
        column_right - column_left, text_height + 8.0);
    if (!paint_rect.intersects(clip)) {
        return;
    }

    const auto line_color = secondary_reading_color();
    const auto bg = st::windowBg->c;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    QPen pen(line_color);
    pen.setWidthF(1.0);
    painter.setPen(pen);
    painter.drawLine(
        QPointF(column_left, center_y),
        QPointF(text_left - pad_h, center_y));
    painter.drawLine(
        QPointF(text_right + pad_h, center_y),
        QPointF(column_right, center_y));
    painter.fillRect(text_bg, bg);
    painter.setPen(line_color);
    painter.setFont(font);
    painter.drawText(text_bg, Qt::AlignCenter, label);
    painter.restore();
}

void paint_day_separators(
        QPainter &painter,
        const QPaintEvent *event,
        const QTextDocument *document,
        const QAbstractTextDocumentLayout *document_layout,
        int viewport_width,
        int v_offset) {
    const auto clip = QRectF(event->rect());
    auto previous_bottom = 0.0;
    for (auto it = document->rootFrame()->begin(); !it.atEnd(); ++it) {
        if (it.currentFrame()) {
            continue;
        }
        const auto block = it.currentBlock();
        if (!block.isValid()) {
            continue;
        }
        previous_bottom = qMax(
            previous_bottom,
            document_layout->blockBoundingRect(block).bottom());
    }
    for (auto *frame : document->rootFrame()->childFrames()) {
        const auto label = frame->frameFormat()
            .property(kDaySeparatorProperty).toString();
        const auto frame_rect = document_layout->frameBoundingRect(frame);
        if (!label.isEmpty()) {
            paint_day_separator_in_gap(
                painter,
                previous_bottom,
                frame_rect.top(),
                label,
                viewport_width,
                v_offset,
                clip);
        }
        previous_bottom = frame_rect.bottom();
    }
}

QTextCharFormat body_format(bool outgoing) {
    auto format = QTextCharFormat();
    format.setForeground(body_reading_color(outgoing));
    auto font = QApplication::font();
    font.setPixelSize(16);
    font.setWeight(QFont::Normal);
    font.setStyleHint(QFont::SansSerif);
    format.setFont(font);
    return format;
}

// The accepted safe-markdown character formats, each derived from the message
// body's base format so the direction colors stay intact while the run becomes
// visually distinct from the plain 16px body.
QTextCharFormat emphasized_text_format(const QTextCharFormat &base) {
    auto format = base;
    auto font = format.font();
    font.setWeight(QFont::Bold);
    format.setFont(font);
    return format;
}

QTextCharFormat heading_text_format(const QTextCharFormat &base) {
    auto format = base;
    auto font = format.font();
    font.setPixelSize(17);
    font.setWeight(QFont::DemiBold);
    format.setFont(font);
    return format;
}

QColor code_surface_color() {
    return st::windowBg->c.lightness() >= 128
        ? QColor(QStringLiteral("#F1F3F5"))
        : QColor(255, 255, 255, 24);
}

QTextCharFormat code_text_format(const QTextCharFormat &base) {
    auto format = base;
    auto font = format.font();
    font.setFixedPitch(true);
    font.setStyleHint(QFont::Monospace);
    format.setFont(font);
    format.setBackground(code_surface_color());
    return format;
}

QTextCharFormat quote_text_format(const QTextCharFormat &base) {
    auto format = base;
    auto font = format.font();
    font.setItalic(true);
    format.setFont(font);
    return format;
}

// One line scanner for the whole message body: the stored raw text is rendered
// through the accepted safe-markdown contract. Control markers disappear from
// the displayed plain text, formatted runs become real QTextCharFormat runs in
// the existing QTextDocument, raw HTML is never interpreted, and no resource,
// image, or URL load ever enters the document. Each non-delimiter body line is
// inserted into its own real, unmarked QTextBlock cloned from the caller's
// continuation block format; ``` fence delimiters only toggle the fence state
// and create no block. Everything outside the accepted marker set, including
// literal HTML, is inserted verbatim with insertText.
void insert_markdown_body(
        QTextCursor &cursor,
        const QString &body,
        const QTextCharFormat &base,
        const QTextBlockFormat &continuation,
        bool first_line_in_current_block = false) {
    const auto bold = emphasized_text_format(base);
    const auto heading = heading_text_format(base);
    const auto code = code_text_format(base);
    const auto quote = quote_text_format(base);
    const QRegularExpression technical(
        QStringLiteral(R"((\.[A-Za-z0-9_-]+(?:[/.][A-Za-z0-9_.-]+)+|[A-Za-z_][A-Za-z0-9_-]*(?:\.[A-Za-z0-9_-]+){2,}|--[A-Za-z0-9-]+))"));

    const auto insert_plain = [&](const QString &text,
                                  const QTextCharFormat &plain) {
        auto offset = 0;
        auto matches = technical.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            if (match.capturedStart() > offset) {
                cursor.insertText(text.mid(
                    offset, match.capturedStart() - offset), plain);
            }
            cursor.insertText(match.captured(), code);
            offset = match.capturedEnd();
        }
        cursor.insertText(text.mid(offset), plain);
    };

    const auto insert_inline = [&](const QString &line,
                                   const QTextCharFormat &plain) {
        auto index = 0;
        while (index < line.size()) {
            const auto bold_open = line.indexOf(QStringLiteral("**"), index);
            const auto code_open = line.indexOf(QChar('`'), index);
            const auto link_open = line.indexOf(QChar('['), index);
            auto at = -1;
            auto kind = 0;
            const auto consider = [&](int candidate, int candidate_kind) {
                if (candidate >= 0 && (at < 0 || candidate < at)) {
                    at = candidate;
                    kind = candidate_kind;
                }
            };
            consider(bold_open, 1);
            consider(code_open, 2);
            consider(link_open, 3);
            if (at < 0) {
                insert_plain(line.mid(index), plain);
                break;
            }
            if (at > index) {
                insert_plain(line.mid(index, at - index), plain);
            }
            if (kind == 2) {
                const auto close = line.indexOf(QChar('`'), at + 1);
                if (close < 0) {
                    insert_plain(line.mid(at), plain);
                    break;
                }
                cursor.insertText(line.mid(at + 1, close - at - 1), code);
                index = close + 1;
            } else if (kind == 3) {
                const auto close = line.indexOf(QStringLiteral("]("), at + 1);
                const auto end = close >= 0
                    ? line.indexOf(QChar(')'), close + 2)
                    : -1;
                if (close < 0 || end < 0) {
                    insert_plain(line.mid(at), plain);
                    break;
                }
                auto link_format = base;
                auto link_font = link_format.font();
                link_font.setUnderline(true);
                link_format.setFont(link_font);
                link_format.setForeground(QColor(0x1a, 0x73, 0xe8));
                link_format.setAnchor(true);
                link_format.setAnchorHref(
                    line.mid(close + 2, end - close - 2));
                cursor.insertText(line.mid(at + 1, close - at - 1), link_format);
                index = end + 1;
            } else {
                const auto close = line.indexOf(QStringLiteral("**"), at + 2);
                if (close < 0) {
                    insert_plain(line.mid(at), plain);
                    break;
                }
                cursor.insertText(line.mid(at + 2, close - at - 2), bold);
                index = close + 2;
            }
        }
    };

    auto in_code_fence = false;
    auto paragraph_break = false;
    const QRegularExpression heading_line(QStringLiteral(R"(^(#{1,6})\s+(.+)$)"));
    const QRegularExpression unordered(QStringLiteral(R"(^[-*]\s+(.+)$)"));
    const QRegularExpression ordered(QStringLiteral(R"(^(\d+[.)])\s+(.+)$)"));
    for (const auto &line : body.split(QChar::LineSeparator)) {
        if (line.startsWith(QStringLiteral("```"))) {
            in_code_fence = !in_code_fence;
            continue;
        }
        if (line.trimmed().isEmpty()) {
            if (cursor.block().isValid() && !cursor.block().text().isEmpty()) {
                auto previous = cursor.block().blockFormat();
                previous.setBottomMargin(kParagraphBottomMargin);
                cursor.setBlockFormat(previous);
            }
            paragraph_break = true;
            continue;
        }

        if (first_line_in_current_block) {
            first_line_in_current_block = false;
        } else {
            cursor.insertBlock(continuation);
        }
        auto block = continuation;
        block.setLineHeight(160, QTextBlockFormat::ProportionalHeight);
        block.setTopMargin(0);
        block.setBottomMargin(0);
        if (paragraph_break) {
            paragraph_break = false;
        }

        const auto heading_match = heading_line.match(line);
        const auto unordered_match = unordered.match(line);
        const auto ordered_match = ordered.match(line);
        if (in_code_fence) {
            block.setAlignment(Qt::AlignLeft);
            block.setLeftMargin(block.leftMargin() + 8);
            block.setRightMargin(block.rightMargin() + 8);
            block.setTopMargin(4);
            block.setBottomMargin(4);
            // Keep a nonempty semantic brush on the block for document/a11y
            // inspection, but let paintEvent own the visible rounded surface.
            block.setBackground(QColor(0, 0, 0, 1));
            block.setProperty(kCodeBlockProperty, true);
            cursor.setBlockFormat(block);
            cursor.insertText(line, code);
        } else if (heading_match.hasMatch()) {
            block.setAlignment(Qt::AlignLeft);
            block.setTopMargin(6);
            block.setBottomMargin(6);
            cursor.setBlockFormat(block);
            cursor.insertText(heading_match.captured(2), heading);
        } else if (unordered_match.hasMatch() || ordered_match.hasMatch()) {
            block.setAlignment(Qt::AlignLeft);
            block.setLeftMargin(block.leftMargin() + 16);
            block.setTextIndent(-14);
            block.setBottomMargin(kListBottomMargin);
            block.setLineHeight(150, QTextBlockFormat::ProportionalHeight);
            cursor.setBlockFormat(block);
            if (ordered_match.hasMatch()) {
                cursor.insertText(ordered_match.captured(1) + QChar(' '), base);
                insert_inline(ordered_match.captured(2), base);
            } else {
                cursor.insertText(QStringLiteral("• "), base);
                insert_inline(unordered_match.captured(1), base);
            }
        } else if (line.startsWith(QStringLiteral("> "))) {
            cursor.setBlockFormat(block);
            insert_inline(line.mid(2), quote);
        } else {
            cursor.setBlockFormat(block);
            insert_inline(line, base);
        }
    }
}

// The plain-state vertical anchor lives on the document root frame's top
// margin rather than on the first block: Qt's first-block layout does not
// honor a first block's own topMargin and places the line at the frame's top
// edge. Drop that margin whenever a real conversation is rebuilt or the plain
// state clears, so empty-state spacing can never leak into message layout.
void clear_plain_state_anchor(QTextDocument *document) {
    auto frame_format = document->rootFrame()->frameFormat();
    frame_format.setTopMargin(0);
    document->rootFrame()->setFrameFormat(frame_format);
}

// Apply the plain-state lane formatting to a whole document: symmetric
// reading-column gutters on the sides, horizontal center, and a root-frame
// top margin that anchors the first line a third of the way down the
// viewport, with the placeholder tone carried by the same secondary format
// messages use.
void apply_plain_state_formatting(
        QTextDocument *document,
        int viewport_width,
        int viewport_height) {
    const auto gutter = reading_column_margins(viewport_width);
    const auto line_height = QFontMetricsF(secondary_format().font()).height();
    QTextBlockFormat format;
    format.setAlignment(Qt::AlignCenter);
    format.setLeftMargin(gutter);
    format.setRightMargin(gutter);
    format.setTopMargin(0);
    auto cursor = QTextCursor(document);
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(format);
    cursor.select(QTextCursor::Document);
    cursor.mergeCharFormat(secondary_format());

    auto frame_format = document->rootFrame()->frameFormat();
    frame_format.setTopMargin(
        qMax(0.0, (viewport_height - line_height) / 3.0));
    document->rootFrame()->setFrameFormat(frame_format);
}

QTextCharFormat empty_state_title_format() {
    auto format = QTextCharFormat();
    format.setForeground(st::msgServiceFg);
    auto font = format.font();
    font.setPixelSize(kEmptyStateTitleSize);
    font.setWeight(QFont::Normal);
    format.setFont(font);
    return format;
}

QTextCharFormat select_agent_title_format() {
    auto format = QTextCharFormat();
    format.setForeground(st::dialogsNameFg);
    auto font = format.font();
    font.setPixelSize(kSelectAgentTitleSize);
    font.setWeight(QFont::DemiBold);
    format.setFont(font);
    return format;
}

// Quiet line-art illustration for the no-selection empty state: two figures
// with speech bubbles in a muted companion to the composer Send blue.
QPixmap select_agent_illustration(int size) {
    auto pixmap = QPixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    auto ink = st::windowBgActive->c;
    ink.setAlpha(160);
    auto pen = QPen(ink, 2.2);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const auto s = qreal(size);
    // Left figure.
    painter.drawEllipse(QRectF(s * 0.18, s * 0.18, s * 0.22, s * 0.22));
    painter.drawRoundedRect(
        QRectF(s * 0.14, s * 0.44, s * 0.30, s * 0.34), s * 0.12, s * 0.12);
    // Right figure.
    painter.drawEllipse(QRectF(s * 0.58, s * 0.22, s * 0.22, s * 0.22));
    painter.drawRoundedRect(
        QRectF(s * 0.54, s * 0.48, s * 0.30, s * 0.34), s * 0.12, s * 0.12);
    // Speech bubbles.
    painter.drawRoundedRect(
        QRectF(s * 0.34, s * 0.08, s * 0.22, s * 0.14), 4, 4);
    painter.drawLine(
        QPointF(s * 0.40, s * 0.22), QPointF(s * 0.36, s * 0.28));
    painter.drawRoundedRect(
        QRectF(s * 0.52, s * 0.34, s * 0.20, s * 0.12), 4, 4);
    painter.drawLine(
        QPointF(s * 0.62, s * 0.46), QPointF(s * 0.66, s * 0.52));
    painter.end();
    return pixmap;
}

// The small empty-state Agent avatar: a quiet muted circle carrying the
// Agent's own initial, painted with the same shared palette tokens the roster
// rows use so the empty conversation reads as the same partner identity.
QPixmap empty_state_avatar(const QString &initial, int diameter) {
    auto pixmap = QPixmap(diameter, diameter);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(st::windowBgActive->c);
    painter.drawEllipse(0, 0, diameter, diameter);
    auto font = QFont();
    font.setPixelSize(std::max(14, diameter / 2));
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(QColor(Qt::white));
    painter.drawText(QRect(0, 0, diameter, diameter), Qt::AlignCenter, initial);
    painter.end();
    return pixmap;
}

// Pale accent wash for drag-select. Soft enough to stay Slack-like on both
// light and dark windowBg without drowning body ink.
[[nodiscard]] QColor text_selection_wash_color() {
    auto wash = st::windowBgActive->c;
    wash.setAlpha(st::windowBg->c.lightness() >= 128 ? 48 : 72);
    return wash;
}

[[nodiscard]] QTextFrame *frame_owning_block(
        QTextDocument *document,
        const QTextBlock &block) {
    if (!document || !block.isValid()) {
        return nullptr;
    }
    const auto position = block.position();
    for (auto *frame : document->rootFrame()->childFrames()) {
        if (position >= frame->firstPosition()
                && position <= frame->lastPosition()) {
            return frame;
        }
    }
    return document->rootFrame();
}

// Paint selection behind the glyphs only (cursorToX → natural text span), not
// across the full message lane width Qt would fill with QPalette::Highlight.
void paint_glyph_tight_selection(
        QPainter &painter,
        QTextDocument *document,
        const QTextCursor &cursor,
        int h_offset,
        int v_offset,
        const QRect &clip) {
    if (!document || !cursor.hasSelection()) {
        return;
    }
    const auto sel_start = cursor.selectionStart();
    const auto sel_end = cursor.selectionEnd();
    if (sel_start >= sel_end) {
        return;
    }
    auto *document_layout = document->documentLayout();
    if (!document_layout) {
        return;
    }
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(text_selection_wash_color());
    for (auto block = document->findBlock(sel_start);
            block.isValid() && block.position() < sel_end;
            block = block.next()) {
        auto *block_layout = block.layout();
        if (!block_layout || block_layout->lineCount() <= 0) {
            continue;
        }
        auto *frame = frame_owning_block(document, block);
        if (!frame) {
            continue;
        }
        const auto frame_origin =
            document_layout->frameBoundingRect(frame).topLeft();
        // Exclude the block separator so a whole-line select stops at the last
        // glyph instead of extending through the trailing paragraph break.
        const auto block_pos = block.position();
        const auto local_start = std::max(0, sel_start - block_pos);
        const auto local_end = std::min(
            std::max(0, block.length() - 1),
            sel_end - block_pos);
        if (local_start >= local_end) {
            continue;
        }
        for (auto i = 0; i != block_layout->lineCount(); ++i) {
            const auto line = block_layout->lineAt(i);
            const auto line_start = line.textStart();
            const auto line_end = line_start + line.textLength();
            const auto from = std::max(local_start, line_start);
            const auto to = std::min(local_end, line_end);
            if (from >= to) {
                continue;
            }
            const auto x1 = line.cursorToX(from);
            const auto x2 = line.cursorToX(to);
            auto glyph = QRectF(
                std::min(x1, x2),
                line.y(),
                std::abs(x2 - x1),
                line.height())
                .translated(block_layout->position())
                .translated(frame_origin);
            glyph.translate(-h_offset, -v_offset);
            if (!glyph.intersects(QRectF(clip))) {
                continue;
            }
            painter.drawRoundedRect(glyph.adjusted(-1, 0, 1, 0), 3, 3);
        }
    }
    painter.restore();
}

} // namespace

ConversationSurface::ConversationSurface(QWidget *parent)
: QTextEdit(parent) {
    setReadOnly(true);
    setUndoRedoEnabled(false);
    setMinimumWidth(0);
    // The chat backdrop and bubbles are painted in paintEvent, so the Qt
    // viewport must not paint its own solid background on top of them.
    auto transparent_palette = palette();
    transparent_palette.setBrush(QPalette::Base, QBrush(Qt::transparent));
    transparent_palette.setBrush(QPalette::Window, QBrush(Qt::transparent));
    // Selection geometry is painted by paintEvent (glyph-tight). Keep Highlight
    // transparent so Qt does not fill the full message lane underneath.
    transparent_palette.setColor(QPalette::Highlight, Qt::transparent);
    transparent_palette.setColor(QPalette::HighlightedText, st::windowFg->c);
    setPalette(transparent_palette);
    viewport()->setAutoFillBackground(false);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent, false);
    document()->setDocumentMargin(kDocumentMargin);
}

void ConversationSurface::set_plain_state(const QString &text) {
    if (text == last_plain_state_
            && !select_agent_prompt_active_
            && !empty_state_active_) {
        return;
    }
    empty_state_active_ = false;
    select_agent_prompt_active_ = false;
    select_agent_main_name_.clear();
    them_.clear();
    last_plain_state_ = text;
    last_messages_.clear();
    last_reactions_.clear();
    // A plain (selection/no-route/empty) state has no lazy history, so the
    // render-time window resets to the full initial tail.
    history_offset_ = 0;
    setPlainText(text);
    if (!text.isEmpty()) {
        apply_plain_state_formatting(
            document(), viewport()->width(), viewport()->height());
    } else {
        clear_plain_state_anchor(document());
    }
    last_layout_width_ = int(viewport()->width() / 8) * 8;
}

void ConversationSurface::set_select_agent_prompt(const QString &main_agent_name) {
    auto text = QStringLiteral(
        "Select an agent\n"
        "Choose an agent from the sidebar to view its conversation.");
    if (!main_agent_name.trimmed().isEmpty()) {
        text += QStringLiteral("\nYour main agent is %1.")
            .arg(main_agent_name.trimmed());
    }
    // Keep the no-selection prompt on the plain-state path so a later
    // set_conversation() rebuild is identical to the pre-illustration flow.
    // The design illustration is painted in paintEvent while this prompt is
    // active (see select_agent_prompt_active_).
    if (select_agent_prompt_active_
            && select_agent_main_name_ == main_agent_name
            && last_plain_state_ == text) {
        return;
    }
    select_agent_main_name_ = main_agent_name;
    select_agent_prompt_active_ = true;
    empty_state_active_ = false;
    them_.clear();
    last_messages_.clear();
    last_reactions_.clear();
    history_offset_ = 0;
    last_plain_state_ = text;
    setPlainText(text);
    // Leave room above the title for the painted illustration.
    apply_plain_state_formatting(
        document(), viewport()->width(), viewport()->height());
    auto frame_format = document()->rootFrame()->frameFormat();
    frame_format.setTopMargin(std::max(
        frame_format.topMargin(),
        qreal(kSelectAgentIllustrationSize + kSelectAgentIllustrationGap
            + kEmptyStateHeadroom)));
    document()->rootFrame()->setFrameFormat(frame_format);
    auto cursor = QTextCursor(document());
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    cursor.mergeCharFormat(select_agent_title_format());
    last_layout_width_ = int(viewport()->width() / 8) * 8;
    update();
}

bool ConversationSurface::same_content(
        const std::vector<DirectConversationMessage> &messages,
        const std::unordered_map<std::string, MessageReactions> &reactions)
        const {
    if (last_messages_.size() != messages.size()
        || last_reactions_.size() != reactions.size()) {
        return false;
    }
    for (auto index = std::size_t{0}; index != messages.size(); ++index) {
        const auto &before = last_messages_[index];
        const auto &after = messages[index];
        if (before.id != after.id || before.outgoing != after.outgoing
            || before.timestamp != after.timestamp
            || before.text != after.text) {
            return false;
        }
    }
    for (const auto &[key, after] : reactions) {
        const auto found = last_reactions_.find(key);
        if (found == last_reactions_.end()
            || found->second.list.size() != after.list.size()) {
            return false;
        }
        for (auto index = std::size_t{0}; index != after.list.size(); ++index) {
            const auto &left = found->second.list[index];
            const auto &right = after.list[index];
            if (left.id != right.id || left.count != right.count
                || left.source != right.source
                || left.reactor != right.reactor) {
                return false;
            }
        }
    }
    return true;
}

void ConversationSurface::set_conversation(
        const QString &them,
        const std::vector<DirectConversationMessage> &messages,
        const std::unordered_map<std::string, MessageReactions> &reactions) {
    if (them == them_ && same_content(messages, reactions)) {
        return;
    }
    them_ = them;
    last_messages_ = messages;
    last_reactions_ = reactions;
    last_plain_state_.clear();
    select_agent_prompt_active_ = false;
    select_agent_main_name_.clear();
    empty_state_active_ = messages.empty();
    // Reset the render-time window to the initial chronological tail whenever
    // the conversation identity or content is replaced: nothing is paged in
    // beyond what the fresh cache naturally reveals.
    history_offset_ = int(messages.size() > std::size_t(kHistoryPageSize)
        ? messages.size() - std::size_t(kHistoryPageSize)
        : std::size_t{0});
    if (messages.empty()) {
        rebuild_empty_state();
    } else {
        rebuild_document();
    }
    last_layout_width_ = int(viewport()->width() / 8) * 8;
}

void ConversationSurface::rebuild_document() {
    if (rebuild_in_progress_) {
        return;
    }
    rebuild_in_progress_ = true;

    // Capture the human's exact scroll state before rebuilding the document,
    // so a changed refresh follows the new bottom only when the human was
    // already there and otherwise preserves the prior non-bottom position.
    auto *scrollbar = verticalScrollBar();
    const auto previous = scrollbar->value();
    const auto was_at_bottom = previous >= scrollbar->maximum();

    auto *document = this->document();
    document->clear();
    clear_plain_state_anchor(document);

    // The render-time history window: only the cached rows from history_offset_
    // to the end become real message frames, so an unchanged cache is never
    // paged in beyond the window a reveal already requested.
    const auto visible_begin = std::size_t(history_offset_);
    const auto visible_end = last_messages_.size();
    const auto viewport_width = viewport()->width();

    // When older rows are hidden, one centered muted banner on the root frame
    // leads the stream. It is plain root blocks, never a message QTextFrame, so
    // the banner does not count among the direct child message frames and keeps
    // the per-message container contract intact.
    if (history_offset_ > 0) {
        auto banner_cursor = QTextCursor(document);
        banner_cursor.movePosition(QTextCursor::Start);
        auto banner_format = QTextBlockFormat();
        banner_format.setAlignment(Qt::AlignCenter);
        const auto gutter = reading_column_margins(viewport_width);
        banner_format.setLeftMargin(gutter);
        banner_format.setRightMargin(gutter);
        banner_format.setTopMargin(kBannerTopMargin);
        banner_format.setBottomMargin(kBannerBottomMargin);
        banner_cursor.setBlockFormat(banner_format);
        banner_cursor.insertText(
            QStringLiteral("\u25B2 %1 older \u2014 ctrl+u to load")
                .arg(history_offset_),
            secondary_format());
    }

    // One transparent borderless sibling QTextFrame per message under the root
    // frame, each owning its header and body blocks: the frame's first block
    // is the header carrying the message block format, and every body logical
    // line becomes its own real unmarked QTextBlock cloned from that format,
    // so the standard layout honors the block alignment and the margins bind
    // each message to the shared reading-column width.
    const auto separator = QString(QChar::LineSeparator);
    const auto agent_lane_max = message_block_width(viewport_width);
    const auto human_lane_max = human_message_block_width(viewport_width);
    // The formatted day tracks only this visible lazy suffix, so a separator
    // appears before the first message of each changed nonempty day within the
    // window and never leaks across reveals.
    // Telegram Desktop's sizing is two-stage on one real text object: first lay
    // the actual message out at its allowed lane, then read that same layout's
    // real line widths and shrink the lane once. Keep the real frames here for
    // that second stage; no detached measurement document is involved.
    struct PendingMessageWidth final {
        QTextFrame *frame = nullptr;
        bool outgoing = false;
        int lane_min = 0;
        int lane_max = 0;
        int horizontal_padding = 0;
        qreal provisional_left = 0;
        qreal provisional_right = 0;
    };
    std::vector<PendingMessageWidth> pending_widths;

    QString previous_day;
    for (auto index = visible_begin; index != visible_end; ++index) {
        const auto &message = last_messages_[index];
        const auto outgoing = message.outgoing;
        // The renderer-only presentation (HH:mm time, yyyy/MM/dd day) is
        // computed once per visible message and shared by the incoming header,
        // the outgoing frame time, and the day separator.
        const auto present = present_timestamp(message.timestamp);
        const auto day_changed = !present.day.isEmpty()
            && present.day != previous_day;
        // A visible Agent group starts at the lazy-window boundary, after a
        // Human row/day boundary, or when the preceding Agent message is more
        // than five minutes away. Only a proven short chronological interval
        // creates a headerless continuation.
        const auto continues_previous = !outgoing
            && index > visible_begin
            && !last_messages_[index - 1].outgoing
            && !day_changed
            && within_same_agent_interval(
                last_messages_[index - 1].timestamp, message.timestamp);
        const auto incoming_group_first = !outgoing && !continues_previous;
        auto group_continues = !outgoing
            && index + 1 < visible_end
            && !last_messages_[index + 1].outgoing
            && within_same_agent_interval(
                message.timestamp, last_messages_[index + 1].timestamp);
        if (group_continues) {
            const auto next_day = present_timestamp(
                last_messages_[index + 1].timestamp).day;
            group_continues = present.day.isEmpty() || next_day.isEmpty()
                || next_day == present.day;
        }

        // Each message's width is content-driven. Agent prose retains the
        // established 72% lane and minimum; Human bubbles use a smaller 60%
        // column cap, a genuinely compact minimum, and their own 15px padding.
        const auto lane_max = outgoing ? human_lane_max : agent_lane_max;
        const auto lane_min = outgoing
            ? kHumanMinMessageWidth
            : kMinMessageWidth;
        const auto horizontal_padding = outgoing
            ? kHumanBubbleHPadding
            : kBubbleHPadding;
        // Stage one uses the whole allowed lane. This is the same provisional
        // width Telegram passes into its real text object's countSize(): it lets
        // the real frame decide line breaks before any content-sized shrink.
        const auto block_format = message_block_format(
            outgoing, viewport_width, lane_max);
        // A fresh document-end cursor places every message as its own direct
        // sibling QTextFrame under the root frame, never nested. The frame is
        // transparent with no border, padding, or margin, so only the first
        // block's own alignment and margins shape the message lane.
        auto cursor = QTextCursor(document);
        cursor.movePosition(QTextCursor::End);
        auto frame_format = QTextFrameFormat();
        frame_format.setBorder(0);
        frame_format.setPadding(0);
        frame_format.setMargin(0);
        // The current frame owns its gap to the next sibling: consecutive
        // short-interval same-Agent rows keep a small rendered rhythm, while a
        // long-pause/direction/day/end boundary gets the visibly larger break.
        frame_format.setBottomMargin(outgoing
            ? kHumanMessageBottomMargin
            : (group_continues
                ? kWithinGroupBottomMargin
                : kBetweenGroupBottomMargin));
        frame_format.setBackground(Qt::transparent);
        frame_format.setProperty(kMessageOutgoingProperty, outgoing);
        frame_format.setProperty(
            kMessageIdProperty, QString::fromStdString(message.id));
        const auto reaction_found = last_reactions_.find(message.id);
        const auto reaction_labels = reaction_found == last_reactions_.end()
            ? QStringList()
            : reaction_chip_labels(reaction_found->second);
        if (!reaction_labels.isEmpty()) {
            frame_format.setProperty(
                kMessageReactionsProperty, reaction_labels);
            // Telegram-like in-bubble chips need room below the body text.
            const auto chip_font = secondary_format().font();
            const auto chip_row = reaction_row_height(QFontMetricsF(chip_font));
            frame_format.setBottomMargin(
                (outgoing
                    ? kHumanMessageBottomMargin
                    : (group_continues
                        ? kWithinGroupBottomMargin
                        : kBetweenGroupBottomMargin))
                + chip_row + kReactionRowTopGap);
        }
        frame_format.setProperty(
            kMessageAvatarProperty, incoming_group_first);
        // The Human timestamp is not a visible header: it rides on the frame so
        // paintEvent can draw a compact muted line below-right of the body-only
        // bubble, or omit it when the presented HH:mm is empty.
        if (outgoing) {
            frame_format.setProperty(kMessageTimestampProperty, present.time);
        }
        // The first message frame of each calendar day carries the divider
        // label on its frame metadata and extra top margin so paintEvent can
        // draw the centered rules in the gap above it, between message groups.
        if (day_changed) {
            frame_format.setTopMargin(kDayTopMargin + kDayBottomMargin);
            frame_format.setProperty(
                kDaySeparatorProperty, format_day_label(present.date));
            previous_day = present.day;
        }
        auto *frame = cursor.insertFrame(frame_format);
        cursor = frame->firstCursorPosition();
        // The header block carries the whole-message lane format with no block
        // bottom margin: the frame's bottom margin owns the outer sibling gap.
        auto header_format = block_format;
        header_format.setBottomMargin(0);
        cursor.setBlockFormat(header_format);
        if (!outgoing && incoming_group_first) {
            cursor.insertText(them_, sender_format(outgoing));
            if (!present.time.isEmpty()) {
                cursor.insertText(
                    QStringLiteral(" · %1").arg(present.time),
                    message_metadata_format());
            }
            cursor.insertText(separator, secondary_format());
        }
        // Stored email subject/title metadata is deliberately absent from every
        // message surface; the conversation begins with sender metadata (Agent)
        // or the body itself (Human).
        // The stored raw body is rendered through the accepted safe-markdown
        // contract: only the narrow marker set becomes character formatting,
        // everything else (including raw HTML) stays literal text. Each body
        // line becomes its own real unmarked QTextBlock cloned from the
        // message block format with zero top/bottom margins and no message
        // property, so only the message header keeps the property. Paragraph
        // delimiters are normalized first so a decoded LF, CRLF/CR or U+2029
        // reads as a line break; stored source text is untouched.
        auto body = QString::fromStdString(message.text);
        body.replace(QStringLiteral("\r\n"), QString(QChar::LineSeparator));
        body.replace(QChar::LineFeed, QString(QChar::LineSeparator));
        body.replace(QChar::CarriageReturn, QString(QChar::LineSeparator));
        body.replace(QChar::ParagraphSeparator, QString(QChar::LineSeparator));
        auto continuation = block_format;
        continuation.setTopMargin(0);
        continuation.setBottomMargin(0);
        continuation.clearProperty(kMessageBlockProperty);
        // An outgoing body-only message keeps its first real body line in the
        // frame's existing initial block (preserving that block's lane and top
        // margin) instead of opening a fresh continuation block. Incoming group
        // firsts retain a sender/time header; headerless continuations put their
        // body directly in the initial block at the identical left axis.
        const auto first_in_initial = outgoing
            || (!outgoing && !incoming_group_first);
        insert_markdown_body(
            cursor, body, body_format(outgoing), continuation,
            first_in_initial);
        pending_widths.push_back({
            frame,
            outgoing,
            lane_min,
            lane_max,
            horizontal_padding,
            block_format.leftMargin(),
            block_format.rightMargin(),
        });
    }

    // Stage two reads the real QTextLayouts just created above, equivalent to
    // Telegram's countSize()/textRealWidth() pass. Shrink every message once to
    // its widest real line plus declared padding, preserving per-block list
    // indents and other semantic formatting as the outer lane margins move.
    document->documentLayout()->documentSize();
    for (const auto &pending : pending_widths) {
        auto real_width = 0.0;
        for (auto it = pending.frame->begin(); !it.atEnd(); ++it) {
            const auto block = it.currentBlock();
            if (!block.isValid()) {
                continue;
            }
            const auto *layout = block.layout();
            for (auto line_index = 0;
                    line_index != layout->lineCount(); ++line_index) {
                real_width = std::max(
                    real_width,
                    layout->lineAt(line_index).horizontalAdvance());
            }
        }
        // Block margins are measured in the viewport, while QTextDocument also
        // subtracts its root margin from both sides of the actual line lane. Add
        // that existing inset back so declared bubble padding is not consumed by
        // the document chrome during the second layout pass.
        const auto document_inset = int(std::ceil(
            2 * document->documentMargin()));
        const auto final_width = qBound(
            qMin(pending.lane_min, pending.lane_max),
            int(std::ceil(real_width)) + 2 * pending.horizontal_padding
                + document_inset,
            pending.lane_max);
        const auto final_lane = message_block_format(
            pending.outgoing, viewport_width, final_width);
        const auto left_delta = final_lane.leftMargin()
            - pending.provisional_left;
        const auto right_delta = final_lane.rightMargin()
            - pending.provisional_right;
        for (auto it = pending.frame->begin(); !it.atEnd(); ++it) {
            const auto block = it.currentBlock();
            if (!block.isValid()) {
                continue;
            }
            auto format = block.blockFormat();
            format.setLeftMargin(format.leftMargin() + left_delta);
            format.setRightMargin(format.rightMargin() + right_delta);
            auto block_cursor = QTextCursor(block);
            block_cursor.setBlockFormat(format);
        }
    }
    document->documentLayout()->documentSize();

    if (was_at_bottom) {
        scroll_to_bottom();
    } else {
        // Cancel any deferred bottom pin from a prior follow-bottom rebuild so
        // it cannot yank the viewport after this non-bottom restore.
        ++scroll_pin_generation_;
        scrollbar->setValue(std::min(previous, scrollbar->maximum()));
    }

    rebuild_in_progress_ = false;
}

void ConversationSurface::rebuild_select_agent_prompt() {
    if (!select_agent_prompt_active_ || last_plain_state_.isEmpty()) {
        return;
    }
    apply_plain_state_formatting(
        document(), viewport()->width(), viewport()->height());
    auto cursor = QTextCursor(document());
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    cursor.mergeCharFormat(select_agent_title_format());
}

void ConversationSurface::rebuild_empty_state() {
    auto *document = this->document();
    document->clear();
    clear_plain_state_anchor(document);
    auto cursor = QTextCursor(document);
    cursor.movePosition(QTextCursor::Start);

    // The empty no-message state is one centered group inside the same reading
    // column messages use: symmetric gutters bound every block, and AlignCenter
    // holds the small Agent avatar, the title, and the muted prompt together
    // rather than the full pane.
    const auto gutter = reading_column_margins(viewport()->width());
    auto centered = QTextBlockFormat();
    centered.setAlignment(Qt::AlignCenter);
    centered.setLeftMargin(gutter);
    centered.setRightMargin(gutter);

    auto avatar_format = centered;
    avatar_format.setBottomMargin(kEmptyAvatarGap);
    cursor.setBlockFormat(avatar_format);
    auto image_format = QTextImageFormat();
    image_format.setName(QStringLiteral("empty-state-agent-avatar"));
    image_format.setWidth(kEmptyAvatarDiameter);
    image_format.setHeight(kEmptyAvatarDiameter);
    cursor.insertImage(image_format);

    auto title_format = centered;
    title_format.setBottomMargin(kEmptyTitleGap);
    cursor.insertBlock(title_format);
    cursor.insertText(
        QStringLiteral("No messages yet"), empty_state_title_format());

    cursor.insertBlock(centered);
    cursor.insertText(
        QStringLiteral("Send a message or start the Agent."),
        secondary_format());

    auto frame_format = document->rootFrame()->frameFormat();
    frame_format.setTopMargin(kEmptyStateHeadroom);
    document->rootFrame()->setFrameFormat(frame_format);

    const auto initial = them_.trimmed().isEmpty()
        ? QStringLiteral("A")
        : them_.trimmed().left(1).toUpper();
    document->addResource(
        QTextDocument::ImageResource,
        QUrl(QStringLiteral("empty-state-agent-avatar")),
        empty_state_avatar(initial, kEmptyAvatarDiameter));
}

void ConversationSurface::reveal_older() {
    // Page one more older slice of the cached history into the render-time
    // window, clamped so the offset never goes below zero (the full stream).
    history_offset_ = std::max(0, history_offset_ - kHistoryPageSize);

    // Rebuild with the wider window, then restore the previously first-visible
    // message's viewport Y. Revealing rows at the top grows the document by
    // exactly the new scroll maximum's increase, so shifting the current
    // scroll position by that delta keeps the old anchor frame on screen.
    auto *scrollbar = verticalScrollBar();
    const auto old_maximum = scrollbar->maximum();
    const auto previous = scrollbar->value();
    rebuild_document();
    const auto delta = scrollbar->maximum() - old_maximum;
    scrollbar->setValue(std::min(previous + delta, scrollbar->maximum()));
}

void ConversationSurface::scroll_to_bottom_now() {
    auto *bar = verticalScrollBar();
    const auto laid_out = document()->documentLayout()->documentSize().height();
    const auto layout_max = std::max(0,
        int(std::ceil(laid_out - viewport()->height())));
    if (layout_max > bar->maximum()) {
        bar->setMaximum(layout_max);
    }
    bar->setValue(bar->maximum());
}

void ConversationSurface::scroll_to_bottom() {
    scroll_to_bottom_now();
    // QTextEdit may still sync its slider from a queued documentSizeChanged
    // after a new-day frame top margin lands. Pin again once that maximum is
    // real so the viewport sits on the true document bottom — but only if the
    // human has not scrolled away from the pin in the meantime.
    const auto pinned_at = verticalScrollBar()->value();
    const auto generation = ++scroll_pin_generation_;
    QTimer::singleShot(0, this, [this, generation, pinned_at] {
        if (generation != scroll_pin_generation_) return;
        if (verticalScrollBar()->value() != pinned_at) return;
        scroll_to_bottom_now();
    });
}

void ConversationSurface::paintEvent(QPaintEvent *event) {
    auto *surface_viewport = viewport();
    QPainter painter(surface_viewport);
    painter.setClipRect(event->rect());
    // The chat backdrop shares the shell's single-canvas `windowBg` base;
    // message bubbles and transient interaction states keep their own tokens.
    painter.fillRect(event->rect(), st::windowBg);

    if (select_agent_prompt_active_) {
        const auto illustration = select_agent_illustration(
            kSelectAgentIllustrationSize);
        const auto x = (surface_viewport->width()
            - kSelectAgentIllustrationSize) / 2;
        const auto y = std::max(16,
            int(document()->rootFrame()->frameFormat().topMargin())
                - kSelectAgentIllustrationSize
                - kSelectAgentIllustrationGap);
        painter.drawPixmap(x, y, illustration);
    }

    const auto h_offset = horizontalScrollBar()->value();
    const auto v_offset = verticalScrollBar()->value();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    const auto *document_layout = document()->documentLayout();
    const auto message_frames = document()->rootFrame()->childFrames();
    for (auto *frame : message_frames) {
        // Each sibling message frame carries its own advancing document
        // origin, while the blocks' QTextLayout positions stay relative to
        // that frame. Translate every line by both the block layout position
        // and the frame's document top-left so each bubble sits around its
        // native Qt text instead of the first frame's local coordinates.
        const auto frame_origin = document_layout
            ->frameBoundingRect(frame)
            .topLeft();
        QTextBlock first_valid_block;
        auto text_bounds = QRectF();
        auto code_bounds = QRectF();
        for (auto block = frame->begin(); !block.atEnd();
             ++block) {
            const auto current_block = block.currentBlock();
            if (!current_block.isValid()) {
                continue;
            }
            const auto *block_layout = current_block.layout();
            auto block_bounds = QRectF();
            for (auto i = 0; i != block_layout->lineCount(); ++i) {
                const auto line = block_layout->lineAt(i);
                const auto line_bounds = line.naturalTextRect()
                    .translated(block_layout->position())
                    .translated(frame_origin);
                block_bounds = block_bounds.isNull()
                    ? line_bounds
                    : block_bounds.united(line_bounds);
            }
            if (block_bounds.isNull()) {
                continue;
            }
            if (current_block.blockFormat()
                    .property(kCodeBlockProperty).toBool()) {
                code_bounds = code_bounds.isNull()
                    ? block_bounds
                    : code_bounds.united(block_bounds);
            }
            if (!first_valid_block.isValid()) {
                first_valid_block = current_block;
            }
            text_bounds = text_bounds.isNull()
                ? block_bounds
                : text_bounds.united(block_bounds);
        }
        if (text_bounds.isNull() || !first_valid_block.isValid()) {
            continue;
        }
        text_bounds.translate(-h_offset, -v_offset);
        if (!code_bounds.isNull()) {
            code_bounds.translate(-h_offset, -v_offset);
        }
        const auto outgoing = frame->frameFormat()
            .property(kMessageOutgoingProperty)
            .toBool();
        if (outgoing) {
            // `naturalTextRect()` stops at the longest painted glyph run, so
            // word-wrap slack made otherwise identical capped Human lanes end
            // at different x positions. The QTextLine rect owns the actual
            // right-anchored lane width; use that edge for both bubble and time.
            const auto *first_layout = first_valid_block.layout();
            const auto lane_right = first_layout->lineAt(0).rect()
                .translated(first_layout->position())
                .translated(frame_origin)
                .right() - h_offset;
            auto bubble = text_bounds.adjusted(
                -kHumanBubbleHPadding,
                -kHumanBubbleVPadding,
                kHumanBubbleHPadding,
                kHumanBubbleVPadding);
            bubble.setRight(lane_right + kHumanBubbleHPadding);
            const auto reaction_labels = frame->frameFormat()
                .property(kMessageReactionsProperty)
                .toStringList();
            auto chip_font = secondary_format().font();
            chip_font.setPixelSize(13);
            const auto chip_metrics = QFontMetricsF(chip_font);
            const auto chip_row = reaction_labels.isEmpty()
                ? 0
                : reaction_row_height(chip_metrics);
            if (chip_row > 0) {
                bubble.setBottom(
                    bubble.bottom() + kReactionRowTopGap + chip_row
                    + kReactionRowBottomInset);
            }
            // The painter is already clipped to event->rect(). Do not skip the
            // whole message merely because the bubble itself misses a partial
            // repaint: the below-bubble timestamp may still intersect it.
            painter.setBrush(human_bubble_color());
            painter.drawRoundedRect(
                bubble, kHumanBubbleRadius, kHumanBubbleRadius);
            if (chip_row > 0) {
                auto chip_x = bubble.left() + kReactionRowSideInset;
                const auto chip_y = bubble.bottom() - kReactionRowBottomInset
                    - chip_row;
                for (const auto &label : reaction_labels) {
                    const auto text_width = chip_metrics.horizontalAdvance(label);
                    const auto chip_width = text_width + 2 * kReactionChipHPadding;
                    const auto chip_rect = QRectF(
                        chip_x, chip_y, chip_width, chip_row);
                    painter.setBrush(reaction_chip_fill(true));
                    painter.drawRoundedRect(chip_rect, chip_row / 2.0, chip_row / 2.0);
                    painter.setPen(body_reading_color(true));
                    painter.setFont(chip_font);
                    painter.drawText(
                        chip_rect, Qt::AlignCenter, label);
                    painter.setPen(Qt::NoPen);
                    chip_x += chip_width + kReactionChipGap;
                }
            }
            // The stored HH:mm is one compact muted 12px line below the bubble,
            // sharing its right edge instead of floating beside the top.
            const auto timestamp = frame->frameFormat()
                .property(kMessageTimestampProperty)
                .toString();
            if (!timestamp.isEmpty()) {
                const auto metadata = secondary_format();
                const auto metrics = QFontMetricsF(metadata.font());
                const auto text_width = metrics.horizontalAdvance(timestamp);
                const auto text_height = metrics.height();
                const auto time_rect = QRectF(
                    bubble.right() - text_width,
                    bubble.bottom() + kTimestampGap,
                    text_width,
                    text_height);
                if (time_rect.intersects(QRectF(event->rect()))) {
                    painter.save();
                    painter.setPen(secondary_reading_color());
                    painter.setFont(metadata.font());
                    painter.drawText(
                        time_rect,
                        Qt::AlignRight | Qt::AlignVCenter,
                        timestamp);
                    painter.restore();
                }
            }
        } else {
            const auto reaction_labels = frame->frameFormat()
                .property(kMessageReactionsProperty)
                .toStringList();
            if (!reaction_labels.isEmpty()) {
                auto chip_font = secondary_format().font();
                chip_font.setPixelSize(13);
                const auto chip_metrics = QFontMetricsF(chip_font);
                const auto chip_row = reaction_row_height(chip_metrics);
                auto chip_x = text_bounds.left();
                const auto chip_y = text_bounds.bottom() + kReactionRowTopGap;
                for (const auto &label : reaction_labels) {
                    const auto text_width = chip_metrics.horizontalAdvance(label);
                    const auto chip_width = text_width + 2 * kReactionChipHPadding;
                    const auto chip_rect = QRectF(
                        chip_x, chip_y, chip_width, chip_row);
                    if (chip_rect.intersects(QRectF(event->rect()))) {
                        painter.setBrush(reaction_chip_fill(false));
                        painter.drawRoundedRect(
                            chip_rect, chip_row / 2.0, chip_row / 2.0);
                        painter.setPen(body_reading_color(false));
                        painter.setFont(chip_font);
                        painter.drawText(chip_rect, Qt::AlignCenter, label);
                        painter.setPen(Qt::NoPen);
                    }
                    chip_x += chip_width + kReactionChipGap;
                }
            }
            if (frame->frameFormat()
                .property(kMessageAvatarProperty).toBool()) {
                const auto avatar = QRectF(
                    text_bounds.left() - kMessageAvatarGap
                        - kMessageAvatarDiameter,
                    text_bounds.top(),
                    kMessageAvatarDiameter,
                    kMessageAvatarDiameter);
                if (avatar.intersects(QRectF(event->rect()))) {
                    painter.setBrush(st::windowBgActive->c);
                    painter.drawEllipse(avatar);
                    auto font = QFont();
                    font.setPixelSize(18);
                    font.setWeight(QFont::DemiBold);
                    painter.setFont(font);
                    painter.setPen(QColor(Qt::white));
                    painter.drawText(avatar, Qt::AlignCenter,
                        them_.trimmed().left(1).toUpper());
                    painter.setPen(Qt::NoPen);
                }
            }
        }
        // Only fenced code gets a separate low-contrast rounded surface;
        // ordinary paragraphs remain on the single Conversation canvas.
        if (!code_bounds.isNull()) {
            const auto code_surface = code_bounds.adjusted(-6, -4, 6, 4);
            if (code_surface.intersects(QRectF(event->rect()))) {
                painter.save();
                painter.setPen(Qt::NoPen);
                painter.setBrush(code_surface_color());
                painter.drawRoundedRect(code_surface, 6, 6);
                painter.restore();
            }
        }
    }
    // Glyph-tight selection wash sits above bubbles and below Qt's text draw,
    // so selected ink stays readable without a full-lane Highlight fill.
    paint_glyph_tight_selection(
        painter,
        document(),
        textCursor(),
        h_offset,
        v_offset,
        event->rect());
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.end();

    // Let Qt draw the text with its native scroll translation, selection,
    // copy, and accessibility behavior; the widget's transparent Base keeps
    // the custom backdrop and bubbles visible underneath.
    QTextEdit::paintEvent(event);

    QPainter overlay(surface_viewport);
    overlay.setClipRect(event->rect());
    paint_day_separators(
        overlay,
        event,
        document(),
        document_layout,
        surface_viewport->width(),
        v_offset);
    overlay.end();
}

void ConversationSurface::reflow_to_viewport() {
    const auto viewport_width = viewport()->width();
    if (viewport_width < 16) {
        last_layout_width_ = -1;
        return;
    }
    if (!last_plain_state_.isEmpty()) {
        apply_plain_state_formatting(
            document(), viewport_width, viewport()->height());
    }
    // Quantize the viewport/layout width so a live resize only reflows when
    // the bound meaningfully changes. This follows the full layout width, not
    // just the capped message width: on a very wide pane the centered reading
    // column's outer gutters keep moving after the message cap stops.
    const auto width = int(viewport_width / 8) * 8;
    if (width == last_layout_width_) {
        return;
    }
    last_layout_width_ = width;
    if (rebuild_in_progress_) {
        return;
    }
    if (select_agent_prompt_active_) {
        rebuild_select_agent_prompt();
    } else if (empty_state_active_) {
        rebuild_empty_state();
    } else if (!last_messages_.empty()) {
        // A resize reflows the already-materialized render-time window; it
        // never pages older rows in, so the current history offset is kept.
        rebuild_document();
    }
}

void ConversationSurface::resizeEvent(QResizeEvent *event) {
    QTextEdit::resizeEvent(event);
    reflow_to_viewport();
}

void ConversationSurface::showEvent(QShowEvent *event) {
    QTextEdit::showEvent(event);
    if (last_messages_.empty() && last_plain_state_.isEmpty()
            && !empty_state_active_ && !select_agent_prompt_active_) {
        return;
    }
    last_layout_width_ = -1;
    reflow_to_viewport();
}

void ConversationSurface::keyPressEvent(QKeyEvent *event) {
    // Ctrl+U reveals the next older page, but only while the window is pinned
    // to the very top and older rows remain hidden. Everywhere else the
    // inherited text-edit key handling (scrolling, selection, copy) stays.
    if (event->modifiers() == Qt::ControlModifier
        && event->key() == Qt::Key_U
        && history_offset_ > 0
        && verticalScrollBar()->value() <= verticalScrollBar()->minimum()) {
        reveal_older();
        event->accept();
        return;
    }
    QTextEdit::keyPressEvent(event);
}

} // namespace lingtai::desktop
