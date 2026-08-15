#include "ui/agent_roster.h"

#include "agent_projection.h"
#include "styles/palette.h"
#include "ui/rp_widget.h"
#include "ui/style/style_core_scale.h"

#include <QtCore/QString>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayout>
#include <QtWidgets/QPushButton>

#include <filesystem>
#include <iostream>
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
