#pragma once

#include <string_view>

namespace lingtai::desktop {

// Compiled Desktop-owned first-boot content. Unknown/empty language requests
// deliberately use English so direct callers receive a complete project too.
struct ProjectCreationResources {
    std::string_view language;
    std::string_view greeting_template;
    std::string_view adaptive_playbook;
    std::string_view command_reference;
};

[[nodiscard]] const ProjectCreationResources &project_creation_resources(
    std::string_view language) noexcept;

} // namespace lingtai::desktop
