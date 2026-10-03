#pragma once

// Interface-internal state shared between the shell, the kit and the pages.
// Not for use outside src/ui/ (that is what ui.hpp is for).

namespace gucci::ui::detail {

    // The key-capture control currently waiting for a key (nullptr: none).
    int* captureTarget();
    void setCaptureTarget(int* target);
    // True once, on the frame after the keyboard hook stored a key into
    // `target` (so the control can report the change).
    bool takeCaptured(int* target);

} // namespace gucci::ui::detail
