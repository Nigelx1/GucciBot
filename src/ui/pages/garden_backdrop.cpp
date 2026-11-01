// RondoBot's scene (Nigel, 2026-10-31): the TD Garden's parquet behind the
// menu, a hint of floor in the sidebar and the gaps between the cards.
// Squares of five planks, running across in one square and down in the
// next, every other plank a shade darker, at an alpha low enough that the
// menu still reads white (the first one washed it tan) and the text stays
// crisp; and a thin line of Celtics gold along the bottom edge and down the
// sidebar's inner edge, like a court's boundary.
//
// Drawn the way the JMF sky is (trainer_pages.cpp jupiterBackdrop): on the
// menu window's draw list before its children, so the cards stand in front;
// colours with their own alpha; inside the window's own clip, which keeps it
// off the rounded corners. A floor holds still, so Animations doesn't touch
// it. Written fresh.

#include "ui/look.hpp"
#include "ui/pages.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace gucci::ui::pages {

    namespace {

        ImVec4 const kPlankLight{0.886f, 0.780f, 0.588f, 1.f};   // #E2C796
        ImVec4 const kPlankDark{0.788f, 0.643f, 0.416f, 1.f};    // #C9A46A
        ImVec4 const kSeam{0.690f, 0.541f, 0.322f, 1.f};         // #B08A52
        ImVec4 const kCelticsGold{0.729f, 0.588f, 0.325f, 1.f};  // #BA9653

        ImU32 col(ImVec4 const& c, float alpha) {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, alpha));
        }

    } // namespace

    void gardenBackdrop() {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiStyle const& style = ImGui::GetStyle();
        ImVec2 const a = ImGui::GetWindowPos();
        ImVec2 const size = ImGui::GetWindowSize();
        ImVec2 const b(a.x + size.x, a.y + size.y);
        dl->PushClipRect(a, b, true);

        // The wood's own tone, then the squares: about eleven down the
        // window's short side, never smaller than 40 units.
        dl->AddRectFilled(a, b, col(kPlankLight, 0.05f), style.WindowRounding);
        float const square = std::max(40.f, std::min(size.x, size.y) / 11.f);
        float const plank = square / 5.f;
        int const columns = static_cast<int>(std::ceil(size.x / square));
        int const rows = static_cast<int>(std::ceil(size.y / square));
        for (int j = 0; j < rows; ++j) {
            for (int i = 0; i < columns; ++i) {
                ImVec2 const s0(a.x + square * static_cast<float>(i), a.y + square * static_cast<float>(j));
                bool const across = (i + j) % 2 == 0;  // the planks run across this square, down its neighbours
                if (!across)
                    dl->AddRectFilled(s0, ImVec2(s0.x + square, s0.y + square), col(kPlankDark, 0.03f));
                for (int k : {1, 3}) {
                    float const o = plank * static_cast<float>(k);
                    ImVec2 const p0 = across ? ImVec2(s0.x, s0.y + o) : ImVec2(s0.x + o, s0.y);
                    ImVec2 const p1 = across ? ImVec2(s0.x + square, s0.y + o + plank)
                                             : ImVec2(s0.x + o + plank, s0.y + square);
                    dl->AddRectFilled(p0, p1, col(kPlankDark, 0.045f));
                }
            }
        }
        for (int i = 1; i < columns; ++i) {
            float const x = a.x + square * static_cast<float>(i);
            dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), col(kSeam, 0.09f), 1.f);
        }
        for (int j = 1; j < rows; ++j) {
            float const y = a.y + square * static_cast<float>(j);
            dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), col(kSeam, 0.09f), 1.f);
        }

        // Celtics gold: along the bottom edge, clear of the rounded corners,
        // and down the middle of the gap between the sidebar and the page to
        // meet it.
        float const round = style.WindowRounding;
        dl->AddRectFilled(ImVec2(a.x + round, b.y - 6.f), ImVec2(b.x - round, b.y - 3.f), col(kCelticsGold, 0.9f));
        float const sideX = a.x + style.WindowPadding.x + kSidebarWidth + style.ItemSpacing.x * 0.5f;
        dl->AddRectFilled(ImVec2(sideX - 1.f, a.y + style.WindowPadding.y), ImVec2(sideX + 1.f, b.y - 6.f),
                          col(kCelticsGold, 0.9f));
        dl->PopClipRect();
    }

} // namespace gucci::ui::pages
