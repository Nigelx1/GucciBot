#include "gui/gui.hpp"
#include "core/GucciBot.hpp"
#include "audio/clicksounds.hpp"
#include "hacks/autoclicker.hpp"
#include "analysis/pathfinder.hpp"
#include "trainers/calibration.hpp"
#include "audio/bigbrrr.hpp"
#include "trainers/jupiterghost.hpp"
#include "trainers/trainerghost.hpp"

#include "legacy_renderer.hpp"
#include "render/renderer.hpp"
#include "tools/selfcheck.hpp"
#include <Geode/Bindings.hpp>
#include <Geode/cocos/textures/CCTexture2D.h>
#include <Geode/modify/LoadingLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/Task.hpp>
#include <fmt/format.h>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <regex>
#include <system_error>
#include <vector>
#include <unordered_map>
#include <climits>
using namespace geode::prelude;

namespace gucci {

    static ImVec4 lerpColor(const ImVec4& a, const ImVec4& b, float t) {
        return ImVec4(a.x + (b.x - a.x) * t,
                      a.y + (b.y - a.y) * t,
                      a.z + (b.z - a.z) * t,
                      a.w + (b.w - a.w) * t);
    }
    static float smoothStep(float cur, float tgt, float spd, float dt) {
        return cur + (tgt - cur) * std::min(1.f, dt * spd);
    }
    static ImVec4 withAlpha(ImVec4 c, float a) {
        c.w = a;
        return c;
    }
    static ImVec4 brighten(const ImVec4& c, float amt) {
        return ImVec4(std::clamp(c.x + amt, 0.f, 1.f),
                      std::clamp(c.y + amt, 0.f, 1.f),
                      std::clamp(c.z + amt, 0.f, 1.f),
                      c.w);
    }
    static ImU32 toU32(const ImVec4& c) {
        return ImGui::ColorConvertFloat4ToU32(c);
    }
    static ImVec2 snapPos(ImVec2 p) {
        return ImVec2(std::round(p.x), std::round(p.y));
    }
    static float frand(float lo, float hi) {
        return lo + (hi - lo) * ((float)std::rand() / (float)RAND_MAX);
    }

    struct JupiterSegment {
        std::string label;
        float x = 0.f;
        std::string note;
    };

    static std::string jupEscapeField(std::string s) {
        std::string out;
        for (char c : s) {
            if (c == ',')
                out += "&#44;";
            else if (c == ';')
                out += "&#59;";
            else
                out += c;
        }
        return out;
    }
    static std::string jupUnescapeField(std::string s) {
        auto replaceAll = [](std::string& str, const std::string& from, const std::string& to) {
            size_t p = 0;
            while ((p = str.find(from, p)) != std::string::npos) {
                str.replace(p, from.size(), to);
                p += to.size();
            }
        };
        replaceAll(s, "&#44;", ",");
        replaceAll(s, "&#59;", ";");
        return s;
    }

    static std::vector<JupiterSegment> parseJupiterSegments(std::string const& raw) {
        std::vector<JupiterSegment> segs;
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t semi = raw.find(';', pos);
            std::string entry =
                raw.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
            JupiterSegment seg;
            size_t cLast = entry.rfind(',');
            if (cLast != std::string::npos) {
                std::string beforeLast = entry.substr(0, cLast);
                std::string lastTok = entry.substr(cLast + 1);
                size_t cPrev = beforeLast.rfind(',');
                bool parsedNew = false;
                if (cPrev != std::string::npos) {
                    std::string xTok = beforeLast.substr(cPrev + 1);
                    char* endp = nullptr;
                    float xv = std::strtof(xTok.c_str(), &endp);
                    if (endp && *endp == '\0' && endp != xTok.c_str()) {
                        seg.label = beforeLast.substr(0, cPrev);
                        seg.x = xv;
                        seg.note = jupUnescapeField(lastTok);
                        parsedNew = true;
                    }
                }
                if (!parsedNew) {
                    seg.label = beforeLast;
                    try {
                        seg.x = std::stof(lastTok);
                    } catch (...) {
                    }
                }
                segs.push_back(seg);
            }
            if (semi == std::string::npos)
                break;
            pos = semi + 1;
        }
        return segs;
    }

    static std::string serializeJupiterSegments(std::vector<JupiterSegment> const& segs) {
        std::string out;
        for (size_t i = 0; i < segs.size(); i++) {
            if (i)
                out += ";";
            out += segs[i].label + "," + std::to_string(segs[i].x) + "," +
                   jupEscapeField(segs[i].note);
        }
        return out;
    }

    static const char kB64Chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static std::string base64Encode(std::string const& in) {
        std::string out;
        int val = 0, valb = -6;
        for (unsigned char c : in) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                out.push_back(kB64Chars[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6)
            out.push_back(kB64Chars[((val << 8) >> (valb + 8)) & 0x3F]);
        while (out.size() % 4)
            out.push_back('=');
        return out;
    }
    static std::string base64Decode(std::string const& in) {
        int T[256];
        std::fill(std::begin(T), std::end(T), -1);
        for (int i = 0; i < 64; i++)
            T[(unsigned char)kB64Chars[i]] = i;
        std::string out;
        int val = 0, valb = -8;
        for (unsigned char c : in) {
            if (T[c] == -1)
                continue;
            val = (val << 6) + T[c];
            valb += 6;
            if (valb >= 0) {
                out.push_back((char)((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return out;
    }

    static std::string exportSegmentsCode(std::string const& segmentsRaw,
                                          std::string const& notes) {
        std::string blob = segmentsRaw + "\x1F" + notes;
        return "JMF1:" + base64Encode(blob);
    }
    static bool importSegmentsCode(std::string const& code,
                                   std::string& outSegmentsRaw,
                                   std::string& outNotes,
                                   std::string& err) {
        static const std::string kPrefix = "JMF1:";
        if (code.compare(0, kPrefix.size(), kPrefix) != 0) {
            err = "Not a valid segment code.";
            return false;
        }
        std::string blob = base64Decode(code.substr(kPrefix.size()));
        size_t sep = blob.find('\x1F');
        if (sep == std::string::npos) {
            err = "Corrupted code.";
            return false;
        }
        outSegmentsRaw = blob.substr(0, sep);
        outNotes = blob.substr(sep + 1);
        return true;
    }

    static std::vector<JupiterSegment>
    suggestSegmentsFromClickDensity(std::vector<std::pair<double, double>> const& clickIntervalsSec,
                                    std::vector<MacroPathSample> const& pathSamples,
                                    double clickBarTps,
                                    std::string const& existingSegmentsRaw) {
        std::vector<JupiterSegment> out;
        if (clickIntervalsSec.empty() || pathSamples.empty())
            return out;
        double tps = clickBarTps > 0.0 ? clickBarTps : 240.0;

        double maxT = 0.0;
        for (auto const& iv : clickIntervalsSec)
            maxT = std::max(maxT, iv.second);
        if (maxT <= 0.0)
            return out;

        const double bucketSec = 0.5;
        int nBuckets = (int)(maxT / bucketSec) + 1;
        std::vector<int> counts(nBuckets, 0);
        for (auto const& iv : clickIntervalsSec) {
            int b = std::clamp((int)(iv.first / bucketSec), 0, nBuckets - 1);
            counts[b]++;
        }
        double meanCount = 0.0;
        for (int c : counts)
            meanCount += c;
        meanCount /= std::max(1, nBuckets);

        struct Cand {
            int bucket;
            int count;
        };
        std::vector<Cand> cands;
        for (int b = 0; b < nBuckets; b++)
            if (counts[b] >= 3 && (double)counts[b] > meanCount * 2.0)
                cands.push_back({b, counts[b]});
        std::sort(cands.begin(), cands.end(), [](Cand const& a, Cand const& b) {
            return a.count > b.count;
        });
        if (cands.size() > 5)
            cands.resize(5);

        auto existing = parseJupiterSegments(existingSegmentsRaw);
        for (auto const& c : cands) {
            double midSec = (c.bucket + 0.5) * bucketSec;
            uint32_t frame = (uint32_t)(midSec * tps);
            if (frame >= pathSamples.size())
                continue;
            float x = pathSamples[frame].p1x;
            bool dup = false;
            for (auto const& s : existing)
                if (std::fabs(s.x - x) < 50.f) {
                    dup = true;
                    break;
                }
            for (auto const& s : out)
                if (std::fabs(s.x - x) < 50.f) {
                    dup = true;
                    break;
                }
            if (dup)
                continue;
            JupiterSegment seg;
            seg.label = "Auto: dense clicks (" + std::to_string(c.count) + "/500ms)";
            seg.x = x;
            out.push_back(seg);
        }
        return out;
    }

    static void applyBigBrrrBounce(bool jupiterActive) {
        static float restY = 0.f;
        static float offset = 0.f;
        static bool active = false;
        static double beatRefTime = 0.0;

        if (jupiterActive) {
            active = false;
            offset = 0.f;
            return;
        }

        bool on = BigBrrrManager::get()->enabled;
        if (on) {
            if (!active) {
                restY = ImGui::GetWindowPos().y - offset;
                active = true;
                beatRefTime = ImGui::GetTime() - BigBrrrManager::kStartOffsetSec();
            }
            double elapsed = ImGui::GetTime() - beatRefTime;
            double omega = 2.0 * 3.14159265358979 * BigBrrrManager::kBpm() / 60.0;
            offset = (float)(std::sin(elapsed * omega) * 10.0);
        } else if (active) {
            offset *= 0.75f;
            if (std::fabs(offset) < 0.05f) {
                offset = 0.f;
                active = false;
            }
        } else {
            return;
        }

        ImVec2 wp = ImGui::GetWindowPos();
        ImGui::SetWindowPos(ImVec2(wp.x, restY + offset));
    }

    static float bigBrrrFlickerAlpha(bool jupiterActive) {
        static float smoothed = 0.f;
        auto* brrr = BigBrrrManager::get();
        bool on = !jupiterActive && brrr->enabled && brrr->shakeEnabled;
        float target = on ? brrr->getBassLevel() : 0.f;
        float rate = (target > smoothed) ? 40.f : 6.f;
        smoothed += (target - smoothed) * std::min(1.f, rate * ImGui::GetIO().DeltaTime);
        return 1.f - smoothed * smoothed * brrr->flickerIntensity;
    }

    static void applyBigBrrrShake(bool jupiterActive) {
        static float smoothed = 0.f;
        static float anchorX = 0.f, anchorY = 0.f;
        static bool active = false;
        // Anchor is captured once when a shake episode starts, never read back
        // from GetWindowPos() mid-episode. Reading position back and
        // subtracting the last offset round-trips it through ImGui's internal
        // storage every frame; if that storage quantizes to whole pixels, each
        // round-trip loses a small consistently-signed fraction and the window
        // drifts steadily in one direction over time. Don't reintroduce a
        // per-frame GetWindowPos() read here.
        auto* brrr = BigBrrrManager::get();
        bool on = !jupiterActive && brrr->enabled && brrr->shakeEnabled;
        float target = on ? brrr->getBassLevel() : 0.f;
        float rate = (target > smoothed) ? 40.f : 6.f;
        smoothed += (target - smoothed) * std::min(1.f, rate * ImGui::GetIO().DeltaTime);

        if (smoothed <= 0.001f) {
            if (active) {
                ImGui::SetWindowPos(ImVec2(anchorX, anchorY));
                active = false;
            }
            return;
        }

        if (!active) {
            ImVec2 wp = ImGui::GetWindowPos();
            anchorX = wp.x;
            anchorY = wp.y;
            active = true;
        }

        float amp = smoothed * smoothed * 28.f;
        float sx = ((float)(rand() % 2001) / 1000.f - 1.f) * amp;
        float sy = ((float)(rand() % 2001) / 1000.f - 1.f) * amp;
        ImGui::SetWindowPos(ImVec2(anchorX + sx, anchorY + sy));
    }

    static const char* getAccuracyTag(AccuracyMode m) {
        switch (m) {
        case AccuracyMode::CBS:
            return "CBS";
        case AccuracyMode::CBF:
            return "CBF";
        default:
            return nullptr;
        }
    }
    static ImVec4 getAccuracyTagColor(AccuracyMode m) {
        switch (m) {
        case AccuracyMode::CBS:
        case AccuracyMode::CBF:
            return ImVec4(1.f, 0.22f, 0.22f, 1.f);
        default:
            return ImVec4(1, 1, 1, 1);
        }
    }
    static ImVec4 getBRRTagColor() {
        return ImVec4(0.30f, 0.70f, 1.0f, 1.0f);
    }

    std::string currentThemeExtension(MenuInterface* ui) {
        // CustomTheme::extension is always stored bare (alnum only, no dot --
        // see deriveCustomThemeExtension/sanitizeCustomExtension), but every
        // built-in branch below returns a dot-prefixed extension and callers
        // (both here and getThemeExtension() in brr_format.cpp) append this
        // return value directly after a macro name expecting the dot to
        // already be there. Returning it bare produced filenames like
        // "MacroNamemytheme" with no extension at all, which then couldn't
        // match anything in allKnownMacroExtensions()'s dot-prefixed list on
        // the next directory scan -- the macro silently vanished from every
        // macro list after saving under a custom theme (reported by
        // anticroom, 2026-09-08).
        if (auto* c = ui->getActiveCustomTheme())
            return "." + c->extension;
        switch (ui->activeTheme) {
        case THEME_TOOSII:
        case THEME_TOOSII_SYRACUSE:
        case THEME_TOOSII_SACSTATE:
            return ".toosii";
        case THEME_JA:
            return ".ja";
        case THEME_GIDDEY:
            return ".giddey";
        case THEME_BAM:
            return ".bam";
        case THEME_SEXYY:
            return ".sexyy";
        case THEME_JUICE:
            return ".juice";
        case THEME_BUTLER:
            return ".butler";
        case THEME_SAWEETIE:
            return ".saweetie";
        case THEME_MAYBACH:
            return ".maybach";
        case THEME_ROMO:
            return ".romo";
        case THEME_GRIZZLEY:
            return ".grizzley";
        case THEME_REDKINGDOM:
            return ".redkingdom";
        case THEME_LEMONADE:
            return ".lemonade";
        case THEME_BRRR:
            return ".icebrrr";
        case THEME_WAKA:
            return ".waka";
        case THEME_YOUNGSTA:
            return ".youngsta";
        case THEME_KNOCKERZ:
            return ".knockerz";
        default:
            return ".brrr";
        }
    }

    static float sanitizeClamped(float v, float lo, float hi, float fb) {
        if (!std::isfinite(v))
            return fb;
        return std::clamp(v, lo, hi);
    }
    static ImVec4 sanitizeColor(ImVec4 v, ImVec4 fb) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
            !std::isfinite(v.w))
            return fb;
        float mx = std::max({v.x, v.y, v.z, v.w});
        if (mx > 1.0001f && mx <= 255.f) {
            v.x /= 255.f;
            v.y /= 255.f;
            v.z /= 255.f;
            v.w /= 255.f;
        }
        v.x = std::clamp(v.x, 0.f, 1.f);
        v.y = std::clamp(v.y, 0.f, 1.f);
        v.z = std::clamp(v.z, 0.f, 1.f);
        v.w = std::clamp(v.w, 0.f, 1.f);
        return v;
    }

    template <class T>
    static T loadSV(Mod* mod,
                    std::string_view key,
                    T def,
                    std::initializer_list<std::string_view> legacy = {}) {
        if (mod->hasSavedValue(std::string(key)))
            return mod->getSavedValue<T>(std::string(key), def);
        for (auto lk : legacy)
            if (mod->hasSavedValue(std::string(lk)))
                return mod->getSavedValue<T>(std::string(lk), def);
        return def;
    }

    static void drawSolidRect(ImDrawList* dl,
                              ImVec2 mn,
                              ImVec2 mx,
                              float r,
                              const ThemeEngine& t,
                              float a,
                              bool border = true) {
        ImVec4 fill(t.cardColor.x, t.cardColor.y, t.cardColor.z, t.cardColor.w * a);
        dl->AddRectFilled(mn, mx, toU32(fill), r);
        if (border)
            dl->AddRect(mn, mx, t.getAccentU32(0.18f * a), r, 0, 1.f);
    }

    static std::vector<ImVec2>
    jupiterStarPoints(ImVec2 center, float outerR, float innerR, int points, float rotRad) {
        std::vector<ImVec2> pts;
        int total = points * 2;
        for (int i = 0; i < total; i++) {
            float r = (i % 2 == 0) ? outerR : innerR;
            float a = rotRad + (float)i / (float)total * 2.0f * 3.14159265f;
            pts.push_back(ImVec2(center.x + r * cosf(a), center.y + r * sinf(a)));
        }
        return pts;
    }

    static void
    drawJupiterOrnament(ImDrawList* dl, ImVec2 center, float baseR, float time, float spin) {
        const ImU32 gold = IM_COL32(252, 245, 80, 255);
        const float PI = 3.14159265f;

        const int nRings = 4;
        float ringR[nRings];
        for (int i = 0; i < nRings; i++) {
            ringR[i] = baseR * (0.34f + 0.22f * (float)i);
            const int segs = 48;
            std::vector<ImVec2> arc(segs + 1);
            for (int s = 0; s <= segs; s++) {
                float a = PI + (float)s / (float)segs * PI;
                arc[s] = ImVec2(center.x + ringR[i] * cosf(a), center.y + ringR[i] * sinf(a));
            }
            dl->AddPolyline(arc.data(), segs + 1, gold, 0, 3.2f);
        }

        for (int i = 0; i < nRings; i++) {
            int count = 6 + i * 3;
            for (int d = 1; d < count; d++) {
                float a = PI + (float)d / (float)count * PI;
                ImVec2 p(center.x + ringR[i] * cosf(a), center.y + ringR[i] * sinf(a));
                if (d % 2 == 1)
                    dl->AddCircleFilled(p, 5.f, gold, 12);
                else {
                    ImVec2 t0(center.x + (ringR[i] - 6.f) * cosf(a),
                              center.y + (ringR[i] - 6.f) * sinf(a));
                    ImVec2 t1(center.x + (ringR[i] + 6.f) * cosf(a),
                              center.y + (ringR[i] + 6.f) * sinf(a));
                    dl->AddLine(t0, t1, gold, 2.4f);
                }
            }
        }

        float coreR = baseR * 0.14f;
        int nRays = 11;
        for (int i = 0; i <= nRays; i++) {
            float a = PI + (float)i / (float)nRays * PI + time * spin * 0.15f;
            float rayLen = ringR[0] * 1.05f;
            float halfW = coreR * 0.32f;
            float pa = a + PI * 0.5f;
            ImVec2 tip(center.x + rayLen * cosf(a), center.y + rayLen * sinf(a));
            ImVec2 base1(center.x + halfW * cosf(pa), center.y + halfW * sinf(pa));
            ImVec2 base2(center.x - halfW * cosf(pa), center.y - halfW * sinf(pa));
            dl->AddTriangleFilled(base1, base2, tip, gold);
        }
        dl->AddCircleFilled(center, coreR, gold, 48);
    }

    static void drawJupiterWaveRibbon(ImDrawList* dl, ImVec2 pos, ImVec2 size) {
        const ImU32 gold = IM_COL32(252, 245, 80, 255);

        struct Pt {
            float x, y;
        };
        static const Pt spine[] = {
            {0.38f, 0.00f},
            {0.50f, 0.33f},
            {0.36f, 0.66f},
            {0.48f, 1.00f},
        };
        const int n = (int)(sizeof(spine) / sizeof(spine[0]));
        const float offsetX = size.x * 0.08f;

        std::vector<ImVec2> orig(n), copy(n);
        for (int i = 0; i < n; i++) {
            orig[i] = ImVec2(pos.x + size.x * spine[i].x, pos.y + size.y * spine[i].y);
            copy[i] = ImVec2(orig[i].x + offsetX, orig[i].y);
        }
        dl->AddPolyline(orig.data(), n, gold, 0, 6.f);
        dl->AddPolyline(copy.data(), n, gold, 0, 6.f);

        std::vector<float> segLen(n - 1);
        float totalLen = 0.f;
        for (int i = 0; i < n - 1; i++) {
            float dx = orig[i + 1].x - orig[i].x, dy = orig[i + 1].y - orig[i].y;
            segLen[i] = sqrtf(dx * dx + dy * dy);
            totalLen += segLen[i];
        }

        const float step = 13.f;
        int seg = 0;
        float segPos = 0.f;
        for (float dist = 0.f; dist < totalLen; dist += step, segPos += step) {
            while (seg < n - 2 && segPos > segLen[seg]) {
                segPos -= segLen[seg];
                seg++;
            }
            float segL = segLen[seg] > 0.001f ? segLen[seg] : 0.001f;
            float t = segPos / segL;
            ImVec2 a = orig[seg], b = orig[seg + 1];
            ImVec2 pO(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
            ImVec2 pC(pO.x + offsetX, pO.y);
            dl->AddLine(pO, pC, gold, 2.6f);
        }
    }

    static void drawJupiterBackdrop(ImDrawList* dl, ImVec2 pos, ImVec2 size, float time) {
        drawJupiterWaveRibbon(dl, pos, size);

        drawJupiterOrnament(dl, ImVec2(pos.x + size.x * 0.93f, pos.y + size.y), 76.f, time, 0.05f);

        struct StarSpec {
            float x, y, r, rot;
        };
        static const StarSpec stars[] = {
            {0.65f, 0.22f, 58.f, 0.3f},
            {0.85f, 0.44f, 52.f, 1.1f},
            {0.66f, 0.60f, 50.f, 0.7f},
            {0.76f, 0.33f, 26.f, 2.0f},
            {0.92f, 0.58f, 22.f, 1.4f},
            {0.57f, 0.70f, 24.f, 0.4f},
            {0.79f, 0.68f, 28.f, 2.6f},
            {0.89f, 0.16f, 24.f, 0.9f},
            {0.60f, 0.40f, 20.f, 1.6f},
        };
        for (auto const& s : stars) {
            ImVec2 c(pos.x + size.x * s.x, pos.y + size.y * s.y);
            auto pts = jupiterStarPoints(c, s.r, s.r * 0.42f, 5, s.rot);
            dl->AddPolyline(
                pts.data(), (int)pts.size(), IM_COL32(252, 245, 80, 255), ImDrawFlags_Closed, 3.5f);
            std::vector<ImVec2> pent;
            for (int k = 1; k < (int)pts.size(); k += 2)
                pent.push_back(pts[k]);
            dl->AddPolyline(pent.data(),
                            (int)pent.size(),
                            IM_COL32(252, 245, 80, 255),
                            ImDrawFlags_Closed,
                            2.2f);
        }
    }

    namespace {
        static std::filesystem::path getReplayDir() {
            return GucciEngine::get()->getReplayDir();
        }

        // .fw/.path/.trainer sidecars are named after a macro's own full
        // filename (see sidecarPath() in engine_core.cpp), so a rename or
        // delete of the macro itself needs to carry its sidecars along --
        // otherwise they're silently orphaned under the old name, which is
        // exactly the kind of clutter Juice's file-organization ask was
        // about in the first place. Handles both the current replays/
        // sidecars/ location and any pre-reorganization file still sitting
        // flat in replays/ (remove()/rename() on a path that doesn't exist
        // just no-ops via the error_code overload, so it's safe to try both
        // unconditionally).
        static void removeSidecarsFor(const std::filesystem::path& dir,
                                      const std::string& fullMacroName) {
            std::error_code ec;
            auto sidecarDir = dir / "sidecars";
            for (const char* ext : {".fw", ".path", ".trainer"}) {
                std::filesystem::remove(sidecarDir / (fullMacroName + ext), ec);
                std::filesystem::remove(dir / (fullMacroName + ext), ec);
            }
        }
        static void renameSidecarsFor(const std::filesystem::path& dir,
                                      const std::string& oldFullName,
                                      const std::string& newFullName) {
            std::error_code ec;
            auto sidecarDir = dir / "sidecars";
            std::filesystem::create_directories(sidecarDir, ec);
            for (const char* ext : {".fw", ".path", ".trainer"}) {
                auto newPath = sidecarDir / (newFullName + ext);
                auto oldInSidecars = sidecarDir / (oldFullName + ext);
                auto oldFlat = dir / (oldFullName + ext);
                if (std::filesystem::exists(oldInSidecars, ec))
                    std::filesystem::rename(oldInSidecars, newPath, ec);
                else if (std::filesystem::exists(oldFlat, ec))
                    std::filesystem::rename(oldFlat, newPath, ec);
            }
        }

        static bool renameStoredReplay(const std::string& oldN,
                                       const std::string& req,
                                       std::string& finalN,
                                       std::string& err) {
            err.clear();
            auto sanitized = req;
            if (sanitized.empty()) {
                err = "Name cannot be empty.";
                return false;
            }
            auto dir = getReplayDir();
            std::error_code ec;
            std::filesystem::path oldPath;
            for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (e.is_regular_file() && e.path().stem().string() == oldN) {
                    oldPath = e.path();
                    break;
                }
            }
            if (oldPath.empty()) {
                err = "Original file not found.";
                return false;
            }
            auto newName = sanitized;
            auto oldFullName = oldPath.filename().string();
            auto newPath = dir / (newName + oldPath.extension().string());
            std::filesystem::rename(oldPath, newPath, ec);
            if (ec) {
                err = "Rename failed: " + ec.message();
                return false;
            }
            renameSidecarsFor(dir, oldFullName, newPath.filename().string());
            finalN = newName;
            return true;
        }

        static bool deleteStoredReplay(const std::string& name, std::string& err) {
            err.clear();
            auto dir = getReplayDir();
            std::error_code ec;
            std::filesystem::path found;
            for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (e.is_regular_file() && e.path().stem().string() == name) {
                    found = e.path();
                    break;
                }
            }
            if (found.empty()) {
                err = "File not found.";
                return false;
            }
            auto fullName = found.filename().string();
            std::filesystem::remove(found, ec);
            if (ec) {
                err = "Delete failed: " + ec.message();
                return false;
            }
            removeSidecarsFor(dir, fullName);
            return true;
        }

        static void drawPopupChrome(MenuInterface& ui,
                                    const char* title,
                                    float rounding = 0.f,
                                    float titleBandH = 28.f) {
            ImVec2 wp = snapPos(ImGui::GetWindowPos()), ws = snapPos(ImGui::GetWindowSize());
            ImDrawList *dl = ImGui::GetWindowDrawList(), *fg = ImGui::GetForegroundDrawList();
            ImVec2 wm(wp.x + ws.x, wp.y + ws.y);
            drawSolidRect(dl, wp, wm, rounding, ui.theme, 0.72f, false);
            fg->AddRect(wp, wm, ui.theme.getAccentU32(0.36f), rounding, 0, 1.f);
            float ty = wp.y + 12.f;
            dl->AddText(ImVec2(wp.x + 14.f, ty), ui.theme.getTextU32(), title);
            float dy = ty + ImGui::GetFontSize() + 10.f;
            dl->AddLine(ImVec2(wp.x + 1.f, dy),
                        ImVec2(wp.x + ws.x - 1.f, dy),
                        ui.theme.getAccentU32(0.30f),
                        1.f);
            ImGui::Dummy(ImVec2(0.f, titleBandH + 8.f));
        }
    } // namespace

    namespace Widgets {
        void GucciQuote(const char* quote, const char* attr, ThemeEngine& theme) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 pos = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            dl->AddRectFilled(pos, ImVec2(pos.x + 3, pos.y + 36), theme.getAccentU32(0.7f), 1.f);
            ImGui::SetCursorScreenPos(ImVec2(pos.x + 10, pos.y + 2));
            ImGui::PushStyleColor(ImGuiCol_Text, theme.getAccent());
            ImGui::TextUnformatted(quote);
            ImGui::PopStyleColor();
            ImGui::SetCursorScreenPos(ImVec2(pos.x + 10, pos.y + 18));
            ImGui::PushStyleColor(ImGuiCol_Text, withAlpha(theme.getAccent(), 0.6f));
            ImGui::TextUnformatted(attr);
            ImGui::PopStyleColor();
            ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + 40));
            ImGui::Dummy(ImVec2(0, 0));
        }
    } // namespace Widgets

    static const ThemePreset kThemePresets[] = {
        {"GucciBot",
         ImVec4(0.788f, 0.659f, 0.298f, 1.f),
         ImVec4(0.051f, 0.051f, 0.051f, 0.96f),
         ImVec4(0.078f, 0.078f, 0.078f, 1.f),
         ImVec4(0.941f, 0.910f, 0.816f, 1.f),
         ImVec4(0.478f, 0.447f, 0.376f, 1.f),
         5.f,
         0.96f},
        {"ToosiiBot (LSU)",
         ImVec4(0.992f, 0.816f, 0.137f, 1.f),
         ImVec4(0.110f, 0.055f, 0.188f, 0.96f),
         ImVec4(0.165f, 0.082f, 0.275f, 1.f),
         ImVec4(0.960f, 0.940f, 0.870f, 1.f),
         ImVec4(0.600f, 0.490f, 0.300f, 1.f),
         5.f,
         0.96f},
        {"ToosiiBot (Syracuse)",
         ImVec4(0.961f, 0.404f, 0.031f, 1.f),
         ImVec4(0.027f, 0.043f, 0.114f, 0.96f),
         ImVec4(0.055f, 0.082f, 0.188f, 1.f),
         ImVec4(0.960f, 0.940f, 0.920f, 1.f),
         ImVec4(0.600f, 0.500f, 0.400f, 1.f),
         5.f,
         0.96f},
        {"ToosiiBot (Sac State)",
         ImVec4(0.918f, 0.878f, 0.820f, 1.f),
         ImVec4(0.016f, 0.188f, 0.094f, 0.96f),
         ImVec4(0.024f, 0.251f, 0.125f, 1.f),
         ImVec4(0.940f, 0.960f, 0.940f, 1.f),
         ImVec4(0.500f, 0.650f, 0.520f, 1.f),
         5.f,
         0.96f},
        {"JaBot",
         ImVec4(0.420f, 0.784f, 0.953f, 1.f),
         ImVec4(0.027f, 0.078f, 0.200f, 0.96f),
         ImVec4(0.047f, 0.118f, 0.275f, 1.f),
         ImVec4(0.920f, 0.950f, 0.980f, 1.f),
         ImVec4(0.400f, 0.540f, 0.720f, 1.f),
         5.f,
         0.96f},
        {"GiddeyBot",
         ImVec4(1.000f, 0.310f, 0.106f, 1.f),
         ImVec4(0.031f, 0.145f, 0.278f, 0.96f),
         ImVec4(0.047f, 0.220f, 0.400f, 1.f),
         ImVec4(0.975f, 0.985f, 0.995f, 1.f),
         ImVec4(0.580f, 0.740f, 0.900f, 1.f),
         5.f,
         0.96f},
        {"BamBot",
         ImVec4(0.878f, 0.067f, 0.153f, 1.f),
         ImVec4(0.047f, 0.027f, 0.071f, 0.96f),
         ImVec4(0.078f, 0.043f, 0.114f, 1.f),
         ImVec4(0.980f, 0.980f, 0.980f, 1.f),
         ImVec4(0.600f, 0.400f, 0.460f, 1.f),
         5.f,
         0.97f},
        {"SexyyBot",
         ImVec4(0.910f, 0.004f, 0.580f, 1.f),
         ImVec4(0.063f, 0.016f, 0.094f, 0.97f),
         ImVec4(0.102f, 0.027f, 0.149f, 1.f),
         ImVec4(0.990f, 0.950f, 0.980f, 1.f),
         ImVec4(0.580f, 0.340f, 0.520f, 1.f),
         5.f,
         0.97f},
        {"JuiceBot",
         ImVec4(0.960f, 0.520f, 0.380f, 1.f),
         ImVec4(0.020f, 0.090f, 0.086f, 0.96f),
         ImVec4(0.035f, 0.130f, 0.122f, 1.f),
         ImVec4(0.980f, 0.960f, 0.940f, 1.f),
         ImVec4(0.625f, 0.565f, 0.478f, 1.f),
         5.f,
         0.96f},
        {"ButlerBot",
         ImVec4(0.808f, 0.067f, 0.255f, 1.f),
         ImVec4(0.035f, 0.020f, 0.024f, 0.96f),
         ImVec4(0.070f, 0.030f, 0.040f, 1.f),
         ImVec4(0.980f, 0.970f, 0.970f, 1.f),
         ImVec4(0.560f, 0.400f, 0.420f, 1.f),
         5.f,
         0.96f},
        {"SaweetieBot",
         ImVec4(1.000f, 0.180f, 0.520f, 1.f),
         ImVec4(0.090f, 0.020f, 0.060f, 0.96f),
         ImVec4(0.140f, 0.035f, 0.095f, 1.f),
         ImVec4(0.990f, 0.960f, 0.980f, 1.f),
         ImVec4(0.620f, 0.400f, 0.520f, 1.f),
         5.f,
         0.96f},
        {"MaybachBot",
         ImVec4(0.780f, 0.780f, 0.800f, 1.f),
         ImVec4(0.035f, 0.035f, 0.038f, 0.97f),
         ImVec4(0.070f, 0.070f, 0.075f, 1.f),
         ImVec4(0.960f, 0.960f, 0.965f, 1.f),
         ImVec4(0.500f, 0.500f, 0.520f, 1.f),
         5.f,
         0.97f},
        {"RomoBot",
         ImVec4(0.760f, 0.800f, 0.850f, 1.f),
         ImVec4(0.020f, 0.055f, 0.110f, 0.96f),
         ImVec4(0.035f, 0.085f, 0.160f, 1.f),
         ImVec4(0.960f, 0.965f, 0.975f, 1.f),
         ImVec4(0.520f, 0.560f, 0.620f, 1.f),
         5.f,
         0.96f},
        {"GrizzleyBot",
         ImVec4(0.870f, 0.090f, 0.070f, 1.f),
         ImVec4(0.040f, 0.038f, 0.036f, 0.96f),
         ImVec4(0.072f, 0.068f, 0.064f, 1.f),
         ImVec4(0.975f, 0.970f, 0.965f, 1.f),
         ImVec4(0.540f, 0.460f, 0.400f, 1.f),
         5.f,
         0.96f},
        {"Red Kingdom",
         ImVec4(0.820f, 0.035f, 0.035f, 1.f),
         ImVec4(0.028f, 0.008f, 0.008f, 1.f),
         ImVec4(0.055f, 0.014f, 0.014f, 1.f),
         ImVec4(0.960f, 0.930f, 0.930f, 1.f),
         ImVec4(0.520f, 0.260f, 0.260f, 1.f),
         0.f,
         1.0f},
        {"LemonadeBot",
         ImVec4(0.980f, 0.851f, 0.145f, 1.f),
         ImVec4(0.090f, 0.075f, 0.020f, 0.96f),
         ImVec4(0.140f, 0.115f, 0.030f, 1.f),
         ImVec4(0.980f, 0.975f, 0.940f, 1.f),
         ImVec4(0.620f, 0.580f, 0.380f, 1.f),
         5.f,
         0.96f},
        {"BrrrBot",
         ImVec4(0.580f, 0.850f, 0.980f, 1.f),
         ImVec4(0.020f, 0.040f, 0.070f, 0.96f),
         ImVec4(0.038f, 0.070f, 0.115f, 1.f),
         ImVec4(0.960f, 0.975f, 0.990f, 1.f),
         ImVec4(0.520f, 0.600f, 0.680f, 1.f),
         5.f,
         0.96f},
        // Nigel's pick, 2026-09-03: Waka Flocka Flame, Blac Youngsta,
        // Speaker Knockerz -- see about.md for full color names. Green,
        // violet, and teal deliberately weren't used by any theme above.
        {"WakaBot",
         ImVec4(0.204f, 0.780f, 0.302f, 1.f),
         ImVec4(0.018f, 0.050f, 0.026f, 0.96f),
         ImVec4(0.033f, 0.088f, 0.046f, 1.f),
         ImVec4(0.955f, 0.980f, 0.955f, 1.f),
         ImVec4(0.460f, 0.600f, 0.480f, 1.f),
         5.f,
         0.96f},
        {"YoungstaBot",
         // Nigel's ask, 2026-09-03: black + red, #000000/#ff0000, to
         // resemble the "223" album cover. bg is literal pure black; card
         // is a hair off it (not also pure black) purely so card-based UI
         // elements stay visible against the background, not a departure
         // from the reference.
         ImVec4(1.000f, 0.000f, 0.000f, 1.f),
         ImVec4(0.000f, 0.000f, 0.000f, 0.96f),
         ImVec4(0.070f, 0.020f, 0.020f, 1.f),
         ImVec4(0.980f, 0.960f, 0.960f, 1.f),
         ImVec4(0.620f, 0.320f, 0.320f, 1.f),
         5.f,
         0.96f},
        {"KnockerzBot",
         ImVec4(0.070f, 0.780f, 0.720f, 1.f),
         ImVec4(0.014f, 0.058f, 0.056f, 0.96f),
         ImVec4(0.027f, 0.098f, 0.093f, 1.f),
         ImVec4(0.938f, 0.984f, 0.978f, 1.f),
         ImVec4(0.420f, 0.600f, 0.580f, 1.f),
         5.f,
         0.96f},
    };

    ImVec4 ThemeEngine::getAccent() const {
        if (activePreset == THEME_REDKINGDOM)
            return computeRedKingdomPulse();
        if (glowCycleEnabled)
            return computeCycleColor(glowCycleRate);
        return accentColor;
    }
    ImVec4 ThemeEngine::getGlowAccent() const {
        return accentColor;
    }
    ImVec4 ThemeEngine::computeCycleColor(float rate) const {
        float t = (float)ImGui::GetTime() * rate;
        float r = 0.5f + 0.5f * std::sin(t);
        float g = 0.5f + 0.5f * std::sin(t + 2.094f);
        float b = 0.5f + 0.5f * std::sin(t + 4.189f);
        return ImVec4(r, g, b, 1.f);
    }
    ImVec4 ThemeEngine::computeRedKingdomPulse() const {
        float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 2.4f);
        ImVec4 dark(0.550f, 0.020f, 0.020f, 1.f), hot(1.000f, 0.140f, 0.080f, 1.f);
        return ImVec4(dark.x + (hot.x - dark.x) * pulse,
                      dark.y + (hot.y - dark.y) * pulse,
                      dark.z + (hot.z - dark.z) * pulse,
                      1.f);
    }
    ImU32 ThemeEngine::getAccentU32(float a) const {
        ImVec4 c = getAccent();
        c.w = a;
        return toU32(c);
    }
    ImU32 ThemeEngine::getAccentDimU32(float f) const {
        ImVec4 c = getAccent();
        c.x *= f;
        c.y *= f;
        c.z *= f;
        return toU32(c);
    }
    ImU32 ThemeEngine::getTextU32() const {
        return toU32(textPrimary);
    }
    ImU32 ThemeEngine::getTextSecondaryU32() const {
        return toU32(textSecondary);
    }
    ImU32 ThemeEngine::getCardU32() const {
        return toU32(cardColor);
    }
    void ThemeEngine::applyToImGuiStyle() {
        ImGuiStyle& s = ImGui::GetStyle();
        ImVec4 accent = getAccent();
        s.WindowRounding = s.FrameRounding = s.PopupRounding = s.ScrollbarRounding = cornerRadius;
        s.WindowPadding = ImVec2(14, 12);
        ImVec4* col = s.Colors;
        col[ImGuiCol_WindowBg] = withAlpha(bgColor, bgOpacity);
        col[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        col[ImGuiCol_PopupBg] = withAlpha(bgColor, 0.94f);
        col[ImGuiCol_Border] = withAlpha(accent, 0.22f);
        col[ImGuiCol_FrameBg] = withAlpha(cardColor, 0.6f);
        col[ImGuiCol_FrameBgHovered] = withAlpha(cardColor, 0.8f);
        col[ImGuiCol_FrameBgActive] = withAlpha(cardColor, 1.f);
        col[ImGuiCol_TitleBg] = col[ImGuiCol_TitleBgActive] = col[ImGuiCol_TitleBgCollapsed] =
            withAlpha(bgColor, 1.f);
        col[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        col[ImGuiCol_ScrollbarGrab] = withAlpha(accent, 0.35f);
        col[ImGuiCol_ScrollbarGrabHovered] = withAlpha(accent, 0.55f);
        col[ImGuiCol_ScrollbarGrabActive] = withAlpha(accent, 0.75f);
        col[ImGuiCol_SliderGrab] = accent;
        col[ImGuiCol_SliderGrabActive] = brighten(accent, 0.15f);
        col[ImGuiCol_Button] = withAlpha(cardColor, 0.7f);
        col[ImGuiCol_ButtonHovered] = withAlpha(accent, 0.22f);
        col[ImGuiCol_ButtonActive] = withAlpha(accent, 0.38f);
        col[ImGuiCol_Header] = withAlpha(accent, 0.18f);
        col[ImGuiCol_HeaderHovered] = withAlpha(accent, 0.28f);
        col[ImGuiCol_HeaderActive] = withAlpha(accent, 0.38f);
        col[ImGuiCol_CheckMark] = accent;
        col[ImGuiCol_Text] = textPrimary;
        col[ImGuiCol_TextDisabled] = textSecondary;
        col[ImGuiCol_Separator] = withAlpha(accent, 0.18f);
    }
    void ThemeEngine::resetDefaults() {
        accentColor = ImVec4(0.788f, 0.659f, 0.298f, 1.f);
        bgColor = ImVec4(0.051f, 0.051f, 0.051f, 0.96f);
        cardColor = ImVec4(0.078f, 0.078f, 0.078f, 1.f);
        textPrimary = ImVec4(0.941f, 0.910f, 0.816f, 1.f);
        textSecondary = ImVec4(0.478f, 0.447f, 0.376f, 1.f);
        bgOpacity = 0.96f;
        cornerRadius = 5.f;
        textScale = 1.f;
        glowCycleEnabled = false;
        glowCycleRate = 0.5f;
        activePreset = 0;
    }
    void ThemeEngine::applyPreset(int i) {
        if (i < 0 || i >= getPresetCount())
            return;
        const auto& p = kThemePresets[i];
        accentColor = p.accent;
        bgColor = p.bg;
        cardColor = p.card;
        textPrimary = p.textPrimary;
        textSecondary = p.textSecondary;
        cornerRadius = p.cornerRadius;
        bgOpacity = p.bgOpacity;
        activePreset = i;
    }
    const ThemePreset* ThemeEngine::getPresets() {
        return kThemePresets;
    }
    int ThemeEngine::getPresetCount() {
        return sizeof(kThemePresets) / sizeof(kThemePresets[0]);
    }

    float AnimationState::easeOutCubic(float t) {
        t = std::clamp(t, 0.f, 1.f);
        float i = 1.f - t;
        return 1.f - i * i * i;
    }
    float AnimationState::easeInOutQuad(float t) {
        t = std::clamp(t, 0.f, 1.f);
        return t < 0.5f ? 2 * t * t : 1.f - ((-2 * t + 2) * (-2 * t + 2)) / 2.f;
    }
    void AnimationState::update(float dt) {
        if (dt > 0.05f)
            dt = 0.05f;
        float step = dt * animSpeed;
        if (opening) {
            openProgress += step;
            if (openProgress >= 1.f) {
                openProgress = 1.f;
                opening = false;
            }
        }
        if (closing) {
            openProgress -= step;
            if (openProgress <= 0.f) {
                openProgress = 0.f;
                closing = false;
            }
        }
        tabTransition = std::min(1.f, tabTransition + dt * animSpeed * 1.2f);
    }

    MenuInterface* MenuInterface::get() {
        static MenuInterface* s = new MenuInterface();
        return s;
    }

    void MenuInterface::markReplayListDirty(bool queueRefresh) {
        replayListDirty = true;
        if (queueRefresh)
            replayRefreshQueued = true;
    }
    bool MenuInterface::hasReplayDirectoryChanged() const {
        std::error_code ec;
        auto dir = getReplayDir();
        if (!std::filesystem::exists(dir, ec) || ec) {
            return !replayDirTimeValid;
        }
        auto t = std::filesystem::last_write_time(dir, ec);
        if (ec)
            return false;
        if (!replayDirTimeValid)
            return true;
        return t != replayDirLastWriteTime;
    }
    void MenuInterface::captureReplayDirectoryTimestamp() {
        std::error_code ec;
        auto dir = getReplayDir();
        if (!std::filesystem::exists(dir, ec) || ec) {
            replayDirTimeValid = false;
            return;
        }
        replayDirLastWriteTime = std::filesystem::last_write_time(dir, ec);
        replayDirTimeValid = !ec;
    }
    void MenuInterface::refreshReplayListIfNeeded(bool force) {
        if (!force && !replayRefreshQueued && !hasReplayDirectoryChanged())
            return;
        GucciEngine::get()->reloadMacroList();
        captureReplayDirectoryTimestamp();
        replayListDirty = false;
        replayRefreshQueued = false;
    }

    std::string getKeyName(int code) {
        if (code == 0)
            return "None";
        if (code == 9)
            return "Tab";
        if (code == 13)
            return "Enter";
        if (code == 27)
            return "Escape";
        if (code == 32)
            return "Space";
        if (code == 8)
            return "Backspace";
        if (code == 46)
            return "Delete";
        if (code == 0xA4)
            return "L.Alt";
        if (code == 0xA5)
            return "R.Alt";
        if (code == 0x12)
            return "Alt";
        if (code >= 65 && code <= 90) {
            char s[2] = {(char)code, 0};
            return s;
        }
        if (code >= 48 && code <= 57) {
            char s[2] = {(char)code, 0};
            return s;
        }
        if (code >= 112 && code <= 123) {
            char s[4];
            snprintf(s, sizeof(s), "F%d", code - 111);
            return s;
        }
        char s[32];
        snprintf(s, sizeof(s), "0x%X", code);
        return s;
    }

    namespace Widgets {
        bool
        ToggleSwitch(const char* label, bool* value, ThemeEngine& theme, AnimationState& anim) {
            ImGuiID id = ImGui::GetID(label);
            float& t = anim.toggleAnims[id];
            float target = *value ? 1.f : 0.f;
            t = t + (target - t) * std::min(1.f, ImGui::GetIO().DeltaTime * 14.f);
            t = std::clamp(t, 0.f, 1.f);
            ImVec2 cursor = ImGui::GetCursorScreenPos();
            const float trackW = 36.f, trackH = 16.f, knobR = 6.f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 trackMin = cursor, trackMax(cursor.x + trackW, cursor.y + trackH);
            ImGui::InvisibleButton(label, ImVec2(ImGui::GetContentRegionAvail().x, trackH));
            bool clicked = ImGui::IsItemClicked();
            if (clicked)
                *value = !*value;
            bool hovered = ImGui::IsItemHovered();
            ImVec4 trackCol =
                lerpColor(ImVec4(0.22f, 0.22f, 0.22f, 1.f), withAlpha(theme.getAccent(), 0.75f), t);
            dl->AddRectFilled(trackMin, trackMax, toU32(trackCol), trackH * 0.5f);
            float capR = trackH * 0.5f;
            float knobCX = trackMin.x + capR + (trackW - 2.f * capR) * t;
            float knobCY = trackMin.y + capR;
            ImVec4 knobCol =
                lerpColor(ImVec4(0.55f, 0.55f, 0.55f, 1.f), ImVec4(1.f, 1.f, 1.f, 1.f), t);
            dl->AddCircleFilled(ImVec2(knobCX, knobCY), knobR, toU32(knobCol));
            ImVec2 textPos(trackMin.x + trackW + 8.f,
                           trackMin.y + (trackH - ImGui::GetTextLineHeight()) * 0.5f);
            dl->AddText(textPos, hovered ? theme.getAccentU32() : theme.getTextU32(), label);
            return clicked;
        }

        bool StyledButton(const char* label,
                          ImVec2 size,
                          ThemeEngine& theme,
                          AnimationState& anim,
                          float roundingOverride) {
            float r = roundingOverride >= 0 ? roundingOverride : theme.cornerRadius;
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, r);
            ImGui::PushStyleColor(ImGuiCol_Button, withAlpha(theme.cardColor, 0.7f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, withAlpha(theme.getAccent(), 0.22f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, withAlpha(theme.getAccent(), 0.38f));
            bool r2 = ImGui::Button(label, size);
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
            return r2;
        }

        bool StyledSliderFloat(
            const char* label, float* v, float lo, float hi, ThemeEngine& theme, bool allowInput) {
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, theme.getAccent());
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, brighten(theme.getAccent(), 0.15f));
            bool changed;
            if (allowInput) {
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80);
                changed = ImGui::SliderFloat(label, v, lo, hi, "%.2f");
                ImGui::SameLine(0, 6);
                char buf[16];
                snprintf(buf, sizeof(buf), "%.2f", *v);
                ImGui::SetNextItemWidth(70);
                char ib[16];
                snprintf(ib, sizeof(ib), "##%s_in", label);
                if (ImGui::InputFloat(ib, v, 0, 0, "%.2f"))
                    changed = true;
            } else {
                ImGui::SetNextItemWidth(-1);
                changed = ImGui::SliderFloat(label, v, lo, hi, "%.2f");
            }
            ImGui::PopStyleColor(2);
            return changed;
        }

        bool StyledSliderInt(const char* label, int* v, int lo, int hi, ThemeEngine& theme) {
            ImGui::TextUnformatted(label);
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, theme.getAccent());
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, brighten(theme.getAccent(), 0.15f));
            ImGui::SetNextItemWidth(-1);
            char hidden[160];
            snprintf(hidden, sizeof(hidden), "##%s", label);
            bool r = ImGui::SliderInt(hidden, v, lo, hi);
            ImGui::PopStyleColor(2);
            return r;
        }

        void SectionHeader(const char* text, ThemeEngine& theme) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            ImVec2 sz = ImGui::CalcTextSize(text);
            dl->AddText(p, theme.getAccentU32(), text);
            float lineY = p.y + sz.y * 0.5f;
            float lineX = p.x + sz.x + 8.f;
            dl->AddLine(
                ImVec2(lineX, lineY), ImVec2(p.x + w, lineY), theme.getAccentU32(0.25f), 1.f);
            ImGui::Dummy(ImVec2(0, sz.y + 4.f));
        }

        bool ModuleCard(const char* name,
                        const char* desc,
                        bool* enabled,
                        ThemeEngine& theme,
                        AnimationState& anim,
                        int* keybind) {
            ImGuiID id = ImGui::GetID(name);
            float& hov = anim.hoverAnims[id];
            ImVec2 pos = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x, h = 46.f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImGui::InvisibleButton(name, ImVec2(w, h));
            bool clicked = ImGui::IsItemClicked();
            bool isHov = ImGui::IsItemHovered();
            hov = smoothStep(hov, isHov ? 1.f : 0.f, 12.f, ImGui::GetIO().DeltaTime);
            ImVec4 bg = lerpColor(theme.cardColor, brighten(theme.cardColor, 0.05f), hov);
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), toU32(bg), theme.cornerRadius);
            dl->AddRect(pos,
                        ImVec2(pos.x + w, pos.y + h),
                        theme.getAccentU32(*enabled ? 0.45f : 0.12f),
                        theme.cornerRadius,
                        0,
                        *enabled ? 1.2f : 0.5f);
            if (*enabled)
                dl->AddRectFilled(pos,
                                  ImVec2(pos.x + 3, pos.y + h),
                                  theme.getAccentU32(0.85f),
                                  theme.cornerRadius);
            dl->AddText(ImVec2(pos.x + 12, pos.y + 8),
                        *enabled ? theme.getAccentU32() : theme.getTextU32(),
                        name);
            if (desc && *desc)
                dl->AddText(ImVec2(pos.x + 12, pos.y + 26), theme.getTextSecondaryU32(), desc);
            const float tw = 36.f, th = 16.f, knobR = 6.f;
            float tx = pos.x + w - tw - 8, ty = pos.y + (h - th) * 0.5f;
            float& tt = anim.toggleAnims[id];
            tt = tt + (*enabled ? 1.f : 0.f - tt) * std::min(1.f, ImGui::GetIO().DeltaTime * 14.f);
            tt = std::clamp(tt, 0.f, 1.f);
            ImVec4 tc = lerpColor(
                ImVec4(0.22f, 0.22f, 0.22f, 1.f), withAlpha(theme.getAccent(), 0.75f), tt);
            dl->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + tw, ty + th), toU32(tc), th * 0.5f);
            float capR = th * 0.5f;
            float knobCX = tx + capR + (tw - 2.f * capR) * tt;
            float knobCY = ty + capR;
            dl->AddCircleFilled(
                ImVec2(knobCX, knobCY),
                knobR,
                toU32(lerpColor(ImVec4(0.55f, 0.55f, 0.55f, 1.f), ImVec4(1.f, 1.f, 1.f, 1.f), tt)));
            if (clicked)
                *enabled = !*enabled;
            return *enabled;
        }

        bool ModuleCardBegin(const char* name,
                             const char* desc,
                             bool* enabled,
                             ThemeEngine& theme,
                             AnimationState& anim,
                             int* keybind) {
            ModuleCard(name, desc, enabled, theme, anim, keybind);
            if (*enabled) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
                ImGui::Indent(8);
            }
            return *enabled;
        }
        void ModuleCardEnd() {
            ImGui::Unindent(8);
            ImGui::Dummy(ImVec2(0, 4));
        }

        void StatusBadge(const char* text, ImVec4 color) {
            ImVec2 ts = ImGui::CalcTextSize(text);
            ImVec2 pos = ImGui::GetCursorScreenPos();
            float pad = 6.f, h = ts.y + pad * 2, w = ts.x + pad * 2;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(
                pos, ImVec2(pos.x + w, pos.y + h), toU32(withAlpha(color, 0.18f)), 999.f);
            dl->AddRect(
                pos, ImVec2(pos.x + w, pos.y + h), toU32(withAlpha(color, 0.6f)), 999.f, 0, 1.f);
            dl->AddText(ImVec2(pos.x + pad, pos.y + pad), toU32(color), text);
            ImGui::Dummy(ImVec2(w, h));
        }

        bool PillButton(
            const char* label, bool active, float width, ThemeEngine& theme, AnimationState& anim) {
            ImGuiID id = ImGui::GetID(label);
            float& t = anim.hoverAnims[id];
            ImVec2 pos = ImGui::GetCursorScreenPos();
            float h = 32.f;
            ImGui::InvisibleButton(label, ImVec2(width, h));
            bool clicked = ImGui::IsItemClicked();
            bool hovered = ImGui::IsItemHovered();
            t = smoothStep(t, hovered ? 1.f : 0.f, 12.f, ImGui::GetIO().DeltaTime);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec4 bg = active ? withAlpha(theme.getAccent(), 0.22f)
                               : lerpColor(withAlpha(theme.cardColor, 0.5f),
                                           withAlpha(theme.getAccent(), 0.1f),
                                           t);
            dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + h), toU32(bg), h * 0.5f);
            dl->AddRect(pos,
                        ImVec2(pos.x + width, pos.y + h),
                        theme.getAccentU32(active ? 0.8f : 0.25f),
                        h * 0.5f,
                        0,
                        active ? 1.2f : 0.5f);
            ImVec2 ts = ImGui::CalcTextSize(label);
            ImU32 tc = active    ? theme.getAccentU32()
                       : hovered ? theme.getTextU32()
                                 : theme.getTextSecondaryU32();
            dl->AddText(
                ImVec2(pos.x + (width - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), tc, label);
            return clicked;
        }

        void
        KeybindButton(const char* label, int* keyCode, ThemeEngine& theme, AnimationState& anim) {
            ImGui::Text("%s", label);
            ImGui::SameLine();
            char btnLabel[64];
            snprintf(btnLabel, sizeof(btnLabel), "%s##kb_%s", getKeyName(*keyCode).c_str(), label);
            bool isRebinding = (MenuInterface::get()->rebindTarget == keyCode);
            if (isRebinding) {
                ImGui::PushStyleColor(ImGuiCol_Button, withAlpha(theme.getAccent(), 0.3f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, withAlpha(theme.getAccent(), 0.4f));
                if (ImGui::Button("Press key...", ImVec2(110, 0))) {
                    MenuInterface::get()->rebindTarget = nullptr;
                }
                ImGui::PopStyleColor(2);
            } else {
                if (StyledButton(btnLabel, ImVec2(110, 0), theme, anim)) {
                    MenuInterface::get()->rebindTarget = keyCode;
                }
            }
        }
    } // namespace Widgets

    void MenuInterface::drawBackdrop() {
        if (anim.openProgress <= 0.f)
            return;
        auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::SetNextWindowBgAlpha(0.f);
        ImGui::Begin("##backdrop",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoFocusOnAppearing);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float a = anim.easeOutCubic(anim.openProgress) * 0.35f;
        dl->AddRectFilled(vp->Pos,
                          ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                          IM_COL32(0, 0, 0, (int)(a * 255)));
        if (ambientWavesEnabled)
            drawAmbientWaves(dl, vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y));
        ImGui::End();
    }

    void MenuInterface::drawAmbientWaves(ImDrawList* dl, ImVec2 mn, ImVec2 mx) {
        ambientTime += ImGui::GetIO().DeltaTime * 0.3f;
        ImVec4 acc = theme.getAccent();
        float w = mx.x - mn.x, h = mx.y - mn.y;
        for (int i = 0; i < 3; i++) {
            float phase = (float)i * 2.094f;
            float amp = h * 0.06f, freq = 1.5f + i * 0.5f;
            float baseY = mn.y + h * (0.3f + i * 0.2f);
            const int segs = 80;
            ImVec2 prev;
            for (int j = 0; j <= segs; j++) {
                float fx = (float)j / segs;
                float x = mn.x + fx * w;
                float y = baseY + amp * std::sin(fx * freq * 3.14159f * 2 + ambientTime + phase);
                ImVec2 cur(x, y);
                if (j > 0)
                    dl->AddLine(prev,
                                cur,
                                IM_COL32((int)(acc.x * 255),
                                         (int)(acc.y * 255),
                                         (int)(acc.z * 255),
                                         (int)(18.f * (1.f - i * 0.25f) * anim.openProgress)),
                                1.f);
                prev = cur;
            }
        }
    }

    void MenuInterface::drawSnowOverlay(ImDrawList* dl, ImVec2 mn, ImVec2 mx) {
        constexpr int kFlakeCount = 220;
        float w = mx.x - mn.x, h = mx.y - mn.y;
        if (w <= 0.f || h <= 0.f)
            return;
        if (snowFlakes.size() != kFlakeCount) {
            snowFlakes.resize(kFlakeCount);
            for (auto& f : snowFlakes) {
                f.x = frand(0.f, 1.f);
                f.y = frand(0.f, 1.f);
                f.speed = frand(35.f, 150.f);
                f.size = frand(1.1f, 3.4f);
                f.drift = frand(0.f, 6.2832f);
            }
        }
        float dt = ImGui::GetIO().DeltaTime;
        float t = (float)ImGui::GetTime();
        dl->PushClipRect(mn, mx, true);
        for (auto& f : snowFlakes) {
            f.y += (f.speed / h) * dt;
            if (f.y > 1.05f) {
                f.y = -0.05f;
                f.x = frand(0.f, 1.f);
                f.speed = frand(35.f, 150.f);
                f.size = frand(1.1f, 3.4f);
            }
            float sway = std::sin(t * 0.9f + f.drift) * 0.012f;
            float px = mn.x + std::clamp(f.x + sway, 0.f, 1.f) * w;
            float py = mn.y + f.y * h;
            float alpha = 0.30f + 0.40f * ((f.size - 1.1f) / (3.4f - 1.1f));
            dl->AddCircleFilled(
                ImVec2(px, py), f.size, IM_COL32(232, 244, 255, (int)(alpha * 255)));
        }
        dl->PopClipRect();
    }

    static void drawJupiterClickBar(ThemeEngine& theme,
                                    AnimationState& anim,
                                    GucciEngine* engine,
                                    float windowSeconds,
                                    bool externalWidgetJustReleased,
                                    float h);

    // REVERTED 2026-08-31 (build -i): the glTexSubImage2D update-in-place
    // attempt below (kept in this comment for history, not live code) broke
    // rendering the instant a SECOND frame was ever uploaded through it.
    // Nigel's precise repro nailed it: pressing "Enable Video Mode" alone
    // (first frame only, still goes through the initWithData branch below)
    // was fine; pressing Play/Resume (which starts decoding/uploading
    // frame 2 onward, the first time the subImage2D branch ever ran) is
    // exactly when the screen went garbled -- including the click bar and
    // exit button, which don't touch this texture at all, going missing
    // too. That points at glTexSubImage2D (or the ccGLBindTexture2D call
    // right before it) corrupting some OTHER piece of shared GL/render
    // state -- a real candidate, not confirmed: ImGui's entire UI (all
    // text, all widgets, including the click bar/exit button) samples from
    // ONE shared font atlas texture, and if ccGLBindTexture2D's cached
    // "currently bound" texture unit didn't actually match this texture's
    // real GL binding at the moment this ran, the subImage2D call could
    // have silently targeted the wrong texture -- e.g. overwriting part of
    // that shared atlas -- which would explain unrelated widgets breaking
    // too. Genuinely not confirmed, so not shipping a second guess at
    // fixing THIS approach -- reverted to always recreating a fresh
    // CCTexture2D per frame (the exact approach confirmed working in build
    // -f), which is back to the ~5fps Nigel first reported, but correct.
    // If this perf win is worth revisiting later, it needs real evidence
    // first (e.g. glGetError() checks around the subImage2D call, or
    // confirming the actual bound-texture-unit assumption some other way)
    // rather than another blind attempt at the same technique.
    static void uploadOrUpdateRgbaTexture(cocos2d::CCTexture2D*& texObj,
                                          int w,
                                          int h,
                                          const std::vector<uint8_t>& rgba) {
        if (texObj) {
            texObj->release();
            texObj = nullptr;
        }
        auto* newTex = new cocos2d::CCTexture2D();
        newTex->initWithData(rgba.data(),
                             cocos2d::kCCTexture2DPixelFormat_RGBA8888,
                             (unsigned int)w,
                             (unsigned int)h,
                             cocos2d::CCSizeMake((float)w, (float)h));
        texObj = newTex;
    }

    // The real JMF showcase video ships bundled as a mod resource (see
    // mod.json) -- Video Mode works out of the box with zero setup. Nigel's
    // explicit call: now that the built .geode isn't tracked in git anymore
    // (see feedback_github_workflow), there's no GitHub size ceiling on the
    // PACKAGE itself forcing a "drop it in a folder yourself" workaround --
    // bundle it for real, same as every Big Brrr track already is. The file
    // picker below still lets Nigel override with a different video; when
    // set, the picked path takes priority over the bundled default, same
    // precedence Big Brrr's own "brrr" folder already has over its bundled
    // per-theme track.
    static std::filesystem::path getBundledJmfVideoPath() {
        return Mod::get()->getResourcesDir() / "jmf_showcase.mp4";
    }

    // Video Mode -- a review/playback overlay, not a live-gameplay one; no
    // level needs to be open. Video plays full-screen (letterboxed to its
    // own aspect ratio), riding the exact same clock drawJupiterClickBar
    // already uses (jupiterClickBarPosSec), offset by jupiterVideoOffsetSec
    // so the video's own timestamp and the macro's time-zero can be
    // manually aligned. The click bar itself is drawn again here, in its
    // own always-fully-opaque window near the bottom -- the ORIGINAL
    // panel-embedded call site is untouched, this is a second, independent
    // call into the same shared drawing function.
    // On-screen debug text depends on rendering actually working, which is
    // exactly what's in question here -- a log file doesn't, so this settles
    // "is the function even being reached, and with what value" independent
    // of any GL/ImGui rendering issue. Same dedicated-log-file pattern as
    // guccibot_calcdeath.log. Deliberately logs on every call (throttled to
    // roughly once a second) rather than only on toggle-change, so a case
    // where the toggle visually flips but the value never reaches this
    // function would also show up.
    static void logVideoModeDebug(const std::string& line) {
        static std::ofstream log;
        if (!log.is_open()) {
            auto path = Mod::get()->getSaveDir() / "guccibot_videomode.log";
            log.open(path, std::ios::out | std::ios::trunc);
            geode::log::info("[VIDEOMODE] log file at: {}", path.string());
        }
        if (log.is_open()) {
            log << line << '\n';
            log.flush();
        }
    }

    void MenuInterface::drawJupiterVideoOverlay() {
        auto* engine = GucciEngine::get();
        static double s_lastVideoModeLogTime = -1000.0;
        double nowT = ImGui::GetTime();
        if (nowT - s_lastVideoModeLogTime > 1.0) {
            s_lastVideoModeLogTime = nowT;
            char line[128];
            snprintf(line,
                    sizeof(line),
                    "[t=%.2f] drawJupiterVideoOverlay called, jupiterVideoModeEnabled=%d",
                    nowT,
                    (int)engine->jupiterVideoModeEnabled);
            logVideoModeDebug(line);
        }
        if (!engine->jupiterVideoModeEnabled)
            return;

        // Real bug found 2026-08-31 (Nigel: "on the video click bar, i dont
        // see my inputs"): jupiterClickBarPageVisible is the flag
        // GB7KeyHandler::dispatchKeyboardMSG (keybinds.cpp) gates real
        // spacebar/click capture on -- but the ONLY place that ever sets it
        // true is drawJupiterClickTrainerPage(), which lives inside
        // drawMainWindow()/drawMegaHackWindow(). The -h/-i perf fix skips
        // that whole window while Video Mode is on (drawInterface() resets
        // this flag to false every frame, unconditionally, before either
        // path runs) -- so with the main window skipped, nothing ever sets
        // it back to true, and every real keypress was silently going
        // uncaptured the entire time Video Mode was up, regardless of
        // whether a level was open. Video Mode has its own independent
        // click bar instance (the window below) and needs to assert this
        // itself rather than depend on the buried tab page doing it.
        engine->jupiterClickBarPageVisible = true;

        // Always the bundled default now -- the custom-video picker was
        // removed (see the comment on jupiterVideoModeEnabled in
        // GucciBot.hpp for why).
        std::string effectivePath = getBundledJmfVideoPath().string();

        // -k's toggle-state log confirmed this function DOES get reached with
        // jupiterVideoModeEnabled correctly true -- so whatever's wrong is
        // deeper than that. On-screen debug text still isn't proving
        // anything (could be a real rendering bug, could be the text itself
        // not showing for some unrelated reason, e.g. no font pushed) so
        // this whole path now also logs to the same rendering-independent
        // file, not just the top-level toggle state.
        bool openedThisFrame = false;
        bool openFailed = false;
        if (jupiterVideoLoadedPath != effectivePath) {
            jupiterVideoDecoder.close();
            if (jupiterVideoDecoder.open(effectivePath)) {
                jupiterVideoLoadedPath = effectivePath;
                openedThisFrame = true;
            } else {
                jupiterVideoLoadedPath.clear();
                engine->jupiterVideoModeEnabled = false; // don't retry a broken path every frame
                openFailed = true;
            }
            char line[512];
            snprintf(line,
                    sizeof(line),
                    "[t=%.2f] (re)open attempt: path=%s openedThisFrame=%d openFailed=%d",
                    (double)ImGui::GetTime(),
                    effectivePath.c_str(),
                    (int)openedThisFrame,
                    (int)openFailed);
            logVideoModeDebug(line);
        }

        auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::SetNextWindowBgAlpha(0.f);
        bool windowVisible = ImGui::Begin("##jupiterVideoOverlay",
                                          nullptr,
                                          ImGuiWindowFlags_NoDecoration |
                                              ImGuiWindowFlags_NoInputs |
                                              ImGuiWindowFlags_NoMove |
                                              ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_NoFocusOnAppearing);
        // Confirmed against ImGui's own source (imgui.cpp, Begin()): a window
        // created with NoBringToFrontOnFocus is push_front'd into g.Windows
        // instead of push_back'd -- i.e. it paints at the very BACK of the
        // whole z-stack, permanently, not just "doesn't jump forward on
        // click." That flag used to be set here, which is almost certainly
        // why every previous build's texture/decode fixes never mattered:
        // the main Jupiter-tab window renders full-viewport at alpha=1.0
        // (see jupiterActive branch below) and is the window you have to
        // click to even reach this toggle, so it was always winning the
        // z-order and painting over this window entirely, regardless of
        // whether the texture itself was ever correct. Flag removed above.
        // Still need this every frame (not just on first creation) since any
        // later click back in the main window would otherwise push IT back
        // to the front again -- SetWindowFocus() forces this window back on
        // top each frame, undoing that. (ImGui source also confirms
        // SetWindowFocus()/FocusWindow() itself no-ops the reorder if
        // NoBringToFrontOnFocus is set, so removing the flag isn't optional.)
        //
        // Real bug found 2026-08-31 from this exact line: FocusWindow()
        // (imgui.cpp) "steals active widgets" -- if some OTHER window
        // currently owns g.ActiveId (e.g. the click bar's own Resume button
        // mid-press, in ##jupiterVideoClickBar below), focusing a DIFFERENT
        // window calls ClearActiveID() and cancels that in-progress click
        // outright. Since this ran unconditionally every frame, it was
        // silently killing the Resume button's press-then-release cycle the
        // very next frame after mouse-down, before release could ever
        // register -- confirmed via a real click-handler log that never
        // once fired despite genuine repeated presses. Skipping the call
        // whenever ANYTHING is currently active anywhere fixes it: an
        // active widget only lives 1-2 frames past its own window's own
        // focus call (which is safe, see the matching comment below), so
        // this costs at most a one-frame delay in reclaiming front z-order,
        // never a stuck click.
        if (!ImGui::IsAnyItemActive())
            ImGui::SetWindowFocus();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        double videoTimeSec = 0.0;
        bool decodedThisFrame = false;
        size_t rgbaBytes = 0;
        if (!openFailed && jupiterVideoDecoder.isOpen()) {
            // Alignment tool active: show the manually-scrubbed position
            // directly instead of the click bar's live clock, so Nigel can
            // find the exact frame of the macro's first click by eye. See
            // jupiterVideoAlignToolActive's declaration (GucciBot.hpp).
            if (engine->jupiterVideoAlignToolActive)
                videoTimeSec = (double)engine->jupiterVideoAlignScrubSec;
            else
                videoTimeSec = engine->jupiterClickBarPosSec + (double)engine->jupiterVideoOffsetSec;
            videoTimeSec = std::clamp(
                videoTimeSec, 0.0, std::max(0.0, jupiterVideoDecoder.durationSec() - 0.001));

            std::vector<uint8_t> rgba;
            decodedThisFrame = jupiterVideoDecoder.getFrameAt(videoTimeSec, rgba);
            rgbaBytes = rgba.size();
            if (decodedThisFrame) {
                uploadOrUpdateRgbaTexture(jupiterVideoTexture,
                                          jupiterVideoDecoder.width(),
                                          jupiterVideoDecoder.height(),
                                          rgba);
                jupiterVideoTexW = jupiterVideoDecoder.width();
                jupiterVideoTexH = jupiterVideoDecoder.height();
            }
        }

        static double s_lastFullLogTime = -1000.0;
        double nowT2 = ImGui::GetTime();
        if (nowT2 - s_lastFullLogTime > 1.0) {
            s_lastFullLogTime = nowT2;
            char line[512];
            snprintf(line,
                    sizeof(line),
                    "[t=%.2f] frame: openFailed=%d decoder.isOpen=%d windowVisible=%d "
                    "vpPos=(%.0f,%.0f) vpSize=(%.0f,%.0f) requestedT=%.3f decodeOk=%d "
                    "rgbaBytes=%zu tex=%u texW=%d texH=%d clickBarPaused=%d "
                    "clickBarPosSec=%.3f clickBarLastRealTime=%.3f clickBarEnabled=%d "
                    "clickBarPageOpen=%d",
                    nowT2,
                    (int)openFailed,
                    (int)jupiterVideoDecoder.isOpen(),
                    (int)windowVisible,
                    vp->Pos.x,
                    vp->Pos.y,
                    vp->Size.x,
                    vp->Size.y,
                    videoTimeSec,
                    (int)decodedThisFrame,
                    rgbaBytes,
                    (unsigned int)(jupiterVideoTexture ? jupiterVideoTexture->getName() : 0),
                    jupiterVideoTexW,
                    jupiterVideoTexH,
                    (int)engine->jupiterClickBarPaused,
                    engine->jupiterClickBarPosSec,
                    engine->jupiterClickBarLastRealTime,
                    (int)engine->jupiterClickBarEnabled,
                    (int)jupiterClickBarPageOpen);
            logVideoModeDebug(line);
        }

        if (openFailed) {
            dl->AddText(ImVec2(vp->Pos.x + 20, vp->Pos.y + 20),
                       IM_COL32(255, 80, 80, 255),
                       ("VIDEO MODE DEBUG: failed to open: " + effectivePath).c_str());
            ImGui::End();
            return;
        }
        if (!jupiterVideoDecoder.isOpen()) {
            dl->AddText(ImVec2(vp->Pos.x + 20, vp->Pos.y + 20),
                       IM_COL32(255, 80, 80, 255),
                       "VIDEO MODE DEBUG: decoder not open");
            ImGui::End();
            return;
        }

        char debugLine[512];
        snprintf(debugLine,
                sizeof(debugLine),
                "VIDEO MODE DEBUG: path=%s opened=%d decoder.isOpen=%d requestedT=%.3f "
                "decodeOk=%d tex=%u texW=%d texH=%d rgbaBytes=%zu",
                effectivePath.c_str(),
                (int)openedThisFrame,
                (int)jupiterVideoDecoder.isOpen(),
                videoTimeSec,
                (int)decodedThisFrame,
                (unsigned int)(jupiterVideoTexture ? jupiterVideoTexture->getName() : 0),
                jupiterVideoTexW,
                jupiterVideoTexH,
                rgbaBytes);
        dl->AddText(
            ImVec2(vp->Pos.x + 20, vp->Pos.y + 20), IM_COL32(80, 255, 120, 255), debugLine);

        if (!jupiterVideoTexture) {
            ImGui::End();
            return;
        }

        float vw = vp->Size.x, vh = vp->Size.y;
        float videoAspect = (float)jupiterVideoTexW / (float)jupiterVideoTexH;
        float viewAspect = vw / vh;
        float drawW, drawH;
        if (videoAspect > viewAspect) {
            drawW = vw;
            drawH = vw / videoAspect;
        } else {
            drawH = vh;
            drawW = vh * videoAspect;
        }
        ImVec2 topLeft(vp->Pos.x + (vw - drawW) * 0.5f, vp->Pos.y + (vh - drawH) * 0.5f);
        ImVec2 bottomRight(topLeft.x + drawW, topLeft.y + drawH);

        unsigned char alpha =
            (unsigned char)(std::clamp(engine->jupiterVideoOpacity, 0.f, 1.f) * 255.f);
        dl->AddImage((ImTextureID)(intptr_t)jupiterVideoTexture->getName(),
                    topLeft,
                    bottomRight,
                    ImVec2(0, 0),
                    ImVec2(1, 1),
                    IM_COL32(255, 255, 255, alpha));
        ImGui::End();

        ImGui::SetNextWindowPos(
            ImVec2(vp->Pos.x + vp->Size.x * 0.1f, vp->Pos.y + vp->Size.y * 0.82f));
        ImGui::SetNextWindowSize(ImVec2(vp->Size.x * 0.8f, vp->Size.y * 0.14f));
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##jupiterVideoClickBar",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
        // Same z-order fix as ##jupiterVideoOverlay above, and needs it
        // independently: this window doesn't set NoBringToFrontOnFocus so
        // it draws on top correctly the first time it's created, but with
        // no per-frame reassertion, any later click back in the main GUI
        // window (e.g. dragging the Opacity/Offset sliders) reclaims front
        // z-order for the main window and buries this one again. Called
        // after ##jupiterVideoOverlay's own SetWindowFocus() this same
        // frame, so this one wins and stays visually on top of the video
        // image, matching the original spec ("clickbar playing somewhere
        // over it"). Same IsAnyItemActive() guard as above -- this window's
        // OWN Resume/Pause/Reset/Loop buttons live right below this call, so
        // without the guard, THIS call would just as easily cancel the exit
        // button's press (##jupiterVideoExit, below) or vice versa on the
        // frame after either one goes down.
        if (!ImGui::IsAnyItemActive())
            ImGui::SetWindowFocus();
        drawJupiterClickBar(theme, anim, engine, engine->jupiterClickBarWindow, false, 60.f);
        ImGui::End();

        // Real regression this same z-order fix introduced (Nigel, same
        // session: "i cant back out of it in any way i think"): the ONLY
        // control that turns Video Mode off is the "Enable Video Mode"
        // toggle inside the main GUI window -- which is now BEHIND these two
        // overlay windows by design (that's the whole fix above), and the
        // video draws at opacity up to 1.0, so the toggle can be completely
        // invisible with no way to tell it's even there to click blind on.
        // A dedicated, always-on-top, always-reachable exit control fixes
        // this regardless of opacity/theme/window layout underneath, rather
        // than relying on the user finding a hidden button.
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 190.f, vp->Pos.y + 16.f));
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##jupiterVideoExit",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
        // Same IsAnyItemActive() guard as the other two windows in this
        // function -- see ##jupiterVideoOverlay's comment above for the
        // real mechanism (FocusWindow() clears another window's in-progress
        // active widget). Without it, THIS call could cancel a Resume press
        // in ##jupiterVideoClickBar the frame after it goes down, same bug,
        // different direction.
        if (!ImGui::IsAnyItemActive())
            ImGui::SetWindowFocus();
        if (ImGui::Button("Exit Video Mode", ImVec2(170.f, 0.f))) {
            engine->jupiterVideoModeEnabled = false;
        }
        ImGui::End();
    }

    void MenuInterface::drawTitleBar() {
        auto* engine = GucciEngine::get();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        float barH = 52.f;
        ImVec4 barBg = withAlpha(theme.bgColor, 0.3f);
        barBg.w = 0.f;
        dl->AddRectFilled(wp,
                          ImVec2(wp.x + ws.x, wp.y + barH),
                          toU32(barBg),
                          theme.cornerRadius,
                          ImDrawFlags_RoundCornersTop);
        ImVec2 lc(wp.x + 24, wp.y + barH * 0.5f);
        float lr = 10.f;
        ImVec4 acc = theme.getAccent();
        dl->AddQuad(ImVec2(lc.x, lc.y - lr),
                    ImVec2(lc.x + lr, lc.y),
                    ImVec2(lc.x, lc.y + lr),
                    ImVec2(lc.x - lr, lc.y),
                    theme.getAccentU32(0.9f),
                    1.5f);
        dl->AddQuadFilled(ImVec2(lc.x, lc.y - lr),
                          ImVec2(lc.x + lr, lc.y),
                          ImVec2(lc.x, lc.y),
                          ImVec2(lc.x - lr, lc.y),
                          theme.getAccentU32(0.18f));
        auto* activeCustom = getActiveCustomTheme();
        std::string botNameStr =
            activeCustom ? activeCustom->name
            : (activeTheme == THEME_TOOSII || activeTheme == THEME_TOOSII_SYRACUSE ||
               activeTheme == THEME_TOOSII_SACSTATE)
                ? "ToosiiBot"
            : (activeTheme == THEME_JA)         ? "JaBot"
            : (activeTheme == THEME_GIDDEY)     ? "GiddeyBot"
            : (activeTheme == THEME_BAM)        ? "BamBot"
            : (activeTheme == THEME_SEXYY)      ? "SexyyBot"
            : (activeTheme == THEME_JUICE)      ? "JuiceBot"
            : (activeTheme == THEME_BUTLER)     ? "ButlerBot"
            : (activeTheme == THEME_SAWEETIE)   ? "SaweetieBot"
            : (activeTheme == THEME_MAYBACH)    ? "MaybachBot"
            : (activeTheme == THEME_ROMO)       ? "RomoBot"
            : (activeTheme == THEME_GRIZZLEY)   ? "GrizzleyBot"
            : (activeTheme == THEME_REDKINGDOM) ? "Red Kingdom"
            : (activeTheme == THEME_LEMONADE)   ? "LemonadeBot"
            : (activeTheme == THEME_BRRR)       ? "BrrrBot"
            : (activeTheme == THEME_WAKA)       ? "WakaBot"
            : (activeTheme == THEME_YOUNGSTA)   ? "YoungstaBot"
            : (activeTheme == THEME_KNOCKERZ)   ? "KnockerzBot"
                                                : "GucciBot";
        const char* botName = botNameStr.c_str();
        ImVec2 npos(wp.x + 40, wp.y + 10);
        if (fontHeading)
            ImGui::PushFont(fontHeading);
        dl->AddText(npos, theme.getAccentU32(), botName);
        if (fontHeading)
            ImGui::PopFont();
        std::string subStr =
            activeCustom ? ("v" MOD_VERSION "  -  " + activeCustom->subtitle)
            : (activeTheme == THEME_TOOSII || activeTheme == THEME_TOOSII_SYRACUSE ||
               activeTheme == THEME_TOOSII_SACSTATE)
                ? "v" MOD_VERSION "  -  Running routes. Dropping passes."
            : (activeTheme == THEME_JA) ? "v" MOD_VERSION "  -  They can't stop me. I'm different."
            : (activeTheme == THEME_GIDDEY) ? "v" MOD_VERSION "  -  G'day. I'm open, apparently."
            : (activeTheme == THEME_BAM)    ? "v" MOD_VERSION "  -  BITCH IM KOBEEE!!!"
            : (activeTheme == THEME_SEXYY)  ? "v" MOD_VERSION
                                             "  -  Frame perfect. Goes stupid. Skee yee."
            : (activeTheme == THEME_JUICE)  ? "v" MOD_VERSION "  -  That's tuff. Brrr."
            : (activeTheme == THEME_BUTLER) ? "v" MOD_VERSION "  -  Playoff Jimmy mode: always on."
            : (activeTheme == THEME_SAWEETIE) ? "v" MOD_VERSION
                                                "  -  Icy girl. Tap in, don't fall off."
            : (activeTheme == THEME_MAYBACH) ? "v" MOD_VERSION
                                               "  -  Huh. Maybach Music. Frame perfect."
            : (activeTheme == THEME_ROMO) ? "v" MOD_VERSION
                                            "  -  Frame perfect. Definitely not over the limit."
            : (activeTheme == THEME_GRIZZLEY) ? "v" MOD_VERSION "  -  First day out. Frame perfect."
            : (activeTheme == THEME_REDKINGDOM) ? "v" MOD_VERSION
                                                  "  -  Long live the kingdom. Frame perfect."
            : (activeTheme == THEME_LEMONADE) ? "v" MOD_VERSION
                                                "  -  Make you some lemonade. Frame perfect."
            : (activeTheme == THEME_BRRR) ? "v" MOD_VERSION "  -  Frame perfect. Ice cold. Brrr."
            : (activeTheme == THEME_WAKA)
                ? "v" MOD_VERSION "  -  Hard in the paint. Frame perfect."
            : (activeTheme == THEME_YOUNGSTA)
                ? "v" MOD_VERSION "  -  Everyday's my birthday. Frame perfect."
            : (activeTheme == THEME_KNOCKERZ)
                ? "v" MOD_VERSION "  -  Two step. Frame perfect."
                                                : "v" MOD_VERSION "  -  Frame perfect. GBR6. Brrr.";
        const char* sub = subStr.c_str();
        ImVec2 spos(wp.x + 40, wp.y + 30);
        if (fontSmall)
            ImGui::PushFont(fontSmall);
        dl->AddText(spos, theme.getTextSecondaryU32(), sub);
        if (fontSmall)
            ImGui::PopFont();
        dl->AddLine(ImVec2(wp.x, wp.y + barH),
                    ImVec2(wp.x + ws.x, wp.y + barH),
                    theme.getAccentU32(0.15f),
                    1.f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + barH + 2);
    }

    void MenuInterface::switchTab(int newTab) {
        if (newTab == activeTab)
            return;
        previousTab = activeTab;
        activeTab = newTab;
        anim.tabTransition = 0.f;
        anim.transitionFromTab = previousTab;
        if (previousTab == 5 && newTab != 5)
            windowPosInitialized = false;
    }

    void MenuInterface::drawTabBar() {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float width = ImGui::GetContentRegionAvail().x;
        const char* names[] = {"Macro",
                               "Render",
                               "Autoclicker",
                               "Click Indicator",
                               "Frame Windows",
                               "JMF",
                               "Trainer",
                               "HUD",
                               "Settings",
                               "Pathfinder",
                               "Credits"};
        const int N = (int)(sizeof(names) / sizeof(names[0]));
        float tabW = width / N, tabH = 34.f;
        float dt = ImGui::GetIO().DeltaTime;
        if (tabIndicatorX < 0)
            tabIndicatorX = pos.x + activeTab * tabW;
        tabIndicatorX =
            smoothStep(tabIndicatorX, pos.x + activeTab * tabW, 14.f + anim.animSpeed * 0.7f, dt);
        for (int i = 0; i < N; i++) {
            ImVec2 tMin(pos.x + i * tabW, pos.y), tMax(tMin.x + tabW, pos.y + tabH);
            char tid[32];
            snprintf(tid, sizeof(tid), "##tab%d", i);
            ImGui::SetCursorScreenPos(tMin);
            ImGui::InvisibleButton(tid, ImVec2(tabW, tabH));
            bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked())
                switchTab(i);
            if (fontSmall)
                ImGui::PushFont(fontSmall);
            ImU32 tc = (activeTab == i) ? theme.getAccentU32(0.98f)
                       : (i == 5)       ? IM_COL32(200, 175, 90, 190)
                                        : (hov ? theme.getTextU32() : theme.getTextSecondaryU32());
            if (i == 5 && activeTab == 5) {
                const char* full = "Nigel's Jupiter My Favourite Trainer";
                std::vector<std::string> words;
                {
                    std::string w;
                    for (const char* p = full;; ++p) {
                        if (*p == ' ' || *p == 0) {
                            if (!w.empty())
                                words.push_back(w);
                            w.clear();
                            if (*p == 0)
                                break;
                        } else
                            w.push_back(*p);
                    }
                }
                std::vector<std::string> lines;
                std::string cur;
                for (auto& w : words) {
                    std::string trial = cur.empty() ? w : (cur + " " + w);
                    if (ImGui::CalcTextSize(trial.c_str()).x <= tabW - 6.f || cur.empty())
                        cur = trial;
                    else {
                        lines.push_back(cur);
                        cur = w;
                    }
                }
                if (!cur.empty())
                    lines.push_back(cur);
                float lineH = ImGui::GetFontSize();
                float totalH = lineH * (float)lines.size();
                float ly = tMin.y + (tabH - totalH) * 0.5f;
                for (auto& ln : lines) {
                    ImVec2 ts = ImGui::CalcTextSize(ln.c_str());
                    dl->AddText(ImVec2(tMin.x + (tabW - ts.x) * 0.5f, ly), tc, ln.c_str());
                    ly += lineH;
                }
            } else {
                ImVec2 ts = ImGui::CalcTextSize(names[i]);
                if (ts.x <= tabW - 6.f) {
                    ImVec2 tp(tMin.x + (tabW - ts.x) * 0.5f, tMin.y + (tabH - ts.y) * 0.5f);
                    dl->AddText(tp, tc, names[i]);
                } else {
                    std::vector<std::string> words;
                    {
                        std::string w;
                        for (const char* p = names[i];; ++p) {
                            if (*p == ' ' || *p == 0) {
                                if (!w.empty())
                                    words.push_back(w);
                                w.clear();
                                if (*p == 0)
                                    break;
                            } else
                                w.push_back(*p);
                        }
                    }
                    std::vector<std::string> lines;
                    std::string cur;
                    for (auto& w : words) {
                        std::string trial = cur.empty() ? w : (cur + " " + w);
                        if (ImGui::CalcTextSize(trial.c_str()).x <= tabW - 6.f || cur.empty())
                            cur = trial;
                        else {
                            lines.push_back(cur);
                            cur = w;
                        }
                    }
                    if (!cur.empty())
                        lines.push_back(cur);
                    float lineH = ImGui::GetFontSize();
                    float totalH = lineH * (float)lines.size();
                    float ly = tMin.y + (tabH - totalH) * 0.5f;
                    for (auto& ln : lines) {
                        ImVec2 lts = ImGui::CalcTextSize(ln.c_str());
                        dl->AddText(ImVec2(tMin.x + (tabW - lts.x) * 0.5f, ly), tc, ln.c_str());
                        ly += lineH;
                    }
                }
            }
            if (fontSmall)
                ImGui::PopFont();
        }
        float indW = tabW * 0.5f, indX = tabIndicatorX + (tabW - indW) * 0.5f;
        dl->AddRectFilled(ImVec2(indX, pos.y + tabH - 2),
                          ImVec2(indX + indW, pos.y + tabH),
                          theme.getAccentU32(0.92f),
                          2.f);
        dl->AddLine(ImVec2(pos.x, pos.y + tabH),
                    ImVec2(pos.x + width, pos.y + tabH),
                    theme.getAccentU32(0.15f),
                    1.f);
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + tabH + 6));
    }

    void MenuInterface::drawMainSubTabBar() {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float width = ImGui::GetContentRegionAvail().x;
        const char* sub[] = {"Replay", "Tools & Hacks"};
        const int SN = 2;
        float subW = width / SN, subH = 30.f;
        float dt = ImGui::GetIO().DeltaTime;
        static float subIndX = -1.f;
        if (subIndX < 0)
            subIndX = pos.x + mainSubTab * subW;
        subIndX = smoothStep(subIndX, pos.x + mainSubTab * subW, 14.f + anim.animSpeed * 0.7f, dt);
        for (int i = 0; i < SN; i++) {
            ImVec2 tMin(pos.x + i * subW, pos.y), tMax(tMin.x + subW, pos.y + subH);
            char tid[32];
            snprintf(tid, sizeof(tid), "##stab%d", i);
            ImGui::SetCursorScreenPos(tMin);
            ImGui::InvisibleButton(tid, ImVec2(subW, subH));
            bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked())
                mainSubTab = i;
            if (fontSmall)
                ImGui::PushFont(fontSmall);
            ImVec2 ts = ImGui::CalcTextSize(sub[i]);
            ImVec2 tp(tMin.x + (subW - ts.x) * 0.5f, tMin.y + (subH - ts.y) * 0.5f);
            ImU32 tc = (mainSubTab == i) ? theme.getAccentU32(0.98f)
                                         : (hov ? theme.getTextU32() : theme.getTextSecondaryU32());
            dl->AddText(tp, tc, sub[i]);
            if (fontSmall)
                ImGui::PopFont();
        }
        float indW = subW * 0.5f, indX = subIndX + (subW - indW) * 0.5f;
        dl->AddRectFilled(ImVec2(indX, pos.y + subH - 2),
                          ImVec2(indX + indW, pos.y + subH),
                          theme.getAccentU32(0.92f),
                          2.f);
        dl->AddLine(ImVec2(pos.x, pos.y + subH),
                    ImVec2(pos.x + width, pos.y + subH),
                    theme.getAccentU32(0.15f),
                    1.f);
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + subH + 6));
    }

    void MenuInterface::drawStatusBar() {
        auto* engine = GucciEngine::get();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        float padX = ImGui::GetStyle().WindowPadding.x, barH = 30.f;
        float barY = wp.y + ws.y - barH - 10.f;
        ImVec2 bMin(wp.x + padX, barY), bMax(wp.x + ws.x - padX, barY + barH);
        drawSolidRect(dl, bMin, bMax, theme.cornerRadius, theme, 1.f);
        if (fontSmall)
            ImGui::PushFont(fontSmall);
        char buf[256];
        int tick = PlayLayer::get() ? 0 : 0;
        snprintf(buf,
                 sizeof(buf),
                 "TPS: %.0f    Speed: %.2fx    Tick: %d",
                 engine->updater.m_tps,
                 engine->updater.m_speedhack,
                 tick);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(wp.x + padX + 12, barY + (barH - ts.y) * 0.5f),
                    theme.getTextSecondaryU32(),
                    buf);
        auto* statusCustom = getActiveCustomTheme();
        std::string brandStr =
            statusCustom ? statusCustom->brandTag
            : (activeTheme == THEME_TOOSII || activeTheme == THEME_TOOSII_SYRACUSE ||
               activeTheme == THEME_TOOSII_SACSTATE)
                ? "Open!"
            : (activeTheme == THEME_JA)         ? "IYKYK!"
            : (activeTheme == THEME_GIDDEY)     ? "Crikey!"
            : (activeTheme == THEME_BAM)        ? "83 pts."
            : (activeTheme == THEME_SEXYY)      ? "Skee yee."
            : (activeTheme == THEME_JUICE)      ? "Tuff."
            : (activeTheme == THEME_BUTLER)     ? "Playoff Jimmy."
            : (activeTheme == THEME_SAWEETIE)   ? "Icy!"
            : (activeTheme == THEME_MAYBACH)    ? "MMG!"
            : (activeTheme == THEME_ROMO)       ? "Called it!"
            : (activeTheme == THEME_GRIZZLEY)   ? "Activated!"
            : (activeTheme == THEME_REDKINGDOM) ? "Kneel."
            : (activeTheme == THEME_LEMONADE)   ? "Squeezed."
            : (activeTheme == THEME_BRRR)       ? "Frozen."
            : (activeTheme == THEME_WAKA)       ? "OW!"
            : (activeTheme == THEME_YOUNGSTA)   ? "Everyday!"
            : (activeTheme == THEME_KNOCKERZ)   ? "Two step!"
                                                : "Brrr.";
        const char* brand = brandStr.c_str();
        ImVec2 bts = ImGui::CalcTextSize(brand);
        dl->AddText(ImVec2(wp.x + ws.x - padX - bts.x - 12, barY + (barH - bts.y) * 0.5f),
                    theme.getAccentU32(0.6f),
                    brand);
        if (fontSmall)
            ImGui::PopFont();
    }

    void MenuInterface::drawTabContent() {
        if (frameEditor.isActive()) {
            if (fontBody)
                ImGui::PushFont(fontBody);
            frameEditor.draw(*this);
            if (fontBody)
                ImGui::PopFont();
            return;
        }
        float t = anim.easeOutCubic(anim.tabTransition);
        float offsetY = (1.f - t) * 14.f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offsetY);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t);
        if (fontBody)
            ImGui::PushFont(fontBody);
        switch (activeTab) {
        case 0:
            drawMainSubTabBar();
            switch (mainSubTab) {
            case 0:
                drawReplayTab();
                break;
            case 1:
                drawToolsTab();
                break;
            }
            break;
        case 1:
            drawRenderTab();
            break;
        case 2:
            drawAutoclickerTab();
            break;
        case 3:
            drawIndicatorsTab();
            break;
        case 4:
            drawFrameWindowsTab();
            break;
        case 5:
            drawJupiterTab();
            break;
        case 6:
            drawTrainerTab();
            break;
        case 7:
            drawHudTab();
            break;
        case 8:
            drawSettingsTab();
            break;
        case 9:
            drawPathfinderTab();
            break;
        case 10:
            drawCreditsTab();
            break;
        }
        if (fontBody)
            ImGui::PopFont();
        ImGui::PopStyleVar();
    }

    void MenuInterface::drawMainWindow() {
        auto* engine = GucciEngine::get();
        float t = anim.easeOutCubic(anim.openProgress);
        if (t <= 0.f)
            return;

        bool jupiterActive = (activeTab == 5);
        t *= bigBrrrFlickerAlpha(jupiterActive);
        ThemeEngine savedTheme = theme;
        if (jupiterActive) {
            theme.accentColor = ImVec4(0.988f, 0.961f, 0.314f, 1.f);
            theme.bgColor = ImVec4(0.063f, 0.024f, 0.502f, 1.f);
            theme.cardColor = ImVec4(0.09f, 0.05f, 0.58f, 1.f);
            theme.textPrimary = ImVec4(0.988f, 0.961f, 0.314f, 1.f);
            theme.textSecondary = ImVec4(0.70f, 0.66f, 0.85f, 1.f);
        }
        theme.applyToImGuiStyle();
        if (jupiterActive) {
            auto* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->Pos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(vp->Size, ImGuiCond_Always);
        } else {
            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            if (!windowPosInitialized) {
                ImGui::SetNextWindowPos(
                    ImVec2(center.x - windowSize.x * 0.5f, center.y - windowSize.y * 0.5f),
                    ImGuiCond_Always);
                windowPosInitialized = true;
            }
            ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
        }
        ImGui::SetNextWindowBgAlpha((jupiterActive ? 1.f : theme.bgOpacity) * t);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t);
        ImGui::Begin("##GucciBot",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoTitleBar);
        applyBigBrrrBounce(jupiterActive);
        applyBigBrrrShake(jupiterActive);
        if (!jupiterActive) {
            ImVec2 wp = ImGui::GetWindowPos();
            ImVec2 ws = ImGui::GetWindowSize();
            ImDrawList* fdl = ImGui::GetForegroundDrawList();
            ImVec2 dragMin(wp.x + ws.x * 0.35f, wp.y + 3);
            ImVec2 dragMax(wp.x + ws.x * 0.65f, wp.y + 6);
            float shimT = (float)ImGui::GetTime() * 1.2f;
            for (int i = 0; i < 3; i++) {
                float phase = (float)i * 0.4f;
                float alpha = 0.18f + 0.12f * std::sin(shimT + phase);
                float x1 = dragMin.x + (dragMax.x - dragMin.x) * ((float)i / 3.f);
                float x2 = dragMin.x + (dragMax.x - dragMin.x) * ((float)(i + 1) / 3.f);
                fdl->AddRectFilled(
                    ImVec2(x1, dragMin.y), ImVec2(x2, dragMax.y), theme.getAccentU32(alpha), 2.f);
            }
            fdl->AddRectFilled(dragMin, dragMax, theme.getAccentU32(0.35f), 2.f);
        }
        windowPos = ImGui::GetWindowPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = windowPos, ws = ImGui::GetWindowSize();
        if (jupiterActive && !jupiterClickBarPageOpen) {
            drawJupiterBackdrop(dl, wp, ws, (float)ImGui::GetTime());
        } else if (!jupiterActive) {
            dl->AddRect(wp,
                        ImVec2(wp.x + ws.x, wp.y + ws.y),
                        theme.getAccentU32(0.35f),
                        theme.cornerRadius,
                        0,
                        1.5f);
        }
        if (!jupiterActive)
            drawTitleBar();
        ImGui::SetNextWindowContentSize(ImVec2(0, 0));
        float contentH = ws.y - (jupiterActive ? 14.f : 52.f) - 40 - 14;
        if (jupiterActive)
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild("##content", ImVec2(-1, contentH), false, ImGuiWindowFlags_NoScrollbar);
        drawTabBar();
        ImGui::BeginChild("##tabcontent", ImVec2(-1, -1), false);
        drawTabContent();
        ImGui::EndChild();
        ImGui::EndChild();
        if (jupiterActive)
            ImGui::PopStyleColor();
        if (!jupiterActive)
            drawStatusBar();
        if (!jupiterActive && activeTheme == THEME_BRRR)
            drawSnowOverlay(dl, wp, ImVec2(wp.x + ws.x, wp.y + ws.y));
        ImGui::End();
        ImGui::PopStyleVar();
        if (jupiterActive)
            theme = savedTheme;
    }

    void MenuInterface::drawMegaHackWindow() {
        float t = anim.easeOutCubic(anim.openProgress);
        if (t <= 0.f)
            return;

        bool jupiterActive = (activeTab == 5);
        t *= bigBrrrFlickerAlpha(jupiterActive);
        ThemeEngine savedTheme = theme;
        if (jupiterActive) {
            theme.accentColor = ImVec4(0.988f, 0.961f, 0.314f, 1.f);
            theme.bgColor = ImVec4(0.063f, 0.024f, 0.502f, 1.f);
            theme.cardColor = ImVec4(0.09f, 0.05f, 0.58f, 1.f);
            theme.textPrimary = ImVec4(0.988f, 0.961f, 0.314f, 1.f);
            theme.textSecondary = ImVec4(0.70f, 0.66f, 0.85f, 1.f);
        }
        theme.applyToImGuiStyle();
        if (jupiterActive) {
            auto* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->Pos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(vp->Size, ImGuiCond_Always);
        } else {
            ImVec2 center = ImGui::GetMainViewport()->GetCenter();
            ImVec2 mhSize(620.f, 400.f);
            if (!windowPosInitialized) {
                ImGui::SetNextWindowPos(
                    ImVec2(center.x - mhSize.x * 0.5f, center.y - mhSize.y * 0.5f),
                    ImGuiCond_Always);
                windowPosInitialized = true;
            } else {
                ImGui::SetNextWindowPos(
                    ImVec2(center.x - mhSize.x * 0.5f, center.y - mhSize.y * 0.5f),
                    ImGuiCond_FirstUseEver);
            }
            ImGui::SetNextWindowSize(mhSize, ImGuiCond_Always);
        }
        ImGui::SetNextWindowBgAlpha(0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t);
        ImGui::Begin("##GucciBotMH",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoTitleBar);
        applyBigBrrrBounce(jupiterActive);
        applyBigBrrrShake(jupiterActive);
        windowPos = ImGui::GetWindowPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = windowPos, ws = ImGui::GetWindowSize();
        const float railW = 150.f, headH = 44.f, footH = 30.f, rnd = 6.f;
        ImU32 bgMain =
            jupiterActive ? IM_COL32(16, 6, 128, 255) : IM_COL32(18, 19, 26, (int)(243 * t));
        ImU32 bgRail =
            jupiterActive ? IM_COL32(16, 6, 128, 255) : IM_COL32(13, 14, 19, (int)(248 * t));
        ImU32 bgHead =
            jupiterActive ? IM_COL32(16, 6, 128, 255) : IM_COL32(22, 24, 32, (int)(248 * t));
        dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), bgMain, rnd);
        dl->AddRectFilled(
            wp, ImVec2(wp.x + railW, wp.y + ws.y), bgRail, rnd, ImDrawFlags_RoundCornersLeft);
        dl->AddRectFilled(ImVec2(wp.x + railW, wp.y),
                          ImVec2(wp.x + ws.x, wp.y + headH),
                          bgHead,
                          rnd,
                          ImDrawFlags_RoundCornersTopRight);
        if (jupiterActive && !jupiterClickBarPageOpen) {
            drawJupiterBackdrop(dl, wp, ws, (float)ImGui::GetTime());
        } else if (!jupiterActive) {
            dl->AddRect(
                wp, ImVec2(wp.x + ws.x, wp.y + ws.y), theme.getAccentU32(0.45f), rnd, 0, 1.f);
        }
        dl->AddLine(ImVec2(wp.x + railW, wp.y),
                    ImVec2(wp.x + railW, wp.y + ws.y),
                    theme.getAccentU32(0.12f),
                    1.f);
        dl->AddLine(ImVec2(wp.x + railW, wp.y + headH),
                    ImVec2(wp.x + ws.x, wp.y + headH),
                    theme.getAccentU32(0.10f),
                    1.f);
        if (!jupiterActive) {
            const char* title = (activeTheme == THEME_TOOSII) ? "TOOSIIBOT" : "GUCCIBOT";
            float titleW = 0.f;
            if (fontHeading)
                ImGui::PushFont(fontHeading);
            titleW = ImGui::CalcTextSize(title).x;
            dl->AddText(ImVec2(wp.x + railW + 14, wp.y + (headH - ImGui::GetFontSize()) * 0.5f),
                        theme.getAccentU32(0.96f),
                        title);
            if (fontHeading)
                ImGui::PopFont();
            if (fontSmall)
                ImGui::PushFont(fontSmall);
            dl->AddText(ImVec2(wp.x + railW + 14 + titleW + 10, wp.y + headH * 0.5f - 5),
                        theme.getTextSecondaryU32(),
                        "v" MOD_VERSION "  mega edition. brrr.");
            if (fontSmall)
                ImGui::PopFont();
            if (fontHeading)
                ImGui::PushFont(fontHeading);
            dl->AddText(ImVec2(wp.x + 16, wp.y + 12), theme.getAccentU32(0.92f), "GB");
            if (fontHeading)
                ImGui::PopFont();
        }
        const char* names[] = {"Macro",
                               "Render",
                               "Autoclicker",
                               "Click Indicator",
                               "Frame Windows",
                               "JMF",
                               "Trainer",
                               "HUD",
                               "Settings",
                               "Pathfinder",
                               "Credits"};
        const int railTabCount = (int)(sizeof(names) / sizeof(names[0]));
        float rowH = 34.f, railTop = headH + 10.f;
        for (int i = 0; i < railTabCount; i++) {
            ImVec2 rMin(wp.x, wp.y + railTop + i * rowH), rMax(wp.x + railW, rMin.y + rowH);
            char rid[24];
            snprintf(rid, sizeof(rid), "##mhTab%d", i);
            ImGui::SetCursorScreenPos(rMin);
            ImGui::InvisibleButton(rid, ImVec2(railW, rowH));
            bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked())
                switchTab(i);
            bool act = (activeTab == i);
            if (act)
                dl->AddRectFilled(rMin, rMax, theme.getAccentU32(0.10f));
            else if (hov)
                dl->AddRectFilled(rMin, rMax, IM_COL32(255, 255, 255, 10));
            if (act)
                dl->AddRectFilled(rMin, ImVec2(rMin.x + 3, rMax.y), theme.getAccentU32(0.95f));
            if (fontBody)
                ImGui::PushFont(fontBody);
            ImU32 tc = act        ? theme.getAccentU32(0.98f)
                       : (i == 5) ? IM_COL32(200, 175, 90, 190)
                                  : (hov ? theme.getTextU32() : theme.getTextSecondaryU32());
            if (i == 5 && act) {
                const char* full = "Nigel's Jupiter My Favourite Trainer";
                std::vector<std::string> words;
                {
                    std::string w;
                    for (const char* p = full;; ++p) {
                        if (*p == ' ' || *p == 0) {
                            if (!w.empty())
                                words.push_back(w);
                            w.clear();
                            if (*p == 0)
                                break;
                        } else
                            w.push_back(*p);
                    }
                }
                std::vector<std::string> lines;
                std::string cur;
                float maxW = railW - 22.f;
                for (auto& w : words) {
                    std::string trial = cur.empty() ? w : (cur + " " + w);
                    if (ImGui::CalcTextSize(trial.c_str()).x <= maxW || cur.empty())
                        cur = trial;
                    else {
                        lines.push_back(cur);
                        cur = w;
                    }
                }
                if (!cur.empty())
                    lines.push_back(cur);
                float lineH = ImGui::GetFontSize();
                float ly = rMin.y + (rowH - lineH * (float)lines.size()) * 0.5f;
                for (auto& ln : lines) {
                    dl->AddText(ImVec2(rMin.x + 16, ly), tc, ln.c_str());
                    ly += lineH;
                }
            } else {
                float maxW = railW - 22.f;
                if (ImGui::CalcTextSize(names[i]).x <= maxW) {
                    dl->AddText(ImVec2(rMin.x + 16, rMin.y + (rowH - ImGui::GetFontSize()) * 0.5f),
                                tc,
                                names[i]);
                } else {
                    std::vector<std::string> words;
                    {
                        std::string w;
                        for (const char* p = names[i];; ++p) {
                            if (*p == ' ' || *p == 0) {
                                if (!w.empty())
                                    words.push_back(w);
                                w.clear();
                                if (*p == 0)
                                    break;
                            } else
                                w.push_back(*p);
                        }
                    }
                    std::vector<std::string> lines;
                    std::string cur;
                    for (auto& w : words) {
                        std::string trial = cur.empty() ? w : (cur + " " + w);
                        if (ImGui::CalcTextSize(trial.c_str()).x <= maxW || cur.empty())
                            cur = trial;
                        else {
                            lines.push_back(cur);
                            cur = w;
                        }
                    }
                    if (!cur.empty())
                        lines.push_back(cur);
                    float lineH = ImGui::GetFontSize();
                    float ly = rMin.y + (rowH - lineH * (float)lines.size()) * 0.5f;
                    for (auto& ln : lines) {
                        dl->AddText(ImVec2(rMin.x + 16, ly), tc, ln.c_str());
                        ly += lineH;
                    }
                }
            }
            if (fontBody)
                ImGui::PopFont();
        }
        ImGui::SetCursorScreenPos(ImVec2(wp.x + railW + 12, wp.y + headH + 8));
        if (jupiterActive)
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild("##mhContent",
                          ImVec2(ws.x - railW - 24, ws.y - headH - footH - 16),
                          false,
                          ImGuiWindowFlags_NoScrollbar);
        drawTabContent();
        ImGui::EndChild();
        if (jupiterActive)
            ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(ImVec2(wp.x + railW + 12, wp.y + ws.y - footH + 2));
        if (!jupiterActive)
            drawStatusBar();
        if (!jupiterActive && activeTheme == THEME_BRRR)
            drawSnowOverlay(dl, wp, ImVec2(wp.x + ws.x, wp.y + ws.y));
        ImGui::End();
        ImGui::PopStyleVar();
        if (jupiterActive)
            theme = savedTheme;
    }

    void MenuInterface::drawCompactWindow() {
        auto* engine = GucciEngine::get();
        auto* upd = &engine->updater;
        auto* mod = Mod::get();

        float t = anim.easeOutCubic(anim.openProgress);
        if (t <= 0.f)
            return;

        auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 10, vp->Pos.y + 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(240, 0), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t);
        ImGui::Begin("##gbCompact",
                     nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing);

        if (fontHeading)
            ImGui::PushFont(fontHeading);
        ImGui::TextColored(theme.getAccent(), "GucciBot");
        if (fontHeading)
            ImGui::PopFont();
        ImGui::SameLine(ImGui::GetWindowWidth() - 58);
        if (Widgets::StyledButton("Full", ImVec2(48, 22), theme, anim, 4.f)) {
            compactMode = false;
            Mod::get()->setSavedValue("ui_compact_mode", compactMode);
        }
        ImGui::Separator();

        const char* modeStr = engine->isRecording() ? "RECORDING"
                              : engine->isPlaying() ? "PLAYING"
                                                    : "IDLE";
        ImVec4 modeCol = engine->isRecording() ? ImVec4(1.f, 0.3f, 0.3f, 1.f)
                         : engine->isPlaying() ? ImVec4(0.3f, 1.f, 0.3f, 1.f)
                                               : theme.textSecondary;
        ImGui::TextColored(modeCol, "%s", modeStr);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::Text(" f=%u  %.0f TPS  %.2fx", upd->getFrame(), upd->m_tps, upd->m_speedhack);
        ImGui::PopStyleColor();
        if (!engine->replayName.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped("%s", engine->replayName.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, 4));

        {
            refreshReplayListIfNeeded(false);
            static int compactMacroIdx = -1;
            if (!engine->storedMacros.empty()) {
                std::string preview =
                    (compactMacroIdx >= 0 && compactMacroIdx < (int)engine->storedMacros.size())
                        ? engine->storedMacros[compactMacroIdx]
                        : "Select macro...";
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 56);
                if (ImGui::BeginCombo("##compactMacroSel", preview.c_str())) {
                    for (int i = 0; i < (int)engine->storedMacros.size(); ++i) {
                        bool sel = (i == compactMacroIdx);
                        if (ImGui::Selectable(engine->storedMacros[i].c_str(), sel))
                            compactMacroIdx = i;
                        if (sel)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                bool canLoad = compactMacroIdx >= 0 && !engine->isRecording();
                if (!canLoad)
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
                bool loadClicked = Widgets::StyledButton("Load", ImVec2(48, 0), theme, anim, 4.f);
                if (!canLoad)
                    ImGui::PopStyleVar();
                if (loadClicked && canLoad) {
                    std::string mn = engine->storedMacros[compactMacroIdx];
                    std::string extFound;
                    auto dir = Mod::get()->getSaveDir() / "replays";
                    for (auto& ext : allKnownMacroExtensions()) {
                        if (std::filesystem::exists(dir / (mn + ext))) {
                            extFound = ext;
                            break;
                        }
                    }
                    if (!extFound.empty()) {
                        engine->replay.load(dir / (mn + extFound));
                        engine->replayName = mn;
                    }
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped("No saved macros yet.");
                ImGui::PopStyleColor();
            }
        }
        ImGui::Dummy(ImVec2(0, 4));

        float bw = (ImGui::GetContentRegionAvail().x - 6) / 2.f;
        if (engine->isRecording()) {
            if (Widgets::StyledButton("Stop Recording", ImVec2(-1, 26), theme, anim))
                engine->setMode(GucciEngine::Mode::Idle);
        } else if (engine->isPlaying()) {
            if (Widgets::StyledButton("Stop Playback", ImVec2(-1, 26), theme, anim))
                engine->setMode(GucciEngine::Mode::Idle);
        } else {
            if (Widgets::StyledButton("Record", ImVec2(bw, 26), theme, anim))
                engine->setMode(GucciEngine::Mode::Recording);
            ImGui::SameLine(0, 6);
            bool canPlay = !engine->replay.m_actionAtom.m_actions.empty();
            if (!canPlay)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
            bool playClicked = Widgets::StyledButton("Play", ImVec2(bw, 26), theme, anim);
            if (!canPlay)
                ImGui::PopStyleVar();
            if (playClicked && canPlay)
                engine->setMode(GucciEngine::Mode::Playing);
        }
        ImGui::Dummy(ImVec2(0, 6));

        {
            bool hasActions = !engine->replay.m_actionAtom.m_actions.empty();
            if (engine->isRecording()) {
                if (!macroNameReady) {
                    strncpy(
                        macroNameBuffer, engine->replayName.c_str(), sizeof(macroNameBuffer) - 1);
                    macroNameBuffer[sizeof(macroNameBuffer) - 1] = 0;
                    macroNameReady = true;
                }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputTextWithHint("##compactMacroName",
                                             "Macro name",
                                             macroNameBuffer,
                                             sizeof(macroNameBuffer)))
                    engine->replayName = macroNameBuffer;
                ImGui::Dummy(ImVec2(0, 4));
            }
            std::string extStr = currentThemeExtension(this);
            const char* ext = extStr.c_str();
            bool canSave = hasActions && !engine->replayName.empty();
            if (!canSave)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
            bool saveClicked = Widgets::StyledButton("Save", ImVec2(bw, 26), theme, anim);
            if (!canSave)
                ImGui::PopStyleVar();
            if (saveClicked && canSave) {
                auto savePath = Mod::get()->getSaveDir() / "replays" / (engine->replayName + ext);
                if (engine->replayBackupsEnabled)
                    engine->replay.backupExisting(savePath);
                engine->replay.save(savePath);
                markReplayListDirty();
                refreshReplayListIfNeeded(true);
                Notification::create("Macro saved", NotificationIcon::Success)->show();
            }
            ImGui::SameLine(0, 6);
            bool canCalc = PlayLayer::get() != nullptr && hasActions;
            if (!canCalc)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
            bool calcClicked = Widgets::StyledButton("Calculate", ImVec2(bw, 26), theme, anim);
            if (!canCalc)
                ImGui::PopStyleVar();
            if (calcClicked && canCalc)
                engine->analyzeFrameWindows();
        }
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::Separator();

        ImGui::SetNextItemWidth(bw);
        ImGui::InputFloat("##ctps", &compactTempTickRate, 0, 0, "%.0f TPS");
        ImGui::SameLine(0, 6);
        if (Widgets::StyledButton("Apply##ctps", ImVec2(bw, 24), theme, anim)) {
            if (!PlayLayer::get() || !engine->isPlaying()) {
                upd->m_tps = compactTempTickRate;
                mod->setSavedValue("eng_tick_rate", (float)upd->m_tps);
            }
        }
        ImGui::SetNextItemWidth(bw);
        ImGui::InputFloat("##cspd", &compactTempGameSpeed, 0, 0, "%.2fx");
        ImGui::SameLine(0, 6);
        if (Widgets::StyledButton("Apply##cspd", ImVec2(bw, 24), theme, anim))
            upd->m_speedhack = compactTempGameSpeed;
        ImGui::Dummy(ImVec2(0, 6));

        if (Widgets::ToggleSwitch("Frame Advance", &upd->m_paused, theme, anim)) {
        }
        if (upd->m_paused) {
            if (Widgets::StyledButton("<< Back", ImVec2(bw, 24), theme, anim, 4.f)) {
                if (upd->m_backwardsStepping)
                    upd->backwardsStep(1);
            }
            ImGui::SameLine(0, 6);
            if (Widgets::StyledButton("Step >>", ImVec2(bw, 24), theme, anim, 4.f))
                upd->m_stepOnce_ = true;
        }
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Separator();

        if (Widgets::ToggleSwitch("Noclip", &engine->noclipEnabled, theme, anim))
            mod->setSavedValue("hack_noclip", engine->noclipEnabled);
        if (Widgets::ToggleSwitch("Layout Mode", &engine->layoutMode, theme, anim))
            mod->setSavedValue("hack_layout_mode", engine->layoutMode);
        if (Widgets::ToggleSwitch("Show Hitboxes", &engine->showHitboxes, theme, anim))
            mod->setSavedValue("hack_hitboxes", engine->showHitboxes);
        if (Widgets::ToggleSwitch("No Mirror", &engine->noMirrorEffect, theme, anim))
            mod->setSavedValue("hack_no_mirror", engine->noMirrorEffect);
        if (Widgets::ToggleSwitch(
                "Swap Player Inputs", &engine->replay.m_mirrorInputs, theme, anim)) {
        }

        ImGui::End();
        ImGui::PopStyleVar();
    }

    void MenuInterface::drawReplayTab() {
        auto* engine = GucciEngine::get();
        if (auto* replayCustom = getActiveCustomTheme())
            Widgets::GucciQuote(replayCustom->quoteReplay.text.c_str(),
                                replayCustom->quoteReplay.attribution.c_str(),
                                theme);
        else if ((activeTheme == THEME_TOOSII || activeTheme == THEME_TOOSII_SYRACUSE ||
                  activeTheme == THEME_TOOSII_SACSTATE))
            Widgets::GucciQuote("\"What's cover 1?\"", "-- Toosii, asking the cornerback", theme);
        else if (activeTheme == THEME_JA)
            Widgets::GucciQuote(
                "\"Nobody can replay what I just did. Nobody.\"", " -- Ja Morant", theme);
        else if (activeTheme == THEME_GIDDEY)
            Widgets::GucciQuote("\"6 was a little high; I was expecting to be in the 7-13 range.\"",
                                "-- Josh Giddey, on being the 6th pick",
                                theme);
        else if (activeTheme == THEME_BAM)
            Widgets::GucciQuote("\"I don't record inputs. I record history. 83 points of it.\"",
                                "-- Bam, in the zone",
                                theme);
        else if (activeTheme == THEME_SEXYY)
            Widgets::GucciQuote(
                "\"I don't miss. Not a single frame. Skee yee.\"", "-- Sexyy Red, probably", theme);
        else if (activeTheme == THEME_JUICE)
            Widgets::GucciQuote(
                "\"I tested every frame. Every single one.\"", "-- Juice, probably", theme);
        else if (activeTheme == THEME_BUTLER)
            Widgets::GucciQuote(
                "\"Regular season replays don't count. I lock in for the playoffs.\"",
                "-- Jimmy Butler, probably",
                theme);
        else if (activeTheme == THEME_SAWEETIE)
            Widgets::GucciQuote(
                "\"I don't miss. I'm too expensive to miss.\"", "-- Saweetie, probably", theme);
        else if (activeTheme == THEME_MAYBACH)
            Widgets::GucciQuote("\"Every replay a hit. Every frame a boss move.\"",
                                "-- Rick Ross, probably",
                                theme);
        else if (activeTheme == THEME_ROMO)
            Widgets::GucciQuote("\"I called that replay before it even happened.\"",
                                "-- Tony Romo, probably",
                                theme);
        else if (activeTheme == THEME_GRIZZLEY)
            Widgets::GucciQuote(
                "\"First day out, first frame perfect.\"", "-- Tee Grizzley, probably", theme);
        else if (activeTheme == THEME_REDKINGDOM)
            Widgets::GucciQuote(
                "\"I don't practice. I conquer.\"", "-- Tech N9ne, probably", theme);
        else if (activeTheme == THEME_LEMONADE)
            Widgets::GucciQuote(
                "\"I squeeze every frame till it's sweet.\"", "-- Gucci Mane, probably", theme);
        else if (activeTheme == THEME_BRRR)
            Widgets::GucciQuote(
                "\"Cold enough to freeze a frame in place.\"", "-- Gucci Mane, probably", theme);
        else if (activeTheme == THEME_WAKA)
            Widgets::GucciQuote(
                "\"I don't walk in, I turn up in.\"", "-- Waka Flocka Flame, probably", theme);
        else if (activeTheme == THEME_YOUNGSTA)
            Widgets::GucciQuote(
                "\"Everyday my birthday. Every frame a gift.\"", "-- Blac Youngsta, probably", theme);
        else if (activeTheme == THEME_KNOCKERZ)
            Widgets::GucciQuote("\"Every step's a two step. Every frame's a step ahead.\"",
                                "-- Speaker Knockerz, probably",
                                theme);
        else
            Widgets::GucciQuote("\"I got so many replays I got files in my files.\"",
                                "-- Gucci Mane, probably",
                                theme);
        Widgets::SectionHeader("Mode", theme);
        float pillW = (ImGui::GetContentRegionAvail().x - 20) / 3.f;
        static bool showFormatPopup = false;

        if (Widgets::PillButton("Disable", engine->isIdle(), pillW, theme, anim)) {
            if (engine->isRecording()) {
            }
            engine->setMode(GucciEngine::Mode::Idle);
            engine->startPosWarning.clear();
        }
        ImGui::SameLine(0, 10);

        if (Widgets::PillButton("Record", engine->isRecording(), pillW, theme, anim)) {
            if (engine->isRecording()) {
                engine->setMode(GucciEngine::Mode::Idle);
            } else if (engine->isPlaying() && !engine->replay.m_actionAtom.empty()) {
                if (engine->beginResumeRecording()) {
                    anim.closing = true;
                    anim.opening = false;
                }
            } else {
                engine->replay.m_actionAtom.clear();
                engine->replay.m_pathSamples.clear();
                engine->replay.m_inputIndex = 0;
                engine->updater.resetFrame();
                engine->updater.m_frameOnLastAttempt = 0;
                engine->setMode(GucciEngine::Mode::Recording);
                anim.closing = true;
                anim.opening = false;
            }
        }
        ImGui::SameLine(0, 10);

        bool playbackActive = engine->isPlaying();
        if (Widgets::PillButton("Playback", playbackActive, pillW, theme, anim)) {
            if (playbackActive) {
                engine->setMode(GucciEngine::Mode::Idle);
            } else if (!engine->replay.m_actionAtom.m_actions.empty()) {
                engine->updater.resetFrame();
                engine->updater.m_frameOnLastAttempt = 0;
                engine->replay.m_inputIndex = 0;
                engine->setMode(GucciEngine::Mode::Playing);
                anim.closing = true;
                anim.opening = false;
            }
        }
        if (ImGui::BeginPopup("##FmtSel",
                              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
            drawPopupChrome(*this, "Select Format");
            float bw = 110.f;
            std::string nativeLabelStr = currentThemeExtension(this);
            const char* nativeLabel = nativeLabelStr.c_str();
            if (Widgets::StyledButton(nativeLabel, ImVec2(bw, 30), theme, anim, 6.f)) {
                if (PlayLayer::get())
                    engine->setMode(GucciEngine::Mode::Recording);
                else
                    engine->setMode(GucciEngine::Mode::Recording);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::Dummy(ImVec2(0, 8));

        if (engine->isRecording()) {
            size_t cnt = engine->replay.m_actionAtom.m_actions.size();
            std::string extLabelStr = currentThemeExtension(this);
            const char* extLabel = extLabelStr.c_str();
            Widgets::StatusBadge("RECORDING", ImVec4(1.f, 0.3f, 0.3f, 1.f));
            ImGui::SameLine();
            Widgets::StatusBadge(extLabel, getBRRTagColor());
            ImGui::SameLine();
            ImGui::Text("Actions: %zu", cnt);
            ImGui::Dummy(ImVec2(0, 4));

            bool pending = engine->updater.m_canDie;
            if (pending) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.15f, 0.15f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 0.25f, 0.25f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.1f, 0.1f, 1.f));
            }
            if (Widgets::StyledButton(pending ? "Cancel Intentional Death"
                                              : "Mark Next Death as Intentional",
                                      ImVec2(-1, 30),
                                      theme,
                                      anim))
                engine->updater.m_canDie = !engine->updater.m_canDie;
            if (pending) {
                ImGui::PopStyleColor(3);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.7f, 0.1f, 1.f));
                ImGui::TextWrapped(
                    "Next player death will be recorded as intentional. Playback will "
                    "continue to the next attempt.");
                ImGui::PopStyleColor();
            }

            ImGui::Dummy(ImVec2(0, 4));
            if (Widgets::StyledButton("Record TPS Change", ImVec2(-1, 28), theme, anim)) {
                engine->recordTpsChange(engine->updater.m_tps);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped("Inserts a TPS change action at the current frame. Change TPS in "
                               "the Tools tab first.");
            ImGui::PopStyleColor();

            ImGui::Dummy(ImVec2(0, 4));
            if (!macroNameReady) {
                strncpy(macroNameBuffer, engine->replayName.c_str(), sizeof(macroNameBuffer) - 1);
                macroNameBuffer[sizeof(macroNameBuffer) - 1] = 0;
                macroNameReady = true;
            }
            ImGui::Text("Macro Name:");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##recName", macroNameBuffer, sizeof(macroNameBuffer)))
                engine->replayName = macroNameBuffer;
            ImGui::Dummy(ImVec2(0, 4));
            float bw = (ImGui::GetContentRegionAvail().x - 10) / 2.f;
            if (Widgets::StyledButton("Save Macro", ImVec2(bw, 30), theme, anim)) {
                if (!engine->replay.m_actionAtom.m_actions.empty()) {
                    auto savePath =
                        Mod::get()->getSaveDir() / "replays" / (engine->replayName + extLabel);
                    if (engine->replayBackupsEnabled)
                        engine->replay.backupExisting(savePath);
                    engine->replay.save(savePath);
                    markReplayListDirty();
                    refreshReplayListIfNeeded(true);
                    ImGui::OpenPopup("SaveFrameWindows");
                }
            }
            if (ImGui::BeginPopupModal("SaveFrameWindows",
                                       nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoTitleBar)) {
                if (fontHeading)
                    ImGui::PushFont(fontHeading);
                ImGui::PushStyleColor(ImGuiCol_Text, theme.getAccent());
                ImGui::TextUnformatted("Macro Saved");
                ImGui::PopStyleColor();
                if (fontHeading)
                    ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, 6));
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.f);
                ImGui::TextUnformatted(
                    "Calculate frame windows for this macro? This replays the macro and simulates "
                    "each "
                    "click against the real engine to measure how tight it is. You must be in the "
                    "level. May take a moment for long macros.");
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 6));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.8f, 0.2f, 1.f));
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.f);
                ImGui::TextUnformatted(
                    "Known limitation: the replay itself is accurate now (2026-08-13, "
                    "ground-truth-forced), but each click's tested timing shifts still fall back "
                    "to "
                    "real simulation past that click -- so windows near tricky slope sections may "
                    "still read off.");
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 10));
                float pbw = (ImGui::GetContentRegionAvail().x - 8) / 2.f;
                if (Widgets::StyledButton("Calculate", ImVec2(pbw, 30), theme, anim, 6.f)) {
                    engine->analyzeFrameWindows();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine(0, 8);
                if (Widgets::StyledButton("Skip", ImVec2(pbw, 30), theme, anim, 6.f))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::SameLine(0, 10);
            if (Widgets::StyledButton("Stop", ImVec2(bw, 30), theme, anim)) {
                engine->setMode(GucciEngine::Mode::Idle);
                macroNameReady = false;
            }
            ImGui::Dummy(ImVec2(0, 4));
        } else
            macroNameReady = false;

        if (engine->isPlaying() && !engine->replay.m_actionAtom.m_actions.empty()) {
            size_t cnt = engine->replay.m_actionAtom.m_actions.size();
            std::string nm = engine->replayName;
            std::string extLabel2Str = currentThemeExtension(this);
            const char* extLabel2 = extLabel2Str.c_str();
            Widgets::StatusBadge("PLAYING", ImVec4(0.3f, 1.f, 0.3f, 1.f));
            ImGui::SameLine();
            Widgets::StatusBadge(extLabel2, getBRRTagColor());
            ImGui::SameLine();
            ImGui::Text("%s | Actions: %zu", nm.c_str(), cnt);
            ImGui::Dummy(ImVec2(0, 4));
            if (Widgets::StyledButton("Stop Playback", ImVec2(-1, 30), theme, anim))
                engine->setMode(GucciEngine::Mode::Idle);
            ImGui::Dummy(ImVec2(0, 4));
            float pbw2 = (ImGui::GetContentRegionAvail().x - 8) / 2.f;
            if (Widgets::StyledButton("Save", ImVec2(pbw2, 28), theme, anim)) {
                auto savePath =
                    Mod::get()->getSaveDir() / "replays" / (engine->replayName + extLabel2);
                if (engine->replayBackupsEnabled)
                    engine->replay.backupExisting(savePath);
                engine->replay.save(savePath);
                markReplayListDirty();
                refreshReplayListIfNeeded(true);
                Notification::create("Macro saved", NotificationIcon::Success)->show();
            }
            ImGui::SameLine(0, 8);
            bool canCalc = PlayLayer::get() != nullptr;
            if (!canCalc)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
            bool calcClicked = Widgets::StyledButton("Calculate", ImVec2(pbw2, 28), theme, anim);
            if (!canCalc)
                ImGui::PopStyleVar();
            if (calcClicked && canCalc)
                engine->analyzeFrameWindows();
            if (!canCalc) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped("Enter the level to Calculate.");
                ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, 4));
        }
        if (!engine->startPosWarning.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.8f, 0.2f, 1.f));
            ImGui::TextWrapped("%s", engine->startPosWarning.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 4));
        }

        Widgets::SectionHeader("Saved Replays", theme);
        static char macroFilter[64] = "";
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint(
            "##macroSearch", "Search macros...", macroFilter, sizeof(macroFilter));
        auto matchesFilter = [&](const std::string& nm) -> bool {
            if (macroFilter[0] == 0)
                return true;
            std::string a = nm, b = macroFilter;
            std::transform(a.begin(), a.end(), a.begin(), ::tolower);
            std::transform(b.begin(), b.end(), b.begin(), ::tolower);
            return a.find(b) != std::string::npos;
        };
        refreshReplayListIfNeeded(false);
        float listPadY = 8.f, listPadX = 10.f;
        float listH = std::max(
            80.f, std::min(200.f, (float)engine->storedMacros.size() * 28.f + listPadY * 2));
        ImVec2 listPos = ImGui::GetCursorScreenPos();
        float listW = ImGui::GetContentRegionAvail().x;
        drawSolidRect(ImGui::GetWindowDrawList(),
                      listPos,
                      ImVec2(listPos.x + listW, listPos.y + listH),
                      theme.cornerRadius,
                      theme,
                      0.55f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.f);
        ImGui::BeginChild("##MacroList", ImVec2(-1, listH), false);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + listPadX);
        ImGui::Dummy(ImVec2(0, listPadY));
        if (engine->storedMacros.empty()) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + listPadX);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.4f));
            const char* emptyMsg = (activeTheme == THEME_TOOSII)
                                       ? "No saved replays -- get to work"
                                       : "No saved replays -- get to work";
            ImGui::Text("%s", emptyMsg);
            ImGui::PopStyleColor();
        }
        auto macroCopy = engine->storedMacros;
        for (const auto& mn : macroCopy) {
            if (!matchesFilter(mn))
                continue;
            bool isSel = (!engine->replay.m_actionAtom.m_actions.empty() &&
                          !engine->isRecording() && engine->replayName == mn);
            bool isIncompat = engine->incompatibleMacros.count(mn) > 0;

            ImGui::PushID(mn.c_str());
            const float xBtnW = 20.f, dotsBtnW = 22.f, btnGap = 3.f;
            float rowH = ImGui::GetTextLineHeight() + 8.f, fullW = ImGui::GetContentRegionAvail().x;
            float rightReserved = xBtnW + dotsBtnW + btnGap + listPadX;
            std::string rowLbl = mn;
            float accW = 0.f;
            std::string fmtTagStr = ".gdr";
            if (true) {
                auto* eng3 = GucciEngine::get();
                bool foundCustom = false;
                for (auto& [ext, set] : eng3->customThemeMacrosByExt) {
                    if (set.count(mn)) {
                        fmtTagStr = "." + ext;
                        foundCustom = true;
                        break;
                    }
                }
                if (foundCustom) {
                } else if (eng3->jaMacros.count(mn))
                    fmtTagStr = ".ja";
                else if (eng3->giddeyMacros.count(mn))
                    fmtTagStr = ".giddey";
                else if (eng3->toosiiMacros.count(mn))
                    fmtTagStr = ".toosii";
                else if (eng3->bamMacros.count(mn))
                    fmtTagStr = ".bam";
                else if (eng3->sexyyMacros.count(mn))
                    fmtTagStr = ".sexyy";
                else if (eng3->juiceMacros.count(mn))
                    fmtTagStr = ".juice";
                else if (eng3->butlerMacros.count(mn))
                    fmtTagStr = ".butler";
                else if (eng3->saweetieMacros.count(mn))
                    fmtTagStr = ".saweetie";
                else if (eng3->maybachMacros.count(mn))
                    fmtTagStr = ".maybach";
                else if (eng3->romoMacros.count(mn))
                    fmtTagStr = ".romo";
                else if (eng3->grizzleyMacros.count(mn))
                    fmtTagStr = ".grizzley";
                else if (eng3->redKingdomMacros.count(mn))
                    fmtTagStr = ".redkingdom";
                else if (eng3->lemonadeMacros.count(mn))
                    fmtTagStr = ".lemonade";
                else if (eng3->brrrMacros.count(mn))
                    fmtTagStr = ".icebrrr";
                else if (eng3->wakaMacros.count(mn))
                    fmtTagStr = ".waka";
                else if (eng3->youngstaMacros.count(mn))
                    fmtTagStr = ".youngsta";
                else if (eng3->knockerzMacros.count(mn))
                    fmtTagStr = ".knockerz";
                else
                    fmtTagStr = ".brrr";
            }
            const char* fmtTag = fmtTagStr.c_str();
            float fmtW = (!isIncompat) ? (ImGui::CalcTextSize(fmtTag).x + 8) : 0;
            float inW = isIncompat ? (ImGui::CalcTextSize("Incompatible").x + 8) : 0;
            float maxNW =
                std::max(40.f, fullW - rightReserved - accW - fmtW - inW - listPadX - 8.f);
            if (ImGui::CalcTextSize(rowLbl.c_str()).x > maxNW) {
                while (!rowLbl.empty() && ImGui::CalcTextSize((rowLbl + "...").c_str()).x > maxNW)
                    rowLbl.pop_back();
                rowLbl += "...";
            }
            ImGui::SetCursorPosX(ImGui::GetCursorPosX());
            ImVec2 rowStart = ImGui::GetCursorScreenPos();
            if (isIncompat)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.4f));
            bool rowAct = ImGui::Selectable(
                "##row", isSel, ImGuiSelectableFlags_AllowOverlap, ImVec2(fullW, rowH));
            bool rowDbl = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
            bool rowRC = ImGui::IsItemClicked(1);
            if (isIncompat)
                ImGui::PopStyleColor();
            if (rowDbl || rowRC) {
                replayActionMacroName = mn;
                replayActionPopupRequested = true;
            } else if (!isIncompat && rowAct && !engine->isRecording()) {

                {
                    std::string extFound;
                    auto dir = Mod::get()->getSaveDir() / "replays";
                    for (auto& ext : allKnownMacroExtensions()) {
                        if (std::filesystem::exists(dir / (mn + ext))) {
                            extFound = ext;
                            break;
                        }
                    }
                    if (!extFound.empty()) {
                        engine->replay.load(dir / (mn + extFound));
                        engine->replayName = mn;
                    } else if (isIncompat) {
                        engine->convertToBRR(mn);
                        engine->replayName = mn;
                    }
                }
            }
            float iy = rowStart.y, ih = rowH;
            auto* wdl = ImGui::GetWindowDrawList();
            wdl->AddText(
                ImVec2(rowStart.x + listPadX, iy + (ih - ImGui::GetTextLineHeight()) * 0.5f),
                isIncompat ? toU32(ImVec4(1, 1, 1, 0.4f)) : theme.getTextU32(),
                rowLbl.c_str());
            float tagX = rowStart.x + fullW - rightReserved - 4.f;
            if (isIncompat) {
                auto ts = ImGui::CalcTextSize("Incompatible");
                tagX -= ts.x + 4;
                wdl->AddText(ImVec2(tagX, iy + (ih - ts.y) * 0.5f),
                             toU32(ImVec4(1.f, 0.2f, 0.2f, 1.f)),
                             "Incompatible");
            }
            if (!isIncompat) {
                std::string tagStr = ".brrr";
                ImVec4 tagCol = getBRRTagColor();
                auto* eng2 = GucciEngine::get();
                bool foundCustomTag = false;
                for (auto& [ext, set] : eng2->customThemeMacrosByExt) {
                    if (!set.count(mn))
                        continue;
                    tagStr = "." + ext;
                    for (auto& ct : customThemes)
                        if (ct.extension == ext) {
                            tagCol = ct.accent;
                            break;
                        }
                    foundCustomTag = true;
                    break;
                }
                if (foundCustomTag) {
                } else if (eng2->jaMacros.count(mn)) {
                    tagStr = ".ja";
                    tagCol = ImVec4(0.42f, 0.78f, 0.95f, 1.f);
                } else if (eng2->giddeyMacros.count(mn)) {
                    tagStr = ".giddey";
                    tagCol = ImVec4(1.000f, 0.310f, 0.106f, 1.f);
                } else if (eng2->toosiiMacros.count(mn)) {
                    tagStr = ".toosii";
                    tagCol = ImVec4(0.99f, 0.82f, 0.14f, 1.f);
                } else if (eng2->bamMacros.count(mn)) {
                    tagStr = ".bam";
                    tagCol = ImVec4(0.878f, 0.067f, 0.153f, 1.f);
                } else if (eng2->sexyyMacros.count(mn)) {
                    tagStr = ".sexyy";
                    tagCol = ImVec4(0.910f, 0.004f, 0.580f, 1.f);
                } else if (eng2->juiceMacros.count(mn)) {
                    tagStr = ".juice";
                    tagCol = ImVec4(0.960f, 0.520f, 0.380f, 1.f);
                } else if (eng2->butlerMacros.count(mn)) {
                    tagStr = ".butler";
                    tagCol = ImVec4(0.808f, 0.067f, 0.255f, 1.f);
                } else if (eng2->saweetieMacros.count(mn)) {
                    tagStr = ".saweetie";
                    tagCol = ImVec4(1.000f, 0.180f, 0.520f, 1.f);
                } else if (eng2->maybachMacros.count(mn)) {
                    tagStr = ".maybach";
                    tagCol = ImVec4(0.780f, 0.780f, 0.800f, 1.f);
                } else if (eng2->romoMacros.count(mn)) {
                    tagStr = ".romo";
                    tagCol = ImVec4(0.760f, 0.800f, 0.850f, 1.f);
                } else if (eng2->grizzleyMacros.count(mn)) {
                    tagStr = ".grizzley";
                    tagCol = ImVec4(0.870f, 0.090f, 0.070f, 1.f);
                } else if (eng2->redKingdomMacros.count(mn)) {
                    tagStr = ".redkingdom";
                    tagCol = ImVec4(0.820f, 0.035f, 0.035f, 1.f);
                } else if (eng2->lemonadeMacros.count(mn)) {
                    tagStr = ".lemonade";
                    tagCol = ImVec4(0.980f, 0.851f, 0.145f, 1.f);
                } else if (eng2->brrrMacros.count(mn)) {
                    tagStr = ".icebrrr";
                    tagCol = ImVec4(0.580f, 0.850f, 0.980f, 1.f);
                } else if (eng2->wakaMacros.count(mn)) {
                    tagStr = ".waka";
                    tagCol = ImVec4(0.204f, 0.780f, 0.302f, 1.f);
                } else if (eng2->youngstaMacros.count(mn)) {
                    tagStr = ".youngsta";
                    tagCol = ImVec4(1.000f, 0.000f, 0.000f, 1.f);
                } else if (eng2->knockerzMacros.count(mn)) {
                    tagStr = ".knockerz";
                    tagCol = ImVec4(0.070f, 0.780f, 0.720f, 1.f);
                }
                const char* tag = tagStr.c_str();
                auto ts = ImGui::CalcTextSize(tag);
                tagX -= ts.x + 4;
                wdl->AddText(ImVec2(tagX, iy + (ih - ts.y) * 0.5f), toU32(tagCol), tag);
            }
            float btnY = iy + (ih - xBtnW) * 0.5f;
            float xBtnX = rowStart.x + fullW - xBtnW - listPadX;
            float dotsBtnX = xBtnX - dotsBtnW - btnGap;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::SetCursorScreenPos(ImVec2(dotsBtnX, btnY));
            if (ImGui::InvisibleButton("##dots", ImVec2(dotsBtnW, xBtnW))) {
                replayActionMacroName = mn;
                replayActionPopupRequested = true;
            }
            bool dotsHov = ImGui::IsItemHovered();
            wdl->AddRectFilled(ImVec2(dotsBtnX, btnY),
                               ImVec2(dotsBtnX + dotsBtnW, btnY + xBtnW),
                               dotsHov ? theme.getAccentU32(0.15f) : IM_COL32(0, 0, 0, 0),
                               3.f);
            ImVec2 dts = ImGui::CalcTextSize("...");
            wdl->AddText(
                ImVec2(dotsBtnX + (dotsBtnW - dts.x) * 0.5f, btnY + (xBtnW - dts.y) * 0.5f),
                dotsHov ? theme.getAccentU32() : theme.getTextSecondaryU32(),
                "...");
            ImGui::SetCursorScreenPos(ImVec2(xBtnX, btnY));
            if (ImGui::InvisibleButton("##del", ImVec2(xBtnW, xBtnW))) {
                auto dir = getReplayDir();
                std::string fullMacroName; // e.g. "xyz.brrr" -- sidecars are named after this
                for (auto& e : std::filesystem::directory_iterator(dir)) {
                    if (e.is_regular_file() && e.path().stem().string() == mn) {
                        fullMacroName = e.path().filename().string();
                        std::filesystem::remove(e.path());
                        break;
                    }
                }
                // Also remove this macro's .fw/.path/.trainer sidecars -- see
                // removeSidecarsFor's comment. This used to only delete the
                // macro itself, silently leaving its sidecars behind
                // forever, which was itself part of why the macro folder
                // accumulated clutter over time.
                if (!fullMacroName.empty())
                    removeSidecarsFor(dir, fullMacroName);
                markReplayListDirty();
                refreshReplayListIfNeeded(true);
            }
            bool delHov = ImGui::IsItemHovered();
            wdl->AddRectFilled(ImVec2(xBtnX, btnY),
                               ImVec2(xBtnX + xBtnW, btnY + xBtnW),
                               delHov ? IM_COL32(200, 50, 50, 60) : IM_COL32(0, 0, 0, 0),
                               3.f);
            ImVec2 xts = ImGui::CalcTextSize("x");
            wdl->AddText(ImVec2(xBtnX + (xBtnW - xts.x) * 0.5f, btnY + (xBtnW - xts.y) * 0.5f),
                         delHov ? IM_COL32(255, 100, 100, 255) : theme.getTextSecondaryU32(),
                         "x");
            ImGui::PopStyleVar();
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        if (replayActionPopupRequested) {
            ImGui::OpenPopup("MacroActions");
            replayActionPopupRequested = false;
        }
        if (replayRenamePopupRequested) {
            ImGui::OpenPopup("RenameReplay");
            replayRenamePopupRequested = false;
        }
        ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_Appearing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 0));
        if (ImGui::BeginPopupModal("MacroActions",
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoResize)) {
            drawPopupChrome(*this, "Macro Actions");
            ImGui::TextColored(theme.getAccent(), "%s", replayActionMacroName.c_str());
            ImGui::Dummy(ImVec2(0, 6));
            float aw = 230.f;
            if (Widgets::StyledButton("Rename##ar", ImVec2(aw, 30), theme, anim, 6.f)) {
                replayRenameOriginalName = replayActionMacroName;
                strncpy(replayRenameBuffer,
                        replayActionMacroName.c_str(),
                        sizeof(replayRenameBuffer) - 1);
                replayRenameError.clear();
                replayRenameFocusInput = true;
                ImGui::CloseCurrentPopup();
                replayRenamePopupRequested = true;
            }
            ImGui::Dummy(ImVec2(0, 4));
            {
                bool canEdit_unused = true;
                if (false) {
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
                    Widgets::StyledButton(
                        "Open Frame Editor##ae", ImVec2(aw, 30), theme, anim, 6.f);
                    ImGui::PopStyleVar();
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y + 34),
                        IM_COL32(255, 180, 80, 200),
                        "CBS/CBF macros cannot be edited");
                } else if (Widgets::StyledButton(
                               "Open Frame Editor##ae", ImVec2(aw, 30), theme, anim, 6.f)) {
                    BRRMacro* loaded = BRRMacro::loadFromDisk(replayActionMacroName);
                    if (loaded) {
                        frameEditor.openBRR(replayActionMacroName, loaded);
                        delete loaded;
                    }
                    ImGui::CloseCurrentPopup();
                }
            }

            // Convert to .brrr -- for macros saved under another GucciBot
            // theme's extension (.toosii, .icebrrr, ...). Those are already
            // BRR data, so this is purely a rename; no format conversion is
            // involved. Foreign formats (.gdr/.xd/.json) are NOT handled here
            // -- clicking an "Incompatible" row already converts those.
            {
                auto rdir = Mod::get()->getSaveDir() / "replays";
                std::error_code exEc;
                std::filesystem::path themeFile;
                for (auto& ext : allKnownMacroExtensions()) {
                    auto cand = rdir / (replayActionMacroName + ext);
                    if (std::filesystem::exists(cand, exEc)) {
                        themeFile = cand;
                        break;
                    }
                }
                if (!themeFile.empty() && themeFile.extension() != ".brrr") {
                    ImGui::Dummy(ImVec2(0, 4));
                    if (Widgets::StyledButton(
                            "Convert to .brrr##acv", ImVec2(aw, 30), theme, anim, 6.f)) {
                        auto dest = rdir / (replayActionMacroName + ".brrr");
                        std::error_code destEc;
                        if (std::filesystem::exists(dest, destEc)) {
                            Notification::create("A .brrr with that name already exists",
                                                 NotificationIcon::Warning)
                                ->show();
                        } else {
                            std::error_code mvEc;
                            std::filesystem::rename(themeFile, dest, mvEc);
                            if (!mvEc) {
                                // Carry the sidecars (<name>.<ext>.fw/.path/
                                // .trainer/.bak) across too, or they'd be
                                // orphaned by the rename.
                                std::string oldBase = themeFile.filename().string();
                                std::string newBase = dest.filename().string();
                                std::error_code itEc;
                                std::vector<std::pair<std::filesystem::path,
                                                      std::filesystem::path>> sidecars;
                                for (auto& it : std::filesystem::directory_iterator(rdir, itEc)) {
                                    if (itEc)
                                        break;
                                    if (!it.is_regular_file())
                                        continue;
                                    auto fn = it.path().filename().string();
                                    if (fn.size() > oldBase.size() + 1 &&
                                        fn.compare(0, oldBase.size(), oldBase) == 0 &&
                                        fn[oldBase.size()] == '.') {
                                        sidecars.emplace_back(
                                            it.path(), rdir / (newBase + fn.substr(oldBase.size())));
                                    }
                                }
                                for (auto& [from, to] : sidecars) {
                                    std::error_code scEc;
                                    std::filesystem::rename(from, to, scEc);
                                }
                                Notification::create("Converted to .brrr", NotificationIcon::Success)
                                    ->show();
                            } else {
                                Notification::create("Couldn't rename that macro",
                                                     NotificationIcon::Error)
                                    ->show();
                            }
                        }
                        markReplayListDirty();
                        refreshReplayListIfNeeded(true);
                        ImGui::CloseCurrentPopup();
                    }
                }
            }

            // Run Calculate straight from here instead of making the user
            // load the macro first and then go find the button.
            {
                bool inLevel = PlayLayer::get() != nullptr;
                ImGui::Dummy(ImVec2(0, 4));
                if (!inLevel)
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
                bool calcClicked =
                    Widgets::StyledButton("Calculate##acalc", ImVec2(aw, 30), theme, anim, 6.f);
                if (!inLevel)
                    ImGui::PopStyleVar();
                if (calcClicked && inLevel) {
                    auto rdir = Mod::get()->getSaveDir() / "replays";
                    std::string extFound;
                    std::error_code exEc;
                    for (auto& ext : allKnownMacroExtensions()) {
                        if (std::filesystem::exists(rdir / (replayActionMacroName + ext), exEc)) {
                            extFound = ext;
                            break;
                        }
                    }
                    if (!extFound.empty()) {
                        engine->replay.load(rdir / (replayActionMacroName + extFound));
                        engine->replayName = replayActionMacroName;
                        engine->analyzeFrameWindows();
                    } else {
                        Notification::create("Convert this macro first", NotificationIcon::Warning)
                            ->show();
                    }
                    ImGui::CloseCurrentPopup();
                }
                if (!inLevel) {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                    ImGui::TextWrapped("Open a level to run Calculate.");
                    ImGui::PopStyleColor();
                }
            }

            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.18f, 0.18f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.08f, 0.08f, 1.f));
            if (Widgets::StyledButton("Delete##ad", ImVec2(aw, 30), theme, anim, 6.f)) {
                replayDeleteName = replayActionMacroName;
                replayDeleteError.clear();
                ImGui::CloseCurrentPopup();
                replayDeletePopupRequested = true;
            }
            ImGui::PopStyleColor(3);

            ImGui::Dummy(ImVec2(0, 6));
            if (Widgets::StyledButton("Cancel##ac", ImVec2(aw, 28), theme, anim, 6.f))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);
        if (replayDeletePopupRequested) {
            ImGui::OpenPopup("DeleteReplay");
            replayDeletePopupRequested = false;
        }
        ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_Appearing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 0));
        if (ImGui::BeginPopupModal("DeleteReplay",
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoResize)) {
            drawPopupChrome(*this, "Delete Replay");
            ImGui::Text("Permanently delete:");
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", replayDeleteName.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped("This removes the file from disk. It cannot be undone.");
            ImGui::PopStyleColor();
            if (!replayDeleteError.empty()) {
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "%s", replayDeleteError.c_str());
            }
            ImGui::Dummy(ImVec2(0, 10));
            float pbw = 125.f;
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.18f, 0.18f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.08f, 0.08f, 1.f));
            bool confirmDel =
                Widgets::StyledButton("Delete##cd", ImVec2(pbw, 28), theme, anim, 6.f);
            ImGui::PopStyleColor(3);
            ImGui::SameLine(0, 8);
            bool cancelDel = Widgets::StyledButton("Cancel##cd", ImVec2(pbw, 28), theme, anim, 6.f);
            if (confirmDel) {
                if (deleteStoredReplay(replayDeleteName, replayDeleteError)) {
                    auto* eng4 = GucciEngine::get();
                    if (!eng4->isRecording() && eng4->replayName == replayDeleteName) {
                        eng4->replay.m_actionAtom.clear();
                        eng4->replayName.clear();
                    }
                    eng4->incompatibleMacros.erase(replayDeleteName);
                    eng4->jaMacros.erase(replayDeleteName);
                    eng4->giddeyMacros.erase(replayDeleteName);
                    eng4->toosiiMacros.erase(replayDeleteName);
                    eng4->bamMacros.erase(replayDeleteName);
                    eng4->sexyyMacros.erase(replayDeleteName);
                    eng4->juiceMacros.erase(replayDeleteName);
                    eng4->butlerMacros.erase(replayDeleteName);
                    eng4->saweetieMacros.erase(replayDeleteName);
                    eng4->maybachMacros.erase(replayDeleteName);
                    eng4->romoMacros.erase(replayDeleteName);
                    eng4->grizzleyMacros.erase(replayDeleteName);
                    eng4->redKingdomMacros.erase(replayDeleteName);
                    eng4->lemonadeMacros.erase(replayDeleteName);
                    eng4->brrrMacros.erase(replayDeleteName);
                    for (auto& [ext, set] : eng4->customThemeMacrosByExt)
                        set.erase(replayDeleteName);
                    replayDeleteName.clear();
                    replayDeleteError.clear();
                    markReplayListDirty();
                    refreshReplayListIfNeeded(true);
                    ImGui::CloseCurrentPopup();
                }
            }
            if (cancelDel) {
                replayDeleteName.clear();
                replayDeleteError.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);
        ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Appearing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 0));
        if (ImGui::BeginPopupModal("RenameReplay",
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoResize)) {
            drawPopupChrome(*this, "Rename Replay");
            ImGui::Text("Rename replay:");
            ImGui::TextColored(theme.getAccent(), "%s", replayRenameOriginalName.c_str());
            ImGui::Dummy(ImVec2(0, 6));
            if (replayRenameFocusInput) {
                ImGui::SetKeyboardFocusHere();
                replayRenameFocusInput = false;
            }
            bool submitted = ImGui::InputText("##renameR",
                                              replayRenameBuffer,
                                              sizeof(replayRenameBuffer),
                                              ImGuiInputTextFlags_AutoSelectAll |
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            if (!replayRenameError.empty()) {
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "%s", replayRenameError.c_str());
            }
            ImGui::Dummy(ImVec2(0, 10));
            float pbw = 110.f;
            bool confirm = Widgets::StyledButton("Rename##cr", ImVec2(pbw, 28), theme, anim, 6.f);
            ImGui::SameLine(0, 8);
            bool cancel = Widgets::StyledButton("Cancel##cr", ImVec2(pbw, 28), theme, anim, 6.f);
            if (confirm || submitted) {
                std::string renamedTo;
                if (renameStoredReplay(replayRenameOriginalName,
                                       replayRenameBuffer,
                                       renamedTo,
                                       replayRenameError)) {
                    auto* engine2 = GucciEngine::get();
                    if (!engine2->isRecording() && engine2->replayName == replayRenameOriginalName)
                        engine2->replayName = renamedTo;
                    replayRenameOriginalName.clear();
                    replayRenameError.clear();
                    replayRenameBuffer[0] = 0;
                    markReplayListDirty();
                    refreshReplayListIfNeeded(true);
                    ImGui::CloseCurrentPopup();
                }
            }
            if (cancel) {
                replayRenameOriginalName.clear();
                replayRenameError.clear();
                replayRenameBuffer[0] = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);
        ImGui::Dummy(ImVec2(0, 4));
        float bw = (ImGui::GetContentRegionAvail().x - 10) / 2.f;
        if (Widgets::StyledButton("Refresh", ImVec2(bw, 28), theme, anim)) {
            markReplayListDirty();
            refreshReplayListIfNeeded(true);
        }
        ImGui::SameLine(0, 10);
        if (Widgets::StyledButton("Open Folder", ImVec2(bw, 28), theme, anim)) {
            auto dir = getReplayDir();
            if (std::filesystem::exists(dir) || std::filesystem::create_directory(dir))
                utils::file::openFolder(dir);
        }

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Macro Diff", theme);
        static char diffNameA[128] = {}, diffNameB[128] = {};
        static std::vector<GucciEngine::DiffEntry> diffResults;
        static bool diffRun = false;
        ImGui::SetNextItemWidth((ImGui::GetContentRegionAvail().x - 10) / 2.f);
        ImGui::InputText("##diffA", diffNameA, sizeof(diffNameA));
        ImGui::SameLine(0, 10);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##diffB", diffNameB, sizeof(diffNameB));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextUnformatted("A                                          B");
        ImGui::PopStyleColor();
        if (Widgets::StyledButton("Compare", ImVec2(-1, 28), theme, anim) && diffNameA[0] &&
            diffNameB[0]) {
            diffResults = engine->diffMacros(diffNameA, diffNameB);
            diffRun = true;
        }
        if (diffRun) {
            if (diffResults.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1, 0.3f, 1));
                ImGui::TextUnformatted("No differences found.");
                ImGui::PopStyleColor();
            } else {
                char lbl[64];
                snprintf(lbl, sizeof(lbl), "%zu difference(s):", diffResults.size());
                ImGui::TextUnformatted(lbl);
                float listH = std::min((float)diffResults.size() * 18.f, 180.f);
                ImGui::BeginChild("##diffList", ImVec2(-1, listH), true);
                for (auto& d : diffResults) {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                    ImGui::TextUnformatted(d.description.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::EndChild();
            }
        }

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Macro Surgery", theme);
        static char trimName[128] = {};
        static int trimStart = 0, trimEnd = 0;
        static bool trimRebase = true;
        static int mergeGap = 0;
        static std::string surgeryStatus;
        float sgw = ImGui::GetContentRegionAvail().x;
        ImGui::Text("Trim");
        ImGui::SameLine(70);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##trimN", "macro name", trimName, sizeof(trimName));
        ImGui::Text("Range");
        ImGui::SameLine(70);
        ImGui::SetNextItemWidth((sgw - 70 - 8) * 0.5f);
        ImGui::InputInt("##trimS", &trimStart, 0, 0);
        ImGui::SameLine(0, 8);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##trimE", &trimEnd, 0, 0);
        Widgets::ToggleSwitch("Rebase to frame 0", &trimRebase, theme, anim);
        if (Widgets::StyledButton("Trim -> _trim", ImVec2(-1, 26), theme, anim) && trimName[0]) {
            surgeryStatus = engine->trimMacro(trimName, trimStart, trimEnd, trimRebase)
                                ? "Trim saved."
                                : "Trim failed (check name and range).";
            markReplayListDirty();
            refreshReplayListIfNeeded(true);
        }
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped("Merge appends B after A using the Compare fields above. Gap = extra "
                           "frames between them.");
        ImGui::PopStyleColor();
        ImGui::Text("Gap");
        ImGui::SameLine(70);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##mGap", &mergeGap, 0, 0);
        if (Widgets::StyledButton("Merge A + B -> _merged", ImVec2(-1, 26), theme, anim) &&
            diffNameA[0] && diffNameB[0]) {
            surgeryStatus = engine->mergeMacros(diffNameA, diffNameB, mergeGap)
                                ? "Merge saved."
                                : "Merge failed (check the Compare names).";
            markReplayListDirty();
            refreshReplayListIfNeeded(true);
        }
        if (!surgeryStatus.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextUnformatted(surgeryStatus.c_str());
            ImGui::PopStyleColor();
        }

        if (!engine->incompatibleMacros.empty()) {
            ImGui::Dummy(ImVec2(0, 8));
            Widgets::SectionHeader("Convert to BRR", theme);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped("These macros are in legacy format. Click to convert to BRR.");
            ImGui::PopStyleColor();
            for (auto& mn : engine->incompatibleMacros) {
                if (Widgets::StyledButton(
                        ("Convert: " + mn).c_str(), ImVec2(-1, 26), theme, anim)) {
                    engine->convertToBRR(mn);
                    markReplayListDirty();
                    refreshReplayListIfNeeded(true);
                }
            }
        }
        if (!engine->replay.m_actionAtom.m_actions.empty() && engine->isPlaying() &&
            PlayLayer::get()) {
            size_t cnt = engine->replay.m_actionAtom.m_actions.size();
            std::string nm = engine->replayName;
            ImGui::Dummy(ImVec2(0, 4));
            Widgets::SectionHeader("Loaded Macro", theme);
            ImGui::Text("Name: %s", nm.c_str());
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, getBRRTagColor());
            ImGui::TextUnformatted("(BRR)");
            ImGui::PopStyleColor();
            ImGui::Text("Actions: %zu", cnt);
            {
                std::string lvl = engine->loadedMacroLevelName;
                if (!lvl.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                    ImGui::Text("Level: %s", lvl.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::Text("TPS: %d", (int)engine->updater.m_tps);
                ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, 4));
            Widgets::ToggleSwitch(
                "Ignore Manual Input", &engine->replay.m_ignoreInputs, theme, anim);
            if (engine->replay.m_pathSamples.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(
                    "No path data for this macro -- only recorded going forward, not "
                    "backfilled for older macros.");
                ImGui::PopStyleColor();
            } else if (Widgets::ToggleSwitch(
                           "Show Macro Path", &engine->showMacroPath, theme, anim)) {
                Mod::get()->setSavedValue("hack_show_macro_path", engine->showMacroPath);
            }
            if (engine->showMacroPath && !engine->replay.m_pathSamples.empty()) {
                if (Widgets::StyledSliderFloat(
                        "Marker Size", &engine->macroPathMarkerSize, 3.f, 20.f, theme))
                    Mod::get()->setSavedValue("hack_macro_path_marker_size",
                                              (double)engine->macroPathMarkerSize);
                if (Widgets::StyledSliderFloat(
                        "Line Opacity", &engine->macroPathLineOpacity, 0.1f, 1.f, theme))
                    Mod::get()->setSavedValue("hack_macro_path_line_opacity",
                                              (double)engine->macroPathLineOpacity);
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(
                    "Line shows the recorded path. Squares mark clicks (and releases in "
                    "Wave/Robot/Ship) -- drawn with an inverted-colour blend so they "
                    "stay visible over any terrain.");
                ImGui::PopStyleColor();
            }
        }
    }

    void MenuInterface::drawToolsTab() {
        auto* engine = GucciEngine::get();
        if (auto* toolsCustom = getActiveCustomTheme())
            Widgets::GucciQuote(toolsCustom->quoteTools.text.c_str(),
                                toolsCustom->quoteTools.attribution.c_str(),
                                theme);
        else if ((activeTheme == THEME_TOOSII || activeTheme == THEME_TOOSII_SYRACUSE ||
                  activeTheme == THEME_TOOSII_SACSTATE))
            Widgets::GucciQuote("\"I run the route so fast the DB thinks I'm a speedhack.\"",
                                "-- Toosii, route running",
                                theme);
        else if (activeTheme == THEME_JA)
            Widgets::GucciQuote(
                "\"I don't use speedhack. That's just me.\"", " -- Ja Morant", theme);
        else if (activeTheme == THEME_GIDDEY)
            Widgets::GucciQuote(
                "\"Speed 1.0x seems fast enough. I'm still jetlagged.\"", " -- Josh Giddey", theme);
        else if (activeTheme == THEME_BAM)
            Widgets::GucciQuote("\"Speed? I hit 83 at my own pace. You can't guard that.\"",
                                "-- Bam, on speedhack",
                                theme);
        else if (activeTheme == THEME_SEXYY)
            Widgets::GucciQuote(
                "\"I run this at my own speed and it still goes stupid.\"", "-- Sexyy Red", theme);
        else if (activeTheme == THEME_JUICE)
            Widgets::GucciQuote("\"Speed doesn't mean much if the frame windows are wrong.\"",
                                "-- Juice, keeping you honest",
                                theme);
        else if (activeTheme == THEME_BUTLER)
            Widgets::GucciQuote(
                "\"I don't need speedhack. I just lock in.\"", "-- Jimmy Butler", theme);
        else if (activeTheme == THEME_SAWEETIE)
            Widgets::GucciQuote(
                "\"Fast money, fast frames. Tap in.\"", "-- Saweetie, on speedhack", theme);
        else if (activeTheme == THEME_MAYBACH)
            Widgets::GucciQuote("\"I don't rush. The Maybach arrives exactly on time.\"",
                                "-- Rick Ross, on speedhack",
                                theme);
        else if (activeTheme == THEME_ROMO)
            Widgets::GucciQuote("\"I don't need speedhack. I've had worse rides.\"",
                                "-- Tony Romo, probably",
                                theme);
        else if (activeTheme == THEME_GRIZZLEY)
            Widgets::GucciQuote("\"I don't need speedhack. I move different.\"",
                                "-- Tee Grizzley, probably",
                                theme);
        else if (activeTheme == THEME_REDKINGDOM)
            Widgets::GucciQuote("\"I don't need speedhack. I run the kingdom at my own pace.\"",
                                "-- Tech N9ne, probably",
                                theme);
        else if (activeTheme == THEME_LEMONADE)
            Widgets::GucciQuote(
                "\"I don't need speedhack. Lemonade's already sweet enough.\"",
                "-- Gucci Mane, probably",
                theme);
        else if (activeTheme == THEME_BRRR)
            Widgets::GucciQuote(
                "\"I don't need speedhack. Cold moves fast on its own.\"",
                "-- Gucci Mane, probably",
                theme);
        else if (activeTheme == THEME_WAKA)
            Widgets::GucciQuote(
                "\"I don't need speedhack. I'm already hard in the paint.\"",
                "-- Waka Flocka Flame, probably",
                theme);
        else if (activeTheme == THEME_YOUNGSTA)
            Widgets::GucciQuote("\"I don't need speedhack. I move like it's my birthday.\"",
                                "-- Blac Youngsta, probably",
                                theme);
        else if (activeTheme == THEME_KNOCKERZ)
            Widgets::GucciQuote("\"I don't need speedhack. I'm already a step ahead.\"",
                                "-- Speaker Knockerz, probably",
                                theme);
        else
            Widgets::GucciQuote("\"I run this game at my own speed. You can't keep up.\"",
                                "-- Gucci Mane, on speedhacks",
                                theme);
        Widgets::SectionHeader("TPS Control", theme);
        ImGui::TextColored(theme.getAccent(), "Current: %.0f TPS", engine->updater.m_tps);
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90);
        ImGui::InputFloat("##tps", &tempTickRate, 0, 0, "%.0f");
        ImGui::SameLine(0, 6);
        if (Widgets::StyledButton("Apply###tps", ImVec2(78, 28), theme, anim)) {
            bool can = !PlayLayer::get() || !engine->isPlaying();
            if (can) {
                engine->updater.m_tps = tempTickRate;
                Mod::get()->setSavedValue("eng_tick_rate", (float)engine->updater.m_tps);
            }
        }
        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Speed Control", theme);
        ImGui::TextColored(theme.getAccent(), "Current: %.2fx", engine->updater.m_speedhack);
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90);
        ImGui::InputFloat("##spd", &tempGameSpeed, 0, 0, "%.2f");
        ImGui::SameLine(0, 6);
        if (Widgets::StyledButton("Apply###spd", ImVec2(78, 28), theme, anim))
            engine->updater.m_speedhack = tempGameSpeed;
        ImGui::Dummy(ImVec2(0, 12));
        Widgets::SectionHeader("Features", theme);
        if (Widgets::ModuleCardBegin("Frame Advance",
                                     (activeTheme == THEME_TOOSII)
                                         ? "Frame advance, like a running back hitting the hole"
                                         : "Pause and step frame-by-frame",
                                     &engine->updater.m_paused,
                                     theme,
                                     anim,
                                     &keybinds.frameAdvance))
            Widgets::ModuleCardEnd();
        if (Widgets::ModuleCardBegin("Speedhack Audio",
                                     "Apply speed changes to game audio",
                                     &engine->audioPitchEnabled,
                                     theme,
                                     anim,
                                     &keybinds.audioPitch))
            Widgets::ModuleCardEnd();
        if (Widgets::ModuleCardBegin("Layout Mode",
                                     "Remove all decorations",
                                     &engine->layoutMode,
                                     theme,
                                     anim,
                                     &keybinds.layoutMode))
            Widgets::ModuleCardEnd();
        if (Widgets::ModuleCardBegin("No Mirror Effect",
                                     (activeTheme == THEME_TOOSII)
                                         ? "Can't tackle what you can't see. Block that mirror."
                                         : "Disable mirror portal visual flip",
                                     &engine->noMirrorEffect,
                                     theme,
                                     anim,
                                     &keybinds.noMirror)) {
            Widgets::ToggleSwitch("Only Recording", &engine->noMirrorRecordingOnly, theme, anim);
            Widgets::ModuleCardEnd();
        }

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("v4.0 — From Silicate", theme);

        if (Widgets::ModuleCardBegin("Backwards Stepping",
                                     "Step backwards through recorded frames during frame advance",
                                     &engine->updater.m_backwardsStepping,
                                     theme,
                                     anim,
                                     &keybinds.backStep)) {
            Widgets::StyledSliderInt("History Size",
                                     reinterpret_cast<int*>(&engine->updater.m_maxBackstepFrames),
                                     10,
                                     480,
                                     theme);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "Stores up to N ticks for rewind. Press Back Step hotkey while frame-advancing.");
            ImGui::PopStyleColor();
            Widgets::ModuleCardEnd();
        }

        if (Widgets::ModuleCardBegin("Lock Delta",
                                     "Lock physics dt for deterministic simulation",
                                     &engine->updater.m_lockDelta,
                                     theme,
                                     anim)) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "Forces exact 1/TPS every step. The old Performance mode (batching "
                "multiple ticks into one to catch up) was removed -- it measurably "
                "undercounted the frame number relative to real physics progress, and "
                "this bot doesn't need the speed badly enough to be worth that.");
            ImGui::PopStyleColor();
            Widgets::ModuleCardEnd();
        }

        if (Widgets::ModuleCard("Frame Extrapolation",
                                "Smoothly interpolate player position between physics ticks",
                                &engine->updater.m_extrapolateFrames,
                                theme,
                                anim)) {
        }

        ImGui::Dummy(ImVec2(0, 4));
        Widgets::ToggleSwitch("Show Noclip Accuracy", &engine->noclipAccuracyVisible, theme, anim);

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Autosave", theme);
        if (Widgets::ToggleSwitch(
                "Save on Level Complete", &engine->autosaveAtLevelEnd, theme, anim))
            Mod::get()->setSavedValue("autosave_atLevelEnd", engine->autosaveAtLevelEnd);
        if (Widgets::ToggleSwitch("Save at Interval", &engine->autosaveAtInterval, theme, anim)) {
            Mod::get()->setSavedValue("autosave_atInterval", engine->autosaveAtInterval);
            engine->applyIntervalAutosave();
        }
        if (engine->autosaveAtInterval) {
            float intervalF = static_cast<float>(engine->autosaveIntervalSec);
            if (Widgets::StyledSliderFloat("Interval (sec)", &intervalF, 10.f, 600.f, theme)) {
                engine->autosaveIntervalSec = static_cast<double>(intervalF);
                Mod::get()->setSavedValue("autosave_interval", engine->autosaveIntervalSec);
                engine->applyIntervalAutosave();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            char ibuf[32];
            snprintf(ibuf, sizeof(ibuf), "%.0f sec", engine->autosaveIntervalSec);
            ImGui::Text("Saves every %s while recording.", ibuf);
            ImGui::PopStyleColor();
        }
        Widgets::ToggleSwitch(
            "Backup Before Overwrite", &engine->replayBackupsEnabled, theme, anim);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped("Backups saved to replays/backups/ subfolder.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Hacks", theme);
        if (Widgets::ModuleCardBegin(
                "Safe Mode",
                (activeTheme == THEME_TOOSII)
                    ? "Safe mode is just playing with no pads. Still catching everything."
                    : "Prevents stats and percentage gain",
                &engine->protectedMode,
                theme,
                anim,
                &keybinds.safeMode))
            Widgets::ModuleCardEnd();
        if (Widgets::ModuleCardBegin("Show Trajectory",
                                     (activeTheme == THEME_TOOSII)
                                         ? "Run the route. Don't look back. Ball's already there."
                                         : "Display predicted player path",
                                     &engine->pathPreview,
                                     theme,
                                     anim,
                                     &keybinds.trajectory)) {
            Widgets::StyledSliderInt("Trajectory Length", &engine->pathLength, 50, 480, theme);
            Widgets::ModuleCardEnd();
        }
        if (Widgets::ModuleCardBegin("Show Hitboxes",
                                     "Display collision bounds for objects",
                                     &engine->showHitboxes,
                                     theme,
                                     anim,
                                     &keybinds.hitboxes)) {
            Widgets::ToggleSwitch("On Death Only", &engine->hitboxOnDeath, theme, anim);
            Widgets::ToggleSwitch("Draw Trail", &engine->hitboxTrail, theme, anim);
            if (engine->hitboxTrail)
                Widgets::StyledSliderInt(
                    "Trail Length", &engine->hitboxTrailLength, 10, 600, theme);
            Widgets::ModuleCardEnd();
        }
        if (Widgets::ModuleCardBegin(
                "Noclip",
                (activeTheme == THEME_TOOSII)
                    ? "Can't cover what you can't see. Route so clean it's invisible."
                    : "Disable collision with obstacles",
                &engine->noclipEnabled,
                theme,
                anim,
                &keybinds.noclip)) {

            if (engine->noclipAccuracyVisible) {
                float pct = engine->noclipAccuracy * 100.f;
                ImVec4 hc = pct >= 90   ? ImVec4(0.3f, 1, 0.3f, 1)
                            : pct >= 70 ? ImVec4(1, 1, 0.3f, 1)
                                        : ImVec4(1, 0.3f, 0.3f, 1);
                ImGui::Text("Accuracy: ");
                ImGui::SameLine();
                ImGui::TextColored(hc, "%.2f%%", pct);
                ImGui::Dummy(ImVec2(0, 4));
                bool hasThresh = engine->noclipThreshold > 0.f;
                if (Widgets::ToggleSwitch("Accuracy Threshold", &hasThresh, theme, anim))
                    engine->noclipThreshold = hasThresh ? 0.80f : 0.f;
                if (hasThresh) {
                    ImGui::SetNextItemWidth(-1);
                    float t = engine->noclipThreshold * 100.f;
                    if (ImGui::SliderFloat("##noclipThresh", &t, 1.f, 100.f, "%.1f%%"))
                        engine->noclipThreshold = t / 100.f;
                    ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                    ImGui::TextWrapped("Noclip disables itself when accuracy reaches this value.");
                    ImGui::PopStyleColor();
                }
                ImGui::Dummy(ImVec2(0, 4));
            }

            Widgets::ToggleSwitch("On Death Color", &engine->noclipDeathFlash, theme, anim);
            if (engine->noclipDeathFlash) {
                float col[3] = {engine->noclipDeathColorR,
                                engine->noclipDeathColorG,
                                engine->noclipDeathColorB};
                if (ImGui::ColorEdit3(
                        "##dc", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                    engine->noclipDeathColorR = col[0];
                    engine->noclipDeathColorG = col[1];
                    engine->noclipDeathColorB = col[2];
                }
            }
            Widgets::ModuleCardEnd();
        }

        if (Widgets::ModuleCardBegin(
                "RNG Lock",
                (activeTheme == THEME_TOOSII)
                    ? "Fixed seed. Like my routes -- always finding the soft spot in zone."
                    : "Use fixed seed for consistent RNG",
                &engine->rngLocked,
                theme,
                anim,
                &keybinds.rngLock)) {
            if (!rngBufferInit) {
                snprintf(rngBuffer, sizeof(rngBuffer), "%u", engine->rngSeedVal);
                rngBufferInit = true;
            }
            ImGui::Text("Seed Value:");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText(
                    "##seed", rngBuffer, sizeof(rngBuffer), ImGuiInputTextFlags_CharsDecimal)) {
                try {
                    engine->rngSeedVal = (unsigned)std::stoull(rngBuffer);
                } catch (...) {
                    engine->rngSeedVal = 1;
                }
            }
            Widgets::ModuleCardEnd();
        }

        if (Widgets::ModuleCardBegin("Auto-Flip on Death",
                                     "Flip gravity instead of dying -- great for mirror levels",
                                     &engine->updater.m_autoFlipOnDeath,
                                     theme,
                                     anim,
                                     &keybinds.autoFlip)) {
            if (engine->updater.m_isAutoFlipped) {
                Widgets::StatusBadge("FLIPPED", ImVec4(0.4f, 0.8f, 1.f, 1.f));
            }
            Widgets::ModuleCardEnd();
        }

        if (Widgets::ModuleCard("Prevent Death",
                                "Absorb all hits silently -- no collision counter",
                                &engine->updater.m_preventDeath,
                                theme,
                                anim,
                                &keybinds.preventDeath)) {
        }

        if (Widgets::ModuleCardBegin("Mirror Inputs",
                                     "Replay as if left/right controls are swapped",
                                     &engine->replay.m_mirrorInputs,
                                     theme,
                                     anim,
                                     &keybinds.mirrorInputs)) {
            Widgets::ToggleSwitch("Invert Players", &engine->replay.m_mirrorInverted, theme, anim);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped("Invert Players: swap which player each input goes to.");
            ImGui::PopStyleColor();
            Widgets::ModuleCardEnd();
        }

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Frame Stepping", theme);
        {
            auto& upd = engine->updater;
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::Text("Current frame: %u", upd.getFrame());
            ImGui::PopStyleColor();
            bool paused = upd.m_paused;
            if (Widgets::ToggleSwitch("Pause Physics", &paused, theme, anim))
                upd.setPaused(paused);
            if (upd.m_paused) {
                float bw = (ImGui::GetContentRegionAvail().x - 8) / 2.f;
                if (Widgets::StyledButton("<< Step Back", ImVec2(bw, 28), theme, anim, 6.f)) {
                    if (upd.m_backwardsStepping)
                        upd.backwardsStep(1);
                }
                ImGui::SameLine(0, 8);
                if (Widgets::StyledButton("Step Fwd >>", ImVec2(bw, 28), theme, anim, 6.f)) {
                    upd.m_stepOnce_ = true;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                if (!upd.m_backwardsStepping)
                    ImGui::TextWrapped("Enable Backwards Stepping (above) to step back.");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(
                    "Pause physics to step frame-by-frame. Hotkeys are in Settings > Keybinds.");
                ImGui::PopStyleColor();
            }
        }

        ImGui::Dummy(ImVec2(0, 8));
        drawMoreHacksTab();
        ImGui::Dummy(ImVec2(0, 8));
        drawClicksTab();
    }

    static geode::Task<int> importFwAssetFilesTask() {
        auto pickResult = co_await geode::utils::file::pickMany(geode::utils::file::FilePickOptions{
            std::nullopt, {{"Audio/Image Files", {"wav", "mp3", "ogg", "png"}}}});
        if (pickResult.isErr())
            co_return -1;
        auto paths = pickResult.unwrap();
        if (paths.empty())
            co_return -1;

        auto destDir = Mod::get()->getSaveDir() / "fw_assets";
        std::error_code ec;
        std::filesystem::create_directories(destDir, ec);

        int copied = 0;
        for (auto& p : paths) {
            std::filesystem::copy_file(
                p, destDir / p.filename(), std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec)
                copied++;
        }
        co_return copied;
    }
    // Must be stored, not an unstored temporary -- Task<T>'s own coroutine
    // promise holds only a weak_ptr to its Handle, so this static is the ONLY
    // thing keeping an in-flight file-picker Task alive. An unstored temporary
    // here caused a real crash (use-after-free while the OS picker dialog's
    // background thread was still running). Task::listen()'s callback is also
    // a no-op in this Geode version -- poll isFinished() instead, don't add a
    // .listen() call back in.
    static geode::Task<int> s_fwAssetFilesTask;
    static geode::Task<int> s_fwAssetFolderTask;
    static geode::Task<bool> s_trainerMusicTask;
    // Shared guard across every GucciBot button that opens a native OS file
    // picker via Geode's async file::pick()/pickMany(). One re-click on the
    // SAME button is already blocked per-site below, but that alone wasn't
    // enough: GitHub issue #3 (MoriiiLL, 2026-09-10) crashed again on v1.6.4,
    // this time resuming importFwAssetFolderTask instead of the files one --
    // symbolized the same way as issue #1 (matching PDB + llvm-symbolizer),
    // same exact crash signature (arc::Context::shouldCoopYield on a freed
    // coroutine). "Import Sounds/Images" and "Import Folder" sit on the same
    // row in the UI, so clicking one, seeing nothing happen (the OS dialog
    // can open behind a fullscreen GD window), and clicking the OTHER one
    // right next to it is an easy real sequence a per-button guard can't
    // catch. Checking every known picker before starting a new one closes
    // that gap. NOTE: this narrows how OFTEN the crash can trigger but may
    // not be the full story -- Geode's own Task<T>/coroutine cancellation
    // path (Task.hpp) has real dead/commented-out cancel-propagation code at
    // this SDK version, so there may be a genuine lifetime gap inside Geode
    // itself between a Task's Handle being destroyed and its still-running
    // background OS-dialog thread later writing back into freed memory --
    // not something GucciBot can fully close from this side alone.
    static bool anyFwPickerPending() {
        return s_fwAssetFilesTask.isPending() || s_fwAssetFolderTask.isPending() ||
               s_trainerMusicTask.isPending();
    }
    static void importFwAssetFiles() {
        if (anyFwPickerPending())
            return;
        s_fwAssetFilesTask = importFwAssetFilesTask();
    }
    static void pollFwAssetImportTasks() {
        if (s_fwAssetFilesTask.isFinished()) {
            auto* copied = s_fwAssetFilesTask.getFinishedValue();
            if (copied && *copied > 0)
                Notification::create(fmt::format("Imported {} file(s) into fw_assets", *copied),
                                     NotificationIcon::Success)
                    ->show();
            else
                Notification::create("Import failed or cancelled", NotificationIcon::Warning)
                    ->show();
            s_fwAssetFilesTask = {};
        }
        if (s_fwAssetFolderTask.isFinished()) {
            auto* copied = s_fwAssetFolderTask.getFinishedValue();
            if (copied && *copied > 0)
                Notification::create(fmt::format("Imported {} file(s) into fw_assets", *copied),
                                     NotificationIcon::Success)
                    ->show();
            else
                Notification::create("Import failed or cancelled", NotificationIcon::Warning)
                    ->show();
            s_fwAssetFolderTask = {};
        }
    }

    static geode::Task<int> importFwAssetFolderTask() {
        auto pickResult = co_await geode::utils::file::pick(
            geode::utils::file::PickMode::OpenFolder,
            geode::utils::file::FilePickOptions{std::nullopt, {}});
        if (pickResult.isErr())
            co_return -1;
        auto srcOpt = pickResult.unwrap();
        if (!srcOpt.has_value())
            co_return -1;

        auto destDir = Mod::get()->getSaveDir() / "fw_assets" / srcOpt->filename();
        std::error_code ec;
        std::filesystem::create_directories(destDir, ec);

        int copied = 0;
        for (auto& entry : std::filesystem::recursive_directory_iterator(*srcOpt, ec)) {
            if (ec || !entry.is_regular_file())
                continue;
            auto rel = std::filesystem::relative(entry.path(), *srcOpt, ec);
            if (ec)
                continue;
            auto dest = destDir / rel;
            std::filesystem::create_directories(dest.parent_path(), ec);
            std::filesystem::copy_file(
                entry.path(), dest, std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec)
                copied++;
        }
        co_return copied;
    }
    static void importFwAssetFolder() {
        if (anyFwPickerPending())
            return;
        s_fwAssetFolderTask = importFwAssetFolderTask();
    }

    void MenuInterface::drawPathfinderTab() {
        auto* pf = Pathfinder::get();
        auto* engine = GucciEngine::get();
        Widgets::GucciQuote("\"I don't find the path. The path finds me. Then I take it anyway.\"",
                            "-- Gucci Mane, probably",
                            theme);
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Searches for a click sequence that beats this level with no macro to start from. "
            "Runs the real game forward with no input until it dies, then works backward from "
            "that death for a press (and hold length) that gets further, backtracking through "
            "real checkpoints when a branch dead-ends. Nothing is simulated separately -- "
            "survival is whatever GD itself says. v1: player 1 only, one press at a time. Let "
            "it grind; when it finishes, the result is loaded as the current macro -- go to the "
            "Macro tab to name and save it.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 8));

        Widgets::SectionHeader("Search", theme);
        bool locked = pf->active;
        if (locked)
            ImGui::BeginDisabled();
        if (Widgets::StyledSliderInt("Window (frames back from death)", &pf->windowFrames, 5, 240, theme))
            pf->saveSettings();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped("How far before each death it looks for the click that was needed. Bigger "
                           "finds longer-lead jumps but costs more per decision point.");
        ImGui::PopStyleColor();
        if (Widgets::StyledSliderInt("Checkpoint every N frames", &pf->checkpointInterval, 1, 60, theme))
            pf->saveSettings();
        if (Widgets::StyledSliderInt("Min progress to count (frames)", &pf->minProgressFrames, 1, 60, theme))
            pf->saveSettings();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped("How much further a click has to survive before it counts as real progress, "
                           "instead of just delaying the same death by a frame or two. Too low and it "
                           "can get stuck accepting non-answers; too high and it may reject a genuinely "
                           "tight escape.");
        ImGui::PopStyleColor();
        if (Widgets::StyledSliderInt("Max runs", &pf->maxRuns, 100, 200000, theme))
            pf->saveSettings();
        if (locked)
            ImGui::EndDisabled();
        if (Widgets::ToggleSwitch("Hide the search (surprise me)", &pf->hideSearch, theme, anim))
            pf->saveSettings();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(pf->hideSearch
                                ? "On: a full-screen cover hides the search, just Calculating... and "
                                  "the best percentage reached until a macro exists."
                                : "Off: no cover -- watch the level play out while it searches, with "
                                  "a small status readout in the corner instead.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 8));

        bool inLevel = PlayLayer::get() != nullptr;
        if (!pf->active) {
            if (!inLevel)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
            bool clicked = Widgets::StyledButton("Start Pathfinder", ImVec2(-1, 30), theme, anim, 6.f);
            if (!inLevel)
                ImGui::PopStyleVar();
            if (clicked && inLevel)
                pf->begin();
            if (!inLevel) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped("Enter the level to start.");
                ImGui::PopStyleColor();
            }
        } else {
            if (Widgets::StyledButton("Cancel", ImVec2(-1, 30), theme, anim, 6.f))
                pf->cancel();
        }

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Status", theme);
        if (pf->active) {
            Widgets::StatusBadge("SEARCHING", ImVec4(0.3f, 1.f, 0.4f, 1.f));
            ImGui::SameLine();
            ImGui::Text("%s", pf->stage.c_str());
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme.getAccent());
            char ov[64];
            snprintf(ov, sizeof(ov), "best %.1f%%", pf->bestPct);
            ImGui::ProgressBar(std::clamp(pf->bestPct / 100.f, 0.f, 1.f), ImVec2(-1, 18), ov);
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::Text("runs %d  |  depth %zu  |  best frame %u", pf->runs, pf->depth, pf->bestFrame);
            ImGui::PopStyleColor();
        } else if (pf->hasResult) {
            if (pf->lastResultSuccess) {
                Widgets::StatusBadge("SOLVED", ImVec4(0.3f, 1.f, 0.4f, 1.f));
                ImGui::SameLine();
                ImGui::Text("%zu inputs, %d runs", pf->resultInputCount, pf->runs);
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                if (!pf->savedAs.empty())
                    ImGui::TextWrapped(
                        "Loaded as the current macro and saved as \"%s\" -- hit Playback to watch it, "
                        "or rename it from the Macro tab.",
                        pf->savedAs.c_str());
                else
                    ImGui::TextWrapped(
                        "Loaded as the current macro and saved under your existing macro name -- hit "
                        "Playback to watch it.");
                ImGui::PopStyleColor();
            } else {
                Widgets::StatusBadge(pf->stage == "cancelled" ? "CANCELLED" : "GAVE UP",
                                     ImVec4(1.f, 0.5f, 0.3f, 1.f));
                ImGui::SameLine();
                ImGui::Text("best %.1f%% after %d runs", pf->bestPct, pf->runs);
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped("Your previous macro was left untouched. Try a bigger window if it kept "
                                   "dying in the same spot -- the click it needed may be earlier than the "
                                   "window reaches.");
                ImGui::PopStyleColor();
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped("Nothing run yet.");
            ImGui::PopStyleColor();
        }
        (void)engine;
    }

    void MenuInterface::drawFrameWindowsTab() {
        pollFwAssetImportTasks();
        auto* engine = GucciEngine::get();
        Widgets::GucciQuote("\"Speed doesn't mean much if the frame windows are wrong.\"",
                            "-- Juice, keeping you honest",
                            theme);
        ImGui::Dummy(ImVec2(0, 6));
        {
            auto& replay = engine->replay;
            bool hasActions = !replay.m_actionAtom.m_actions.empty();
            bool canCalc = PlayLayer::get() != nullptr && hasActions;
            if (!canCalc)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
            bool calcClicked = Widgets::StyledButton("Calculate", ImVec2(-1, 30), theme, anim, 6.f);
            if (!canCalc)
                ImGui::PopStyleVar();
            if (calcClicked && canCalc)
                engine->analyzeFrameWindows();
            if (!canCalc) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(PlayLayer::get() ? "Record or load a macro with actions first."
                                                    : "Enter the level to Calculate.");
                ImGui::PopStyleColor();
            }
        }
        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Frame Window Tracker", theme);
        {
            bool locked = engine->fwAnalyzing;
            if (locked)
                ImGui::BeginDisabled();
            const char* algoNames[] = {"Time-Based", "Recovery Range", "Alignment-Independent"};
            int algoIdx = engine->fwUseAlignmentIndependent
                             ? 2
                             : (engine->fwUseRecoveryRangeAlgorithm ? 1 : 0);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##fwAlgo", &algoIdx, algoNames, 3)) {
                engine->fwUseAlignmentIndependent = (algoIdx == 2);
                engine->fwUseRecoveryRangeAlgorithm = (algoIdx == 1);
                Mod::get()->setSavedValue("fw_use_align_indep", engine->fwUseAlignmentIndependent);
                Mod::get()->setSavedValue("fw_use_recovery_range",
                                          engine->fwUseRecoveryRangeAlgorithm);
            }
            if (locked)
                ImGui::EndDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                locked
                    ? "Locked while Calculate is running -- finish or cancel the current run to "
                      "switch algorithms."
                    : "Time-Based (default): faster, watches the shifted click survive on its own "
                      "-- good for most levels. Recovery Range: Juice's original algorithm, "
                      "revived "
                      "-- also checks whether the NEXT click's timing could shift slightly to "
                      "still "
                      "work, which is more accurate but noticeably slower (extra probe runs per "
                      "shift). Unverified since being brought back -- worth A/B'ing against "
                      "Time-Based on the same section. Alignment-Independent: Juice's new algorithm "
                      "-- also shifts the PREVIOUS click's timing when testing this one (so a click "
                      "right after a spam release gets tested under a few different real release "
                      "timings, not just the exact recorded one), and only counts a timing as good "
                      "if the click AFTER it also has some way to keep going. Brute-force and "
                      "noticeably slower than either method above -- runs entirely separately from "
                      "them and never changes what those show. Version 1: correctness first, no "
                      "caching/parallelism yet.");
            ImGui::PopStyleColor();
            if (locked)
                ImGui::BeginDisabled();
            if (engine->fwUseRecoveryRangeAlgorithm &&
                Widgets::StyledSliderInt("Recovery Range", &engine->fwRecoveryRange, 1, 10, theme))
                Mod::get()->setSavedValue("fw_recovery_range", (int64_t)engine->fwRecoveryRange);
            if (engine->fwUseAlignmentIndependent) {
                if (Widgets::StyledSliderInt("Search Radius (Z)", &engine->fwAiZ, 1, 30, theme))
                    Mod::get()->setSavedValue("fw_ai_z", (int64_t)engine->fwAiZ);
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(
                    "How many frames each shifted click (both the previous one and this one) is "
                    "tested across. Independent of Sweep Range below (Analysis Settings) -- that's "
                    "Time-Based/Recovery Range's own setting, this one no longer borrows or gets "
                    "capped by it. Cost grows fast: roughly (2*Z+1)^2 simulated runs per click "
                    "before continuation testing, so keep this small (3-5) unless you're prepared "
                    "to wait.");
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(
                    "What \"predecessor\" means: for each click being measured, it re-tests the "
                    "click BEFORE it at a few nearby timings too (not just the exact recorded "
                    "frame), and re-measures the click under each of those -- since a real player's "
                    "previous click landing 1-2 frames early or late can change what's actually "
                    "reachable next. The progress bar's \"predecessor +2\" / \"align 3/7, x +1\" text "
                    "reads as: which of those earlier-click timings is being tried, then which "
                    "shift of THIS click is being tried under that specific earlier timing.");
                ImGui::PopStyleColor();
                int contIdx = std::clamp(engine->fwAiContinuationDepth, 0, 1);
                const char* contNames[] = {"0 (off)", "1"};
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##fwAiDepth", &contIdx, contNames, 2)) {
                    engine->fwAiContinuationDepth = contIdx;
                    Mod::get()->setSavedValue("fw_ai_cont_depth", (int64_t)engine->fwAiContinuationDepth);
                }
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(
                    "Continuation Depth: 0 means surviving to the next click's target is enough. 1 "
                    "(default) also requires the click AFTER that to have some viable timing of "
                    "its own -- Juice's spec's core idea. Only 0/1 are implemented in this version, "
                    "not arbitrary depth.");
                ImGui::PopStyleColor();
                if (Widgets::StyledSliderFloat(
                        "Cluster Ratio", &engine->fwAiClusterRatio, 1.02f, 2.f, theme))
                    Mod::get()->setSavedValue("fw_ai_cluster_ratio", engine->fwAiClusterRatio);
                if (Widgets::StyledSliderFloat("Dominant Cluster Threshold",
                                               &engine->fwAiDominantThreshold,
                                               0.1f,
                                               1.f,
                                               theme))
                    Mod::get()->setSavedValue("fw_ai_dominant_threshold",
                                              engine->fwAiDominantThreshold);
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(
                    "How the windows measured under different previous-click timings get combined "
                    "into one number: values within Cluster Ratio of each other count as the same "
                    "cluster, and the largest cluster needs to cover at least this fraction of all "
                    "valid alignments to be trusted. If nothing reaches that, the result falls back "
                    "to whatever Time-Based/Recovery Range already measured for that click.");
                ImGui::PopStyleColor();
            }
            if (locked)
                ImGui::EndDisabled();
        }
        {
            bool recLocked = engine->isRecording();
            if (recLocked)
                ImGui::BeginDisabled();
            if (Widgets::ToggleSwitch("Show Live", &engine->fwEnabledLive, theme, anim))
                Mod::get()->setSavedValue("fw_live", engine->fwEnabledLive);
            if (recLocked)
                ImGui::EndDisabled();
            if (Widgets::ToggleSwitch("Show in Renders", &engine->fwEnabledRender, theme, anim))
                Mod::get()->setSavedValue("fw_render", engine->fwEnabledRender);
            if (recLocked)
                ImGui::BeginDisabled();
            if (Widgets::ToggleSwitch("Show Legend", &engine->fwLegendEnabled, theme, anim))
                Mod::get()->setSavedValue("fw_legend", engine->fwLegendEnabled);
            if (recLocked)
                ImGui::EndDisabled();

            ImGui::Dummy(ImVec2(0, 6));
            if (Widgets::ToggleSwitch("Default Look", &engine->fwDefaultLook, theme, anim))
                Mod::get()->setSavedValue("fw_default_look", engine->fwDefaultLook);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "Overrides everything below: one plain ring per click, a fixed colour ramp "
                "(red = tightest, blue = most lenient), and the number to the left of the ring. "
                "Tiers, shapes, marker images and Circle Skin are all ignored while this is on -- "
                "your settings for them are kept, just not used.");
            ImGui::PopStyleColor();
            if (engine->fwDefaultLook) {
                if (Widgets::ToggleSwitch(
                        "   Use Bell Sounds", &engine->fwDefaultLookBells, theme, anim))
                    Mod::get()->setSavedValue("fw_default_look_bells", engine->fwDefaultLookBells);
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped("Off: Gucci Mane saying \"Brrr\" (GucciBot's own default). "
                                   "On: the bundled per-window bell set, which is what this style "
                                   "normally uses.");
                ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, 6));
        }
        {
            bool aiLocked = !engine->fwAiHasData;
            if (aiLocked)
                ImGui::BeginDisabled();
            if (Widgets::ToggleSwitch("Show Alignment-Independent In-Level",
                                      &engine->fwOverlayShowAlignmentIndependent,
                                      theme,
                                      anim))
                Mod::get()->setSavedValue("fw_overlay_show_ai",
                                          engine->fwOverlayShowAlignmentIndependent);
            if (aiLocked)
                ImGui::EndDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                aiLocked ? "No Alignment-Independent results yet -- run Calculate with it selected "
                           "above first."
                         : "Off (default): the markers above show Time-Based/Recovery Range, same "
                           "as always. On: they show Alignment-Independent's representative window "
                           "instead (falling back to the Macro value for a click when no dominant "
                           "cluster was found) -- switch back and forth freely, both results stay "
                           "available once measured.");
            ImGui::PopStyleColor();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            engine->isRecording()
                ? "Show Live and Show Legend are locked off while recording -- both read state "
                  "that "
                  "doesn't exist yet mid-recording. Available again once recording stops."
                : "Juice's idea: a running tally in the top-left corner, like the frame-window "
                  "counter "
                  "overlays in some GD YouTube videos -- how many of the clicks reached so far "
                  "landed "
                  "in each Tier's window range below. Shows nothing until at least one Tier is "
                  "configured and Calculate has results.");
        ImGui::PopStyleColor();
        if (engine->fwLegendEnabled &&
            Widgets::StyledSliderFloat("Legend Size", &engine->fwLegendScale, 0.5f, 3.f, theme))
            Mod::get()->setSavedValue("fw_legend_scale", engine->fwLegendScale);
        if (Widgets::StyledSliderFloat("Ring Boldness", &engine->fwRingBoldness, 0.5f, 8.f, theme))
            Mod::get()->setSavedValue("fw_ring_boldness", engine->fwRingBoldness);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Marker stroke thickness for the concentric double-ring style. Only affects "
            "markers without a Tier-specific image configured.");
        ImGui::PopStyleColor();
        if (Widgets::ToggleSwitch("Circle Skin", &engine->fwCircleSkinEnabled, theme, anim))
            Mod::get()->setSavedValue("fw_circle_skin", engine->fwCircleSkinEnabled);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Juice's osu!mania-style idea: a small dot at the exact click timing, with an "
            "unfilled ring around it that grows with that click's window size -- a wide-open "
            "input reads as an obviously bigger halo instead of a same-size marker with a "
            "different number next to it. Replaces Tier shapes/images while on.");
        ImGui::PopStyleColor();
        if (engine->fwCircleSkinEnabled) {
            if (Widgets::StyledSliderFloat(
                    "Dot Radius", &engine->fwCircleSkinDotRadius, 1.f, 20.f, theme))
                Mod::get()->setSavedValue("fw_circleskin_dot_radius", engine->fwCircleSkinDotRadius);
            if (Widgets::StyledSliderFloat("Ring Growth (per frame)",
                                           &engine->fwCircleSkinRadiusPerFrame,
                                           0.2f,
                                           10.f,
                                           theme))
                Mod::get()->setSavedValue("fw_circleskin_radius_per_frame",
                                          engine->fwCircleSkinRadiusPerFrame);
            if (Widgets::StyledSliderFloat(
                    "Max Ring Radius", &engine->fwCircleSkinMaxRadius, 10.f, 300.f, theme))
                Mod::get()->setSavedValue("fw_circleskin_max_radius", engine->fwCircleSkinMaxRadius);
        }
        if (Widgets::ToggleSwitch("Test Ship Releases", &engine->fwTestShipReleases, theme, anim))
            Mod::get()->setSavedValue("fw_test_ship_releases", engine->fwTestShipReleases);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped("Calculate measures release timing windows too now, for Wave/Ship/Robot "
                           "(the only gamemodes where a release's timing matters) -- Ship's can be "
                           "finicky to probe reliably, so it has its own switch here.");
        ImGui::PopStyleColor();
        if (Widgets::ToggleSwitch(
                "Orb-Aware Release Skip", &engine->fwOrbAwareReleaseSkip, theme, anim))
            Mod::get()->setSavedValue("fw_orb_aware_release_skip", engine->fwOrbAwareReleaseSkip);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "In Robot mode, a release right after clicking a non-dash orb isn't treated "
            "as its own measurable input (dash orbs and non-orb clicks still are). Turn "
            "off to go back to testing every Robot release, no exceptions.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Practice Range", theme);
        if (Widgets::ToggleSwitch(
                "Show During Playback", &engine->practiceRangeEnabled, theme, anim))
            Mod::get()->setSavedValue("practice_range", engine->practiceRangeEnabled);
        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Analysis Settings", theme);
        if (engine->fwMaxWindow > 2 * engine->fwSweepRange) {
            engine->fwMaxWindow = 2 * engine->fwSweepRange;
            Mod::get()->setSavedValue("fw_maxwindow", (int64_t)engine->fwMaxWindow);
        }
        if (Widgets::StyledSliderInt(
                "Max Window (frames)", &engine->fwMaxWindow, 1, 2 * engine->fwSweepRange, theme))
            Mod::get()->setSavedValue("fw_maxwindow", (int64_t)engine->fwMaxWindow);
        if (Widgets::StyledSliderInt(
                "Sweep Range (+/- frames)", &engine->fwSweepRange, 1, 30, theme))
            Mod::get()->setSavedValue("fw_sweeprange", (int64_t)engine->fwSweepRange);
        if (Widgets::StyledSliderInt("Slack Window (frames)", &engine->fwSlackWindow, 0, 20, theme))
            Mod::get()->setSavedValue("fw_slackwindow", (int64_t)engine->fwSlackWindow);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped("A shift survives if it stays alive through roughly how long the "
                           "original macro takes to "
                           "reach the next input, MINUS this many frames of slack (a shorter "
                           "survival still counts as "
                           "a pass -- it never needs to survive longer than the original gap). The "
                           "next input itself "
                           "is never moved -- it always fires at its own original frame.");
        ImGui::PopStyleColor();
        if (Widgets::ToggleSwitch(
                "Position Tolerance", &engine->fwPositionCheckEnabled, theme, anim))
            Mod::get()->setSavedValue("fw_position_check", engine->fwPositionCheckEnabled);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Juice's request: surviving isn't proof a shift actually worked -- the "
            "player could be alive but far off the real path. When on, a survived shift "
            "only counts if the player ends up within the slack below of the next "
            "input's TRUE position (both axes); otherwise it's treated as failed.");
        ImGui::PopStyleColor();
        if (engine->fwPositionCheckEnabled &&
            Widgets::StyledSliderFloat(
                "Position Slack (units)", &engine->fwPositionSlack, 1.f, 200.f, theme))
            Mod::get()->setSavedValue("fw_position_slack", engine->fwPositionSlack);
        if (Widgets::ToggleSwitch("Full-Range Sweep", &engine->fwFullRangeSweep, theme, anim))
            Mod::get()->setSavedValue("fw_full_range_sweep", engine->fwFullRangeSweep);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Off (default): stop expanding a direction the moment one shift fails "
            "there. On: keep testing every shift out to the sweep range regardless of "
            "failures in between, so non-contiguous survivable windows actually show up "
            "instead of being silently missed. Slower.");
        ImGui::PopStyleColor();
        if (Widgets::StyledSliderInt(
                "Max Frames Measured", &engine->fwMaxFramesMeasured, 16, 480, theme))
            Mod::get()->setSavedValue("fw_maxframes", (int64_t)engine->fwMaxFramesMeasured);
        if (Widgets::StyledSliderInt("Simulation Speed", &engine->fwSimSpeed, 1, 8, theme))
            Mod::get()->setSavedValue("fw_simspeed", (int64_t)engine->fwSimSpeed);
        if (Widgets::ToggleSwitch("Debug Mode", &engine->fwDebugMode, theme, anim))
            Mod::get()->setSavedValue("fw_debug_mode", engine->fwDebugMode);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Pauses briefly after every individual shift test and drops a green/red "
            "mark where the player ended up, so you can watch Calculate work through a "
            "click instead of only seeing the final number.");
        ImGui::PopStyleColor();
        if (Widgets::ToggleSwitch(
                "Delay Marker Capture (diagnostic)", &engine->fwDelayMarkerCapture, theme, anim))
            Mod::get()->setSavedValue("fw_delay_marker_capture", engine->fwDelayMarkerCapture);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Juice's position-lag report: run Calculate once with this off, once with "
            "it on, on the same macro/click. Doesn't change any measurement, only where "
            "the marker ring gets drawn -- whichever run's rings actually line up with "
            "the real click tells us which way the fix needs to go.");
        ImGui::PopStyleColor();
        if (engine->fwDebugMode &&
            Widgets::StyledSliderInt(
                "Debug Pause (ticks)", &engine->fwDebugSlowdown, 1, 120, theme))
            Mod::get()->setSavedValue("fw_debug_slowdown", (int64_t)engine->fwDebugSlowdown);
        if (!engine->fwDebugMarks.empty() &&
            Widgets::StyledButton("View Debug History (...)", ImVec2(-1, 28), theme, anim, 6.f))
            ImGui::OpenPopup("FwDebugHistory");
        ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 0));
        if (ImGui::BeginPopupModal("FwDebugHistory",
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoResize)) {
            drawPopupChrome(*this, "Debug History");
            ImGui::TextColored(
                theme.getAccent(), "%zu test(s) recorded this run", engine->fwDebugMarks.size());
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "\"Go\" teleports you to that test's exact checkpoint + shift and lets it play out "
                "exactly like the real test did -- watch it, or take over yourself.");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 6));
            float listH = std::min((float)engine->fwDebugMarks.size() * 26.f, 320.f);
            ImGui::BeginChild("##fwDebugHistList", ImVec2(410, listH), true);
            for (size_t i = 0; i < engine->fwDebugMarks.size(); ++i) {
                auto const& mk = engine->fwDebugMarks[i];
                ImGui::PushID((int)i + 11000);
                ImGui::TextColored(mk.survived ? ImVec4(0.3f, 1.f, 0.4f, 1.f)
                                               : ImVec4(1.f, 0.3f, 0.3f, 1.f),
                                   "%s",
                                   mk.survived ? "PASS" : "FAIL");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::Text("in#%d f=%u -> shift f=%u  %s p%d",
                            mk.inputNumber,
                            mk.macroFrame,
                            mk.testedFrame,
                            mk.isRelease ? "rel" : "press",
                            mk.player2 ? 2 : 1);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (Widgets::StyledButton("Go", ImVec2(40, 20), theme, anim, 4.f)) {
                    engine->debugTeleportToMark(i);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::Dummy(ImVec2(0, 6));
            if (Widgets::StyledButton("Close##fwDebugHist", ImVec2(-1, 28), theme, anim, 6.f))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);
        if (engine->fwAnalyzeRunning) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme.getAccent());
            char ov[64];
            if (engine->fwAnalyzeTotal > 0)
                snprintf(ov,
                         sizeof(ov),
                         "%s  %d/%d  (%.0f%%)",
                         engine->fwAnalyzeStage.c_str(),
                         engine->fwAnalyzeCur,
                         engine->fwAnalyzeTotal,
                         engine->fwAnalyzeProgress * 100.f);
            else
                snprintf(ov,
                         sizeof(ov),
                         "%s  (%.0f%%)",
                         engine->fwAnalyzeStage.c_str(),
                         engine->fwAnalyzeProgress * 100.f);
            ImGui::ProgressBar(engine->fwAnalyzeProgress, ImVec2(-1, 18), ov);
            ImGui::PopStyleColor();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        if (engine->fwHasData) {
            int vis = 0, loosest = 0;
            for (auto const& mk : engine->fwMarks) {
                if (mk.window <= engine->fwMaxWindow)
                    ++vis;
                if (mk.window > loosest)
                    loosest = mk.window;
            }
            ImGui::Text("%zu clicks analyzed, %d shown (window <= %d).",
                        engine->fwMarks.size(),
                        vis,
                        engine->fwMaxWindow);
            if (vis == 0 && !engine->fwMarks.empty())
                ImGui::TextWrapped("None visible: every click is looser than %d frames (loosest is "
                                   "%d). Raise Max Window to see them.",
                                   engine->fwMaxWindow,
                                   loosest);
        } else if (!engine->fwAnalyzeRunning)
            ImGui::TextWrapped(
                "No analysis yet. Save a macro while in the level and choose Calculate "
                "to simulate frame windows.");
        ImGui::PopStyleColor();

        if (engine->fwAiHasData) {
            static size_t s_aiDebugFilterClick = SIZE_MAX;
            static bool s_aiDebugPopupRequested = false;

            ImGui::Dummy(ImVec2(0, 8));
            Widgets::SectionHeader("Alignment-Independent Results", theme);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "Doesn't touch the markers above -- Time-Based/Recovery Range still drive those. "
                "\"Representative\" is blank when no dominant cluster was found for that click, "
                "meaning it fell back to the Macro column. Macro shows \"--\" if Time-Based/"
                "Recovery Range hasn't measured this click at all yet (not the same as an actual "
                "0-frame result) -- run one of those too if you want a real number there.");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 4));
            float tblH = std::min((float)engine->fwAiResults.size() * 24.f + 28.f, 280.f);
            if (ImGui::BeginTable("##fwAiResultsTbl",
                                  7,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY,
                                  ImVec2(-1, tblH))) {
                ImGui::TableSetupColumn("Click");
                ImGui::TableSetupColumn("Macro");
                ImGui::TableSetupColumn("Representative");
                ImGui::TableSetupColumn("Observed");
                ImGui::TableSetupColumn("Valid Align.");
                ImGui::TableSetupColumn("Sensitivity");
                ImGui::TableSetupColumn("Branches");
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < engine->fwAiResults.size(); ++i) {
                    auto const& r = engine->fwAiResults[i];
                    ImGui::PushID((int)i + 12000);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%.1f%% %s%s",
                               r.percent,
                               r.isRelease ? "rel" : "press",
                               r.player2 ? " p2" : "");
                    ImGui::TableSetColumnIndex(1);
                    if (r.hasMacroMatch)
                        ImGui::Text("%d", r.macroWindow);
                    else
                        ImGui::TextColored(theme.textSecondary, "--");
                    ImGui::TableSetColumnIndex(2);
                    if (r.representativeWindow > 0)
                        ImGui::Text("%d", r.representativeWindow);
                    else
                        ImGui::TextColored(theme.textSecondary, "--");
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%d..%d", r.observedMin, r.observedMax);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%d/%d", r.validAlignments, r.totalAlignments);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%.2f", r.sensitivity);
                    ImGui::TableSetColumnIndex(6);
                    bool hasBranches = std::any_of(
                        engine->fwAiDebugBranches.begin(),
                        engine->fwAiDebugBranches.end(),
                        [i](auto const& br) {
                            return br.clickIdx == i;
                        });
                    if (!hasBranches)
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
                    if (Widgets::StyledButton("View", ImVec2(50, 20), theme, anim, 4.f) &&
                        hasBranches) {
                        // Can't call ImGui::OpenPopup here directly -- we're
                        // inside this row's PushID, so the string-ID popup
                        // it would open is scoped to THIS row and never
                        // matches the BeginPopupModal call below (which runs
                        // outside any PushID). Same trap the existing
                        // replayActionPopupRequested/replayRenamePopupRequested
                        // fields elsewhere in this file exist to avoid --
                        // defer the actual OpenPopup call to after the table
                        // closes, matching that pattern. This was exactly
                        // why "View" did nothing (Juice, 2026-09-02).
                        s_aiDebugFilterClick = i;
                        s_aiDebugPopupRequested = true;
                    }
                    if (!hasBranches)
                        ImGui::PopStyleVar();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (s_aiDebugPopupRequested) {
                ImGui::OpenPopup("AiDebugHistory");
                s_aiDebugPopupRequested = false;
            }
            if (!engine->fwDebugMode) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(
                    "Branches only get recorded when Debug Mode (Analysis Settings, below) is "
                    "on during the Calculate run -- turn it on and re-run to fill these in.");
                ImGui::PopStyleColor();
            }

            ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
            ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 0));
            if (ImGui::BeginPopupModal("AiDebugHistory",
                                       nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoTitleBar |
                                           ImGuiWindowFlags_NoResize)) {
                drawPopupChrome(*this, "Alignment-Independent Branches");
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(
                    "\"Go\" restores the exact predecessor-alignment checkpoint this branch used "
                    "and applies its click shift, then lets it play out at whatever speed you "
                    "normally run at -- turn on \"Show Macro Path\" first if you want to see the "
                    "recorded line to compare against.");
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 6));
                std::vector<size_t> shown;
                for (size_t bi = 0; bi < engine->fwAiDebugBranches.size(); ++bi)
                    if (engine->fwAiDebugBranches[bi].clickIdx == s_aiDebugFilterClick)
                        shown.push_back(bi);
                float listH = std::min((float)shown.size() * 26.f, 320.f);
                ImGui::BeginChild("##aiDebugHistList", ImVec2(430, listH), true);
                for (size_t bi : shown) {
                    auto const& br = engine->fwAiDebugBranches[bi];
                    ImGui::PushID((int)bi + 13000);
                    const char* statusStr = "?";
                    ImVec4 statusCol = ImVec4(1.f, 1.f, 1.f, 1.f);
                    switch (br.status) {
                    case GucciEngine::FwAiStatus::Dead:
                        statusStr = "DEAD";
                        statusCol = ImVec4(1.f, 0.3f, 0.3f, 1.f);
                        break;
                    case GucciEngine::FwAiStatus::MissedTarget:
                        statusStr = "MISSED";
                        statusCol = ImVec4(1.f, 0.6f, 0.2f, 1.f);
                        break;
                    case GucciEngine::FwAiStatus::Partial:
                        statusStr = "PARTIAL";
                        statusCol = ImVec4(0.9f, 0.9f, 0.3f, 1.f);
                        break;
                    case GucciEngine::FwAiStatus::Viable:
                        statusStr = "VIABLE";
                        statusCol = ImVec4(0.3f, 1.f, 0.4f, 1.f);
                        break;
                    case GucciEngine::FwAiStatus::DeadEnd:
                        statusStr = "DEAD_END";
                        statusCol = ImVec4(1.f, 0.4f, 0.4f, 1.f);
                        break;
                    }
                    ImGui::TextColored(statusCol, "%s", statusStr);
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                    ImGui::Text("pred %+d, x %+d", br.predShift, br.xShift);
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    if (Widgets::StyledButton("Go", ImVec2(40, 20), theme, anim, 4.f)) {
                        engine->debugTeleportToAiBranch(bi);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::Dummy(ImVec2(0, 6));
                if (Widgets::StyledButton("Close##aiDebugHist", ImVec2(-1, 28), theme, anim, 6.f))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);
        }

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Manual Frame Windows", theme);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Set or override a click's window by hand -- for clicks Calculate hasn't "
            "measured yet, or a reading you don't trust. Manual entries are protected: "
            "re-running Calculate fills in everything else but leaves these alone.");
        ImGui::PopStyleColor();
        if (engine->fwAnalyzing) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "Unavailable while Calculate is running -- it's actively reshaping the macro's "
                "action "
                "list to run its tests. Manual Frame Windows will show up again once it finishes.");
            ImGui::PopStyleColor();
        } else {
            auto& acts = engine->replay.m_actionAtom.m_actions;
            auto& samples = engine->replay.m_pathSamples;
            std::vector<size_t> clickIdx;
            for (size_t i = 0; i < acts.size(); ++i)
                if (acts[i].isInput())
                    clickIdx.push_back(i);

            if (clickIdx.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped("No macro loaded, or it has no inputs to list.");
                ImGui::PopStyleColor();
            } else {
                float listH = std::min((float)clickIdx.size() * 24.f, 200.f);
                ImGui::BeginChild("##fwManualList", ImVec2(-1, listH), true);
                for (size_t row = 0; row < clickIdx.size(); ++row) {
                    auto& a = acts[clickIdx[row]];
                    ImGui::PushID((int)row + 9000);

                    GucciEngine::FrameWindowMark* mk = nullptr;
                    for (auto& m : engine->fwMarks)
                        if (m.frame == a.m_frame && m.player2 == a.m_player2) {
                            mk = &m;
                            break;
                        }

                    float pct = mk ? mk->percent : -1.f;
                    if (!mk && a.m_frame < samples.size() && engine->m_levelLength > 0.f) {
                        float px = a.m_player2 ? samples[a.m_frame].p2x : samples[a.m_frame].p1x;
                        pct = std::clamp(px / engine->m_levelLength * 100.f, 0.f, 100.f);
                    }

                    bool isRel = !a.m_holding;
                    char rowLabel[64];
                    if (pct >= 0.f)
                        snprintf(rowLabel,
                                 sizeof(rowLabel),
                                 "f=%u  p%d  %s  %.1f%%",
                                 a.m_frame,
                                 a.m_player2 ? 2 : 1,
                                 isRel ? "rel" : "press",
                                 pct);
                    else
                        snprintf(rowLabel,
                                 sizeof(rowLabel),
                                 "f=%u  p%d  %s",
                                 a.m_frame,
                                 a.m_player2 ? 2 : 1,
                                 isRel ? "rel" : "press");
                    ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                    ImGui::TextUnformatted(rowLabel);
                    ImGui::PopStyleColor();
                    float labelW = ImGui::CalcTextSize(rowLabel).x;
                    ImGui::SameLine(std::max(190.f, labelW + 12.f));

                    int win = mk ? mk->window : 0;
                    ImGui::SetNextItemWidth(60);
                    if (ImGui::InputInt("##win", &win, 0, 0)) {
                        win = std::max(1, win);
                        if (mk) {
                            mk->window = win;
                            mk->manual = true;
                        } else {
                            GucciEngine::FrameWindowMark nm;
                            nm.frame = a.m_frame;
                            nm.player2 = a.m_player2;
                            nm.window = win;
                            nm.manual = true;
                            nm.isRelease = isRel;
                            if (a.m_frame < samples.size()) {
                                nm.x =
                                    a.m_player2 ? samples[a.m_frame].p2x : samples[a.m_frame].p1x;
                                nm.y =
                                    a.m_player2 ? samples[a.m_frame].p2y : samples[a.m_frame].p1y;
                            }
                            nm.percent = pct >= 0.f ? pct : 0.f;
                            engine->fwMarks.push_back(nm);
                            engine->fwHasData = true;
                        }
                    }
                    if (mk) {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              mk->manual ? theme.getAccent() : theme.textSecondary);
                        ImGui::TextUnformatted(mk->manual ? "manual" : "auto");
                        ImGui::PopStyleColor();
                        if (mk->manual) {
                            ImGui::SameLine();
                            if (Widgets::StyledButton("Clear", ImVec2(50, 20), theme, anim, 4.f)) {
                                engine->fwMarks.erase(engine->fwMarks.begin() +
                                                      (mk - engine->fwMarks.data()));
                                engine->fwHasData = !engine->fwMarks.empty();
                            }
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();

                if (Widgets::StyledButton("Save Manual Marks", ImVec2(-1, 26), theme, anim, 6.f))
                    engine->saveFwMarksNow();
            }
        }

        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Tiers map gap sizes to a marker image and sound. Put PNG/audio files in the mod's "
            "fw_assets folder and enter the filenames. No tier = default colored ring.");
        ImGui::TextWrapped("Sound: leave blank for the built-in default (Gucci Mane saying \"Brrr\" "
                           "-- that's intentional, not a broken file), type a filename for custom, "
                           "or 'none' to silence that tier. Markers appear as the bot reaches each "
                           "click.");
        ImGui::PopStyleColor();
        {
            float halfW = (ImGui::GetContentRegionAvail().x - 8) / 2.f;
            if (Widgets::StyledButton("Import Sounds/Images", ImVec2(halfW, 24), theme, anim, 4.f))
                importFwAssetFiles();
            ImGui::SameLine(0, 8);
            if (Widgets::StyledButton("Import Folder", ImVec2(halfW, 24), theme, anim, 4.f))
                importFwAssetFolder();
        }
        ImGui::Dummy(ImVec2(0, 4));
        int tierRemove = -1;
        for (size_t ti = 0; ti < engine->fwTiers.size(); ++ti) {
            auto& t = engine->fwTiers[ti];
            ImGui::PushID((int)(7000 + ti));
            char hdrLabel[64];
            snprintf(hdrLabel, sizeof(hdrLabel), "Tier %d-%d frames###tierhdr", t.lo, t.hi);
            if (!ImGui::CollapsingHeader(hdrLabel)) {
                ImGui::PopID();
                continue;
            }
            float third = (ImGui::GetContentRegionAvail().x - 16) / 3.f;
            ImGui::SetNextItemWidth(third);
            ImGui::InputInt("##lo", &t.lo, 0, 0);
            ImGui::SameLine(0, 8);
            ImGui::SetNextItemWidth(third);
            ImGui::InputInt("##hi", &t.hi, 0, 0);
            ImGui::SameLine(0, 8);
            if (Widgets::StyledButton("X", ImVec2(-1, 22), theme, anim, 4.f))
                tierRemove = (int)ti;
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##legendgroup",
                                     "Legend group (optional -- e.g. make lo=hi=5 and lo=hi=6 both "
                                     "'5-6' to customize each "
                                     "individually but combine them in the Legend)",
                                     t.legendGroup,
                                     sizeof(t.legendGroup));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint(
                "##img", "marker.png (in fw_assets)", t.imageFile, sizeof(t.imageFile));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint(
                "##snd", "sound file, or 'none' for silent", t.soundFile, sizeof(t.soundFile));
            float col3[3] = {t.r, t.g, t.b};
            if (ImGui::ColorEdit3("##col", col3, ImGuiColorEditFlags_NoInputs)) {
                t.r = col3[0];
                t.g = col3[1];
                t.b = col3[2];
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextUnformatted("tint / ring color");
            ImGui::PopStyleColor();

            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextUnformatted("Shape (used when no marker image is set above):");
            ImGui::PopStyleColor();
            {
                const char* shapeNames[] = {"Circle", "Star", "Spiral", "Geometric"};
                int shapeIdx = (int)t.shape;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##shape", &shapeIdx, shapeNames, 4))
                    t.shape = (GucciEngine::FwMarkerShape)shapeIdx;
            }
            if (t.shape == GucciEngine::FwMarkerShape::Polygon) {
                ImGui::SetNextItemWidth(third);
                ImGui::InputInt("##sides", &t.polygonSides, 0, 0);
                t.polygonSides = std::clamp(t.polygonSides, 3, 12);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextUnformatted("sides");
                ImGui::PopStyleColor();
                ImGui::SetNextItemWidth(third);
                ImGui::SliderFloat("##cornerrad", &t.polygonCornerRadius, 0.f, 1.f, "%.2f");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextUnformatted("corner radius");
                ImGui::PopStyleColor();
            }

            {
                const char* fillNames[] = {"Inverted (default)", "Normal (donut)"};
                int fillIdx = (int)t.fillStyle;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##fillstyle", &fillIdx, fillNames, 2))
                    t.fillStyle = (GucciEngine::FwFillStyle)fillIdx;
            }
            if (t.fillStyle == GucciEngine::FwFillStyle::Normal) {
                ImGui::Checkbox("No Border##tier", &t.noBorder);
            } else if (t.noBorder) {
                t.noBorder = false;
            }
            ImGui::SetNextItemWidth(third);
            ImGui::SliderFloat("##stroke", &t.strokeSize, 0.5f, 10.f, "%.1f stroke");
            ImGui::SetNextItemWidth(third);
            ImGui::SliderFloat("##size", &t.sizeScale, 0.3f, 3.f, "%.2fx size");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##vol", &t.volume, 0.f, 1.f, "%.2f sound volume");

            if (ImGui::TreeNodeEx("Pulse Effects", ImGuiTreeNodeFlags_None)) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(
                    "Fades from the marker/text's normal color to the pulse color, "
                    "holds, then fades back -- once, the moment this mark first shows "
                    "up each time you watch the macro play. Doesn't loop.");
                ImGui::PopStyleColor();
                ImGui::Checkbox("Enable Marker Pulse", &t.markerPulseEnabled);
                if (t.markerPulseEnabled) {
                    ImGui::ColorEdit3(
                        "Marker Pulse Color", t.markerPulseColor, ImGuiColorEditFlags_NoInputs);
                    ImGui::SetNextItemWidth(third);
                    ImGui::InputFloat("##mfi", &t.markerPulseFadeIn, 0, 0, "%.2fs in");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(third);
                    ImGui::InputFloat("##mhd", &t.markerPulseHold, 0, 0, "%.2fs hold");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(third);
                    ImGui::InputFloat("##mfo", &t.markerPulseFadeOut, 0, 0, "%.2fs out");
                    t.markerPulseFadeIn = std::max(0.f, t.markerPulseFadeIn);
                    t.markerPulseHold = std::max(0.f, t.markerPulseHold);
                    t.markerPulseFadeOut = std::max(0.f, t.markerPulseFadeOut);
                }
                ImGui::Checkbox("Enable Text Pulse", &t.textPulseEnabled);
                if (t.textPulseEnabled) {
                    ImGui::ColorEdit3(
                        "Text Pulse Color", t.textPulseColor, ImGuiColorEditFlags_NoInputs);
                    ImGui::SetNextItemWidth(third);
                    ImGui::InputFloat("##tfi", &t.textPulseFadeIn, 0, 0, "%.2fs in");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(third);
                    ImGui::InputFloat("##thd", &t.textPulseHold, 0, 0, "%.2fs hold");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(third);
                    ImGui::InputFloat("##tfo", &t.textPulseFadeOut, 0, 0, "%.2fs out");
                    t.textPulseFadeIn = std::max(0.f, t.textPulseFadeIn);
                    t.textPulseHold = std::max(0.f, t.textPulseHold);
                    t.textPulseFadeOut = std::max(0.f, t.textPulseFadeOut);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (tierRemove >= 0)
            engine->fwTiers.erase(engine->fwTiers.begin() + tierRemove);
        ImGui::Separator();
        if (Widgets::StyledButton("+ Add Range", ImVec2(-1, 26), theme, anim, 6.f)) {
            GucciEngine::FrameWindowTier nt;
            if (!engine->fwTiers.empty()) {
                nt.lo = engine->fwTiers.back().hi + 1;
                nt.hi = nt.lo + 3;
            }
            engine->fwTiers.push_back(nt);
        }
        if (!engine->fwTiers.empty()) {
            if (Widgets::StyledButton("Save Tiers", ImVec2(-1, 24), theme, anim, 6.f)) {
                std::string enc;
                for (auto& t : engine->fwTiers) {
                    enc += std::to_string(t.lo) + "|" + std::to_string(t.hi) + "|" + t.imageFile +
                           "|" + t.soundFile + "|" + std::to_string(t.r) + "|" +
                           std::to_string(t.g) + "|" + std::to_string(t.b) + "|" +
                           std::to_string((int)t.shape) + "|" + std::to_string(t.polygonSides) +
                           "|" + std::to_string(t.polygonCornerRadius) + "|" +
                           std::to_string((int)t.fillStyle) + "|" + (t.noBorder ? "1" : "0") + "|" +
                           std::to_string(t.strokeSize) + "|" + std::to_string(t.volume) + "|" +
                           (t.markerPulseEnabled ? "1" : "0") + "|" +
                           std::to_string(t.markerPulseColor[0]) + "|" +
                           std::to_string(t.markerPulseColor[1]) + "|" +
                           std::to_string(t.markerPulseColor[2]) + "|" +
                           std::to_string(t.markerPulseFadeIn) + "|" +
                           std::to_string(t.markerPulseHold) + "|" +
                           std::to_string(t.markerPulseFadeOut) + "|" +
                           (t.textPulseEnabled ? "1" : "0") + "|" +
                           std::to_string(t.textPulseColor[0]) + "|" +
                           std::to_string(t.textPulseColor[1]) + "|" +
                           std::to_string(t.textPulseColor[2]) + "|" +
                           std::to_string(t.textPulseFadeIn) + "|" +
                           std::to_string(t.textPulseHold) + "|" +
                           std::to_string(t.textPulseFadeOut) + "|" + std::to_string(t.sizeScale) +
                           "|" + t.legendGroup + ";";
                }
                Mod::get()->setSavedValue("fw_tiers", enc);
            }
        }
    }

    void MenuInterface::drawRenderTab() {
        auto* engine = GucciEngine::get();
        auto* mod = Mod::get();
        struct ResPreset {
            const char* name;
            int w, h;
        };
        static const ResPreset presets[] = {{"720p (1280x720)", 1280, 720},
                                            {"1080p (1920x1080)", 1920, 1080},
                                            {"1440p (2560x1440)", 2560, 1440},
                                            {"4K (3840x2160)", 3840, 2160}};
        if (!renderBufsInit)
            loadRenderSettings();
        float iW = ImGui::GetContentRegionAvail().x * 0.45f;

        if (!SLRenderer::get()->isFFmpegLoaded()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.55f, 0.3f, 1.f));
            ImGui::TextWrapped(
                "FFmpeg libraries not found -- rendering won't work until these are installed.");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "Get a shared FFmpeg 8.0 Windows build (avutil-60.dll, swresample-6.dll, "
                "swscale-9.dll, avcodec-62.dll, avformat-62.dll, avfilter-11.dll, avdevice-62.dll) "
                "and "
                "place all 7 DLLs in the folder below.");
            ImGui::PopStyleColor();
            if (Widgets::StyledButton("Open FFmpeg Folder", ImVec2(-1, 24), theme, anim, 4.f)) {
                auto libDir = Mod::get()->getPersistentDir() / "libraries";
                std::error_code ec;
                if (std::filesystem::exists(libDir, ec) ||
                    std::filesystem::create_directories(libDir, ec))
                    utils::file::openFolder(libDir);
            }
            ImGui::Dummy(ImVec2(0, 6));
        }

        Widgets::SectionHeader("Render Presets", theme);
        static char presetNameBuf[64] = "My Preset";
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 170);
        ImGui::InputText("##presetName", presetNameBuf, sizeof(presetNameBuf));
        ImGui::SameLine(0, 6);
        if (Widgets::StyledButton("Save##preset", ImVec2(76, 0), theme, anim)) {
            std::string pk = std::string("rp_") + presetNameBuf;
            mod->setSavedValue(pk + "_w", std::string(renderWidthBuf));
            mod->setSavedValue(pk + "_h", std::string(renderHeightBuf));
            mod->setSavedValue(pk + "_fps", std::string(renderFpsBuf));
            mod->setSavedValue(pk + "_codec", std::string(renderCodecBuf));
            mod->setSavedValue(pk + "_bitrate", std::string(renderBitrateBuf));
            mod->setSavedValue(pk + "_ext", std::string(renderExtBuf));
            mod->setSavedValue(pk + "_args", std::string(renderArgsBuf));
            mod->setSavedValue(pk + "_pixfmt", std::string(renderPixFmtBuf));
            mod->setSavedValue(pk + "_vargs", std::string(renderVideoArgsBuf));
            mod->setSavedValue(pk + "_aargs", std::string(renderAudioArgsBuf));
            mod->setSavedValue(pk + "_safter", std::string(renderSecondsAfterBuf));
            mod->setSavedValue(pk + "_acodec", std::string(renderAudioCodecBuf));
            mod->setSavedValue(pk + "_abitrate", std::string(renderAudioBitrateBuf));
            auto existing = mod->getSavedValue<std::string>("rp_list", "");
            if (existing.find(std::string(presetNameBuf) + "|") == std::string::npos)
                mod->setSavedValue("rp_list", existing + presetNameBuf + "|");
        }
        ImGui::SameLine(0, 6);
        if (Widgets::StyledButton("Load##preset", ImVec2(76, 0), theme, anim)) {
            std::string pk = std::string("rp_") + presetNameBuf;
            if (mod->hasSavedValue(pk + "_w")) {
                snprintf(renderWidthBuf,
                         sizeof(renderWidthBuf),
                         "%s",
                         mod->getSavedValue<std::string>(pk + "_w", "1920").c_str());
                snprintf(renderHeightBuf,
                         sizeof(renderHeightBuf),
                         "%s",
                         mod->getSavedValue<std::string>(pk + "_h", "1080").c_str());
                snprintf(renderFpsBuf,
                         sizeof(renderFpsBuf),
                         "%s",
                         mod->getSavedValue<std::string>(pk + "_fps", "60").c_str());
                snprintf(renderCodecBuf,
                         sizeof(renderCodecBuf),
                         "%s",
                         mod->getSavedValue<std::string>(pk + "_codec", "").c_str());
                snprintf(renderBitrateBuf,
                         sizeof(renderBitrateBuf),
                         "%s",
                         mod->getSavedValue<std::string>(pk + "_bitrate", "30").c_str());
                snprintf(renderExtBuf,
                         sizeof(renderExtBuf),
                         "%s",
                         mod->getSavedValue<std::string>(pk + "_ext", ".mp4").c_str());
                snprintf(renderArgsBuf,
                         sizeof(renderArgsBuf),
                         "%s",
                         mod->getSavedValue<std::string>(pk + "_args", "-pix_fmt yuv420p").c_str());
                snprintf(renderPixFmtBuf,
                         sizeof(renderPixFmtBuf),
                         "%s",
                         mod->getSavedValue<std::string>(pk + "_pixfmt", "yuv420p").c_str());
                snprintf(renderVideoArgsBuf,
                         sizeof(renderVideoArgsBuf),
                         "%s",
                         mod->getSavedValue<std::string>(pk + "_vargs", "").c_str());
                snprintf(renderAudioArgsBuf,
                         sizeof(renderAudioArgsBuf),
                         "%s",
                         mod->getSavedValue<std::string>(pk + "_aargs", "").c_str());
                snprintf(renderSecondsAfterBuf,
                         sizeof(renderSecondsAfterBuf),
                         "%s",
                         mod->getSavedValue<std::string>(pk + "_safter", "3").c_str());
                snprintf(renderAudioCodecBuf,
                         sizeof(renderAudioCodecBuf),
                         "%s",
                         mod->getSavedValue<std::string>(pk + "_acodec", "aac").c_str());
                snprintf(renderAudioBitrateBuf,
                         sizeof(renderAudioBitrateBuf),
                         "%s",
                         mod->getSavedValue<std::string>(pk + "_abitrate", "192k").c_str());
            }
        }
        {
            auto list = mod->getSavedValue<std::string>("rp_list", "");
            if (!list.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::Text("Saved: %s", list.c_str());
                ImGui::PopStyleColor();
            }
        }

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Render", theme);
        ImGui::Text("Output Name");
        ImGui::SameLine(iW);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##rName", renderNameBuf, sizeof(renderNameBuf)))
            mod->setSavedValue("render_name", std::string(renderNameBuf));

        ImGui::Text("Output Folder");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##rFolder",
                             outputFolderBuf,
                             sizeof(outputFolderBuf),
                             ImGuiInputTextFlags_AutoSelectAll))
            mod->setSavedValue("render_output_folder", std::string(outputFolderBuf));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::Text("Type or paste a path. Leave blank to use GD/renders/");
        ImGui::PopStyleColor();
        float obtnW = (ImGui::GetContentRegionAvail().x - 8) * 0.5f;
        if (Widgets::StyledButton("Create & Open##folder", ImVec2(obtnW, 0), theme, anim)) {
            std::filesystem::path folderPath(outputFolderBuf);
            if (folderPath.empty())
                folderPath = dirs::getGameDir() / "renders";
            std::error_code ec;
            if (!std::filesystem::exists(folderPath, ec))
                std::filesystem::create_directories(folderPath, ec);
            utils::file::openFolder(folderPath);
        }
        ImGui::SameLine(0, 8);
        if (Widgets::StyledButton("Clear##folder", ImVec2(obtnW, 0), theme, anim)) {
            outputFolderBuf[0] = 0;
            mod->setSavedValue("render_output_folder", std::string(""));
        }
        ImGui::Dummy(ImVec2(0, 4));
        auto* sl = SLRenderer::get();
        if (sl->isRecording()) {
            Widgets::StatusBadge("Rendering", ImVec4(0.9f, 0.3f, 0.3f, 1.f));
            ImGui::Dummy(ImVec2(0, 4));
            if (Widgets::StyledButton("Stop Render", ImVec2(-1, 36), theme, anim))
                sl->signalStop();
        } else {
            if (Widgets::StyledButton("Start Render", ImVec2(-1, 36), theme, anim)) {
                sl->loadSettingsFromGeode();
                sl->queueStart();
            }
        }
        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Resolution", theme);
        ImGui::Text("Preset");
        ImGui::SameLine(iW);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##rPreset", presets[renderPresetIndex].name)) {
            for (int i = 0; i < 4; i++) {
                bool sel = (renderPresetIndex == i);
                if (ImGui::Selectable(presets[i].name, sel)) {
                    renderPresetIndex = i;
                    snprintf(renderWidthBuf, sizeof(renderWidthBuf), "%d", presets[i].w);
                    snprintf(renderHeightBuf, sizeof(renderHeightBuf), "%d", presets[i].h);
                }
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::Text("FPS");
        ImGui::SameLine(iW);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText(
            "##rFPS", renderFpsBuf, sizeof(renderFpsBuf), ImGuiInputTextFlags_CharsDecimal);
        {
            struct QPreset {
                const char* n;
                int w, h, f;
                const char* br;
            };
            static const QPreset qp[] = {{"1080p60", 1920, 1080, 60, "16"},
                                         {"1440p60", 2560, 1440, 60, "24"},
                                         {"4K60", 3840, 2160, 60, "50"}};
            float qw = (ImGui::GetContentRegionAvail().x - 16) / 3.f;
            for (int i = 0; i < 3; i++) {
                if (i)
                    ImGui::SameLine(0, 8);
                if (Widgets::StyledButton(qp[i].n, ImVec2(qw, 24), theme, anim)) {
                    snprintf(renderWidthBuf, sizeof(renderWidthBuf), "%d", qp[i].w);
                    snprintf(renderHeightBuf, sizeof(renderHeightBuf), "%d", qp[i].h);
                    snprintf(renderFpsBuf, sizeof(renderFpsBuf), "%d", qp[i].f);
                    snprintf(renderBitrateBuf, sizeof(renderBitrateBuf), "%s", qp[i].br);
                    mod->setSavedValue("render_width", (int64_t)qp[i].w);
                    mod->setSavedValue("render_height", (int64_t)qp[i].h);
                    mod->setSavedValue("render_fps", (int64_t)qp[i].f);
                    mod->setSavedValue("render_bitrate", std::string(qp[i].br));
                }
            }
        }
        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Encoding", theme);

        static const char* vCodecs[] = {"libx264",
                                        "libx265",
                                        "h264_nvenc",
                                        "hevc_nvenc",
                                        "h264_amf",
                                        "hevc_amf",
                                        "h264_qsv",
                                        "hevc_qsv",
                                        "libvpx-vp9",
                                        "av1_nvenc",
                                        "libaom-av1"};
        static const int vCodecCount = 11;
        static char vCodecSearch[64] = "";
        ImGui::Text("Video Codec");
        ImGui::SameLine(iW);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##vCodecCombo", renderCodecBuf[0] ? renderCodecBuf : "Select...")) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##vCodecSearch", vCodecSearch, sizeof(vCodecSearch));
            ImGui::Separator();
            std::string vSearch(vCodecSearch);
            std::transform(vSearch.begin(), vSearch.end(), vSearch.begin(), ::tolower);
            for (int i = 0; i < vCodecCount; i++) {
                std::string cn(vCodecs[i]);
                std::string cnl = cn;
                std::transform(cnl.begin(), cnl.end(), cnl.begin(), ::tolower);
                if (!vSearch.empty() && cnl.find(vSearch) == std::string::npos)
                    continue;
                bool sel = (cn == renderCodecBuf);
                if (ImGui::Selectable(vCodecs[i], sel)) {
                    snprintf(renderCodecBuf, sizeof(renderCodecBuf), "%s", vCodecs[i]);
                    mod->setSavedValue("render_codec", std::string(renderCodecBuf));
                }
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::Text("Or type custom:");
            ImGui::PopStyleColor();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##vCodecCustom",
                                 renderCodecBuf,
                                 sizeof(renderCodecBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
                mod->setSavedValue("render_codec", std::string(renderCodecBuf));
            ImGui::EndCombo();
        }

        ImGui::Text("Bitrate (M)");
        ImGui::SameLine(iW);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##rBitrate", renderBitrateBuf, sizeof(renderBitrateBuf));
        ImGui::Text("Extension");
        ImGui::SameLine(iW);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##rExt", renderExtBuf, sizeof(renderExtBuf));
        ImGui::Text("Pixel Format");
        ImGui::SameLine(iW);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##rPixFmt", renderPixFmtBuf, sizeof(renderPixFmtBuf)))
            mod->setSavedValue("render_pix_fmt", std::string(renderPixFmtBuf));

        static const char* aCodecs[] = {
            "aac", "mp3", "opus", "flac", "ac3", "eac3", "vorbis", "pcm_s16le", "copy"};
        static const int aCodecCount = 9;
        static const char* aCodecDescs[] = {"AAC (recommended)",
                                            "MP3",
                                            "Opus (great quality)",
                                            "FLAC (lossless)",
                                            "AC3 (Dolby)",
                                            "E-AC3",
                                            "Vorbis (OGG)",
                                            "PCM WAV (uncompressed)",
                                            "Copy stream as-is"};
        static char aCodecSearch[64] = "";
        ImGui::Text("Audio Codec");
        ImGui::SameLine(iW);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##aCodecCombo",
                              renderAudioCodecBuf[0] ? renderAudioCodecBuf : "Select...")) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##aCodecSearch", aCodecSearch, sizeof(aCodecSearch));
            ImGui::Separator();
            std::string aSearch(aCodecSearch);
            std::transform(aSearch.begin(), aSearch.end(), aSearch.begin(), ::tolower);
            for (int i = 0; i < aCodecCount; i++) {
                std::string cn(aCodecs[i]);
                std::string cnl = cn;
                std::transform(cnl.begin(), cnl.end(), cnl.begin(), ::tolower);
                if (!aSearch.empty() && cnl.find(aSearch) == std::string::npos)
                    continue;
                bool sel = (cn == renderAudioCodecBuf);
                char label[128];
                snprintf(label, sizeof(label), "%s  —  %s", aCodecs[i], aCodecDescs[i]);
                if (ImGui::Selectable(label, sel)) {
                    snprintf(renderAudioCodecBuf, sizeof(renderAudioCodecBuf), "%s", aCodecs[i]);
                    mod->setSavedValue("render_audio_codec", std::string(renderAudioCodecBuf));
                }
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::Text("Or type custom:");
            ImGui::PopStyleColor();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##aCodecCustom",
                                 renderAudioCodecBuf,
                                 sizeof(renderAudioCodecBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
                mod->setSavedValue("render_audio_codec", std::string(renderAudioCodecBuf));
            ImGui::EndCombo();
        }

        static const char* aBitrates[] = {"96k", "128k", "192k", "256k", "320k", "512k"};
        static const int aBitrateCount = 6;
        ImGui::Text("Audio Bitrate");
        ImGui::SameLine(iW);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##aBitrateCombo",
                              renderAudioBitrateBuf[0] ? renderAudioBitrateBuf : "192k")) {
            for (int i = 0; i < aBitrateCount; i++) {
                bool sel = (std::string(aBitrates[i]) == renderAudioBitrateBuf);
                if (ImGui::Selectable(aBitrates[i], sel)) {
                    snprintf(
                        renderAudioBitrateBuf, sizeof(renderAudioBitrateBuf), "%s", aBitrates[i]);
                    mod->setSavedValue("render_audio_bitrate", std::string(renderAudioBitrateBuf));
                }
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::Text("Or type custom:");
            ImGui::PopStyleColor();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##aBitrateCustom",
                                 renderAudioBitrateBuf,
                                 sizeof(renderAudioBitrateBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
                mod->setSavedValue("render_audio_bitrate", std::string(renderAudioBitrateBuf));
            ImGui::EndCombo();
        }
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::Text("Video Args (passed to -vf)");
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##rVArgs", renderVideoArgsBuf, sizeof(renderVideoArgsBuf)))
            mod->setSavedValue("render_video_args", std::string(renderVideoArgsBuf));
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::Text("Audio Args (extra ffmpeg audio flags)");
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##rAArgs", renderAudioArgsBuf, sizeof(renderAudioArgsBuf)))
            mod->setSavedValue("render_audio_args", std::string(renderAudioArgsBuf));
        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Audio", theme);
        if (Widgets::ToggleSwitch("Include Audio", &renderIncludeAudio, theme, anim))
            mod->setSavedValue("render_include_audio", renderIncludeAudio);
        if (renderIncludeAudio) {
            if (Widgets::ToggleSwitch("Split Into 4 Tracks", &renderSplitAudioTracks, theme, anim))
                mod->setSavedValue("render_split_audio_tracks", renderSplitAudioTracks);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "Off: one combined audio track, same as always. On: four tracks in the "
                "output file -- the combined mix, plus music, level/UI SFX, and "
                "frame-window cues isolated separately.");
            ImGui::PopStyleColor();
            Widgets::StyledSliderFloat("Music Volume", &renderMusicVol, 0.f, 2.f, theme, true);
            Widgets::StyledSliderFloat("SFX Volume", &renderSfxVol, 0.f, 2.f, theme, true);
        }
        if (Widgets::ToggleSwitch("Auto Color Fix", &renderColorFix, theme, anim))
            mod->setSavedValue("render_color_fix", renderColorFix);
        if (Widgets::ToggleSwitch("Include Click Sounds", &renderIncludeClicks, theme, anim))
            mod->setSavedValue("render_include_clicks", renderIncludeClicks);
        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Options", theme);
        if (Widgets::ToggleSwitch("Hide End Screen", &renderHideEndscreen, theme, anim))
            mod->setSavedValue("render_hide_endscreen", renderHideEndscreen);
        if (Widgets::ToggleSwitch("Hide Level Complete", &renderHideLevelComplete, theme, anim))
            mod->setSavedValue("render_hide_levelcomplete", renderHideLevelComplete);
        ImGui::Dummy(ImVec2(0, 4));
        {
            std::error_code ffec;
            auto ffSave = Mod::get()->getSaveDir() / "ffmpeg.exe";
            auto ffRes = Mod::get()->getResourcesDir() / "ffmpeg.exe";
            bool ffFound =
                std::filesystem::exists(ffSave, ffec) || std::filesystem::exists(ffRes, ffec);
            if (ffFound) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.f, 0.3f, 1.f));
                ImGui::TextWrapped("ffmpeg.exe found.");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.8f, 0.2f, 0.8f));
                ImGui::TextWrapped(
                    "ffmpeg.exe not found. Place ffmpeg.exe in the mod resources or save folder.");
                ImGui::PopStyleColor();
            }
        }

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Playback Fixes", theme);
        Widgets::ToggleSwitch("Scroll Speed Fix", &engine->updater.m_ssbFix, theme, anim);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped("Prevents scroll speed desync at high TPS. Recommended when rendering.");
        ImGui::PopStyleColor();
    }

    void MenuInterface::drawClicksTab() {
        auto* csm = ClickSoundManager::get();
        auto* mod = Mod::get();
        if (!clickPacksScanned) {
            csm->scanClickPacks();
            csm->scanClickPacksP2();
            clickPacksScanned = true;
            if (!csm->activePackName.empty())
                for (int i = 0; i < (int)csm->availablePacks.size(); i++)
                    if (csm->availablePacks[i] == csm->activePackName) {
                        clickPackIndex = i;
                        break;
                    }
            if (!csm->activePackNameP2.empty())
                for (int i = 0; i < (int)csm->availablePacksP2.size(); i++)
                    if (csm->availablePacksP2[i] == csm->activePackNameP2) {
                        clickPackIndexP2 = i;
                        break;
                    }
        }
        ImGui::Dummy(ImVec2(0, 4));
        if (Widgets::ModuleCard("Click Sounds",
                                "Play click and release sounds on input",
                                &csm->enabled,
                                theme,
                                anim))
            mod->setSavedValue("click_enabled", csm->enabled);
        ImGui::Dummy(ImVec2(0, 6));
        Widgets::SectionHeader("Click Pack", theme);
        float bw = 80.f, cw = ImGui::GetContentRegionAvail().x - bw - 8.f;
        if (csm->availablePacks.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 0.6f));
            ImGui::TextWrapped("No click packs found. Use Open Folder to add packs.");
            ImGui::PopStyleColor();
        } else {
            ImGui::SetNextItemWidth(cw);
            if (ImGui::BeginCombo("##cp", csm->availablePacks[clickPackIndex].c_str())) {
                for (int i = 0; i < (int)csm->availablePacks.size(); i++) {
                    bool sel = (clickPackIndex == i);
                    if (ImGui::Selectable(csm->availablePacks[i].c_str(), sel)) {
                        clickPackIndex = i;
                        csm->activePackName = csm->availablePacks[i];
                        csm->loadClickPack(csm->activePackName, csm->p1Pack);
                        mod->setSavedValue("click_pack", csm->activePackName);
                    }
                    if (sel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
        }
        if (Widgets::StyledButton("Refresh", ImVec2(bw, 0), theme, anim)) {
            csm->scanClickPacks();
            clickPackIndex = 0;
            if (!csm->availablePacks.empty()) {
                csm->activePackName = csm->availablePacks[0];
                csm->loadClickPack(csm->activePackName, csm->p1Pack);
            }
        }
        ImGui::Dummy(ImVec2(0, 4));
        if (Widgets::StyledButton("Open Folder", ImVec2(-1, 32), theme, anim))
            csm->openClickFolder();
        if (!csm->p1Pack.empty()) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::Text("Hard: %d  Soft: %d  Release: %d  Noise: %d",
                        csm->p1Pack.hardCount(),
                        csm->p1Pack.softCount(),
                        csm->p1Pack.releaseCount(),
                        csm->p1Pack.noiseCount());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Volume", theme);
        if (!csm->p1Pack.hardClicks.empty())
            if (Widgets::StyledSliderFloat(
                    "Hard Click", &csm->p1Pack.hardVolume, 0.f, 2.f, theme, true))
                mod->setSavedValue("click_hard_vol", (double)csm->p1Pack.hardVolume);
        if (!csm->p1Pack.softClicks.empty())
            if (Widgets::StyledSliderFloat(
                    "Soft Click", &csm->p1Pack.softVolume, 0.f, 2.f, theme, true))
                mod->setSavedValue("click_soft_vol", (double)csm->p1Pack.softVolume);
        if (csm->p1Pack.releaseCount() > 0)
            if (Widgets::StyledSliderFloat(
                    "Release", &csm->p1Pack.releaseVolume, 0.f, 2.f, theme, true))
                mod->setSavedValue("click_release_vol", (double)csm->p1Pack.releaseVolume);
        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Behavior", theme);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::Text("Softness -- 0 = always hard,  1 = always soft clicks");
        ImGui::PopStyleColor();
        if (Widgets::StyledSliderFloat("##softness", &csm->softness, 0.f, 1.f, theme))
            mod->setSavedValue("click_softness", (double)csm->softness);
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::Text("Click Delay -- adds a random timing offset to sound more human");
        ImGui::PopStyleColor();
        if (Widgets::StyledSliderFloat("##delaymin", &csm->clickDelayMin, 0.f, 100.f, theme)) {
            if (csm->clickDelayMin > csm->clickDelayMax)
                csm->clickDelayMax = csm->clickDelayMin;
            mod->setSavedValue("click_delay_min", (double)csm->clickDelayMin);
        }
        if (Widgets::StyledSliderFloat("##delaymax", &csm->clickDelayMax, 0.f, 100.f, theme)) {
            if (csm->clickDelayMax < csm->clickDelayMin)
                csm->clickDelayMin = csm->clickDelayMax;
            mod->setSavedValue("click_delay_max", (double)csm->clickDelayMax);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::Text("Min: %.0f ms    Max: %.0f ms", csm->clickDelayMin, csm->clickDelayMax);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 4));
        if (Widgets::ToggleSwitch("Play During Playback", &csm->playDuringPlayback, theme, anim))
            mod->setSavedValue("click_play_during_playback", csm->playDuringPlayback);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped("When off, click sounds are silent during macro playback.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Background Noise", theme);
        if (csm->p1Pack.noiseFiles.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 0.6f));
            ImGui::TextWrapped("No noise files. Add a 'noise' folder to your click pack.");
            ImGui::PopStyleColor();
        } else {
            if (Widgets::ToggleSwitch(
                    "Enable Background Noise", &csm->backgroundNoiseEnabled, theme, anim)) {
                mod->setSavedValue("click_bg_noise", csm->backgroundNoiseEnabled);
                if (csm->backgroundNoiseEnabled)
                    csm->startBackgroundNoise();
                else
                    csm->stopBackgroundNoise();
            }
            if (Widgets::StyledSliderFloat(
                    "Noise Volume", &csm->backgroundNoiseVolume, 0.f, 2.f, theme, true)) {
                mod->setSavedValue("click_bg_noise_vol", (double)csm->backgroundNoiseVolume);
                if (csm->bgNoiseChannel)
                    csm->bgNoiseChannel->setVolume(csm->backgroundNoiseVolume);
            }
        }
        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Player 2", theme);
        if (Widgets::ToggleSwitch("Separate P2 Clicks", &csm->separateP2Clicks, theme, anim))
            mod->setSavedValue("click_separate_p2", csm->separateP2Clicks);
        if (csm->separateP2Clicks) {
            ImGui::Dummy(ImVec2(0, 6));
            Widgets::SectionHeader("P2 Click Pack", theme);
            float p2bw = 80.f, p2cw = ImGui::GetContentRegionAvail().x - p2bw - 8.f;
            if (!csm->availablePacksP2.empty()) {
                ImGui::SetNextItemWidth(p2cw);
                if (ImGui::BeginCombo("##cpp2", csm->availablePacksP2[clickPackIndexP2].c_str())) {
                    for (int i = 0; i < (int)csm->availablePacksP2.size(); i++) {
                        bool sel = (clickPackIndexP2 == i);
                        if (ImGui::Selectable(csm->availablePacksP2[i].c_str(), sel)) {
                            clickPackIndexP2 = i;
                            csm->activePackNameP2 = csm->availablePacksP2[i];
                            csm->loadClickPack(csm->activePackNameP2, csm->p2Pack, true);
                            mod->setSavedValue("click_pack_p2", csm->activePackNameP2);
                        }
                        if (sel)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
            }
            if (Widgets::StyledButton("Refresh##p2", ImVec2(p2bw, 0), theme, anim)) {
                csm->scanClickPacksP2();
                clickPackIndexP2 = 0;
                if (!csm->availablePacksP2.empty()) {
                    csm->activePackNameP2 = csm->availablePacksP2[0];
                    csm->loadClickPack(csm->activePackNameP2, csm->p2Pack, true);
                }
            }
            ImGui::Dummy(ImVec2(0, 4));
            if (Widgets::StyledButton("Open P2 Folder", ImVec2(-1, 32), theme, anim))
                csm->openClickFolderP2();
        }
    }

    void MenuInterface::drawAutoclickerTab() {
        auto* ac = Autoclicker::get();
        auto* mod = Mod::get();
        auto* eng = GucciEngine::get();
        ImGui::Dummy(ImVec2(0, 4));
        if (Widgets::ModuleCard(
                "Autoclicker", "Auto-click at configurable intervals", &ac->enabled, theme, anim))
            mod->setSavedValue("ac_enabled", ac->enabled);
        if (ac->enabled && eng->isPlaying()) {
            Widgets::StatusBadge("PAUSED (PLAYBACK)", ImVec4(0.8f, 0.6f, 0.2f, 1.f));
        } else if (ac->enabled) {
            Widgets::StatusBadge("ACTIVE", ImVec4(0.3f, 1.f, 0.4f, 1.f));
        }
        ImGui::Dummy(ImVec2(0, 8));
        // Per-player timing (Silicate 1.1.0 parity, 2026-09-03): each player
        // used to share one Hold/Release Ticks pair -- now fully
        // independent, with a one-shot "Sync to P1" copy button instead of
        // a permanent link, matching how Silicate's own version works.
        auto drawPlayerSettings = [&](const char* label,
                                      Autoclicker::PlayerSettings& s,
                                      const char* enabledKey,
                                      const char* holdKey,
                                      const char* releaseKey,
                                      const char* clicksKey) {
            ImGui::PushID(label);
            Widgets::SectionHeader(label, theme);
            if (Widgets::ToggleSwitch("Enabled", &s.enabled, theme, anim))
                mod->setSavedValue(enabledKey, s.enabled);
            if (Widgets::StyledSliderInt("Hold Ticks", &s.holdTicks, 1, 120, theme))
                mod->setSavedValue(holdKey, s.holdTicks);
            if (Widgets::StyledSliderInt("Release Ticks", &s.releaseTicks, 1, 120, theme))
                mod->setSavedValue(releaseKey, s.releaseTicks);
            if (Widgets::StyledSliderInt("Clicks Per Hold", &s.clicksPerHold, 1, 10, theme))
                mod->setSavedValue(clicksKey, s.clicksPerHold);
            float cps = (float)eng->updater.m_tps / (float)(s.holdTicks + s.releaseTicks);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::Text("~%.1f clicks/sec at %.0f TPS%s",
                        cps * (float)std::max(1, s.clicksPerHold),
                        eng->updater.m_tps,
                        s.clicksPerHold > 1 ? " (incl. extra clicks per hold)" : "");
            ImGui::PopStyleColor();
            ImGui::PopID();
        };

        drawPlayerSettings(
            "Player 1", ac->p1, "ac_p1_enabled", "ac_p1_hold_ticks", "ac_p1_release_ticks", "ac_p1_clicks");
        ImGui::Dummy(ImVec2(0, 8));
        drawPlayerSettings(
            "Player 2", ac->p2, "ac_p2_enabled", "ac_p2_hold_ticks", "ac_p2_release_ticks", "ac_p2_clicks");
        if (Widgets::StyledButton("Sync Player 2 to Player 1", ImVec2(-1, 28), theme, anim)) {
            ac->syncP2FromP1();
            mod->setSavedValue("ac_p2_enabled", ac->p2.enabled);
            mod->setSavedValue("ac_p2_hold_ticks", ac->p2.holdTicks);
            mod->setSavedValue("ac_p2_release_ticks", ac->p2.releaseTicks);
            mod->setSavedValue("ac_p2_clicks", ac->p2.clicksPerHold);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "A one-time copy, not a permanent link -- Player 2's settings can still be changed "
            "independently afterward.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Options", theme);
        if (Widgets::ToggleSwitch("Only While Holding", &ac->onlyWhileHolding, theme, anim))
            mod->setSavedValue("ac_only_holding", ac->onlyWhileHolding);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped("When enabled, only auto-clicks while you hold the jump button.");
        ImGui::PopStyleColor();
    }

    void MenuInterface::drawSettingsTab() {
        auto* eng = GucciEngine::get();
        Widgets::SectionHeader("Interface", theme);
        if (Widgets::ToggleSwitch("MegaHack-Style Menu", &megaHackLook, theme, anim))
            Mod::get()->setSavedValue("ui_megahack_look", megaHackLook);
        if (Widgets::ToggleSwitch("Compact Mode", &compactMode, theme, anim))
            Mod::get()->setSavedValue("ui_compact_mode", compactMode);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "A small corner panel instead of the full menu -- record/play, TPS/speed, frame step, "
            "noclip, and a few other essentials, small enough to leave open while actually "
            "playing.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 8));

        Widgets::SectionHeader("Diagnostics", theme);
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.getAccent());
            ImGui::Text("Build: %s", GB_BUILD_LABEL);
            ImGui::PopStyleColor();
            bool healthy = gbcheck::g_ranOnce && gbcheck::g_failCount == 0;
            ImVec4 statusCol = !gbcheck::g_ranOnce ? theme.textSecondary
                                                   : (healthy ? ImVec4(0.3f, 0.85f, 0.3f, 1.f)
                                                              : ImVec4(0.95f, 0.3f, 0.3f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text, statusCol);
            if (!gbcheck::g_ranOnce)
                ImGui::TextUnformatted("Self-check: not yet run");
            else if (healthy)
                ImGui::Text("Self-check: PASSED (%d checks)", gbcheck::g_passCount);
            else
                ImGui::Text("Self-check: FAILED (%d of %d failed)",
                            gbcheck::g_failCount,
                            gbcheck::g_passCount + gbcheck::g_failCount);
            ImGui::PopStyleColor();
            for (auto const& r : gbcheck::g_results) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      r.passed ? ImVec4(0.3f, 0.8f, 0.3f, 1.f)
                                               : ImVec4(0.95f, 0.35f, 0.35f, 1.f));
                ImGui::Text("  %s  %s", r.passed ? "[OK]" : "[X]", r.name.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            const char* mode = eng->isRecording() ? "Recording"
                               : eng->isPlaying() ? "Playing"
                                                  : "Idle";
            ImGui::Text(
                "Mode: %s   Stored frames: %zu", mode, eng->practiceFix.m_storedFrames.size());
            ImGui::Text("Frame-window marks: %zu   Analyzed: %s",
                        eng->fwMarks.size(),
                        eng->fwHasData ? "yes" : "no");
            ImGui::Text("Renderer: %s", eng->renderer.recording ? "RECORDING" : "idle");
            if (eng->renderer.lastRender.fileSize > 0)
                ImGui::Text("Last render: %.2f MB",
                            (double)eng->renderer.lastRender.fileSize / (1024.0 * 1024.0));
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 4));
            if (Widgets::ToggleSwitch(
                    "Log Frame Increments", &eng->updater.m_logFrameIncrements, theme, anim))
                Mod::get()->setSavedValue("diag_log_frame_increments",
                                          eng->updater.m_logFrameIncrements);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "Juice's frame-skip report: logs every place the frame counter advances, tagged by "
                "call site, to the mod log. Flip on right before reproducing (an MH step, a "
                "release "
                "test), then off -- leaving it on floods the log during normal play.");
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Theme", theme);
        int pc = ThemeEngine::getPresetCount();
        const ThemePreset* presets = ThemeEngine::getPresets();
        float colW = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
        float btnH = 32.f;
        for (int i = 0; i < pc; i++) {
            if (i % 2 == 1)
                ImGui::SameLine(0, 8);
            bool active = (theme.activePreset == i);
            bool clickedRk = false;
            if (i == (int)THEME_REDKINGDOM) {
                ImVec2 pos = ImGui::GetCursorScreenPos();
                float h = 32.f;
                ImGui::InvisibleButton(presets[i].name, ImVec2(colW, h));
                clickedRk = ImGui::IsItemClicked();
                bool hoveredRk = ImGui::IsItemHovered();
                ImVec4 pulse = theme.computeRedKingdomPulse();
                ImU32 pulseU32 = toU32(pulse);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec4 bgFill = withAlpha(pulse, active ? 0.28f : (hoveredRk ? 0.16f : 0.10f));
                dl->AddRectFilled(pos, ImVec2(pos.x + colW, pos.y + h), toU32(bgFill), 0.f);
                dl->AddRect(
                    pos, ImVec2(pos.x + colW, pos.y + h), pulseU32, 0.f, 0, active ? 2.2f : 1.4f);
                ImVec2 ts = ImGui::CalcTextSize(presets[i].name);
                dl->AddText(ImVec2(pos.x + (colW - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f),
                            pulseU32,
                            presets[i].name);
            }
            if (clickedRk || (i != (int)THEME_REDKINGDOM &&
                              Widgets::PillButton(presets[i].name, active, colW, theme, anim))) {
                theme.applyPreset(i);
                if (i == 0)
                    activeTheme = THEME_GUCCI;
                else if (i == 1)
                    activeTheme = THEME_TOOSII;
                else if (i == 2)
                    activeTheme = THEME_TOOSII_SYRACUSE;
                else if (i == 3)
                    activeTheme = THEME_TOOSII_SACSTATE;
                else if (i == 4)
                    activeTheme = THEME_JA;
                else if (i == 5)
                    activeTheme = THEME_GIDDEY;
                else if (i == 6)
                    activeTheme = THEME_BAM;
                else if (i == 7)
                    activeTheme = THEME_SEXYY;
                else if (i == 8)
                    activeTheme = THEME_JUICE;
                else if (i == 9)
                    activeTheme = THEME_BUTLER;
                else if (i == 10)
                    activeTheme = THEME_SAWEETIE;
                else if (i == 11)
                    activeTheme = THEME_MAYBACH;
                else if (i == 12)
                    activeTheme = THEME_ROMO;
                else if (i == 13)
                    activeTheme = THEME_GRIZZLEY;
                else if (i == 14)
                    activeTheme = THEME_REDKINGDOM;
                else if (i == 15)
                    activeTheme = THEME_LEMONADE;
                else if (i == 16)
                    activeTheme = THEME_BRRR;
                else if (i == 17)
                    activeTheme = THEME_WAKA;
                else if (i == 18)
                    activeTheme = THEME_YOUNGSTA;
                else if (i == 19)
                    activeTheme = THEME_KNOCKERZ;
                activeCustomThemeName.clear();
                if (BigBrrrManager::get()->enabled)
                    BigBrrrManager::get()->setEnabled(true);
                saveSettings();
            }
            if (i % 2 == 0 && i + 1 >= pc)
                ImGui::Dummy(ImVec2(0, 0));
        }

        ImGui::Dummy(ImVec2(0, 10));
        Widgets::SectionHeader("Custom Themes", theme);
        if (customThemes.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped("No custom themes yet -- colors, identity, quotes, and your own Big "
                               "Brrr track, all yours to set.");
            ImGui::PopStyleColor();
        } else {
            for (size_t i = 0; i < customThemes.size(); i++) {
                auto& ct = customThemes[i];
                ImGui::PushID((int)i + 20000);
                bool active = (activeTheme == THEME_CUSTOM && activeCustomThemeName == ct.name);
                float rowW = ImGui::GetContentRegionAvail().x;
                float editW = 50.f, gap = 6.f;
                if (Widgets::PillButton(ct.name.c_str(), active, rowW - editW - gap, theme, anim)) {
                    activeTheme = THEME_CUSTOM;
                    activeCustomThemeName = ct.name;
                    theme.accentColor = ct.accent;
                    theme.bgColor = ct.bg;
                    theme.cardColor = ct.card;
                    theme.textPrimary = ct.textPrimary;
                    theme.textSecondary = ct.textSecondary;
                    theme.cornerRadius = ct.cornerRadius;
                    theme.bgOpacity = ct.bgOpacity;
                    theme.activePreset = -1;
                    if (BigBrrrManager::get()->enabled)
                        BigBrrrManager::get()->setEnabled(true);
                    saveSettings();
                }
                ImGui::SameLine(0, gap);
                if (Widgets::StyledButton("Edit", ImVec2(editW, 32), theme, anim))
                    openCustomThemeEditor(&ct);
                ImGui::PopID();
            }
        }
        if (Widgets::StyledButton("+ Create New Custom Theme", ImVec2(-1, 30), theme, anim, 6.f))
            openCustomThemeEditor(nullptr);
        ImGui::Dummy(ImVec2(0, 10));

        ImGui::Text("Accent Color");
        ImGui::SameLine();
        if (ImGui::ColorEdit4("##acc",
                              (float*)&theme.accentColor,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
            theme.activePreset = -1;
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Text("Background Color");
        ImGui::SameLine();
        if (ImGui::ColorEdit4("##bgc",
                              (float*)&theme.bgColor,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
            theme.activePreset = -1;
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Text("Card Color");
        ImGui::SameLine();
        if (ImGui::ColorEdit4("##cdc",
                              (float*)&theme.cardColor,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
            theme.activePreset = -1;
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::Text("Corner Rounding  (0 = sharp,  16 = fully rounded)");
        ImGui::PopStyleColor();
        Widgets::StyledSliderFloat("##cornerRadius", &theme.cornerRadius, 0.f, 16.f, theme);
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::Text("Background Opacity  (0.5 = translucent,  1.0 = solid)");
        ImGui::PopStyleColor();
        Widgets::StyledSliderFloat("##bgOpacity", &theme.bgOpacity, 0.5f, 1.f, theme);
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::Text("Animation Speed  (2 = slow and smooth,  24 = snappy)");
        ImGui::PopStyleColor();
        Widgets::StyledSliderFloat("##animSpeed", &anim.animSpeed, 2.f, 24.f, theme);
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Text("Open Animation");
        const char* animNames[] = {"Center", "From Left", "From Right", "From Top", "From Bottom"};
        int dir = (int)anim.openDirection;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##animDir", &dir, animNames, 5))
            anim.openDirection = (AnimDirection)dir;
        ImGui::Dummy(ImVec2(0, 4));
        Widgets::ToggleSwitch("Glow Color Cycle", &theme.glowCycleEnabled, theme, anim);
        if (theme.glowCycleEnabled) {
            ImGui::Dummy(ImVec2(0, 4));
            Widgets::StyledSliderFloat("Cycle Speed", &theme.glowCycleRate, 0.02f, 1.f, theme);
        }
        ImGui::Dummy(ImVec2(0, 4));
        Widgets::ToggleSwitch("Ambient Waves", &ambientWavesEnabled, theme, anim);
        ImGui::Dummy(ImVec2(0, 12));
        Widgets::SectionHeader("Bot Settings Presets", theme);
        {
            static char presetNameBuf[64] = {};
            ImGui::SetNextItemWidth(-80.f);
            ImGui::InputText("##presetName", presetNameBuf, sizeof(presetNameBuf));
            ImGui::SameLine(0, 6);
            if (Widgets::StyledButton("Save", ImVec2(70, 26), theme, anim) && presetNameBuf[0]) {
                eng->saveBotSettingsPreset(presetNameBuf);
            }
            ImGui::Dummy(ImVec2(0, 4));
            for (auto& preset : eng->settingsPresets) {
                float pw = (ImGui::GetContentRegionAvail().x - 10) / 2.f;
                if (Widgets::StyledButton(preset.name.c_str(), ImVec2(pw, 26), theme, anim))
                    eng->loadBotSettingsPreset(preset.name);
                ImGui::SameLine(0, 10);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 0.8f));
                if (ImGui::Button(("X##del_" + preset.name).c_str(), ImVec2(pw, 26)))
                    eng->deleteBotSettingsPreset(preset.name);
                ImGui::PopStyleColor();
            }
            if (eng->settingsPresets.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped("No presets saved yet. Type a name and press Save.");
                ImGui::PopStyleColor();
            }
        }
        ImGui::Dummy(ImVec2(0, 12));
        Widgets::SectionHeader("Advanced", theme);
        Widgets::ModuleCard("FastPlayback",
                            "Start playback without restarting the level",
                            &eng->fastPlayback,
                            theme,
                            anim);
        ImGui::Dummy(ImVec2(0, 12));

        Widgets::SectionHeader("Fun", theme);
        {
            auto* brrr = BigBrrrManager::get();
            bool brrrOn = brrr->enabled;
            if (Widgets::ToggleSwitch("BIG BRRRR", &brrrOn, theme, anim))
                brrr->setEnabled(brrrOn);
            ImGui::SameLine();
            if (Widgets::StyledButton("Open BRRRR Folder", ImVec2(160, 0), theme, anim))
                brrr->openBrrrFolder();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            if (brrr->hasFile())
                ImGui::TextWrapped("Loops the audio file in your BRRRR folder (overriding the "
                                   "bundled one) and makes "
                                   "the whole menu bounce. Doesn't touch the Jupiter tab.");
            else
                ImGui::TextWrapped(
                    "Loops the bundled BRRRR track and makes the whole menu bounce. "
                    "Doesn't touch the Jupiter tab. Drop your own mp3/wav/ogg in the "
                    "BRRRR folder to swap it without a rebuild.");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 6));
            bool shakeOn = brrr->shakeEnabled;
            if (Widgets::ToggleSwitch("Bass Shake", &shakeOn, theme, anim))
                brrr->shakeEnabled = shakeOn;
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "Reads the track's actual bass energy live and shakes/flickers the menu "
                "on hits -- not a beat guess, a real audio tap on the BRRRR channel. "
                "Deliberately intense by default, not a subtle wobble.");
            ImGui::PopStyleColor();
            if (Widgets::StyledSliderFloat(
                    "Flicker Intensity", &brrr->flickerIntensity, 0.f, 1.f, theme))
                Mod::get()->setSavedValue("bigbrrr_flicker_intensity",
                                          (double)brrr->flickerIntensity);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "How far the menu fades out on hard hits. 0 turns the flicker off "
                "(position shake stays); 1 lets it go fully transparent at peak bass.");
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, 12));

        Widgets::SectionHeader("Keybinds", theme);
        struct {
            const char* label;
            int* ptr;
        } kbs[] = {{"Menu Toggle", &keybinds.menu},
                   {"Frame Advance", &keybinds.frameAdvance},
                   {"Frame Step", &keybinds.frameStep},
                   {"Replay Toggle", &keybinds.replayToggle},
                   {"Noclip", &keybinds.noclip},
                   {"Safe Mode", &keybinds.safeMode},
                   {"Trajectory", &keybinds.trajectory},
                   {"Hitboxes", &keybinds.hitboxes},
                   {"Audio Pitch", &keybinds.audioPitch},
                   {"RNG Lock", &keybinds.rngLock},
                   {"Layout Mode", &keybinds.layoutMode},
                   {"No Mirror", &keybinds.noMirror},
                   {"Autoclicker", &keybinds.autoclicker},
                   {"Intentional Death", &keybinds.intentionalDeath},
                   {"Back Step", &keybinds.backStep},
                   {"Auto-Flip", &keybinds.autoFlip},
                   {"Prevent Death", &keybinds.preventDeath},
                   {"Mirror Inputs", &keybinds.mirrorInputs},
                   {"Compact Mode", &keybinds.compactMode}};
        for (auto& e : kbs) {
            ImGui::Dummy(ImVec2(0, 4));
            Widgets::KeybindButton(e.label, e.ptr, theme, anim);
        }
        ImGui::Dummy(ImVec2(0, 12));
        if (Widgets::StyledButton("Reset to Defaults", ImVec2(-1, 32), theme, anim)) {
            theme.resetDefaults();
            activeTheme = THEME_GUCCI;
            if (BigBrrrManager::get()->enabled)
                BigBrrrManager::get()->setEnabled(true);
            anim.animSpeed = 8.f;
            anim.openDirection = ANIM_CENTER;
            eng->fastPlayback = false;
            ambientWavesEnabled = true;
            saveSettings();
        }
    }

    void MenuInterface::drawMoreHacksTab() {
        auto* engine = GucciEngine::get();
        auto* mod = Mod::get();
        Widgets::GucciQuote("\"They asked how many attempts. I said don't worry about it.\"",
                            "-- GucciBot, attempt counter hidden",
                            theme);
        ImGui::Dummy(ImVec2(0, 4));

        Widgets::SectionHeader("Display", theme);
        if (Widgets::ToggleSwitch("Hide Attempt Counter", &engine->hackHideAttempts, theme, anim))
            mod->setSavedValue("hack_hide_attempts", engine->hackHideAttempts);
        if (Widgets::ToggleSwitch(
                "Hide Percentage (experimental)", &engine->hackHidePercentage, theme, anim))
            mod->setSavedValue("hack_hide_percentage", engine->hackHidePercentage);
        if (Widgets::ToggleSwitch("No Death Flash", &engine->hackNoSpikeFlash, theme, anim))
            mod->setSavedValue("hack_no_flash", engine->hackNoSpikeFlash);

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Respawn", theme);
        if (Widgets::ToggleSwitch("Auto Retry", &engine->hackAutoRetry, theme, anim))
            mod->setSavedValue("hack_auto_retry", engine->hackAutoRetry);
        if (engine->hackAutoRetry) {
            if (Widgets::StyledSliderFloat(
                    "Retry Delay (s)", &engine->hackAutoRetryDelay, 0.f, 2.f, theme))
                mod->setSavedValue("hack_auto_retry_delay", (double)engine->hackAutoRetryDelay);
        }
        if (Widgets::ToggleSwitch("Instant Respawn", &engine->hackRespawnInstant, theme, anim))
            mod->setSavedValue("hack_respawn_instant", engine->hackRespawnInstant);

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Controls", theme);
        if (Widgets::ToggleSwitch("Force Platformer Controls (experimental)",
                                  &engine->hackForcePlatformer,
                                  theme,
                                  anim))
            mod->setSavedValue("hack_force_platformer", engine->hackForcePlatformer);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Hide Attempts, No Death Flash, Auto Retry, and Instant Respawn are wired. Hide "
            "Percentage "
            "and Force Platformer are marked experimental pending verified game bindings.");
        ImGui::PopStyleColor();
    }

    void MenuInterface::drawIndicatorsTab() {
        auto* engine = GucciEngine::get();
        auto* mod = Mod::get();
        Widgets::GucciQuote("\"Green means go. Red means don't.\"", "-- Survival Indicator", theme);
        ImGui::Dummy(ImVec2(0, 4));

        if (Widgets::ToggleSwitch(
                "Enable Survival Indicator", &engine->survivalIndicator, theme, anim))
            mod->setSavedValue("hack_survival_indicator", engine->survivalIndicator);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "A marker on the player that turns green when the next click keeps you "
            "alive for the lookahead window, red otherwise. Runs on its own -- doesn't "
            "need \"Show Trajectory\" enabled.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 8));

        Widgets::SectionHeader("Style", theme);
        const char* styles[] = {"Ring", "Classic", "Converge", "Pulse"};
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##indicatorStyle", &engine->indicatorStyle, styles, 4))
            mod->setSavedValue("hack_indicator_style", engine->indicatorStyle);

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Timing", theme);
        if (Widgets::StyledSliderInt(
                "Lookahead (frames)", &engine->indicatorLookahead, 5, 120, theme))
            mod->setSavedValue("hack_survival_indicator_lookahead", engine->indicatorLookahead);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "How many frames ahead the indicator checks before calling a click safe.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Appearance", theme);
        if (Widgets::StyledSliderFloat("Opacity", &engine->indicatorOpacity, 0.1f, 1.f, theme))
            mod->setSavedValue("hack_indicator_opacity", (double)engine->indicatorOpacity);
        ImGui::Text("Safe Colour");
        ImGui::SameLine();
        {
            float col[3] = {engine->indicatorSafeColorR,
                            engine->indicatorSafeColorG,
                            engine->indicatorSafeColorB};
            if (ImGui::ColorEdit3("##indSafeC",
                                  col,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                engine->indicatorSafeColorR = col[0];
                engine->indicatorSafeColorG = col[1];
                engine->indicatorSafeColorB = col[2];
                mod->setSavedValue("hack_indicator_safe_r", (double)col[0]);
                mod->setSavedValue("hack_indicator_safe_g", (double)col[1]);
                mod->setSavedValue("hack_indicator_safe_b", (double)col[2]);
            }
        }
        ImGui::SameLine();
        ImGui::Text("Danger Colour");
        ImGui::SameLine();
        {
            float col[3] = {engine->indicatorDangerColorR,
                            engine->indicatorDangerColorG,
                            engine->indicatorDangerColorB};
            if (ImGui::ColorEdit3("##indDangerC",
                                  col,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                engine->indicatorDangerColorR = col[0];
                engine->indicatorDangerColorG = col[1];
                engine->indicatorDangerColorB = col[2];
                mod->setSavedValue("hack_indicator_danger_r", (double)col[0]);
                mod->setSavedValue("hack_indicator_danger_g", (double)col[1]);
                mod->setSavedValue("hack_indicator_danger_b", (double)col[2]);
            }
        }
        if (Widgets::ToggleSwitch("Flash On Click", &engine->indicatorFlashEnabled, theme, anim))
            mod->setSavedValue("hack_indicator_flash", engine->indicatorFlashEnabled);

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Sound", theme);
        if (Widgets::ToggleSwitch(
                "Pitch-Shifted Click Cue", &engine->indicatorSoundEnabled, theme, anim))
            mod->setSavedValue("hack_indicator_sound", engine->indicatorSoundEnabled);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped("Pitches your existing click sound higher the tighter the window is. "
                           "Requires Click Sounds enabled (Clicks tab) -- this doesn't add a new "
                           "sound, it reshapes the one you already have.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Calibration", theme);
        auto& calib = CalibrationService::get();
        static int calibModeSel = 0;
        const char* gmNames[GM_Count] = {"Cube", "Ship", "Ball", "UFO", "Wave", "Robot", "Spider"};
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##calibMode", &calibModeSel, gmNames, GM_Count);
        auto& gcal = calib.modes[calibModeSel];

        if (gcal.sampleCount > 0) {
            ImGui::Text("Lead: %.0f ms    Jitter: %.0f ms    (%d samples)",
                        gcal.leadMs,
                        gcal.jitterMs,
                        gcal.sampleCount);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextUnformatted("Not calibrated yet.");
            ImGui::PopStyleColor();
        }

        float cbw = (ImGui::GetContentRegionAvail().x - 10) / 2.f;
        if (calib.active && calib.calibratingMode == calibModeSel) {
            char prog[64];
            snprintf(prog, sizeof(prog), "Cancel (%d/%d)", calib.repsDone, calib.repsTarget);
            if (Widgets::StyledButton(prog, ImVec2(cbw, 28), theme, anim))
                calib.cancel();
        } else if (!calib.active) {
            if (Widgets::StyledButton("Start Calibration", ImVec2(cbw, 28), theme, anim) &&
                PlayLayer::get())
                calib.start(calibModeSel);
        } else {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
            Widgets::StyledButton("Start Calibration", ImVec2(cbw, 28), theme, anim);
            ImGui::PopStyleVar();
        }
        ImGui::SameLine(0, 10);
        if (Widgets::StyledButton("Reset", ImVec2(cbw, 28), theme, anim))
            calib.resetMode(calibModeSel);

        if (calib.active && calib.calibratingMode == calibModeSel) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped("In the level, click steadily along with the cue. %d reps.",
                               calib.repsTarget);
            ImGui::PopStyleColor();
        } else if (!PlayLayer::get()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "Enter a level to run calibration -- it needs real clicks to measure against.");
            ImGui::PopStyleColor();
        }

        if (Widgets::ToggleSwitch(("Show Guide In " + std::string(gmNames[calibModeSel])).c_str(),
                                  &gcal.guideEnabled,
                                  theme,
                                  anim))
            calib.save();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Calibration currently measures and stores your lead/jitter per gamemode. It does not "
            "yet "
            "shift the indicator's timing -- the indicator's flash/sound fire in the same frame as "
            "your real click, so there's nothing to offset against. Told Nigel; revisit if a "
            "scheduled/count-in style cue gets added.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Stats", theme);
        if (Widgets::ToggleSwitch(
                "Accuracy / Streak HUD", &engine->accuracyHudEnabled, theme, anim))
            mod->setSavedValue("hack_accuracy_hud", engine->accuracyHudEnabled);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "On-screen readout of how many of your real clicks matched the indicator's "
            "safe/unsafe call, plus your current and best streak this level. Only "
            "counts clicks while the indicator above is enabled.");
        ImGui::PopStyleColor();
    }

    // Shared display for a ClickIndicatorScore -- used by both the Jupiter
    // and Trainer Click Trainer pages. Scoring itself happens event-driven,
    // off the real handleButton hook (hook_gjbasegamelayer.cpp), not polled
    // here; this function only renders whatever's already been recorded.
    static void drawClickScorePanel(const ClickIndicatorScore& score) {
        if (!score.hasLastReading) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.f));
            ImGui::TextWrapped("Waiting for your first click...");
            ImGui::PopStyleColor();
            return;
        }
        int f = score.lastDeltaFrames;
        const char* verdict = f == 0 ? "on time" : (f < 0 ? "early" : "late");
        ImVec4 lastCol = f == 0 ? ImVec4(0.3f, 0.9f, 0.4f, 1.f)
                                : (std::abs(f) <= 3 ? ImVec4(0.95f, 0.85f, 0.3f, 1.f)
                                                    : ImVec4(0.95f, 0.35f, 0.35f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, lastCol);
        ImGui::Text(
            "Last click: %d frame%s %s", std::abs(f), std::abs(f) == 1 ? "" : "s", verdict);
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.9f, 0.4f, 1.f));
        ImGui::Text("Perfect: %d", score.perfect);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.85f, 0.3f, 1.f));
        ImGui::Text("OK: %d", score.ok);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.f));
        ImGui::Text("Miss: %d", score.miss);
        ImGui::PopStyleColor();
    }

    static void drawJupiterClickBar(ThemeEngine& theme,
                                    AnimationState& anim,
                                    GucciEngine* engine,
                                    float windowSeconds,
                                    bool externalWidgetJustReleased,
                                    float h = 46.f) {
        auto& jup = engine->jupiterMacro;
        if (jup.clickIntervalsSec.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped("No click data yet.");
            ImGui::PopStyleColor();
            return;
        }

        double maxT = 0.0;
        for (auto const& iv : jup.clickIntervalsSec)
            maxT = std::max(maxT, iv.second);
        double loopLen = std::max(maxT, 1.0);

        double realNow = ImGui::GetTime();
        if (!engine->jupiterClickBarPaused) {
            double dt = realNow - engine->jupiterClickBarLastRealTime;
            if (dt > 0.0 && dt < 1.0) {
                double newPos = engine->jupiterClickBarPosSec + dt;
                if (newPos >= loopLen) {
                    engine->jupiterClickBarPosSec = 0.0;
                    if (engine->jupiterClickBarLoop) {
                        engine->jupiterClickBarMyClicks.clear();
                        engine->jupiterClickBarMyReleases.clear();
                    } else {
                        engine->jupiterClickBarPaused = true;
                    }
                } else {
                    engine->jupiterClickBarPosSec = newPos;
                }
            }
        }
        engine->jupiterClickBarLastRealTime = realNow;

        gbju::syncClickBarMusic(true, engine->jupiterClickBarPaused, engine->jupiterClickBarPosSec);

        if (Widgets::StyledButton(
                engine->jupiterClickBarPaused ? "Resume" : "Pause", ImVec2(80, 24), theme, anim)) {
            engine->jupiterClickBarPaused = !engine->jupiterClickBarPaused;
            // Diagnostic added 2026-08-31: Nigel pressed Resume in Video
            // Mode and reported nothing happening -- logging the click
            // itself (not just periodic state) since drawJupiterClickBar is
            // called from two places (the normal tab AND Video Mode's own
            // overlay) and it matters which one actually registered this.
            char line[128];
            snprintf(line,
                    sizeof(line),
                    "[t=%.2f] click bar Resume/Pause clicked, new jupiterClickBarPaused=%d",
                    (double)ImGui::GetTime(),
                    (int)engine->jupiterClickBarPaused);
            logVideoModeDebug(line);
        }
        ImGui::SameLine();
        if (Widgets::StyledButton("Reset", ImVec2(70, 24), theme, anim)) {
            engine->jupiterClickBarPosSec = 0.0;
            engine->jupiterClickBarMyClicks.clear();
            engine->jupiterClickBarMyReleases.clear();
        }
        ImGui::SameLine();
        if (Widgets::ToggleSwitch("Loop", &engine->jupiterClickBarLoop, theme, anim))
            Mod::get()->setSavedValue("jupiter_clickbar_loop", engine->jupiterClickBarLoop);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::Text("%.1fs / %.1fs", engine->jupiterClickBarPosSec, loopLen);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 6));

        ImVec2 pos = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        bool mouseOverBar = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + w, pos.y + h));
        bool blockMark = !mouseOverBar || externalWidgetJustReleased;
        if (!blockMark && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            engine->jupiterClickBarMyClicks.push_back(engine->jupiterClickBarPosSec);
        if (!blockMark && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            engine->jupiterClickBarMyReleases.push_back(engine->jupiterClickBarPosSec);

        const ImU32 barCol = IM_COL32(137, 126, 94, 255);
        const ImU32 white = IM_COL32(255, 255, 255, 255);
        const ImU32 clickCol = theme.getAccentU32(1.f);

        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), barCol, 4.f);

        float centerX = pos.x + w * 0.5f;
        float halfWindow = std::max(windowSeconds, 0.2f) * 0.5f;
        float pxPerSec = (w * 0.5f) / halfWindow;
        double nowSec = engine->jupiterClickBarPosSec;

        for (auto const& iv : jup.clickIntervalsSec) {
            double relStart = iv.first - nowSec, relEnd = iv.second - nowSec;
            if (relEnd < -halfWindow || relStart > halfWindow)
                continue;
            float x0 = centerX + (float)relStart * pxPerSec;
            float x1 = centerX + (float)relEnd * pxPerSec;
            x0 = std::max(x0, pos.x);
            x1 = std::min(x1, pos.x + w);
            if (x1 > x0)
                dl->AddRectFilled(ImVec2(x0, pos.y + 5), ImVec2(x1, pos.y + h - 5), clickCol, 2.f);
        }

        auto drawMyMark = [&](double t, bool isRelease) {
            double rel = t - nowSec;
            if (rel < -halfWindow || rel > halfWindow)
                return;
            float x = centerX + (float)rel * pxPerSec;
            float yMid = pos.y + h * 0.5f;
            if (isRelease)
                dl->AddLine(ImVec2(x, pos.y + 3), ImVec2(x, yMid), white, 2.f);
            else
                dl->AddLine(ImVec2(x, yMid), ImVec2(x, pos.y + h - 3), white, 2.f);
        };
        for (double t : engine->jupiterClickBarMyClicks)
            drawMyMark(t, false);
        for (double t : engine->jupiterClickBarMyReleases)
            drawMyMark(t, true);

        dl->AddLine(ImVec2(centerX, pos.y - 4), ImVec2(centerX, pos.y + h + 4), white, 3.f);

        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##clickBarSkim", ImVec2(w, h));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            engine->jupiterClickBarPaused = true;
            double posSec = engine->jupiterClickBarPosSec - ImGui::GetIO().MouseDelta.x / pxPerSec;
            posSec = std::clamp(posSec, 0.0, loopLen);
            engine->jupiterClickBarPosSec = posSec;
        }
        ImGui::Dummy(ImVec2(0, 4));
    }

    // The "choose a different video" picker (pickJupiterVideoTask and its
    // guard/poll machinery) was removed 2026-08-31 -- Nigel: one click,
    // immediate crash, every time ("for the jmf trainer, ditch the button").
    // Not the same double-click race the earlier picker crash was (that
    // guard was verified still intact and correct before this was pulled),
    // something more fundamentally broken about this specific flow that
    // wasn't chased further since the bundled JMF video already covers the
    // real use case with zero setup. If this ever comes back, start from
    // scratch rather than assuming the old guard-based approach was close --
    // it wasn't the problem here.

    void MenuInterface::drawJupiterClickTrainerPage() {
        auto* engine = GucciEngine::get();
        auto* mod = Mod::get();
        engine->jupiterClickBarPageVisible = true;

        rebindTarget = nullptr;

        if (Widgets::StyledButton("<- Back", ImVec2(90, 28), theme, anim)) {
            jupiterClickBarPageOpen = false;
            gbju::stopClickBarMusic();
            engine->jupiterClickBarMyClicks.clear();
            engine->jupiterClickBarMyReleases.clear();
        }
        ImGui::Dummy(ImVec2(0, 10));

        if (fontHeading)
            ImGui::PushFont(fontHeading);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.getAccent());
        ImGui::TextWrapped("Click Trainer");
        ImGui::PopStyleColor();
        if (fontHeading)
            ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Left edge of a block crossing the white line means click, right edge means release.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 12));

        if (Widgets::ToggleSwitch("Show Click Bar", &engine->jupiterClickBarEnabled, theme, anim))
            mod->setSavedValue("jupiter_clickbar_enabled", engine->jupiterClickBarEnabled);
        if (engine->jupiterClickBarEnabled) {
            if (Widgets::StyledSliderFloat(
                    "Window (sec)", &engine->jupiterClickBarWindow, 0.3f, 4.f, theme))
                mod->setSavedValue("jupiter_clickbar_window",
                                   (double)engine->jupiterClickBarWindow);
            bool sliderJustReleased = ImGui::IsItemDeactivated();
            ImGui::Dummy(ImVec2(0, 14));
            drawJupiterClickBar(
                theme, anim, engine, engine->jupiterClickBarWindow, sliderJustReleased, 90.f);
        }

        ImGui::Dummy(ImVec2(0, 18));
        Widgets::SectionHeader("Video Mode", theme);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "A review overlay, not a live one -- no level needs to be open. Plays the bundled "
            "JMF showcase footage full-screen, riding this exact same click bar clock, with the "
            "bar itself overlaid near the bottom.");
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.9f, 0.4f, 1.f));
        ImGui::TextWrapped("The JMF showcase video ships built in -- works with zero setup.");
        ImGui::PopStyleColor();

        bool videoModeOn = engine->jupiterVideoModeEnabled;
        if (Widgets::ToggleSwitch("Enable Video Mode", &videoModeOn, theme, anim)) {
            engine->jupiterVideoModeEnabled = videoModeOn;
            char line[128];
            snprintf(line,
                    sizeof(line),
                    "[t=%.2f] toggle clicked, new jupiterVideoModeEnabled=%d",
                    (double)ImGui::GetTime(),
                    (int)videoModeOn);
            logVideoModeDebug(line);
        }
        if (Widgets::StyledSliderFloat(
                "Opacity", &engine->jupiterVideoOpacity, 0.f, 1.f, theme))
            mod->setSavedValue("jupiter_video_opacity", (double)engine->jupiterVideoOpacity);
        // Manual "Alignment Offset" debug slider removed 2026-08-31 --
        // Nigel: "remove the delay slider" (its own description literally
        // said "delays the video"), now that the Alignment Tool below
        // covers the same job without trial-and-error nudging. The
        // underlying value (jupiterVideoOffsetSec) is untouched and still
        // drives playback -- it's just no longer directly draggable, only
        // settable via "Snap Offset to This Frame". Shown read-only here
        // so the current value isn't a total mystery between snaps.
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::Text("Current offset: %.2fs", (double)engine->jupiterVideoOffsetSec);
        ImGui::PopStyleColor();

        // Alignment tool, added 2026-08-31 (Nigel: "any easier way to fix
        // up the delay stuff, debug slider is tedious... heres a scroll
        // bar for the video, scroll until right at the first click").
        // Scrubs the video directly to find the first click by eye, then
        // computes Alignment Offset from that instead of trial-and-error.
        ImGui::Dummy(ImVec2(0, 6));
        Widgets::ToggleSwitch("Alignment Tool", &engine->jupiterVideoAlignToolActive, theme, anim);
        if (engine->jupiterVideoAlignToolActive) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            if (!engine->jupiterVideoModeEnabled) {
                ImGui::TextWrapped("Turn on Enable Video Mode too -- this scrubs the video "
                                   "you're actually watching, it needs to be showing.");
            } else if (engine->jupiterMacro.clickIntervalsSec.empty()) {
                ImGui::TextWrapped("No click data loaded, nothing to align to.");
            } else {
                ImGui::TextWrapped("Scrub below until you're right on the FIRST click, then hit "
                                   "the button below it.");
            }
            ImGui::PopStyleColor();
            if (engine->jupiterVideoModeEnabled && !engine->jupiterMacro.clickIntervalsSec.empty()) {
                float maxDur = (float)std::max(0.0, jupiterVideoDecoder.durationSec());
                Widgets::StyledSliderFloat(
                    "##videoAlignScrub", &engine->jupiterVideoAlignScrubSec, 0.f, maxDur, theme);
                if (Widgets::StyledButton(
                        "Snap Offset to This Frame", ImVec2(220, 26), theme, anim)) {
                    double firstClick = engine->jupiterMacro.clickIntervalsSec.front().first;
                    engine->jupiterVideoOffsetSec =
                        engine->jupiterVideoAlignScrubSec - (float)firstClick;
                    mod->setSavedValue("jupiter_video_offset_sec",
                                       (double)engine->jupiterVideoOffsetSec);
                    Notification::create("Offset set", NotificationIcon::Success)->show();
                }
            }
        }

        ImGui::Dummy(ImVec2(0, 18));
        Widgets::SectionHeader("Click Deviation", theme);
        {
            auto* pl = PlayLayer::get();
            if (engine->jupiterMacro.clickIntervalsSec.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped("No click data yet.");
                ImGui::PopStyleColor();
            } else if (!pl || !pl->m_player1 || engine->isPlaying()) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(
                    "Play the level yourself (not bot playback) to compare your clicks "
                    "against the macro's.");
                ImGui::PopStyleColor();
            } else {
                drawClickScorePanel(engine->jupiterClickScore);
            }
        }

        ImGui::Dummy(ImVec2(0, 18));
        Widgets::SectionHeader("Music", theme);
        if (Widgets::ToggleSwitch("Synced Level Music", &engine->jupiterMusicEnabled, theme, anim))
            mod->setSavedValue("jupiter_music_enabled", engine->jupiterMusicEnabled);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Plays while actually on Jupiter My Favourite, seeked to match your current "
            "frame -- frame 0 is song position 0, no offset, and it resyncs itself "
            "after respawns/restarts instead of just playing through once.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 18));
        Widgets::SectionHeader("Ghosts & Scrub Preview", theme);
        if (Widgets::ToggleSwitch("Macro Ghost", &engine->jupiterGhostEnabled, theme, anim))
            mod->setSavedValue("jupiter_ghost_enabled", engine->jupiterGhostEnabled);
        if (Widgets::ToggleSwitch(
                "Your Best-Attempt Ghost", &engine->jupiterBestGhostEnabled, theme, anim))
            mod->setSavedValue("jupiter_bestghost_enabled", engine->jupiterBestGhostEnabled);
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped("Best-attempt ghost is session-only, not saved to disk, and only tracks "
                           "real manual attempts, not bot playback. Both ghosts render in the game "
                           "world, right on the player's actual path.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 10));

        Widgets::ToggleSwitch("Scrub Preview", &engine->jupiterScrubActive, theme, anim);
        if (engine->jupiterScrubActive) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "Ghosts freeze at this position in the level instead of following live "
                "playback, so you can preview any point without touching the actual "
                "player -- not a real teleport (the checkpoint system that would need "
                "is the same fragile one flagged elsewhere in this tab).");
            ImGui::PopStyleColor();
            Widgets::StyledSliderFloat(
                "Scrub Percent", &engine->jupiterScrubPercent, 0.f, 100.f, theme);
        }
    }

    void MenuInterface::drawJupiterTab() {
        if (jupiterClickBarPageOpen) {
            drawJupiterClickTrainerPage();
            return;
        }

        auto* engine = GucciEngine::get();
        auto* mod = Mod::get();
        static char jupiterNotesBuf[1024];
        static bool jupiterNotesInit = false;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild(
            "##jmfConstrain", ImVec2(ImGui::GetContentRegionAvail().x * 0.32f, -1), false);

        if (fontHeading)
            ImGui::PushFont(fontHeading);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.getAccent());
        ImGui::TextWrapped("Nigel's Jupiter My Favourite Trainer");
        ImGui::PopStyleColor();
        if (fontHeading)
            ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 6));

        auto* pl = PlayLayer::get();
        std::string currentLevel = (pl && pl->m_level) ? std::string(pl->m_level->m_levelName)
                                                       : engine->loadedMacroLevelName;
        bool isJupiter = false;
        {
            std::string lower = currentLevel;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            isJupiter = lower.find("jupiter my favourite") != std::string::npos;
        }

        if (isJupiter) {
            Widgets::StatusBadge("ACTIVE", ImVec4(0.30f, 0.88f, 0.92f, 1.f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                currentLevel.empty()
                    ? "No level loaded. Enter (or load a macro for) Jupiter my "
                      "Favourite to activate the trainer."
                    : ("Currently on \"" + currentLevel +
                       "\" -- this tab is scoped to Jupiter my Favourite specifically, "
                       "but everything below still works on whatever's loaded.")
                          .c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, 8));

        Widgets::SectionHeader("Stats", theme);
        ImGui::Text("Attempts this session: %d", engine->jupiterAttemptCount);
        ImGui::Text("Best this session: %.1f%%", engine->jupiterSessionBestPct);
        if (!engine->jupiterDeathPcts.empty()) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::Text("Death heatmap (%zu death%s logged this session)",
                        engine->jupiterDeathPcts.size(),
                        engine->jupiterDeathPcts.size() == 1 ? "" : "s");
            ImGui::PopStyleColor();
            ImVec2 hmPos = ImGui::GetCursorScreenPos();
            float hmW = ImGui::GetContentRegionAvail().x, hmH = 18.f;
            ImDrawList* hmDl = ImGui::GetWindowDrawList();
            hmDl->AddRectFilled(
                hmPos, ImVec2(hmPos.x + hmW, hmPos.y + hmH), IM_COL32(30, 26, 60, 255), 3.f);
            const int bins = 40;
            int counts[bins] = {0};
            int maxCount = 1;
            for (float p : engine->jupiterDeathPcts) {
                int b = std::clamp((int)(p / 100.f * bins), 0, bins - 1);
                counts[b]++;
                maxCount = std::max(maxCount, counts[b]);
            }
            for (int b = 0; b < bins; b++) {
                if (counts[b] == 0)
                    continue;
                float bx0 = hmPos.x + hmW * ((float)b / bins);
                float bx1 = hmPos.x + hmW * ((float)(b + 1) / bins);
                float t = (float)counts[b] / (float)maxCount;
                ImU32 col = theme.getAccentU32(0.35f + 0.65f * t);
                hmDl->AddRectFilled(
                    ImVec2(bx0, hmPos.y + hmH * (1.f - t)), ImVec2(bx1, hmPos.y + hmH), col);
            }
            ImGui::Dummy(ImVec2(hmW, hmH + 4));
        }

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Trainer", theme);
        if (engine->replay.m_pathSamples.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "No path data yet for the loaded macro. Load your converted TCBot macro "
                "and let it play through once (bot or manual) -- ground truth gets "
                "captured automatically and saved when the level completes.");
            ImGui::PopStyleColor();
        } else {
            if (Widgets::ToggleSwitch("Show Path", &engine->showMacroPath, theme, anim))
                mod->setSavedValue("hack_show_macro_path", engine->showMacroPath);
            if (Widgets::ToggleSwitch(
                    "Progressive Reveal", &engine->trainerRevealEnabled, theme, anim))
                mod->setSavedValue("hack_trainer_reveal_enabled", engine->trainerRevealEnabled);
            if (engine->trainerRevealEnabled) {
                if (Widgets::StyledSliderFloat(
                        "Reveal Buffer", &engine->trainerRevealBuffer, 0.f, 300.f, theme))
                    mod->setSavedValue("hack_trainer_reveal_buffer",
                                       (double)engine->trainerRevealBuffer);
                ImGui::Text("Furthest reached: %.0f", engine->replay.m_trainerBestX);
                ImGui::SameLine();
                if (Widgets::StyledButton(
                        "Reset Progress##trainer", ImVec2(140, 24), theme, anim)) {
                    engine->replay.m_trainerBestX = 0.f;
                    engine->replay.saveTrainerProgressNow();
                }
            }
            if (Widgets::StyledSliderFloat(
                    "Marker Size", &engine->macroPathMarkerSize, 3.f, 20.f, theme))
                mod->setSavedValue("hack_macro_path_marker_size",
                                   (double)engine->macroPathMarkerSize);
            if (Widgets::StyledSliderFloat(
                    "Line Opacity", &engine->macroPathLineOpacity, 0.1f, 1.f, theme))
                mod->setSavedValue("hack_macro_path_line_opacity",
                                   (double)engine->macroPathLineOpacity);
        }

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Click Trainer", theme);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped("Click/hold windows scrolling toward a fixed line at constant real-time "
                           "speed. Its own page now -- too cramped squeezed in here.");
        ImGui::PopStyleColor();
        if (Widgets::StyledButton("Open Click Trainer ->", ImVec2(-1, 32), theme, anim)) {
            jupiterClickBarPageOpen = true;
            engine->jupiterClickBarPaused = true;
            engine->jupiterClickBarPosSec = 0.0;
            engine->jupiterClickBarMyClicks.clear();
            engine->jupiterClickBarMyReleases.clear();
        }

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Segments", theme);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Personal landmarks -- name the hard parts so the reveal overlay means something at a "
            "glance. Doesn't jump you there, just labels a position for your own reference.");
        ImGui::PopStyleColor();

        static char segLabelBuf[64] = "";
        ImGui::SetNextItemWidth(-90);
        ImGui::InputTextWithHint("##segLabel", "segment name", segLabelBuf, sizeof(segLabelBuf));
        ImGui::SameLine();
        bool canMark = pl && pl->m_player1 && segLabelBuf[0];
        if (!canMark)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
        bool markClicked = Widgets::StyledButton("Mark Here", ImVec2(84, 0), theme, anim);
        if (!canMark)
            ImGui::PopStyleVar();
        if (markClicked && canMark) {
            float x = pl->m_player1->m_position.x;
            if (!engine->jupiterSegmentsRaw.empty())
                engine->jupiterSegmentsRaw += ";";
            engine->jupiterSegmentsRaw += std::string(segLabelBuf) + "," + std::to_string(x) + ",";
            mod->setSavedValue("jupiter_segments", engine->jupiterSegmentsRaw);
            segLabelBuf[0] = 0;
        }

        if (!engine->jupiterMacro.clickIntervalsSec.empty() &&
            !engine->jupiterMacro.pathSamples.empty()) {
            if (Widgets::StyledButton(
                    "Suggest Segments (from click density)", ImVec2(-1, 26), theme, anim)) {
                auto suggestions =
                    suggestSegmentsFromClickDensity(engine->jupiterMacro.clickIntervalsSec,
                                                    engine->jupiterMacro.pathSamples,
                                                    engine->jupiterMacro.clickBarTps,
                                                    engine->jupiterSegmentsRaw);
                if (!suggestions.empty()) {
                    auto segs = parseJupiterSegments(engine->jupiterSegmentsRaw);
                    for (auto& s : suggestions)
                        segs.push_back(s);
                    engine->jupiterSegmentsRaw = serializeJupiterSegments(segs);
                    mod->setSavedValue("jupiter_segments", engine->jupiterSegmentsRaw);
                }
            }
        }

        {
            static int noteEditIdx = -1;
            static char noteBuf[128] = "";
            auto segs = parseJupiterSegments(engine->jupiterSegmentsRaw);
            int removeIdx = -1;
            bool dirty = false;
            for (int i = 0; i < (int)segs.size(); i++) {
                ImGui::PushID(i);
                ImGui::Text("%s", segs[i].label.c_str());
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::Text("(x=%.0f)", segs[i].x);
                ImGui::PopStyleColor();
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 44);
                if (ImGui::SmallButton(noteEditIdx == i ? "note v" : "note >")) {
                    if (noteEditIdx == i)
                        noteEditIdx = -1;
                    else {
                        noteEditIdx = i;
                        snprintf(noteBuf, sizeof(noteBuf), "%s", segs[i].note.c_str());
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                    removeIdx = i;
                if (noteEditIdx == i) {
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint(
                            "##segNote", "note for this segment", noteBuf, sizeof(noteBuf))) {
                        segs[i].note = noteBuf;
                        dirty = true;
                    }
                } else if (!segs[i].note.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                    ImGui::TextWrapped("  %s", segs[i].note.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::PopID();
            }
            if (removeIdx >= 0) {
                segs.erase(segs.begin() + removeIdx);
                noteEditIdx = -1;
                dirty = true;
            }
            if (dirty) {
                engine->jupiterSegmentsRaw = serializeJupiterSegments(segs);
                mod->setSavedValue("jupiter_segments", engine->jupiterSegmentsRaw);
            }
        }

        ImGui::Dummy(ImVec2(0, 10));
        Widgets::SectionHeader("Segment Looping", theme);
        {
            auto segs = parseJupiterSegments(engine->jupiterSegmentsRaw);
            if (segs.size() < 2) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped("Mark at least two segments to loop between them.");
                ImGui::PopStyleColor();
            } else {
                if (engine->jupiterLoopStartIdx >= (int)segs.size())
                    engine->jupiterLoopStartIdx = -1;
                if (engine->jupiterLoopEndIdx >= (int)segs.size())
                    engine->jupiterLoopEndIdx = -1;
                auto segCombo = [&](const char* id, int* idx) {
                    std::string preview =
                        (*idx >= 0 && *idx < (int)segs.size()) ? segs[*idx].label : "(none)";
                    ImGui::SetNextItemWidth((ImGui::GetContentRegionAvail().x - 8) * 0.5f);
                    if (ImGui::BeginCombo(id, preview.c_str())) {
                        for (int i = 0; i < (int)segs.size(); i++)
                            if (ImGui::Selectable(segs[i].label.c_str(), *idx == i))
                                *idx = i;
                        ImGui::EndCombo();
                    }
                };
                segCombo("##loopStart", &engine->jupiterLoopStartIdx);
                ImGui::SameLine();
                segCombo("##loopEnd", &engine->jupiterLoopEndIdx);
                bool validRange =
                    engine->jupiterLoopStartIdx >= 0 && engine->jupiterLoopEndIdx >= 0 &&
                    segs[engine->jupiterLoopStartIdx].x < segs[engine->jupiterLoopEndIdx].x;
                if (!validRange)
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
                if (Widgets::ToggleSwitch("Auto-Loop", &engine->jupiterLoopEnabled, theme, anim) &&
                    !validRange)
                    engine->jupiterLoopEnabled = false;
                if (!validRange)
                    ImGui::PopStyleVar();
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(
                    !validRange
                        ? "Pick a start and end segment (start must come before end) to arm the "
                          "loop."
                        : "The first time you reach the start segment, a real practice checkpoint "
                          "gets "
                          "placed there (pl->markCheckpoint() -- the same call your own checkpoint "
                          "keybind makes, not a reconstructed one) -- dying anywhere after that "
                          "respawns you there automatically, same as normal practice mode. Only "
                          "places "
                          "one per enable, so it won't pile up checkpoints or touch any you've "
                          "placed "
                          "yourself elsewhere.");
                ImGui::PopStyleColor();

                if (engine->jupiterLoopEnabled && validRange && pl && pl->m_player1) {
                    static bool loopArmed = true;
                    static bool checkpointPlaced = false;
                    static int lastStartIdx = -1;
                    if (lastStartIdx != engine->jupiterLoopStartIdx) {
                        lastStartIdx = engine->jupiterLoopStartIdx;
                        checkpointPlaced = false;
                    }

                    float startX = segs[engine->jupiterLoopStartIdx].x;
                    float endX = segs[engine->jupiterLoopEndIdx].x;
                    float px = pl->m_player1->m_position.x;
                    if (px < startX + 5.f) {
                        loopArmed = true;
                    } else {
                        if (!checkpointPlaced) {
                            checkpointPlaced = true;
                            pl->markCheckpoint();
                            Notification::create("Loop checkpoint placed",
                                                 NotificationIcon::Success)
                                ->show();
                        }
                        if (loopArmed && px >= endX) {
                            loopArmed = false;
                            Notification::create("Loop end reached", NotificationIcon::Success)
                                ->show();
                        }
                    }
                }
            }
        }

        ImGui::Dummy(ImVec2(0, 10));
        Widgets::SectionHeader("Share", theme);
        {
            static char importBuf[512] = "";
            static std::string importErr;
            if (Widgets::StyledButton("Copy Export Code", ImVec2(-1, 26), theme, anim)) {
                ImGui::SetClipboardText(
                    exportSegmentsCode(engine->jupiterSegmentsRaw, engine->jupiterNotes).c_str());
                Notification::create("Copied JMF code to clipboard", NotificationIcon::Success)
                    ->show();
            }
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::SetNextItemWidth(-90);
            ImGui::InputTextWithHint(
                "##importCode", "paste JMF code here", importBuf, sizeof(importBuf));
            ImGui::SameLine();
            if (Widgets::StyledButton("Import", ImVec2(80, 0), theme, anim)) {
                if (importSegmentsCode(
                        importBuf, engine->jupiterSegmentsRaw, engine->jupiterNotes, importErr)) {
                    mod->setSavedValue("jupiter_segments", engine->jupiterSegmentsRaw);
                    mod->setSavedValue("jupiter_notes", engine->jupiterNotes);
                    jupiterNotesInit = false;
                    importBuf[0] = 0;
                    Notification::create("Imported segments + notes", NotificationIcon::Success)
                        ->show();
                } else {
                    Notification::create(importErr.c_str(), NotificationIcon::Error)->show();
                }
            }
        }

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Notes", theme);
        if (!jupiterNotesInit) {
            snprintf(jupiterNotesBuf, sizeof(jupiterNotesBuf), "%s", engine->jupiterNotes.c_str());
            jupiterNotesInit = true;
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextMultiline(
                "##jupiterNotes", jupiterNotesBuf, sizeof(jupiterNotesBuf), ImVec2(-1, 100))) {
            engine->jupiterNotes = jupiterNotesBuf;
            mod->setSavedValue("jupiter_notes", engine->jupiterNotes);
        }

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Rehearsal mode (scrub playback, segment looping) isn't built yet -- that's "
            "the next pass. Click-rhythm cues are live above.");
        ImGui::PopStyleColor();

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    static void drawTrainerClickBar(ThemeEngine& theme,
                                    AnimationState& anim,
                                    GucciEngine* engine,
                                    float windowSeconds,
                                    bool externalWidgetJustReleased,
                                    float h = 46.f) {
        auto& trn = engine->trainerMacro;
        if (trn.clickIntervalsSec.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped("No click data yet.");
            ImGui::PopStyleColor();
            return;
        }

        double maxT = 0.0;
        for (auto const& iv : trn.clickIntervalsSec)
            maxT = std::max(maxT, iv.second);
        double loopLen = std::max(maxT, 1.0);

        double realNow = ImGui::GetTime();
        if (!engine->trainerClickBarPaused) {
            double dt = realNow - engine->trainerClickBarLastRealTime;
            if (dt > 0.0 && dt < 1.0) {
                double newPos = engine->trainerClickBarPosSec + dt;
                if (newPos >= loopLen) {
                    engine->trainerClickBarPosSec = 0.0;
                    if (engine->trainerClickBarLoop) {
                        engine->trainerClickBarMyClicks.clear();
                        engine->trainerClickBarMyReleases.clear();
                    } else {
                        engine->trainerClickBarPaused = true;
                    }
                } else {
                    engine->trainerClickBarPosSec = newPos;
                }
            }
        }
        engine->trainerClickBarLastRealTime = realNow;

        gbtr::syncTrainerClickBarMusic(
            true, engine->trainerClickBarPaused, engine->trainerClickBarPosSec);

        if (Widgets::StyledButton(
                engine->trainerClickBarPaused ? "Resume" : "Pause", ImVec2(80, 24), theme, anim))
            engine->trainerClickBarPaused = !engine->trainerClickBarPaused;
        ImGui::SameLine();
        if (Widgets::StyledButton("Reset", ImVec2(70, 24), theme, anim)) {
            engine->trainerClickBarPosSec = 0.0;
            engine->trainerClickBarMyClicks.clear();
            engine->trainerClickBarMyReleases.clear();
        }
        ImGui::SameLine();
        if (Widgets::ToggleSwitch("Loop", &engine->trainerClickBarLoop, theme, anim))
            Mod::get()->setSavedValue("trainer_clickbar_loop", engine->trainerClickBarLoop);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::Text("%.1fs / %.1fs", engine->trainerClickBarPosSec, loopLen);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 6));

        ImVec2 pos = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        bool mouseOverBar = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + w, pos.y + h));
        bool blockMark = !mouseOverBar || externalWidgetJustReleased;
        if (!blockMark && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            engine->trainerClickBarMyClicks.push_back(engine->trainerClickBarPosSec);
        if (!blockMark && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            engine->trainerClickBarMyReleases.push_back(engine->trainerClickBarPosSec);

        const ImU32 barCol = IM_COL32(137, 126, 94, 255);
        const ImU32 white = IM_COL32(255, 255, 255, 255);
        const ImU32 clickCol = theme.getAccentU32(1.f);

        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), barCol, 4.f);

        float centerX = pos.x + w * 0.5f;
        float halfWindow = std::max(windowSeconds, 0.2f) * 0.5f;
        float pxPerSec = (w * 0.5f) / halfWindow;
        double nowSec = engine->trainerClickBarPosSec;

        for (auto const& iv : trn.clickIntervalsSec) {
            double relStart = iv.first - nowSec, relEnd = iv.second - nowSec;
            if (relEnd < -halfWindow || relStart > halfWindow)
                continue;
            float x0 = centerX + (float)relStart * pxPerSec;
            float x1 = centerX + (float)relEnd * pxPerSec;
            x0 = std::max(x0, pos.x);
            x1 = std::min(x1, pos.x + w);
            if (x1 > x0)
                dl->AddRectFilled(ImVec2(x0, pos.y + 5), ImVec2(x1, pos.y + h - 5), clickCol, 2.f);
        }

        auto drawMyMark = [&](double t, bool isRelease) {
            double rel = t - nowSec;
            if (rel < -halfWindow || rel > halfWindow)
                return;
            float x = centerX + (float)rel * pxPerSec;
            float yMid = pos.y + h * 0.5f;
            if (isRelease)
                dl->AddLine(ImVec2(x, pos.y + 3), ImVec2(x, yMid), white, 2.f);
            else
                dl->AddLine(ImVec2(x, yMid), ImVec2(x, pos.y + h - 3), white, 2.f);
        };
        for (double t : engine->trainerClickBarMyClicks)
            drawMyMark(t, false);
        for (double t : engine->trainerClickBarMyReleases)
            drawMyMark(t, true);

        dl->AddLine(ImVec2(centerX, pos.y - 4), ImVec2(centerX, pos.y + h + 4), white, 3.f);

        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##trainerClickBarSkim", ImVec2(w, h));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            engine->trainerClickBarPaused = true;
            double posSec = engine->trainerClickBarPosSec - ImGui::GetIO().MouseDelta.x / pxPerSec;
            posSec = std::clamp(posSec, 0.0, loopLen);
            engine->trainerClickBarPosSec = posSec;
        }
        ImGui::Dummy(ImVec2(0, 4));
    }

    static geode::Task<bool> importTrainerMusicTask() {
        auto pickResult = co_await geode::utils::file::pick(
            geode::utils::file::PickMode::OpenFile,
            geode::utils::file::FilePickOptions{std::nullopt, {{"Audio Files", {"mp3"}}}});
        if (pickResult.isErr())
            co_return false;
        auto pathOpt = pickResult.unwrap();
        if (!pathOpt.has_value())
            co_return false;

        auto dest = Mod::get()->getSaveDir() / "trainer_music.mp3";
        std::error_code ec;
        std::filesystem::copy_file(
            *pathOpt, dest, std::filesystem::copy_options::overwrite_existing, ec);
        co_return !ec;
    }
    // Must stay a stored static, never an unstored temporary -- see
    // s_fwAssetFilesTask's comment above for why (real crash otherwise).
    // Declared earlier in this file (with s_fwAssetFilesTask/s_fwAssetFolderTask)
    // so anyFwPickerPending() can see it too.
    static void importTrainerMusic() {
        if (anyFwPickerPending())
            return;
        s_trainerMusicTask = importTrainerMusicTask();
    }
    static void pollTrainerMusicImportTask() {
        if (s_trainerMusicTask.isFinished()) {
            auto* ok = s_trainerMusicTask.getFinishedValue();
            auto* gb = GucciEngine::get();
            if (ok && *ok) {
                gb->trainerMusicImported = true;
                Mod::get()->setSavedValue("trainer_music_imported", true);
                Notification::create("Music imported", NotificationIcon::Success)->show();
            } else {
                Notification::create("Import failed or cancelled", NotificationIcon::Warning)
                    ->show();
            }
            s_trainerMusicTask = {};
        }
    }

    void MenuInterface::drawTrainerClickTrainerPage() {
        pollTrainerMusicImportTask();
        auto* engine = GucciEngine::get();
        auto* mod = Mod::get();
        engine->trainerClickBarPageVisible = true;

        rebindTarget = nullptr;

        if (Widgets::StyledButton("<- Back", ImVec2(90, 28), theme, anim)) {
            trainerClickBarPageOpen = false;
            gbtr::stopTrainerClickBarMusic();
            engine->trainerClickBarMyClicks.clear();
            engine->trainerClickBarMyReleases.clear();
        }
        ImGui::Dummy(ImVec2(0, 10));

        if (fontHeading)
            ImGui::PushFont(fontHeading);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.getAccent());
        ImGui::TextWrapped("Click Trainer");
        ImGui::PopStyleColor();
        if (fontHeading)
            ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Left edge of a block crossing the white line means click, right edge means release.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 12));

        if (Widgets::ToggleSwitch("Show Click Bar", &engine->trainerClickBarEnabled, theme, anim))
            mod->setSavedValue("trainer_clickbar_enabled", engine->trainerClickBarEnabled);
        if (engine->trainerClickBarEnabled) {
            if (Widgets::StyledSliderFloat(
                    "Window (sec)", &engine->trainerClickBarWindow, 0.3f, 4.f, theme))
                mod->setSavedValue("trainer_clickbar_window",
                                   (double)engine->trainerClickBarWindow);
            bool sliderJustReleased = ImGui::IsItemDeactivated();
            ImGui::Dummy(ImVec2(0, 14));
            drawTrainerClickBar(
                theme, anim, engine, engine->trainerClickBarWindow, sliderJustReleased, 90.f);
        }

        ImGui::Dummy(ImVec2(0, 18));
        Widgets::SectionHeader("Click Deviation", theme);
        {
            auto* pl = PlayLayer::get();
            if (engine->trainerMacro.clickIntervalsSec.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped("No click data yet.");
                ImGui::PopStyleColor();
            } else if (!pl || !pl->m_player1 || engine->isPlaying()) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(
                    "Play the level yourself (not bot playback) to compare your clicks "
                    "against the macro's.");
                ImGui::PopStyleColor();
            } else {
                drawClickScorePanel(engine->trainerClickScore);
            }
        }

        ImGui::Dummy(ImVec2(0, 18));
        Widgets::SectionHeader("Music", theme);
        if (Widgets::ToggleSwitch("Synced Music", &engine->trainerMusicEnabled, theme, anim))
            mod->setSavedValue("trainer_music_enabled", engine->trainerMusicEnabled);
        if (Widgets::StyledButton(engine->trainerMusicImported ? "Re-Import Music" : "Import Music",
                                  ImVec2(160, 26),
                                  theme,
                                  anim))
            importTrainerMusic();
        if (engine->trainerMusicImported) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextUnformatted("Music imported.");
            ImGui::PopStyleColor();
        }
        if (Widgets::StyledSliderFloat(
                "Offset (sec)", &engine->trainerMusicOffsetSec, -3.f, 3.f, theme))
            mod->setSavedValue("trainer_music_offset_sec", (double)engine->trainerMusicOffsetSec);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped("Plays while on the macro's level (or any level, if it has no recorded "
                           "level name), seeked to match your current frame plus the offset above. "
                           "Positive offset delays the music; negative brings it earlier.");
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 18));
        Widgets::SectionHeader("Ghosts & Scrub Preview", theme);
        if (Widgets::ToggleSwitch("Macro Ghost", &engine->trainerGhostEnabled, theme, anim))
            mod->setSavedValue("trainer_ghost_enabled", engine->trainerGhostEnabled);
        if (Widgets::ToggleSwitch(
                "Your Best-Attempt Ghost", &engine->trainerBestGhostEnabled, theme, anim))
            mod->setSavedValue("trainer_bestghost_enabled", engine->trainerBestGhostEnabled);
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped("Best-attempt ghost is session-only, not saved to disk, and only tracks "
                           "real manual attempts, not bot playback.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 10));

        Widgets::ToggleSwitch("Scrub Preview", &engine->trainerScrubActive, theme, anim);
        if (engine->trainerScrubActive) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "Ghosts freeze at this position instead of following live playback.");
            ImGui::PopStyleColor();
            Widgets::StyledSliderFloat(
                "Scrub Percent", &engine->trainerScrubPercent, 0.f, 100.f, theme);
        }
    }

    void MenuInterface::drawTrainerTab() {
        if (trainerClickBarPageOpen) {
            drawTrainerClickTrainerPage();
            return;
        }

        auto* engine = GucciEngine::get();
        auto* mod = Mod::get();
        static char trainerNotesBuf[1024];
        static bool trainerNotesInit = false;

        if (fontHeading)
            ImGui::PushFont(fontHeading);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.getAccent());
        ImGui::TextWrapped("Trainer");
        ImGui::PopStyleColor();
        if (fontHeading)
            ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Same toolset as the JMF tab -- Click Trainer, Ghosts, Segments, Stats, "
            "Music -- but for any of your own saved macros instead of one fixed level.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 8));

        Widgets::SectionHeader("Macro", theme);
        if (engine->trainerMacro.loaded) {
            ImGui::Text("Loaded: %s", engine->trainerMacroName.c_str());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped("No macro loaded -- pick one below.");
            ImGui::PopStyleColor();
        }
        static char trainerMacroFilter[64] = "";
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##trainerMacroSearch",
                                 "Search macros...",
                                 trainerMacroFilter,
                                 sizeof(trainerMacroFilter));
        auto matchesTrainerFilter = [&](const std::string& nm) -> bool {
            if (trainerMacroFilter[0] == 0)
                return true;
            std::string a = nm, b = trainerMacroFilter;
            std::transform(a.begin(), a.end(), a.begin(), ::tolower);
            std::transform(b.begin(), b.end(), b.begin(), ::tolower);
            return a.find(b) != std::string::npos;
        };
        refreshReplayListIfNeeded(false);
        float trainerListH =
            std::max(80.f, std::min(160.f, (float)engine->storedMacros.size() * 24.f + 16.f));
        ImGui::BeginChild("##TrainerMacroList", ImVec2(-1, trainerListH), true);
        auto trainerMacroCopy = engine->storedMacros;
        if (trainerMacroCopy.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped("No saved macros yet.");
            ImGui::PopStyleColor();
        }
        for (const auto& mn : trainerMacroCopy) {
            if (!matchesTrainerFilter(mn))
                continue;
            bool isSel = (engine->trainerMacroName == mn && engine->trainerMacro.loaded);
            ImGui::PushID(mn.c_str());
            if (ImGui::Selectable(mn.c_str(), isSel)) {
                if (engine->loadTrainerMacro(mn))
                    Notification::create("Loaded macro into Trainer", NotificationIcon::Success)
                        ->show();
                else
                    Notification::create("Couldn't load that macro", NotificationIcon::Error)
                        ->show();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (!engine->incompatibleMacros.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "%zu macro(s) need converting first -- see the Macro tab's Saved Replays list.",
                engine->incompatibleMacros.size());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, 8));

        auto* pl = PlayLayer::get();
        std::string currentLevel = (pl && pl->m_level) ? std::string(pl->m_level->m_levelName) : "";
        bool levelKnown = !engine->trainerMacro.levelName.empty();
        if (!engine->trainerMacro.loaded) {
        } else if (!levelKnown) {
            Widgets::StatusBadge("ACTIVE (level unknown)", ImVec4(0.95f, 0.75f, 0.25f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(
                "This macro has no recorded level name (common for imported/converted macros), so "
                "Stats/Ghost/Music stay active on any level instead of just one.");
            ImGui::PopStyleColor();
        } else if (gbtr::isTrainerLevel(pl)) {
            Widgets::StatusBadge("ACTIVE", ImVec4(0.30f, 0.88f, 0.92f, 1.f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped(currentLevel.empty()
                                   ? ("No level loaded. Enter \"" + engine->trainerMacro.levelName +
                                      "\" to activate Stats/Ghost/Music for this macro.")
                                         .c_str()
                                   : ("Currently on \"" + currentLevel +
                                      "\" -- this macro is for \"" +
                                      engine->trainerMacro.levelName +
                                      "\", but everything below still works on whatever's loaded.")
                                         .c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, 8));

        Widgets::SectionHeader("Stats", theme);
        ImGui::Text("Attempts this session: %d", engine->trainerAttemptCount);
        ImGui::Text("Best this session: %.1f%%", engine->trainerSessionBestPct);
        if (!engine->trainerDeathPcts.empty()) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::Text("Death heatmap (%zu death%s logged this session)",
                        engine->trainerDeathPcts.size(),
                        engine->trainerDeathPcts.size() == 1 ? "" : "s");
            ImGui::PopStyleColor();
            ImVec2 hmPos = ImGui::GetCursorScreenPos();
            float hmW = ImGui::GetContentRegionAvail().x, hmH = 18.f;
            ImDrawList* hmDl = ImGui::GetWindowDrawList();
            hmDl->AddRectFilled(
                hmPos, ImVec2(hmPos.x + hmW, hmPos.y + hmH), IM_COL32(30, 26, 60, 255), 3.f);
            const int bins = 40;
            int counts[bins] = {0};
            int maxCount = 1;
            for (float p : engine->trainerDeathPcts) {
                int b = std::clamp((int)(p / 100.f * bins), 0, bins - 1);
                counts[b]++;
                maxCount = std::max(maxCount, counts[b]);
            }
            for (int b = 0; b < bins; b++) {
                if (counts[b] == 0)
                    continue;
                float bx0 = hmPos.x + hmW * ((float)b / bins);
                float bx1 = hmPos.x + hmW * ((float)(b + 1) / bins);
                float t = (float)counts[b] / (float)maxCount;
                ImU32 col = theme.getAccentU32(0.35f + 0.65f * t);
                hmDl->AddRectFilled(
                    ImVec2(bx0, hmPos.y + hmH * (1.f - t)), ImVec2(bx1, hmPos.y + hmH), col);
            }
            ImGui::Dummy(ImVec2(hmW, hmH + 4));
        }

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Click Trainer", theme);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Click/hold windows scrolling toward a fixed line at constant real-time speed.");
        ImGui::PopStyleColor();
        if (Widgets::StyledButton("Open Click Trainer ->", ImVec2(-1, 32), theme, anim)) {
            trainerClickBarPageOpen = true;
            engine->trainerClickBarPaused = true;
            engine->trainerClickBarPosSec = 0.0;
            engine->trainerClickBarMyClicks.clear();
            engine->trainerClickBarMyReleases.clear();
        }

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Segments", theme);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
        ImGui::TextWrapped(
            "Personal landmarks -- name the hard parts. Doesn't jump you there, just "
            "labels a position for your own reference.");
        ImGui::PopStyleColor();

        static char trainerSegLabelBuf[64] = "";
        ImGui::SetNextItemWidth(-90);
        ImGui::InputTextWithHint(
            "##trainerSegLabel", "segment name", trainerSegLabelBuf, sizeof(trainerSegLabelBuf));
        ImGui::SameLine();
        bool canMarkT = pl && pl->m_player1 && trainerSegLabelBuf[0];
        if (!canMarkT)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
        bool markClickedT = Widgets::StyledButton("Mark Here", ImVec2(84, 0), theme, anim);
        if (!canMarkT)
            ImGui::PopStyleVar();
        if (markClickedT && canMarkT) {
            float x = pl->m_player1->m_position.x;
            if (!engine->trainerSegmentsRaw.empty())
                engine->trainerSegmentsRaw += ";";
            engine->trainerSegmentsRaw +=
                std::string(trainerSegLabelBuf) + "," + std::to_string(x) + ",";
            mod->setSavedValue("trainer_segments", engine->trainerSegmentsRaw);
            trainerSegLabelBuf[0] = 0;
        }

        if (!engine->trainerMacro.clickIntervalsSec.empty() &&
            !engine->trainerMacro.pathSamples.empty()) {
            if (Widgets::StyledButton(
                    "Suggest Segments (from click density)", ImVec2(-1, 26), theme, anim)) {
                auto suggestions =
                    suggestSegmentsFromClickDensity(engine->trainerMacro.clickIntervalsSec,
                                                    engine->trainerMacro.pathSamples,
                                                    engine->trainerMacro.clickBarTps,
                                                    engine->trainerSegmentsRaw);
                if (!suggestions.empty()) {
                    auto segs = parseJupiterSegments(engine->trainerSegmentsRaw);
                    for (auto& s : suggestions)
                        segs.push_back(s);
                    engine->trainerSegmentsRaw = serializeJupiterSegments(segs);
                    mod->setSavedValue("trainer_segments", engine->trainerSegmentsRaw);
                }
            }
        }

        {
            static int noteEditIdxT = -1;
            static char noteBufT[128] = "";
            auto segs = parseJupiterSegments(engine->trainerSegmentsRaw);
            int removeIdx = -1;
            bool dirty = false;
            for (int i = 0; i < (int)segs.size(); i++) {
                ImGui::PushID(i + 5000);
                ImGui::Text("%s", segs[i].label.c_str());
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::Text("(x=%.0f)", segs[i].x);
                ImGui::PopStyleColor();
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 44);
                if (ImGui::SmallButton(noteEditIdxT == i ? "note v" : "note >")) {
                    if (noteEditIdxT == i)
                        noteEditIdxT = -1;
                    else {
                        noteEditIdxT = i;
                        snprintf(noteBufT, sizeof(noteBufT), "%s", segs[i].note.c_str());
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                    removeIdx = i;
                if (noteEditIdxT == i) {
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##trainerSegNote",
                                                 "note for this segment",
                                                 noteBufT,
                                                 sizeof(noteBufT))) {
                        segs[i].note = noteBufT;
                        dirty = true;
                    }
                } else if (!segs[i].note.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                    ImGui::TextWrapped("  %s", segs[i].note.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::PopID();
            }
            if (removeIdx >= 0) {
                segs.erase(segs.begin() + removeIdx);
                noteEditIdxT = -1;
                dirty = true;
            }
            if (dirty) {
                engine->trainerSegmentsRaw = serializeJupiterSegments(segs);
                mod->setSavedValue("trainer_segments", engine->trainerSegmentsRaw);
            }
        }

        ImGui::Dummy(ImVec2(0, 10));
        Widgets::SectionHeader("Segment Looping", theme);
        {
            auto segs = parseJupiterSegments(engine->trainerSegmentsRaw);
            if (segs.size() < 2) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped("Mark at least two segments to loop between them.");
                ImGui::PopStyleColor();
            } else {
                if (engine->trainerLoopStartIdx >= (int)segs.size())
                    engine->trainerLoopStartIdx = -1;
                if (engine->trainerLoopEndIdx >= (int)segs.size())
                    engine->trainerLoopEndIdx = -1;
                auto segCombo = [&](const char* id, int* idx) {
                    std::string preview =
                        (*idx >= 0 && *idx < (int)segs.size()) ? segs[*idx].label : "(none)";
                    ImGui::SetNextItemWidth((ImGui::GetContentRegionAvail().x - 8) * 0.5f);
                    if (ImGui::BeginCombo(id, preview.c_str())) {
                        for (int i = 0; i < (int)segs.size(); i++)
                            if (ImGui::Selectable(segs[i].label.c_str(), *idx == i))
                                *idx = i;
                        ImGui::EndCombo();
                    }
                };
                segCombo("##trainerLoopStart", &engine->trainerLoopStartIdx);
                ImGui::SameLine();
                segCombo("##trainerLoopEnd", &engine->trainerLoopEndIdx);
                bool validRange =
                    engine->trainerLoopStartIdx >= 0 && engine->trainerLoopEndIdx >= 0 &&
                    segs[engine->trainerLoopStartIdx].x < segs[engine->trainerLoopEndIdx].x;
                if (!validRange)
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
                if (Widgets::ToggleSwitch("Auto-Loop", &engine->trainerLoopEnabled, theme, anim) &&
                    !validRange)
                    engine->trainerLoopEnabled = false;
                if (!validRange)
                    ImGui::PopStyleVar();
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(
                    !validRange
                        ? "Pick a start and end segment (start must come before end) to arm the "
                          "loop."
                        : "The first time you reach the start segment, a real practice checkpoint "
                          "gets "
                          "placed there (pl->markCheckpoint() -- the same call your own checkpoint "
                          "keybind makes, not a reconstructed one) -- dying anywhere after that "
                          "respawns you there automatically. Only places one per enable.");
                ImGui::PopStyleColor();

                if (engine->trainerLoopEnabled && validRange && pl && pl->m_player1) {
                    static bool loopArmedT = true;
                    static bool checkpointPlacedT = false;
                    static int lastStartIdxT = -1;
                    if (lastStartIdxT != engine->trainerLoopStartIdx) {
                        lastStartIdxT = engine->trainerLoopStartIdx;
                        checkpointPlacedT = false;
                    }

                    float startX = segs[engine->trainerLoopStartIdx].x;
                    float endX = segs[engine->trainerLoopEndIdx].x;
                    float px = pl->m_player1->m_position.x;
                    if (px < startX + 5.f) {
                        loopArmedT = true;
                    } else {
                        if (!checkpointPlacedT) {
                            checkpointPlacedT = true;
                            pl->markCheckpoint();
                            Notification::create("Loop checkpoint placed",
                                                 NotificationIcon::Success)
                                ->show();
                        }
                        if (loopArmedT && px >= endX) {
                            loopArmedT = false;
                            Notification::create("Loop end reached", NotificationIcon::Success)
                                ->show();
                        }
                    }
                }
            }
        }

        ImGui::Dummy(ImVec2(0, 10));
        Widgets::SectionHeader("Share", theme);
        {
            static char importBufT[512] = "";
            static std::string importErrT;
            if (Widgets::StyledButton("Copy Export Code", ImVec2(-1, 26), theme, anim)) {
                ImGui::SetClipboardText(
                    exportSegmentsCode(engine->trainerSegmentsRaw, engine->trainerNotes).c_str());
                Notification::create("Copied Trainer code to clipboard", NotificationIcon::Success)
                    ->show();
            }
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::SetNextItemWidth(-90);
            ImGui::InputTextWithHint(
                "##trainerImportCode", "paste Trainer code here", importBufT, sizeof(importBufT));
            ImGui::SameLine();
            if (Widgets::StyledButton("Import", ImVec2(80, 0), theme, anim)) {
                if (importSegmentsCode(
                        importBufT, engine->trainerSegmentsRaw, engine->trainerNotes, importErrT)) {
                    mod->setSavedValue("trainer_segments", engine->trainerSegmentsRaw);
                    mod->setSavedValue("trainer_notes", engine->trainerNotes);
                    trainerNotesInit = false;
                    importBufT[0] = 0;
                    Notification::create("Imported segments + notes", NotificationIcon::Success)
                        ->show();
                } else {
                    Notification::create(importErrT.c_str(), NotificationIcon::Error)->show();
                }
            }
        }

        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Notes", theme);
        if (!trainerNotesInit) {
            snprintf(trainerNotesBuf, sizeof(trainerNotesBuf), "%s", engine->trainerNotes.c_str());
            trainerNotesInit = true;
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextMultiline(
                "##trainerNotes", trainerNotesBuf, sizeof(trainerNotesBuf), ImVec2(-1, 100))) {
            engine->trainerNotes = trainerNotesBuf;
            mod->setSavedValue("trainer_notes", engine->trainerNotes);
        }
    }

    void MenuInterface::drawCreditsTab() {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::Dummy(ImVec2(0, 8));
        {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            float avail = ImGui::GetContentRegionAvail().x, heroH = 90.f;
            dl->AddRectFilled(
                pos, ImVec2(pos.x + avail, pos.y + heroH), IM_COL32(14, 10, 2, 255), 8.f);
            dl->AddRect(
                pos, ImVec2(pos.x + avail, pos.y + heroH), theme.getAccentU32(0.55f), 8.f, 0, 1.2f);
            auto diamond = [&](float cx, float cy, float r) {
                dl->AddQuad(ImVec2(cx, cy - r),
                            ImVec2(cx + r, cy),
                            ImVec2(cx, cy + r),
                            ImVec2(cx - r, cy),
                            theme.getAccentU32(0.4f),
                            0.8f);
            };
            diamond(pos.x + 18, pos.y + heroH / 2, 10);
            diamond(pos.x + avail - 18, pos.y + heroH / 2, 10);
            ImFont* bigF = fontTitle ? fontTitle : (fontHeading ? fontHeading : fontBody);
            if (bigF)
                ImGui::PushFont(bigF);
            ImVec2 ns = ImGui::CalcTextSize("Gucci Mane Fan");
            dl->AddText(bigF,
                        bigF ? bigF->FontSize : 22.f,
                        ImVec2(pos.x + (avail - ns.x) / 2, pos.y + 8),
                        theme.getAccentU32(),
                        "Gucci Mane Fan");
            if (bigF)
                ImGui::PopFont();
            if (fontSmall)
                ImGui::PushFont(fontSmall);
            dl->AddText(
                ImVec2(pos.x + (avail - ImGui::CalcTextSize("Concept, Direction & Testing").x) / 2,
                       pos.y + 34),
                theme.getTextSecondaryU32(),
                "Concept, Direction & Testing");
            auto* creditsCustom = getActiveCustomTheme();
            std::string badgeStr =
                creditsCustom ? creditsCustom->creditsBadge
                : (activeTheme == THEME_TOOSII || activeTheme == THEME_TOOSII_SYRACUSE ||
                   activeTheme == THEME_TOOSII_SACSTATE)
                    ? "WR1 | Rapper | Never Covered"
                : (activeTheme == THEME_JA)       ? "High Flyer | Ball Don't Lie | IYKYK"
                : (activeTheme == THEME_GIDDEY)   ? "Australian | NBA | G'day Mate"
                : (activeTheme == THEME_BAM)      ? "83 Pts | Center | BITCH IM KOBE"
                : (activeTheme == THEME_SEXYY)    ? "Skee Yee | STL | Pound Town"
                : (activeTheme == THEME_JUICE)    ? "Beta Tester | Bug Hunter | That's Tuff"
                : (activeTheme == THEME_BUTLER)   ? "Playoff Jimmy | Big Face Coffee | Buckets"
                : (activeTheme == THEME_SAWEETIE) ? "Icy Grl | Tap In | Best Friend"
                : (activeTheme == THEME_MAYBACH)  ? "MMG | Boss | Huh"
                : (activeTheme == THEME_ROMO)     ? "Analyst | Prophet | One Bad Afternoon"
                : (activeTheme == THEME_GRIZZLEY) ? "Detroit | Activated | First Day Out"
                : (activeTheme == THEME_REDKINGDOM)
                    ? "Kansas City | Strange Music | Long Live the Kingdom"
                : (activeTheme == THEME_LEMONADE) ? "State vs. Radric Davis | 2009 | Lemonade"
                : (activeTheme == THEME_BRRR)     ? "St. Brick Intro | Frame Perfect | Ice Cold"
                : (activeTheme == THEME_WAKA)     ? "Brick Squad | Grove St. Party | OW!"
                : (activeTheme == THEME_YOUNGSTA) ? "Heatmakerz | Memphis | Everyday's My Birthday"
                : (activeTheme == THEME_KNOCKERZ) ? "Two Step | South Carolina | Bow Bow Bow"
                    : "Concept | Vision | Brrr";
            const char* badge = badgeStr.c_str();
            ImVec2 bs = ImGui::CalcTextSize(badge);
            float bx = pos.x + (avail - bs.x - 16) / 2, by = pos.y + 52;
            dl->AddRectFilled(
                ImVec2(bx, by), ImVec2(bx + bs.x + 16, by + 18), theme.getAccentU32(0.12f), 9.f);
            dl->AddRect(ImVec2(bx, by),
                        ImVec2(bx + bs.x + 16, by + 18),
                        theme.getAccentU32(0.4f),
                        9.f,
                        0,
                        0.5f);
            dl->AddText(ImVec2(bx + 8, by + 2), theme.getAccentU32(), badge);
            if (fontSmall)
                ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, heroH + 12));
        }
        struct {
            const char* init;
            const char* name;
            const char* role;
        } entries[] = {
            {"N",
             "Nigelx1",
             "Creator & owner of GucciBot -- every idea, every call, his"},
            {"C", "Claude", "Wrote the code. All of it. Not a euphemism."},
            {"J",
             "Juice",
             "Frame-window algorithm design & lead co-tester -- ran the mod into the ground on "
             "purpose finding the bugs nobody else caught"},
            {"A",
             "anticroom",
             "Frame-window accuracy fixes -- GucciBot's first outside pull request (also one of "
             "ToastyReplay's own devs)"},
            {"P", "peony", "Silicate dev -- dropped the source like Gucci drops albums. Brrr."},
            {"T",
             "ToastexGD",
             "Built ToastyReplay -- the project GucciBot actually started as before Silicate. "
             "Still runs the renderer today, exactly as he built it."},
            {"G", "GWDdoS", "Astral -- and the codebase cleanup that got this repo public-ready"},
            {"B",
             "Bogdaner09",
             "Click Indicators inspiration -- github.com/Bogdaner09/mod. Vibecoded by his own "
             "admission, so credit's probably owed elsewhere too."},
            {"G", "Gucci Mane", "He's the truth. Brrr."},
        };
        for (auto& e : entries) {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            float avail = ImGui::GetContentRegionAvail().x, rowH = 46.f;
            dl->AddRectFilled(pos, ImVec2(pos.x + avail, pos.y + rowH), theme.getCardU32(), 7.f);
            dl->AddRect(
                pos, ImVec2(pos.x + avail, pos.y + rowH), theme.getAccentU32(0.1f), 7.f, 0, 0.5f);
            float avR = 16.f, avX = pos.x + 22, avY = pos.y + rowH / 2;
            dl->AddCircleFilled(ImVec2(avX, avY), avR, theme.getAccentU32(0.2f));
            dl->AddCircle(ImVec2(avX, avY), avR, theme.getAccentU32(0.4f), 0, 0.8f);
            char ini[2] = {e.init[0], 0};
            ImVec2 is = ImGui::CalcTextSize(ini);
            dl->AddText(ImVec2(avX - is.x / 2, avY - is.y / 2), theme.getAccentU32(), ini);
            if (fontBody)
                ImGui::PushFont(fontBody);
            dl->AddText(ImVec2(pos.x + 46, pos.y + 8), theme.getTextU32(), e.name);
            if (fontBody)
                ImGui::PopFont();
            if (fontSmall)
                ImGui::PushFont(fontSmall);
            dl->AddText(ImVec2(pos.x + 46, pos.y + 26), theme.getTextSecondaryU32(), e.role);
            if (fontSmall)
                ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, rowH + 5));
        }
        ImGui::Dummy(ImVec2(0, 4));
        if (auto* creditsQuoteCustom = getActiveCustomTheme())
            Widgets::GucciQuote(creditsQuoteCustom->quoteCredits.text.c_str(),
                                creditsQuoteCustom->quoteCredits.attribution.c_str(),
                                theme);
        else if (activeTheme == THEME_TOOSII || activeTheme == THEME_TOOSII_SYRACUSE ||
                 activeTheme == THEME_TOOSII_SACSTATE)
            Widgets::GucciQuote(
                "\"Every click is a catch. I don't drop nothing. Not even frames.\"",
                "-- Toosii, post-game presser",
                theme);
        else if (activeTheme == THEME_JA)
            Widgets::GucciQuote(
                "\"Watch me. That's all I ask. Just watch.\"", "-- Ja Morant", theme);
        else if (activeTheme == THEME_GIDDEY)
            Widgets::GucciQuote("\"I'm just happy to be here. Genuinely. This is a great game.\"",
                                "-- Josh Giddey",
                                theme);
        else if (activeTheme == THEME_BAM)
            Widgets::GucciQuote("\"Every frame is a bucket. 83 of them. BITCH IM KOBE!!!\"",
                                "-- Bam Adebayo",
                                theme);
        else if (activeTheme == THEME_SEXYY)
            Widgets::GucciQuote("\"Every click go stupid. Skee yee.\"", "-- Sexyy Red", theme);
        else if (activeTheme == THEME_JUICE)
            Widgets::GucciQuote(
                "\"I just wanted the frame windows to work. Then I got a whole theme.\"",
                "-- Juice",
                theme);
        else if (activeTheme == THEME_BUTLER)
            Widgets::GucciQuote(
                "\"Every frame's the playoffs to me. Brrr.\"", "-- Jimmy Butler", theme);
        else if (activeTheme == THEME_SAWEETIE)
            Widgets::GucciQuote("\"Every frame's a flex. Stay icy.\"", "-- Saweetie", theme);
        else if (activeTheme == THEME_MAYBACH)
            Widgets::GucciQuote("\"Every input's a deal closed. Huh.\"", "-- Rick Ross", theme);
        else if (activeTheme == THEME_ROMO)
            Widgets::GucciQuote("\"Every frame, I saw coming. Every single one -- well, almost.\"",
                                "-- Tony Romo",
                                theme);
        else if (activeTheme == THEME_GRIZZLEY)
            Widgets::GucciQuote("\"Every frame, I earned it.\"", "-- Tee Grizzley", theme);
        else if (activeTheme == THEME_REDKINGDOM)
            Widgets::GucciQuote("\"Every frame bows to me.\"", "-- Tech N9ne", theme);
        else if (activeTheme == THEME_LEMONADE)
            Widgets::GucciQuote("\"Every frame's sweet when you make it yourself.\"",
                                "-- Gucci Mane",
                                theme);
        else if (activeTheme == THEME_BRRR)
            Widgets::GucciQuote("\"Every frame's ice cold. Brrr.\"", "-- Gucci Mane", theme);
        else if (activeTheme == THEME_WAKA)
            Widgets::GucciQuote("\"Grove St. party never stopped. Frame perfect either.\"",
                                "-- Gucci Mane",
                                theme);
        else if (activeTheme == THEME_YOUNGSTA)
            Widgets::GucciQuote(
                "\"Real recognize real. Real frames recognize real frames.\"",
                "-- Gucci Mane",
                theme);
        else if (activeTheme == THEME_KNOCKERZ)
            Widgets::GucciQuote("\"Two step in, frame perfect out.\"", "-- Gucci Mane", theme);
        else
            Widgets::GucciQuote(
                "\"I'm the foundation of all of this. Brrr.\"", "-- Gucci Mane", theme);
    }
    void MenuInterface::drawHudTab() {
        auto* engine = GucciEngine::get();
        Widgets::GucciQuote("\"Stats don't lie. Show me the numbers.\"", "-- GucciBot v4.0", theme);
        ImGui::Dummy(ImVec2(0, 4));
        Widgets::ToggleSwitch("Enable HUD", &engine->hud.enabled, theme, anim);
        if (!engine->hud.enabled) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
            ImGui::TextWrapped("Enable to show live stats in-game.");
            ImGui::PopStyleColor();
            return;
        }
        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Displayed Stats", theme);
        Widgets::ToggleSwitch("Tick / Frame", &engine->hud.showFrame, theme, anim);
        Widgets::ToggleSwitch("TPS", &engine->hud.showTPS, theme, anim);
        Widgets::ToggleSwitch("Player X", &engine->hud.showX, theme, anim);
        Widgets::ToggleSwitch("Player Y", &engine->hud.showY, theme, anim);
        Widgets::ToggleSwitch("X Velocity", &engine->hud.showXVel, theme, anim);
        Widgets::ToggleSwitch("Y Velocity", &engine->hud.showYVel, theme, anim);
        Widgets::ToggleSwitch("Rotation", &engine->hud.showRot, theme, anim);
        Widgets::ToggleSwitch("Bot State", &engine->hud.showState, theme, anim);
        ImGui::Dummy(ImVec2(0, 8));
        Widgets::SectionHeader("Appearance", theme);
        const char* anchors[] = {"Top Left", "Top Right", "Bottom Left", "Bottom Right"};
        ImGui::Text("Position");
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##hudAnchor", &engine->hud.anchor, anchors, 4);
        Widgets::ToggleSwitch("Big Font", &engine->hud.bigFont, theme, anim);
        Widgets::StyledSliderFloat("Scale", &engine->hud.scale, 0.3f, 2.0f, theme);
        Widgets::StyledSliderFloat("Opacity", &engine->hud.opacity, 0.1f, 1.0f, theme);
    }

    static void saveColor(const char* pfx, const ImVec4& c) {
        auto* m = Mod::get();
        m->setSavedValue(std::string(pfx) + "_r", c.x);
        m->setSavedValue(std::string(pfx) + "_g", c.y);
        m->setSavedValue(std::string(pfx) + "_b", c.z);
        m->setSavedValue(std::string(pfx) + "_a", c.w);
    }
    static ImVec4 loadColor(const char* pfx, const ImVec4& def) {
        auto* m = Mod::get();
        return ImVec4(m->getSavedValue<float>(std::string(pfx) + "_r", def.x),
                      m->getSavedValue<float>(std::string(pfx) + "_g", def.y),
                      m->getSavedValue<float>(std::string(pfx) + "_b", def.z),
                      m->getSavedValue<float>(std::string(pfx) + "_a", def.w));
    }

    void MenuInterface::saveSettings() {
        auto* mod = Mod::get();
        auto* eng = GucciEngine::get();
        saveColor("theme_accent", theme.accentColor);
        saveColor("theme_bg", theme.bgColor);
        saveColor("theme_card", theme.cardColor);
        saveColor("theme_text", theme.textPrimary);
        saveColor("theme_text2", theme.textSecondary);
        mod->setSavedValue("theme_bg_opacity", theme.bgOpacity);
        mod->setSavedValue("theme_corner_radius", theme.cornerRadius);
        mod->setSavedValue("theme_active_preset", theme.activePreset);
        mod->setSavedValue("theme_glow_cycle", theme.glowCycleEnabled);
        mod->setSavedValue("theme_glow_rate", theme.glowCycleRate);
        mod->setSavedValue("ambient_waves", ambientWavesEnabled);
        mod->setSavedValue("anim_speed", anim.animSpeed);
        mod->setSavedValue("anim_direction", (int)anim.openDirection);
        mod->setSavedValue("active_theme", (int)activeTheme);
        mod->setSavedValue("active_custom_theme", activeCustomThemeName);
        mod->setSavedValue("active_theme_preset", theme.activePreset);
        mod->setSavedValue("key_menu", keybinds.menu);
        mod->setSavedValue("key_frame_advance", keybinds.frameAdvance);
        mod->setSavedValue("key_frame_step", keybinds.frameStep);
        mod->setSavedValue("key_replay_toggle", keybinds.replayToggle);
        mod->setSavedValue("key_noclip", keybinds.noclip);
        mod->setSavedValue("key_safe_mode", keybinds.safeMode);
        mod->setSavedValue("key_trajectory", keybinds.trajectory);
        mod->setSavedValue("key_audio_pitch", keybinds.audioPitch);
        mod->setSavedValue("key_rng_lock", keybinds.rngLock);
        mod->setSavedValue("key_hitboxes", keybinds.hitboxes);
        mod->setSavedValue("key_layout_mode", keybinds.layoutMode);
        mod->setSavedValue("key_no_mirror", keybinds.noMirror);
        mod->setSavedValue("key_autoclicker", keybinds.autoclicker);
        mod->setSavedValue("key_intentional_death", keybinds.intentionalDeath);
        mod->setSavedValue("key_back_step", keybinds.backStep);
        mod->setSavedValue("key_auto_flip", keybinds.autoFlip);
        mod->setSavedValue("key_prevent_death", keybinds.preventDeath);
        mod->setSavedValue("key_mirror_inputs", keybinds.mirrorInputs);
        mod->setSavedValue("key_compact_mode", keybinds.compactMode);
        mod->setSavedValue("hack_hitboxes", eng->showHitboxes);
        mod->setSavedValue("hack_hitbox_death", eng->hitboxOnDeath);
        mod->setSavedValue("hack_hitbox_trail", eng->hitboxTrail);
        mod->setSavedValue("hack_hitbox_trail_len", eng->hitboxTrailLength);
        mod->setSavedValue("hack_trajectory", eng->pathPreview);
        mod->setSavedValue("hack_trajectory_len", eng->pathLength);
        mod->setSavedValue("hack_survival_indicator", eng->survivalIndicator);
        mod->setSavedValue("hack_survival_indicator_lookahead", eng->indicatorLookahead);
        mod->setSavedValue("hack_indicator_style", eng->indicatorStyle);
        mod->setSavedValue("hack_indicator_opacity", (double)eng->indicatorOpacity);
        mod->setSavedValue("hack_indicator_safe_r", (double)eng->indicatorSafeColorR);
        mod->setSavedValue("hack_indicator_safe_g", (double)eng->indicatorSafeColorG);
        mod->setSavedValue("hack_indicator_safe_b", (double)eng->indicatorSafeColorB);
        mod->setSavedValue("hack_indicator_danger_r", (double)eng->indicatorDangerColorR);
        mod->setSavedValue("hack_indicator_danger_g", (double)eng->indicatorDangerColorG);
        mod->setSavedValue("hack_indicator_danger_b", (double)eng->indicatorDangerColorB);
        mod->setSavedValue("hack_indicator_flash", eng->indicatorFlashEnabled);
        mod->setSavedValue("hack_indicator_sound", eng->indicatorSoundEnabled);
        mod->setSavedValue("hack_accuracy_hud", eng->accuracyHudEnabled);
        mod->setSavedValue("hack_show_macro_path", eng->showMacroPath);
        mod->setSavedValue("hack_macro_path_marker_size", (double)eng->macroPathMarkerSize);
        mod->setSavedValue("hack_macro_path_line_opacity", (double)eng->macroPathLineOpacity);
        mod->setSavedValue("hack_trainer_reveal_enabled", eng->trainerRevealEnabled);
        mod->setSavedValue("hack_trainer_reveal_buffer", (double)eng->trainerRevealBuffer);
        mod->setSavedValue("jupiter_notes", eng->jupiterNotes);
        mod->setSavedValue("jupiter_segments", eng->jupiterSegmentsRaw);
        mod->setSavedValue("jupiter_clickbar_enabled", eng->jupiterClickBarEnabled);
        mod->setSavedValue("jupiter_clickbar_window", (double)eng->jupiterClickBarWindow);
        mod->setSavedValue("jupiter_clickbar_loop", eng->jupiterClickBarLoop);
        mod->setSavedValue("jupiter_video_offset_sec", (double)eng->jupiterVideoOffsetSec);
        mod->setSavedValue("jupiter_video_opacity", (double)eng->jupiterVideoOpacity);
        mod->setSavedValue("jupiter_ghost_enabled", eng->jupiterGhostEnabled);
        mod->setSavedValue("jupiter_bestghost_enabled", eng->jupiterBestGhostEnabled);
        mod->setSavedValue("jupiter_music_enabled", eng->jupiterMusicEnabled);
        mod->setSavedValue("jupiter_music_offset_sec", (double)eng->jupiterMusicOffsetSec);
        mod->setSavedValue("trainer_macro_name", eng->trainerMacroName);
        mod->setSavedValue("trainer_notes", eng->trainerNotes);
        mod->setSavedValue("trainer_segments", eng->trainerSegmentsRaw);
        mod->setSavedValue("trainer_clickbar_enabled", eng->trainerClickBarEnabled);
        mod->setSavedValue("trainer_clickbar_window", (double)eng->trainerClickBarWindow);
        mod->setSavedValue("trainer_clickbar_loop", eng->trainerClickBarLoop);
        mod->setSavedValue("trainer_ghost_enabled", eng->trainerGhostEnabled);
        mod->setSavedValue("trainer_bestghost_enabled", eng->trainerBestGhostEnabled);
        mod->setSavedValue("trainer_music_enabled", eng->trainerMusicEnabled);
        mod->setSavedValue("trainer_music_offset_sec", (double)eng->trainerMusicOffsetSec);
        mod->setSavedValue("trainer_music_imported", eng->trainerMusicImported);
        mod->setSavedValue("hack_noclip", eng->noclipEnabled);
        mod->setSavedValue("hack_noclip_flash", eng->noclipDeathFlash);
        mod->setSavedValue("hack_noclip_color_r", eng->noclipDeathColorR);
        mod->setSavedValue("hack_noclip_color_g", eng->noclipDeathColorG);
        mod->setSavedValue("hack_noclip_color_b", eng->noclipDeathColorB);
        mod->setSavedValue("hack_noclipThreshold", (double)eng->noclipThreshold);
        mod->setSavedValue("hack_rng_lock", eng->rngLocked);
        mod->setSavedValue("hack_rng_seed", eng->rngSeedVal);
        mod->setSavedValue("hack_safe_mode", eng->protectedMode);
        mod->setSavedValue("hack_audio_pitch", eng->audioPitchEnabled);
        mod->setSavedValue("hack_no_mirror", eng->noMirrorEffect);
        mod->setSavedValue("hack_layout_mode", eng->layoutMode);
        mod->setSavedValue("hack_no_mirror_rec_only", eng->noMirrorRecordingOnly);
        mod->setSavedValue("feat_backwards_step", eng->updater.m_backwardsStepping);
        mod->setSavedValue("feat_back_step_count", eng->updater.m_maxBackstepFrames);
        mod->setSavedValue("feat_auto_flip", eng->updater.m_autoFlipOnDeath);
        mod->setSavedValue("feat_prevent_death", eng->updater.m_preventDeath);
        mod->setSavedValue("feat_mirror_inputs", eng->replay.m_mirrorInputs);
        mod->setSavedValue("feat_mirror_inverted", eng->replay.m_mirrorInverted);
        mod->setSavedValue("feat_maintain_gravity", eng->replay.m_maintainGravity);
        mod->setSavedValue("feat_autosave_end", eng->autosaveAtLevelEnd);
        mod->setSavedValue("feat_autosave_interval", eng->autosaveAtInterval);
        mod->setSavedValue("feat_autosave_interval_sec", eng->autosaveIntervalSec);
        mod->setSavedValue("feat_replay_backups", eng->replayBackupsEnabled);
        mod->setSavedValue("feat_scroll_speed_fix", eng->updater.m_ssbFix);
        mod->setSavedValue("feat_lock_delta", eng->updater.m_lockDelta);
        mod->setSavedValue("feat_frame_extrapolation", eng->updater.m_extrapolateFrames);
        mod->setSavedValue("hud_enabled", eng->hud.enabled);
        mod->setSavedValue("hud_show_frame", eng->hud.showFrame);
        mod->setSavedValue("hud_show_tps", eng->hud.showTPS);
        mod->setSavedValue("hud_show_x", eng->hud.showX);
        mod->setSavedValue("hud_show_y", eng->hud.showY);
        mod->setSavedValue("hud_show_xvel", eng->hud.showXVel);
        mod->setSavedValue("hud_show_yvel", eng->hud.showYVel);
        mod->setSavedValue("hud_show_rot", eng->hud.showRot);
        mod->setSavedValue("hud_show_state", eng->hud.showState);
        mod->setSavedValue("hud_anchor", eng->hud.anchor);
        mod->setSavedValue("hud_big_font", eng->hud.bigFont);
        mod->setSavedValue("hud_opacity", eng->hud.opacity);
        mod->setSavedValue("hud_scale", eng->hud.scale);

        mod->setSavedValue("hack_auto_retry", eng->hackAutoRetry);
        mod->setSavedValue("render_audio_codec", std::string(renderAudioCodecBuf));
        mod->setSavedValue("render_audio_bitrate", std::string(renderAudioBitrateBuf));
        auto* ac = Autoclicker::get();
        mod->setSavedValue("ac_enabled", ac->enabled);
        mod->setSavedValue("ac_p1_enabled", ac->p1.enabled);
        mod->setSavedValue("ac_p1_hold_ticks", ac->p1.holdTicks);
        mod->setSavedValue("ac_p1_release_ticks", ac->p1.releaseTicks);
        mod->setSavedValue("ac_p1_clicks", ac->p1.clicksPerHold);
        mod->setSavedValue("ac_p2_enabled", ac->p2.enabled);
        mod->setSavedValue("ac_p2_hold_ticks", ac->p2.holdTicks);
        mod->setSavedValue("ac_p2_release_ticks", ac->p2.releaseTicks);
        mod->setSavedValue("ac_p2_clicks", ac->p2.clicksPerHold);
        mod->setSavedValue("ac_only_holding", ac->onlyWhileHolding);

        mod->setSavedValue("eng_tick_rate", (float)eng->updater.m_tps);
        mod->setSavedValue("eng_speed", (float)eng->updater.m_speedhack);
        mod->setSavedValue("render_name", std::string(renderNameBuf));
        mod->setSavedValue("render_width", (int64_t)std::atoi(renderWidthBuf));
        mod->setSavedValue("render_height", (int64_t)std::atoi(renderHeightBuf));
        mod->setSavedValue("render_fps", (int64_t)std::atoi(renderFpsBuf));
        mod->setSavedValue("render_codec", std::string(renderCodecBuf));
        mod->setSavedValue("render_bitrate", std::string(renderBitrateBuf));
        mod->setSavedValue("render_file_extension", std::string(renderExtBuf));
        mod->setSavedValue("render_args", std::string(renderArgsBuf));
        mod->setSavedValue("render_pix_fmt", std::string(renderPixFmtBuf));
        mod->setSavedValue("render_video_args", std::string(renderVideoArgsBuf));
        mod->setSavedValue("render_audio_args", std::string(renderAudioArgsBuf));
        mod->setSavedValue("render_seconds_after", std::string(renderSecondsAfterBuf));
        mod->setSavedValue("render_include_audio", renderIncludeAudio);
        mod->setSavedValue("render_split_audio_tracks", renderSplitAudioTracks);
        mod->setSavedValue("render_include_clicks", renderIncludeClicks);
        mod->setSavedValue("render_sfx_volume", (double)renderSfxVol);
        mod->setSavedValue("render_music_volume", (double)renderMusicVol);
        mod->setSavedValue("render_hide_endscreen", renderHideEndscreen);
        mod->setSavedValue("render_hide_levelcomplete", renderHideLevelComplete);
        auto* csm = ClickSoundManager::get();
        mod->setSavedValue("click_enabled", csm->enabled);
        mod->setSavedValue("click_pack", csm->activePackName);
        mod->setSavedValue("click_hard_vol", (double)csm->p1Pack.hardVolume);
        mod->setSavedValue("click_soft_vol", (double)csm->p1Pack.softVolume);
        mod->setSavedValue("click_release_vol", (double)csm->p1Pack.releaseVolume);
        mod->setSavedValue("click_softness", (double)csm->softness);
        mod->setSavedValue("click_delay_min", (double)csm->clickDelayMin);
        mod->setSavedValue("click_delay_max", (double)csm->clickDelayMax);
        mod->setSavedValue("click_play_during_playback", csm->playDuringPlayback);
        mod->setSavedValue("click_separate_p2", csm->separateP2Clicks);
        mod->setSavedValue("click_bg_noise", csm->backgroundNoiseEnabled);
        mod->setSavedValue("click_bg_noise_vol", (double)csm->backgroundNoiseVolume);
        mod->setSavedValue("window_size_w", windowSize.x);
        mod->setSavedValue("window_size_h", windowSize.y);
        mod->setSavedValue("main_sub_tab", mainSubTab);
    }

    void MenuInterface::loadRenderSettings() {
        auto* mod = Mod::get();
        auto rn = mod->getSavedValue<std::string>("render_name", "");
        auto rw = loadSV<int64_t>(mod, "render_width", 1920);
        auto rh = loadSV<int64_t>(mod, "render_height", 1080);
        auto rf = loadSV<int64_t>(mod, "render_fps", 60);
        auto rc = loadSV<std::string>(mod, "render_codec", "");
        auto rb = loadSV<std::string>(mod, "render_bitrate", "30");
        auto re = loadSV<std::string>(mod, "render_file_extension", ".mp4");
        auto ra = loadSV<std::string>(mod, "render_args", "-pix_fmt yuv420p");
        auto rpf = loadSV<std::string>(mod, "render_pix_fmt", "yuv420p");
        auto rv = loadSV<std::string>(
            mod, "render_video_args", "colorspace=all=bt709:iall=bt470bg:fast=1");
        auto raa = loadSV<std::string>(mod, "render_audio_args", "");
        auto rs = loadSV<std::string>(mod, "render_seconds_after", "3");
        renderIncludeAudio = loadSV<bool>(mod, "render_include_audio", true);
        renderSplitAudioTracks = loadSV<bool>(mod, "render_split_audio_tracks", false);
        renderColorFix = loadSV<bool>(mod, "render_color_fix", true);
        renderIncludeClicks = loadSV<bool>(mod, "render_include_clicks", false);
        renderSfxVol = (float)loadSV<double>(mod, "render_sfx_volume", 1.0);
        renderMusicVol = (float)loadSV<double>(mod, "render_music_volume", 1.0);
        renderHideEndscreen = loadSV<bool>(mod, "render_hide_endscreen", false);
        renderHideLevelComplete = loadSV<bool>(mod, "render_hide_levelcomplete", false);
        snprintf(renderNameBuf, sizeof(renderNameBuf), "%s", rn.c_str());
        snprintf(renderWidthBuf, sizeof(renderWidthBuf), "%lld", rw);
        snprintf(renderHeightBuf, sizeof(renderHeightBuf), "%lld", rh);
        snprintf(renderFpsBuf, sizeof(renderFpsBuf), "%lld", rf);
        snprintf(renderCodecBuf, sizeof(renderCodecBuf), "%s", rc.c_str());
        snprintf(renderBitrateBuf, sizeof(renderBitrateBuf), "%s", rb.c_str());
        snprintf(renderExtBuf, sizeof(renderExtBuf), "%s", re.c_str());
        snprintf(renderArgsBuf, sizeof(renderArgsBuf), "%s", ra.c_str());
        snprintf(renderPixFmtBuf, sizeof(renderPixFmtBuf), "%s", rpf.c_str());
        snprintf(renderVideoArgsBuf, sizeof(renderVideoArgsBuf), "%s", rv.c_str());
        snprintf(renderAudioArgsBuf, sizeof(renderAudioArgsBuf), "%s", raa.c_str());
        snprintf(renderSecondsAfterBuf, sizeof(renderSecondsAfterBuf), "%s", rs.c_str());
        renderBufsInit = true;
        auto rFolder = mod->getSavedValue<std::string>("render_output_folder", "");
        snprintf(outputFolderBuf, sizeof(outputFolderBuf), "%s", rFolder.c_str());
    }

    void MenuInterface::loadSettings() {
        auto* mod = Mod::get();
        auto* eng = GucciEngine::get();
        ImVec4 accDef(0.788f, 0.659f, 0.298f, 1.f), bgDef(0.051f, 0.051f, 0.051f, 0.96f);
        ImVec4 cardDef(0.078f, 0.078f, 0.078f, 1.f), txtDef(0.941f, 0.910f, 0.816f, 1.f);
        ImVec4 txt2Def(0.478f, 0.447f, 0.376f, 1.f);
        theme.accentColor = sanitizeColor(loadColor("theme_accent", accDef), accDef);
        theme.bgColor = sanitizeColor(loadColor("theme_bg", bgDef), bgDef);
        theme.cardColor = sanitizeColor(loadColor("theme_card", cardDef), cardDef);
        theme.textPrimary = sanitizeColor(loadColor("theme_text", txtDef), txtDef);
        theme.textSecondary = sanitizeColor(loadColor("theme_text2", txt2Def), txt2Def);
        theme.bgOpacity =
            sanitizeClamped(mod->getSavedValue<float>("theme_bg_opacity", 0.96f), 0.5f, 1.f, 0.96f);
        theme.cornerRadius =
            sanitizeClamped(mod->getSavedValue<float>("theme_corner_radius", 5.f), 0.f, 16.f, 5.f);
        theme.activePreset = std::clamp(mod->getSavedValue<int>("theme_active_preset", 0),
                                        0,
                                        ThemeEngine::getPresetCount() - 1);
        theme.glowCycleEnabled = mod->getSavedValue<bool>("theme_glow_cycle", false);
        theme.glowCycleRate =
            sanitizeClamped(mod->getSavedValue<float>("theme_glow_rate", 0.5f), 0.02f, 1.f, 0.5f);
        ambientWavesEnabled = mod->getSavedValue<bool>("ambient_waves", true);
        anim.animSpeed =
            sanitizeClamped(mod->getSavedValue<float>("anim_speed", 8.f), 2.f, 24.f, 8.f);
        anim.openDirection = (AnimDirection)mod->getSavedValue<int>("anim_direction", 0);
        {
            loadCustomThemes();
            activeCustomThemeName = mod->getSavedValue<std::string>("active_custom_theme", "");
            int saved = mod->getSavedValue<int>("active_theme", (int)THEME_GUCCI);
            activeTheme = (saved == (int)THEME_CUSTOM)
                              ? THEME_CUSTOM
                              : (BotTheme)std::clamp(saved, 0, ThemeEngine::getPresetCount() - 1);
            if (activeTheme == THEME_CUSTOM && !getActiveCustomTheme()) {
                activeTheme = THEME_GUCCI;
                activeCustomThemeName.clear();
            }
        }
        keybinds.menu = mod->getSavedValue<int>("key_menu", 0xA4);
        keybinds.frameAdvance = mod->getSavedValue<int>("key_frame_advance", 0x56);
        keybinds.frameStep = mod->getSavedValue<int>("key_frame_step", 0x43);
        keybinds.replayToggle = mod->getSavedValue<int>("key_replay_toggle", 0);
        keybinds.noclip = mod->getSavedValue<int>("key_noclip", 0);
        keybinds.safeMode = mod->getSavedValue<int>("key_safe_mode", 0);
        keybinds.trajectory = mod->getSavedValue<int>("key_trajectory", 0);
        keybinds.audioPitch = mod->getSavedValue<int>("key_audio_pitch", 0);
        keybinds.rngLock = mod->getSavedValue<int>("key_rng_lock", 0);
        keybinds.hitboxes = mod->getSavedValue<int>("key_hitboxes", 0);
        keybinds.layoutMode = mod->getSavedValue<int>("key_layout_mode", 0);
        keybinds.noMirror = mod->getSavedValue<int>("key_no_mirror", 0);
        keybinds.autoclicker = mod->getSavedValue<int>("key_autoclicker", 0);
        eng->showHitboxes = mod->getSavedValue<bool>("hack_hitboxes", false);
        eng->hitboxOnDeath = mod->getSavedValue<bool>("hack_hitbox_death", false);
        eng->hitboxTrail = mod->getSavedValue<bool>("hack_hitbox_trail", false);
        eng->hitboxTrailLength = mod->getSavedValue<int>("hack_hitbox_trail_len", 240);
        eng->pathPreview = mod->getSavedValue<bool>("hack_trajectory", false);
        eng->pathLength = mod->getSavedValue<int>("hack_trajectory_len", 312);
        eng->survivalIndicator = mod->getSavedValue<bool>("hack_survival_indicator", false);
        eng->indicatorLookahead = mod->getSavedValue<int>("hack_survival_indicator_lookahead", 20);
        eng->indicatorStyle = mod->getSavedValue<int>("hack_indicator_style", 0);
        eng->indicatorOpacity = mod->getSavedValue<float>("hack_indicator_opacity", 0.9f);
        eng->indicatorSafeColorR = mod->getSavedValue<float>("hack_indicator_safe_r", 0.25f);
        eng->indicatorSafeColorG = mod->getSavedValue<float>("hack_indicator_safe_g", 0.95f);
        eng->indicatorSafeColorB = mod->getSavedValue<float>("hack_indicator_safe_b", 0.35f);
        eng->indicatorDangerColorR = mod->getSavedValue<float>("hack_indicator_danger_r", 0.95f);
        eng->indicatorDangerColorG = mod->getSavedValue<float>("hack_indicator_danger_g", 0.25f);
        eng->indicatorDangerColorB = mod->getSavedValue<float>("hack_indicator_danger_b", 0.25f);
        eng->indicatorFlashEnabled = mod->getSavedValue<bool>("hack_indicator_flash", true);
        eng->indicatorSoundEnabled = mod->getSavedValue<bool>("hack_indicator_sound", false);
        eng->accuracyHudEnabled = mod->getSavedValue<bool>("hack_accuracy_hud", false);
        eng->showMacroPath = mod->getSavedValue<bool>("hack_show_macro_path", false);
        eng->macroPathMarkerSize = mod->getSavedValue<float>("hack_macro_path_marker_size", 8.f);
        eng->macroPathLineOpacity = mod->getSavedValue<float>("hack_macro_path_line_opacity", 0.6f);
        eng->trainerRevealEnabled = mod->getSavedValue<bool>("hack_trainer_reveal_enabled", true);
        eng->trainerRevealBuffer = mod->getSavedValue<float>("hack_trainer_reveal_buffer", 40.f);
        BigBrrrManager::get()->flickerIntensity =
            mod->getSavedValue<float>("bigbrrr_flicker_intensity", 0.5f);
        eng->jupiterNotes = mod->getSavedValue<std::string>("jupiter_notes", "");
        eng->jupiterSegmentsRaw = mod->getSavedValue<std::string>("jupiter_segments", "");
        eng->jupiterClickBarEnabled = mod->getSavedValue<bool>("jupiter_clickbar_enabled", true);
        eng->jupiterClickBarWindow = mod->getSavedValue<float>("jupiter_clickbar_window", 2.f);
        eng->jupiterVideoOffsetSec =
            mod->getSavedValue<float>("jupiter_video_offset_sec", 0.f);
        eng->jupiterVideoOpacity = mod->getSavedValue<float>("jupiter_video_opacity", 0.6f);
        eng->jupiterClickBarLoop = mod->getSavedValue<bool>("jupiter_clickbar_loop", false);
        eng->jupiterGhostEnabled = mod->getSavedValue<bool>("jupiter_ghost_enabled", true);
        eng->jupiterBestGhostEnabled = mod->getSavedValue<bool>("jupiter_bestghost_enabled", false);
        eng->jupiterMusicEnabled = mod->getSavedValue<bool>("jupiter_music_enabled", true);
        eng->jupiterMusicOffsetSec = mod->getSavedValue<float>("jupiter_music_offset_sec", 0.f);
        eng->trainerNotes = mod->getSavedValue<std::string>("trainer_notes", "");
        eng->trainerSegmentsRaw = mod->getSavedValue<std::string>("trainer_segments", "");
        eng->trainerClickBarEnabled = mod->getSavedValue<bool>("trainer_clickbar_enabled", true);
        eng->trainerClickBarWindow = mod->getSavedValue<float>("trainer_clickbar_window", 2.f);
        eng->trainerClickBarLoop = mod->getSavedValue<bool>("trainer_clickbar_loop", false);
        eng->trainerGhostEnabled = mod->getSavedValue<bool>("trainer_ghost_enabled", true);
        eng->trainerBestGhostEnabled = mod->getSavedValue<bool>("trainer_bestghost_enabled", false);
        eng->trainerMusicEnabled = mod->getSavedValue<bool>("trainer_music_enabled", false);
        eng->trainerMusicOffsetSec = mod->getSavedValue<float>("trainer_music_offset_sec", 0.f);
        eng->trainerMusicImported = mod->getSavedValue<bool>("trainer_music_imported", false);
        {
            std::string savedTrainerMacro =
                mod->getSavedValue<std::string>("trainer_macro_name", "");
            if (!savedTrainerMacro.empty())
                eng->loadTrainerMacro(savedTrainerMacro);
        }
        eng->noclipEnabled = mod->getSavedValue<bool>("hack_noclip", false);
        eng->noclipDeathFlash = mod->getSavedValue<bool>("hack_noclip_flash", true);
        eng->noclipDeathColorR = mod->getSavedValue<float>("hack_noclip_color_r", 1.f);
        eng->noclipDeathColorG = mod->getSavedValue<float>("hack_noclip_color_g", 0.f);
        eng->noclipDeathColorB = mod->getSavedValue<float>("hack_noclip_color_b", 0.f);
        eng->noclipThreshold = mod->getSavedValue<float>("hack_noclipThreshold", 0.f);
        eng->rngLocked = mod->getSavedValue<bool>("hack_rng_lock", false);
        eng->rngSeedVal = mod->getSavedValue<int>("hack_rng_seed", 1);
        eng->protectedMode = mod->getSavedValue<bool>("hack_safe_mode", false);
        eng->audioPitchEnabled = mod->getSavedValue<bool>("hack_audio_pitch", true);
        eng->noMirrorEffect = mod->getSavedValue<bool>("hack_no_mirror", false);
        eng->layoutMode = mod->getSavedValue<bool>("hack_layout_mode", false);
        eng->noMirrorRecordingOnly = mod->getSavedValue<bool>("hack_no_mirror_rec_only", false);

        eng->hackAutoRetry = mod->getSavedValue<bool>("hack_auto_retry", false);
        snprintf(renderAudioCodecBuf,
                 sizeof(renderAudioCodecBuf),
                 "%s",
                 mod->getSavedValue<std::string>("render_audio_codec", "aac").c_str());
        snprintf(renderAudioBitrateBuf,
                 sizeof(renderAudioBitrateBuf),
                 "%s",
                 mod->getSavedValue<std::string>("render_audio_bitrate", "192k").c_str());
        auto* ac = Autoclicker::get();
        ac->enabled = mod->getSavedValue<bool>("ac_enabled", false);
        // Migrate from the pre-2026-09-03 shared-settings scheme (one
        // Hold/Release Ticks pair for both players) -- old "ac_player1"/
        // "ac_hold_ticks" etc. become each player's starting point (via the
        // default-value fallback below) instead of silently resetting
        // everyone the first time this runs post-update.
        bool oldPlayer1 = mod->getSavedValue<bool>("ac_player1", true);
        bool oldPlayer2 = mod->getSavedValue<bool>("ac_player2", false);
        int oldHoldTicks = mod->getSavedValue<int>("ac_hold_ticks", 1);
        int oldReleaseTicks = mod->getSavedValue<int>("ac_release_ticks", 1);
        ac->p1.enabled = mod->getSavedValue<bool>("ac_p1_enabled", oldPlayer1);
        ac->p1.holdTicks = mod->getSavedValue<int>("ac_p1_hold_ticks", oldHoldTicks);
        ac->p1.releaseTicks = mod->getSavedValue<int>("ac_p1_release_ticks", oldReleaseTicks);
        ac->p1.clicksPerHold = mod->getSavedValue<int>("ac_p1_clicks", 1);
        ac->p2.enabled = mod->getSavedValue<bool>("ac_p2_enabled", oldPlayer2);
        ac->p2.holdTicks = mod->getSavedValue<int>("ac_p2_hold_ticks", oldHoldTicks);
        ac->p2.releaseTicks = mod->getSavedValue<int>("ac_p2_release_ticks", oldReleaseTicks);
        ac->p2.clicksPerHold = mod->getSavedValue<int>("ac_p2_clicks", 1);
        ac->onlyWhileHolding = mod->getSavedValue<bool>("ac_only_holding", false);

        eng->updater.m_tps = mod->getSavedValue<float>("eng_tick_rate", 240.f);
        eng->updater.m_speedhack = mod->getSavedValue<float>("eng_speed", 1.f);
        tempTickRate = (float)eng->updater.m_tps;
        tempGameSpeed = (float)eng->updater.m_speedhack;
        compactTempTickRate = tempTickRate;
        compactTempGameSpeed = tempGameSpeed;
        auto* csm = ClickSoundManager::get();
        csm->enabled = mod->getSavedValue<bool>("click_enabled", false);
        csm->activePackName = mod->getSavedValue<std::string>("click_pack", "");
        csm->p1Pack.hardVolume = (float)mod->getSavedValue<double>("click_hard_vol", 1.0);
        csm->p1Pack.softVolume = (float)mod->getSavedValue<double>("click_soft_vol", 0.5);
        csm->p1Pack.releaseVolume = (float)mod->getSavedValue<double>("click_release_vol", 0.8);
        csm->softness = (float)mod->getSavedValue<double>("click_softness", 0.5);
        csm->clickDelayMin = (float)mod->getSavedValue<double>("click_delay_min", 0.0);
        csm->clickDelayMax = (float)mod->getSavedValue<double>("click_delay_max", 0.0);
        csm->playDuringPlayback = mod->getSavedValue<bool>("click_play_during_playback", true);
        csm->separateP2Clicks = mod->getSavedValue<bool>("click_separate_p2", false);
        csm->backgroundNoiseEnabled = mod->getSavedValue<bool>("click_bg_noise", false);
        csm->backgroundNoiseVolume = (float)mod->getSavedValue<double>("click_bg_noise_vol", 0.5);
        windowSize.x = mod->getSavedValue<float>("window_size_w", 580.f);
        windowSize.y = mod->getSavedValue<float>("window_size_h", 540.f);
        mainSubTab = mod->getSavedValue<int>("main_sub_tab", 0);
        if (mainSubTab < 0 || mainSubTab > 1)
            mainSubTab = 0;
        keybinds.intentionalDeath = mod->getSavedValue<int>("key_intentional_death", 0);
        keybinds.backStep = mod->getSavedValue<int>("key_back_step", 0);
        keybinds.autoFlip = mod->getSavedValue<int>("key_auto_flip", 0);
        keybinds.preventDeath = mod->getSavedValue<int>("key_prevent_death", 0);
        keybinds.mirrorInputs = mod->getSavedValue<int>("key_mirror_inputs", 0);
        keybinds.compactMode = mod->getSavedValue<int>("key_compact_mode", 0);
        eng->updater.m_backwardsStepping = mod->getSavedValue<bool>("feat_backwards_step", false);
        eng->fwSweepRange = mod->getSavedValue<int>("fw_sweeprange", 12);
        if (eng->fwMaxWindow > 2 * eng->fwSweepRange)
            eng->fwMaxWindow = 2 * eng->fwSweepRange;
        eng->fwSlackWindow = mod->getSavedValue<int>("fw_slackwindow", 3);
        eng->fwPositionCheckEnabled = mod->getSavedValue<bool>("fw_position_check", false);
        eng->fwPositionSlack = mod->getSavedValue<float>("fw_position_slack", 50.f);
        eng->fwFullRangeSweep = mod->getSavedValue<bool>("fw_full_range_sweep", false);
        eng->fwMaxFramesMeasured = mod->getSavedValue<int>("fw_maxframes", 240);
        eng->fwSimSpeed = mod->getSavedValue<int>("fw_simspeed", 1);
        eng->fwTestShipReleases = mod->getSavedValue<bool>("fw_test_ship_releases", true);
        eng->fwOrbAwareReleaseSkip = mod->getSavedValue<bool>("fw_orb_aware_release_skip", true);
        eng->fwLegendEnabled = mod->getSavedValue<bool>("fw_legend", false);
        eng->fwLegendScale = mod->getSavedValue<float>("fw_legend_scale", 1.f);
        eng->fwDefaultLook = mod->getSavedValue<bool>("fw_default_look", false);
        eng->fwDefaultLookBells = mod->getSavedValue<bool>("fw_default_look_bells", false);
        eng->fwRingBoldness = mod->getSavedValue<float>("fw_ring_boldness", 2.2f);
        eng->fwCircleSkinEnabled = mod->getSavedValue<bool>("fw_circle_skin", false);
        eng->fwCircleSkinDotRadius = mod->getSavedValue<float>("fw_circleskin_dot_radius", 5.f);
        eng->fwCircleSkinRadiusPerFrame =
            mod->getSavedValue<float>("fw_circleskin_radius_per_frame", 2.2f);
        eng->fwCircleSkinMaxRadius = mod->getSavedValue<float>("fw_circleskin_max_radius", 60.f);
        eng->fwOverlayShowAlignmentIndependent =
            mod->getSavedValue<bool>("fw_overlay_show_ai", false);
        eng->fwUseRecoveryRangeAlgorithm = mod->getSavedValue<bool>("fw_use_recovery_range", false);
        eng->fwRecoveryRange = mod->getSavedValue<int>("fw_recovery_range", 4);
        eng->fwUseAlignmentIndependent = mod->getSavedValue<bool>("fw_use_align_indep", false);
        eng->fwAiZ = mod->getSavedValue<int>("fw_ai_z", 3);
        eng->fwAiContinuationDepth = mod->getSavedValue<int>("fw_ai_cont_depth", 1);
        eng->fwAiClusterRatio = mod->getSavedValue<float>("fw_ai_cluster_ratio", 1.15f);
        eng->fwAiDominantThreshold = mod->getSavedValue<float>("fw_ai_dominant_threshold", 0.5f);
        eng->fwDebugMode = mod->getSavedValue<bool>("fw_debug_mode", false);
        eng->fwDebugSlowdown = mod->getSavedValue<int>("fw_debug_slowdown", 30);
        eng->fwDelayMarkerCapture = mod->getSavedValue<bool>("fw_delay_marker_capture", false);
        eng->updater.m_logFrameIncrements =
            mod->getSavedValue<bool>("diag_log_frame_increments", false);
        eng->updater.m_maxBackstepFrames = mod->getSavedValue<int>("feat_back_step_count", 120);
        eng->updater.m_autoFlipOnDeath = mod->getSavedValue<bool>("feat_auto_flip", false);
        eng->updater.m_preventDeath = mod->getSavedValue<bool>("feat_prevent_death", false);
        eng->replay.m_mirrorInputs = mod->getSavedValue<bool>("feat_mirror_inputs", false);
        eng->replay.m_mirrorInverted = mod->getSavedValue<bool>("feat_mirror_inverted", false);
        eng->replay.m_maintainGravity = mod->getSavedValue<bool>("feat_maintain_gravity", false);
        eng->autosaveAtLevelEnd = mod->getSavedValue<bool>("feat_autosave_end", true);
        eng->autosaveAtInterval = mod->getSavedValue<bool>("feat_autosave_interval", false);
        eng->autosaveIntervalSec = mod->getSavedValue<double>("feat_autosave_interval_sec", 180.0);
        eng->replayBackupsEnabled = mod->getSavedValue<bool>("feat_replay_backups", true);
        eng->updater.m_ssbFix = mod->getSavedValue<bool>("feat_scroll_speed_fix", false);
        eng->updater.m_lockDelta = mod->getSavedValue<bool>("feat_lock_delta", true);
        eng->updater.m_extrapolateFrames =
            mod->getSavedValue<bool>("feat_frame_extrapolation", false);
        eng->hud.enabled = mod->getSavedValue<bool>("hud_enabled", false);
        eng->hud.showFrame = mod->getSavedValue<bool>("hud_show_frame", true);
        eng->hud.showTPS = mod->getSavedValue<bool>("hud_show_tps", false);
        eng->hud.showX = mod->getSavedValue<bool>("hud_show_x", false);
        eng->hud.showY = mod->getSavedValue<bool>("hud_show_y", false);
        eng->hud.showXVel = mod->getSavedValue<bool>("hud_show_xvel", false);
        eng->hud.showYVel = mod->getSavedValue<bool>("hud_show_yvel", false);
        eng->hud.showRot = mod->getSavedValue<bool>("hud_show_rot", false);
        eng->hud.showState = mod->getSavedValue<bool>("hud_show_state", false);
        eng->hud.anchor = mod->getSavedValue<int>("hud_anchor", 0);
        eng->hud.bigFont = mod->getSavedValue<bool>("hud_big_font", false);
        eng->hud.opacity = mod->getSavedValue<float>("hud_opacity", 1.f);
        eng->hud.scale = mod->getSavedValue<float>("hud_scale", 0.7f);
        megaHackLook = mod->getSavedValue<bool>("ui_megahack_look", false);
        compactMode = mod->getSavedValue<bool>("ui_compact_mode", false);
        {
            std::string enc = mod->getSavedValue<std::string>("fw_tiers", "");
            eng->fwTiers.clear();
            size_t pos = 0;
            while (pos < enc.size()) {
                size_t semi = enc.find(';', pos);
                if (semi == std::string::npos)
                    break;
                std::string row = enc.substr(pos, semi - pos);
                pos = semi + 1;
                std::vector<std::string> f;
                size_t fp = 0;
                while (fp <= row.size()) {
                    size_t bar = row.find('|', fp);
                    if (bar == std::string::npos) {
                        f.push_back(row.substr(fp));
                        break;
                    }
                    f.push_back(row.substr(fp, bar - fp));
                    fp = bar + 1;
                }
                if (f.size() >= 7) {
                    GucciEngine::FrameWindowTier t;
                    t.lo = atoi(f[0].c_str());
                    t.hi = atoi(f[1].c_str());
                    snprintf(t.imageFile, sizeof(t.imageFile), "%s", f[2].c_str());
                    snprintf(t.soundFile, sizeof(t.soundFile), "%s", f[3].c_str());
                    t.r = (float)atof(f[4].c_str());
                    t.g = (float)atof(f[5].c_str());
                    t.b = (float)atof(f[6].c_str());
                    if (f.size() >= 28) {
                        t.shape = (GucciEngine::FwMarkerShape)atoi(f[7].c_str());
                        t.polygonSides = atoi(f[8].c_str());
                        t.polygonCornerRadius = (float)atof(f[9].c_str());
                        t.fillStyle = (GucciEngine::FwFillStyle)atoi(f[10].c_str());
                        t.noBorder = (f[11] == "1");
                        t.strokeSize = (float)atof(f[12].c_str());
                        t.volume = (float)atof(f[13].c_str());
                        t.markerPulseEnabled = (f[14] == "1");
                        t.markerPulseColor[0] = (float)atof(f[15].c_str());
                        t.markerPulseColor[1] = (float)atof(f[16].c_str());
                        t.markerPulseColor[2] = (float)atof(f[17].c_str());
                        t.markerPulseFadeIn = (float)atof(f[18].c_str());
                        t.markerPulseHold = (float)atof(f[19].c_str());
                        t.markerPulseFadeOut = (float)atof(f[20].c_str());
                        t.textPulseEnabled = (f[21] == "1");
                        t.textPulseColor[0] = (float)atof(f[22].c_str());
                        t.textPulseColor[1] = (float)atof(f[23].c_str());
                        t.textPulseColor[2] = (float)atof(f[24].c_str());
                        t.textPulseFadeIn = (float)atof(f[25].c_str());
                        t.textPulseHold = (float)atof(f[26].c_str());
                        t.textPulseFadeOut = (float)atof(f[27].c_str());
                        if (f.size() >= 29)
                            t.sizeScale = (float)atof(f[28].c_str());
                        if (f.size() >= 30)
                            snprintf(t.legendGroup, sizeof(t.legendGroup), "%s", f[29].c_str());
                    }
                    eng->fwTiers.push_back(t);
                }
            }
        }
        theme.applyToImGuiStyle();
    }

    void MenuInterface::drawInterface() {
        auto* engine = GucciEngine::get();
        if (!setupComplete)
            return;
        engine->jupiterClickBarPageVisible = false;
        engine->trainerClickBarPageVisible = false;
        anim.update(ImGui::GetIO().DeltaTime);
        if (!anim.closing && !anim.opening && anim.openProgress <= 0.f && shown) {
            shown = false;
            previouslyShown = false;
        }
        if (shown && !previouslyShown && engine) {
            engine->reloadMacroList();
            previouslyShown = true;
        }
        if (shown && !anim.closing)
            PlatformToolbox::showCursor();
        theme.applyToImGuiStyle();
        drawBackdrop();
        // Nigel, 2026-08-31: Video Mode working but running at ~5fps, asked
        // to "shut down the rest of the menu when thats running." Real,
        // worthwhile win, not just tidiness: the active tab's entire worth
        // of sliders/buttons/sections/its own click bar copy was still
        // being fully laid out and drawn by ImGui every single frame even
        // though Video Mode's own opaque overlay completely covers all of
        // it -- 100% wasted CPU/draw-call work for something nobody can
        // see. jupiterMacro/click-bar state itself doesn't depend on this
        // running -- the click bar's own tick logic lives inside
        // drawJupiterClickBar, and Video Mode's overlay calls that directly
        // on its own, so skipping the main window doesn't stop playback.
        //
        // 2026-08-31 follow-up: originally also skipped drawBackdrop() and
        // the two popup draws here, on the same "nobody can see it"
        // reasoning. Nigel's next screenshot showed a broken screen with
        // NEITHER the click bar NOR the exit button visible -- both drawn
        // unconditionally by drawJupiterVideoOverlay() below regardless of
        // this gate, so their total absence points at something more
        // fundamental than a texture bug, possibly tied to skipping every
        // single ImGui window for the frame (drawBackdrop used to always be
        // the first Begin() call every frame; this may have been the first
        // real frame where drawJupiterVideoOverlay() was the very first
        // ImGui call of the frame at all). Root cause not confirmed -- so
        // rather than guess further, pulled back to the safer, proven-
        // correct half of this change: keep skipping only the actual
        // expensive part (the full tab window, by far the bulk of the
        // wasted work), restore the cheap backdrop + popup calls that were
        // always running before and never implicated in the regression.
        if (!engine->jupiterVideoModeEnabled) {
            if (anim.openProgress > 0.f) {
                if (compactMode)
                    drawCompactWindow();
                else if (megaHackLook)
                    drawMegaHackWindow();
                else
                    drawMainWindow();
            }
        }
        drawRenderCompletePopup();
        drawCustomThemeEditorPopup();
        drawJupiterVideoOverlay();
    }

    void MenuInterface::drawRenderCompletePopup() {
        auto* engine = GucciEngine::get();
        auto& lr = engine->renderer.lastRender;
        if (lr.pending) {
            lr.pending = false;
            ImGui::OpenPopup("RenderComplete");
        }
        ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Appearing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
        if (ImGui::BeginPopupModal("RenderComplete",
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoResize)) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.getAccent());
            if (fontHeading)
                ImGui::PushFont(fontHeading);
            ImGui::TextUnformatted(lr.success ? "Render Complete" : "Render Failed");
            if (fontHeading)
                ImGui::PopFont();
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 8));
            if (lr.success) {
                std::string fname = std::filesystem::path(lr.path).filename().string();
                double mb = (double)lr.fileSize / (1024.0 * 1024.0);
                ImGui::Text("File: %s", fname.c_str());
                ImGui::Text("Resolution: %ux%u @ %ufps", lr.width, lr.height, lr.fps);
                int ds = (int)lr.duration;
                ImGui::Text("Duration: %d:%02d", ds / 60, ds % 60);
                if (mb >= 0.01)
                    ImGui::Text("Size: %.2f MB", mb);
                else
                    ImGui::Text("Size: %llu bytes", (unsigned long long)lr.fileSize);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
                ImGui::TextWrapped(
                    "The render did not finish successfully. Check the log for details.");
                ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, 12));
            float bw = (ImGui::GetContentRegionAvail().x - 8) / 2.f;
            if (Widgets::StyledButton("Open Folder", ImVec2(bw, 30), theme, anim, 6.f)) {
                std::error_code ec;
                auto folder = std::filesystem::path(lr.path).parent_path();
                if (std::filesystem::exists(folder, ec))
                    utils::file::openFolder(folder);
            }
            ImGui::SameLine(0, 8);
            if (Widgets::StyledButton("Close", ImVec2(bw, 30), theme, anim, 6.f))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    void MenuInterface::initialize() {
        auto& io = ImGui::GetIO();
        io.FontGlobalScale = theme.textScale;
        auto* mod = Mod::get();
        auto fontPath = mod->getResourcesDir() / "Roboto-Regular.ttf";
        auto boldPath = mod->getResourcesDir() / "Roboto-Bold.ttf";
        if (std::filesystem::exists(fontPath)) {
            fontBody = io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), 14.f);
            fontSmall = io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), 11.f);
            if (std::filesystem::exists(boldPath)) {
                fontHeading = io.Fonts->AddFontFromFileTTF(boldPath.string().c_str(), 16.f);
                fontTitle = io.Fonts->AddFontFromFileTTF(boldPath.string().c_str(), 28.f);
            }
        }
        loadSettings();
        theme.applyToImGuiStyle();
        setupComplete = true;
    }

    void displayOverlayBranding() {
        auto* ui = MenuInterface::get();
        auto* engine = GucciEngine::get();
        if (!ui || !ui->setupComplete)
            return;
        if (!ui->shown)
            return;
        auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 10, vp->Pos.y + vp->Size.y - 10),
                                ImGuiCond_Always,
                                ImVec2(1, 1));
        ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.f);
        ImGui::Begin("##wm",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (ui->fontSmall)
            ImGui::PushFont(ui->fontSmall);
        ImVec4 a = ui->theme.getAccent();
        const char* brand = (ui->activeTheme == THEME_TOOSII) ? "ToosiiBot v" MOD_VERSION "  Open!"
                                                              : "GucciBot v" MOD_VERSION "  Brrr.";
        ImGui::TextColored(ImVec4(a.x, a.y, a.z, 0.55f), "%s", brand);
        if (ui->fontSmall)
            ImGui::PopFont();
        ImGui::End();
    }

    void displayRenderHUD() {
        auto* ui = MenuInterface::get();
        auto* engine = GucciEngine::get();
        if (!ui || !ui->setupComplete || !engine)
            return;
        auto& r = engine->renderer;
        if (!r.recording)
            return;
        auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(
            ImVec2(vp->Pos.x + vp->Size.x - 10, vp->Pos.y + 10), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("##renderhud",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (ui->fontBody)
            ImGui::PushFont(ui->fontBody);
        float pulse = 0.55f + 0.45f * std::sin((float)ImGui::GetTime() * 4.f);
        ImGui::TextColored(ImVec4(1.f, 0.25f, 0.25f, pulse), "REC");
        ImGui::SameLine();
        int frames = (int)r.renderedFrames.size();
        int secs = (int)r.lastFrame_t;
        ImGui::Text("%d frames  %d:%02d  @%ufps", frames, secs / 60, secs % 60, r.fps);
        if (ui->fontBody)
            ImGui::PopFont();
        ImGui::End();
    }

    void displayCalculatingHUD() {
        auto* ui = MenuInterface::get();
        auto* engine = GucciEngine::get();
        if (!ui || !ui->setupComplete || !engine)
            return;
        // Pathfinder reuses fwAnalyzing as its headless-sim flag -- it has
        // its own HUD below, don't show "Calculating..." over it.
        if (Pathfinder::get()->active)
            return;
        if (!engine->fwAnalyzing)
            return;

        auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(
            ImVec2(vp->Pos.x + vp->Size.x - 10, vp->Pos.y + 10), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.75f);
        ImGui::Begin("##calcHud",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (ui->fontBody)
            ImGui::PushFont(ui->fontBody);
        float pulse = 0.55f + 0.45f * std::sin((float)ImGui::GetTime() * 4.f);
        ImVec4 accent = ui->theme.getAccent();
        ImGui::TextColored(ImVec4(accent.x, accent.y, accent.z, pulse), "Calculating...");
        if (ui->fontBody)
            ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, ui->theme.textSecondary);
        if (engine->fwAnalyzeTotal > 0)
            ImGui::Text("%s  %d/%d  (%.0f%%)",
                        engine->fwAnalyzeStage.c_str(),
                        engine->fwAnalyzeCur,
                        engine->fwAnalyzeTotal,
                        engine->fwAnalyzeProgress * 100.f);
        else
            ImGui::Text(
                "%s  (%.0f%%)", engine->fwAnalyzeStage.c_str(), engine->fwAnalyzeProgress * 100.f);
        ImGui::PopStyleColor();
        if (Widgets::StyledButton("Cancel", ImVec2(-1, 24), ui->theme, ui->anim, 4.f))
            engine->cancelAnalysis();
        ImGui::End();
    }

    // Small corner status readout, shown instead of the full-screen cover
    // when Pathfinder's "hide the search" toggle is off -- Nigel wants to
    // actually watch the level play out sometimes, not just be surprised.
    static void displayPathfinderCornerHUD(MenuInterface* ui, Pathfinder* pf) {
        auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(
            ImVec2(vp->Pos.x + vp->Size.x - 10, vp->Pos.y + 10), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.75f);
        ImGui::Begin("##pathfinderHud",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (ui->fontBody)
            ImGui::PushFont(ui->fontBody);
        float pulse = 0.55f + 0.45f * std::sin((float)ImGui::GetTime() * 4.f);
        ImVec4 accent = ui->theme.getAccent();
        ImGui::TextColored(ImVec4(accent.x, accent.y, accent.z, pulse), "Pathfinding...");
        if (ui->fontBody)
            ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, ui->theme.textSecondary);
        ImGui::Text("best %.1f%%  |  run %d  |  depth %zu", pf->bestPct, pf->runs, pf->depth);
        ImGui::Text("%s", pf->stage.c_str());
        ImGui::PopStyleColor();
        if (Widgets::StyledButton("Cancel", ImVec2(-1, 24), ui->theme, ui->anim, 4.f))
            pf->cancel();
        ImGui::End();
    }

    // Full-screen opaque cover while Pathfinder runs -- Nigel's ask: don't
    // show the search chewing through the level ("surprises are cool"),
    // just "Calculating..." and the best % reached until a macro exists,
    // like camila314's mod does. Drawn BEFORE the menu (see the draw
    // lambda) with NoBringToFrontOnFocus so the menu can still open on
    // top of it. The game keeps running underneath; only the view is hidden.
    // Toggle-able (Pathfinder::hideSearch) -- off falls back to the small
    // corner HUD above instead, so the level is actually visible.
    void displayPathfinderHUD() {
        auto* ui = MenuInterface::get();
        auto* pf = Pathfinder::get();
        if (!ui || !ui->setupComplete || !pf->active)
            return;
        if (!pf->hideSearch) {
            displayPathfinderCornerHUD(ui, pf);
            return;
        }

        auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(vp->Size, ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.02f, 1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::Begin("##pathfinderCover",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImVec4 accent = ui->theme.getAccent();
        float pulse = 0.6f + 0.4f * std::sin((float)ImGui::GetTime() * 3.f);
        float cx = vp->Size.x * 0.5f;
        float cy = vp->Size.y * 0.5f;

        auto centered = [&](const char* text, float y) {
            ImVec2 sz = ImGui::CalcTextSize(text);
            ImGui::SetCursorPos(ImVec2(cx - sz.x * 0.5f, y));
            ImGui::TextUnformatted(text);
        };

        if (ui->fontHeading)
            ImGui::PushFont(ui->fontHeading);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(accent.x, accent.y, accent.z, pulse));
        centered("Calculating...", cy - 70.f);
        ImGui::PopStyleColor();
        char pct[32];
        snprintf(pct, sizeof(pct), "%.1f%%", pf->bestPct);
        ImGui::PushStyleColor(ImGuiCol_Text, ui->theme.textPrimary);
        centered(pct, cy - 28.f);
        ImGui::PopStyleColor();
        if (ui->fontHeading)
            ImGui::PopFont();

        float barW = 320.f;
        ImGui::SetCursorPos(ImVec2(cx - barW * 0.5f, cy + 14.f));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accent);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.f, 1.f, 1.f, 0.06f));
        ImGui::ProgressBar(std::clamp(pf->bestPct / 100.f, 0.f, 1.f), ImVec2(barW, 6.f), "");
        ImGui::PopStyleColor(2);

        if (ui->fontSmall)
            ImGui::PushFont(ui->fontSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, ui->theme.textSecondary);
        char sub[64];
        snprintf(sub, sizeof(sub), "best percentage reached  |  run %d", pf->runs);
        centered(sub, cy + 30.f);
        ImGui::PopStyleColor();
        if (ui->fontSmall)
            ImGui::PopFont();

        float btnW = 160.f;
        ImGui::SetCursorPos(ImVec2(cx - btnW * 0.5f, cy + 64.f));
        if (Widgets::StyledButton("Cancel", ImVec2(btnW, 26), ui->theme, ui->anim, 4.f))
            pf->cancel();

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    void displayFwLegendHUD() {
        auto* ui = MenuInterface::get();
        auto* engine = GucciEngine::get();
        if (!ui || !ui->setupComplete || !engine)
            return;
        // Default Look supplies its own fixed rows, so an empty tier list is
        // fine there -- only the tier-driven legend needs tiers to exist.
        if (!engine->fwLegendEnabled || !engine->fwHasData)
            return;
        if (!engine->fwDefaultLook && engine->fwTiers.empty())
            return;
        if (!PlayLayer::get())
            return;

        uint32_t curFrame = engine->updater.getFrame();

        struct LegendGroup {
            int lo = INT_MAX, hi = INT_MIN;
            int count = 0;
            float r = 1, g = 1, b = 1;
            bool colorSet = false;
        };
        std::unordered_map<std::string, LegendGroup> groups;
        auto groupKey = [&](size_t i) -> std::string {
            auto const& t = engine->fwTiers[i];
            return t.legendGroup[0] ? std::string(t.legendGroup) : ("##solo" + std::to_string(i));
        };
        for (size_t i = 0; i < engine->fwTiers.size(); ++i) {
            auto const& t = engine->fwTiers[i];
            auto& g = groups[groupKey(i)];
            if (!g.colorSet) {
                g.r = t.r;
                g.g = t.g;
                g.b = t.b;
                g.colorSet = true;
            }
            g.lo = std::min(g.lo, t.lo);
            g.hi = std::max(g.hi, t.hi);
        }
        for (auto const& mk : engine->fwMarks) {
            if (mk.frame > curFrame)
                continue;
            for (size_t i = 0; i < engine->fwTiers.size(); ++i) {
                auto const& t = engine->fwTiers[i];
                if (mk.window >= t.lo && mk.window <= t.hi) {
                    groups[groupKey(i)].count++;
                    break;
                }
            }
        }

        std::vector<LegendGroup> sorted;
        if (engine->fwDefaultLook) {
            // Fixed rows + fixed ramp, matching the markers exactly.
            for (auto const& row : GucciEngine::fwDefaultLookRows()) {
                LegendGroup g;
                g.lo = row.lo;
                g.hi = row.hi;
                auto c = GucciEngine::fwDefaultLookColor(row.lo);
                g.r = c.r;
                g.g = c.g;
                g.b = c.b;
                for (auto const& mk : engine->fwMarks) {
                    if (mk.frame > curFrame)
                        continue;
                    if (mk.window >= row.lo && mk.window <= row.hi)
                        g.count++;
                }
                sorted.push_back(g);
            }
        } else {
            sorted.reserve(groups.size());
            for (auto const& kv : groups)
                sorted.push_back(kv.second);
            std::sort(sorted.begin(), sorted.end(), [](auto const& a, auto const& b) {
                return a.hi > b.hi;
            });
        }

        // Drawn as free-floating outlined text straight onto the foreground
        // draw list rather than as a themed ImGui panel. Nigel (2026-09-13):
        // the old one looked "embedded to guccibot"; this matches the
        // reference overlay -- no background, no border, label left and count
        // in its own right-aligned column, black outline so it stays readable
        // over both the bright and dark parts of a level.
        auto* vp = ImGui::GetMainViewport();
        // Background list, not foreground: this draws over the game but still
        // UNDER GucciBot's own windows, which is how the old panel stacked.
        // The foreground list would paint the legend on top of the menu.
        auto* dl = ImGui::GetBackgroundDrawList();
        ImFont* font = ui->fontBody ? ui->fontBody : ImGui::GetFont();
        float scale = std::max(0.1f, engine->fwLegendScale);
        float fontSize = ImGui::GetFontSize() * scale * 1.35f;
        float lineStep = fontSize * 1.18f;
        float originX = vp->Pos.x + 14.f;
        float originY = vp->Pos.y + 12.f;

        // Widest label decides where the count column starts, so the numbers
        // line up in a column instead of ragging off the end of each label.
        float labelW = 0.f;
        std::vector<std::string> labels;
        std::vector<std::string> counts;
        labels.reserve(sorted.size());
        counts.reserve(sorted.size());
        for (auto const& g : sorted) {
            labels.push_back(g.lo == g.hi ? fmt::format("{}:", g.lo)
                                          : fmt::format("{}-{}:", g.lo, g.hi));
            counts.push_back(fmt::format("{}", g.count));
            labelW = std::max(labelW,
                              font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, labels.back().c_str()).x);
        }
        float countX = originX + labelW + fontSize * 0.9f;

        auto outlinedText = [&](ImVec2 pos, ImU32 col, const char* text) {
            const float o = std::max(1.f, fontSize * 0.075f);
            const ImU32 black = IM_COL32(0, 0, 0, 235);
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy) {
                    if (!dx && !dy)
                        continue;
                    dl->AddText(font, fontSize, ImVec2(pos.x + dx * o, pos.y + dy * o), black, text);
                }
            dl->AddText(font, fontSize, pos, col, text);
        };

        for (size_t i = 0; i < sorted.size(); ++i) {
            auto const& g = sorted[i];
            ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(g.r, g.g, g.b, 1.f));
            float y = originY + lineStep * (float)i;
            outlinedText(ImVec2(originX, y), col, labels[i].c_str());
            outlinedText(ImVec2(countX, y), col, counts[i].c_str());
        }
    }

    void displayGameplayHUD() {
        auto* ui = MenuInterface::get();
        auto* engine = GucciEngine::get();
        if (!ui || !ui->setupComplete || !engine)
            return;
        auto& h = engine->hud;
        if (!h.enabled)
            return;
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1)
            return;
        auto* p = pl->m_player1;

        char buf[512];
        buf[0] = '\0';
        int n = 0;
        auto add = [&](const char* fmt, auto val) {
            char line[96];
            snprintf(line, sizeof(line), fmt, val);
            if (n++)
                strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
        };
        if (h.showFrame)
            add("Frame: %u", engine->updater.getFrame());
        if (h.showTPS)
            add("TPS: %.0f", engine->updater.m_tps);
        if (h.showX)
            add("X: %.1f", p->m_position.x);
        if (h.showY)
            add("Y: %.1f", p->m_position.y);
        if (h.showXVel)
            add("X Vel: %.2f", p->getCurrentXVelocity());
        if (h.showYVel)
            add("Y Vel: %.2f", p->m_yVelocity);
        if (h.showRot)
            add("Rot: %.0f", p->getRotation());
        if (h.showState)
            add("On ground: %s", p->m_isOnGround ? "yes" : "no");
        if (n == 0)
            return;

        auto* vp = ImGui::GetMainViewport();
        float px = (h.anchor == 1 || h.anchor == 3) ? vp->Pos.x + vp->Size.x - 10 : vp->Pos.x + 10;
        float py = (h.anchor == 2 || h.anchor == 3) ? vp->Pos.y + vp->Size.y - 10 : vp->Pos.y + 10;
        ImVec2 pivot((h.anchor == 1 || h.anchor == 3) ? 1.f : 0.f,
                     (h.anchor == 2 || h.anchor == 3) ? 1.f : 0.f);
        ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_Always, pivot);
        ImGui::SetNextWindowBgAlpha(0.45f * h.opacity);
        ImGui::Begin("##gbhud",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (h.bigFont && ui->fontHeading)
            ImGui::PushFont(ui->fontHeading);
        else if (ui->fontBody)
            ImGui::PushFont(ui->fontBody);
        ImGui::SetWindowFontScale(h.scale);
        ImGui::TextColored(ImVec4(1, 1, 1, h.opacity), "%s", buf);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
        ImGui::End();
    }

    void displayAccuracyHUD() {
        auto* ui = MenuInterface::get();
        auto* engine = GucciEngine::get();
        if (!ui || !ui->setupComplete || !engine)
            return;
        if (!engine->accuracyHudEnabled)
            return;
        if (!PlayLayer::get())
            return;

        int pct = engine->accuracyTotalClicks > 0
                      ? (int)((float)engine->accuracyGoodClicks /
                                  (float)engine->accuracyTotalClicks * 100.f +
                              0.5f)
                      : 100;

        auto* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(
            ImVec2(vp->Pos.x + 10, vp->Pos.y + vp->Size.y - 10), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##accuracyhud",
                     nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (ui->fontBody)
            ImGui::PushFont(ui->fontBody);
        ImGui::Text("Accuracy: %d%%   Streak: %d (Best: %d)",
                    pct,
                    engine->currentStreak,
                    engine->bestStreak);
        if (ui->fontBody)
            ImGui::PopFont();
        ImGui::End();
    }

    $on_mod(Loaded) {
        ImGuiCocos::get()
            .setup([] {
                MenuInterface::get()->initialize();
            })
            .draw([] {
                auto* ui = MenuInterface::get();
                // Cover goes first so the menu (drawn next) stacks above it.
                displayPathfinderHUD();
                ui->drawInterface();
                displayOverlayBranding();
                displayRenderHUD();
                displayCalculatingHUD();
                displayFwLegendHUD();
                displayGameplayHUD();
                displayAccuracyHUD();
            });
    }

} // namespace gucci
