#include "ui/agent_roster.h"

#include "agent_projection.h"
#include "styles/palette.h"
#include "ui/rp_widget.h"
#include "ui/style/style_core_scale.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtGui/QAction>
#include <QtGui/QColor>
#include <QtGui/QFontMetricsF>
#include <QtGui/QFontMetrics>
#include <QtGui/QImage>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFrame>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayout>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;

using lingtai::desktop::AgentManifestDiagnosticKind;
using lingtai::desktop::AgentManifestKind;
using lingtai::desktop::AgentPresenceKind;
using lingtai::desktop::AgentRole;
using lingtai::desktop::AgentRow;
using lingtai::desktop::AgentScanState;
using lingtai::desktop::AgentSnapshot;
using lingtai::desktop::AgentRoster;

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

AgentRow make_row(fs::path key, AgentRole role) {
    AgentRow result;
    result.directory_key = key;
    result.directory_path = key;
    result.manifest_kind = AgentManifestKind::valid;
    result.manifest_diagnostic = AgentManifestDiagnosticKind::none;
    result.role = role;
    result.presence = (role == AgentRole::human)
        ? AgentPresenceKind::alive_human
        : AgentPresenceKind::alive;
    return result;
}

constexpr double kInkDistance = 48.0;
constexpr double kHierarchyScaleRatio = 1.05;
constexpr double kSecondaryMaturityRatio = 0.85;
constexpr double kSelectedNeutralMajority = 0.5;
constexpr double kAvatarDiameter = 40.0;
constexpr double kRowVerticalFrame = 8.0;
constexpr double kRowHorizontalFrame = 14.0;
constexpr double kAvatarTextGap = 10.0;
constexpr int kCanvasGrabWidth = 232;

QWidget *roster_rows_canvas(AgentRoster &roster) {
    auto *canvas = roster.findChild<QWidget *>("lingtai_agent_roster_rows");
    require(canvas != nullptr, "the rendered roster rows canvas must exist");
    return canvas;
}

int canvas_row_height(QWidget *canvas) {
    auto primary_font = canvas->font();
    primary_font.setPointSize(13);
    primary_font.setWeight(QFont::DemiBold);
    auto secondary_font = canvas->font();
    secondary_font.setPointSize(12);
    const auto text_height = QFontMetrics(primary_font).height()
        + QFontMetrics(secondary_font).height();
    return std::max(int(kAvatarDiameter), text_height) + 2 * int(kRowVerticalFrame);
}

QImage grab_canvas_row(
        AgentRoster &roster,
        int canvas_width,
        int canvas_height,
        std::size_t row_index = 0) {
    roster.resize(260, 320);
    roster.show();
    QCoreApplication::processEvents();
    auto *canvas = roster_rows_canvas(roster);
    const auto total_height = canvas_height * int(row_index + 1);
    canvas->setFixedSize(canvas_width, total_height);
    QCoreApplication::processEvents();
    const auto image = canvas->grab().toImage();
    const auto dpr = std::max(1.0, double(image.devicePixelRatio()));
    return image.copy(QRect(
        0,
        int(row_index * canvas_height * dpr),
        int(canvas_width * dpr),
        int(canvas_height * dpr)));
}

int reference_canvas_row_height(const AgentSnapshot &snapshot) {
    QWidget parent;
    AgentRoster roster(&parent);
    roster.set_rows(snapshot, std::nullopt);
    roster.show();
    QCoreApplication::processEvents();
    return canvas_row_height(roster_rows_canvas(roster));
}

std::vector<AgentRow> visible_rows(const AgentSnapshot &snapshot) {
    auto rows = snapshot.items;
    rows.erase(std::remove_if(rows.begin(), rows.end(),
        [](const AgentRow &item) { return item.role == AgentRole::human; }),
        rows.end());
    return rows;
}

std::size_t visible_row_index(const AgentSnapshot &snapshot, std::string_view key) {
    const auto rows = visible_rows(snapshot);
    const auto expected = fs::path(key);
    for (std::size_t index = 0; index != rows.size(); ++index) {
        if (rows[index].directory_key == expected) {
            return index;
        }
    }
    throw std::runtime_error(
        std::string("missing visible Agent row: ") + std::string(key));
}

QColor sample_idle_background(const QImage &image, double dpr) {
    // The idle row background fills the whole row; the text rect only ever
    // covers x10..width-10, y8..54, so every pixel outside it is safe to
    // sample, and the most common such pixel is the row's idle background.
    std::map<QRgb, int> counts;
    const auto min_x = int(10.0 * dpr);
    const auto max_x = image.width() - int(10.0 * dpr);
    const auto min_y = int(8.0 * dpr);
    const auto max_y = int(54.0 * dpr);
    for (auto y = 0; y != image.height(); ++y) {
        for (auto x = 0; x != image.width(); ++x) {
            if (x >= min_x && x < max_x && y >= min_y && y < max_y) {
                continue;
            }
            ++counts[image.pixel(x, y)];
        }
    }
    auto best = image.pixel(0, 0);
    auto best_count = 0;
    for (const auto &[rgb, count] : counts) {
        if (count > best_count) {
            best = rgb;
            best_count = count;
        }
    }
    return QColor(best);
}

double band_ink_span(const QImage &image, double dpr,
        const QColor &background, double logical_min_y, double logical_max_y) {
    // Only the text column starting after the leading avatar column counts;
    // any pixel at least kInkDistance RGB away from the idle background is
    // glyph ink, and the physical min/max y span converts back to logical
    // pixels so the result is devicePixelRatio-agnostic.
    const auto min_x = int((kRowHorizontalFrame + kAvatarDiameter
        + kAvatarTextGap) * dpr);
    const auto max_x = image.width() - int(kRowHorizontalFrame * dpr);
    const auto min_y = int(logical_min_y * dpr);
    const auto max_y = int(logical_max_y * dpr);
    auto ink_min = std::optional<int>();
    auto ink_max = std::optional<int>();
    for (auto y = min_y; y < max_y && y < image.height(); ++y) {
        for (auto x = min_x; x < max_x && x < image.width(); ++x) {
            const auto pixel = image.pixelColor(x, y);
            const auto dr = pixel.red() - background.red();
            const auto dg = pixel.green() - background.green();
            const auto db = pixel.blue() - background.blue();
            const auto distance = std::sqrt(
                double(dr * dr + dg * dg + db * db));
            if (distance >= kInkDistance) {
                if (!ink_min.has_value()) {
                    ink_min = y;
                }
                ink_max = y;
            }
        }
    }
    if (!ink_min.has_value() || !ink_max.has_value()) {
        return 0.0;
    }
    return (double(*ink_max + 1) / dpr) - (double(*ink_min) / dpr);
}

// The leading avatar disc is a solid single-color fill distinct from the idle
// row background. Text glyph ink is thin, multi-shade, and never forms a
// contiguous fill, so the single most frequent non-background color in the
// leading strip is the avatar disc's solid fill, and its count is DPR-scaled
// to the strip area.
int leading_avatar_fill(const QImage &image, double dpr,
        const QColor &background) {
    std::map<QRgb, int> counts;
    const auto min_x = int(10.0 * dpr);
    const auto max_x = int((10.0 + kAvatarDiameter) * dpr);
    for (auto y = 0; y != image.height(); ++y) {
        for (auto x = min_x; x < max_x && x < image.width(); ++x) {
            const auto pixel = image.pixelColor(x, y);
            const auto dr = pixel.red() - background.red();
            const auto dg = pixel.green() - background.green();
            const auto db = pixel.blue() - background.blue();
            if (std::sqrt(double(dr * dr + dg * dg + db * db))
                    >= kInkDistance) {
                ++counts[pixel.rgb()];
            }
        }
    }
    auto best = 0;
    for (const auto &[rgb, count] : counts) {
        (void)rgb;
        best = std::max(best, count);
    }
    return best;
}

void verify_modern_roster_typography_hierarchy() {
    AgentSnapshot snapshot;
    snapshot.scan = AgentScanState::complete;
    snapshot.items = { make_row("Main Agent · Active", AgentRole::main) };

    QWidget parent;
    AgentRoster roster(&parent);
    roster.set_rows(snapshot, std::nullopt);
    roster.resize(260, roster.height());
    roster.show();
    QCoreApplication::processEvents();

    const auto row_height = reference_canvas_row_height(snapshot);
    const auto image = grab_canvas_row(roster, kCanvasGrabWidth, row_height);
    const auto dpr = std::max(1.0, double(image.devicePixelRatio()));

    const auto background = sample_idle_background(image, dpr);
    const auto primary_span = band_ink_span(
        image, dpr, background, 8.0, 31.0);
    const auto secondary_span = band_ink_span(
        image, dpr, background, 31.0, 54.0);
    require(primary_span > 0.0 && secondary_span > 0.0,
        "the rendered row must show glyph ink in both the primary and "
        "secondary text bands");
    require(primary_span >= secondary_span * kHierarchyScaleRatio,
        "the primary identity must render at a purposefully larger visual "
        "scale than the secondary metadata");
    require(secondary_span >= primary_span * kSecondaryMaturityRatio,
        "the secondary metadata must keep a readable mature subordinate "
        "scale; the exact 12pt target remains source-reviewed");
}

void verify_selected_row_keeps_neutral_majority_surface() {
    AgentSnapshot snapshot;
    snapshot.scan = AgentScanState::complete;
    snapshot.items = { make_row("Selected", AgentRole::main) };

    QWidget parent;
    AgentRoster roster(&parent);
    roster.set_rows(snapshot, fs::path("Selected"));
    roster.resize(260, roster.height());
    roster.show();
    QCoreApplication::processEvents();

    const auto row_height = reference_canvas_row_height(snapshot);
    const auto image = grab_canvas_row(roster, kCanvasGrabWidth, row_height);
    const auto area = image.width() * image.height();

    const auto accent = st::dialogsBgActive->c;
    auto accent_pixels = 0;
    for (auto y = 0; y != image.height(); ++y) {
        for (auto x = 0; x != image.width(); ++x) {
            if (image.pixelColor(x, y) == accent) {
                ++accent_pixels;
            }
        }
    }
    require(double(accent_pixels) / area >= kSelectedNeutralMajority,
        "the selected row surface must paint a calm dialogsBgActive body "
        "across the rounded row");
    require(image.pixelColor(0, 0) != accent
            && image.pixelColor(image.width() - 1, 0) != accent
            && image.pixelColor(0, image.height() - 1) != accent
            && image.pixelColor(image.width() - 1, image.height() - 1)
                != accent,
        "the selected rounded body must leave all four outer corners outside "
        "its fill");
    const auto dpr = std::max(1.0, double(image.devicePixelRatio()));
    require(image.pixelColor(
            image.width() - int(4.0 * dpr), image.height() / 2) == accent,
        "the rounded selected body must still fill its interior with the "
        "shared dialogsBgActive token");
}

void verify_roster_rows_are_virtual_canvas() {
    AgentSnapshot snapshot;
    snapshot.scan = AgentScanState::complete;
    snapshot.items = {
        make_row("a-agent", AgentRole::agent),
        make_row("b-main", AgentRole::main),
    };

    QWidget parent;
    AgentRoster roster(&parent);
    roster.set_rows(snapshot, std::nullopt);

    auto *rows = roster.findChild<Ui::RpWidget *>(
        "lingtai_agent_roster_rows");
    require(rows != nullptr, "the rendered roster rows container must exist");

    const auto buttons = rows->findChildren<QPushButton *>(
        QString(), Qt::FindDirectChildrenOnly);
    require(buttons.empty(),
        "Agent rows are virtual canvas data, not child QPushButtons");
}

void verify_sidebar_header_typography() {
    AgentSnapshot snapshot;
    snapshot.scan = AgentScanState::complete;
    snapshot.items = { make_row("a-agent", AgentRole::agent) };

    QWidget parent;
    AgentRoster roster(&parent);
    roster.set_rows(snapshot, std::nullopt);

    auto *heading = roster.findChild<QLabel *>(
        "lingtai_agent_roster_heading");
    auto *state = roster.findChild<QLabel *>(
        "lingtai_agent_roster_state");
    require(heading != nullptr && state != nullptr,
        "the compact Agents heading and count must exist");
    require(heading->font().weight() == QFont::Normal,
        "the compact Agents heading must use regular weight so it does not "
        "compete with the semibold app and selected-Agent titles");
    require(state->font().weight() == QFont::Normal,
        "the tertiary Agent count must remain regular weight");
    require(state->palette().color(QPalette::WindowText)
            == st::windowSubTextFg->c,
        "the tertiary Agent count must use the smaller/lighter shared "
        "windowSubTextFg tone");
}

void verify_human_hidden_from_roster() {
    AgentSnapshot snapshot;
    snapshot.scan = AgentScanState::complete;
    snapshot.items = {
        make_row("a-human", AgentRole::human),
        make_row("b-main", AgentRole::main),
        make_row("c-agent", AgentRole::agent),
        make_row("d-stale", AgentRole::agent),
    };

    QWidget parent;
    AgentRoster roster(&parent);
    roster.set_rows(snapshot, std::nullopt);

    const auto rendered = visible_rows(snapshot);
    require(rendered.size() == 3,
        "the human pseudo-agent must never appear as an Agent roster row");
    for (const auto key : { "b-main", "c-agent", "d-stale" }) {
        require(visible_row_index(snapshot, key) < rendered.size(),
            std::string("real Agent row must remain rendered: ") + key);
    }

    auto *rows = roster_rows_canvas(roster);
    const auto buttons = rows->findChildren<QPushButton *>(
        QString(), Qt::FindDirectChildrenOnly);
    require(buttons.empty(),
        "Agent rows are virtual canvas data, not child QPushButtons");
    require(rendered[0].directory_key == fs::path("b-main")
            && rendered[1].directory_key == fs::path("c-agent")
            && rendered[2].directory_key == fs::path("d-stale"),
        "rendered rows must retain the snapshot's deterministic order with "
        "the human omitted");

    auto *state = roster.findChild<QLabel *>("lingtai_agent_roster_state");
    require(state != nullptr, "the roster state label must exist");
    require(state->text() == QStringLiteral("3"),
        "the compact roster header must show only the visible Agent count");
    require(roster.findChild<QLabel *>(
            "lingtai_sidebar_workspace_label") == nullptr,
        "the redundant Workspace label must be absent");
    auto *heading = roster.findChild<QLabel *>(
        "lingtai_agent_roster_heading");
    auto *scroll = roster.findChild<QScrollArea *>(
        "lingtai_agent_roster_scroll");
    require(heading != nullptr && scroll != nullptr,
        "the compact Agents heading and roster scroll area must exist");
    roster.show();
    QCoreApplication::processEvents();
    require(std::abs(heading->geometry().center().y()
            - state->geometry().center().y()) <= 2,
        "the Agents heading and optional count must share one compact row");
    require(scroll->frameShape() == QFrame::NoFrame,
        "the Agent-list scroll area must not draw an outer frame");
    require(rows->layout() == nullptr,
        "the virtual canvas rows must not use a child-widget layout");

    AgentRoster selected_roster(&parent);
    selected_roster.set_rows(snapshot, fs::path("c-agent"));
    selected_roster.resize(260, selected_roster.height());
    selected_roster.show();
    QCoreApplication::processEvents();
    const auto row_height = reference_canvas_row_height(snapshot);
    const auto c_agent_index = visible_row_index(snapshot, "c-agent");
    const auto idle_image = grab_canvas_row(
        roster, kCanvasGrabWidth, row_height, c_agent_index);
    const auto selected_image = grab_canvas_row(
        selected_roster, kCanvasGrabWidth, row_height, c_agent_index);
    require(idle_image != selected_image,
        "selection must still bind to the caller's real-Agent key");

    verify_modern_roster_typography_hierarchy();
}

void verify_intrinsic_roster_row_behavior() {
    // One long display/directory key at the constrained 260px roster width is
    // the intrinsic-row contract: the row leads with a fixed-diameter circular
    // avatar visibly distinct from the row background, derives its height from
    // avatar + two font lines + stable padding instead of a hard min=max62
    // box, keeps the full untruncated directory truth in the accessible
    // surface while the visible text column stays bounded, and needs no
    // public/test-only production seam.
    const auto key = std::string(
        "org/lingtai/workspaces/very/long/agent/display/name/for/elision");
    AgentSnapshot snapshot;
    snapshot.scan = AgentScanState::complete;
    snapshot.items = { make_row(key, AgentRole::agent) };

    QWidget parent;
    AgentRoster roster(&parent);
    roster.set_rows(snapshot, std::nullopt);
    roster.resize(260, roster.height());
    roster.show();
    QCoreApplication::processEvents();

    auto *canvas = roster_rows_canvas(roster);
    const auto row_height = canvas_row_height(canvas);
    require(row_height != 62,
        "the roster row must not be a hard min=max62 box: its height must be "
        "intrinsic from avatar + two font lines + stable padding");
    require(canvas->sizeHint().height() >= int(kAvatarDiameter)
            + 2 * int(kRowVerticalFrame),
        "the intrinsic canvas sizeHint must accommodate the fixed avatar disc "
        "plus the stable vertical framing");

    const auto image = grab_canvas_row(roster, kCanvasGrabWidth, row_height);
    const auto dpr = std::max(1.0, double(image.devicePixelRatio()));

    const auto background = sample_idle_background(image, dpr);
    const auto strip_area = int(kAvatarDiameter * dpr) * image.height();
    require(leading_avatar_fill(image, dpr, background) >= strip_area / 4,
        "the leading region must render a fixed-diameter solid circular "
        "avatar disc visibly distinct from the row background");

    const auto full_key = QString::fromUtf8(key.data(), key.size());
    const auto visible_column = canvas->width() - int(kAvatarDiameter)
        - 2 * int(kRowHorizontalFrame);
    QFontMetricsF metrics(canvas->font());
    require(metrics.horizontalAdvance(full_key) > double(visible_column),
        "the long full key must be wider than the available visible text "
        "column, so the visible name has to be constrained to fit; the "
        "ellipsis glyph itself is bound by source review of the production "
        "elidedText call, not pixel-claimed here");
    require(canvas->width() <= 260,
        "the roster canvas must stay bounded within the 260px roster column");
}

// I2 modern-roster contract. Whole-row selection must visibly diverge from
// idle with a calm rounded `windowBgRipple` body and no legacy accent stripe.
// The idle row and the list field must both merge into the Sidebar's
// `st::windowBgOver` semantic role (the old white `windowBg` autofill
// created a full rectangular panel), and the visible secondary line
// must render the friendly 1:1 label form while the accessible description
// keeps the raw facts verbatim. These are asserted on the current widget tree
// only; no future production symbol is referenced.
void verify_row_selection_diverges_and_field_uses_sidebar_bg() {
    constexpr double kWholeRowSelectionDifference = 0.05;

    AgentSnapshot snapshot;
    snapshot.scan = AgentScanState::complete;
    snapshot.items = { make_row("Alpha", AgentRole::main) };

    QWidget parent;
    AgentRoster idle_roster(&parent);
    idle_roster.set_rows(snapshot, std::nullopt);
    idle_roster.resize(260, idle_roster.height());
    idle_roster.show();
    QCoreApplication::processEvents();

    AgentRoster selected_roster(&parent);
    selected_roster.set_rows(snapshot, fs::path("Alpha"));
    selected_roster.resize(260, selected_roster.height());
    selected_roster.show();
    QCoreApplication::processEvents();

    const auto row_height = reference_canvas_row_height(snapshot);
    const auto idle_image = grab_canvas_row(
        idle_roster, kCanvasGrabWidth, row_height);
    const auto selected_image = grab_canvas_row(
        selected_roster, kCanvasGrabWidth, row_height);
    require(idle_image.deviceIndependentSize()
            == selected_image.deviceIndependentSize(),
        "the idle and selected row grabs must be the same size to compare");
    auto selected_compare = selected_image;
    if (selected_compare.size() != idle_image.size()) {
        selected_compare = selected_compare.scaled(
            idle_image.size(), Qt::IgnoreAspectRatio);
    }
    const auto dpr = std::max(1.0, double(idle_image.devicePixelRatio()));

    auto differing_pixels = 0;
    auto comparable_pixels = 0;
    for (auto y = 0; y != idle_image.height(); ++y) {
        for (auto x = 0; x != idle_image.width(); ++x) {
            ++comparable_pixels;
            if (idle_image.pixelColor(x, y)
                    != selected_compare.pixelColor(x, y)) {
                ++differing_pixels;
            }
        }
    }
    require(double(differing_pixels) / comparable_pixels
            >= kWholeRowSelectionDifference,
        "the idle and selected row bodies must differ materially across "
        "the rounded surface rather than through a narrow accent cue");

    const auto accent = st::dialogsBgActive->c;
    auto accent_pixels = 0;
    for (auto y = 0; y != selected_compare.height(); ++y) {
        for (auto x = 0; x != selected_compare.width(); ++x) {
            if (selected_compare.pixelColor(x, y) == accent) {
                ++accent_pixels;
            }
        }
    }
    const auto selected_area =
        selected_compare.width() * selected_compare.height();
    require(double(accent_pixels) / selected_area >= kSelectedNeutralMajority,
        "the selected row must keep its calm dialogsBgActive body as it "
        "diverges from the idle row");

    auto *rows = roster_rows_canvas(idle_roster);
    auto *scroll = idle_roster.findChild<QScrollArea *>(
        "lingtai_agent_roster_scroll");
    require(rows != nullptr && scroll != nullptr,
        "the roster rows field and its scroll area must exist");
    require(scroll->viewport()->palette().color(QPalette::Window)
            == st::windowBg->c,
        "the list field viewport must auto-fill the shared Sidebar windowBg "
        "surface instead of forming a white rectangle");
    require(scroll->viewport()->autoFillBackground(),
        "the list field viewport must auto-fill the shared Sidebar role so "
        "its empty area remains seamless rather than becoming a white panel");
    const auto idle_background = sample_idle_background(idle_image, dpr);
    require(idle_background == st::windowBgOver->c,
        "the idle row body must merge into the same st::windowBgOver Sidebar "
        "surface as the list field");

    const auto &row = snapshot.items.front();
    require(friendly_agent_role_text(row.role) == QStringLiteral("Main Agent")
            && friendly_agent_presence_text(row.presence)
                == QStringLiteral("Active"),
        "the visible secondary line must render the friendly 1:1 label form "
        "(role · presence) while the primary line stays the agent key");
}

// Project-toolbar contract. The top-left control is one compact flat
// `LingTai` selector whose chevron is painted as a small icon. It must not
// fall back to the platform's beveled/default QPushButton frame. The selector
// menu owns the current path and the existing Open Project action identity.
void verify_compact_project_selector_and_menu() {
    QWidget parent;
    AgentRoster roster(&parent);
    roster.show();
    QCoreApplication::processEvents();

    auto *selector = roster.findChild<QPushButton *>(
        "lingtai_project_selector");
    require(selector != nullptr,
        "the project toolbar must expose its LingTai selector");
    require(roster.findChild<QPushButton *>("lingtai_new_project_button")
            == nullptr,
        "the project toolbar must not expose a separate New Project control");
    require(selector->text() == QStringLiteral("LingTai"),
        "the selector label must be plain `LingTai`; its small chevron is an "
        "adjacent painted icon, not a wide text glyph");
    require(selector->isFlat(),
        "the project selector must suppress the platform's default "
        "beveled button frame");
    require(selector->accessibleName().contains(QStringLiteral("LingTai")),
        "the compact selector must keep a LingTai project accessible "
        "identity");

    auto *root = roster.findChild<QLabel *>("lingtai_project_root");
    require(root != nullptr,
        "the project-root label that reports the current path must remain");
    const auto current_path = root->text();

    auto *open_entry = static_cast<QAction *>(nullptr);
    auto path_entry_seen = false;
    selector->click();
    QCoreApplication::processEvents();
    for (auto *menu : roster.findChildren<QMenu *>()) {
        for (auto *action : menu->actions()) {
            if (action->objectName()
                    == QStringLiteral("lingtai_open_project_button")) {
                open_entry = action;
            } else if (action->objectName()
                    == QStringLiteral("lingtai_new_project_button")) {
                require(false,
                    "the selector menu must not expose a separate New Project "
                    "entry");
            }
            if (action->text() == current_path) {
                path_entry_seen = true;
            }
        }
    }
    require(open_entry != nullptr,
        "the selector menu must expose Open Project with the existing object "
        "identity");
    require(path_entry_seen,
        "the selector menu must surface the current project path");
}

// Light single-canvas contract. At a normal height the roster exposes a large
// bare base surface below its compact toolbar, heading, and rows. That field
// must use the same plain `st::windowBg` canvas as the content pane, not the
// raised `st::windowBgOver` token reserved for transient interaction states.
void verify_large_base_surface_is_light_canvas() {
    AgentSnapshot snapshot;
    snapshot.scan = AgentScanState::complete;
    snapshot.items = { make_row("a-main", AgentRole::main) };

    QWidget parent;
    AgentRoster roster(&parent);
    roster.set_rows(snapshot, std::nullopt);
    roster.resize(260, 560);
    roster.show();
    QCoreApplication::processEvents();
    roster.clearFocus();

    const auto image = roster.grab().toImage();
    const auto base_min_y = int(image.height() * 0.60);
    require(base_min_y < image.height(),
        "a normal-height roster must leave room for a sampled base band");

    auto mismatch = 0;
    auto sampled = 0;
    for (auto y = base_min_y; y != image.height(); ++y) {
        for (auto x = 0; x != image.width(); ++x) {
            ++sampled;
            if (image.pixelColor(x, y) != st::windowBg->c) {
                ++mismatch;
            }
        }
    }
    require(sampled > 0 && mismatch == 0,
        "the roster's large bare base surface must be the plain light "
        "st::windowBg canvas rather than the st::windowBgOver Sidebar role");
}

} // namespace

int main(int argc, char **argv) {
    (void)argv;
    try {
        QApplication application(argc, argv);
        style::internal::init_palette(style::kScaleDefault);
        if (argc > 1
                && QString::fromLocal8Bit(argv[1])
                    == QStringLiteral("--project-toolbar-only")) {
            verify_compact_project_selector_and_menu();
            std::cout << "project toolbar presentation: OK\n";
            return 0;
        }
        if (argc > 1
                && QString::fromLocal8Bit(argv[1])
                    == QStringLiteral("--header-typography-only")) {
            verify_sidebar_header_typography();
            std::cout << "agent roster header typography: OK\n";
            return 0;
        }
        if (argc > 1
                && QString::fromLocal8Bit(argv[1])
                    == QStringLiteral("--light-canvas-only")) {
            verify_large_base_surface_is_light_canvas();
            std::cout << "agent roster light canvas: OK\n";
            return 0;
        }
        verify_roster_rows_are_virtual_canvas();
        verify_human_hidden_from_roster();
        verify_selected_row_keeps_neutral_majority_surface();
        verify_intrinsic_roster_row_behavior();
        verify_row_selection_diverges_and_field_uses_sidebar_bg();
        verify_compact_project_selector_and_menu();
        std::cout << "agent roster presentation: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "agent roster presentation: " << error.what() << '\n';
        return 1;
    }
}
