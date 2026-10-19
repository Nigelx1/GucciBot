// Copy a macro with one player-1 jump input moved by whole frames, to check a
// Calculate window in the real game: if the click really has a 1-frame window,
// both the -1 and the +1 copy die and the original finishes. Play the copies
// with tools/calc_check.py over Assistant Access.
//
//   shift_click IN.brrr OUT.brrr FRAME PRESS(1|0) SHIFT
//
// Build (from a VS x64 prompt, in the repo root, as one line):
//   cl /nologo /std:c++20 /EHsc /O2 /MD /I src tools/shift_click.cpp
//      src/tools/macro_ops.cpp src/core/gbr6_format.cpp /Fe:build/shift_click.exe /Fo:build/
#include "tools/macro_ops.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace gucci;
using namespace gucci::macroops;

int main(int argc, char** argv) {
    if (argc < 6) {
        std::puts("usage: shift_click IN OUT FRAME PRESS(1|0) SHIFT");
        return 2;
    }
    std::string err;
    auto m = load(argv[1], &err);
    if (!m) {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }
    uint32_t const frame = (uint32_t)std::atoi(argv[3]);
    bool const press = std::atoi(argv[4]) != 0;
    int const shift = std::atoi(argv[5]);
    int hit = -1;
    for (size_t i = 0; i < m->actions.size(); i++) {
        auto const& a = m->actions[i];
        if (a.m_frame == frame && a.m_holding == press && !a.m_player2 && a.m_type == gb::ActionType::Jump) {
            hit = (int)i;
            break;
        }
    }
    if (hit < 0) {
        std::puts("no such input");
        return 1;
    }
    int const prev = hit > 0 ? (int)m->actions[hit - 1].m_frame : -1;
    int const next = hit + 1 < (int)m->actions.size() ? (int)m->actions[hit + 1].m_frame : -1;
    std::printf("input %d of %zu: frame %u %s subtick %.3f; neighbours %d and %d\n", hit + 1, m->actions.size(), frame,
                press ? "press" : "release", m->actions[hit].m_subtick, prev, next);
    m->actions[hit].m_frame = (uint32_t)((int)frame + shift);
    std::stable_sort(m->actions.begin(), m->actions.end(),
                     [](gb::Action const& a, gb::Action const& b) { return a.m_frame < b.m_frame; });
    if (!save(*m, argv[2], &err)) {
        std::printf("save failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("wrote %s with that input at %u\n", argv[2], (unsigned)((int)frame + shift));
    return 0;
}
