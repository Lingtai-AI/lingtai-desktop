#pragma once

#include <filesystem>

namespace lingtai::desktop {

// Explicit runtime knobs for deterministic UI construction. The default values
// preserve normal product behavior; tests and smoke/offscreen entrypoints can
// opt in without relying on hidden shell state.
struct RuntimeOptions {
    bool offscreen_mode = false;
    bool smoke_mode = false;
    // Minimal widget tree for narrow headless contract tests only — not for
    // visual baselines or full-shell UI tests.
    bool deterministic_ui = false;
    // Central UI-test mode: disables periodic refresh timers and other
    // background behavior that makes functional/visual tests flaky.
    bool ui_test_mode = false;
    bool animations_enabled = true;
    bool network_enabled = true;
    // Optional committed or generated fixture opened automatically when the
    // product entrypoint runs with --ui-test.
    std::filesystem::path fixture_path;
};

// Parses `--ui-test`, `APP_UI_TEST_MODE`, `APP_UI_TEST_FIXTURE`, and related
// environment variables. Does not read argv beyond known flags.
[[nodiscard]] RuntimeOptions resolve_runtime_options(int argc, char **argv);

// Applies ui_test_mode side effects (default animation/network off).
[[nodiscard]] RuntimeOptions normalize_runtime_options(RuntimeOptions options);

} // namespace lingtai::desktop
