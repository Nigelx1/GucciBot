#include "shim.hpp"

#include "framewindow.hpp"

// The one instance of anticroom's analyzer. Silicate holds this as a member on
// Bot; GucciEngine can't, because of the include direction (see the comment on
// Bot::frameWindow in shim.hpp), so it lives here instead. Same lifetime in
// practice: constructed on first use, alive for the process.
//
// Nothing drives it yet beyond tick(). Until GucciBot calls start(), running()
// stays false and every gate that asks about it answers no, so its presence
// costs nothing.
FrameWindowAnalyzer& Bot::frameWindow() {
    static FrameWindowAnalyzer inst;
    return inst;
}
