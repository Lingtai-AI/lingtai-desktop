#pragma once

namespace lingtai::desktop {

// Explicit runtime knobs for deterministic UI construction. The default values
// preserve normal product behavior; tests and smoke/offscreen entrypoints can
// opt in without relying on hidden shell state.
struct RuntimeOptions {
    bool offscreen_mode = false;
    bool smoke_mode = false;
    bool deterministic_ui = false;
};

} // namespace lingtai::desktop
