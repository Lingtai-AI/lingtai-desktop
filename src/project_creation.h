#pragma once

#include "agent_setup_store.h"
#include "preset_catalog.h"

#include <QtCore/QObject>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace lingtai::desktop {

enum class ProjectCreationFailure {
    none,
    invalid_destination,
    invalid_agent_name,
    unsafe_path,
    existing_project,
    invalid_preset,
    runtime_unavailable,
    staging_failed,
    publish_failed,
    local_failure,
};

// Stable transaction boundary carried with every result. A failure reports
// the boundary whose work was refused; success reports `complete`. Runtime
// start remains a post-commit AgentLifecycleController result rather than a
// creation stage.
enum class ProjectCreationStage {
    none,
    draft_validation,
    staging,
    staged_generation,
    staged_validation,
    publication,
    complete,
};

[[nodiscard]] const char *project_creation_stage_name(
    ProjectCreationStage stage) noexcept;

enum class ProjectCreationFailurePoint {
    none,
    after_staging,
    after_generation,
    after_marker_removal,
    publish_refused,
};

struct ProjectCreationRequest {
    std::filesystem::path destination;
    std::filesystem::path preset_path;
    std::vector<std::filesystem::path> allowed_preset_paths;
    std::filesystem::path runtime_python;
    std::filesystem::path env_file;
    std::filesystem::path covenant_file;
    std::string agent_name;
    std::string agent_directory;
    AgentSetupDraft setup;
    std::string comment;
    ProjectCreationFailurePoint failure_point =
        ProjectCreationFailurePoint::none;
};

struct ProjectCreationResult {
    bool created = false;
    std::filesystem::path project_dir;
    std::filesystem::path agent_key;
    ProjectCreationFailure failure = ProjectCreationFailure::local_failure;
    ProjectCreationStage stage = ProjectCreationStage::none;
    std::string detail;

    [[nodiscard]] explicit operator bool() const noexcept { return created; }
};

// Builds one new .lingtai tree in an owned sibling staging directory and
// publishes it with one no-replace rename. The destination must already be a
// real absolute directory. Existing contents are never read, changed, or
// removed except for the exclusively-created staging leaf and the final
// .lingtai publication. This function never launches a process.
[[nodiscard]] ProjectCreationResult create_project(
    const ProjectCreationRequest &request) noexcept;

// The UI-facing serial worker. Preset scans and creation both run away from
// the Qt UI thread and deliver at most one queued completion while the runner
// remains alive. Destruction joins the single owned worker before its QObject
// delivery context is released.
class ProjectCreationRunner final {
public:
    using PresetDone = std::function<void(PresetCatalogLoadResult)>;
    using CreateDone = std::function<void(ProjectCreationResult)>;

    ProjectCreationRunner();
    ~ProjectCreationRunner();

    ProjectCreationRunner(const ProjectCreationRunner &) = delete;
    ProjectCreationRunner &operator=(const ProjectCreationRunner &) = delete;

    void run_catalog(const QString &global_dir, PresetDone done);
    void run_create(ProjectCreationRequest request, CreateDone done);
    [[nodiscard]] bool is_pending() const noexcept;

private:
    struct DeliveryState;
    QObject delivery_context_;
    std::shared_ptr<DeliveryState> delivery_;
    std::thread worker_;
    bool pending_ = false;
};

} // namespace lingtai::desktop
