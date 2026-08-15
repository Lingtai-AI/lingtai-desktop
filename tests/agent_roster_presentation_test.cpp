#include "ui/agent_roster.h"

#include "agent_projection.h"
#include "styles/palette.h"
#include "ui/rp_widget.h"
#include "ui/style/style_core_scale.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayout>
#include <QtWidgets/QPushButton>

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

QPushButton *agent_row(QWidget &widget, std::string_view key) {
    const auto expected = QString::fromUtf8(key.data(), key.size());
    for (auto *candidate : widget.findChildren<QPushButton *>()) {
        if (candidate->property("directory_key").toString() == expected) {
            return candidate;
        }
    }
    return nullptr;
}

constexpr double kInkDistance = 48.0;
constexpr double kSecondaryScaleRatio = 0.90;

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
    // Only the x10..width-10 text column counts; any pixel at least
    // kInkDistance RGB away from the idle background is glyph ink, and the
    // physical min/max y span converts back to logical pixels so the result
    // is devicePixelRatio-agnostic.
    const auto min_x = int(10.0 * dpr);
    const auto max_x = image.width() - int(10.0 * dpr);
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

void verify_secondary_roster_text_matches_primary_scale() {
    AgentSnapshot snapshot;
    snapshot.scan = AgentScanState::complete;
    snapshot.items = { make_row("MMMMMMMM", AgentRole::main) };

    QWidget parent;
    AgentRoster roster(&parent);
    roster.set_rows(snapshot, std::nullopt);
    roster.resize(260, roster.height());
    roster.show();
    QCoreApplication::processEvents();

    auto *button = agent_row(roster, "MMMMMMMM");
    require(button != nullptr, "the glyph-rich row must render for grab");
    button->clearFocus();
    roster.clearFocus();

    const auto image = button->grab().toImage();
    const auto dpr = std::max(1.0, double(image.devicePixelRatio()));

    const auto background = sample_idle_background(image, dpr);
    const auto primary_span = band_ink_span(
        image, dpr, background, 8.0, 31.0);
    const auto secondary_span = band_ink_span(
        image, dpr, background, 31.0, 54.0);
    require(primary_span > 0.0 && secondary_span > 0.0,
        "the rendered row must show glyph ink in both the primary and "
        "secondary text bands");
    require(secondary_span >= primary_span * kSecondaryScaleRatio,
        "the secondary roster text must render at the same mature visual "
        "scale as the primary");
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

    require(agent_row(roster, "a-human") == nullptr,
        "the human pseudo-agent must never appear as an Agent roster row");
    for (const auto key : { "b-main", "c-agent", "d-stale" }) {
        require(agent_row(roster, key) != nullptr,
            std::string("real Agent row must remain rendered: ") + key);
    }

    auto *rows = roster.findChild<Ui::RpWidget *>(
        "lingtai_agent_roster_rows");
    require(rows != nullptr, "the rendered roster rows container must exist");
    auto visible_keys = std::vector<std::string>();
    for (auto index = 0; index != rows->layout()->count(); ++index) {
        if (const auto *row = qobject_cast<QPushButton *>(
                rows->layout()->itemAt(index)->widget())) {
            visible_keys.push_back(
                row->property("directory_key").toString().toStdString());
        }
    }
    require(visible_keys == std::vector<std::string>{
            "b-main", "c-agent", "d-stale"},
        "rendered rows must retain the snapshot's deterministic order with "
        "the human omitted");

    auto *state = roster.findChild<QLabel *>("lingtai_agent_roster_state");
    require(state != nullptr, "the roster state label must exist");
    require(state->text()
            == QStringLiteral("3 Agent(s) — scan complete"),
        "roster state must count visible Agent rows only");

    roster.set_rows(snapshot, fs::path("c-agent"));
    require(agent_row(roster, "c-agent")->isChecked()
            && !agent_row(roster, "b-main")->isChecked(),
        "selection must still bind to the caller's real-Agent key");

    verify_secondary_roster_text_matches_primary_scale();
}

} // namespace

int main(int argc, char **argv) {
    (void)argv;
    try {
        QApplication application(argc, argv);
        style::internal::init_palette(style::kScaleDefault);
        verify_human_hidden_from_roster();
        std::cout << "agent roster presentation: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "agent roster presentation: " << error.what() << '\n';
        return 1;
    }
}
