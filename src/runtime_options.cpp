#include "runtime_options.h"

#include <cstdlib>
#include <string_view>

namespace lingtai::desktop {
namespace {

bool env_truthy(const char *name) {
    const auto *value = std::getenv(name);
    return value && *value && std::string_view(value) != "0";
}

std::filesystem::path env_path(const char *name) {
    const auto *value = std::getenv(name);
    if (!value || !*value) {
        return {};
    }
    return std::filesystem::path(value);
}

} // namespace

RuntimeOptions normalize_runtime_options(RuntimeOptions options) {
    if (options.ui_test_mode) {
        options.animations_enabled = env_truthy("APP_UI_TEST_ANIMATIONS");
        options.network_enabled = env_truthy("APP_UI_TEST_NETWORK");
    }
    return options;
}

RuntimeOptions resolve_runtime_options(int argc, char **argv) {
    RuntimeOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        if (arg == "--smoke") {
            options.smoke_mode = true;
            options.offscreen_mode = true;
            options.deterministic_ui = true;
        } else if (arg == "--offscreen") {
            options.offscreen_mode = true;
            options.deterministic_ui = true;
        } else if (arg == "--ui-test") {
            options.ui_test_mode = true;
            options.offscreen_mode = true;
        }
    }

    if (env_truthy("APP_UI_TEST_MODE")) {
        options.ui_test_mode = true;
        options.offscreen_mode = true;
    }
    if (env_truthy("APP_SMOKE_MODE")) {
        options.smoke_mode = true;
        options.offscreen_mode = true;
        options.deterministic_ui = true;
    }

    const auto fixture = env_path("APP_UI_TEST_FIXTURE");
    if (!fixture.empty()) {
        options.fixture_path = fixture;
    }

    return normalize_runtime_options(options);
}

} // namespace lingtai::desktop
