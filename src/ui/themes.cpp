#include "ui/themes.hpp"

#include <Geode/Geode.hpp>
#include <matjson.hpp>

#include <algorithm>
#include <cctype>

using namespace geode::prelude;

namespace gucci::ui::themes {

    namespace {

        namespace fs = std::filesystem;

        // GucciBot's own drop: what every theme without a track of its own plays.
        constexpr double kStockBpm = 140.0;
        constexpr double kStockDrop = 20.0 + 11.0 / 15.0;
        constexpr const char* kStockTrack = "big_brrr.mp3";

        constexpr size_t kMaxExtension = 16;
        constexpr size_t kMaxName = 32;

        // ------------------------------------------------------------ built-in data
        //
        // Nigel's themes, as the old menu showed them. A Voice is everything a
        // theme says (and the extension and drop that go with it); a Paint is
        // one theme's look. The three ToosiiBot themes share a voice.

        struct Voice {
            const char* title;
            const char* ext;
            const char* subtitle;
            const char* brand;
            const char* badge;
            const char* replay[2];   // quote, attribution
            const char* tools[2];
            const char* credits[2];
            double bpm;
            double drop;  // seconds; written as seconds + frames at 30 fps
            const char* track;
        };

        enum VoiceId {
            vGucci, vToosii, vJa, vGiddey, vBam, vSexyy, vJuice, vButler, vSaweetie, vMaybach,
            vRomo, vGrizzley, vRedKingdom, vLemonade, vBrrr, vWaka, vYoungsta, vKnockerz,
        };

        const Voice kVoices[] = {
            /* vGucci */ {
                .title = "GucciBot", .ext = ".brrr",
                .subtitle = "Frame perfect. GBR6. Brrr.", .brand = "Brrr.", .badge = "Concept | Vision | Brrr",
                .replay = {"\"I got so many replays I got files in my files.\"", "-- Gucci Mane, probably"},
                .tools = {"\"I run this game at my own speed. You can't keep up.\"", "-- Gucci Mane, on speedhacks"},
                .credits = {"\"I'm the foundation of all of this. Brrr.\"", "-- Gucci Mane"},
                .bpm = kStockBpm, .drop = kStockDrop, .track = kStockTrack,
            },
            /* vToosii */ {
                .title = "ToosiiBot", .ext = ".toosii",
                .subtitle = "Running routes. Dropping passes.", .brand = "Open!", .badge = "WR1 | Rapper | Never Covered",
                .replay = {"\"What's cover 1?\"", "-- Toosii, asking the cornerback"},
                .tools = {"\"I run the route so fast the DB thinks I'm a speedhack.\"", "-- Toosii, route running"},
                .credits = {"\"Every click is a catch. I don't drop nothing. Not even frames.\"", "-- Toosii, post-game presser"},
                .bpm = 116.0, .drop = 33.0 + 25.0 / 30.0, .track = "big_brrr_toosii.mp3",
            },
            /* vJa */ {
                .title = "JaBot", .ext = ".ja",
                .subtitle = "They can't stop me. I'm different.", .brand = "IYKYK!", .badge = "High Flyer | Ball Don't Lie | IYKYK",
                .replay = {"\"Nobody can replay what I just did. Nobody.\"", "-- Ja Morant"},
                .tools = {"\"I don't use speedhack. That's just me.\"", "-- Ja Morant"},
                .credits = {"\"Watch me. That's all I ask. Just watch.\"", "-- Ja Morant"},
                .bpm = kStockBpm, .drop = kStockDrop, .track = kStockTrack,
            },
            /* vGiddey */ {
                .title = "GiddeyBot", .ext = ".giddey",
                .subtitle = "G'day. I'm open, apparently.", .brand = "Crikey!", .badge = "Australian | NBA | G'day Mate",
                .replay = {"\"6 was a little high; I was expecting to be in the 7-13 range.\"", "-- Josh Giddey, on being the 6th pick"},
                .tools = {"\"Speed 1.0x seems fast enough. I'm still jetlagged.\"", "-- Josh Giddey"},
                .credits = {"\"I'm just happy to be here. Genuinely. This is a great game.\"", "-- Josh Giddey"},
                .bpm = kStockBpm, .drop = kStockDrop, .track = kStockTrack,
            },
            /* vBam */ {
                .title = "BamBot", .ext = ".bam",
                .subtitle = "BITCH IM KOBEEE!!!", .brand = "83 pts.", .badge = "83 Pts | Center | BITCH IM KOBE",
                .replay = {"\"I don't record inputs. I record history. 83 points of it.\"", "-- Bam, in the zone"},
                .tools = {"\"Speed? I hit 83 at my own pace. You can't guard that.\"", "-- Bam, on speedhack"},
                .credits = {"\"Every frame is a bucket. 83 of them. BITCH IM KOBE!!!\"", "-- Bam Adebayo"},
                .bpm = kStockBpm, .drop = kStockDrop, .track = kStockTrack,
            },
            /* vSexyy */ {
                .title = "SexyyBot", .ext = ".sexyy",
                .subtitle = "Frame perfect. Goes stupid. Skee yee.", .brand = "Skee yee.", .badge = "Skee Yee | STL | Pound Town",
                .replay = {"\"I don't miss. Not a single frame. Skee yee.\"", "-- Sexyy Red, probably"},
                .tools = {"\"I run this at my own speed and it still goes stupid.\"", "-- Sexyy Red"},
                .credits = {"\"Every click go stupid. Skee yee.\"", "-- Sexyy Red"},
                .bpm = 178.0, .drop = 11.0 + 2.0 / 30.0, .track = "big_brrr_sexyy.mp3",
            },
            /* vJuice */ {
                .title = "JuiceBot", .ext = ".juice",
                .subtitle = "That's tuff. Brrr.", .brand = "Tuff.", .badge = "Beta Tester | Bug Hunter | That's Tuff",
                .replay = {"\"I tested every frame. Every single one.\"", "-- Juice, probably"},
                .tools = {"\"Speed doesn't mean much if the frame windows are wrong.\"", "-- Juice, keeping you honest"},
                .credits = {"\"I just wanted the frame windows to work. Then I got a whole theme.\"", "-- Juice"},
                .bpm = kStockBpm, .drop = kStockDrop, .track = kStockTrack,
            },
            /* vButler */ {
                .title = "ButlerBot", .ext = ".butler",
                .subtitle = "Playoff Jimmy mode: always on.", .brand = "Playoff Jimmy.",
                .badge = "Playoff Jimmy | Big Face Coffee | Buckets",
                .replay = {"\"Regular season replays don't count. I lock in for the playoffs.\"", "-- Jimmy Butler, probably"},
                .tools = {"\"I don't need speedhack. I just lock in.\"", "-- Jimmy Butler"},
                .credits = {"\"Every frame's the playoffs to me. Brrr.\"", "-- Jimmy Butler"},
                .bpm = kStockBpm, .drop = kStockDrop, .track = kStockTrack,
            },
            /* vSaweetie */ {
                .title = "SaweetieBot", .ext = ".saweetie",
                .subtitle = "Icy girl. Tap in, don't fall off.", .brand = "Icy!", .badge = "Icy Grl | Tap In | Best Friend",
                .replay = {"\"I don't miss. I'm too expensive to miss.\"", "-- Saweetie, probably"},
                .tools = {"\"Fast money, fast frames. Tap in.\"", "-- Saweetie, on speedhack"},
                .credits = {"\"Every frame's a flex. Stay icy.\"", "-- Saweetie"},
                .bpm = 105.0, .drop = 11.0 + 1.0 / 30.0, .track = "big_brrr_saweetie.mp3",
            },
            /* vMaybach */ {
                .title = "MaybachBot", .ext = ".maybach",
                .subtitle = "Huh. Maybach Music. Frame perfect.", .brand = "MMG!", .badge = "MMG | Boss | Huh",
                .replay = {"\"Every replay a hit. Every frame a boss move.\"", "-- Rick Ross, probably"},
                .tools = {"\"I don't rush. The Maybach arrives exactly on time.\"", "-- Rick Ross, on speedhack"},
                .credits = {"\"Every input's a deal closed. Huh.\"", "-- Rick Ross"},
                .bpm = 75.0, .drop = 0.0, .track = "big_brrr_maybach.mp3",
            },
            /* vRomo */ {
                .title = "RomoBot", .ext = ".romo",
                .subtitle = "Frame perfect. Definitely not over the limit.", .brand = "Called it!",
                .badge = "Analyst | Prophet | One Bad Afternoon",
                .replay = {"\"I called that replay before it even happened.\"", "-- Tony Romo, probably"},
                .tools = {"\"I don't need speedhack. I've had worse rides.\"", "-- Tony Romo, probably"},
                .credits = {"\"Every frame, I saw coming. Every single one -- well, almost.\"", "-- Tony Romo"},
                .bpm = 130.0, .drop = 16.0 + 11.0 / 30.0, .track = "big_brrr_romo.mp3",
            },
            /* vGrizzley */ {
                .title = "GrizzleyBot", .ext = ".grizzley",
                .subtitle = "First day out. Frame perfect.", .brand = "Activated!", .badge = "Detroit | Activated | First Day Out",
                .replay = {"\"First day out, first frame perfect.\"", "-- Tee Grizzley, probably"},
                .tools = {"\"I don't need speedhack. I move different.\"", "-- Tee Grizzley, probably"},
                .credits = {"\"Every frame, I earned it.\"", "-- Tee Grizzley"},
                .bpm = 98.0, .drop = 90.0 + 4.0 / 30.0, .track = "big_brrr_grizzley.mp3",
            },
            /* vRedKingdom */ {
                .title = "Red Kingdom", .ext = ".redkingdom",
                .subtitle = "Long live the kingdom. Frame perfect.", .brand = "Kneel.",
                .badge = "Kansas City | Strange Music | Long Live the Kingdom",
                .replay = {"\"I don't practice. I conquer.\"", "-- Tech N9ne, probably"},
                .tools = {"\"I don't need speedhack. I run the kingdom at my own pace.\"", "-- Tech N9ne, probably"},
                .credits = {"\"Every frame bows to me.\"", "-- Tech N9ne"},
                .bpm = 100.0, .drop = 14.0 + 17.0 / 30.0, .track = "big_brrr_redkingdom.mp3",
            },
            /* vLemonade */ {
                .title = "LemonadeBot", .ext = ".lemonade",
                .subtitle = "Make you some lemonade. Frame perfect.", .brand = "Squeezed.",
                .badge = "State vs. Radric Davis | 2009 | Lemonade",
                .replay = {"\"I squeeze every frame till it's sweet.\"", "-- Gucci Mane, probably"},
                .tools = {"\"I don't need speedhack. Lemonade's already sweet enough.\"", "-- Gucci Mane, probably"},
                .credits = {"\"Every frame's sweet when you make it yourself.\"", "-- Gucci Mane"},
                .bpm = 142.0, .drop = 0.0, .track = "big_brrr_lemonade.mp3",
            },
            /* vBrrr */ {
                .title = "BrrrBot", .ext = ".icebrrr",
                .subtitle = "Frame perfect. Ice cold. Brrr.", .brand = "Frozen.",
                .badge = "St. Brick Intro | Frame Perfect | Ice Cold",
                .replay = {"\"Cold enough to freeze a frame in place.\"", "-- Gucci Mane, probably"},
                .tools = {"\"I don't need speedhack. Cold moves fast on its own.\"", "-- Gucci Mane, probably"},
                .credits = {"\"Every frame's ice cold. Brrr.\"", "-- Gucci Mane"},
                .bpm = 150.0, .drop = 38.0 + 19.0 / 30.0, .track = "big_brrr_brrrbot.mp3",
            },
            /* vWaka */ {
                .title = "WakaBot", .ext = ".waka",
                .subtitle = "Hard in the paint. Frame perfect.", .brand = "OW!", .badge = "Brick Squad | Grove St. Party | OW!",
                .replay = {"\"I don't walk in, I turn up in.\"", "-- Waka Flocka Flame, probably"},
                .tools = {"\"I don't need speedhack. I'm already hard in the paint.\"", "-- Waka Flocka Flame, probably"},
                .credits = {"\"Grove St. party never stopped. Frame perfect either.\"", "-- Gucci Mane"},
                .bpm = kStockBpm, .drop = kStockDrop, .track = kStockTrack,
            },
            /* vYoungsta */ {
                .title = "YoungstaBot", .ext = ".youngsta",
                .subtitle = "Everyday's my birthday. Frame perfect.", .brand = "Everyday!",
                .badge = "Heatmakerz | Memphis | Everyday's My Birthday",
                .replay = {"\"Everyday my birthday. Every frame a gift.\"", "-- Blac Youngsta, probably"},
                .tools = {"\"I don't need speedhack. I move like it's my birthday.\"", "-- Blac Youngsta, probably"},
                .credits = {"\"Real recognize real. Real frames recognize real frames.\"", "-- Gucci Mane"},
                .bpm = 144.0, .drop = 0.0, .track = "big_brrr_youngsta.mp3",
            },
            /* vKnockerz */ {
                .title = "KnockerzBot", .ext = ".knockerz",
                .subtitle = "Two step. Frame perfect.", .brand = "Two step!", .badge = "Two Step | South Carolina | Bow Bow Bow",
                .replay = {"\"Every step's a two step. Every frame's a step ahead.\"", "-- Speaker Knockerz, probably"},
                .tools = {"\"I don't need speedhack. I'm already a step ahead.\"", "-- Speaker Knockerz, probably"},
                .credits = {"\"Two step in, frame perfect out.\"", "-- Gucci Mane"},
                .bpm = 90.0, .drop = 21.0 + 17.0 / 30.0, .track = "big_brrr_knockerz.mp3",
            },
        };

        using Rgb = std::array<float, 3>;

        struct Paint {
            const char* name;
            VoiceId voice;
            Rgb accent;
            Rgb background;
            float backgroundAlpha;
            Rgb card;
            Rgb text;
            Rgb muted;
            float radius;
            float opacity;
            bool pulse = false;
            bool snow = false;
        };

        // In id order: the index is the number saved as "active_theme".
        const Paint kPaints[] = {
            {"GucciBot", vGucci, {0.788f, 0.659f, 0.298f}, {0.051f, 0.051f, 0.051f}, 0.96f,
             {0.078f, 0.078f, 0.078f}, {0.941f, 0.910f, 0.816f}, {0.478f, 0.447f, 0.376f}, 5.f, 0.96f},
            {"ToosiiBot (LSU)", vToosii, {0.992f, 0.816f, 0.137f}, {0.110f, 0.055f, 0.188f}, 0.96f,
             {0.165f, 0.082f, 0.275f}, {0.960f, 0.940f, 0.870f}, {0.600f, 0.490f, 0.300f}, 5.f, 0.96f},
            {"ToosiiBot (Syracuse)", vToosii, {0.961f, 0.404f, 0.031f}, {0.027f, 0.043f, 0.114f}, 0.96f,
             {0.055f, 0.082f, 0.188f}, {0.960f, 0.940f, 0.920f}, {0.600f, 0.500f, 0.400f}, 5.f, 0.96f},
            {"ToosiiBot (Sac State)", vToosii, {0.918f, 0.878f, 0.820f}, {0.016f, 0.188f, 0.094f}, 0.96f,
             {0.024f, 0.251f, 0.125f}, {0.940f, 0.960f, 0.940f}, {0.500f, 0.650f, 0.520f}, 5.f, 0.96f},
            {"JaBot", vJa, {0.420f, 0.784f, 0.953f}, {0.027f, 0.078f, 0.200f}, 0.96f,
             {0.047f, 0.118f, 0.275f}, {0.920f, 0.950f, 0.980f}, {0.400f, 0.540f, 0.720f}, 5.f, 0.96f},
            {"GiddeyBot", vGiddey, {1.000f, 0.310f, 0.106f}, {0.031f, 0.145f, 0.278f}, 0.96f,
             {0.047f, 0.220f, 0.400f}, {0.975f, 0.985f, 0.995f}, {0.580f, 0.740f, 0.900f}, 5.f, 0.96f},
            {"BamBot", vBam, {0.878f, 0.067f, 0.153f}, {0.047f, 0.027f, 0.071f}, 0.96f,
             {0.078f, 0.043f, 0.114f}, {0.980f, 0.980f, 0.980f}, {0.600f, 0.400f, 0.460f}, 5.f, 0.97f},
            {"SexyyBot", vSexyy, {0.910f, 0.004f, 0.580f}, {0.063f, 0.016f, 0.094f}, 0.97f,
             {0.102f, 0.027f, 0.149f}, {0.990f, 0.950f, 0.980f}, {0.580f, 0.340f, 0.520f}, 5.f, 0.97f},
            {"JuiceBot", vJuice, {0.960f, 0.520f, 0.380f}, {0.020f, 0.090f, 0.086f}, 0.96f,
             {0.035f, 0.130f, 0.122f}, {0.980f, 0.960f, 0.940f}, {0.625f, 0.565f, 0.478f}, 5.f, 0.96f},
            {"ButlerBot", vButler, {0.808f, 0.067f, 0.255f}, {0.035f, 0.020f, 0.024f}, 0.96f,
             {0.070f, 0.030f, 0.040f}, {0.980f, 0.970f, 0.970f}, {0.560f, 0.400f, 0.420f}, 5.f, 0.96f},
            {"SaweetieBot", vSaweetie, {1.000f, 0.180f, 0.520f}, {0.090f, 0.020f, 0.060f}, 0.96f,
             {0.140f, 0.035f, 0.095f}, {0.990f, 0.960f, 0.980f}, {0.620f, 0.400f, 0.520f}, 5.f, 0.96f},
            {"MaybachBot", vMaybach, {0.780f, 0.780f, 0.800f}, {0.035f, 0.035f, 0.038f}, 0.97f,
             {0.070f, 0.070f, 0.075f}, {0.960f, 0.960f, 0.965f}, {0.500f, 0.500f, 0.520f}, 5.f, 0.97f},
            {"RomoBot", vRomo, {0.760f, 0.800f, 0.850f}, {0.020f, 0.055f, 0.110f}, 0.96f,
             {0.035f, 0.085f, 0.160f}, {0.960f, 0.965f, 0.975f}, {0.520f, 0.560f, 0.620f}, 5.f, 0.96f},
            {"GrizzleyBot", vGrizzley, {0.870f, 0.090f, 0.070f}, {0.040f, 0.038f, 0.036f}, 0.96f,
             {0.072f, 0.068f, 0.064f}, {0.975f, 0.970f, 0.965f}, {0.540f, 0.460f, 0.400f}, 5.f, 0.96f},
            {"Red Kingdom", vRedKingdom, {0.820f, 0.035f, 0.035f}, {0.028f, 0.008f, 0.008f}, 1.f,
             {0.055f, 0.014f, 0.014f}, {0.960f, 0.930f, 0.930f}, {0.520f, 0.260f, 0.260f}, 0.f, 1.f, true},
            {"LemonadeBot", vLemonade, {0.980f, 0.851f, 0.145f}, {0.090f, 0.075f, 0.020f}, 0.96f,
             {0.140f, 0.115f, 0.030f}, {0.980f, 0.975f, 0.940f}, {0.620f, 0.580f, 0.380f}, 5.f, 0.96f},
            {"BrrrBot", vBrrr, {0.580f, 0.850f, 0.980f}, {0.020f, 0.040f, 0.070f}, 0.96f,
             {0.038f, 0.070f, 0.115f}, {0.960f, 0.975f, 0.990f}, {0.520f, 0.600f, 0.680f}, 5.f, 0.96f, false, true},
            // Nigel's 2026-09-03 picks: green, black + red (the "223" cover:
            // pure black behind, cards a hair off it so they still show), teal.
            {"WakaBot", vWaka, {0.204f, 0.780f, 0.302f}, {0.018f, 0.050f, 0.026f}, 0.96f,
             {0.033f, 0.088f, 0.046f}, {0.955f, 0.980f, 0.955f}, {0.460f, 0.600f, 0.480f}, 5.f, 0.96f},
            {"YoungstaBot", vYoungsta, {1.000f, 0.000f, 0.000f}, {0.000f, 0.000f, 0.000f}, 0.96f,
             {0.070f, 0.020f, 0.020f}, {0.980f, 0.960f, 0.960f}, {0.620f, 0.320f, 0.320f}, 5.f, 0.96f},
            {"KnockerzBot", vKnockerz, {0.070f, 0.780f, 0.720f}, {0.014f, 0.058f, 0.056f}, 0.96f,
             {0.027f, 0.098f, 0.093f}, {0.938f, 0.984f, 0.978f}, {0.420f, 0.600f, 0.580f}, 5.f, 0.96f},
        };

        ImVec4 opaque(const Rgb& c) {
            return ImVec4(c[0], c[1], c[2], 1.f);
        }

        Theme fromTables(int id) {
            Paint const& p = kPaints[id];
            Voice const& v = kVoices[p.voice];
            Theme t;
            t.id = id;
            t.name = p.name;
            t.title = v.title;
            t.extension = v.ext;
            t.subtitle = v.subtitle;
            t.brand = v.brand;
            t.badge = v.badge;
            const char* const* said[kSlotCount] = {v.replay, v.tools, v.credits};
            for (int s = 0; s < kSlotCount; ++s)
                t.quotes[s] = Quote{said[s][0], said[s][1]};
            t.accent = opaque(p.accent);
            t.background = ImVec4(p.background[0], p.background[1], p.background[2], p.backgroundAlpha);
            t.card = opaque(p.card);
            t.text = opaque(p.text);
            t.textMuted = opaque(p.muted);
            t.radius = p.radius;
            t.opacity = p.opacity;
            t.pulse = p.pulse;
            t.snow = p.snow;
            t.bpm = v.bpm;
            t.dropOffsetSec = v.drop;
            t.track = v.track;
            return t;
        }

        // ------------------------------------------------------------ helpers

        std::string lowered(std::string_view s) {
            std::string out(s);
            for (char& c : out)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return out;
        }

        bool sameText(std::string_view a, std::string_view b) {
            return a.size() == b.size() && lowered(a) == lowered(b);
        }

        std::string trimmed(std::string_view s) {
            auto const isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
            while (!s.empty() && isSpace(s.front()))
                s.remove_prefix(1);
            while (!s.empty() && isSpace(s.back()))
                s.remove_suffix(1);
            return std::string(s);
        }

        std::string bare(std::string_view extension) {
            return std::string(!extension.empty() && extension.front() == '.' ? extension.substr(1) : extension);
        }

        // File types GucciBot already reads or writes next to macros (imports,
        // sidecars, backups) or for themes and audio: a theme can't claim them.
        constexpr std::string_view kTaken[] = {
            ".gdr", ".gdr2", ".xd", ".json", ".brr", ".fw", ".fwac", ".path", ".trainer",
            ".bak", ".mp3", ".wav", ".ogg", ".txt", ".log", ".ini",
        };

        // ------------------------------------------------------------ JSON
        //
        // The old editor's keys, read and written through one table so the
        // two directions can't drift apart. Colours are [r, g, b, a] arrays of
        // 0..1 numbers; the extension is stored without its dot.

        struct TextKey {
            const char* key;
            std::string Theme::*field;
        };
        const TextKey kTextKeys[] = {
            {"name", &Theme::name},
            {"subtitle", &Theme::subtitle},
            {"brandTag", &Theme::brand},
            {"creditsBadge", &Theme::badge},
        };

        struct ColourKey {
            const char* key;
            ImVec4 Theme::*field;
        };
        const ColourKey kColourKeys[] = {
            {"accent", &Theme::accent},
            {"bg", &Theme::background},
            {"card", &Theme::card},
            {"textPrimary", &Theme::text},
            {"textSecondary", &Theme::textMuted},
        };

        struct FloatKey {
            const char* key;
            float Theme::*field;
        };
        const FloatKey kFloatKeys[] = {
            {"cornerRadius", &Theme::radius},
            {"bgOpacity", &Theme::opacity},
        };

        struct NumberKey {
            const char* key;
            double Theme::*field;
        };
        const NumberKey kNumberKeys[] = {
            {"bpm", &Theme::bpm},
            {"dropOffsetSec", &Theme::dropOffsetSec},
        };

        // "hasAudio" is the old key; "pulse" is new (older files lack it).
        struct FlagKey {
            const char* key;
            bool Theme::*field;
        };
        const FlagKey kFlagKeys[] = {
            {"hasAudio", &Theme::hasTrack},
            {"pulse", &Theme::pulse},
        };

        // Quote keys are "quote" + slot + "Text" / "Attr".
        constexpr const char* kSlotKeys[kSlotCount] = {"Replay", "Tools", "Credits"};

        std::string quoteKey(int slot, bool attribution) {
            return fmt::format("quote{}{}", kSlotKeys[slot], attribution ? "Attr" : "Text");
        }

        matjson::Value toJson(const Theme& t) {
            auto out = matjson::Value::object();
            for (auto const& k : kTextKeys)
                out.set(k.key, t.*(k.field));
            out.set("extension", bare(t.extension));
            for (auto const& k : kColourKeys) {
                ImVec4 const& c = t.*(k.field);
                auto rgba = matjson::Value::array();
                for (float channel : {c.x, c.y, c.z, c.w})
                    rgba.push(static_cast<double>(channel));
                out.set(k.key, std::move(rgba));
            }
            for (auto const& k : kFloatKeys)
                out.set(k.key, static_cast<double>(t.*(k.field)));
            for (int s = 0; s < kSlotCount; ++s) {
                out.set(quoteKey(s, false), t.quotes[s].text);
                out.set(quoteKey(s, true), t.quotes[s].by);
            }
            for (auto const& k : kNumberKeys)
                out.set(k.key, t.*(k.field));
            for (auto const& k : kFlagKeys)
                out.set(k.key, t.*(k.field));
            return out;
        }

        // Fills `t` from a parsed file, leaving the starter value wherever a
        // key is missing or has the wrong type. False when it has no usable
        // extension (not even from its file name).
        bool fromJson(const matjson::Value& v, const fs::path& file, Theme& t) {
            for (auto const& k : kTextKeys)
                if (auto s = v[k.key].asString())
                    t.*(k.field) = s.unwrap();
            for (auto const& k : kColourKeys) {
                matjson::Value const& rgba = v[k.key];
                if (!rgba.isArray())
                    continue;
                ImVec4& c = t.*(k.field);
                float* channels[4] = {&c.x, &c.y, &c.z, &c.w};
                for (size_t i = 0; i < 4 && i < rgba.size(); ++i)
                    if (auto n = rgba[i].asDouble())
                        *channels[i] = std::clamp(static_cast<float>(n.unwrap()), 0.f, 1.f);
            }
            for (auto const& k : kFloatKeys)
                if (auto n = v[k.key].asDouble())
                    t.*(k.field) = static_cast<float>(n.unwrap());
            for (int s = 0; s < kSlotCount; ++s) {
                if (auto text = v[quoteKey(s, false)].asString())
                    t.quotes[s].text = text.unwrap();
                if (auto by = v[quoteKey(s, true)].asString())
                    t.quotes[s].by = by.unwrap();
            }
            for (auto const& k : kNumberKeys)
                if (auto n = v[k.key].asDouble())
                    t.*(k.field) = n.unwrap();
            for (auto const& k : kFlagKeys)
                if (auto b = v[k.key].asBool())
                    t.*(k.field) = b.unwrap();

            std::string ext = sanitizeExtension(v["extension"].asString().unwrapOr(""));
            if (ext.empty())
                ext = sanitizeExtension(geode::utils::string::pathToString(file.stem()));
            if (ext.empty())
                return false;
            t.extension = ext;
            t.name = trimmed(t.name);
            if (t.name.empty())
                t.name = geode::utils::string::pathToString(file.stem());
            t.title = t.name;
            t.radius = std::clamp(t.radius, 0.f, 14.f);
            t.opacity = std::clamp(t.opacity, 0.3f, 1.f);
            if (!(t.bpm > 0.0))
                t.bpm = kStockBpm;
            t.dropOffsetSec = std::max(0.0, t.dropOffsetSec);
            t.file = file;
            std::error_code ec;
            // The file decides, not the flag: a track dropped into the folder
            // by hand counts, and a flag left behind by a deleted one doesn't.
            t.hasTrack = fs::exists(customTrackPath(t.extension), ec);
            return true;
        }

        // ------------------------------------------------------------ state

        int s_activeId = 0;
        std::string s_activeCustom;

        Theme s_preview;
        int s_previewFrame = 0;
        bool s_previewSet = false;

        bool previewLive() {
            if (!s_previewSet || !ImGui::GetCurrentContext())
                return false;
            // refresh() reads active() before the page draws, so the call
            // from the frame before is the one that counts.
            return ImGui::GetFrameCount() - s_previewFrame <= 1;
        }

        const Theme& chosen() {
            if (s_activeId == kCustomThemeId)
                if (const Theme* t = findCustom(s_activeCustom))
                    return *t;
            if (const Theme* t = builtin(s_activeId))
                return *t;
            return builtins().front();
        }

    } // namespace

    const Quote& quote(const Theme& theme, Slot slot) {
        return theme.quotes[static_cast<size_t>(slot)];
    }

    const std::vector<Theme>& builtins() {
        static const std::vector<Theme> s_builtins = [] {
            std::vector<Theme> list;
            int const count = static_cast<int>(std::size(kPaints));
            list.reserve(static_cast<size_t>(count));
            for (int id = 0; id < count; ++id)
                list.push_back(fromTables(id));
            return list;
        }();
        return s_builtins;
    }

    const Theme* builtin(int id) {
        auto const& list = builtins();
        if (id < 0 || id >= static_cast<int>(list.size()))
            return nullptr;
        return &list[static_cast<size_t>(id)];
    }

    std::filesystem::path customThemesDir() {
        return Mod::get()->getSaveDir() / "customthemes";
    }

    std::vector<Theme>& customs() {
        static std::vector<Theme> s_customs;
        return s_customs;
    }

    const Theme* findCustom(std::string_view name) {
        for (auto const& t : customs())
            if (t.name == name)
                return &t;
        return nullptr;
    }

    Theme starterCustom() {
        Theme t;
        t.id = kCustomThemeId;
        t.custom = true;
        t.name = "Custom";
        t.title = t.name;
        t.extension = ".custom";
        t.subtitle = "Frame perfect. Custom theme.";
        t.brand = "Brrr.";
        t.badge = "Custom | Made | By You";
        t.quotes[static_cast<size_t>(Slot::Replay)] = {"\"Custom, and proud of it.\"", "-- probably"};
        t.quotes[static_cast<size_t>(Slot::Tools)] = {"\"My theme, my rules.\"", "-- probably"};
        t.quotes[static_cast<size_t>(Slot::Credits)] = {"\"I built this one myself.\"", ""};
        t.bpm = kStockBpm;
        t.dropOffsetSec = 0.0;
        return t;
    }

    void loadCustoms() {
        std::vector<Theme> found;
        std::error_code ec;
        for (auto const& entry : fs::directory_iterator(customThemesDir(), ec)) {
            std::error_code typeEc;
            if (!entry.is_regular_file(typeEc) || lowered(geode::utils::string::pathToString(entry.path().extension())) != ".json")
                continue;
            auto parsed = geode::utils::file::readJson(entry.path());
            if (!parsed || !parsed.unwrap().isObject()) {
                log::warn("[GucciBot] Custom theme {} isn't readable JSON, skipped", entry.path());
                continue;
            }
            Theme t = starterCustom();
            if (!fromJson(parsed.unwrap(), entry.path(), t)) {
                log::warn("[GucciBot] Custom theme {} has no usable extension, skipped", entry.path());
                continue;
            }
            found.push_back(std::move(t));
        }
        std::stable_sort(found.begin(), found.end(),
                         [](Theme const& a, Theme const& b) { return lowered(a.name) < lowered(b.name); });
        customs() = std::move(found);
    }

    std::string sanitizeExtension(std::string_view raw) {
        std::string kept;
        for (char ch : raw) {
            if (kept.size() == kMaxExtension)
                break;
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
                kept += ch;
            else if (ch >= 'A' && ch <= 'Z')
                kept += static_cast<char>(ch - 'A' + 'a');
        }
        return kept.empty() ? std::string() : "." + kept;
    }

    std::string extensionProblem(std::string_view extension, std::string_view self) {
        if (extension.size() < 2 || extension.front() != '.')
            return "Give it a file extension: letters and numbers.";
        if (sanitizeExtension(extension) != extension)
            return "An extension is lowercase letters and numbers only, 16 at most.";
        for (auto const& t : builtins())
            if (t.extension == extension)
                return fmt::format("{} is {}'s. Macros saved with it would mix with that theme's.", extension, t.name);
        for (auto taken : kTaken)
            if (taken == extension)
                return fmt::format("GucciBot already uses {} files for something else.", extension);
        for (auto const& t : customs())
            if (t.name != self && t.extension == extension)
                return fmt::format("Your theme {} already saves macros as {}.", t.name, extension);
        return {};
    }

    std::string nameProblem(std::string_view raw, std::string_view self) {
        std::string const name = trimmed(raw);
        if (name.empty())
            return "Give the theme a name.";
        if (name.size() > kMaxName)
            return fmt::format("Keep the name to {} characters.", kMaxName);
        for (auto const& t : builtins())
            if (sameText(t.name, name))
                return fmt::format("{} is one of the built-in themes. Pick another name.", t.name);
        for (auto const& t : customs())
            if (t.name != self && sameText(t.name, name))
                return fmt::format("You already have a theme called {}.", t.name);
        return {};
    }

    std::filesystem::path customTrackPath(std::string_view extension) {
        return customThemesDir() / (bare(extension) + "_brrr.mp3");
    }

    std::filesystem::path trackPath(const Theme& theme) {
        if (theme.custom && theme.hasTrack)
            return customTrackPath(theme.extension);
        bool const ownBundled = !theme.custom && !theme.track.empty();
        return Mod::get()->getResourcesDir() / (ownBundled ? theme.track : std::string(kStockTrack));
    }

    std::string saveCustom(const Theme& theme, std::string_view previousName, const TrackChange& track) {
        std::string const name = trimmed(theme.name);
        std::string const ext = sanitizeExtension(theme.extension);
        if (auto why = nameProblem(name, previousName); !why.empty())
            return why;
        if (auto why = extensionProblem(ext, previousName); !why.empty())
            return why;

        // What this save replaces, copied out before the list is reloaded.
        std::string const previous(previousName);
        fs::path oldFile;
        std::string oldExt;
        if (!previous.empty())
            if (const Theme* old = findCustom(previous)) {
                oldFile = old->file;
                oldExt = old->extension;
            }
        bool const extChanged = !oldExt.empty() && oldExt != ext;

        fs::path const dir = customThemesDir();
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec)
            return "Couldn't make the themes folder: " + ec.message();

        // The track first: if copying fails, nothing has changed yet.
        fs::path const trackFile = customTrackPath(ext);
        fs::path source = track.copyFrom;
        if (source.empty() && !track.remove && extChanged && fs::exists(customTrackPath(oldExt), ec))
            source = customTrackPath(oldExt);
        if (!source.empty() && !fs::equivalent(source, trackFile, ec)) {
            fs::copy_file(source, trackFile, fs::copy_options::overwrite_existing, ec);
            if (ec)
                return "Couldn't copy the track: " + ec.message();
        } else if (source.empty() && track.remove) {
            fs::remove(trackFile, ec);
            if (ec)
                return "Couldn't remove the track: " + ec.message();
        }

        Theme out = theme;
        out.id = kCustomThemeId;
        out.custom = true;
        out.name = name;
        out.title = name;
        out.extension = ext;
        out.radius = std::clamp(out.radius, 0.f, 14.f);
        out.opacity = std::clamp(out.opacity, 0.3f, 1.f);
        out.hasTrack = fs::exists(trackFile, ec);

        fs::path const jsonFile = dir / (bare(ext) + ".json");
        auto written = geode::utils::file::writeStringSafe(jsonFile, toJson(out).dump());
        if (!written)
            return "Couldn't save the theme: " + written.unwrapErr();

        // Only now let go of what the old name and extension left behind.
        if (!oldFile.empty() && !fs::equivalent(oldFile, jsonFile, ec))
            fs::remove(oldFile, ec);
        if (extChanged)
            fs::remove(customTrackPath(oldExt), ec);

        bool const wasActive = s_activeId == kCustomThemeId && !previous.empty() && s_activeCustom == previous;
        loadCustoms();
        if (wasActive)
            s_activeCustom = name;
        return {};
    }

    std::string deleteCustom(std::string_view nameView) {
        std::string const name(nameView);
        const Theme* t = findCustom(name);
        if (!t)
            return "That theme is already gone.";
        fs::path const file = t->file;
        std::string const ext = t->extension;

        std::error_code ec;
        if (!file.empty() && fs::exists(file, ec)) {
            fs::remove(file, ec);
            if (ec)
                return "Couldn't delete the theme: " + ec.message();
        }
        fs::remove(customThemesDir() / (bare(ext) + ".json"), ec);
        fs::remove(customTrackPath(ext), ec);

        bool const wasActive = s_activeId == kCustomThemeId && s_activeCustom == name;
        loadCustoms();
        if (wasActive)
            setActive(0);
        return {};
    }

    ThemeAudio audio(const Theme& theme) {
        ThemeAudio out;
        if (theme.custom && !theme.hasTrack) {
            out.bpm = kStockBpm;
            out.dropOffsetSec = kStockDrop;
            out.file = kStockTrack;
            return out;
        }
        out.bpm = theme.bpm > 0.0 ? theme.bpm : kStockBpm;
        out.dropOffsetSec = std::max(0.0, theme.dropOffsetSec);
        if (theme.custom)
            out.file = geode::utils::string::pathToString(customTrackPath(theme.extension));
        else
            out.file = theme.track.empty() ? std::string(kStockTrack) : theme.track;
        return out;
    }

    const Theme& active() {
        return previewLive() ? s_preview : chosen();
    }

    int activeId() {
        return s_activeId;
    }

    const std::string& activeCustomName() {
        return s_activeCustom;
    }

    void setActive(int id) {
        s_activeId = builtin(id) ? id : 0;
        s_activeCustom.clear();
    }

    void setActiveCustom(const std::string& name) {
        s_activeId = kCustomThemeId;
        s_activeCustom = name;
    }

    void preview(const Theme& draft) {
        if (!ImGui::GetCurrentContext())
            return;
        Theme const& base = chosen();
        Theme shown = draft;
        shown.id = base.id;
        shown.custom = base.custom;
        shown.extension = base.extension;
        shown.bpm = base.bpm;
        shown.dropOffsetSec = base.dropOffsetSec;
        shown.track = base.track;
        shown.hasTrack = base.hasTrack;
        shown.file = base.file;
        s_preview = std::move(shown);
        s_previewFrame = ImGui::GetFrameCount();
        s_previewSet = true;
    }

    void endPreview() {
        s_previewSet = false;
    }

    bool previewing() {
        return previewLive();
    }

} // namespace gucci::ui::themes
