// The Autoclicker card on the Hacks page (declared in ui/pages.hpp): on/off
// and its key, "only while holding", each player's rhythm, a one-shot copy of
// Player 1's rhythm onto Player 2, and what the clicker is waiting for. Every
// change is saved at once (Autoclicker::saveSettings, hacks/autoclicker.cpp).

#include "ui/pages.hpp"

#include "core/GucciBot.hpp"
#include "hacks/autoclicker.hpp"
#include "ui/kit.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

using namespace geode::prelude;

namespace gucci::ui::pages {

    namespace {

        using Settings = Autoclicker::PlayerSettings;

        // A count of ticks or clicks: typed, or stepped with the buttons.
        bool countRow(const char* label, const char* hint, int* value, int most) {
            kit::RowBegin(label, hint);
            bool const changed = kit::InputInt("##count", value, 1);
            kit::RowEnd();
            if (changed)
                *value = std::clamp(*value, 1, most);
            return changed;
        }

        // "About 120 clicks a second at 240 TPS" for the rhythm as it is set.
        std::string clickRate(Settings const& s) {
            double const tps = GucciEngine::get()->updater.getTps();
            int const cycle = s.swiftClicks ? s.releaseTicks : s.holdTicks + s.releaseTicks;
            if (tps <= 0.0 || cycle <= 0)
                return {};
            double const perSecond = tps * s.clicksPerHold / cycle;
            return fmt::format("About {:g} clicks a second at {:g} TPS", std::round(perSecond * 10.0) / 10.0, tps);
        }

        // One player's rows under its switch. True when something changed.
        bool rhythmRows(Settings& s) {
            bool changed = kit::SwitchRow(
                "Black orb UFO loop",
                s.blackOrbUfo ? "Absent's spam: tap + hold, release + tap, tap + hold two ticks, release + tap. Made for 720 TPS"
                              : "A fixed five-tick loop in place of the rhythm below",
                &s.blackOrbUfo);
            if (s.blackOrbUfo)
                return changed;
            if (!s.swiftClicks)
                changed |= countRow("Hold ticks", "How long each click stays down", &s.holdTicks, Autoclicker::kMaxTicks);
            changed |= countRow("Release ticks", "How long it stays up before the next click", &s.releaseTicks,
                                Autoclicker::kMaxTicks);
            changed |= countRow("Clicks per hold", "Above 1: more clicks on the same tick", &s.clicksPerHold,
                                Autoclicker::kMaxClicksPerHold);
            changed |= kit::SwitchRow("Swift clicks", "Let go on the tick it presses", &s.swiftClicks);
            std::string const rate = clickRate(s);
            if (!rate.empty())
                kit::Hint(rate.c_str());
            return changed;
        }

        // A player's switch and, while it is on, its rows. Each player gets its
        // own id scope, since both use the same row labels.
        bool playerFeature(const char* id, const char* label, const char* hint, Settings& s) {
            ImGui::PushID(id);
            bool changed = false;
            if (kit::FeatureBegin(label, hint, &s.enabled, nullptr, &changed)) {
                changed |= rhythmRows(s);
                kit::FeatureEnd();
            }
            ImGui::PopID();
            return changed;
        }

        void statusRow(Autoclicker const& ac) {
            kit::RowBegin("Status", nullptr);
            if (const char* why = ac.waitingFor()) {
                std::string text = why;
                text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
                kit::Chip(text.c_str(), Tone::Accent);
            } else {
                kit::Chip("Clicking", Tone::Good);
            }
            kit::RowEnd();
        }

    } // namespace

    void autoclickerCard() {
        auto* ac = Autoclicker::get();
        bool changed = false;
        kit::BeginCard("Autoclicker", "Clicks jump for you on a rhythm counted in ticks.");
        if (kit::FeatureBegin("On", "While recording, its clicks go into the macro", &ac->enabled,
                              &keys().onAutoclicker, &changed)) {
            statusRow(*ac);
            changed |= kit::SwitchRow("Only while holding", "Clicks only while you hold Space, W or Up",
                                      &ac->onlyWhileHolding);
            changed |= playerFeature("p1", "Player 1", nullptr, ac->p1);
            changed |= playerFeature(
                "p2", "Player 2",
                "Two-player levels only: in any other level the game gives Player 2's input to Player 1", ac->p2);
            kit::RowBegin("Same as Player 1", "A one-time copy: change either side after");
            if (kit::Button("Copy to Player 2", Tone::Plain, -1.f)) {
                ac->syncP2FromP1();
                changed = true;
            }
            kit::RowEnd();
            kit::FeatureEnd();
        }
        if (changed)
            ac->saveSettings();
        kit::EndCard();
    }

} // namespace gucci::ui::pages
