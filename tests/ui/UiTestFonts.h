#pragma once

namespace lingtai::desktop::ui_test {

// Load bundled Open Sans (when present) and mirror main.cpp fallbacks so UI
// and visual tests render with the same font stack as the desktop app.
void applyUiTestFontDefaults();

} // namespace lingtai::desktop::ui_test
