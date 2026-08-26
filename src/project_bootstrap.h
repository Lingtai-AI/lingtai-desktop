#pragma once

#include "preset_catalog.h"

#include <QtCore/QProcess>
#include <QtCore/QStringList>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace lingtai::desktop {

enum class PresetDiscoveryKind {
    succeeded,
    empty,
    malformed,
    process_failed,
};

struct PresetDiscoveryResult {
    PresetDiscoveryKind kind = PresetDiscoveryKind::process_failed;
    std::vector<PresetEntry> presets;
    std::string code;
    std::string error;
};

enum class SpawnOutcomeKind {
    launched,
    malformed,
    process_failed,
};

struct SpawnOutcome {
    SpawnOutcomeKind kind = SpawnOutcomeKind::process_failed;
    std::filesystem::path project_dir;
    std::string code;
    std::string error;
};

// The one async owner of the two canonical headless TUI calls
// `<exe> presets` and `<exe> spawn <dir> --preset <name>`. Each starts
// exactly one nonblocking QProcess with the exact separate argv -- never a
// shell string and never a joined command line -- and reports exactly one
// result through the one per-call callback. It tracks no PID, retry, lock,
// rollback, or lifecycle state; the TUI child owns the creation work.
class ProjectBootstrapRunner final {
public:
    using PresetDone = std::function<void(PresetDiscoveryResult)>;
    using SpawnDone = std::function<void(SpawnOutcome)>;

    ProjectBootstrapRunner();
    ~ProjectBootstrapRunner();

    ProjectBootstrapRunner(const ProjectBootstrapRunner &) = delete;
    ProjectBootstrapRunner &operator=(const ProjectBootstrapRunner &) = delete;

    void run_presets(
        const std::filesystem::path &executable,
        PresetDone done);
        void run_spawn(
        const std::filesystem::path &executable,
        const std::filesystem::path &destination,
        const std::string &preset_name,
        SpawnDone done,
        const std::string &agent_name = {},
        const std::string &language = {});

    [[nodiscard]] bool is_pending() const noexcept;

private:
    void start(const QString &program, const QStringList &arguments);

    QProcess process_;
    bool pending_ = false;
    bool spawn_mode_ = false;
    PresetDone preset_done_;
    SpawnDone spawn_done_;
};

} // namespace lingtai::desktop
